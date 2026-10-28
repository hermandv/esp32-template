# Built-in app channels (SPEC §5.8): sys.temp, sys.heap, sys.rssi. App only (recovery has no sysmon, §6.1).
idf_component_register(SRCS "devlink_sysmon.c"
                       INCLUDE_DIRS "include"
                       REQUIRES devlink
                       PRIV_REQUIRES esp_driver_tsens esp_wifi esp_timer freertos log)
