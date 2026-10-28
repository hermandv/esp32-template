/*
 * Minimal JSON helpers (SPEC §5.6). Pure C, see dl_json.h.
 */
#include "dl_json.h"

#include <stdio.h>
#include <string.h>

#define MAX_DEPTH 8
#define MAX_KEY   48

typedef struct {
    const char *s;
    size_t len;
    size_t pos;
} cur_t;

static void ws(cur_t *c)
{
    while ((c->pos < c->len) &&
           ((c->s[c->pos] == ' ') || (c->s[c->pos] == '\t') || (c->s[c->pos] == '\n') || (c->s[c->pos] == '\r'))) {
        c->pos++;
    }
}

static bool peek(cur_t *c, char ch)
{
    ws(c);
    return (c->pos < c->len) && (c->s[c->pos] == ch);
}

static int hexval(char h)
{
    if ((h >= '0') && (h <= '9')) {
        return h - '0';
    }
    if ((h >= 'a') && (h <= 'f')) {
        return h - 'a' + 10;
    }
    if ((h >= 'A') && (h <= 'F')) {
        return h - 'A' + 10;
    }
    return -1;
}

/* Append one byte to an optional output; returns false if it does not fit. */
static bool put(char *out, size_t size, size_t *n, char ch)
{
    if (out == NULL) {
        return true;
    }
    if (*n + 1u >= size) {
        return false;
    }
    out[(*n)++] = ch;
    return true;
}

/* Parse a string at the cursor (which must be at '"'). Unescapes into out (may be NULL). */
static dl_json_err_t parse_string(cur_t *c, char *out, size_t size)
{
    size_t n = 0;
    bool fits = true;

    ws(c);
    if ((c->pos >= c->len) || (c->s[c->pos] != '"')) {
        return DL_JSON_SYNTAX;
    }
    c->pos++;
    while (c->pos < c->len) {
        unsigned char ch = (unsigned char)c->s[c->pos++];
        if (ch == '"') {
            if (out != NULL) {
                out[n] = '\0';
            }
            return fits ? DL_JSON_OK : DL_JSON_TOO_LONG;
        }
        if (ch < 0x20u) {
            return DL_JSON_SYNTAX;
        }
        if (ch != '\\') {
            fits = fits && put(out, size, &n, (char)ch);
            continue;
        }
        if (c->pos >= c->len) {
            return DL_JSON_SYNTAX;
        }
        char e = c->s[c->pos++];
        switch (e) {
        case '"':
        case '\\':
        case '/':
            fits = fits && put(out, size, &n, e);
            break;
        case 'b':
            fits = fits && put(out, size, &n, '\b');
            break;
        case 'f':
            fits = fits && put(out, size, &n, '\f');
            break;
        case 'n':
            fits = fits && put(out, size, &n, '\n');
            break;
        case 'r':
            fits = fits && put(out, size, &n, '\r');
            break;
        case 't':
            fits = fits && put(out, size, &n, '\t');
            break;
        case 'u': {
            if (c->pos + 4u > c->len) {
                return DL_JSON_SYNTAX;
            }
            unsigned cp = 0;
            for (int i = 0; i < 4; i++) {
                int h = hexval(c->s[c->pos++]);
                if (h < 0) {
                    return DL_JSON_SYNTAX;
                }
                cp = (cp << 4) | (unsigned)h;
            }
            /* BMP only (surrogate pairs are kept as two 3-byte sequences: enough for SSIDs/labels). */
            if (cp < 0x80u) {
                fits = fits && put(out, size, &n, (char)cp);
            } else if (cp < 0x800u) {
                fits = fits && put(out, size, &n, (char)(0xC0u | (cp >> 6)));
                fits = fits && put(out, size, &n, (char)(0x80u | (cp & 0x3Fu)));
            } else {
                fits = fits && put(out, size, &n, (char)(0xE0u | (cp >> 12)));
                fits = fits && put(out, size, &n, (char)(0x80u | ((cp >> 6) & 0x3Fu)));
                fits = fits && put(out, size, &n, (char)(0x80u | (cp & 0x3Fu)));
            }
            break;
        }
        default:
            return DL_JSON_SYNTAX;
        }
    }
    return DL_JSON_SYNTAX;
}

static bool parse_literal(cur_t *c, const char *lit)
{
    size_t n = strlen(lit);
    if ((c->pos + n <= c->len) && (memcmp(c->s + c->pos, lit, n) == 0)) {
        c->pos += n;
        return true;
    }
    return false;
}

