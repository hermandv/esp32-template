/*
 * Streaming (SPEC §5.7): channel registry, WebSocket /ws (max 2 clients), per-client rings + rates, a sender task,
 * the "log" channel tapped from ESP_LOG*, and debug channels. Format and ring logic: dl_stream_core.c.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "devlink_stream.h"
#include "dl_priv.h"
#include "dl_stream_core.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define WS_MSG_MAX 1400u   /* frames per WebSocket message, newline-separated */
#define WS_RX_MAX  512u
#define LOCK_WAIT  pdMS_TO_TICKS(20)
#define STALL_US   10000000LL /* a client that cannot take data for 10 s is closed */

struct devlink_channel {
    char name[32];
    char unit[12];
    devlink_type_t type;
    uint16_t default_hz;
    uint16_t max_hz;
    uint8_t index;
};

typedef struct {
    bool used;
    int fd;
    uint32_t sub;                          /* bit per channel */
    uint16_t hz[DEVLINK_MAX_CHANNELS];
    uint64_t next[DEVLINK_MAX_CHANNELS];
    int64_t stalled_since;                 /* 0 = socket writable last time */
    dl_ring_t ring;
} client_t;

static devlink_channel_t s_ch[DEVLINK_MAX_CHANNELS];
static size_t s_nch;
static client_t s_cl[DEVLINK_MAX_WS_CLIENTS];
static SemaphoreHandle_t s_lock;           /* registry, clients, rings, the static buffers below */
static TaskHandle_t s_sender;
static httpd_handle_t s_server;
static devlink_channel_t *s_log;
static uint32_t s_busy_drops;              /* frames lost because the lock was busy (log tap) */
static vprintf_like_t s_orig_vprintf;
static char s_frame[DL_FRAME_MAX];
static char s_line[DL_FRAME_MAX];
static char s_msg[WS_MSG_MAX];
static float s_temp = -1000.0f;

/* ---- Registry ---- */

static void lazy_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex(); /* first call comes from app_main / devlink_start, before any task */
    }
}

devlink_channel_t *devlink_channel_find(const char *name)
{
    for (size_t i = 0; i < s_nch; i++) {
        if (strcmp(s_ch[i].name, name) == 0) {
            return &s_ch[i];
        }
    }
    return NULL;
}

devlink_channel_t *devlink_channel_register(const devlink_channel_cfg_t *cfg)
{
    lazy_init();
    devlink_channel_t *ch = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if ((s_nch < DEVLINK_MAX_CHANNELS) && (cfg->name != NULL) && (strlen(cfg->name) < sizeof(ch->name)) &&
        (devlink_channel_find(cfg->name) == NULL)) {
        ch = &s_ch[s_nch];
        strcpy(ch->name, cfg->name);
        strncpy(ch->unit, (cfg->unit != NULL) ? cfg->unit : "", sizeof(ch->unit) - 1u);
        ch->type = cfg->type;
        ch->default_hz = cfg->default_hz;
        ch->max_hz = cfg->max_hz;
        ch->index = (uint8_t)s_nch;
        s_nch++;
    }
    xSemaphoreGive(s_lock);
    return ch;
}

uint32_t devlink_channel_wanted_hz(const devlink_channel_t *ch)
{
    uint32_t best = 0;
    if (ch == NULL) {
        return 0;
    }
    for (size_t c = 0; c < DEVLINK_MAX_WS_CLIENTS; c++) {
        if (s_cl[c].used && ((s_cl[c].sub >> ch->index) & 1u)) {
            uint32_t hz = s_cl[c].hz[ch->index];
            if (hz == 0u) {
                hz = (ch->max_hz > 0u) ? ch->max_hz : 1000u;
            }
            best = (hz > best) ? hz : best;
        }
    }
    return best;
}

/* ---- Publishing ---- */

typedef void (*fill_fn)(dl_jw_t *w, const devlink_channel_t *ch, int64_t ts, const void *arg);

