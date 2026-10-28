/*
 * Host unit tests for the devlink boot log, boot decision and retained-RTC block (SPEC §3.4).
 * The "board" helpers simulate the dlboot sector with NOR-flash semantics (writes only clear bits).
 */
#include <string.h>

#include "dl_bootlog.h"
#include "dl_rtc.h"
#include "unity_lite.h"

UT_MAIN_DEFS

#define SEC 4096u

/* ------------------------------------------------------------------------------------------------ */
/* Simulated board                                                                                  */
/* ------------------------------------------------------------------------------------------------ */

typedef struct {
    uint8_t flash[SEC];
    int boots_factory;
    int boots_app;
} board_t;

static void board_init(board_t *b)
{
    memset(b, 0, sizeof(*b));
    memset(b->flash, 0xFF, sizeof(b->flash));
}

static void flash_program(board_t *b, size_t off, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        b->flash[off + i] &= data[i];
    }
}

/* One reset through the bootloader. @p torn_mask: bits of the new record that do NOT get programmed
 * (simulated power loss during the write); 0 = clean write. */
static dl_boot_decision_t board_boot_ex(board_t *b, bool deep_sleep, bool req, bool gpio, uint8_t torn_mask)
{
    dl_bootlog_scan_t scan;
    dl_bootlog_scan(b->flash, SEC, &scan);
    dl_boot_inputs_t in = {.deep_sleep_wake = deep_sleep, .req_recovery = req, .gpio_recovery = gpio, .log = &scan};
    dl_boot_decision_t d = dl_boot_decide(&in);
    if (d.append) {
        dl_bootlog_append_t plan;
        dl_bootlog_plan_append(&scan, SEC, d.record, &plan);
        if (plan.erase) {
            memset(b->flash, 0xFF, SEC);
        }
        uint8_t data[DL_BOOTLOG_CARRY + 1];
        memcpy(data, plan.data, plan.len);
        data[plan.len - 1] |= torn_mask;
        flash_program(b, plan.offset, data, plan.len);
    }
    if (d.target == DL_BOOT_TARGET_FACTORY) {
        b->boots_factory++;
    } else {
        b->boots_app++;
    }
    return d;
}

static dl_boot_decision_t board_boot(board_t *b)
{
    return board_boot_ex(b, false, false, false, 0);
}

/* The running image marks its own (newest) record. */
static void board_mark(board_t *b, uint8_t flag)
{
    dl_bootlog_scan_t scan;
    dl_bootlog_scan(b->flash, SEC, &scan);
    long idx = dl_bootlog_current_index(&scan);
    if (idx >= 0) {
        uint8_t v = (uint8_t)~flag;
        flash_program(b, (size_t)idx, &v, 1);
    }
}

/* The image runs for @p ms and then resets; @p clean = planned restart through devlink_restart(). */
static void board_run(board_t *b, unsigned ms, bool clean)
{
    if (ms >= DL_ALIVE_MS) {
        board_mark(b, DL_REC_ALIVE_5S);
    }
    if (ms >= DL_STABLE_MS) {
        board_mark(b, DL_REC_STABLE);
    }
    if (clean) {
        board_mark(b, DL_REC_CLEAN_RESTART);
    }
}

/* Boot and run an image; returns the decision. */
static dl_boot_decision_t board_cycle(board_t *b, unsigned ms, bool clean)
{
    dl_boot_decision_t d = board_boot(b);
    board_run(b, ms, clean);
    return d;
}

/* Scan helper for direct decision tests: records given oldest-first. */
static dl_boot_decision_t decide_with(const uint8_t *recs, size_t n, bool req, bool gpio, bool deep)
{
    uint8_t sector[64];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, recs, n);
    dl_bootlog_scan_t scan;
    dl_bootlog_scan(sector, sizeof(sector), &scan);
    dl_boot_inputs_t in = {.deep_sleep_wake = deep, .req_recovery = req, .gpio_recovery = gpio, .log = &scan};
    return dl_boot_decide(&in);
}

