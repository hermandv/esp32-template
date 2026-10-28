/*
 * devlink boot log and boot decision (SPEC §3.4). Pure C, see dl_bootlog.h.
 */
#include "dl_bootlog.h"

void dl_bootlog_scan(const uint8_t *sector, size_t len, dl_bootlog_scan_t *out)
{
    size_t i;

    out->end = 0u;
    out->count = 0u;
    out->n_recent = 0u;

    /* Append position = after the last record. A torn write that left the byte erased is simply reused. */
    for (i = len; i > 0u; i--) {
        if (dl_rec_present(sector[i - 1u])) {
            out->end = i;
            break;
        }
    }

    for (i = out->end; i > 0u; i--) {
        uint8_t rec = sector[i - 1u];
        if (!dl_rec_present(rec)) {
            continue;
        }
        out->count++;
        if (out->n_recent < DL_BOOTLOG_RECENT) {
            out->recent[out->n_recent] = rec;
            out->n_recent++;
        }
    }
}

void dl_bootlog_plan_append(const dl_bootlog_scan_t *scan, size_t sector_len, uint8_t rec,
                            dl_bootlog_append_t *out)
{
    if (scan->end < sector_len) {
        out->erase = false;
        out->offset = scan->end;
        out->data[0] = rec;
        out->len = 1u;
        return;
    }

    /* Sector full: erase, then write the newest records back oldest-first, then the new one. */
    size_t carry = (scan->n_recent < DL_BOOTLOG_CARRY) ? scan->n_recent : DL_BOOTLOG_CARRY;
    out->erase = true;
    out->offset = 0u;
    for (size_t i = 0u; i < carry; i++) {
        out->data[i] = scan->recent[carry - 1u - i];
    }
    out->data[carry] = rec;
    out->len = carry + 1u;
}

/* True if the newest @p n records all exist and all match @p pred. */
static bool last_n_all(const dl_bootlog_scan_t *log, size_t n, bool (*pred)(uint8_t rec))
{
    if (log->n_recent < n) {
        return false;
    }
    for (size_t i = 0u; i < n; i++) {
        if (!pred(log->recent[i])) {
            return false;
        }
    }
    return true;
}

/* Rule 2: a boot that ended quickly without a planned restart. */
static bool is_quick(uint8_t rec)
{
    return !dl_rec_has(rec, DL_REC_ALIVE_5S) && !dl_rec_has(rec, DL_REC_CLEAN_RESTART);
}

/* Rule 3: a boot that never got confirmed. */
static bool is_unconfirmed(uint8_t rec)
{
    return !dl_rec_confirmed(rec);
}

/* Rule 3 exception: an unconfirmed boot into recovery. */
static bool is_unconfirmed_recovery(uint8_t rec)
{
    return dl_rec_has(rec, DL_REC_RECOVERY) && !dl_rec_confirmed(rec);
}

dl_boot_decision_t dl_boot_decide(const dl_boot_inputs_t *in)
{
    dl_boot_decision_t d = {
        .target = DL_BOOT_TARGET_STANDARD,
        .reason = DL_BOOT_REASON_NORMAL,
        .append = true,
        .record = DL_REC_NEW_APP,
        .clean_prev = false,
    };
    dl_bootlog_scan_t host_log;
    const dl_bootlog_scan_t *log = in->log;

    /* Deep-sleep wake-ups are not logged and never trigger recovery (SPEC §3.4, test 13a). */
    if (in->deep_sleep_wake) {
        d.reason = DL_BOOT_REASON_DEEP_SLEEP;
        d.append = false;
        return d;
    }

    /* Host reset: the interrupted boot ended in a planned restart (SPEC §3.4, development flashing). */
    if (in->host_reset && (log->n_recent > 0u) && !dl_rec_has(log->recent[0], DL_REC_CLEAN_RESTART)) {
        host_log = *log;
        host_log.recent[0] = (uint8_t)(host_log.recent[0] & ~DL_REC_CLEAN_RESTART);
        log = &host_log;
        d.clean_prev = true;
    }

    /* Recovery itself is crash-looping: rules 2 and 3 must not send us back there (otherwise a recovery that
     * dies before ALIVE_5S would be re-entered forever by the quick-reset rule). */
    bool recovery_loop = last_n_all(log, DL_RECOVERY_LOOP_COUNT, is_unconfirmed_recovery);

    if (in->req_recovery) {
        /* Rule 1: one-shot request. */
        d.target = DL_BOOT_TARGET_FACTORY;
        d.reason = DL_BOOT_REASON_REQUESTED;
    } else if (!recovery_loop && last_n_all(log, DL_QUICK_RESET_COUNT, is_quick)) {
        /* Rule 2: quick-reset escape. */
        d.target = DL_BOOT_TARGET_FACTORY;
        d.reason = DL_BOOT_REASON_QUICK_RESET;
    } else if (last_n_all(log, DL_CRASH_LOOP_COUNT, is_unconfirmed)) {
        /* Rule 3: crash-loop guard, unless recovery is the one looping. */
        if (recovery_loop) {
            d.target = DL_BOOT_TARGET_APP;
            d.reason = DL_BOOT_REASON_RECOVERY_LOOP;
        } else {
            d.target = DL_BOOT_TARGET_FACTORY;
            d.reason = DL_BOOT_REASON_CRASH_LOOP;
        }
    } else if (in->gpio_recovery) {
        /* Rule 4: recovery GPIO. */
        d.target = DL_BOOT_TARGET_FACTORY;
        d.reason = DL_BOOT_REASON_GPIO;
    } else {
        /* Rule 5: standard selection. */
    }

    d.record = (d.target == DL_BOOT_TARGET_FACTORY) ? DL_REC_NEW_RECOVERY : DL_REC_NEW_APP;
    return d;
}

const char *dl_boot_reason_name(dl_boot_reason_t reason)
{
    switch (reason) {
    case DL_BOOT_REASON_NORMAL:
        return "normal";
    case DL_BOOT_REASON_ROLLBACK:
        return "rollback";
    case DL_BOOT_REASON_REQUESTED:
        return "requested";
    case DL_BOOT_REASON_QUICK_RESET:
        return "quick_reset";
    case DL_BOOT_REASON_CRASH_LOOP:
        return "crash_loop";
    case DL_BOOT_REASON_GPIO:
        return "gpio";
    case DL_BOOT_REASON_NO_VALID_APP:
        return "no_valid_app";
    case DL_BOOT_REASON_RECOVERY_LOOP:
        return "recovery_loop";
    case DL_BOOT_REASON_DEEP_SLEEP:
        return "deep_sleep";
    default:
        return "unknown";
    }
}
