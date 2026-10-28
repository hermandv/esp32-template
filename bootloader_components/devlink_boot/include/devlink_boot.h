/*
 * devlink bootloader override entry (SPEC §3.4). Bootloader build only.
 */
#pragma once

#include "bootloader_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** devlink bootloader version, reported by /info. Bump on every change (bootloader is frozen per board). */
#define DEVLINK_BOOT_VERSION 1

/**
 * @brief Read the boot log and retained RTC, apply the decision rules, append this boot's record, pass the boot
 *        reason to the image and return the partition index to start from (for bootloader_utility_load_boot_image).
 *        Falls back to the standard ESP-IDF selection on any internal error.
 *
 * @requirement SPEC-3.4-DECISION, SPEC-3.4-BOOTLOG
 * @parent      I1, I2
 * @test        T11, T12, T13, T13a, T13b, T13c, T17, T18, T20
 */
int devlink_boot_select(bootloader_state_t *bs);

#ifdef __cplusplus
}
#endif
