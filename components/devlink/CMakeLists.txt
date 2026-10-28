# devlink core (milestone 1 subset: boot steps, boot log, dlcfg). SPEC §5.1.
idf_component_register(SRCS "devlink_boot_rt.c" "dlcfg.c" "dlcfg_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES devlink_boot esp_partition
                       PRIV_REQUIRES bootloader_support app_update esp_timer esp_hal_wdt esp_hw_support log freertos)
