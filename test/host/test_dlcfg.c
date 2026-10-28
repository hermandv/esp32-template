/*
 * Host unit tests for the dlcfg record format (SPEC §5.3), including power loss at every point of a write
 * (acceptance test 16) with NOR-flash semantics.
 */
#include <string.h>

#include "dl_rtc.h"
#include "dlcfg_core.h"
#include "unity_lite.h"

UT_MAIN_DEFS

#define SEC DLCFG_SECTOR_SIZE

typedef struct {
    uint8_t s[2][SEC];
} part_t;

static void part_erase(part_t *p)
{
    memset(p->s, 0xFF, sizeof(p->s));
}

static void program(uint8_t *dst, const uint8_t *src, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        dst[i] &= src[i];
    }
}

/* Save like the driver: select, erase the other sector, write body, then header. */
static void save(part_t *p, const dlcfg_t *cfg)
{
    dlcfg_t cur;
    uint32_t seq;
    int sel = dlcfg_select(p->s[0], p->s[1], SEC, &cur, &seq);
    int dst = dlcfg_write_sector(sel);
    uint8_t buf[SEC];
    size_t n = dlcfg_build(cfg, seq + 1u, buf, sizeof(buf));
    memset(p->s[dst], 0xFF, SEC);
    program(p->s[dst] + sizeof(dlcfg_header_t), buf + sizeof(dlcfg_header_t), n - sizeof(dlcfg_header_t));
    program(p->s[dst], buf, sizeof(dlcfg_header_t));
}

static dlcfg_t cfg_with(const char *ssid, const char *label)
{
    dlcfg_t c;
    dlcfg_defaults(&c);
    strncpy(c.wifi_ssid, ssid, sizeof(c.wifi_ssid) - 1);
    strncpy(c.label, label, sizeof(c.label) - 1);
    return c;
}

static void dlcfg_blank_gives_defaults(void)
{
    part_t p;
    part_erase(&p);
    dlcfg_t c;
    uint32_t seq = 99;
    UT_EQ(dlcfg_select(p.s[0], p.s[1], SEC, &c, &seq), -1);
    UT_EQ(seq, 0);
    UT_EQ(c.wifi_ssid[0], 0);
    UT_EQ(c.log_level, 3);
}

static void dlcfg_roundtrip_and_alternate(void)
{
    part_t p;
    part_erase(&p);
    dlcfg_t a = cfg_with("bench", "board A");
    save(&p, &a);
    dlcfg_t c;
    uint32_t seq;
    UT_EQ(dlcfg_select(p.s[0], p.s[1], SEC, &c, &seq), 0);
    UT_EQ(seq, 1);
    UT_CHECK(strcmp(c.wifi_ssid, "bench") == 0);
    UT_CHECK(strcmp(c.label, "board A") == 0);

    dlcfg_t b = cfg_with("bench2", "board B");
    save(&p, &b);
    UT_EQ(dlcfg_select(p.s[0], p.s[1], SEC, &c, &seq), 1);
    UT_EQ(seq, 2);
    UT_CHECK(strcmp(c.label, "board B") == 0);

    save(&p, &a);
    UT_EQ(dlcfg_select(p.s[0], p.s[1], SEC, &c, &seq), 0);
    UT_EQ(seq, 3);
}

static void dlcfg_sequence_wraps(void)
{
    uint8_t s0[SEC], s1[SEC];
    memset(s0, 0xFF, SEC);
    memset(s1, 0xFF, SEC);
    dlcfg_t a = cfg_with("old", "old");
    dlcfg_t b = cfg_with("new", "new");
    dlcfg_build(&a, 0xFFFFFFFFu, s0, SEC);
    dlcfg_build(&b, 0u, s1, SEC); /* wrapped: newer */
    dlcfg_t c;
    uint32_t seq;
    UT_EQ(dlcfg_select(s0, s1, SEC, &c, &seq), 1);
    UT_CHECK(strcmp(c.label, "new") == 0);
}

static void dlcfg_corrupt_byte_falls_back(void)
{
    part_t p;
    part_erase(&p);
    dlcfg_t a = cfg_with("a", "A");
    dlcfg_t b = cfg_with("b", "B");
    save(&p, &a);
    save(&p, &b);
    p.s[1][sizeof(dlcfg_header_t) + 40] ^= 0x01;
    dlcfg_t c;
    uint32_t seq;
    UT_EQ(dlcfg_select(p.s[0], p.s[1], SEC, &c, &seq), 0);
    UT_CHECK(strcmp(c.label, "A") == 0);
}

