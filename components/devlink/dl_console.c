/*
 * USB console (SPEC §5.4): Improv Wi-Fi serial frames and "devlink ..." text lines on the native USB-Serial/JTAG
 * port, next to the normal log output. Other lines go to an optional extra handler (test console in test builds).
 */
#include <stdio.h>
#include <string.h>

#include "devlink.h"
#include "dl_improv.h"
#include "dl_priv.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "dl_console";

#define IMPROV_CONNECT_TIMEOUT_MS 40000u /* scan + slow (WPA3, weak) association + DHCP, with a retry */

static devlink_console_extra_fn s_extra;
static SemaphoreHandle_t s_out_lock;
static dl_improv_rx_state_t s_rx;

/* ---- Output ---- */

static void send_frame(const uint8_t *f, size_t n)
{
    if (n == 0u) {
        return;
    }
    /* Binary frame: write it to the driver directly. Through stdout every 0x0A byte (a length, the checksum or data)
     * would become "\r\n" (newline conversion) and corrupt the frame. */
    xSemaphoreTake(s_out_lock, portMAX_DELAY);
    fflush(stdout); /* keep the order with text already printed */
    (void)usb_serial_jtag_write_bytes(f, n, pdMS_TO_TICKS(100));
    (void)usb_serial_jtag_write_bytes("\n", 1, pdMS_TO_TICKS(100));
    xSemaphoreGive(s_out_lock);
}

static void improv_state(uint8_t state)
{
    uint8_t f[DL_IMPROV_MAX_FRAME];
    send_frame(f, dl_improv_frame(DL_IMPROV_T_STATE, &state, 1, f));
}

static void improv_error(uint8_t err)
{
    uint8_t f[DL_IMPROV_MAX_FRAME];
    send_frame(f, dl_improv_frame(DL_IMPROV_T_ERROR, &err, 1, f));
}

static void improv_result(uint8_t cmd, const char *const *s, size_t n)
{
    uint8_t f[DL_IMPROV_MAX_FRAME];
    send_frame(f, dl_improv_rpc_result(cmd, s, n, f));
}

static bool device_url(char *url, size_t size)
{
    dl_wifi_status_t st;
    dl_wifi_get_status(&st);
    if (!st.sta_connected) {
        return false;
    }
    snprintf(url, size, "http://" IPSTR "/", IP2STR(&st.ip));
    return true;
}

/* ---- Improv RPCs ---- */

static void improv_rpc(const uint8_t *data, size_t len)
{
    uint8_t cmd;
    const uint8_t *pl;
    size_t pll;
    char url[40];

    if (!dl_improv_parse_rpc(data, len, &cmd, &pl, &pll)) {
        improv_error(DL_IMPROV_E_INVALID_RPC);
        return;
    }
    switch (cmd) {
    case DL_IMPROV_CMD_STATE:
        if (device_url(url, sizeof(url))) {
            improv_state(DL_IMPROV_S_PROVISIONED);
            const char *s[] = {url};
            improv_result(cmd, s, 1);
        } else {
            improv_state(DL_IMPROV_S_READY);
        }
        break;
    case DL_IMPROV_CMD_INFO: {
        const dl_ident_t *id = dl_ident();
        const char *s[] = {dl_fw_project(), dl_fw_version(), "ESP32-S3", (id->label[0] != '\0') ? id->label : id->hostname};
        improv_result(cmd, s, 4);
        break;
    }
    case DL_IMPROV_CMD_SCAN: {
        static dl_wifi_ap_rec_t recs[16];
        size_t n = 0;
        if (dl_wifi_scan(recs, 16, &n) != ESP_OK) {
            improv_error(DL_IMPROV_E_UNKNOWN);
            break;
        }
        for (size_t i = 0; i < n; i++) {
            char rssi[8];
            snprintf(rssi, sizeof(rssi), "%d", recs[i].rssi);
            const char *s[] = {recs[i].ssid, rssi, recs[i].secure ? "YES" : "NO"};
            improv_result(cmd, s, 3);
        }
        improv_result(cmd, NULL, 0);
        break;
    }
    case DL_IMPROV_CMD_WIFI: {
        char ssid[33];
        char pass[65];
        if (!dl_improv_parse_wifi(pl, pll, ssid, pass)) {
            improv_error(DL_IMPROV_E_INVALID_RPC);
            break;
        }
        improv_state(DL_IMPROV_S_PROVISIONING);
        /* A wired user is present: saved directly like the AP path (SPEC §5.4). */
        if ((dl_wifi_set(DL_WIFI_SRC_USB, ssid, pass) == ESP_OK) && dl_wifi_wait_connected(IMPROV_CONNECT_TIMEOUT_MS) &&
            device_url(url, sizeof(url))) {
            improv_state(DL_IMPROV_S_PROVISIONED);
            const char *s[] = {url};
            improv_result(cmd, s, 1);
        } else {
            improv_error(DL_IMPROV_E_CONNECT);
            improv_state(DL_IMPROV_S_READY);
        }
        break;
    }
    default:
        improv_error(DL_IMPROV_E_UNKNOWN_RPC);
        break;
    }
}

