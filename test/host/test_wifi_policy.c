/*
 * Host unit tests for the Wi-Fi policy (SPEC §5.4, §6.3; acceptance tests 24, 25 logic).
 */
#include <string.h>

#include "dl_wifi_policy.h"
#include "unity_lite.h"

UT_MAIN_DEFS

#define S(x) ((uint64_t)(x) * 1000u)

/* Run ticks every 500 ms from *now to `until`, counting connect attempts. */
static int tick_to(dl_wifi_policy_t *p, uint64_t *now, uint64_t until, dl_wifi_actions_t *last)
{
    int attempts = 0;
    while (*now < until) {
        *now += 500;
        dl_wifi_actions_t a = dl_wifi_policy_tick(p, *now);
        if (a.connect) {
            attempts++;
            /* The attempt fails 2 s later (handled by the caller's model: here immediately). */
            dl_wifi_policy_sta_down(p, *now);
        }
        if (last != NULL) {
            *last = a;
        }
    }
    return attempts;
}

static void wifi_no_creds_ap_only(void)
{
    dl_wifi_policy_t p;
    uint64_t now = 0;
    dl_wifi_actions_t a = dl_wifi_policy_init(&p, DL_WIFI_MODE_APP, false, 20, 120, now);
    UT_CHECK(a.ap_on);
    UT_CHECK(!a.connect);
    UT_EQ(tick_to(&p, &now, S(600), &a), 0);
    UT_CHECK(a.ap_on); /* stays up until configured */
}

static void wifi_boot_window_then_closed(void)
{
    dl_wifi_policy_t p;
    uint64_t now = 0;
    dl_wifi_actions_t a = dl_wifi_policy_init(&p, DL_WIFI_MODE_APP, true, 20, 120, now);
    UT_CHECK(a.ap_on);
    UT_CHECK(a.connect && !a.use_pending);
    a = dl_wifi_policy_sta_up(&p, S(3));
    UT_CHECK(a.ap_on); /* still inside the 20 s window */
    now = S(3);
    tick_to(&p, &now, S(21), &a);
    UT_CHECK(!a.ap_on);
}

static void wifi_boot_window_zero_is_off(void)
{
    dl_wifi_policy_t p;
    dl_wifi_actions_t a = dl_wifi_policy_init(&p, DL_WIFI_MODE_APP, true, 0, 120, 0);
    UT_CHECK(!a.ap_on);
}

static void wifi_sta_failing_opens_ap_and_backs_off(void)
{
    dl_wifi_policy_t p;
    uint64_t now = 0;
    dl_wifi_actions_t a = dl_wifi_policy_init(&p, DL_WIFI_MODE_APP, true, 0, 120, now);
    dl_wifi_policy_sta_down(&p, S(2)); /* first attempt failed */
    now = S(2);
    tick_to(&p, &now, S(29), &a);
    UT_CHECK(!a.ap_on);
    tick_to(&p, &now, S(31), &a);
    UT_CHECK(a.ap_on); /* 30 s without STA */
    /* Retries: at 17 s (15), 47 (30), 107 (60), 167 (60) ... */
    dl_wifi_policy_init(&p, DL_WIFI_MODE_APP, true, 0, 120, 0);
    dl_wifi_policy_sta_down(&p, S(2));
    now = S(2);
    UT_EQ(tick_to(&p, &now, S(16), NULL), 0);
    UT_EQ(tick_to(&p, &now, S(18), NULL), 1);
    UT_EQ(tick_to(&p, &now, S(46), NULL), 0);
    UT_EQ(tick_to(&p, &now, S(48), NULL), 1);
    UT_EQ(tick_to(&p, &now, S(107) + 500, NULL), 1);
    UT_EQ(tick_to(&p, &now, S(600), NULL), 8); /* every 60 s from then on */
}

static void wifi_ap_stays_until_sta_connects(void)
{
    dl_wifi_policy_t p;
    uint64_t now = 0;
    dl_wifi_actions_t a = dl_wifi_policy_init(&p, DL_WIFI_MODE_APP, true, 0, 120, now);
    dl_wifi_policy_sta_down(&p, S(1));
    now = S(1);
    tick_to(&p, &now, S(3600), &a);
    UT_CHECK(a.ap_on); /* an hour later: still open */
    a = dl_wifi_policy_sta_up(&p, now);
    UT_CHECK(!a.ap_on);
}

