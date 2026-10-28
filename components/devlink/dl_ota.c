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
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/efuse_hal.h"
#include "sdkconfig.h"

static const char *TAG = "dl_ota";

#define CHUNK 4096

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

static esp_err_t fail(httpd_req_t *req, const char *status, const char *error, esp_ota_handle_t *h)
{
    if ((h != NULL) && (*h != 0)) {
        (void)esp_ota_abort(*h); /* ready for a new attempt without a reboot (SPEC §7.1 step 5) */
    }
    s_prog.state = "error";
    s_prog.error = error;
    ESP_LOGW(TAG, "upload rejected: %s", error);
    unlock();
    char body[96];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", error);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_FAIL; /* close the connection: the rest of the body is not read */
}

esp_err_t dl_ota_handle(httpd_req_t *req, bool to_recovery, bool force)
{
    esp_ota_handle_t h = 0;

    if (!lock()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"ota_in_progress\"}");
        return ESP_FAIL;
    }
    memset(&s_prog, 0, sizeof(s_prog));
    s_prog.state = "receiving";
    s_prog.total = req->content_len;

    if (to_recovery) {
        /* Recovery update (SPEC §7.3) comes with milestone 6; recovery never updates itself. */
        return fail(req, "501 Not Implemented", "recovery_update_not_available", NULL);
    }
    const esp_partition_t *part = target_slot();
    if (part == NULL) {
        return fail(req, "500 Internal Server Error", "no_target_slot", NULL);
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
        return fail(req, "413 Payload Too Large", dl_ota_result_name(r), NULL);
    }
    ESP_LOGI(TAG, "upload %u bytes -> %s%s", (unsigned)req->content_len, part->label, force ? " (force)" : "");

    size_t left = req->content_len;
    while (left > 0u) {
        int n = httpd_req_recv(req, (char *)s_buf, (left < CHUNK) ? left : CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            return fail(req, "408 Request Timeout", "idle_timeout", &h); /* 10 s without data */
        }
        if (n <= 0) {
            return fail(req, "400 Bad Request", "connection_lost", &h);
        }
        left -= (size_t)n;
        bool flush;
        size_t off;
        r = dl_ota_check_feed(&s_chk, s_buf, (size_t)n, &flush, &off);
        if (r == DL_OTA_NEED_MORE) {
            continue;
        }
        if (r != DL_OTA_OK) {
            return fail(req, "400 Bad Request", dl_ota_result_name(r), &h);
        }
        if (flush) {
            /* Header accepted: only now touch the flash. Sequential erase keeps the server responsive. */
            strncpy(s_prog.version, s_chk.version, sizeof(s_prog.version) - 1u);
            esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
            if (err == ESP_OK) {
                err = esp_ota_write(h, s_chk.head, s_chk.head_len);
            }
            if (err != ESP_OK) {
                return fail(req, "500 Internal Server Error", esp_err_to_name(err), &h);
            }
            s_prog.written = (uint32_t)s_chk.head_len;
        }
        if ((size_t)n > off) {
            esp_err_t err = esp_ota_write(h, s_buf + off, (size_t)n - off);
            if (err != ESP_OK) {
                return fail(req, "500 Internal Server Error", esp_err_to_name(err), &h);
            }
            s_prog.written += (uint32_t)((size_t)n - off);
        }
    }

    r = dl_ota_check_finish(&s_chk);
    if (r != DL_OTA_OK) {
        return fail(req, "400 Bad Request", dl_ota_result_name(r), &h);
    }
    s_prog.state = "verifying";
    esp_err_t err = esp_ota_end(h); /* SHA-256 / image verification */
    h = 0;
    if (err != ESP_OK) {
        return fail(req, "400 Bad Request", (err == ESP_ERR_OTA_VALIDATE_FAILED) ? "image_corrupt" : esp_err_to_name(err),
                    NULL);
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        return fail(req, "500 Internal Server Error", esp_err_to_name(err), NULL);
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
