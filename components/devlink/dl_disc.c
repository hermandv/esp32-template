/*
 * Discovery (SPEC §5.5): mDNS esp-<id>.local + _devlink._tcp, and the UDP broadcast fallback on port 47800.
 */
#include <string.h>

#include "dl_priv.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"

static const char *TAG = "dl_disc";

#define UDP_PORT 47800
#define QUERY    "DEVLINK?"

static bool s_mdns;

/* TXT: id, mac, label, board, mode, fw, rec, api, boot (values from the discovery record). */
static void set_txt(void)
{
    static char buf[384];
    dl_jw_t w;
    dl_jw_init(&w, buf, sizeof(buf));
    dl_disc_json(&w);

    static const char *keys[] = {"id", "mac", "label", "board", "mode", "fw", "rec", "api", "boot"};
    static char vals[9][34];
    mdns_txt_item_t txt[9];
    for (size_t i = 0; i < 9; i++) {
        vals[i][0] = '\0';
        if (dl_json_get_str(buf, w.len, keys[i], vals[i], sizeof(vals[i])) != DL_JSON_OK) {
            long v;
            if (dl_json_get_int(buf, w.len, keys[i], &v) == DL_JSON_OK) {
                snprintf(vals[i], sizeof(vals[i]), "%ld", v);
            }
        }
        txt[i].key = keys[i];
        txt[i].value = vals[i];
    }
    (void)mdns_service_txt_set("_devlink", "_tcp", txt, 9);
    const dl_ident_t *id = dl_ident();
    (void)mdns_instance_name_set((id->label[0] != '\0') ? id->label : id->hostname);
}

void dl_disc_update(void)
{
    if (s_mdns) {
        set_txt();
    }
}

static void udp_task(void *arg)
{
    (void)arg;
    static char rx[32];
    static char tx[384];
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(UDP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if ((s < 0) || (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0)) {
        ESP_LOGE(TAG, "UDP %d: bind failed", UDP_PORT);
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, rx, sizeof(rx) - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        rx[n] = '\0';
        if (strncmp(rx, QUERY, strlen(QUERY)) != 0) {
            continue;
        }
        dl_jw_t w;
        dl_jw_init(&w, tx, sizeof(tx));
        dl_disc_json(&w);
        (void)sendto(s, tx, w.len, 0, (struct sockaddr *)&from, fl);
    }
}

esp_err_t dl_disc_start(void)
{
    const dl_ident_t *id = dl_ident();
    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        (void)mdns_hostname_set(id->hostname);
        err = mdns_service_add(NULL, "_devlink", "_tcp", 80, NULL, 0);
    }
    if (err == ESP_OK) {
        s_mdns = true;
        set_txt();
        ESP_LOGI(TAG, "mDNS %s.local, _devlink._tcp", id->hostname);
    } else {
        ESP_LOGW(TAG, "mDNS: %s (UDP discovery still works)", esp_err_to_name(err));
    }
    return (xTaskCreate(udp_task, "dl_udp", 3584, NULL, 3, NULL) == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
