/*
 * Host unit tests for the streaming core (SPEC §5.7) and JSON string arrays (WebSocket ops).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dl_json.h"
#include "dl_stream_core.h"
#include "unity_lite.h"

UT_MAIN_DEFS

static void stream_ring_drops_when_full(void)
{
    static dl_ring_t r;
    dl_ring_init(&r);
    for (int i = 0; i < 40; i++) {
        char f[16];
        int n = snprintf(f, sizeof(f), "{\"i\":%d}", i);
        dl_ring_push(&r, f, (size_t)n);
    }
    UT_EQ(r.count, DL_RING_SLOTS);
    UT_EQ(r.drops, 40 - DL_RING_SLOTS);
    char out[2048];
    size_t n = dl_ring_pop_batch(&r, out, sizeof(out));
    out[n] = '\0';
    UT_CHECK(strncmp(out, "{\"i\":0}\n{\"i\":1}\n", 16) == 0); /* oldest kept, newest dropped */
    UT_EQ(r.count, 0);
}

static void stream_ring_batch_respects_size(void)
{
    static dl_ring_t r;
    dl_ring_init(&r);
    for (int i = 0; i < 10; i++) {
        dl_ring_push(&r, "0123456789", 10);
    }
    char out[40];
    size_t n = dl_ring_pop_batch(&r, out, sizeof(out)); /* 10+1+10+1+10 = 32; a 4th would need 43 */
    UT_EQ(n, 32);
    UT_EQ(r.count, 7);
    char big[DL_FRAME_MAX + 10];
    memset(big, 'x', sizeof(big));
    UT_CHECK(!dl_ring_push(&r, big, sizeof(big)));
    UT_EQ(r.drops, 1);
}

static void stream_rate_limits(void)
{
    uint64_t next = 0;
    int sent = 0;
    for (uint64_t t = 0; t < 2000000; t += 10000) { /* 100 Hz source for 2 s */
        sent += dl_rate_due(&next, t, 10) ? 1 : 0;
    }
    UT_CHECK((sent == 20) || (sent == 21)); /* steady 10 Hz; the tolerance shifts the phase by one source tick */
    next = 0;
    sent = 0;
    for (uint64_t t = 0; t < 1000000; t += 10000) {
        sent += dl_rate_due(&next, t, 0) ? 1 : 0;
    }
    UT_EQ(sent, 100); /* hz 0 = every sample */
    /* A producer at exactly the requested rate, with +-2 ms jitter: every sample gets through. */
    next = 0;
    sent = 0;
    for (int i = 0; i < 100; i++) {
        uint64_t t = (uint64_t)i * 100000u + ((i % 2) ? 2000u : 0u) - ((i % 3 == 0) && (i > 0) ? 1500u : 0u);
        sent += dl_rate_due(&next, t, 10) ? 1 : 0;
    }
    UT_EQ(sent, 100);
    /* After a long pause: one frame, not a burst. */
    next = 0;
    dl_rate_due(&next, 0, 10);
    UT_CHECK(dl_rate_due(&next, 5000000, 10));
    UT_CHECK(!dl_rate_due(&next, 5010000, 10));
}

static void stream_frames(void)
{
    char b[DL_FRAME_MAX];
    dl_jw_t w;
    dl_jw_init(&w, b, sizeof(b));
    dl_frame_num(&w, "sys.temp", 12345678, 41.5);
    UT_CHECK(strcmp(b, "{\"ch\":\"sys.temp\",\"ts\":12345678,\"v\":41.5}") == 0);
    UT_CHECK(dl_json_valid_object(b, strlen(b)));

    dl_jw_init(&w, b, sizeof(b));
    double v[3] = {1023, 1018, NAN};
    dl_frame_arr(&w, "adc.raw", 5, v, 3);
    UT_CHECK(strcmp(b, "{\"ch\":\"adc.raw\",\"ts\":5,\"v\":[1023,1018,null]}") == 0);

    dl_jw_init(&w, b, sizeof(b));
    dl_frame_raw(&w, "dbg.x", 7, "{\"a\":1}");
    UT_CHECK(strcmp(b, "{\"ch\":\"dbg.x\",\"ts\":7,\"v\":{\"a\":1}}") == 0);

    dl_jw_init(&w, b, sizeof(b));
    dl_frame_log(&w, 9, 'W', "wifi", 4, "bad \"x\"", 7);
    UT_CHECK(strcmp(b, "{\"ch\":\"log\",\"ts\":9,\"lvl\":\"W\",\"tag\":\"wifi\",\"msg\":\"bad \\\"x\\\"\"}") == 0);
    UT_CHECK(dl_json_valid_object(b, strlen(b)));
}

