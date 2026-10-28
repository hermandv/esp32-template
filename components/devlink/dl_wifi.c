/*
 * Wi-Fi glue (SPEC §5.4, §6.3, §6.4): runs dl_wifi_policy in one task and carries out its actions.
 * No NVS (WIFI_STORAGE_RAM): credentials live in dlcfg.
 */
#include <string.h>

#include "devlink.h"
#include "dl_priv.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "dl_wifi";

#define AP_IP_NET      0x0004A8C0u /* 192.168.4.0 (network byte order) */
#define AP_IP_MASK     0x00FFFFFFu
#define TICK_MS        500u
#define BIT_CONNECTED  BIT0
#define BIT_FAILED     BIT1
#define MAX_ERRORS     3
#define MAX_RESTARTS   3

typedef enum {
    EV_STA_UP,
    EV_STA_DOWN,
    EV_AP_CLIENTS,
    EV_SET,
    EV_FORGET,
    EV_BUTTON,
} ev_type_t;

typedef struct {
    ev_type_t type;
    uint32_t arg;
} ev_t;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_lock;          /* all esp_wifi_* calls and the shared state below */
static EventGroupHandle_t s_bits;
static dl_wifi_policy_t s_pol;
static bool s_recovery;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static char s_cur_ssid[33], s_cur_pass[65];
static char s_new_ssid[33], s_new_pass[65];
static dl_wifi_src_t s_new_src;
static bool s_own_disconnect;
static bool s_ap_on;
static bool s_started;
static int8_t s_tx_dbm;
static int s_errors;
static uint32_t s_restarts;
static const char *s_last_error;
static volatile int64_t s_heartbeat_us;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void post(ev_type_t type, uint32_t arg)
{
    ev_t e = {.type = type, .arg = arg};
    if ((s_q != NULL) && (xQueueSend(s_q, &e, 0) != pdTRUE)) {
        ESP_LOGW(TAG, "event queue full");
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            const wifi_event_sta_disconnected_t *d = data;
            post(EV_STA_DOWN, d->reason);
        } else if ((id == WIFI_EVENT_AP_STACONNECTED) || (id == WIFI_EVENT_AP_STADISCONNECTED)) {
            wifi_sta_list_t list;
            uint32_t n = (esp_wifi_ap_get_sta_list(&list) == ESP_OK) ? (uint32_t)list.num : 0u;
            post(EV_AP_CLIENTS, n);
        }
    } else if ((base == IP_EVENT) && (id == IP_EVENT_STA_GOT_IP)) {
        post(EV_STA_UP, 0);
    }
}

/* Count esp_wifi errors; after a few in a row restart the stack, after a few restarts restart the device. */
static void check(esp_err_t err, const char *what)
{
    if (err == ESP_OK) {
        s_errors = 0;
        return;
    }
    ESP_LOGW(TAG, "%s: %s", what, esp_err_to_name(err));
    if (++s_errors < MAX_ERRORS) {
        return;
    }
    s_errors = 0;
    if (++s_restarts > MAX_RESTARTS) {
        ESP_LOGE(TAG, "Wi-Fi stack keeps failing: planned restart");
        devlink_restart(); /* clean restart, not a crash (SPEC §6.4) */
    }
    ESP_LOGW(TAG, "restarting the Wi-Fi stack (%u)", (unsigned)s_restarts);
    (void)esp_wifi_stop();
    (void)esp_wifi_start();
}

static void apply_ap(bool on)
{
    if (on == s_ap_on) {
        return;
    }
    check(esp_wifi_set_mode(on ? WIFI_MODE_APSTA : WIFI_MODE_STA), "set_mode");
    s_ap_on = on;
    ESP_LOGI(TAG, "config AP %s", on ? "on" : "off");
}

