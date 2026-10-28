/*
 * devlink core, milestone 1 subset (SPEC §3.4, §3.5, §5.3): boot steps, boot log marks, planned restarts,
 * reboot-to-recovery, boot info and the dlcfg partition. Shared by recovery and app.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dl_bootlog.h"
#include "dlcfg_core.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEVLINK_MODE_APP = 0,
    DEVLINK_MODE_RECOVERY = 1,
} devlink_mode_t;

/** RTC WDT timeout during the verify window (SPEC §3.5 rule 2). */
#define DEVLINK_VERIFY_WDT_MS 90000u

typedef struct {
    devlink_mode_t mode;
    dl_boot_reason_t reason;      /* from the bootloader, corrected for loader fallbacks */
    bool rtc_valid;               /* bootloader handed over a valid RTC block */
    uint32_t start_offset;        /* partition the bootloader started from */
    uint32_t running_offset;      /* partition actually running */
    long record_index;            /* this boot's record in dlboot, -1 if none */
    size_t record_count;
    uint8_t recent[16];           /* newest first */
    size_t n_recent;
} devlink_boot_info_t;

/**
 * @brief devlink boot steps, the FIRST call in app_main (SPEC §3.5 rule 7): RTC WDT take-over, boot info,
 *        boot-log timers (ALIVE_5S at 5 s, STABLE at 120 s once valid), reset reason into dlcfg.
 *
 * @requirement SPEC-3.5-R7, SPEC-3.4-BOOTLOG
 * @parent      I1, I2
 * @test        T6, T11, T12, T13b
 */
esp_err_t devlink_boot_early(devlink_mode_t mode);

/**
 * @brief Self-test passed: app -> esp_ota_mark_app_valid_cancel_rollback(); both -> RTC WDT off. STABLE is set
 *        at 120 s uptime, or now if that has already passed.
 *
 * @requirement SPEC-3.5-R3
 * @parent      I1
 * @test        T5, T6, T7
 */
esp_err_t devlink_mark_valid(void);

/**
 * @brief Planned restart: sets CLEAN_RESTART in this boot's record, then restarts (SPEC §3.5 rule 8).
 *
 * @requirement SPEC-3.5-R8
 * @parent      I1
 * @test        T13b
 */
void devlink_restart(void) __attribute__((noreturn));

/**
 * @brief Reboot into recovery once (retained-RTC REQ_RECOVERY), slot states untouched (SPEC §3.4 rule 1).
 *
 * @requirement SPEC-3.4-R1
 * @parent      I2
 * @test        T14
 */
void devlink_restart_to_recovery(void) __attribute__((noreturn));

/** Boot information gathered by devlink_boot_early(). */
const devlink_boot_info_t *devlink_boot_info(void);

/** Set flag bits (DL_REC_*) in this boot's record. Bits only ever get set. */
esp_err_t devlink_bootlog_mark(uint8_t flags);

/** Read the newest @p max records (newest first) from dlboot. Returns the count read. */
size_t devlink_bootlog_recent(uint8_t *out, size_t max, size_t *total);

/* ---- dlcfg partition (SPEC §5.3) ---- */

/**
 * @brief Load the newest valid config, or defaults if there is none.
 *
 * @requirement SPEC-5.3
 * @parent      I3
 * @test        host:dlcfg_*, T16
 */
esp_err_t dlcfg_load(dlcfg_t *cfg);

/**
 * @brief Save @p cfg into the other sector (power-loss safe).
 *
 * @requirement SPEC-5.3
 * @parent      I3
 * @test        host:dlcfg_*, T16
 */
esp_err_t dlcfg_save(const dlcfg_t *cfg);

/** Sequence number of the record last loaded or saved. */
uint32_t dlcfg_sequence(void);

#ifdef __cplusplus
}
#endif
