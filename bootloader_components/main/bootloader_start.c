/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * devlink: copy of ESP-IDF v6.0.3 components/bootloader/subproject/main/bootloader_start.c. The only change is
 * the partition selection, which calls devlink_boot_select() (SPEC §3.4), and the unused before/after-init hooks
 * and deep-sleep fast boot (disabled in sdkconfig) are left out. Keep in sync on IDF upgrades (§3.6).
 */
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_rom_caps.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
#include "devlink_boot.h"

ESP_LOG_ATTR_TAG(TAG, "boot");

static int select_partition_number(bootloader_state_t *bs);

/*
 * We arrive here after the ROM bootloader finished loading this second stage bootloader from flash.
 * The hardware is mostly uninitialized, flash cache is down and the app CPU is in reset.
 * We do have a stack, so we can do the initialization in C.
 */
void __attribute__((noreturn)) call_start_cpu0(void)
{
    // 1. Hardware initialization (stock before/after-init hooks are not used by devlink)
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

    // 2. Select the number of boot partition (devlink rules, SPEC §3.4)
    bootloader_state_t bs = {0};
    int boot_index = select_partition_number(&bs);
    if (boot_index == INVALID_INDEX) {
        bootloader_reset();
    }

    // 3. Load the app image for booting
    bootloader_utility_load_boot_image(&bs, boot_index);
}

// Select the number of boot partition
static int select_partition_number(bootloader_state_t *bs)
{
    // 1. Load partition table
    if (!bootloader_utility_load_partition_table(bs)) {
        ESP_LOGE(TAG, "load partition table error!");
        return INVALID_INDEX;
    }

    // 2. Select the number of boot partition
    return devlink_boot_select(bs);
}

#if CONFIG_LIBC_NEWLIB
// Return global reent struct if any newlib functions are linked to bootloader
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
