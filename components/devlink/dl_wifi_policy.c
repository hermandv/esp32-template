/*
 * Wi-Fi policy (SPEC §5.4, §6.3). Pure C, see dl_wifi_policy.h.
 */
#include "dl_wifi_policy.h"

#include <string.h>

static uint64_t max64(uint64_t a, uint64_t b)
{
    return (a > b) ? a : b;
}

static bool ap_wanted(const dl_wifi_policy_t *p, uint64_t now)
{
    return (p->mode == DL_WIFI_MODE_RECOVERY) || !p->have_creds || p->ap_failing || (p->ap_clients > 0u) ||
           (now < p->ap_until);
}

static dl_wifi_actions_t finish(const dl_wifi_policy_t *p, dl_wifi_actions_t a, uint64_t now)
{
    a.ap_on = ap_wanted(p, now);
    return a;
}

static void start_attempt(dl_wifi_policy_t *p, dl_wifi_actions_t *a, bool pending)
{
    a->connect = true;
    a->use_pending = pending;
    p->connecting = true;
}

dl_wifi_actions_t dl_wifi_policy_init(dl_wifi_policy_t *p, dl_wifi_mode_t mode, bool have_creds,
                                      uint32_t boot_window_s, uint32_t button_window_s, uint64_t now)
{
    dl_wifi_actions_t a = {0};

    memset(p, 0, sizeof(*p));
    p->mode = mode;
    p->boot_window_ms = boot_window_s * 1000u;
    p->button_window_ms = button_window_s * 1000u;
    p->have_creds = have_creds;
    p->retry_ms = DL_WIFI_RETRY_MIN_MS;
    p->fail_since = now;
    if (have_creds) {
        p->ap_until = (p->boot_window_ms > 0u) ? now + p->boot_window_ms : 0u;
        start_attempt(p, &a, false);
    }
    return finish(p, a, now);
}

dl_wifi_actions_t dl_wifi_policy_tick(dl_wifi_policy_t *p, uint64_t now)
{
    dl_wifi_actions_t a = {0};

    if (p->pending && (now >= p->pending_until)) {
        /* Safe change failed (I6): back to the credentials that worked. */
        p->pending = false;
        a.reverted = true;
        p->retry_ms = DL_WIFI_RETRY_MIN_MS;
        start_attempt(p, &a, false);
        return finish(p, a, now);
    }
    if (p->have_creds && !p->connected) {
        if (!p->ap_failing && (now - p->fail_since >= DL_WIFI_STA_FAIL_AP_MS)) {
            p->ap_failing = true;
        }
        /* STA retries are paused while someone is on the AP (scans hop channels and drop the client). */
        if (!p->connecting && (p->ap_clients == 0u) && (now >= p->next_retry)) {
            start_attempt(p, &a, p->pending);
        }
    }
    return finish(p, a, now);
}

dl_wifi_actions_t dl_wifi_policy_sta_up(dl_wifi_policy_t *p, uint64_t now)
{
    dl_wifi_actions_t a = {0};

    p->connected = true;
    p->connecting = false;
    p->ap_failing = false;
    p->retry_ms = DL_WIFI_RETRY_MIN_MS;
    if (p->pending) {
        p->pending = false;
        a.commit = true;
    }
    return finish(p, a, now);
}

dl_wifi_actions_t dl_wifi_policy_sta_down(dl_wifi_policy_t *p, uint64_t now)
{
    dl_wifi_actions_t a = {0};

    if (p->connected) {
        /* Link lost: try again right away, the failure clock starts now. */
        p->connected = false;
        p->fail_since = now;
        if (p->ap_clients == 0u) {
            start_attempt(p, &a, p->pending);
        } else {
            p->connecting = false;
            p->next_retry = now;
        }
    } else {
        /* An attempt failed: back off 15 s -> 30 s -> 60 s; a pending change retries soon, so one transient failure
         * (missed beacon, handshake timeout) does not use up its whole window. */
        p->connecting = false;
        if (p->pending) {
            p->next_retry = now + DL_WIFI_PENDING_RETRY_MS;
        } else {
            p->next_retry = now + p->retry_ms;
            p->retry_ms = (p->retry_ms * 2u > DL_WIFI_RETRY_MAX_MS) ? DL_WIFI_RETRY_MAX_MS : p->retry_ms * 2u;
        }
    }
    return finish(p, a, now);
}

dl_wifi_actions_t dl_wifi_policy_ap_clients(dl_wifi_policy_t *p, uint32_t count, uint64_t now)
{
    dl_wifi_actions_t a = {0};

    if ((count == 0u) && (p->ap_clients > 0u)) {
        p->ap_until = max64(p->ap_until, now + DL_WIFI_CLIENT_LINGER_MS);
    }
    p->ap_clients = count;
    return finish(p, a, now);
}

dl_wifi_actions_t dl_wifi_policy_button(dl_wifi_policy_t *p, uint64_t now)
{
    dl_wifi_actions_t a = {0};

    p->ap_until = max64(p->ap_until, now + p->button_window_ms);
    return finish(p, a, now);
}

dl_wifi_actions_t dl_wifi_policy_set(dl_wifi_policy_t *p, dl_wifi_src_t src, uint64_t now)
{
    dl_wifi_actions_t a = {0};

    p->retry_ms = DL_WIFI_RETRY_MIN_MS;
    if (src == DL_WIFI_SRC_LAN) {
        /* Pending: try the new settings, keep the old ones until they work (I6). */
        p->pending = true;
        p->pending_until = now + DL_WIFI_PENDING_MS;
        p->connected = false;
        p->fail_since = now;
        start_attempt(p, &a, true);
        return finish(p, a, now);
    }
    /* Through the AP or USB (someone is at the board, the current settings are failing anyway): save directly. */
    p->pending = false;
    p->have_creds = true;
    p->ap_failing = false;
    p->fail_since = now;
    a.save_direct = true;
    start_attempt(p, &a, false);
    return finish(p, a, now);
}

dl_wifi_actions_t dl_wifi_policy_forget(dl_wifi_policy_t *p, uint64_t now)
{
    dl_wifi_actions_t a = {0};

    p->have_creds = false;
    p->connected = false;
    p->connecting = false;
    p->pending = false;
    p->ap_failing = false;
    return finish(p, a, now);
}
