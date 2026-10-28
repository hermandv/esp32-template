# devlink core (SPEC §5.1). Pure-C parts (dlcfg_core, dl_json, dl_ota_check, dl_improv, dl_wifi_policy) are also
# host-tested in test/host.
idf_component_register(SRCS "devlink_boot_rt.c" "dlcfg.c" "dlcfg_core.c"
                            "devlink_start.c" "dl_ident.c" "dl_info.c" "dl_wifi.c" "dl_wifi_policy.c"
                            "dl_http.c" "dl_ota.c" "dl_ota_check.c" "dl_json.c" "dl_disc.c" "dl_dns.c"
                            "dl_console.c" "dl_improv.c" "dl_health.c" "dl_web.c" "dl_app.c" "dl_button.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES devlink_boot esp_partition esp_netif
                       PRIV_REQUIRES bootloader_support app_update esp_timer esp_hal_wdt esp_hw_support log freertos
                                     esp_wifi esp_event esp_http_server espcoredump esp_driver_usb_serial_jtag vfs
                                     esp_app_format esp_bootloader_format spi_flash efuse lwip esp_psram esp_driver_gpio)