/* Record values as the images leave them. */
#define R_APP_QUICK   DL_REC_NEW_APP                                                         /* < 5 s */
#define R_APP_ALIVE   ((uint8_t)(DL_REC_NEW_APP & ~DL_REC_ALIVE_5S))                         /* 5..120 s */
#define R_APP_STABLE  ((uint8_t)(DL_REC_NEW_APP & ~(DL_REC_ALIVE_5S | DL_REC_STABLE)))
#define R_APP_CLEANQ  ((uint8_t)(DL_REC_NEW_APP & ~DL_REC_CLEAN_RESTART))                    /* quick + clean */
#define R_REC_QUICK   DL_REC_NEW_RECOVERY
#define R_REC_ALIVE   ((uint8_t)(DL_REC_NEW_RECOVERY & ~DL_REC_ALIVE_5S))
#define R_REC_STABLE  ((uint8_t)(DL_REC_NEW_RECOVERY & ~(DL_REC_ALIVE_5S | DL_REC_STABLE)))

/* ------------------------------------------------------------------------------------------------ */
/* Scan / append                                                                                    */
/* ------------------------------------------------------------------------------------------------ */

static void bootlog_scan_empty(void)
{
    uint8_t s[SEC];
    memset(s, 0xFF, sizeof(s));
    dl_bootlog_scan_t scan;
    dl_bootlog_scan(s, SEC, &scan);
    UT_EQ(scan.end, 0);
    UT_EQ(scan.count, 0);
    UT_EQ(scan.n_recent, 0);
    UT_EQ(dl_bootlog_current_index(&scan), -1);
}

static void bootlog_scan_newest_first(void)
{
    uint8_t s[SEC];
    memset(s, 0xFF, sizeof(s));
    for (int i = 0; i < 10; i++) {
        s[i] = (uint8_t)(0xE0 + i);
    }
    dl_bootlog_scan_t scan;
    dl_bootlog_scan(s, SEC, &scan);
    UT_EQ(scan.end, 10);
    UT_EQ(scan.count, 10);
    UT_EQ(scan.n_recent, DL_BOOTLOG_RECENT);
    UT_EQ(scan.recent[0], 0xE9);
    UT_EQ(scan.recent[7], 0xE2);
    UT_EQ(dl_bootlog_current_index(&scan), 9);
}

static void bootlog_scan_skips_holes(void)
{
    uint8_t s[SEC];
    memset(s, 0xFF, sizeof(s));
    s[0] = 0xFE;
    s[2] = 0xFC; /* s[1] erased: hole */
    dl_bootlog_scan_t scan;
    dl_bootlog_scan(s, SEC, &scan);
    UT_EQ(scan.end, 3);
    UT_EQ(scan.count, 2);
    UT_EQ(scan.recent[0], 0xFC);
    UT_EQ(scan.recent[1], 0xFE);
}

static void bootlog_scan_full_sector(void)
{
    uint8_t s[SEC];
    memset(s, 0xF0, sizeof(s));
    dl_bootlog_scan_t scan;
    dl_bootlog_scan(s, SEC, &scan);
    UT_EQ(scan.end, SEC);
    UT_EQ(scan.count, SEC);
}

static void bootlog_append_simple(void)
{
    uint8_t s[SEC];
    memset(s, 0xFF, sizeof(s));
    s[0] = 0xF2;
    dl_bootlog_scan_t scan;
    dl_bootlog_scan(s, SEC, &scan);
    dl_bootlog_append_t plan;
    dl_bootlog_plan_append(&scan, SEC, DL_REC_NEW_APP, &plan);
    UT_CHECK(!plan.erase);
    UT_EQ(plan.offset, 1);
    UT_EQ(plan.len, 1);
    UT_EQ(plan.data[0], DL_REC_NEW_APP);
}

static void bootlog_append_rollover_carries_last_8(void)
{
    uint8_t s[SEC];
    for (size_t i = 0; i < SEC; i++) {
        s[i] = (uint8_t)(0xE0 | (i & 0x0F));
    }
    dl_bootlog_scan_t scan;
    dl_bootlog_scan(s, SEC, &scan);
    dl_bootlog_append_t plan;
    dl_bootlog_plan_append(&scan, SEC, DL_REC_NEW_RECOVERY, &plan);
    UT_CHECK(plan.erase);
    UT_EQ(plan.offset, 0);
    UT_EQ(plan.len, DL_BOOTLOG_CARRY + 1);
    for (size_t i = 0; i < DL_BOOTLOG_CARRY; i++) {
        UT_EQ(plan.data[i], s[SEC - DL_BOOTLOG_CARRY + i]); /* oldest first, same order as before */
    }
    UT_EQ(plan.data[DL_BOOTLOG_CARRY], DL_REC_NEW_RECOVERY);
}

