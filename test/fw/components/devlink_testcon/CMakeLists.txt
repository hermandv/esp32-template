# Test console for milestone-1 HIL tests (test builds only).
idf_component_register(SRCS "devlink_testcon.c"
                       INCLUDE_DIRS "."
                       REQUIRES devlink
                       PRIV_REQUIRES esp_driver_usb_serial_jtag esp_partition esp_timer esp_app_format vfs)
