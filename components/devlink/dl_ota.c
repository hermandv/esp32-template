/*
 * OTA upload (SPEC §7.1, §6.5): single lock, header checks before writing, sequential erase, idle timeout,
 * SHA-256 check, boot partition, reply, restart.
 */
#include <string.h>

#include "devlink.h"
#include "dl_ota_check.h"
#include "dl_priv.h"
#include "esp_flash.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/efuse_hal.h"
#include "sdkconfig.h"

static const char *TAG = "dl_ota";

#define CHUNK 4096
#define DRAIN_MAX (0x300000u + 0x10000u) /* largest slot + slack */

static uint8_t s_buf[CHUNK];
static dl_ota_check_t s_chk;
static dl_ota_progress_t s_prog = {.state = "idle"};
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;

extern void dl_http_schedule_reboot(int target);

bool dl_ota_busy(void)
{
    return s_busy;
}

void dl_ota_get_progress(dl_ota_progress_t *p)
{
    *p = s_prog;
}

static bool lock(void)
{
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (!s_busy) {
        s_busy = true;
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

static void unlock(void)
{
    portENTER_CRITICAL(&s_mux);
    s_busy = false;
    portEXIT_CRITICAL(&s_mux);
}

/* Target slot (SPEC §6.5): in recovery the slot that is NOT the last valid one, so the good one stays as fallback. */
static const esp_partition_t *target_slot(void)
{
    const devlink_boot_info_t *bi = devlink_boot_info();
    if (bi->mode == DEVLINK_MODE_APP) {
        return esp_ota_get_next_update_partition(NULL);
    }
    int valid;
    int dst = dl_valid_app_slot(&valid) ? (1 - valid) : 0;
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + dst),
                                    NULL);
}

/* Read and discard the rest of the body. Replying and closing while the client is still sending makes the client's
 * TCP stack reset the connection, and it never sees the error. Bounded by the largest slot and the 10 s idle
 * timeout; a body announced larger than that is not drained (the client checks sizes itself, SPEC §7.2). */
static void drain(httpd_req_t *req, size_t left, uint8_t *buf, size_t buf_size)
{
    if (left > DRAIN_MAX) {
        return;
    }
    while (left > 0u) {
        int n = httpd_req_recv(req, (char *)buf, (left < buf_size) ? left : buf_size);
        if (n <= 0) {
            return;
        }
        left -= (size_t)n;
    }
}