/* ------------------------------------------------------------------------------------------------ */
/* Decision rules                                                                                   */
/* ------------------------------------------------------------------------------------------------ */

static void decide_empty_log_is_normal(void)
{
    dl_boot_decision_t d = decide_with(NULL, 0, false, false, false);
    UT_EQ(d.target, DL_BOOT_TARGET_STANDARD);
    UT_EQ(d.reason, DL_BOOT_REASON_NORMAL);
    UT_CHECK(d.append);
    UT_EQ(d.record, DL_REC_NEW_APP);
}

static void decide_deep_sleep_not_logged(void)
{
    const uint8_t r[] = {R_APP_QUICK, R_APP_QUICK, R_APP_QUICK, R_APP_QUICK, R_APP_QUICK};
    dl_boot_decision_t d = decide_with(r, sizeof(r), false, false, true);
    UT_EQ(d.target, DL_BOOT_TARGET_STANDARD);
    UT_EQ(d.reason, DL_BOOT_REASON_DEEP_SLEEP);
    UT_CHECK(!d.append);
}

static void decide_request_goes_to_factory(void)
{
    const uint8_t r[] = {R_APP_STABLE};
    dl_boot_decision_t d = decide_with(r, sizeof(r), true, false, false);
    UT_EQ(d.target, DL_BOOT_TARGET_FACTORY);
    UT_EQ(d.reason, DL_BOOT_REASON_REQUESTED);
    UT_EQ(d.record, DL_REC_NEW_RECOVERY);
}

static void decide_quick_reset_needs_three(void)
{
    const uint8_t two[] = {R_APP_STABLE, R_APP_QUICK, R_APP_QUICK};
    UT_EQ(decide_with(two, sizeof(two), false, false, false).target, DL_BOOT_TARGET_STANDARD);

    const uint8_t three[] = {R_APP_STABLE, R_APP_QUICK, R_APP_QUICK, R_APP_QUICK};
    dl_boot_decision_t d = decide_with(three, sizeof(three), false, false, false);
    UT_EQ(d.target, DL_BOOT_TARGET_FACTORY);
    UT_EQ(d.reason, DL_BOOT_REASON_QUICK_RESET);
}

static void decide_quick_reset_broken_by_alive(void)
{
    /* (A stable boot first, so 5 unconfirmed in a row do not trip the crash-loop guard instead.) */
    const uint8_t r[] = {R_APP_STABLE, R_APP_QUICK, R_APP_ALIVE, R_APP_QUICK, R_APP_QUICK};
    UT_EQ(decide_with(r, sizeof(r), false, false, false).target, DL_BOOT_TARGET_STANDARD);
}

static void decide_clean_restarts_are_not_quick(void)
{
    /* Test 13b: quick planned restarts (OTA, /reboot) never count. */
    const uint8_t r[] = {R_APP_CLEANQ, R_APP_CLEANQ, R_APP_CLEANQ, R_APP_CLEANQ, R_APP_CLEANQ, R_APP_CLEANQ};
    UT_EQ(decide_with(r, sizeof(r), false, false, false).target, DL_BOOT_TARGET_STANDARD);
}

static void decide_crash_loop_needs_five(void)
{
    const uint8_t four[] = {R_APP_STABLE, R_APP_ALIVE, R_APP_ALIVE, R_APP_ALIVE, R_APP_ALIVE};
    UT_EQ(decide_with(four, sizeof(four), false, false, false).target, DL_BOOT_TARGET_STANDARD);

    const uint8_t five[] = {R_APP_STABLE, R_APP_ALIVE, R_APP_ALIVE, R_APP_ALIVE, R_APP_ALIVE, R_APP_ALIVE};
    dl_boot_decision_t d = decide_with(five, sizeof(five), false, false, false);
    UT_EQ(d.target, DL_BOOT_TARGET_FACTORY);
    UT_EQ(d.reason, DL_BOOT_REASON_CRASH_LOOP);
}