static void wifi_retries_paused_with_ap_client(void)
{
    dl_wifi_policy_t p;
    uint64_t now = 0;
    dl_wifi_policy_init(&p, DL_WIFI_MODE_APP, true, 0, 120, now);
    dl_wifi_policy_sta_down(&p, S(1));
    now = S(1);
    dl_wifi_policy_ap_clients(&p, 1, now);
    UT_EQ(tick_to(&p, &now, S(300), NULL), 0); /* no STA attempts while a client is configuring */
    dl_wifi_actions_t a = dl_wifi_policy_ap_clients(&p, 0, now);
    UT_CHECK(a.ap_on);
    UT_CHECK(tick_to(&p, &now, now + S(2), NULL) == 1); /* resumes */
}

static void wifi_client_extends_window(void)
{
    dl_wifi_policy_t p;
    uint64_t now = 0;
    dl_wifi_actions_t a;
    dl_wifi_policy_init(&p, DL_WIFI_MODE_APP, true, 20, 120, now);
    dl_wifi_policy_sta_up(&p, S(2));
    dl_wifi_policy_ap_clients(&p, 1, S(15));
    now = S(15);
    tick_to(&p, &now, S(100), &a);
    UT_CHECK(a.ap_on);
    dl_wifi_policy_ap_clients(&p, 0, S(100));
    tick_to(&p, &now, S(109), &a);
    UT_CHECK(a.ap_on);
    tick_to(&p, &now, S(111), &a);
    UT_CHECK(!a.ap_on);
}

static void wifi_button_window(void)
{
    dl_wifi_policy_t p;
    uint64_t now = 0;
    dl_wifi_actions_t a;
    dl_wifi_policy_init(&p, DL_WIFI_MODE_APP, true, 0, 120, now);
    dl_wifi_policy_sta_up(&p, S(2));
    a = dl_wifi_policy_button(&p, S(10));
    UT_CHECK(a.ap_on);
    now = S(10);
    tick_to(&p, &now, S(129), &a);
    UT_CHECK(a.ap_on);
    tick_to(&p, &now, S(131), &a);
    UT_CHECK(!a.ap_on);
}

static void wifi_recovery_ap_always_on(void)
{
    dl_wifi_policy_t p;
    uint64_t now = 0;
    dl_wifi_actions_t a = dl_wifi_policy_init(&p, DL_WIFI_MODE_RECOVERY, true, 0, 0, now);
    UT_CHECK(a.ap_on && a.connect);
    a = dl_wifi_policy_sta_up(&p, S(2));
    UT_CHECK(a.ap_on);
    now = S(2);
    tick_to(&p, &now, S(3600), &a);
    UT_CHECK(a.ap_on);
}

static void wifi_t24_lan_change_wrong_password_reverts(void)
{
    dl_wifi_policy_t p;
    uint64_t now = 0;
    dl_wifi_policy_init(&p, DL_WIFI_MODE_APP, true, 0, 120, now);
    dl_wifi_policy_sta_up(&p, S(2));
    dl_wifi_actions_t a = dl_wifi_policy_set(&p, DL_WIFI_SRC_LAN, S(100));
    UT_CHECK(a.connect && a.use_pending && !a.save_direct && !a.commit);
    dl_wifi_policy_sta_down(&p, S(103)); /* wrong password */
    now = S(103);
    bool reverted = false;
    bool reconnect_old = false;
    int retries = 0;
    while (now < S(160)) {
        now += 500;
        a = dl_wifi_policy_tick(&p, now);
        if (a.reverted) {
            reverted = true;
            reconnect_old = a.connect && !a.use_pending;
            UT_CHECK(now >= S(145) && now <= S(146));
            break;
        }
        if (a.connect) {
            UT_CHECK(a.use_pending); /* retries before the deadline use the new settings, every 3 s */
            retries++;
            dl_wifi_policy_sta_down(&p, now);
        }
    }
    UT_CHECK(reverted && reconnect_old);
    UT_CHECK(retries >= 12);
    a = dl_wifi_policy_sta_up(&p, now + 2000);
    UT_CHECK(!a.commit); /* old settings back, nothing to save */
    UT_CHECK(!a.ap_on);
}

static void wifi_lan_change_good_password_commits(void)
{
    dl_wifi_policy_t p;
    dl_wifi_policy_init(&p, DL_WIFI_MODE_APP, true, 0, 120, 0);
    dl_wifi_policy_sta_up(&p, S(2));
    dl_wifi_policy_set(&p, DL_WIFI_SRC_LAN, S(100));
    dl_wifi_actions_t a = dl_wifi_policy_sta_up(&p, S(104));
    UT_CHECK(a.commit);
    a = dl_wifi_policy_tick(&p, S(160));
    UT_CHECK(!a.reverted);
}

