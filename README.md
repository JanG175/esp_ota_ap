# ESP OTA AP
This code enables to pragram your ESP32 via WIFI AP hosted on the ESP32 itself.

## Create certificate and start a server
Use:
```shell
bash create_cert_and_server.sh
```
to create a certificate, copy it to the workspace directory and start a SSL server.


## Sources
* https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/ota.html
* https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_wifi.html
