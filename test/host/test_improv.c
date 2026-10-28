/*
 * Host unit tests for the Improv serial protocol (SPEC §5.4).
 */
#include <string.h>

#include "dl_improv.h"
#include "unity_lite.h"

UT_MAIN_DEFS

/* Feed bytes; collect passthrough text and the last frame. */
typedef struct {
    char text[256];
    size_t text_len;
    int frames;
    int bad;
    uint8_t type;
    uint8_t data[256];
    size_t data_len;
} sink_t;

static void feed(dl_improv_rx_state_t *s, sink_t *k, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        dl_improv_rx_t r = dl_improv_rx_byte(s, b[i]);
        if (r == DL_IMPROV_RX_PASS) {
            memcpy(k->text + k->text_len, s->pass, s->pass_len);
            k->text_len += s->pass_len;
            k->text[k->text_len] = '\0';
        } else if (r == DL_IMPROV_RX_FRAME) {
            k->frames++;
            k->type = s->type;
            memcpy(k->data, s->data, s->data_len);
            k->data_len = s->data_len;
        } else if (r == DL_IMPROV_RX_BAD) {
            k->bad++;
        }
    }
}

static void improv_known_frame(void)
{
    /* "Request current state" RPC from the Improv spec: IMPROV 01 03 02 02 00 + checksum. */
    uint8_t f[DL_IMPROV_MAX_FRAME];
    const uint8_t rpc[] = {DL_IMPROV_CMD_STATE, 0x00};
    size_t n = dl_improv_frame(DL_IMPROV_T_RPC, rpc, sizeof(rpc), f);
    const uint8_t want[] = {'I', 'M', 'P', 'R', 'O', 'V', 0x01, 0x03, 0x02, 0x02, 0x00, 0x00};
    UT_EQ(n, sizeof(want));
    uint8_t c = 0;
    for (size_t i = 0; i < n - 1; i++) {
        c = (uint8_t)(c + want[i]);
    }
    UT_CHECK(memcmp(f, want, n - 1) == 0);
    UT_EQ(f[n - 1], c);
}

static void improv_text_and_frames_mixed(void)
{
    dl_improv_rx_state_t s;
    sink_t k = {0};
    dl_improv_rx_init(&s);
    uint8_t f[DL_IMPROV_MAX_FRAME];
    const uint8_t rpc[] = {DL_IMPROV_CMD_INFO, 0x00};
    size_t n = dl_improv_frame(DL_IMPROV_T_RPC, rpc, sizeof(rpc), f);

    feed(&s, &k, (const uint8_t *)"devlink label IMP lab\n", 22);
    feed(&s, &k, f, n);
    feed(&s, &k, (const uint8_t *)"IIMPRx info\n", 12);
    UT_CHECK(strcmp(k.text, "devlink label IMP lab\nIIMPRx info\n") == 0);
    UT_EQ(k.frames, 1);
    UT_EQ(k.type, DL_IMPROV_T_RPC);
    UT_EQ(k.data_len, 2);
    UT_EQ(k.data[0], DL_IMPROV_CMD_INFO);
}

static void improv_bad_checksum_and_version(void)
{
    dl_improv_rx_state_t s;
    sink_t k = {0};
    dl_improv_rx_init(&s);
    uint8_t f[DL_IMPROV_MAX_FRAME];
    const uint8_t rpc[] = {DL_IMPROV_CMD_STATE, 0x00};
    size_t n = dl_improv_frame(DL_IMPROV_T_RPC, rpc, sizeof(rpc), f);
    f[n - 1] ^= 0x01;
    feed(&s, &k, f, n);
    UT_EQ(k.bad, 1);
    n = dl_improv_frame(DL_IMPROV_T_RPC, rpc, sizeof(rpc), f);
    f[6] = 2;
    f[n - 1] = (uint8_t)(f[n - 1] + 1);
    feed(&s, &k, f, n);
    UT_EQ(k.bad, 2);
    /* Receiver recovers for the next frame. */
    n = dl_improv_frame(DL_IMPROV_T_RPC, rpc, sizeof(rpc), f);
    feed(&s, &k, f, n);
    UT_EQ(k.frames, 1);
}

