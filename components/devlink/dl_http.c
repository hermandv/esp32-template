/*
 * HTTP API (SPEC §5.6), port 80, prefix /api/v1, plus the config-AP setup page and captive-portal redirect (§5.4).
 * One server task; all handlers share static buffers. Mutating requests need X-Api-Key and X-Device-Id.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "devlink.h"
#include "dl_priv.h"
#include "esp_core_dump.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "dl_http";

#define BODY_MAX 1024
#define RESP_MAX 4096

static httpd_handle_t s_server;
static char s_body[BODY_MAX + 1];
static char s_resp[RESP_MAX];
static volatile int64_t s_heartbeat_us;
static esp_timer_handle_t s_reboot_timer;
static int s_reboot_target; /* 0 restart, 1 recovery */

extern const char *dl_web_page(void);

/* ---- Helpers ---- */

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *error)
{
    dl_jw_t w;
    dl_jw_init(&w, s_resp, sizeof(s_resp));
    dl_jw_raw(&w, "{");
    dl_jw_kv_str(&w, "error", error);
    dl_jw_raw(&w, "}");
    return send_json(req, status, s_resp);
}

static esp_err_t send_builder(httpd_req_t *req, void (*fill)(dl_jw_t *w))
{
    dl_jw_t w;
    dl_jw_init(&w, s_resp, sizeof(s_resp));
    fill(&w);
    if (w.overflow) {
        return send_error(req, "500 Internal Server Error", "response_too_large");
    }
    return send_json(req, "200 OK", s_resp);
}

static void norm_mac(const char *in, char *out, size_t size)
{
    size_t n = 0;
    for (; (*in != '\0') && (n + 1u < size); in++) {
        if (isxdigit((unsigned char)*in)) {
            out[n++] = (char)tolower((unsigned char)*in);
        }
    }
    out[n] = '\0';
}

/* Mutating requests: API key and device id (SPEC §5.6). Sends the error itself. */
static bool authorized(httpd_req_t *req)
{
    char v[48];
    if ((httpd_req_get_hdr_value_str(req, "X-Api-Key", v, sizeof(v)) != ESP_OK) ||
        (strcmp(v, CONFIG_DEVLINK_API_KEY) != 0)) {
        send_error(req, "401 Unauthorized", "bad_api_key");
        return false;
    }
    char want[13];
    char got[13];
    norm_mac(dl_ident()->mac_str, want, sizeof(want));
    if (httpd_req_get_hdr_value_str(req, "X-Device-Id", v, sizeof(v)) != ESP_OK) {
        v[0] = '\0';
    }
    norm_mac(v, got, sizeof(got));
    if (strcmp(want, got) != 0) {
        send_error(req, "409 Conflict", "wrong_device"); /* DHCP gave this IP to another board (test 22) */
        return false;
    }
    return true;
}

/* Read a small body into s_body. Sends the error itself. */
static bool read_body(httpd_req_t *req, size_t *len)
{
    if (req->content_len > BODY_MAX) {
        send_error(req, "413 Payload Too Large", "body_too_large");
        return false;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, s_body + got, req->content_len - got);
        if (r <= 0) {
            send_error(req, "408 Request Timeout", "body_incomplete");
            return false;
        }
        got += (size_t)r;
    }
    s_body[got] = '\0';
    if (!dl_json_valid_object(s_body, got)) {
        send_error(req, "400 Bad Request", "invalid_json");
        return false;
    }
    *len = got;
    return true;
}

static uint32_t peer_ipv4(httpd_req_t *req)
{
    struct sockaddr_storage a;
    socklen_t al = sizeof(a);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&a, &al) != 0) {
        return 0;
    }
    if (a.ss_family == AF_INET) {
        return ((struct sockaddr_in *)&a)->sin_addr.s_addr;
    }
#if CONFIG_LWIP_IPV6
    if (a.ss_family == AF_INET6) {
        return ((struct sockaddr_in6 *)&a)->sin6_addr.un.u32_addr[3]; /* IPv4-mapped */
    }
#endif
    return 0;
}

static void reboot_cb(void *arg)
{
    (void)arg;
    if (s_reboot_target == 1) {
        devlink_restart_to_recovery();
    }
    devlink_restart();
}

/* Reply first, then restart after 500 ms (SPEC §7.1 step 6). */
void dl_http_schedule_reboot(int target)
{
    s_reboot_target = target;
    if (s_reboot_timer == NULL) {
        const esp_timer_create_args_t a = {.callback = reboot_cb, .name = "dl_reboot"};
        (void)esp_timer_create(&a, &s_reboot_timer);
    }
    (void)esp_timer_start_once(s_reboot_timer, 500000);
}