/* ---- Text lines ---- */

static void print_json(const char *prefix, void (*fill)(dl_jw_t *w))
{
    static char buf[3072];
    dl_jw_t w;
    dl_jw_init(&w, buf, sizeof(buf));
    fill(&w);
    xSemaphoreTake(s_out_lock, portMAX_DELAY);
    printf("%s %s\n", prefix, buf);
    fflush(stdout);
    xSemaphoreGive(s_out_lock);
}

static void handle_line(char *line)
{
    if (strncmp(line, "devlink ", 8) != 0) {
        if ((s_extra == NULL) || !s_extra(line)) {
            if (line[0] != '\0') {
                printf("devlink: unknown command (try 'devlink help')\n");
            }
        }
        return;
    }
    char *cmd = line + 8;
    if (strncmp(cmd, "label ", 6) == 0) {
        dlcfg_t cfg;
        (void)dlcfg_load(&cfg);
        strncpy(cfg.label, cmd + 6, sizeof(cfg.label) - 1u);
        cfg.label[sizeof(cfg.label) - 1u] = '\0';
        esp_err_t err = dlcfg_save(&cfg);
        if (err == ESP_OK) {
            dl_ident_set_label(cfg.label);
            dl_disc_update();
        }
        printf("devlink: label %s\n", (err == ESP_OK) ? "saved" : esp_err_to_name(err));
    } else if (strcmp(cmd, "recovery") == 0) {
        printf("devlink: rebooting into recovery\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));
        devlink_restart_to_recovery();
    } else if (strcmp(cmd, "restart") == 0) {
        printf("devlink: restarting\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));
        devlink_restart();
    } else if (strcmp(cmd, "info") == 0) {
        print_json("devlink info", dl_info_json);
    } else if (strcmp(cmd, "status") == 0) {
        print_json("devlink status", dl_status_json);
    } else {
        printf("devlink: commands: label <text> | recovery | restart | info | status (Wi-Fi: Improv serial)\n");
    }
    fflush(stdout);
}

static void console_task(void *arg)
{
    (void)arg;
    char line[128];
    size_t len = 0;
    uint8_t buf[64];

    for (;;) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(1000));
        for (int i = 0; i < n; i++) {
            dl_improv_rx_t r = dl_improv_rx_byte(&s_rx, buf[i]);
            if (r == DL_IMPROV_RX_FRAME) {
                if (s_rx.type == DL_IMPROV_T_RPC) {
                    improv_rpc(s_rx.data, s_rx.data_len);
                }
                continue;
            }
            if (r == DL_IMPROV_RX_BAD) {
                improv_error(DL_IMPROV_E_INVALID_RPC);
                continue;
            }
            if (r != DL_IMPROV_RX_PASS) {
                continue;
            }
            for (size_t k = 0; k < s_rx.pass_len; k++) {
                char c = (char)s_rx.pass[k];
                if ((c == '\n') || (c == '\r')) {
                    line[len] = '\0';
                    if (len > 0u) {
                        handle_line(line);
                    }
                    len = 0;
                } else if (len < sizeof(line) - 1u) {
                    line[len++] = c;
                }
            }
        }
    }
}

esp_err_t devlink_console_start(devlink_console_extra_fn extra)
{
    s_extra = extra;
    s_out_lock = xSemaphoreCreateMutex();
    dl_improv_rx_init(&s_rx);
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        esp_err_t err = usb_serial_jtag_driver_install(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "USB-Serial/JTAG driver: %s", esp_err_to_name(err));
            return err;
        }
        usb_serial_jtag_vfs_use_driver();
    }
    return (xTaskCreate(console_task, "dl_console", 6144, NULL, 4, NULL) == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
