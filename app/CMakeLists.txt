# devlink test app TEMPLATE -> ota_0 / ota_1 (SPEC §5.1). Copy this project for a new board/IC test app.
cmake_minimum_required(VERSION 3.22)

set(DEVLINK_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(EXTRA_COMPONENT_DIRS
    "${DEVLINK_ROOT}/components"
    "${DEVLINK_ROOT}/bootloader_components/devlink_boot")
set(SDKCONFIG_DEFAULTS "${DEVLINK_ROOT}/tools/sdkconfig.defaults.devlink;sdkconfig.defaults" ${DEVLINK_EXTRA_DEFAULTS})
# sdkconfig lives in the build directory: one per build variant, generated from the defaults files.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(BOOTLOADER_EXTRA_COMPONENT_DIRS "${DEVLINK_ROOT}/bootloader_components" APPEND)
project(devlink_app)
