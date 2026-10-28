# devlink recovery image -> factory partition (SPEC §6). Milestone 1: boot steps + test console only.
cmake_minimum_required(VERSION 3.22)

set(DEVLINK_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(EXTRA_COMPONENT_DIRS
    "${DEVLINK_ROOT}/components"
    "${DEVLINK_ROOT}/bootloader_components/devlink_boot"
    "${DEVLINK_ROOT}/test/fw/components")  # milestone 1 only: test console
# -DDEVLINK_EXTRA_DEFAULTS=<file> adds a defaults file, e.g. test/fw/recovery_crash.defaults (test 20).
set(SDKCONFIG_DEFAULTS "${DEVLINK_ROOT}/tools/sdkconfig.defaults.devlink;sdkconfig.defaults" ${DEVLINK_EXTRA_DEFAULTS})

# sdkconfig lives in the build directory: one per build variant, generated from the defaults files.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The devlink bootloader (frozen, SPEC §3.4) is built with this project; flash_all.py takes it from here.
idf_build_set_property(BOOTLOADER_EXTRA_COMPONENT_DIRS "${DEVLINK_ROOT}/bootloader_components" APPEND)
idf_build_set_property(MINIMAL_BUILD ON)
project(devlink_recovery)