/* ---- Handlers ---- */

static esp_err_t h_info(httpd_req_t *req)
{
    return send_builder(req, dl_info_json);
}

static esp_err_t h_status(httpd_req_t *req)
{
    return send_builder(req, dl_status_json);
}

static void config_json(dl_jw_t *w)
{
    dlcfg_t c;
    (void)dlcfg_load(&c);
    dl_jw_raw(w, "{");
    dl_jw_kv_str(w, "label", c.label);
    dl_jw_kv_uint(w, "ap_window_s", c.ap_window_s);
    dl_jw_kv_uint(w, "button_window_s", c.button_window_s);
    dl_jw_kv_uint(w, "log_level", c.log_level);
    dl_jw_raw(w, "}");
}

static esp_err_t h_config_get(httpd_req_t *req)
{
    return send_builder(req, config_json);
}

static esp_err_t h_config_post(httpd_req_t *req)
{
    size_t len;
    if (!authorized(req) || !read_body(req, &len)) {
        return ESP_OK;
    }
    dlcfg_t c;
    (void)dlcfg_load(&c);
    char label[33];
    long v;
    dl_json_err_t e = dl_json_get_str(s_body, len, "label", label, sizeof(label));
    if (e == DL_JSON_OK) {
        memcpy(c.label, label, sizeof(c.label));
    } else if (e != DL_JSON_MISSING) {
        return send_error(req, "400 Bad Request", "label_invalid");
    }
    if (dl_json_get_int(s_body, len, "ap_window_s", &v) == DL_JSON_OK) {
        if ((v < 0) || (v > 3600)) {
            return send_error(req, "400 Bad Request", "ap_window_s_range");
        }
        c.ap_window_s = (uint16_t)v;
    }
    if (dl_json_get_int(s_body, len, "button_window_s", &v) == DL_JSON_OK) {
        if ((v < 10) || (v > 3600)) {
            return send_error(req, "400 Bad Request", "button_window_s_range");
        }
        c.button_window_s = (uint16_t)v;
    }
    if (dl_json_get_int(s_body, len, "log_level", &v) == DL_JSON_OK) {
        if ((v < 0) || (v > 5)) {
            return send_error(req, "400 Bad Request", "log_level_range");
        }
        c.log_level = (uint8_t)v;
        esp_log_level_set("*", (esp_log_level_t)v);
    }
    if (dlcfg_save(&c) != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "config_save_failed");
    }
    dl_ident_set_label(c.label);
    dl_disc_update();
    return send_builder(req, config_json);
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    size_t len;
    if (!authorized(req) || !read_body(req, &len)) {
        return ESP_OK;
    }
    char ssid[33];
    char pass[65] = "";
    if (dl_json_get_str(s_body, len, "ssid", ssid, sizeof(ssid)) != DL_JSON_OK) {
        return send_error(req, "400 Bad Request", "ssid_invalid");
    }
    dl_json_err_t e = dl_json_get_str(s_body, len, "password", pass, sizeof(pass));
    if ((e != DL_JSON_OK) && (e != DL_JSON_MISSING)) {
        return send_error(req, "400 Bad Request", "password_invalid");
    }
    dl_wifi_status_t st;
    dl_wifi_get_status(&st);
    dl_wifi_src_t src = dl_wifi_src_for_ip(peer_ipv4(req));
    if (dl_wifi_set(src, ssid, pass) != ESP_OK) {
        return send_error(req, "400 Bad Request", "wifi_settings_invalid");
    }
    /* Over the LAN while connected: pending, reverts after 30 s if it does not connect (I6). */
    bool pending = (src == DL_WIFI_SRC_LAN) && st.sta_connected;
    return send_json(req, "200 OK", pending ? "{\"ok\":true,\"mode\":\"pending\",\"revert_after_s\":30}"
                                            : "{\"ok\":true,\"mode\":\"saved\"}");
}

