/*
 * devlink boot log and boot decision (SPEC §3.4).
 *
 * Pure C, no ESP-IDF dependencies: compiled into the bootloader, the recovery and the app, and into the
 * host unit tests. The record format and the thresholds are FROZEN with the bootloader (SPEC §3.3):
 * changing anything here needs a USB re-flash of every board.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Record format: one byte per boot, a flag is SET when its bit is CLEARED (flash only goes 1 -> 0) ---- */

#define DL_REC_EMPTY         0xFFu /* erased byte, no record */
#define DL_REC_STARTED       0x01u /* b0, bootloader: a boot happened */
#define DL_REC_RECOVERY      0x02u /* b1, bootloader (or recovery itself): this boot went to factory */
#define DL_REC_ALIVE_5S      0x04u /* b2, app/recovery: ran >= 5 s */
#define DL_REC_STABLE        0x08u /* b3, app/recovery: ran >= 120 s (app: also marked valid) */
#define DL_REC_CLEAN_RESTART 0x10u /* b4, app/recovery: planned restart via devlink_restart() */

/* Record values the bootloader appends. */
#define DL_REC_NEW_APP      ((uint8_t)(DL_REC_EMPTY & ~DL_REC_STARTED))
#define DL_REC_NEW_RECOVERY ((uint8_t)(DL_REC_EMPTY & ~(DL_REC_STARTED | DL_REC_RECOVERY)))

/* ---- Thresholds (compile-time, frozen with the bootloader) ---- */

#define DL_QUICK_RESET_COUNT  3u   /* rule 2: this many records in a row without ALIVE_5S */
#define DL_ALIVE_MS           5000u
#define DL_CRASH_LOOP_COUNT   5u   /* rule 3: this many records in a row without STABLE and CLEAN_RESTART */
#define DL_STABLE_MS          120000u
#define DL_RECOVERY_LOOP_COUNT 3u  /* rule 3 exception: this many unconfirmed RECOVERY boots in a row */
#define DL_BOOTLOG_CARRY      8u   /* records carried over when the sector is erased */
#define DL_BOOTLOG_RECENT     8u   /* newest records the scan keeps (>= every rule's count) */

/* ---- Record helpers ---- */

/** True if the byte holds a record (anything but erased). */
static inline bool dl_rec_present(uint8_t rec)
{
    return rec != DL_REC_EMPTY;
}

/** True if @p flag is set (its bit cleared) in a present record. */
static inline bool dl_rec_has(uint8_t rec, uint8_t flag)
{
    return dl_rec_present(rec) && ((rec & flag) == 0u);
}

/** Confirmed = the boot reached STABLE or ended in a planned restart. Torn records are unconfirmed. */
static inline bool dl_rec_confirmed(uint8_t rec)
{
    return dl_rec_has(rec, DL_REC_STABLE) || dl_rec_has(rec, DL_REC_CLEAN_RESTART);
}

/* ---- Sector scan ---- */

typedef struct {
    size_t end;                         /* append position: index after the last record (0 = empty) */
    size_t count;                       /* records in the sector (erased holes not counted) */
    uint8_t recent[DL_BOOTLOG_RECENT];  /* newest first */
    size_t n_recent;                    /* valid entries in recent[] */
} dl_bootlog_scan_t;

/**
 * @brief Scan a boot-log sector.
 *
 * @requirement SPEC-3.4-BOOTLOG
 * @parent      I1
 * @test        host:bootlog_scan_*
 */
void dl_bootlog_scan(const uint8_t *sector, size_t len, dl_bootlog_scan_t *out);

/** Where and what to write to append one record. */
typedef struct {
    bool erase;          /* erase the sector first (it is full) */
    size_t offset;       /* byte offset in the sector of data[0] */
    uint8_t data[DL_BOOTLOG_CARRY + 1u];
    size_t len;          /* bytes in data[] */
} dl_bootlog_append_t;

/**
 * @brief Plan appending @p rec after a scan. When the sector is full the plan erases it and carries the last
 *        DL_BOOTLOG_CARRY records over before the new one.
 *
 * @requirement SPEC-3.4-BOOTLOG
 * @parent      I1
 * @test        host:bootlog_append_*, T13c
 */
void dl_bootlog_plan_append(const dl_bootlog_scan_t *scan, size_t sector_len, uint8_t rec,
                            dl_bootlog_append_t *out);

/** Index of the record a running image should mark (the newest one), or -1 if the log is empty. */
static inline long dl_bootlog_current_index(const dl_bootlog_scan_t *scan)
{
    return (scan->end == 0u) ? -1L : (long)(scan->end - 1u);
}

/* ---- Boot decision ---- */

/** Boot reason passed to the image (SPEC §3.4). Values are frozen (stored in retained RTC). */
typedef enum {
    DL_BOOT_REASON_UNKNOWN = 0,
    DL_BOOT_REASON_NORMAL = 1,
    DL_BOOT_REASON_ROLLBACK = 2,
    DL_BOOT_REASON_REQUESTED = 3,
    DL_BOOT_REASON_QUICK_RESET = 4,
    DL_BOOT_REASON_CRASH_LOOP = 5,
    DL_BOOT_REASON_GPIO = 6,
    DL_BOOT_REASON_NO_VALID_APP = 7,
    DL_BOOT_REASON_RECOVERY_LOOP = 8,
    DL_BOOT_REASON_DEEP_SLEEP = 9,
} dl_boot_reason_t;

typedef enum {
    DL_BOOT_TARGET_STANDARD = 0, /* ESP-IDF selection: otadata + rollback, fallbacks */
    DL_BOOT_TARGET_FACTORY = 1,  /* recovery, otadata untouched */
    DL_BOOT_TARGET_APP = 2,      /* standard selection, but never factory unless no app slot is bootable */
} dl_boot_target_t;

typedef struct {
    bool deep_sleep_wake;        /* reset reason is a deep-sleep wake-up */
    bool host_reset;             /* chip reset requested by the PC over USB (esptool, idf.py flash/monitor) */
    bool req_recovery;           /* valid one-shot REQ_RECOVERY flag in retained RTC */
    bool gpio_recovery;          /* recovery GPIO configured and held low */
    const dl_bootlog_scan_t *log;
} dl_boot_inputs_t;

typedef struct {
    dl_boot_target_t target;
    dl_boot_reason_t reason;     /* NORMAL may still become ROLLBACK / NO_VALID_APP in the IDF selection */
    bool append;                 /* append a record for this boot */
    uint8_t record;              /* record value to append */
    bool clean_prev;             /* first set CLEAN_RESTART in the newest existing record (host reset) */
} dl_boot_decision_t;

/**
 * @brief Decide where to boot (SPEC §3.4 decision order). Uses the records written BEFORE this boot.
 *        A host reset (PC over USB) counts as a planned restart of the interrupted boot: its record is treated as
 *        CLEAN_RESTART (and d.clean_prev asks the caller to write that bit), so repeated flashing never trips the
 *        guards. Power-on and EN-button resets are not host resets.
 *
 * @requirement SPEC-3.4-DECISION
 * @parent      I1, I2
 * @test        host:decide_*, T11, T12, T13, T13a, T13b, T20
 */
dl_boot_decision_t dl_boot_decide(const dl_boot_inputs_t *in);

/** Short name of a boot reason ("normal", "rollback", ...). */
const char *dl_boot_reason_name(dl_boot_reason_t reason);

#ifdef __cplusplus
}
#endif
