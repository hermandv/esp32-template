/*
 * Host unit tests for the devlink JSON helpers (SPEC §5.6, §10 row 29: malformed/long input).
 */
#include <stdlib.h>
#include <string.h>

#include "dl_json.h"
#include "unity_lite.h"

UT_MAIN_DEFS

#define J(s) (s), strlen(s)

static void json_get_basic(void)
{
    const char *b = "{ \"ssid\" : \"bench\", \"password\":\"p\\\"w\", \"n\": -42, \"on\": true, \"x\": null }";
    char s[33];
    long n;
    bool on;
    UT_EQ(dl_json_get_str(J(b), "ssid", s, sizeof(s)), DL_JSON_OK);
    UT_CHECK(strcmp(s, "bench") == 0);
    UT_EQ(dl_json_get_str(J(b), "password", s, sizeof(s)), DL_JSON_OK);
    UT_CHECK(strcmp(s, "p\"w") == 0);
    UT_EQ(dl_json_get_int(J(b), "n", &n), DL_JSON_OK);
    UT_EQ(n, -42);
    UT_EQ(dl_json_get_bool(J(b), "on", &on), DL_JSON_OK);
    UT_CHECK(on);
    UT_EQ(dl_json_get_str(J(b), "missing", s, sizeof(s)), DL_JSON_MISSING);
    UT_EQ(dl_json_get_int(J(b), "ssid", &n), DL_JSON_TYPE);
    UT_EQ(dl_json_get_str(J(b), "n", s, sizeof(s)), DL_JSON_TYPE);
}

static void json_skips_nested_values(void)
{
    const char *b = "{\"a\":{\"ssid\":\"inner\",\"b\":[1,2,{\"c\":3}]},\"ssid\":\"outer\"}";
    char s[16];
    UT_EQ(dl_json_get_str(J(b), "ssid", s, sizeof(s)), DL_JSON_OK);
    UT_CHECK(strcmp(s, "outer") == 0);
}

static void json_unicode_escape(void)
{
    const char *b = "{\"label\":\"caf\\u00e9 \\u20ac\"}";
    char s[16];
    UT_EQ(dl_json_get_str(J(b), "label", s, sizeof(s)), DL_JSON_OK);
    UT_CHECK(strcmp(s, "caf\xc3\xa9 \xe2\x82\xac") == 0);
}

static void json_too_long_string(void)
{
    const char *b = "{\"ssid\":\"0123456789abcdef0123456789abcdef0\"}"; /* 33 chars */
    char s[33];
    UT_EQ(dl_json_get_str(J(b), "ssid", s, sizeof(s)), DL_JSON_TOO_LONG);
    char ok[34];
    UT_EQ(dl_json_get_str(J(b), "ssid", ok, sizeof(ok)), DL_JSON_OK);
}