/* Push one frame to every client that wants it now. Caller holds s_lock. */
static void publish_locked(devlink_channel_t *ch, fill_fn fill, const void *arg)
{
    int64_t ts = esp_timer_get_time();
    size_t len = 0;
    bool any = false;
    for (size_t c = 0; c < DEVLINK_MAX_WS_CLIENTS; c++) {
        client_t *cl = &s_cl[c];
        if (!cl->used || !((cl->sub >> ch->index) & 1u) ||
            !dl_rate_due(&cl->next[ch->index], (uint64_t)ts, cl->hz[ch->index])) {
            continue;
        }
        if (len == 0u) {
            dl_jw_t w;
            dl_jw_init(&w, s_frame, sizeof(s_frame));
            fill(&w, ch, ts, arg);
            len = w.overflow ? sizeof(s_frame) : w.len; /* too long -> the ring counts a drop */
        }
        (void)dl_ring_push(&cl->ring, s_frame, len);
        any = true;
    }
    if (any && (s_sender != NULL)) {
        xTaskNotifyGive(s_sender);
    }
}

static bool listening(const devlink_channel_t *ch)
{
    return (ch != NULL) && (s_lock != NULL) && !dl_ota_busy() && (devlink_channel_wanted_hz(ch) > 0u);
}

static void fill_num(dl_jw_t *w, const devlink_channel_t *ch, int64_t ts, const void *arg)
{
    dl_frame_num(w, ch->name, ts, *(const double *)arg);
}

typedef struct {
    const double *v;
    size_t n;
} arr_t;

static void fill_arr(dl_jw_t *w, const devlink_channel_t *ch, int64_t ts, const void *arg)
{
    const arr_t *a = arg;
    dl_frame_arr(w, ch->name, ts, a->v, a->n);
}

static void fill_raw(dl_jw_t *w, const devlink_channel_t *ch, int64_t ts, const void *arg)
{
    dl_frame_raw(w, ch->name, ts, (const char *)arg);
}

void devlink_publish_num(devlink_channel_t *ch, double value)
{
    if (listening(ch) && (xSemaphoreTake(s_lock, LOCK_WAIT) == pdTRUE)) {
        publish_locked(ch, fill_num, &value);
        xSemaphoreGive(s_lock);
    }
}

void devlink_publish_arr(devlink_channel_t *ch, const double *values, size_t n)
{
    arr_t a = {.v = values, .n = n};
    if (listening(ch) && (xSemaphoreTake(s_lock, LOCK_WAIT) == pdTRUE)) {
        publish_locked(ch, fill_arr, &a);
        xSemaphoreGive(s_lock);
    }
}

void devlink_publish_json(devlink_channel_t *ch, const char *json)
{
    if (listening(ch) && (xSemaphoreTake(s_lock, LOCK_WAIT) == pdTRUE)) {
        publish_locked(ch, fill_raw, json);
        xSemaphoreGive(s_lock);
    }
}

