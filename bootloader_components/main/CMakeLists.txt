# Bootloader `main` override: stock ESP-IDF bootloader_start.c with the partition selection replaced by devlink.
idf_component_register(SRCS "bootloader_start.c"
                       REQUIRES bootloader bootloader_support devlink_boot)

# Use the default linker scripts
idf_build_get_property(scripts BOOTLOADER_LINKER_SCRIPT)
target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")
