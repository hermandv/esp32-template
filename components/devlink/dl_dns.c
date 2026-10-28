/*
 * Captive-portal DNS for config-AP clients (SPEC §5.4): every A query from 192.168.4.0/24 is answered with
 * 192.168.4.1, so a browser on the AP lands on the setup page. Queries from other networks are ignored.
 */
#include <string.h>

#include "dl_priv.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "dl_dns";

#define DNS_PORT  53
#define MAX_PKT   512

static void dns_task(void *arg)
{
    (void)arg;
    static uint8_t pkt[MAX_PKT];
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(DNS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if ((s < 0) || (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0)) {
        ESP_LOGE(TAG, "bind failed");
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, pkt, MAX_PKT - 16, 0, (struct sockaddr *)&from, &fl);
        if ((n < 12) || (dl_wifi_src_for_ip(from.sin_addr.s_addr) != DL_WIFI_SRC_AP)) {
            continue;
        }
        uint16_t qd = (uint16_t)((pkt[4] << 8) | pkt[5]);
        if (((pkt[2] & 0x80u) != 0u) || (qd != 1u)) {
            continue; /* not a query, or not exactly one question */
        }
        /* Walk the question name. */
        int p = 12;
        while ((p < n) && (pkt[p] != 0u)) {
            if ((pkt[p] & 0xC0u) != 0u) {
                p = n; /* compression in a question: give up */
                break;
            }
            p += pkt[p] + 1;
        }
        if (p + 5 > n) {
            continue;
        }
        uint16_t qtype = (uint16_t)((pkt[p + 1] << 8) | pkt[p + 2]);
        int len = p + 5; /* end of the question */
        pkt[2] = 0x81;   /* response, recursion desired */
        pkt[3] = 0x80;   /* recursion available, no error */
        pkt[6] = 0;
        pkt[7] = 0;      /* answers */
        pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0;
        if (qtype == 1u) { /* A: answer 192.168.4.1; others (AAAA ...) get no answer and fall back to IPv4 */
            static const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
            memcpy(pkt + len, ans, sizeof(ans));
            len += (int)sizeof(ans);
            pkt[7] = 1;
        }
        (void)sendto(s, pkt, (size_t)len, 0, (struct sockaddr *)&from, fl);
    }
}

esp_err_t dl_dns_start(void)
{
    return (xTaskCreate(dns_task, "dl_dns", 3072, NULL, 3, NULL) == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
