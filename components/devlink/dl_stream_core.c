/*
 * Streaming building blocks (SPEC §5.7). Pure C, see dl_stream_core.h.
 */
#include "dl_stream_core.h"

#include <math.h>
#include <string.h>

/* ---- Ring ---- */

void dl_ring_init(dl_ring_t *r)
{
    r->head = 0;
    r->count = 0;
    r->drops = 0;
}

bool dl_ring_push(dl_ring_t *r, const char *frame, size_t len)
{
    if ((r->count >= DL_RING_SLOTS) || (len >= DL_FRAME_MAX)) {
        r->drops++;
        return false;
    }
    uint32_t i = (r->head + r->count) % DL_RING_SLOTS;
    memcpy(r->slot[i], frame, len);
    r->len[i] = (uint16_t)len;
    r->count++;
    return true;
}

size_t dl_ring_pop_batch(dl_ring_t *r, char *out, size_t size)
{
    size_t n = 0;
    while (r->count > 0u) {
        uint32_t i = r->head;
        size_t need = r->len[i] + ((n > 0u) ? 1u : 0u);
        if (n + need > size) {
            break;
        }
        if (n > 0u) {
            out[n++] = '\n';
        }
        memcpy(out + n, r->slot[i], r->len[i]);
        n += r->len[i];
        r->head = (r->head + 1u) % DL_RING_SLOTS;
        r->count--;
    }
    return n;
}

/* ---- Rate ---- */

bool dl_rate_due(uint64_t *next_us, uint64_t now_us, uint32_t hz)
{
    if (hz == 0u) {
        return true;
    }
    uint64_t period = 1000000u / hz;
    /* Accept samples up to 1/8 period early: a producer running at exactly this rate has jitter, and rejecting a
     * sample that is 0.1 ms early would halve the rate for that slot. */
    if (now_us + period / 8u < *next_us) {
        return false;
    }
    /* Keep a steady cadence, but never queue up a burst after a pause. */
    *next_us = ((*next_us + period) > now_us) ? (*next_us + period) : (now_us + period);
    return true;
}

/* ---- Frames ---- */

static void num(dl_jw_t *w, double v)
{
    if (isnan(v) || isinf(v)) {
        dl_jw_raw(w, "null"); /* JSON has no NaN/Inf */
    } else {
        dl_jw_rawf(w, "%.7g", v);
    }
}

static void head(dl_jw_t *w, const char *ch, int64_t ts)
{
    dl_jw_raw(w, "{");
    dl_jw_kv_str(w, "ch", ch);
    dl_jw_kv_int(w, "ts", ts);
    dl_jw_key(w, "v");
}

void dl_frame_num(dl_jw_t *w, const char *ch, int64_t ts, double v)
{
    head(w, ch, ts);
    num(w, v);
    dl_jw_raw(w, "}");
}

void dl_frame_arr(dl_jw_t *w, const char *ch, int64_t ts, const double *v, size_t n)
{
    head(w, ch, ts);
    dl_jw_raw(w, "[");
    for (size_t i = 0; i < n; i++) {
        if (i > 0u) {
            dl_jw_raw(w, ",");
        }
        num(w, v[i]);
    }
    dl_jw_raw(w, "]}");
}

void dl_frame_raw(dl_jw_t *w, const char *ch, int64_t ts, const char *json)
{
    head(w, ch, ts);
    dl_jw_raw(w, json);
    dl_jw_raw(w, "}");
}

/* Escaped string from a (pointer, length) slice. */
static void str_n(dl_jw_t *w, const char *s, size_t n)
{
    char tmp[DL_FRAME_MAX];
    if (n >= sizeof(tmp)) {
        n = sizeof(tmp) - 1u;
    }
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    dl_jw_str(w, tmp);
}

void dl_frame_log(dl_jw_t *w, int64_t ts, char lvl, const char *tag, size_t tag_len, const char *msg,
                  size_t msg_len)
{
    char l[2] = {lvl, '\0'};
    dl_jw_raw(w, "{\"ch\":\"log\"");
    dl_jw_kv_int(w, "ts", ts);
    dl_jw_kv_str(w, "lvl", l);
    dl_jw_key(w, "tag");
    str_n(w, tag, tag_len);
    dl_jw_key(w, "msg");
    str_n(w, msg, msg_len);
    dl_jw_raw(w, "}");
}

/* ---- Log lines ---- */

/* Skip an ANSI colour sequence "\033[...m" at *p. */
static void skip_ansi(const char *s, size_t len, size_t *p)
{
    while ((*p + 1u < len) && (s[*p] == '\033') && (s[*p + 1u] == '[')) {
        size_t q = *p + 2u;
        while ((q < len) && (s[q] != 'm')) {
            q++;
        }
        *p = (q < len) ? q + 1u : len;
    }
}

void dl_log_parse(const char *line, size_t len, dl_log_line_t *out)
{
    size_t p = 0;
    size_t end = len;

    /* Trailing newline(s) and colour reset. */
    while ((end > 0u) && ((line[end - 1u] == '\n') || (line[end - 1u] == '\r'))) {
        end--;
    }
    if ((end >= 4u) && (memcmp(line + end - 4u, "\033[0m", 4u) == 0)) {
        end -= 4u;
    }
    skip_ansi(line, end, &p);

    out->lvl = 'I';
    out->tag = line + p;
    out->tag_len = 0;
    out->msg = line + p;
    out->msg_len = end - p;

    /* "L (timestamp) tag: msg" */
    if ((p + 4u > end) || (strchr("EWIDV", line[p]) == NULL) || (line[p + 1u] != ' ') || (line[p + 2u] != '(')) {
        return;
    }
    size_t q = p + 3u;
    while ((q < end) && (line[q] != ')')) {
        q++;
    }
    if ((q + 2u >= end) || (line[q + 1u] != ' ')) {
        return;
    }
    size_t tag_start = q + 2u;
    size_t t = tag_start;
    while ((t + 1u < end) && !((line[t] == ':') && (line[t + 1u] == ' '))) {
        t++;
    }
    if (t + 1u >= end) {
        return;
    }
    out->lvl = line[p];
    out->tag = line + tag_start;
    out->tag_len = t - tag_start;
    out->msg = line + t + 2u;
    out->msg_len = end - (t + 2u);
}
