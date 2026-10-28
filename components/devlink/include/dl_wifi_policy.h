/*
 * Wi-Fi policy (SPEC §5.4, §6.3): when the config AP is open, when the STA (re)connects, and the safe Wi-Fi change
 * (I6). Pure C, no ESP-IDF calls: the glue (dl_wifi.c) feeds events with a millisecond clock and carries out the
 * returned actions.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DL_WIFI_STA_FAIL_AP_MS   30000u  /* STA failing this long -> config AP opens until it connects */
#define DL_WIFI_RETRY_MIN_MS     15000u
#define DL_WIFI_RETRY_MAX_MS     60000u
#define DL_WIFI_PENDING_MS       30000u  /* safe change over the LAN: revert if not connected by then */
#define DL_WIFI_CLIENT_LINGER_MS 10000u  /* AP stays this long after the last client left */

typedef enum {
    DL_WIFI_MODE_APP = 0,
    DL_WIFI_MODE_RECOVERY = 1,   /* AP always on */
} dl_wifi_mode_t;

typedef enum {
    DL_WIFI_SRC_LAN = 0,         /* over the LAN while connected: pending + revert */
    DL_WIFI_SRC_AP = 1,          /* through the config AP: saved directly */
    DL_WIFI_SRC_USB = 2,         /* Improv over USB: saved directly */
} dl_wifi_src_t;

typedef struct {
    bool ap_on;          /* desired AP state (always reported) */
    bool connect;        /* start a STA connection attempt now */
    bool use_pending;    /* ... with the pending (new) credentials; otherwise the current ones */
    bool commit;         /* pending credentials work: save them as current (previous = old current) */
    bool save_direct;    /* save the new credentials as current right away */
    bool reverted;       /* pending change failed: back on the current credentials (report wifi_change_failed) */
} dl_wifi_actions_t;

typedef struct {
    dl_wifi_mode_t mode;
    uint32_t boot_window_ms;
    uint32_t button_window_ms;

    bool have_creds;
    bool connected;
    bool connecting;
    uint32_t ap_clients;
    uint64_t ap_until;       /* AP window end (ms), 0 = none */
    bool ap_failing;         /* opened because the STA failed for 30 s */
    uint64_t fail_since;     /* STA not connected since (ms) */
    uint64_t next_retry;     /* next STA attempt (ms) */
    uint32_t retry_ms;       /* current back-off interval */
    bool pending;            /* LAN change in progress */
    uint64_t pending_until;
} dl_wifi_policy_t;

/**
 * @brief Start the policy at boot.
 *
 * @requirement SPEC-5.4, SPEC-6.3
 * @parent      I6, I2
 * @test        host:wifi_*, T24, T25
 */
dl_wifi_actions_t dl_wifi_policy_init(dl_wifi_policy_t *p, dl_wifi_mode_t mode, bool have_creds,
                                      uint32_t boot_window_s, uint32_t button_window_s, uint64_t now);

/** Periodic tick (e.g. every 500 ms): retries, AP window end, pending timeout. */
dl_wifi_actions_t dl_wifi_policy_tick(dl_wifi_policy_t *p, uint64_t now);

/** STA got an IP. */
dl_wifi_actions_t dl_wifi_policy_sta_up(dl_wifi_policy_t *p, uint64_t now);

/** STA disconnected (link lost, or a connection attempt failed). */
dl_wifi_actions_t dl_wifi_policy_sta_down(dl_wifi_policy_t *p, uint64_t now);

/** Number of stations on the config AP changed. */
dl_wifi_actions_t dl_wifi_policy_ap_clients(dl_wifi_policy_t *p, uint32_t count, uint64_t now);

/** Button held 3 s: open the AP for the button window. */
dl_wifi_actions_t dl_wifi_policy_button(dl_wifi_policy_t *p, uint64_t now);

/** New Wi-Fi settings arrived (the glue keeps them as "pending" credentials until commit/save). */
dl_wifi_actions_t dl_wifi_policy_set(dl_wifi_policy_t *p, dl_wifi_src_t src, uint64_t now);

/** Credentials were erased (factory reset): AP only. */
dl_wifi_actions_t dl_wifi_policy_forget(dl_wifi_policy_t *p, uint64_t now);

#ifdef __cplusplus
}
#endif