static void dlcfg_t16_power_loss_at_every_point(void)
{
    /* Write is cut after k bytes (body first, header last), with every partial-bit pattern of byte k. */
    dlcfg_t a = cfg_with("good", "previous");
    dlcfg_t b = cfg_with("next", "next one");
    uint8_t rec[SEC];
    size_t n = dlcfg_build(&b, 2u, rec, sizeof(rec));
    const size_t hdr = sizeof(dlcfg_header_t);
    int bad = 0;
    long tries = 0;

    /* Order bytes are programmed in: body (hdr..n-1), then header (0..hdr-1). */
    size_t order[SEC];
    size_t k = 0;
    for (size_t i = hdr; i < n; i++) {
        order[k++] = i;
    }
    for (size_t i = 0; i < hdr; i++) {
        order[k++] = i;
    }

    for (size_t cut = 0; cut < n; cut++) {
        for (unsigned partial = 0; partial < 8; partial++) { /* sample bit patterns of the torn byte */
            part_t p;
            part_erase(&p);
            save(&p, &a); /* sector 0, seq 1 */
            memset(p.s[1], 0xFF, SEC); /* erase done */
            for (size_t i = 0; i < cut; i++) {
                program(&p.s[1][order[i]], &rec[order[i]], 1);
            }
            uint8_t torn = (uint8_t)(rec[order[cut]] | (uint8_t)(0xFFu << partial));
            program(&p.s[1][order[cut]], &torn, 1);

            dlcfg_t c;
            uint32_t seq;
            int sel = dlcfg_select(p.s[0], p.s[1], SEC, &c, &seq);
            bool ok_old = (sel == 0) && (strcmp(c.label, "previous") == 0);
            bool ok_new = (sel == 1) && (strcmp(c.label, "next one") == 0); /* only if fully written */
            if (!(ok_old || ok_new)) {
                bad++;
            }
            tries++;
        }
    }
    UT_EQ(bad, 0);
    UT_CHECK(tries > 2000);
}

static void dlcfg_older_shorter_payload_keeps_defaults(void)
{
    /* Simulate a record written by an older format that stopped before button_window_s. */
    uint8_t s[SEC];
    memset(s, 0xFF, SEC);
    dlcfg_t a = cfg_with("x", "short");
    dlcfg_build(&a, 1u, s, SEC);
    dlcfg_header_t h;
    memcpy(&h, s, sizeof(h));
    h.payload_len = (uint16_t)offsetof(dlcfg_t, button_window_s);
    uint32_t st = dl_crc32_update(DL_CRC32_INIT, &h, (uint32_t)offsetof(dlcfg_header_t, crc));
    st = dl_crc32_update(st, s + sizeof(h), h.payload_len);
    h.crc = ~st;
    memcpy(s, &h, sizeof(h));
    memset(s + sizeof(h) + h.payload_len, 0xEE, 8); /* garbage beyond the payload must be ignored */

    dlcfg_t c;
    uint32_t seq;
    UT_CHECK(dlcfg_parse(s, SEC, &c, &seq));
    UT_CHECK(strcmp(c.label, "short") == 0);
    UT_EQ(c.button_window_s, 120); /* default */
}

static void dlcfg_unterminated_strings_are_cut(void)
{
    uint8_t s[SEC];
    dlcfg_t a;
    dlcfg_defaults(&a);
    memset(a.label, 'L', sizeof(a.label));
    memset(a.wifi_ssid, 'S', sizeof(a.wifi_ssid));
    dlcfg_build(&a, 1u, s, SEC);
    dlcfg_t c;
    uint32_t seq;
    UT_CHECK(dlcfg_parse(s, SEC, &c, &seq));
    UT_EQ(strlen(c.label), 32);
    UT_EQ(strlen(c.wifi_ssid), 32);
}

static void dlcfg_reset_ring(void)
{
    dlcfg_t c;
    dlcfg_defaults(&c);
    for (uint32_t i = 1; i <= 10; i++) {
        dlcfg_push_reset_reason(&c, i);
    }
    UT_EQ(c.reset_ring_pos, 2);
    UT_EQ(c.reset_ring[1], 10);
    UT_EQ(c.reset_ring[2], 3);
}

int main(void)
{
    UT_RUN(dlcfg_blank_gives_defaults);
    UT_RUN(dlcfg_roundtrip_and_alternate);
    UT_RUN(dlcfg_sequence_wraps);
    UT_RUN(dlcfg_corrupt_byte_falls_back);
    UT_RUN(dlcfg_t16_power_loss_at_every_point);
    UT_RUN(dlcfg_older_shorter_payload_keeps_defaults);
    UT_RUN(dlcfg_unterminated_strings_are_cut);
    UT_RUN(dlcfg_reset_ring);
    return UT_SUMMARY();
}