static void decide_crash_loop_broken_by_stable_or_clean(void)
{
    const uint8_t a[] = {R_APP_ALIVE, R_APP_ALIVE, R_APP_STABLE, R_APP_ALIVE, R_APP_ALIVE};
    UT_EQ(decide_with(a, sizeof(a), false, false, false).target, DL_BOOT_TARGET_STANDARD);
    const uint8_t b[] = {R_APP_ALIVE, R_APP_ALIVE, R_APP_CLEANQ, R_APP_ALIVE, R_APP_ALIVE};
    UT_EQ(decide_with(b, sizeof(b), false, false, false).target, DL_BOOT_TARGET_STANDARD);
}

static void decide_recovery_loop_boots_app(void)
{
    /* Test 20: recovery crashes at start (no ALIVE): neither rule 2 nor rule 3 may send us back. */
    const uint8_t r[] = {R_APP_ALIVE, R_APP_ALIVE, R_REC_QUICK, R_REC_QUICK, R_REC_QUICK};
    dl_boot_decision_t d = decide_with(r, sizeof(r), false, false, false);
    UT_EQ(d.target, DL_BOOT_TARGET_APP);
    UT_EQ(d.reason, DL_BOOT_REASON_RECOVERY_LOOP);
    UT_EQ(d.record, DL_REC_NEW_APP);
}

static void decide_recovery_loop_short_history_is_normal(void)
{
    /* Recovery looped 3 times but older history was good: rule 3 does not fire, rule 2 is suppressed. */
    const uint8_t r[] = {R_APP_STABLE, R_APP_STABLE, R_REC_QUICK, R_REC_QUICK, R_REC_QUICK};
    UT_EQ(decide_with(r, sizeof(r), false, false, false).target, DL_BOOT_TARGET_STANDARD);
}

static void decide_request_beats_recovery_loop(void)
{
    const uint8_t r[] = {R_APP_ALIVE, R_APP_ALIVE, R_REC_QUICK, R_REC_QUICK, R_REC_QUICK};
    UT_EQ(decide_with(r, sizeof(r), true, false, false).target, DL_BOOT_TARGET_FACTORY);
}

static void decide_gpio(void)
{
    const uint8_t r[] = {R_APP_STABLE};
    dl_boot_decision_t d = decide_with(r, sizeof(r), false, true, false);
    UT_EQ(d.target, DL_BOOT_TARGET_FACTORY);
    UT_EQ(d.reason, DL_BOOT_REASON_GPIO);
}

static void decide_torn_records_count_as_unconfirmed(void)
{
    /* Torn: STARTED not even programmed but RECOVERY is (0xFD), or only part of it. */
    const uint8_t r[] = {0xFD, 0xFE, 0xFD};
    UT_EQ(decide_with(r, sizeof(r), false, false, false).reason, DL_BOOT_REASON_QUICK_RESET);
}

static void decide_reason_names(void)
{
    UT_CHECK(strcmp(dl_boot_reason_name(DL_BOOT_REASON_QUICK_RESET), "quick_reset") == 0);
    UT_CHECK(strcmp(dl_boot_reason_name((dl_boot_reason_t)200), "unknown") == 0);
}

/* ------------------------------------------------------------------------------------------------ */
/* Scenarios on the simulated board (acceptance tests 11, 12, 13a, 13b, 13c, 20)                    */
/* ------------------------------------------------------------------------------------------------ */

static void scenario_good_app_never_recovers_across_rollovers(void)
{
    board_t b;
    board_init(&b);
    for (int i = 0; i < 10000; i++) {
        board_cycle(&b, 200000, false);
    }
    UT_EQ(b.boots_factory, 0);
}

static void scenario_t11_crash_loop_after_40s(void)
{
    board_t b;
    board_init(&b);
    board_cycle(&b, 200000, false); /* was good */
    int n = 0;
    dl_boot_decision_t d;
    do {
        d = board_cycle(&b, 40000, false);
        n++;
    } while (d.target != DL_BOOT_TARGET_FACTORY && n < 20);
    UT_EQ(d.reason, DL_BOOT_REASON_CRASH_LOOP);
    UT_EQ(n, 6); /* 5 crashing app boots, the 6th boot goes to recovery */
}