static void stream_log_parse(void)
{
    dl_log_line_t l;
    const char *a = "\033[0;33mW (12345) dl_wifi: STA down (reason 201)\033[0m\n";
    dl_log_parse(a, strlen(a), &l);
    UT_EQ(l.lvl, 'W');
    UT_CHECK((l.tag_len == 7) && (strncmp(l.tag, "dl_wifi", 7) == 0));
    UT_CHECK((l.msg_len == 21) && (strncmp(l.msg, "STA down (reason 201)", 21) == 0));

    const char *b = "I (5) boot: ok\n"; /* no colours */
    dl_log_parse(b, strlen(b), &l);
    UT_EQ(l.lvl, 'I');
    UT_CHECK((l.tag_len == 4) && (strncmp(l.msg, "ok", l.msg_len) == 0) && (l.msg_len == 2));

    const char *c = "plain printf text\n";
    dl_log_parse(c, strlen(c), &l);
    UT_EQ(l.lvl, 'I');
    UT_EQ(l.tag_len, 0);
    UT_EQ(l.msg_len, 17);

    const char *d = "E (1) x"; /* no ": " */
    dl_log_parse(d, strlen(d), &l);
    UT_EQ(l.tag_len, 0);
    UT_EQ(l.msg_len, 7);

    const char *e = "I (12) tag: a: b\n"; /* first ": " ends the tag */
    dl_log_parse(e, strlen(e), &l);
    UT_CHECK((l.tag_len == 3) && (strncmp(l.msg, "a: b", 4) == 0));
}

static void json_string_arrays(void)
{
    const char *j = "{\"op\":\"sub\",\"ch\":[\"sys.temp\", \"log\" ,\"x\"]}";
    char s[16];
    UT_EQ(dl_json_get_str_at(j, strlen(j), "ch", 0, s, sizeof(s)), DL_JSON_OK);
    UT_CHECK(strcmp(s, "sys.temp") == 0);
    UT_EQ(dl_json_get_str_at(j, strlen(j), "ch", 2, s, sizeof(s)), DL_JSON_OK);
    UT_CHECK(strcmp(s, "x") == 0);
    UT_EQ(dl_json_get_str_at(j, strlen(j), "ch", 3, s, sizeof(s)), DL_JSON_MISSING);
    const char *k = "{\"op\":\"rate\",\"ch\":\"sys.temp\",\"hz\":2}";
    UT_EQ(dl_json_get_str_at(k, strlen(k), "ch", 0, s, sizeof(s)), DL_JSON_OK);
    UT_EQ(dl_json_get_str_at(k, strlen(k), "ch", 1, s, sizeof(s)), DL_JSON_MISSING);
    const char *m = "{\"ch\":[1,\"a\"]}";
    UT_EQ(dl_json_get_str_at(m, strlen(m), "ch", 0, s, sizeof(s)), DL_JSON_TYPE);
    UT_EQ(dl_json_get_str_at(m, strlen(m), "ch", 1, s, sizeof(s)), DL_JSON_OK);
    UT_EQ(dl_json_get_str_at("{\"ch\":[]}", 9, "ch", 0, s, sizeof(s)), DL_JSON_MISSING);
}

