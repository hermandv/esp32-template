# bootlab: milestone-1 test app for the OTA slots. Behaviour per boot is set with the "mode" console command
# (stored in the app's NVS), to act out the hostile/broken apps of SPEC §9 tests 11-13b.
cmake_minimum_required(VERSION 3.22)

set(DEVLINK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(EXTRA_COMPONENT_DIRS
    "${DEVLINK_ROOT}/components"
    "${DEVLINK_ROOT}/bootloader_components/devlink_boot"
    "${DEVLINK_ROOT}/test/fw/components")
set(SDKCONFIG_DEFAULTS "${DEVLINK_ROOT}/tools/sdkconfig.defaults.devlink;sdkconfig.defaults")

# sdkconfig lives in the build directory: one per build variant, generated from the defaults files.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(BOOTLOADER_EXTRA_COMPONENT_DIRS "${DEVLINK_ROOT}/bootloader_components" APPEND)
idf_build_set_property(MINIMAL_BUILD ON)
project(bootlab)