static void wifi_lan_change_survives_one_failed_attempt(void)
{
    /* One transient failure (missed beacon, 4-way timeout) right away, then a slow association + DHCP: the retry
     * 3 s later still has most of the 45 s window, and the change commits. */
    dl_wifi_policy_t p;
    dl_wifi_policy_init(&p, DL_WIFI_MODE_APP, true, 0, 120, 0);
    dl_wifi_policy_sta_up(&p, S(2));
    dl_wifi_policy_set(&p, DL_WIFI_SRC_LAN, S(100));
    dl_wifi_policy_sta_down(&p, S(108));
    dl_wifi_actions_t a = dl_wifi_policy_tick(&p, S(111));
    UT_CHECK(a.connect && a.use_pending);
    a = dl_wifi_policy_tick(&p, S(140)); /* still associating / waiting for DHCP */
    UT_CHECK(!a.reverted && !a.connect);
    a = dl_wifi_policy_sta_up(&p, S(142));
    UT_CHECK(a.commit);
}

static void wifi_t25_ap_change_saved_directly(void)
{
    /* Saved password wrong -> AP opens -> new settings through the AP -> saved directly, no revert. */
    dl_wifi_policy_t p;
    uint64_t now = 0;
    dl_wifi_actions_t a;
    dl_wifi_policy_init(&p, DL_WIFI_MODE_APP, true, 0, 120, now);
    dl_wifi_policy_sta_down(&p, S(2));
    now = S(2);
    tick_to(&p, &now, S(40), &a);
    UT_CHECK(a.ap_on);
    a = dl_wifi_policy_set(&p, DL_WIFI_SRC_AP, now);
    UT_CHECK(a.save_direct && a.connect && !a.use_pending);
    a = dl_wifi_policy_sta_up(&p, now + S(3));
    UT_CHECK(!a.commit);
    UT_CHECK(!a.ap_on);
}

static void wifi_usb_set_without_creds(void)
{
    dl_wifi_policy_t p;
    dl_wifi_policy_init(&p, DL_WIFI_MODE_APP, false, 20, 120, 0);
    dl_wifi_actions_t a = dl_wifi_policy_set(&p, DL_WIFI_SRC_USB, S(5));
    UT_CHECK(a.save_direct && a.connect);
    /* The source is decided when the request arrives (dl_wifi_set): a LAN request stays pending even if the link
     * dropped in the second before the policy sees it (I6). */
    dl_wifi_policy_init(&p, DL_WIFI_MODE_APP, true, 0, 120, 0);
    dl_wifi_policy_sta_down(&p, S(2));
    a = dl_wifi_policy_set(&p, DL_WIFI_SRC_LAN, S(5));
    UT_CHECK(!a.save_direct && a.use_pending);
}

static void wifi_link_lost_reconnects_at_once(void)
{
    dl_wifi_policy_t p;
    dl_wifi_policy_init(&p, DL_WIFI_MODE_APP, true, 0, 120, 0);
    dl_wifi_policy_sta_up(&p, S(2));
    dl_wifi_actions_t a = dl_wifi_policy_sta_down(&p, S(50));
    UT_CHECK(a.connect);
}

static void wifi_forget(void)
{
    dl_wifi_policy_t p;
    dl_wifi_policy_init(&p, DL_WIFI_MODE_APP, true, 0, 120, 0);
    dl_wifi_policy_sta_up(&p, S(2));
    dl_wifi_actions_t a = dl_wifi_policy_forget(&p, S(3));
    UT_CHECK(a.ap_on);
    uint64_t now = S(3);
    UT_EQ(tick_to(&p, &now, S(300), NULL), 0);
}

int main(void)
{
    UT_RUN(wifi_no_creds_ap_only);
    UT_RUN(wifi_boot_window_then_closed);
    UT_RUN(wifi_boot_window_zero_is_off);
    UT_RUN(wifi_sta_failing_opens_ap_and_backs_off);
    UT_RUN(wifi_ap_stays_until_sta_connects);
    UT_RUN(wifi_retries_paused_with_ap_client);
    UT_RUN(wifi_client_extends_window);
    UT_RUN(wifi_button_window);
    UT_RUN(wifi_recovery_ap_always_on);
    UT_RUN(wifi_t24_lan_change_wrong_password_reverts);
    UT_RUN(wifi_lan_change_good_password_commits);
    UT_RUN(wifi_lan_change_survives_one_failed_attempt);
    UT_RUN(wifi_t25_ap_change_saved_directly);
    UT_RUN(wifi_usb_set_without_creds);
    UT_RUN(wifi_link_lost_reconnects_at_once);
    UT_RUN(wifi_forget);
    return UT_SUMMARY();
}