static void scenario_t12_three_quick_power_cycles(void)
{
    board_t b;
    board_init(&b);
    board_cycle(&b, 200000, false);
    board_cycle(&b, 1000, false); /* hostile app: user power-cycles within 5 s */
    board_cycle(&b, 1000, false);
    board_cycle(&b, 1000, false);
    dl_boot_decision_t d = board_boot(&b);
    UT_EQ(d.target, DL_BOOT_TARGET_FACTORY);
    UT_EQ(d.reason, DL_BOOT_REASON_QUICK_RESET);
    /* Recovery runs to STABLE, then a power cycle goes back to the app. */
    board_run(&b, 200000, false);
    UT_EQ(board_boot(&b).target, DL_BOOT_TARGET_STANDARD);
}

static void scenario_t13a_deep_sleep_for_an_hour(void)
{
    board_t b;
    board_init(&b);
    board_cycle(&b, 1000, false); /* app goes to sleep after 1 s */
    for (int i = 0; i < 360; i++) { /* 10 s wake-ups for 1 h, each awake < 5 s */
        dl_boot_decision_t d = board_boot_ex(&b, true, false, false, 0);
        UT_EQ(d.target, DL_BOOT_TARGET_STANDARD);
    }
    UT_EQ(b.boots_factory, 0);
}

static void scenario_t13b_many_planned_restarts(void)
{
    board_t b;
    board_init(&b);
    for (int i = 0; i < 200; i++) {
        board_cycle(&b, 2000, true);
    }
    UT_EQ(b.boots_factory, 0);
}

static void scenario_t13c_torn_record_in_good_run(void)
{
    /* Power loss while appending, every possible partial pattern, in the middle of a good run. */
    for (unsigned mask = 1; mask < 0x100; mask++) {
        board_t b;
        board_init(&b);
        for (int i = 0; i < 10; i++) {
            board_cycle(&b, 200000, false);
        }
        board_boot_ex(&b, false, false, false, (uint8_t)mask); /* torn, then power is gone */
        for (int i = 0; i < 10; i++) {
            board_cycle(&b, 200000, false);
        }
        UT_EQ(b.boots_factory, 0);
    }
}

static void scenario_t13c_torn_records_crash_loop_still_caught(void)
{
    /* A crash loop where every append is torn in some way still ends in recovery (errs towards recovery). */
    for (unsigned mask = 0; mask < 0x100; mask++) {
        if ((DL_REC_NEW_APP | (uint8_t)mask) == DL_REC_EMPTY) {
            continue; /* nothing programmed at all: no record, nothing to count */
        }
        board_t b;
        board_init(&b);
        board_cycle(&b, 200000, false);
        dl_boot_decision_t d = {0};
        int n = 0;
        while (n < 10) {
            d = board_boot_ex(&b, false, false, false, (uint8_t)mask);
            n++;
            if (d.target == DL_BOOT_TARGET_FACTORY) {
                break;
            }
            board_run(&b, 40000, false);
        }
        UT_EQ(d.target, DL_BOOT_TARGET_FACTORY);
    }
}

static void scenario_t13c_rollover_keeps_history(void)
{
    /* Crash loop that straddles the sector rollover is still caught after 5 boots. */
    board_t b;
    board_init(&b);
    for (size_t i = 0; i < SEC - 2; i++) {
        board_cycle(&b, 200000, false);
    }
    int n = 0;
    dl_boot_decision_t d;
    do {
        d = board_cycle(&b, 40000, false);
        n++;
    } while (d.target != DL_BOOT_TARGET_FACTORY && n < 20);
    UT_EQ(n, 6);
    UT_EQ(d.reason, DL_BOOT_REASON_CRASH_LOOP);
}

static void scenario_t13c_power_loss_after_rollover_erase(void)
{
    /* Erase done, carry-over lost: the log is empty, which only delays the guards (never a wrong recovery). */
    board_t b;
    board_init(&b);
    memset(b.flash, 0xFF, SEC);
    UT_EQ(board_cycle(&b, 200000, false).target, DL_BOOT_TARGET_STANDARD);
}

