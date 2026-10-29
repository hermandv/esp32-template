# Test console for milestone-1 HIL tests (test builds only).
idf_component_register(SRCS "devlink_testcon.c"
                       INCLUDE_DIRS "."
                       REQUIRES devlink
                       PRIV_REQUIRES esp_partition esp_timer esp_app_format efuse spi_flash)
