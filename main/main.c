#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_flash_partitions.h"

// WIFI configuration
#define ESP_WIFI_SSID      "ESP32_OTA_AP"  // for user change
#define ESP_WIFI_PASS      "esp32_ota_ap" // for user change
#define ESP_WIFI_CHANNEL   1
#define MAX_STA_CONN       1

// HTTPS configuration
#define FIRMWARE_UPG_URL "https://192.168.4.2:8070/RGB_blink.bin" // for user change (it has to match with ca_cert.pem)

#define SSL_SERVER 1       // uncomment while using SSL server
#define SKIP_VERSION_CHECK // uncomment to skip version check

#define AP_MAX_POLLS 30
#define OTA_RECV_TIMEOUT 5000
#define BUFFSIZE 1024
#define HASH_LEN 32 // SHA-256 digest length
#define OTA_URL_SIZE 256

// an ota data write buffer ready to write to the flash
static char ota_write_data[BUFFSIZE + 1] = {0};
extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

static portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;
static bool AP_connected = false;

static const char* TAG = "esp_ota_ap";


/**
 * @brief print sha256 hash
 * 
 * @param image_hash sha256 hash
 * @param label label
*/
static void print_sha256(const uint8_t* image_hash, const char* label)
{
    char hash_print[HASH_LEN * 2 + 1];
    hash_print[HASH_LEN * 2] = 0;
    for (uint32_t i = 0; i < HASH_LEN; ++i)
        sprintf(&hash_print[i * 2], "%02x", image_hash[i]);

    ESP_LOGI(TAG, "%s: %s", label, hash_print);
}


/**
 * @brief WIFI event handler
 * 
 * @param arg event handler arguments
 * @param event_base event base
 * @param event_id event id
 * @param event_data event data
*/
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*)event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d", MAC2STR(event->mac), event->aid);

        portENTER_CRITICAL(&spinlock);
        AP_connected = true;
        portEXIT_CRITICAL(&spinlock);
    }
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*)event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d", MAC2STR(event->mac), event->aid);

        portENTER_CRITICAL(&spinlock);
        AP_connected = false;
        portENTER_CRITICAL(&spinlock);
    }
}


/**
 * @brief HTTP cleanup
 * 
 * @param client http client handle
*/
static void http_cleanup(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}


/**
 * @brief reset ESP32 to the last valid app
*/
static void reset_to_last_valid_app(void)
{
    const esp_partition_t* last_valid_app = esp_ota_get_last_invalid_partition();
    esp_err_t err = esp_ota_set_boot_partition(last_valid_app);

    if (err != ESP_OK)
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));

    esp_restart();
}


/**
 * @brief task fatal error handler
*/
static void task_fatal_error(void)
{
    ESP_LOGE(TAG, "Exiting task due to fatal error...");
    vTaskDelay(5000 / portTICK_PERIOD_MS);

    reset_to_last_valid_app();

    vTaskDelete(NULL);
}


/**
 * @brief ota update task
 * 
 * @param pvParameter task arguments
*/
static void ota_task(void* pvParameter)
{
    esp_err_t err;
    // update handle: set by esp_ota_begin(), must be freed via esp_ota_end()
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t* update_partition = NULL;

    const esp_partition_t* configured = esp_ota_get_boot_partition();
    const esp_partition_t* running = esp_ota_get_running_partition();

    if (configured != running)
    {
        ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08"PRIx32", but running from offset 0x%08"PRIx32,
                 configured->address, running->address);
        ESP_LOGW(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
    }
    ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08"PRIx32")",
             running->type, running->subtype, running->address);

    esp_http_client_config_t config = {
        .url = FIRMWARE_UPG_URL,
        .cert_pem = (char*)server_cert_pem_start,
        .timeout_ms = OTA_RECV_TIMEOUT,
        .keep_alive_enable = true
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialise HTTP connection");
        task_fatal_error();
    }
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        task_fatal_error();
    }
    esp_http_client_fetch_headers(client);

    update_partition = esp_ota_get_next_update_partition(NULL);
    assert(update_partition != NULL);
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%"PRIx32,
             update_partition->subtype, update_partition->address);

    int binary_file_length = 0;
    // deal with all receive packet
    bool image_header_was_checked = false;

    while (1)
    {
        int data_read = esp_http_client_read(client, ota_write_data, BUFFSIZE);
        if (data_read < 0)
        {
            ESP_LOGE(TAG, "Error: SSL data read error");
            http_cleanup(client);
            task_fatal_error();
        }
        else if (data_read > 0)
        {
            if (image_header_was_checked == false)
            {
                esp_app_desc_t new_app_info;
                if (data_read > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
                {
                    // check current version with downloading
                    memcpy(&new_app_info, &ota_write_data[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));
                    ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);

                    esp_app_desc_t running_app_info;
                    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK)
                        ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);

                    const esp_partition_t* last_invalid_app = esp_ota_get_last_invalid_partition();
                    esp_app_desc_t invalid_app_info;
                    if (esp_ota_get_partition_description(last_invalid_app, &invalid_app_info) == ESP_OK)
                        ESP_LOGI(TAG, "Last invalid firmware version: %s", invalid_app_info.version);

#ifndef SKIP_VERSION_CHECK
                    // check current version with last invalid partition
                    if (last_invalid_app != NULL)
                    {
                        if (memcmp(invalid_app_info.version, new_app_info.version, sizeof(new_app_info.version)) == 0)
                        {
                            ESP_LOGW(TAG, "New version is the same as invalid version.");
                            ESP_LOGW(TAG, "Previously, there was an attempt to launch the firmware with %s version, but it failed.", invalid_app_info.version);
                            ESP_LOGW(TAG, "The firmware has been rolled back to the previous version.");
                            http_cleanup(client);
                            task_fatal_error();
                        }
                    }
#else
                    if (memcmp(new_app_info.version, running_app_info.version, sizeof(new_app_info.version)) == 0)
                    {
                        ESP_LOGW(TAG, "Current running version is the same as a new!");
                    }
#endif // SKIP_VERSION_CHECK

                    image_header_was_checked = true;

                    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
                    if (err != ESP_OK)
                    {
                        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
                        http_cleanup(client);
                        esp_ota_abort(update_handle);
                        task_fatal_error();
                    }
                    ESP_LOGI(TAG, "esp_ota_begin succeeded");
                }
                else
                {
                    ESP_LOGE(TAG, "received package is not fit len");
                    http_cleanup(client);
                    esp_ota_abort(update_handle);
                    task_fatal_error();
                }
            }

            err = esp_ota_write(update_handle, (const void*)ota_write_data, data_read);
            if (err != ESP_OK)
            {
                http_cleanup(client);
                esp_ota_abort(update_handle);
                task_fatal_error();
            }

            binary_file_length += data_read;
            ESP_LOGD(TAG, "Written image length %d", binary_file_length);
        }
        else if (data_read == 0)
        {
           /*
            * As esp_http_client_read never returns negative error code, we rely on
            * `errno` to check for underlying transport connectivity closure if any
            */
            if (errno == ECONNRESET || errno == ENOTCONN)
            {
                ESP_LOGE(TAG, "Connection closed, errno = %d", errno);
                break;
            }
            if (esp_http_client_is_complete_data_received(client) == true)
            {
                ESP_LOGI(TAG, "Connection closed");
                break;
            }
        }