static esp_err_t reply_error(httpd_req_t *req, const char *status, const char *error)
{
    char body[96];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", error);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *error, esp_ota_handle_t *h, size_t left)
{
    if ((h != NULL) && (*h != 0)) {
        (void)esp_ota_abort(*h); /* ready for a new attempt without a reboot (SPEC §7.1 step 5) */
        *h = 0;
    }
    s_prog.state = "error";
    s_prog.error = error;
    drain(req, left, s_buf, sizeof(s_buf));
    unlock();
    /* Log after unlocking: streaming (and so the log channel) is paused while the OTA lock is held. */
    ESP_LOGW(TAG, "upload rejected: %s", error);
    (void)reply_error(req, status, error);
    return ESP_FAIL;
}

/* Recovery update (SPEC §7.3, I5): only from a valid app that was not just rolled back. Written straight into
 * `factory` (no A/B), verified, then the self-test boot: the app marks the self-test pending in dlcfg and reboots into
 * recovery; recovery records the result and restarts into the app. */
static esp_err_t run_recovery_upload(httpd_req_t *req, bool force)
{
    const devlink_boot_info_t *bi = devlink_boot_info();
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;

    if (bi->mode != DEVLINK_MODE_APP) {
        return fail(req, "409 Conflict", "recovery_cannot_update_itself", NULL, req->content_len);
    }
    if ((esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK) &&
        (state == ESP_OTA_IMG_PENDING_VERIFY)) {
        return fail(req, "409 Conflict", "app_not_valid_yet", NULL, req->content_len);
    }
    if (bi->reason == DL_BOOT_REASON_ROLLBACK) {
        return fail(req, "409 Conflict", "app_just_rolled_back", NULL, req->content_len);
    }
    if (part == NULL) {
        return fail(req, "500 Internal Server Error", "no_factory_partition", NULL, req->content_len);
    }
    uint32_t flash = 0;
    (void)esp_flash_get_size(NULL, &flash);
    dl_ota_expect_t exp = {
        .chip_id = DL_CHIP_ID_ESP32S3,
        .chip_rev_full = (uint16_t)efuse_hal_chip_revision(),
        .flash_size = flash,
        .partition_size = part->size,
        .project_prefix = "",
        .force = force,
    };
    dl_ota_result_t r = dl_ota_check_begin(&s_chk, &exp, (uint32_t)req->content_len);
    if (r != DL_OTA_OK) {
        return fail(req, "413 Payload Too Large", dl_ota_result_name(r), NULL, 0);
    }
    ESP_LOGW(TAG, "RECOVERY update: %u bytes -> factory", (unsigned)req->content_len);

    size_t left = req->content_len;
    size_t off = 0;        /* bytes written to factory */
    size_t erased = 0;     /* factory erased up to here (sector by sector, so the server stays responsive) */
    bool begun = false;
    while (left > 0u) {
        int n = httpd_req_recv(req, (char *)s_buf, (left < CHUNK) ? left : CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            return fail(req, "408 Request Timeout", "idle_timeout", NULL, 0);
        }
        if (n <= 0) {
            return fail(req, "400 Bad Request", "connection_lost", NULL, 0);
        }
        left -= (size_t)n;
        bool flush;
        size_t data_off;
        r = dl_ota_check_feed(&s_chk, s_buf, (size_t)n, &flush, &data_off);
        if (r == DL_OTA_NEED_MORE) {
            continue;
        }
        if (r != DL_OTA_OK) {
            return fail(req, "400 Bad Request", dl_ota_result_name(r), NULL, left);
        }
        if (flush && !force && (strstr(s_chk.project, "recovery") == NULL)) {
            return fail(req, "400 Bad Request", "not_a_recovery_image", NULL, left); /* nothing written yet */
        }
        /* Header accepted: write the buffered head, then this chunk's remainder. */
        const uint8_t *pieces[2] = {flush ? s_chk.head : NULL, s_buf + data_off};
        size_t lens[2] = {flush ? s_chk.head_len : 0u, (size_t)n - data_off};
        if (flush) {
            begun = true;
            strncpy(s_prog.version, s_chk.version, sizeof(s_prog.version) - 1u);
        }
        for (int i = 0; (i < 2) && begun; i++) {
            if (lens[i] == 0u) {
                continue;
            }
            while (erased < off + lens[i]) {
                if (esp_partition_erase_range(part, erased, 4096) != ESP_OK) {
                    return fail(req, "500 Internal Server Error", "factory_erase_failed", NULL, left);
                }
                erased += 4096u;
            }
            if (esp_partition_write(part, off, pieces[i], lens[i]) != ESP_OK) {
                return fail(req, "500 Internal Server Error", "factory_write_failed", NULL, left);
            }
            off += lens[i];
        }
        s_prog.written = (uint32_t)off;
    }
    r = dl_ota_check_finish(&s_chk);
    if (r != DL_OTA_OK) {
        return fail(req, "400 Bad Request", dl_ota_result_name(r), NULL, 0);
    }
    s_prog.state = "verifying";
    esp_image_metadata_t meta;
    esp_partition_pos_t pos = {.offset = part->address, .size = part->size};
    if (esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta) != ESP_OK) {
        /* factory now holds a broken image; the app stays valid (I5), the PC app offers the previous recovery */
        return fail(req, "400 Bad Request", "recovery_image_corrupt", NULL, 0);
    }

    dlcfg_t cfg;
    (void)dlcfg_load(&cfg);
    cfg.rec_selftest = DLCFG_SELFTEST_PENDING;
    strncpy(cfg.rec_selftest_version, s_chk.version, sizeof(cfg.rec_selftest_version) - 1u);
    cfg.rec_selftest_version[sizeof(cfg.rec_selftest_version) - 1u] = '\0';
    if (dlcfg_save(&cfg) != ESP_OK) {
        return fail(req, "500 Internal Server Error", "config_save_failed", NULL, 0);
    }
    s_prog.state = "done";
    ESP_LOGW(TAG, "recovery %s written and verified: self-test boot", s_chk.version);

    char body[192];
    dl_jw_t w;
    dl_jw_init(&w, body, sizeof(body));
    dl_jw_raw(&w, "{\"ok\":true");
    dl_jw_kv_str(&w, "project", s_chk.project);
    dl_jw_kv_str(&w, "version", s_chk.version);
    dl_jw_kv_str(&w, "slot", part->label);
    dl_jw_kv_str(&w, "selftest", "running");
    dl_jw_raw(&w, "}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    dl_http_schedule_reboot(1); /* one-shot into recovery: the self-test boot */
    return ESP_OK;
}

/* The upload itself, in the OTA worker task (the lock is already held). */
static esp_err_t run_upload(httpd_req_t *req, bool to_recovery, bool force)
{
    esp_ota_handle_t h = 0;

    memset(&s_prog, 0, sizeof(s_prog));
    s_prog.state = "receiving";
    s_prog.total = req->content_len;

    if (to_recovery) {
        return run_recovery_upload(req, force);
    }
    const esp_partition_t *part = target_slot();
    if (part == NULL) {
        return fail(req, "500 Internal Server Error", "no_target_slot", NULL, req->content_len);
    }
    uint32_t flash = 0;
    (void)esp_flash_get_size(NULL, &flash);
    dl_ota_expect_t exp = {
        .chip_id = DL_CHIP_ID_ESP32S3,
        .chip_rev_full = (uint16_t)efuse_hal_chip_revision(),
        .flash_size = flash,
        .partition_size = part->size,
        .project_prefix = CONFIG_DEVLINK_PROJECT_PREFIX,
        .force = force,
    };
    dl_ota_result_t r = dl_ota_check_begin(&s_chk, &exp, (uint32_t)req->content_len);
    if (r != DL_OTA_OK) {
        return fail(req, "413 Payload Too Large", dl_ota_result_name(r), NULL, 0); /* answer at once, no drain */
    }
    ESP_LOGI(TAG, "upload %u bytes -> %s%s", (unsigned)req->content_len, part->label, force ? " (force)" : "");

    size_t left = req->content_len;
    while (left > 0u) {
        int n = httpd_req_recv(req, (char *)s_buf, (left < CHUNK) ? left : CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            return fail(req, "408 Request Timeout", "idle_timeout", &h, 0); /* 10 s without data */
        }
        if (n <= 0) {
            return fail(req, "400 Bad Request", "connection_lost", &h, 0);
        }
        left -= (size_t)n;
        bool flush;
        size_t off;
        r = dl_ota_check_feed(&s_chk, s_buf, (size_t)n, &flush, &off);
        if (r == DL_OTA_NEED_MORE) {
            continue;
        }
        if (r != DL_OTA_OK) {
            return fail(req, "400 Bad Request", dl_ota_result_name(r), &h, left);
        }
        if (flush) {
            /* Header accepted: only now touch the flash. Sequential erase keeps the server responsive. */
            strncpy(s_prog.version, s_chk.version, sizeof(s_prog.version) - 1u);
            esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
            if (err == ESP_OK) {
                err = esp_ota_write(h, s_chk.head, s_chk.head_len);
            }
            if (err != ESP_OK) {
                return fail(req, "500 Internal Server Error", esp_err_to_name(err), &h, left);
            }
            s_prog.written = (uint32_t)s_chk.head_len;
        }
        if ((size_t)n > off) {
            esp_err_t err = esp_ota_write(h, s_buf + off, (size_t)n - off);
            if (err != ESP_OK) {
                return fail(req, "500 Internal Server Error", esp_err_to_name(err), &h, left);
            }
            s_prog.written += (uint32_t)((size_t)n - off);
        }
    }

    r = dl_ota_check_finish(&s_chk);
    if (r != DL_OTA_OK) {
        return fail(req, "400 Bad Request", dl_ota_result_name(r), &h, left);
    }
    s_prog.state = "verifying";
    esp_err_t err = esp_ota_end(h); /* SHA-256 / image verification */
    h = 0;
    if (err != ESP_OK) {
        return fail(req, "400 Bad Request", (err == ESP_ERR_OTA_VALIDATE_FAILED) ? "image_corrupt" : esp_err_to_name(err), NULL,
                    0);
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        return fail(req, "500 Internal Server Error", esp_err_to_name(err), NULL, 0);
    }
    s_prog.state = "done";
    ESP_LOGI(TAG, "%s %s written to %s, restarting", s_chk.project, s_chk.version, part->label);

    char body[192];
    dl_jw_t w; /* project/version come from the uploaded image: escape them */
    dl_jw_init(&w, body, sizeof(body));
    dl_jw_raw(&w, "{\"ok\":true");
    dl_jw_kv_str(&w, "project", s_chk.project);
    dl_jw_kv_str(&w, "version", s_chk.version);
    dl_jw_kv_str(&w, "slot", part->label);
    dl_jw_raw(&w, "}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    dl_http_schedule_reboot(0); /* lock stays taken until the restart */
    return ESP_OK;
}

/* ---- Async front end: the upload runs in its own task, so the HTTP server stays free for /ota/progress, the 409
 * for a second upload (test 10) and the health heartbeat (a long upload must not starve the RTC WDT feed). ---- */

typedef struct {
    httpd_req_t *req;
    bool to_recovery;
    bool force;
} job_t;

static QueueHandle_t s_jobs;    /* uploads, one at a time (lock held) */
static QueueHandle_t s_rejects; /* concurrent uploads to drain and answer with 409 */

static void ota_worker(void *arg)
{
    (void)arg;
    for (;;) {
        job_t j;
        if (xQueueReceive(s_jobs, &j, portMAX_DELAY) == pdTRUE) {
            (void)run_upload(j.req, j.to_recovery, j.force);
            (void)httpd_req_async_handler_complete(j.req);
        }
    }
}

static void reject_worker(void *arg)
{
    (void)arg;
    static uint8_t buf[1024];
    for (;;) {
        httpd_req_t *req;
        if (xQueueReceive(s_rejects, &req, portMAX_DELAY) == pdTRUE) {
            drain(req, req->content_len, buf, sizeof(buf));
            (void)reply_error(req, "409 Conflict", "ota_in_progress");
            (void)httpd_req_async_handler_complete(req);
        }
    }
}

static bool start_workers(void)
{
    if (s_jobs != NULL) {
        return true;
    }
    s_jobs = xQueueCreate(1, sizeof(job_t));
    s_rejects = xQueueCreate(2, sizeof(httpd_req_t *));
    return (s_jobs != NULL) && (s_rejects != NULL) &&
           (xTaskCreate(ota_worker, "dl_ota", 6144, NULL, 5, NULL) == pdPASS) &&
           (xTaskCreate(reject_worker, "dl_ota_rej", 3072, NULL, 4, NULL) == pdPASS);
}

esp_err_t dl_ota_handle(httpd_req_t *req, bool to_recovery, bool force)
{
    httpd_req_t *copy = NULL;

    if (!start_workers()) {
        return reply_error(req, "500 Internal Server Error", "no_memory");
    }
    if (!lock()) {
        if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
            (void)reply_error(req, "409 Conflict", "ota_in_progress");
            return ESP_FAIL;
        }
        if (xQueueSend(s_rejects, &copy, 0) != pdTRUE) { /* reject task busy: answer without draining */
            (void)reply_error(copy, "409 Conflict", "ota_in_progress");
            (void)httpd_req_async_handler_complete(copy);
        }
        return ESP_OK;
    }
    job_t j = {.to_recovery = to_recovery, .force = force};
    if (httpd_req_async_handler_begin(req, &j.req) != ESP_OK) {
        unlock();
        return reply_error(req, "500 Internal Server Error", "ota_start_failed");
    }
    if (xQueueSend(s_jobs, &j, 0) != pdTRUE) { /* cannot happen while the lock is held, but stay safe */
        unlock();
        (void)reply_error(j.req, "500 Internal Server Error", "ota_start_failed");
        (void)httpd_req_async_handler_complete(j.req);
    }
    return ESP_OK;
}