/* Block header layout and round trip (SPEC §5.7 block channels). */
static void stream_block_header_round_trip(void)
{
    const float scale[3] = {0.5f, 1.0f, 2.0f};
    const int16_t data[2][3] = {{1, -2, 3}, {4, 5, -6}};
    dl_block_hdr_t h = {.sample = DL_SAMPLE_I16, .ch_id = 7, .n_fields = 3, .flags = DL_BLOCK_FLAG_GAP, .seq = 42,
                        .t0_us = 0x123456789ULL, .period_ns = 2000000u, .n_samples = 2};
    uint8_t msg[DL_BLOCK_MAX];
    size_t hlen = dl_block_header(msg, &h, scale);
    UT_EQ(hlen, DL_BLOCK_FIXED_HDR + 12u);
    UT_EQ(msg[0], 0x0C); /* magic 0xB10C, little-endian */
    UT_EQ(msg[1], 0xB1);
    UT_EQ(msg[26], hlen);
    memcpy(msg + hlen, data, sizeof(data));
    size_t n = hlen + sizeof(data);

    dl_block_hdr_t p;
    const uint8_t *sc;
    const uint8_t *d;
    UT_CHECK(dl_block_parse(msg, n, &p, &sc, &d));
    UT_EQ(p.ch_id, 7);
    UT_EQ(p.seq, 42u);
    UT_CHECK(p.t0_us == 0x123456789ULL);
    UT_EQ(p.period_ns, 2000000u);
    UT_EQ(p.n_samples, 2);
    UT_EQ(p.n_fields, 3);
    UT_EQ(p.flags, DL_BLOCK_FLAG_GAP);
    float s2;
    memcpy(&s2, sc + 8, sizeof(s2));
    UT_CHECK(s2 == 2.0f);
    UT_CHECK(memcmp(d, data, sizeof(data)) == 0);
    UT_CHECK(!dl_block_parse(msg, n - 1u, &p, &sc, &d)); /* the length must match exactly */
    msg[2] = 2;
    UT_CHECK(!dl_block_parse(msg, n, &p, &sc, &d));      /* unknown version */
}

static void stream_block_ring_drops(void)
{
    uint8_t hdr[DL_BLOCK_FIXED_HDR] = {0};
    uint8_t data[100] = {0};
    static uint8_t mem[4u * (DL_BLOCK_MAX + 2u)];
    UT_EQ(dl_bring_mem_size(4), sizeof(mem));
    dl_bring_t r;

    dl_bring_init(&r, NULL, 4); /* not allocated: every push is a counted drop */
    UT_CHECK(!dl_bring_push(&r, hdr, sizeof(hdr), data, sizeof(data)));
    UT_EQ(r.drops, 1u);

    dl_bring_init(&r, mem, 4);
    for (uint8_t i = 0; i < 6; i++) {
        hdr[0] = i;
        dl_bring_push(&r, hdr, sizeof(hdr), data, sizeof(data));
    }
    UT_EQ(r.count, 4u);
    UT_EQ(r.drops, 2u);
    static uint8_t big[DL_BLOCK_MAX];
    UT_CHECK(!dl_bring_push(&r, hdr, sizeof(hdr), big, sizeof(big))); /* longer than DL_BLOCK_MAX */
    uint8_t out[DL_BLOCK_MAX];
    UT_EQ(dl_bring_pop(&r, out, 10), 0u);                              /* does not fit: stays queued */
    for (uint8_t i = 0; i < 4; i++) {
        UT_EQ(dl_bring_pop(&r, out, sizeof(out)), sizeof(hdr) + sizeof(data));
        UT_EQ(out[0], i);                                              /* oldest first, newest dropped */
    }
    UT_EQ(dl_bring_pop(&r, out, sizeof(out)), 0u);
    hdr[0] = 9;                                                        /* wraps around */
    UT_CHECK(dl_bring_push(&r, hdr, sizeof(hdr), data, 1));
    UT_EQ(dl_bring_pop(&r, out, sizeof(out)), sizeof(hdr) + 1u);
    UT_EQ(out[0], 9);
}

int main(void)
{
    UT_RUN(stream_block_header_round_trip);
    UT_RUN(stream_block_ring_drops);
    UT_RUN(stream_ring_drops_when_full);
    UT_RUN(stream_ring_batch_respects_size);
    UT_RUN(stream_rate_limits);
    UT_RUN(stream_frames);
    UT_RUN(stream_log_parse);
    UT_RUN(json_string_arrays);
    return UT_SUMMARY();
}