static esp_err_t h_scan(httpd_req_t *req)
{
    static dl_wifi_ap_rec_t recs[20];
    size_t n = 0;
    if (dl_wifi_scan(recs, 20, &n) != ESP_OK) {
        return send_error(req, "503 Service Unavailable", "scan_failed");
    }
    dl_jw_t w;
    dl_jw_init(&w, s_resp, sizeof(s_resp));
    dl_jw_raw(&w, "{\"networks\":[");
    for (size_t i = 0; i < n; i++) {
        dl_jw_raw(&w, (i > 0u) ? ",{" : "{");
        dl_jw_kv_str(&w, "ssid", recs[i].ssid);
        dl_jw_kv_int(&w, "rssi", recs[i].rssi);
        dl_jw_kv_uint(&w, "channel", recs[i].channel);
        dl_jw_kv_bool(&w, "secure", recs[i].secure);
        dl_jw_raw(&w, "}");
    }
    dl_jw_raw(&w, "]}");
    return send_json(req, "200 OK", s_resp);
}

static esp_err_t h_ota(httpd_req_t *req)
{
    char q[64] = "";
    char target[16] = "app";
    char force[4] = "0";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        (void)httpd_query_key_value(q, "target", target, sizeof(target));
        (void)httpd_query_key_value(q, "force", force, sizeof(force));
    }
    if (!authorized(req)) {
        return ESP_OK;
    }
    bool to_recovery = (strcmp(target, "recovery") == 0);
    if (!to_recovery && (strcmp(target, "app") != 0)) {
        return send_error(req, "400 Bad Request", "bad_target");
    }
    return dl_ota_handle(req, to_recovery, strcmp(force, "1") == 0);
}

static void progress_json(dl_jw_t *w)
{
    dl_ota_progress_t p;
    dl_ota_get_progress(&p);
    dl_jw_raw(w, "{");
    dl_jw_kv_str(w, "state", p.state);
    dl_jw_kv_uint(w, "written", p.written);
    dl_jw_kv_uint(w, "total", p.total);
    dl_jw_kv_str(w, "error", p.error);
    dl_jw_kv_str(w, "version", p.version);
    dl_jw_raw(w, "}");
}

static esp_err_t h_ota_progress(httpd_req_t *req)
{
    return send_builder(req, progress_json);
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    size_t len;
    if (!authorized(req) || !read_body(req, &len)) {
        return ESP_OK;
    }
    if (dl_ota_busy()) {
        return send_error(req, "409 Conflict", "ota_in_progress");
    }
    char target[12] = "app";
    (void)dl_json_get_str(s_body, len, "target", target, sizeof(target));
    if (strcmp(target, "recovery") == 0) {
        dl_http_schedule_reboot(1);
        return send_json(req, "200 OK", "{\"ok\":true,\"target\":\"recovery\"}");
    }
    int slot = -1;
    if (strcmp(target, "app") == 0) {
        if (!dl_valid_app_slot(&slot)) {
            return send_error(req, "409 Conflict", "no_valid_app");
        }
    } else if ((strcmp(target, "slot0") == 0) || (strcmp(target, "slot1") == 0)) {
        slot = target[4] - '0';
        /* Only a slot that holds a usable image. */
        const esp_partition_t *p = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + slot), NULL);
        esp_app_desc_t d;
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        if ((p == NULL) || (esp_ota_get_partition_description(p, &d) != ESP_OK) ||
            ((esp_ota_get_state_partition(p, &st) == ESP_OK) &&
             ((st == ESP_OTA_IMG_INVALID) || (st == ESP_OTA_IMG_ABORTED)))) {
            return send_error(req, "409 Conflict", "slot_not_valid");
        }
    } else {
        return send_error(req, "400 Bad Request", "bad_target");
    }
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + slot), NULL);
    if (esp_ota_get_boot_partition() != p) {
        esp_err_t err = esp_ota_set_boot_partition(p);
        if (err != ESP_OK) {
            return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
        }
    }
    dl_http_schedule_reboot(0);
    snprintf(s_resp, sizeof(s_resp), "{\"ok\":true,\"target\":\"%s\"}", p->label);
    return send_json(req, "200 OK", s_resp);
}

static esp_err_t h_factory_reset(httpd_req_t *req)
{
    size_t len;
    if (!authorized(req) || !read_body(req, &len)) {
        return ESP_OK;
    }
    bool nvs = false;
    (void)dl_json_get_bool(s_body, len, "nvs", &nvs);
    const esp_partition_t *cfg = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "dlcfg");
    if ((cfg == NULL) || (esp_partition_erase_range(cfg, 0, cfg->size) != ESP_OK)) {
        return send_error(req, "500 Internal Server Error", "erase_failed");
    }
    if (nvs) {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
        if (p != NULL) {
            (void)esp_partition_erase_range(p, 0, p->size);
        }
    }
    dl_ident_set_label("");
    dl_disc_update();
    dl_wifi_forget();
    return send_json(req, "200 OK", nvs ? "{\"ok\":true,\"erased\":[\"dlcfg\",\"nvs\"]}" : "{\"ok\":true,\"erased\":[\"dlcfg\"]}");
}