void devlink_debug(const char *ch, const char *fmt, ...)
{
    char name[32];
    snprintf(name, sizeof(name), "dbg.%s", ch);
    devlink_channel_t *c = devlink_channel_find(name);
    if (c == NULL) {
        c = devlink_channel_register(
            &(devlink_channel_cfg_t){.name = name, .unit = "", .type = DL_OBJ, .default_hz = 0, .max_hz = 0});
    }
    if (!listening(c) || (xSemaphoreTake(s_lock, LOCK_WAIT) != pdTRUE)) {
        return;
    }
    char text[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    dl_jw_t w;
    dl_jw_init(&w, s_line, sizeof(s_line));
    dl_jw_str(&w, text); /* the value is a JSON string */
    publish_locked(c, fill_raw, s_line);
    xSemaphoreGive(s_lock);
}

/* ---- Log tap: ESP_LOG* -> "log" channel, still printed on USB (SPEC §5.7) ---- */

typedef struct {
    const dl_log_line_t *l;
} log_arg_t;

static void fill_log(dl_jw_t *w, const devlink_channel_t *ch, int64_t ts, const void *arg)
{
    (void)ch;
    const dl_log_line_t *l = ((const log_arg_t *)arg)->l;
    dl_frame_log(w, ts, l->lvl, l->tag, l->tag_len, l->msg, l->msg_len);
}

static int log_hook(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int r = s_orig_vprintf(fmt, ap);
    if (!xPortInIsrContext() && listening(s_log)) {
        if (xSemaphoreTake(s_lock, 0) == pdTRUE) { /* never block a logging task */
            int n = vsnprintf(s_line, sizeof(s_line), fmt, copy);
            if (n > 0) {
                size_t len = ((size_t)n < sizeof(s_line)) ? (size_t)n : sizeof(s_line) - 1u;
                dl_log_line_t l;
                dl_log_parse(s_line, len, &l);
                log_arg_t a = {.l = &l};
                publish_locked(s_log, fill_log, &a);
            }
            xSemaphoreGive(s_lock);
        } else {
            s_busy_drops++;
        }
    }
    va_end(copy);
    return r;
}

/* ---- Sender ---- */

static void remove_client_locked(int fd)
{
    for (size_t c = 0; c < DEVLINK_MAX_WS_CLIENTS; c++) {
        if (s_cl[c].used && (s_cl[c].fd == fd)) {
            s_cl[c].used = false;
            s_cl[c].sub = 0;
        }
    }
}

/* True if the socket can take data now. The send itself runs in the HTTP server task (httpd_ws_send_data), so it
 * must never block there: a stalled client would freeze the whole API (and in recovery the health check). */
static bool writable(int fd)
{
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    struct timeval tv = {0, 0};
    return select(fd + 1, NULL, &w, NULL, &tv) > 0;
}

static void sender_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        for (size_t c = 0; c < DEVLINK_MAX_WS_CLIENTS; c++) {
            for (;;) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                int fd = s_cl[c].fd;
                if (!s_cl[c].used) {
                    xSemaphoreGive(s_lock);
                    break;
                }
                if (!writable(fd)) {
                    /* Slow client: frames wait in its ring (new ones are dropped and counted), nothing blocks. */
                    int64_t now = esp_timer_get_time();
                    if (s_cl[c].stalled_since == 0) {
                        s_cl[c].stalled_since = now;
                    }
                    bool give_up = (now - s_cl[c].stalled_since) > STALL_US;
                    if (give_up) {
                        remove_client_locked(fd);
                    }
                    xSemaphoreGive(s_lock);
                    if (give_up) {
                        (void)httpd_sess_trigger_close(s_server, fd);
                    }
                    break;
                }
                s_cl[c].stalled_since = 0;
                size_t n = s_cl[c].used ? dl_ring_pop_batch(&s_cl[c].ring, s_msg, sizeof(s_msg)) : 0u;
                /* s_msg is only used by this task: it can be sent after unlocking */
                xSemaphoreGive(s_lock);
                if (n == 0u) {
                    break;
                }
                httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)s_msg, .len = n};
                if (httpd_ws_send_data(s_server, fd, &f) != ESP_OK) {
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    remove_client_locked(fd);
                    xSemaphoreGive(s_lock);
                    break;
                }
            }
        }
    }
}

/* ---- WebSocket handler (runs in the HTTP server task) ---- */

static client_t *client_for(int fd)
{
    for (size_t c = 0; c < DEVLINK_MAX_WS_CLIENTS; c++) {
        if (s_cl[c].used && (s_cl[c].fd == fd)) {
            return &s_cl[c];
        }
    }
    return NULL;
}

static void ws_reply(httpd_req_t *req, const char *text)
{
    httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)text, .len = strlen(text)};
    (void)httpd_ws_send_frame(req, &f);
}