#ifdef SSL_SERVER
        // SSL server cannot check if the complete data was send by HTTP
        if (data_read >= 0 && data_read < BUFFSIZE)
        {
            ESP_LOGI(TAG, "Connection closed, all data received");
            break;
        }
#endif // SSL_SERVER
    }

#ifndef SSL_SERVER
    ESP_LOGI(TAG, "Total Write binary data length: %d", binary_file_length);
    if (esp_http_client_is_complete_data_received(client) != true)
    {
        ESP_LOGE(TAG, "Error in receiving complete file");
        http_cleanup(client);
        esp_ota_abort(update_handle);
        task_fatal_error();
    }
#endif // SSL_SERVER

    err = esp_ota_end(update_handle);
    if (err != ESP_OK)
    {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED)
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        else
            ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));

        http_cleanup(client);
        task_fatal_error();
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        http_cleanup(client);
        task_fatal_error();
    }

    ESP_LOGI(TAG, "Prepare to restart system!");
    esp_restart();

    vTaskDelete(NULL);
}


/**
 * @brief init WIFI AP
*/
void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = ESP_WIFI_SSID,
            .ssid_len = strlen(ESP_WIFI_SSID),
            .channel = ESP_WIFI_CHANNEL,
            .password = ESP_WIFI_PASS,
            .max_connection = MAX_STA_CONN,
#ifdef CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
            .authmode = WIFI_AUTH_WPA3_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
#else /* CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT */
            .authmode = WIFI_AUTH_WPA2_PSK,
#endif
            .pmf_cfg = {
                .required = true
            }
        }
    };

    if (strlen(ESP_WIFI_PASS) == 0)
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID: %s password: %s channel: %d",
             ESP_WIFI_SSID, ESP_WIFI_PASS, ESP_WIFI_CHANNEL);
}


/**
 * @brief main function
*/
void app_main(void)
{
    uint8_t sha_256[HASH_LEN] = {0};
    esp_partition_t partition;

    // get sha256 digest for the partition table
    partition.address = ESP_PARTITION_TABLE_OFFSET;
    partition.size = ESP_PARTITION_TABLE_MAX_LEN;
    partition.type = ESP_PARTITION_TYPE_DATA;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for the partition table: ");

    // get sha256 digest for bootloader
    partition.address = ESP_BOOTLOADER_OFFSET;
    partition.size = ESP_PARTITION_TABLE_OFFSET;
    partition.type = ESP_PARTITION_TYPE_APP;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for bootloader: ");

    // get sha256 digest for running partition
    esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
    print_sha256(sha_256, "SHA-256 for current firmware: ");

    // initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        /*
         * OTA app partition table has a smaller NVS partition size than the non-OTA
         * partition table. This size mismatch may cause NVS initialization to fail.
         * If this happens, erase NVS partition and initialize NVS again.
         */
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // initialize WIFI AP
    wifi_init_softap();

    // wait for a PC to connect to the AP
    uint32_t AP_polls = 1;
    while (AP_connected == false)
    {
        ESP_LOGI(TAG, "Waiting for a PC to connect to the AP (%lu/%d) ...", AP_polls, AP_MAX_POLLS);
        AP_polls++;
        vTaskDelay(1000 / portTICK_PERIOD_MS);

        if (AP_polls > AP_MAX_POLLS)
        {
            ESP_LOGE(TAG, "No PC connected to the AP. Restarting ...");
            
            // if no PC is connected to the AP, rollback to the last working program
            reset_to_last_valid_app();
        }
    }

    /*
     * Treat successful WiFi connection as a checkpoint to cancel rollback
     * process and mark newly updated firmware image as active.
     */
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK)
    {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY)
        {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
                ESP_LOGI(TAG, "App is valid, rollback cancelled successfully");
            else
            {
                ESP_LOGE(TAG, "Failed to cancel rollback");
                esp_ota_mark_app_invalid_rollback_and_reboot();
            }
        }
    }

    // start OTA task
    xTaskCreate(&ota_task, "ota_task", 8192, NULL, 5, NULL);
}