static void connect_with(const char *ssid, const char *pass)
{
    wifi_config_t c = {0};
    strncpy((char *)c.sta.ssid, ssid, sizeof(c.sta.ssid));
    strncpy((char *)c.sta.password, pass, sizeof(c.sta.password));
    c.sta.threshold.authmode = (pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    c.sta.pmf_cfg.capable = true;
    c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN; /* hidden SSIDs */
    s_own_disconnect = true;
    (void)esp_wifi_disconnect();
    check(esp_wifi_set_config(WIFI_IF_STA, &c), "set_config");
    xEventGroupClearBits(s_bits, BIT_CONNECTED | BIT_FAILED);
    check(esp_wifi_connect(), "connect");
    ESP_LOGI(TAG, "connecting to '%s'", ssid);
}

static void save_creds(void)
{
    dlcfg_t cfg;
    (void)dlcfg_load(&cfg);
    strncpy(cfg.prev_ssid, cfg.wifi_ssid, sizeof(cfg.prev_ssid));
    strncpy(cfg.prev_pass, cfg.wifi_pass, sizeof(cfg.prev_pass));
    strncpy(cfg.wifi_ssid, s_new_ssid, sizeof(cfg.wifi_ssid));
    strncpy(cfg.wifi_pass, s_new_pass, sizeof(cfg.wifi_pass));
    if (dlcfg_save(&cfg) != ESP_OK) {
        s_last_error = "config_save_failed";
    }
    memcpy(s_cur_ssid, s_new_ssid, sizeof(s_cur_ssid));
    memcpy(s_cur_pass, s_new_pass, sizeof(s_cur_pass));
}

static void run(dl_wifi_actions_t a)
{
    if (a.save_direct || a.commit) {
        save_creds();
        ESP_LOGI(TAG, "credentials saved (%s)", a.commit ? "verified" : "direct");
    }
    if (a.reverted) {
        s_last_error = "wifi_change_failed";
        ESP_LOGW(TAG, "new Wi-Fi settings did not connect within 30 s: reverted");
    }
    apply_ap(a.ap_on);
    if (a.connect) {
        if (a.use_pending) {
            connect_with(s_new_ssid, s_new_pass);
        } else {
            connect_with(s_cur_ssid, s_cur_pass);
        }
    }
}

static void wifi_task(void *arg)
{
    (void)arg;
    for (;;) {
        ev_t e;
        bool got = (xQueueReceive(s_q, &e, pdMS_TO_TICKS(TICK_MS)) == pdTRUE);
        s_heartbeat_us = esp_timer_get_time();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint64_t t = now_ms();
        dl_wifi_actions_t a;
        if (!got) {
            a = dl_wifi_policy_tick(&s_pol, t);
        } else {
            switch (e.type) {
            case EV_STA_UP:
                xEventGroupSetBits(s_bits, BIT_CONNECTED);
                a = dl_wifi_policy_sta_up(&s_pol, t);
                break;
            case EV_STA_DOWN:
                xEventGroupClearBits(s_bits, BIT_CONNECTED);
                if (s_own_disconnect && (e.arg == WIFI_REASON_ASSOC_LEAVE)) {
                    s_own_disconnect = false; /* caused by our own reconnect */
                    a = dl_wifi_policy_tick(&s_pol, t);
                } else {
                    xEventGroupSetBits(s_bits, BIT_FAILED);
                    ESP_LOGI(TAG, "STA down (reason %u)", (unsigned)e.arg);
                    a = dl_wifi_policy_sta_down(&s_pol, t);
                }
                break;
            case EV_AP_CLIENTS:
                a = dl_wifi_policy_ap_clients(&s_pol, e.arg, t);
                break;
            case EV_SET:
                s_last_error = NULL;
                a = dl_wifi_policy_set(&s_pol, s_new_src, t);
                break;
            case EV_FORGET:
                s_cur_ssid[0] = '\0';
                s_cur_pass[0] = '\0';
                s_own_disconnect = true;
                (void)esp_wifi_disconnect();
                a = dl_wifi_policy_forget(&s_pol, t);
                break;
            case EV_BUTTON:
            default:
                a = dl_wifi_policy_button(&s_pol, t);
                break;
            }
        }
        run(a);
        xSemaphoreGive(s_lock);
    }
}

esp_err_t dl_wifi_start(bool recovery)
{
    const dl_ident_t *id = dl_ident();
    dlcfg_t cfg;

    s_recovery = recovery;
    s_lock = xSemaphoreCreateMutex();
    s_bits = xEventGroupCreate();
    s_q = xQueueCreate(16, sizeof(ev_t));
    (void)dlcfg_load(&cfg);
    memcpy(s_cur_ssid, cfg.wifi_ssid, sizeof(s_cur_ssid));
    memcpy(s_cur_pass, cfg.wifi_pass, sizeof(s_cur_pass));
    dl_ident_set_label(cfg.label);

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    (void)esp_netif_set_hostname(s_sta_netif, id->hostname);

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    wcfg.nvs_enable = 0; /* SPEC §5.3: no NVS */
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t ap = {0};
    int n = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "ESP32S3-%s-%s", recovery ? "REC" : "CFG", id->id);
    ap.ap.ssid_len = (uint8_t)n;
    strncpy((char *)ap.ap.password, CONFIG_DEVLINK_AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;
    ap.ap.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    s_ap_on = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    /* Brownout handling (SPEC §6.4): weak supplies + Wi-Fi TX peaks -> brownout loops. */
    s_tx_dbm = 20;
    if (recovery) {
        s_tx_dbm = CONFIG_DEVLINK_TX_POWER_DBM;
        if (esp_reset_reason() == ESP_RST_BROWNOUT) {
            ESP_LOGW(TAG, "last reset was a brownout: waiting 2 s, TX power %d dBm",
                     CONFIG_DEVLINK_TX_POWER_BROWNOUT_DBM);
            vTaskDelay(pdMS_TO_TICKS(2000));
            s_tx_dbm = CONFIG_DEVLINK_TX_POWER_BROWNOUT_DBM;
        }
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (recovery) {
        (void)esp_wifi_set_max_tx_power((int8_t)(s_tx_dbm * 4));
    }
    s_started = true;
    ESP_LOGI(TAG, "AP '%s' up, credentials %s", (char *)ap.ap.ssid, (s_cur_ssid[0] != '\0') ? "stored" : "none");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    run(dl_wifi_policy_init(&s_pol, recovery ? DL_WIFI_MODE_RECOVERY : DL_WIFI_MODE_APP, s_cur_ssid[0] != '\0',
                            cfg.ap_window_s, cfg.button_window_s, now_ms()));
    xSemaphoreGive(s_lock);
    s_heartbeat_us = esp_timer_get_time();
    xTaskCreate(wifi_task, "dl_wifi", 4096, NULL, 5, NULL);
    return ESP_OK;
}

void dl_wifi_get_status(dl_wifi_status_t *st)
{
    memset(st, 0, sizeof(*st));
    if (!s_started) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st->started = true;
    st->sta_connected = s_pol.connected;
    st->have_creds = s_pol.have_creds;
    st->ap_on = s_ap_on;
    st->ap_clients = s_pol.ap_clients;
    st->pending = s_pol.pending;
    st->last_error = s_last_error;
    st->stack_restarts = s_restarts;
    st->tx_power_dbm = s_tx_dbm;
    memcpy(st->ssid, s_cur_ssid, sizeof(st->ssid));
    xSemaphoreGive(s_lock);

    esp_netif_ip_info_t ip;
    if (st->sta_connected && (esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK)) {
        st->ip = ip.ip;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            st->rssi = ap.rssi;
            st->channel = ap.primary;
        }
    }
}

#define LAN_SET_DELAY_MS 1000u
static esp_timer_handle_t s_set_timer;

static void deferred_set(void *arg)
{
    (void)arg;
    post(EV_SET, 0);
}

esp_err_t dl_wifi_set(dl_wifi_src_t src, const char *ssid, const char *password)
{
    size_t sl = strlen(ssid);
    size_t pl = strlen(password);
    if ((sl == 0u) || (sl > 32u) || (pl > 64u) || ((pl > 0u) && (pl < 8u))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_new_ssid, 0, sizeof(s_new_ssid));
    memset(s_new_pass, 0, sizeof(s_new_pass));
    memcpy(s_new_ssid, ssid, sl);
    memcpy(s_new_pass, password, pl);
    s_new_src = src;
    xSemaphoreGive(s_lock);
    if (src == DL_WIFI_SRC_LAN) {
        /* Over the LAN the STA link is about to be dropped for the new settings: let the HTTP reply ("pending")
         * leave the board first, otherwise the client never sees it. */
        if (s_set_timer == NULL) {
            const esp_timer_create_args_t a = {.callback = deferred_set, .name = "dl_wifi_set"};
            (void)esp_timer_create(&a, &s_set_timer);
        }
        (void)esp_timer_stop(s_set_timer);
        (void)esp_timer_start_once(s_set_timer, LAN_SET_DELAY_MS * 1000u);
    } else {
        post(EV_SET, 0);
    }
    return ESP_OK;
}

bool dl_wifi_wait_connected(uint32_t timeout_ms)
{
    if (!s_started) {
        return false;
    }
    xEventGroupClearBits(s_bits, BIT_FAILED);
    EventBits_t b = xEventGroupWaitBits(s_bits, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(timeout_ms));
    return (b & BIT_CONNECTED) != 0u;
}

esp_err_t dl_wifi_scan(dl_wifi_ap_rec_t *out, size_t max, size_t *n)
{
    static wifi_ap_record_t recs[20];
    uint16_t count = sizeof(recs) / sizeof(recs[0]);
    *n = 0;
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    wifi_scan_config_t sc = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&count, recs);
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return err;
    }
    for (uint16_t i = 0; (i < count) && (*n < max); i++) {
        if (recs[i].ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (size_t j = 0; j < *n; j++) {
            dup = dup || (strcmp(out[j].ssid, (const char *)recs[i].ssid) == 0);
        }
        if (dup) {
            continue; /* records come sorted by RSSI: keep the strongest */
        }
        strncpy(out[*n].ssid, (const char *)recs[i].ssid, sizeof(out[*n].ssid) - 1u);
        out[*n].ssid[sizeof(out[*n].ssid) - 1u] = '\0';
        out[*n].rssi = recs[i].rssi;
        out[*n].channel = recs[i].primary;
        out[*n].secure = (recs[i].authmode != WIFI_AUTH_OPEN);
        (*n)++;
    }
    return ESP_OK;
}

void dl_wifi_button(void)
{
    post(EV_BUTTON, 0);
}

void dl_wifi_forget(void)
{
    post(EV_FORGET, 0);
}

uint32_t dl_wifi_heartbeat_age_ms(void)
{
    return s_started ? (uint32_t)((esp_timer_get_time() - s_heartbeat_us) / 1000) : 0u;
}

dl_wifi_src_t dl_wifi_src_for_ip(uint32_t peer_ip)
{
    return ((peer_ip & AP_IP_MASK) == AP_IP_NET) ? DL_WIFI_SRC_AP : DL_WIFI_SRC_LAN;
}