void dl_stream_channels_json(dl_jw_t *w)
{
    static const char *types[] = {"number", "array", "object"};
    dl_jw_raw(w, "{\"channels\":[");
    for (size_t i = 0; i < s_nch; i++) {
        dl_jw_raw(w, (i > 0u) ? ",{" : "{");
        dl_jw_kv_str(w, "name", s_ch[i].name);
        dl_jw_kv_str(w, "unit", s_ch[i].unit);
        dl_jw_kv_str(w, "type", types[s_ch[i].type]);
        dl_jw_kv_uint(w, "default_hz", s_ch[i].default_hz);
        dl_jw_kv_uint(w, "max_hz", s_ch[i].max_hz);
        dl_jw_raw(w, "}");
    }
    dl_jw_raw(w, "]");
    dl_jw_kv_uint(w, "max_clients", DEVLINK_MAX_WS_CLIENTS); /* key adds the comma */
    dl_jw_raw(w, "}");
}

static void handle_op(httpd_req_t *req, client_t *cl, const char *j, size_t len)
{
    static char reply[512];
    char op[8] = "";
    char name[32];
    dl_jw_t w;
    dl_jw_init(&w, reply, sizeof(reply));
    (void)dl_json_get_str(j, len, "op", op, sizeof(op));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if ((strcmp(op, "sub") == 0) || (strcmp(op, "unsub") == 0)) {
        bool sub = (op[0] == 's');
        dl_jw_raw(&w, "{");
        dl_jw_kv_str(&w, "op", op);
        dl_jw_key(&w, "ok");
        dl_jw_raw(&w, "[");
        size_t ok = 0;
        size_t i = 0;
        if (!sub && (dl_json_get_str_at(j, len, "ch", 0, name, sizeof(name)) == DL_JSON_MISSING)) {
            cl->sub = 0; /* unsub without "ch": everything */
        }
        for (; dl_json_get_str_at(j, len, "ch", i, name, sizeof(name)) == DL_JSON_OK; i++) {
            devlink_channel_t *ch = devlink_channel_find(name);
            if (ch == NULL) {
                continue;
            }
            if (sub) {
                cl->sub |= 1u << ch->index;
                uint16_t hz = ch->default_hz;
                cl->hz[ch->index] = ((ch->max_hz > 0u) && (hz > ch->max_hz)) ? ch->max_hz : hz;
                cl->next[ch->index] = 0;
            } else {
                cl->sub &= ~(1u << ch->index);
            }
            dl_jw_raw(&w, (ok++ > 0u) ? "," : "");
            dl_jw_str(&w, ch->name);
        }
        dl_jw_raw(&w, "],\"unknown\":[");
        size_t unk = 0;
        for (i = 0; dl_json_get_str_at(j, len, "ch", i, name, sizeof(name)) == DL_JSON_OK; i++) {
            if (devlink_channel_find(name) == NULL) {
                dl_jw_raw(&w, (unk++ > 0u) ? "," : "");
                dl_jw_str(&w, name);
            }
        }
        dl_jw_raw(&w, "]}");
    } else if (strcmp(op, "rate") == 0) {
        long hz = -1;
        devlink_channel_t *ch = NULL;
        if (dl_json_get_str(j, len, "ch", name, sizeof(name)) == DL_JSON_OK) {
            ch = devlink_channel_find(name);
        }
        if ((ch == NULL) || (dl_json_get_int(j, len, "hz", &hz) != DL_JSON_OK) || (hz < 0) || (hz > 65535)) {
            dl_jw_raw(&w, "{\"op\":\"error\",\"error\":\"bad_rate_request\"}");
        } else {
            uint16_t v = (uint16_t)hz;
            if ((ch->max_hz > 0u) && ((v == 0u) || (v > ch->max_hz))) {
                v = ch->max_hz; /* clamp to what the channel allows */
            }
            cl->hz[ch->index] = v;
            cl->next[ch->index] = 0;
            dl_jw_raw(&w, "{\"op\":\"rate\"");
            dl_jw_kv_str(&w, "ch", ch->name);
            dl_jw_kv_uint(&w, "hz", v);
            dl_jw_raw(&w, "}");
        }
    } else if (strcmp(op, "list") == 0) {
        dl_stream_channels_json(&w);
    } else {
        dl_jw_raw(&w, "{\"op\":\"error\",\"error\":\"unknown_op\"}");
    }
    xSemaphoreGive(s_lock);
    ws_reply(req, w.overflow ? "{\"op\":\"error\",\"error\":\"reply_too_long\"}" : reply);
}

