/*
 * Host unit tests for the button logic (SPEC §5.9).
 */
#include "dl_button.h"
#include "unity_lite.h"

UT_MAIN_DEFS

/* Hold from t0 for `ms` (samples every 50 ms), then release; count actions. */
static void hold(dl_button_t *b, uint64_t *t, uint64_t ms, int *ap, int *rec)
{
    uint64_t end = *t + ms;
    for (; *t <= end; *t += 50) {
        dl_btn_action_t a = dl_button_step(b, true, *t);
        *ap += (a == DL_BTN_AP);
        *rec += (a == DL_BTN_RECOVERY);
    }
    dl_button_step(b, false, *t);
    *t += 50;
}

static void button_short_press_does_nothing(void)
{
    dl_button_t b;
    dl_button_init(&b, false);
    uint64_t t = 1000;
    int ap = 0, rec = 0;
    hold(&b, &t, 2900, &ap, &rec);
    UT_EQ(ap, 0);
    UT_EQ(rec, 0);
}

static void button_3s_opens_ap_once(void)
{
    dl_button_t b;
    dl_button_init(&b, false);
    uint64_t t = 1000;
    int ap = 0, rec = 0;
    hold(&b, &t, 5000, &ap, &rec);
    UT_EQ(ap, 1);
    UT_EQ(rec, 0);
    hold(&b, &t, 3100, &ap, &rec); /* a second press opens it again */
    UT_EQ(ap, 2);
}

static void button_10s_recovery(void)
{
    dl_button_t b;
    dl_button_init(&b, false);
    uint64_t t = 1000;
    int ap = 0, rec = 0;
    hold(&b, &t, 30000, &ap, &rec);
    UT_EQ(ap, 1);
    UT_EQ(rec, 1); /* once, even if held much longer */
}

static void button_stuck_at_boot_ignored_until_released(void)
{
    dl_button_t b;
    dl_button_init(&b, true);
    uint64_t t = 0;
    int ap = 0, rec = 0;
    for (; t < 60000; t += 50) { /* held for a minute from boot: e.g. something on the board holds GPIO0 */
        dl_btn_action_t a = dl_button_step(&b, true, t);
        ap += (a == DL_BTN_AP);
        rec += (a == DL_BTN_RECOVERY);
    }
    UT_EQ(ap, 0);
    UT_EQ(rec, 0);
    dl_button_step(&b, false, t);
    t += 50;
    hold(&b, &t, 3500, &ap, &rec); /* a real press after the release works */
    UT_EQ(ap, 1);
}

int main(void)
{
    UT_RUN(button_short_press_does_nothing);
    UT_RUN(button_3s_opens_ap_once);
    UT_RUN(button_10s_recovery);
    UT_RUN(button_stuck_at_boot_ignored_until_released);
    return UT_SUMMARY();
}
