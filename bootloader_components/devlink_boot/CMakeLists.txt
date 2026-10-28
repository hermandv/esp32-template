# devlink_boot: boot log + boot decision (SPEC §3.4).
# In the bootloader build it also contains the glue that reads/writes flash and RTC (devlink_boot.c).
# In app/recovery builds only the pure-C format and decision code is compiled, so all images share one definition.
if(BOOTLOADER_BUILD)
    idf_component_register(SRCS "dl_bootlog.c" "dl_rtc.c" "devlink_boot.c"
                           INCLUDE_DIRS "include"
                           REQUIRES bootloader_support spi_flash esp_rom log hal esp_hw_support)
else()
    idf_component_register(SRCS "dl_bootlog.c" "dl_rtc.c"
                           INCLUDE_DIRS "include")
endif()