/* Called by the server right after it answered the WebSocket handshake (the URI handler is not called for the
 * handshake in IDF v6): register the client, or refuse it (max 2, SPEC §5.7). */
esp_err_t dl_stream_ws_post_handshake(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        client_t *free_cl = NULL;
        for (size_t c = 0; (c < DEVLINK_MAX_WS_CLIENTS) && (free_cl == NULL); c++) {
            free_cl = s_cl[c].used ? NULL : &s_cl[c];
        }
        if (free_cl != NULL) {
            memset(free_cl->hz, 0, sizeof(free_cl->hz));
            memset(free_cl->next, 0, sizeof(free_cl->next));
            dl_ring_init(&free_cl->ring);
            free_cl->stalled_since = 0;
            free_cl->sub = 0;
            free_cl->fd = fd;
            free_cl->used = true;
        }
        xSemaphoreGive(s_lock);
        if (free_cl == NULL) {
            ws_reply(req, "{\"op\":\"error\",\"error\":\"too_many_clients\"}");
            return ESP_FAIL; /* closes the connection */
        }
        ws_reply(req, "{\"op\":\"hello\",\"api\":1}");
        return ESP_OK;
    }
}

/* Data frames from a client (handshake: dl_stream_ws_post_handshake). */
esp_err_t dl_stream_ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    static uint8_t rx[WS_RX_MAX + 1u];
    httpd_ws_frame_t f = {.payload = NULL};
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if ((err != ESP_OK) || (f.len > WS_RX_MAX)) {
        return ESP_FAIL;
    }
    f.payload = rx;
    err = httpd_ws_recv_frame(req, &f, WS_RX_MAX);
    if ((err != ESP_OK) || (f.type != HTTPD_WS_TYPE_TEXT)) {
        return (err == ESP_OK) ? ESP_OK : ESP_FAIL;
    }
    rx[f.len] = '\0';
    client_t *cl = client_for(fd);
    if (cl == NULL) {
        return ESP_FAIL;
    }
    if (!dl_json_valid_object((const char *)rx, f.len)) {
        ws_reply(req, "{\"op\":\"error\",\"error\":\"invalid_json\"}");
        return ESP_OK;
    }
    handle_op(req, cl, (const char *)rx, f.len);
    return ESP_OK;
}

void dl_stream_on_close(int fd)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        remove_client_locked(fd);
        xSemaphoreGive(s_lock);
    }
}

uint32_t dl_stream_drops(void)
{
    uint32_t d = s_busy_drops;
    for (size_t c = 0; c < DEVLINK_MAX_WS_CLIENTS; c++) {
        d += s_cl[c].ring.drops;
    }
    return d;
}

uint32_t dl_stream_clients(void)
{
    uint32_t n = 0;
    for (size_t c = 0; c < DEVLINK_MAX_WS_CLIENTS; c++) {
        n += s_cl[c].used ? 1u : 0u;
    }
    return n;
}

esp_err_t dl_stream_start(void *server)
{
    lazy_init();
    s_server = server;
    if (s_log == NULL) {
        s_log = devlink_channel_register(
            &(devlink_channel_cfg_t){.name = "log", .unit = "", .type = DL_OBJ, .default_hz = 0, .max_hz = 0});
        s_orig_vprintf = esp_log_set_vprintf(log_hook);
    }
    if ((s_sender == NULL) && (xTaskCreate(sender_task, "dl_stream", 4096, NULL, 4, &s_sender) != pdPASS)) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---- Small module helpers ---- */

esp_err_t devlink_sleep(uint32_t seconds)
{
    if (seconds == 0u) {
        return ESP_ERR_INVALID_ARG; /* never sleep without a wake-up source (SPEC §5.8) */
    }
    (void)esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000u);
    esp_deep_sleep_start();
    return ESP_OK;
}

void devlink_set_temperature(float celsius)
{
    s_temp = celsius;
}

bool dl_stream_temperature(float *celsius)
{
    *celsius = s_temp;
    return s_temp > -999.0f;
}