/* Number: returns span; is_int set if no fraction/exponent. */
static bool parse_number(cur_t *c, size_t *start, size_t *end, bool *is_int)
{
    size_t p = c->pos;
    *is_int = true;
    if ((p < c->len) && (c->s[p] == '-')) {
        p++;
    }
    size_t digits = p;
    while ((p < c->len) && (c->s[p] >= '0') && (c->s[p] <= '9')) {
        p++;
    }
    if (p == digits) {
        return false;
    }
    if ((p < c->len) && (c->s[p] == '.')) {
        *is_int = false;
        p++;
        size_t f = p;
        while ((p < c->len) && (c->s[p] >= '0') && (c->s[p] <= '9')) {
            p++;
        }
        if (p == f) {
            return false;
        }
    }
    if ((p < c->len) && ((c->s[p] == 'e') || (c->s[p] == 'E'))) {
        *is_int = false;
        p++;
        if ((p < c->len) && ((c->s[p] == '+') || (c->s[p] == '-'))) {
            p++;
        }
        size_t x = p;
        while ((p < c->len) && (c->s[p] >= '0') && (c->s[p] <= '9')) {
            p++;
        }
        if (p == x) {
            return false;
        }
    }
    *start = c->pos;
    *end = p;
    c->pos = p;
    return true;
}

static bool skip_value(cur_t *c, int depth);

static bool skip_container(cur_t *c, int depth, char open, char close)
{
    if (depth >= MAX_DEPTH) {
        return false;
    }
    c->pos++; /* open */
    if (peek(c, close)) {
        c->pos++;
        return true;
    }
    for (;;) {
        if (open == '{') {
            if (parse_string(c, NULL, 0) != DL_JSON_OK) {
                return false;
            }
            if (!peek(c, ':')) {
                return false;
            }
            c->pos++;
        }
        if (!skip_value(c, depth + 1)) {
            return false;
        }
        if (peek(c, ',')) {
            c->pos++;
            continue;
        }
        if (peek(c, close)) {
            c->pos++;
            return true;
        }
        return false;
    }
}

static bool skip_value(cur_t *c, int depth)
{
    size_t a;
    size_t b;
    bool i;

    ws(c);
    if (c->pos >= c->len) {
        return false;
    }
    switch (c->s[c->pos]) {
    case '"':
        return parse_string(c, NULL, 0) == DL_JSON_OK;
    case '{':
        return skip_container(c, depth, '{', '}');
    case '[':
        return skip_container(c, depth, '[', ']');
    case 't':
        return parse_literal(c, "true");
    case 'f':
        return parse_literal(c, "false");
    case 'n':
        return parse_literal(c, "null");
    default:
        return parse_number(c, &a, &b, &i);
    }
}

bool dl_json_valid_object(const char *json, size_t len)
{
    cur_t c = {.s = json, .len = len, .pos = 0};
    if (!peek(&c, '{')) {
        return false;
    }
    if (!skip_value(&c, 0)) {
        return false;
    }
    ws(&c);
    return c.pos == c.len;
}

/* Position the cursor at the value of top-level @p key. */
static dl_json_err_t find(const char *json, size_t len, const char *key, cur_t *c)
{
    char k[MAX_KEY];

    *c = (cur_t){.s = json, .len = len, .pos = 0};
    if (!dl_json_valid_object(json, len)) {
        return DL_JSON_SYNTAX;
    }
    (void)peek(c, '{');
    c->pos++;
    if (peek(c, '}')) {
        return DL_JSON_MISSING;
    }
    for (;;) {
        dl_json_err_t e = parse_string(c, k, sizeof(k));
        bool match = (e == DL_JSON_OK) && (strcmp(k, key) == 0);
        (void)peek(c, ':');
        c->pos++;
        ws(c);
        if (match) {
            return DL_JSON_OK;
        }
        (void)skip_value(c, 1);
        if (peek(c, ',')) {
            c->pos++;
            continue;
        }
        return DL_JSON_MISSING;
    }
}

dl_json_err_t dl_json_get_str(const char *json, size_t len, const char *key, char *out, size_t out_size)
{
    cur_t c;
    dl_json_err_t e = find(json, len, key, &c);
    if (e != DL_JSON_OK) {
        return e;
    }
    if (!peek(&c, '"')) {
        return DL_JSON_TYPE;
    }
    return parse_string(&c, out, out_size);
}

