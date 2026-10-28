/*
 * Minimal JSON helpers for the devlink API (SPEC §5.6). Pure C.
 *
 * Reading: request bodies are small FLAT objects ({"ssid":"x","password":"y","ap_window_s":20}). dl_json_get_*
 * look up one key; nested objects/arrays are skipped but not readable. Writing: a bounded string builder with
 * proper escaping. Everything is length-checked; nothing allocates.
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DL_JSON_OK = 0,
    DL_JSON_MISSING = 1,   /* key not present */
    DL_JSON_TYPE = 2,      /* present but wrong type */
    DL_JSON_TOO_LONG = 3,  /* string longer than the output buffer */
    DL_JSON_SYNTAX = 4,    /* body is not a valid flat JSON object */
} dl_json_err_t;

/** Check that @p json (length @p len) is a syntactically valid JSON object. */
bool dl_json_valid_object(const char *json, size_t len);

/**
 * @brief Get a string value (unescaped, UTF-8 kept as is, \uXXXX encoded as UTF-8) into @p out (NUL-terminated).
 *
 * @requirement SPEC-5.6
 * @parent      I6
 * @test        host:json_*
 */
dl_json_err_t dl_json_get_str(const char *json, size_t len, const char *key, char *out, size_t out_size);

/** Get an integer value (no fraction/exponent). */
dl_json_err_t dl_json_get_int(const char *json, size_t len, const char *key, long *out);

/** Get a boolean value. */
dl_json_err_t dl_json_get_bool(const char *json, size_t len, const char *key, bool *out);

/* ---- Writer ---- */

typedef struct {
    char *buf;
    size_t size;
    size_t len;
    bool overflow;   /* set when something did not fit; buf stays NUL-terminated */
} dl_jw_t;

void dl_jw_init(dl_jw_t *w, char *buf, size_t size);
/** Append raw text (caller guarantees it is valid JSON). */
void dl_jw_raw(dl_jw_t *w, const char *s);
/** Append printf-formatted raw text. */
void dl_jw_rawf(dl_jw_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/** Append a quoted, escaped string. NULL -> null. */
void dl_jw_str(dl_jw_t *w, const char *s);
/** Append "key": with a leading comma unless it is the first member (tracks by looking at the last char). */
void dl_jw_key(dl_jw_t *w, const char *key);
/** Convenience: "key":"value" / "key":number / "key":bool. */
void dl_jw_kv_str(dl_jw_t *w, const char *key, const char *value);
void dl_jw_kv_int(dl_jw_t *w, const char *key, long long value);
void dl_jw_kv_uint(dl_jw_t *w, const char *key, unsigned long long value);
void dl_jw_kv_bool(dl_jw_t *w, const char *key, bool value);

#ifdef __cplusplus
}
#endif
