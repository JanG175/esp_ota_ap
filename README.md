# ESP OTA AP
This code enables to pragram the ESP32 OTA via WIFI AP hosted on the ESP32 itself.

## How 2 use?
* Build your desired project that will be uploaded OTA.
* Copy the desired project's path to `create_cert_and_server.sh`.
* Set `FIRMWARE_UPG_URL` macro in `main/main.c` to match the desired project's name of the binary file (`.bin` - it can be found in `build/` directory), for example: `https://192.168.4.2:8070/file_name.bin`.
* Run `create_cert_and_server.sh` in the new terminal.
* Build and upload this program (`esp_ota_ap`) to the ESP32 and connect a PC to its WIFI AP (using `ESP_WIFI_SSID` and `ESP_WIFI_PASS` located in `main/main.c`).
* Upload of the desired program should start automatically.
* ESP32 will reboot and start the desired program execution.
* In case of any errors, ESP32 will try to perform a rollback to the last working version. It will also happen, if there is no connection to the AP for `AP_MAX_POLLS` seconds.

## Notes
* Uncomment `#define SSL_SERVER 1       // uncomment while using SSL server` in `main/main.c` while using the SSL server.
* Comment out `#define SKIP_VERSION_CHECK // uncomment to skip version check` in `main/main.c` to enable a version check during the update.
* A Python server handler should be added in the future.

## Create certificate and start a server [bash]
Use:
```shell
bash create_cert_and_server.sh
```
to create a certificate, copy it to the workspace directory and start the SSL server.

First time ca_cert.pem creation:
* Country Name (2 letter code) [AU]:PL
* State or Province Name (full name) [Some-State]:Pomorskie
* Locality Name (eg, city) []:Gdansk
* Organization Name (eg, company) [Internet Widgits Pty Ltd]:Home
* Organizational Unit Name (eg, section) []:com
* Common Name (e.g. server FQDN or YOUR name) []:192.168.4.2
* Email Address []: jg@gmail.com

## Sources
* https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/ota.html
* https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_wifi.html