static void scenario_t20_recovery_crashes_falls_back_to_app(void)
{
    board_t b;
    board_init(&b);
    board_cycle(&b, 200000, false);
    /* Hostile app: 3 quick power cycles -> recovery, which crashes at start every time. */
    for (int i = 0; i < 3; i++) {
        board_cycle(&b, 1000, false);
    }
    int factory = 0;
    dl_boot_decision_t d = {0};
    for (int i = 0; i < 12; i++) {
        d = board_boot(&b);
        if (d.target == DL_BOOT_TARGET_FACTORY) {
            factory++;
            board_run(&b, 500, false); /* recovery crashes */
        } else {
            break; /* app gets a chance */
        }
    }
    UT_EQ(d.target, DL_BOOT_TARGET_APP);
    UT_EQ(d.reason, DL_BOOT_REASON_RECOVERY_LOOP);
    UT_EQ(factory, 3);
    /* The valid app runs fine: the board stays on the app. */
    board_run(&b, 200000, false);
    UT_EQ(board_cycle(&b, 200000, false).target, DL_BOOT_TARGET_STANDARD);
}

/* ------------------------------------------------------------------------------------------------ */
/* Retained RTC block                                                                               */
/* ------------------------------------------------------------------------------------------------ */

static void rtc_crc32_known_value(void)
{
    UT_EQ(dl_crc32("123456789", 9), 0xCBF43926u);
}

static void rtc_garbage_is_invalid(void)
{
    dl_rtc_t r;
    memset(&r, 0xA5, sizeof(r));
    UT_CHECK(!dl_rtc_valid(&r));
    memset(&r, 0, sizeof(r));
    UT_CHECK(!dl_rtc_valid(&r));
}

static void rtc_seal_and_tamper(void)
{
    dl_rtc_t r;
    dl_rtc_init(&r);
    UT_CHECK(dl_rtc_valid(&r));
    UT_EQ(r.req, DL_RTC_REQ_NONE);
    r.req = DL_RTC_REQ_RECOVERY;
    UT_CHECK(!dl_rtc_valid(&r));
    dl_rtc_seal(&r);
    UT_CHECK(dl_rtc_valid(&r));
}

int main(void)
{
    UT_RUN(bootlog_scan_empty);
    UT_RUN(bootlog_scan_newest_first);
    UT_RUN(bootlog_scan_skips_holes);
    UT_RUN(bootlog_scan_full_sector);
    UT_RUN(bootlog_append_simple);
    UT_RUN(bootlog_append_rollover_carries_last_8);

    UT_RUN(decide_empty_log_is_normal);
    UT_RUN(decide_deep_sleep_not_logged);
    UT_RUN(decide_request_goes_to_factory);
    UT_RUN(decide_quick_reset_needs_three);
    UT_RUN(decide_quick_reset_broken_by_alive);
    UT_RUN(decide_clean_restarts_are_not_quick);
    UT_RUN(decide_crash_loop_needs_five);
    UT_RUN(decide_crash_loop_broken_by_stable_or_clean);
    UT_RUN(decide_recovery_loop_boots_app);
    UT_RUN(decide_recovery_loop_short_history_is_normal);
    UT_RUN(decide_request_beats_recovery_loop);
    UT_RUN(decide_gpio);
    UT_RUN(decide_torn_records_count_as_unconfirmed);
    UT_RUN(decide_reason_names);

    UT_RUN(scenario_good_app_never_recovers_across_rollovers);
    UT_RUN(scenario_t11_crash_loop_after_40s);
    UT_RUN(scenario_t12_three_quick_power_cycles);
    UT_RUN(scenario_t13a_deep_sleep_for_an_hour);
    UT_RUN(scenario_t13b_many_planned_restarts);
    UT_RUN(scenario_t13c_torn_record_in_good_run);
    UT_RUN(scenario_t13c_torn_records_crash_loop_still_caught);
    UT_RUN(scenario_t13c_rollover_keeps_history);
    UT_RUN(scenario_t13c_power_loss_after_rollover_erase);
    UT_RUN(scenario_t20_recovery_crashes_falls_back_to_app);

    UT_RUN(rtc_crc32_known_value);
    UT_RUN(rtc_garbage_is_invalid);
    UT_RUN(rtc_seal_and_tamper);

    return UT_SUMMARY();
}