static esp_err_t h_coredump_get(httpd_req_t *req)
{
    size_t addr = 0;
    size_t size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK) {
        return send_error(req, "404 Not Found", "no_coredump");
    }
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (p == NULL) {
        return send_error(req, "404 Not Found", "no_coredump_partition");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.elf\"");
    for (size_t off = 0; off < size;) {
        size_t n = (size - off > sizeof(s_resp)) ? sizeof(s_resp) : size - off;
        if ((esp_partition_read(p, (addr - p->address) + off, s_resp, n) != ESP_OK) ||
            (httpd_resp_send_chunk(req, s_resp, (ssize_t)n) != ESP_OK)) {
            return ESP_FAIL;
        }
        off += n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_coredump_delete(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    esp_err_t err = esp_core_dump_image_erase();
    return (err == ESP_OK) ? send_json(req, "200 OK", "{\"ok\":true}")
                           : send_error(req, "500 Internal Server Error", esp_err_to_name(err));
}

static esp_err_t h_channels(httpd_req_t *req)
{
    return send_builder(req, dl_stream_channels_json);
}

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, dl_web_page());
}

/* Captive portal: unknown paths from config-AP clients go to the setup page; API paths get a JSON 404. */
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    if ((strncmp(req->uri, "/api/", 5) != 0) && (dl_wifi_src_for_ip(peer_ipv4(req)) == DL_WIFI_SRC_AP)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        return httpd_resp_send(req, NULL, 0);
    }
    return send_error(req, "404 Not Found", "not_found");
}

/* Socket closed (client gone, LRU purge, error): drop its stream subscription, then close it. */
static void on_close(httpd_handle_t hd, int fd)
{
    (void)hd;
    dl_stream_on_close(fd);
    (void)close(fd);
}

static void heartbeat_work(void *arg)
{
    (void)arg;
    s_heartbeat_us = esp_timer_get_time();
}

uint32_t dl_http_heartbeat_age_ms(void)
{
    if (s_server == NULL) {
        return UINT32_MAX;
    }
    (void)httpd_queue_work(s_server, heartbeat_work, NULL); /* proves the server task still runs */
    return (uint32_t)((esp_timer_get_time() - s_heartbeat_us) / 1000);
}

esp_err_t dl_http_start(void)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_root},
        {.uri = "/api/v1/info", .method = HTTP_GET, .handler = h_info},
        {.uri = "/api/v1/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/v1/config", .method = HTTP_GET, .handler = h_config_get},
        {.uri = "/api/v1/config", .method = HTTP_POST, .handler = h_config_post},
        {.uri = "/api/v1/wifi", .method = HTTP_POST, .handler = h_wifi},
        {.uri = "/api/v1/wifi/scan", .method = HTTP_GET, .handler = h_scan},
        {.uri = "/api/v1/ota", .method = HTTP_POST, .handler = h_ota},
        {.uri = "/api/v1/ota/progress", .method = HTTP_GET, .handler = h_ota_progress},
        {.uri = "/api/v1/reboot", .method = HTTP_POST, .handler = h_reboot},
        {.uri = "/api/v1/factory-reset", .method = HTTP_POST, .handler = h_factory_reset},
        {.uri = "/api/v1/coredump", .method = HTTP_GET, .handler = h_coredump_get},
        {.uri = "/api/v1/coredump", .method = HTTP_DELETE, .handler = h_coredump_delete},
        {.uri = "/api/v1/channels", .method = HTTP_GET, .handler = h_channels},
        {.uri = "/ws", .method = HTTP_GET, .handler = dl_stream_ws_handler, .is_websocket = true,
         .ws_post_handshake_cb = dl_stream_ws_post_handshake},
    };
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = sizeof(uris) / sizeof(uris[0]) + 2u;
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 10; /* OTA idle timeout (SPEC §7.1 step 5) */
    cfg.send_wait_timeout = 10;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 8; /* HTTP + up to 2 WebSocket clients + an OTA (SPEC 5.7) */
    cfg.close_fn = on_close;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uris[i]));
    }
    ESP_ERROR_CHECK(httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, h_404));
    ESP_ERROR_CHECK(dl_stream_start(s_server));
    s_heartbeat_us = esp_timer_get_time();
    return ESP_OK;
}