static void improv_wifi_settings(void)
{
    const uint8_t p[] = {5, 'b', 'e', 'n', 'c', 'h', 3, 'p', 'w', 'd'};
    uint8_t rpc[64];
    rpc[0] = DL_IMPROV_CMD_WIFI;
    rpc[1] = sizeof(p);
    memcpy(rpc + 2, p, sizeof(p));
    uint8_t cmd;
    const uint8_t *pl;
    size_t pll;
    UT_CHECK(dl_improv_parse_rpc(rpc, 2 + sizeof(p), &cmd, &pl, &pll));
    UT_EQ(cmd, DL_IMPROV_CMD_WIFI);
    char ssid[33], pw[65];
    UT_CHECK(dl_improv_parse_wifi(pl, pll, ssid, pw));
    UT_CHECK(strcmp(ssid, "bench") == 0);
    UT_CHECK(strcmp(pw, "pwd") == 0);

    /* Open network: empty password. */
    const uint8_t open[] = {1, 'x', 0};
    UT_CHECK(dl_improv_parse_wifi(open, sizeof(open), ssid, pw));
    UT_EQ(strlen(pw), 0);

    /* Malformed. */
    const uint8_t bad1[] = {0, 0};                   /* empty SSID */
    const uint8_t bad2[] = {5, 'a', 'b'};            /* truncated */
    const uint8_t bad3[] = {1, 'x', 2, 'p'};         /* password truncated */
    const uint8_t bad4[] = {1, 'x', 1, 'p', 'z'};    /* trailing byte */
    const uint8_t bad5[] = {2, 'x', 0, 0};           /* NUL in SSID */
    UT_CHECK(!dl_improv_parse_wifi(bad1, sizeof(bad1), ssid, pw));
    UT_CHECK(!dl_improv_parse_wifi(bad2, sizeof(bad2), ssid, pw));
    UT_CHECK(!dl_improv_parse_wifi(bad3, sizeof(bad3), ssid, pw));
    UT_CHECK(!dl_improv_parse_wifi(bad4, sizeof(bad4), ssid, pw));
    UT_CHECK(!dl_improv_parse_wifi(bad5, sizeof(bad5), ssid, pw));
    UT_CHECK(!dl_improv_parse_rpc(rpc, 3, &cmd, &pl, &pll)); /* length byte disagrees */
}

static void improv_rpc_result(void)
{
    const char *info[] = {"devlink_recovery", "0.2.0", "ESP32-S3", "bench S3 #1"};
    uint8_t f[DL_IMPROV_MAX_FRAME];
    size_t n = dl_improv_rpc_result(DL_IMPROV_CMD_INFO, info, 4, f);
    UT_CHECK(n > 0);

    dl_improv_rx_state_t s;
    sink_t k = {0};
    dl_improv_rx_init(&s);
    feed(&s, &k, f, n);
    UT_EQ(k.frames, 1);
    UT_EQ(k.type, DL_IMPROV_T_RPC_RESULT);
    UT_EQ(k.data[0], DL_IMPROV_CMD_INFO);
    UT_EQ(k.data[1], k.data_len - 2);
    UT_EQ(k.data[2], 16);
    UT_CHECK(memcmp(&k.data[3], "devlink_recovery", 16) == 0);

    /* Too long for one frame. */
    char big[200];
    memset(big, 'a', sizeof(big) - 1);
    big[199] = '\0';
    const char *two[] = {big, big};
    UT_EQ(dl_improv_rpc_result(DL_IMPROV_CMD_SCAN, two, 2, f), 0);
    /* Empty result (end of scan list). */
    UT_EQ(dl_improv_rpc_result(DL_IMPROV_CMD_SCAN, NULL, 0, f), 12);
}

int main(void)
{
    UT_RUN(improv_known_frame);
    UT_RUN(improv_text_and_frames_mixed);
    UT_RUN(improv_bad_checksum_and_version);
    UT_RUN(improv_wifi_settings);
    UT_RUN(improv_rpc_result);
    return UT_SUMMARY();
}
