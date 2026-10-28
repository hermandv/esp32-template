/*
 * devlink internal interfaces between the core modules (not for test modules).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dl_json.h"
#include "dl_wifi_policy.h"
#include "esp_err.h"
#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Identity (SPEC §4) ---- */

typedef struct {
    uint8_t mac[6];
    char id[7];          /* last 3 MAC bytes, upper-case hex */
    char mac_str[18];    /* AA:BB:CC:DD:EE:FF */
    char hostname[16];   /* esp-<id lower> */
    char label[33];
    const char *board;
    const char *mode;    /* "app" / "recovery" */
} dl_ident_t;

const dl_ident_t *dl_ident(void);
void dl_ident_init(bool recovery);
void dl_ident_set_label(const char *label);

/* ---- Wi-Fi (dl_wifi.c) ---- */

typedef struct {
    bool started;
    bool sta_connected;
    bool have_creds;
    bool ap_on;
    uint32_t ap_clients;
    char ssid[33];
    esp_ip4_addr_t ip;
    int8_t rssi;
    uint8_t channel;
    int8_t tx_power_dbm;
    bool pending;
    const char *last_error;  /* e.g. "wifi_change_failed", NULL = none */
    uint32_t stack_restarts;
} dl_wifi_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool secure;
} dl_wifi_ap_rec_t;

esp_err_t dl_wifi_start(bool recovery);
void dl_wifi_get_status(dl_wifi_status_t *st);
/** New credentials; src decides pending (LAN) or direct save (AP/USB). */
esp_err_t dl_wifi_set(dl_wifi_src_t src, const char *ssid, const char *password);
/** Blocking scan (<= ~4 s). */
esp_err_t dl_wifi_scan(dl_wifi_ap_rec_t *out, size_t max, size_t *n);
/** Button held 3 s: open the config AP for the button window. */
void dl_wifi_button(void);
/** Factory reset: forget credentials, AP only. */
void dl_wifi_forget(void);
/** Wait until the STA has an IP or the attempt failed. Returns true if connected. */
bool dl_wifi_wait_connected(uint32_t timeout_ms);
/** Milliseconds since the Wi-Fi task last ran (health check). */
uint32_t dl_wifi_heartbeat_age_ms(void);
/** Request source of the current HTTP request: AP or LAN. */
dl_wifi_src_t dl_wifi_src_for_ip(uint32_t peer_ip);

/* ---- HTTP / OTA / discovery / DNS ---- */

esp_err_t dl_http_start(void);
uint32_t dl_http_heartbeat_age_ms(void);
esp_err_t dl_disc_start(void);
void dl_disc_update(void);          /* label/mode changed: refresh mDNS TXT */
esp_err_t dl_dns_start(void);

typedef struct {
    const char *state;  /* "idle", "receiving", "verifying", "done", "error" */
    uint32_t written;
    uint32_t total;
    const char *error;
    char version[33];
} dl_ota_progress_t;

/* OTA upload (dl_ota.c), called by the HTTP layer with auth checked. Runs the upload in its own task (async request). */
struct httpd_req;
esp_err_t dl_ota_handle(struct httpd_req *req, bool to_recovery, bool force);
void dl_ota_get_progress(dl_ota_progress_t *p);
bool dl_ota_busy(void);

/* ---- Info / status JSON (dl_info.c), shared by HTTP, console, Improv and discovery ---- */

void dl_info_json(dl_jw_t *w);
void dl_status_json(dl_jw_t *w);
/** Short discovery record (mDNS TXT fields / UDP reply). */
void dl_disc_json(dl_jw_t *w);
const char *dl_fw_version(void);
const char *dl_fw_project(void);
/** True if a slot holds a valid app (for /reboot target=app). */
bool dl_valid_app_slot(int *slot);
/** Partition table matches the frozen layout (SPEC §3.7). */
bool dl_partitions_ok(void);

/* ---- Health (recovery, SPEC §6.4) and RTC WDT ---- */

void dl_rwdt_set(uint32_t ms);
void dl_rwdt_feed(void);
void dl_rwdt_disable(void);
esp_err_t dl_health_start(void);
esp_err_t dl_app_start(void);
void dl_note_error(const char *err);
const char *dl_last_error(void);

#ifdef __cplusplus
}
#endif