dl_json_err_t dl_json_get_int(const char *json, size_t len, const char *key, long *out)
{
    cur_t c;
    size_t a;
    size_t b;
    bool is_int;
    char tmp[24];

    dl_json_err_t e = find(json, len, key, &c);
    if (e != DL_JSON_OK) {
        return e;
    }
    if (!parse_number(&c, &a, &b, &is_int) || !is_int || ((b - a) >= sizeof(tmp))) {
        return DL_JSON_TYPE;
    }
    memcpy(tmp, json + a, b - a);
    tmp[b - a] = '\0';
    long v = 0;
    bool neg = (tmp[0] == '-');
    for (const char *p = tmp + (neg ? 1 : 0); *p != '\0'; p++) {
        if (v > (2147483647L - (*p - '0')) / 10) {
            return DL_JSON_TYPE; /* out of range for 32-bit long */
        }
        v = v * 10 + (*p - '0');
    }
    *out = neg ? -v : v;
    return DL_JSON_OK;
}

dl_json_err_t dl_json_get_bool(const char *json, size_t len, const char *key, bool *out)
{
    cur_t c;
    dl_json_err_t e = find(json, len, key, &c);
    if (e != DL_JSON_OK) {
        return e;
    }
    if (parse_literal(&c, "true")) {
        *out = true;
        return DL_JSON_OK;
    }
    if (parse_literal(&c, "false")) {
        *out = false;
        return DL_JSON_OK;
    }
    return DL_JSON_TYPE;
}

/* ---- Writer ---- */

void dl_jw_init(dl_jw_t *w, char *buf, size_t size)
{
    w->buf = buf;
    w->size = size;
    w->len = 0;
    w->overflow = (size == 0u);
    if (size > 0u) {
        buf[0] = '\0';
    }
}

static void wput(dl_jw_t *w, char ch)
{
    if (w->len + 1u < w->size) {
        w->buf[w->len++] = ch;
        w->buf[w->len] = '\0';
    } else {
        w->overflow = true;
    }
}

void dl_jw_raw(dl_jw_t *w, const char *s)
{
    while (*s != '\0') {
        wput(w, *s++);
    }
}

void dl_jw_rawf(dl_jw_t *w, const char *fmt, ...)
{
    char tmp[128]; /* longer output sets overflow (never truncated silently) */
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if ((n < 0) || ((size_t)n >= sizeof(tmp))) {
        w->overflow = true;
        return;
    }
    dl_jw_raw(w, tmp);
}

void dl_jw_str(dl_jw_t *w, const char *s)
{
    if (s == NULL) {
        dl_jw_raw(w, "null");
        return;
    }
    wput(w, '"');
    for (; *s != '\0'; s++) {
        unsigned char ch = (unsigned char)*s;
        if ((ch == '"') || (ch == '\\')) {
            wput(w, '\\');
            wput(w, (char)ch);
        } else if (ch == '\n') {
            dl_jw_raw(w, "\\n");
        } else if (ch == '\r') {
            dl_jw_raw(w, "\\r");
        } else if (ch == '\t') {
            dl_jw_raw(w, "\\t");
        } else if (ch < 0x20u) {
            dl_jw_rawf(w, "\\u%04x", ch);
        } else {
            wput(w, (char)ch);
        }
    }
    wput(w, '"');
}

void dl_jw_key(dl_jw_t *w, const char *key)
{
    if ((w->len > 0u) && (w->buf[w->len - 1u] != '{') && (w->buf[w->len - 1u] != '[')) {
        wput(w, ',');
    }
    dl_jw_str(w, key);
    wput(w, ':');
}

void dl_jw_kv_str(dl_jw_t *w, const char *key, const char *value)
{
    dl_jw_key(w, key);
    dl_jw_str(w, value);
}

void dl_jw_kv_int(dl_jw_t *w, const char *key, long long value)
{
    dl_jw_key(w, key);
    dl_jw_rawf(w, "%lld", value);
}

void dl_jw_kv_uint(dl_jw_t *w, const char *key, unsigned long long value)
{
    dl_jw_key(w, key);
    dl_jw_rawf(w, "%llu", value);
}

void dl_jw_kv_bool(dl_jw_t *w, const char *key, bool value)
{
    dl_jw_key(w, key);
    dl_jw_raw(w, value ? "true" : "false");
}