static void json_rejects_malformed(void)
{
    const char *bad[] = {"", "[]", "{", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{\"a\":01x}", "{\"a\":\"x}",
                         "{\"a\":tru}", "{\"a\":1} x", "{a:1}", "{\"a\":\"\x01\"}", "{\"a\":\"\\q\"}",
                         "{\"a\":\"\\u12\"}", "{\"a\":1.}", "{\"a\":-}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        UT_CHECK(!dl_json_valid_object(bad[i], strlen(bad[i])));
        char s[8];
        UT_EQ(dl_json_get_str(bad[i], strlen(bad[i]), "a", s, sizeof(s)), DL_JSON_SYNTAX);
    }
    UT_CHECK(dl_json_valid_object(J("{}")));
    UT_CHECK(dl_json_valid_object(J(" { \"a\" : [ ] , \"b\" : 1.5e3 } ")));
}

static void json_depth_limit(void)
{
    char b[64] = "{\"a\":";
    for (int i = 0; i < 12; i++) {
        strcat(b, "[");
    }
    for (int i = 0; i < 12; i++) {
        strcat(b, "]");
    }
    strcat(b, "}");
    UT_CHECK(!dl_json_valid_object(b, strlen(b)));
}

static void json_int_range(void)
{
    long n;
    UT_EQ(dl_json_get_int(J("{\"n\":2147483647}"), "n", &n), DL_JSON_OK);
    UT_EQ(dl_json_get_int(J("{\"n\":2147483648}"), "n", &n), DL_JSON_TYPE);
    UT_EQ(dl_json_get_int(J("{\"n\":1.5}"), "n", &n), DL_JSON_TYPE);
}

static void json_not_nul_terminated(void)
{
    /* Body buffers from HTTP are not NUL-terminated: only len bytes may be read. */
    char b[] = {'{', '"', 'a', '"', ':', '1', '}', 'X', 'X'};
    long n;
    UT_EQ(dl_json_get_int(b, 7, "a", &n), DL_JSON_OK);
    UT_EQ(n, 1);
    UT_EQ(dl_json_get_int(b, 6, "a", &n), DL_JSON_SYNTAX);
}

static void json_fuzz_no_crash(void)
{
    /* Random mutations of a valid body: must never crash or read out of bounds (run under -fsanitize where
     * available). */
    const char *base = "{\"ssid\":\"bench\",\"password\":\"secret\",\"ap_window_s\":20,\"x\":[1,{\"y\":\"z\"}]}";
    size_t n = strlen(base);
    char b[128];
    srand(1234);
    for (int i = 0; i < 20000; i++) {
        memcpy(b, base, n);
        int muts = 1 + rand() % 4;
        for (int m = 0; m < muts; m++) {
            b[rand() % (int)n] = (char)(rand() % 256);
        }
        size_t len = (size_t)(rand() % (int)(n + 1));
        char s[33];
        long v;
        bool t;
        (void)dl_json_get_str(b, len, "ssid", s, sizeof(s));
        (void)dl_json_get_int(b, len, "ap_window_s", &v);
        (void)dl_json_get_bool(b, len, "x", &t);
    }
    UT_CHECK(true);
}

static void jw_builds_escaped_object(void)
{
    char buf[128];
    dl_jw_t w;
    dl_jw_init(&w, buf, sizeof(buf));
    dl_jw_raw(&w, "{");
    dl_jw_kv_str(&w, "label", "a\"b\\c\nd\x01");
    dl_jw_kv_int(&w, "n", -5);
    dl_jw_kv_bool(&w, "ok", true);
    dl_jw_key(&w, "arr");
    dl_jw_raw(&w, "[");
    dl_jw_str(&w, "x");
    dl_jw_raw(&w, "]");
    dl_jw_raw(&w, "}");
    UT_CHECK(!w.overflow);
    UT_CHECK(strcmp(buf, "{\"label\":\"a\\\"b\\\\c\\nd\\u0001\",\"n\":-5,\"ok\":true,\"arr\":[\"x\"]}") == 0);
    UT_CHECK(dl_json_valid_object(buf, strlen(buf)));
    char back[16];
    UT_EQ(dl_json_get_str(buf, strlen(buf), "label", back, sizeof(back)), DL_JSON_OK);
    UT_CHECK(strcmp(back, "a\"b\\c\nd\x01") == 0);
}

static void jw_overflow_is_flagged(void)
{
    char buf[8];
    dl_jw_t w;
    dl_jw_init(&w, buf, sizeof(buf));
    dl_jw_str(&w, "0123456789");
    UT_CHECK(w.overflow);
    UT_EQ(strlen(buf), 7);
}

int main(void)
{
    UT_RUN(json_get_basic);
    UT_RUN(json_skips_nested_values);
    UT_RUN(json_unicode_escape);
    UT_RUN(json_too_long_string);
    UT_RUN(json_rejects_malformed);
    UT_RUN(json_depth_limit);
    UT_RUN(json_int_range);
    UT_RUN(json_not_nul_terminated);
    UT_RUN(json_fuzz_no_crash);
    UT_RUN(jw_builds_escaped_object);
    UT_RUN(jw_overflow_is_flagged);
    return UT_SUMMARY();
}
