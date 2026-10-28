/*
 * dlcfg partition driver (SPEC §5.3): two 4 KB sectors, A/B records, power-loss safe. Format in dlcfg_core.c.
 */
#include <string.h>

#include "devlink.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define DL_PART_SUBTYPE_DLCFG ((esp_partition_subtype_t)0x40)

static const char *TAG = "dlcfg";

static uint8_t s_buf[2][DLCFG_SECTOR_SIZE];
static int s_selected = -1;
static uint32_t s_seq;
static SemaphoreHandle_t s_lock;

static const esp_partition_t *part(void)
{
    static const esp_partition_t *p;
    if (p == NULL) {
        p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, DL_PART_SUBTYPE_DLCFG, "dlcfg");
        s_lock = xSemaphoreCreateMutex();
    }
    return p;
}

static esp_err_t read_select(dlcfg_t *cfg)
{
    const esp_partition_t *p = part();
    esp_err_t err;

    if ((p == NULL) || (p->size < 2u * DLCFG_SECTOR_SIZE)) {
        return ESP_ERR_NOT_FOUND;
    }
    err = esp_partition_read(p, 0, s_buf[0], DLCFG_SECTOR_SIZE);
    if (err == ESP_OK) {
        err = esp_partition_read(p, DLCFG_SECTOR_SIZE, s_buf[1], DLCFG_SECTOR_SIZE);
    }
    if (err != ESP_OK) {
        return err;
    }
    s_selected = dlcfg_select(s_buf[0], s_buf[1], DLCFG_SECTOR_SIZE, cfg, &s_seq);
    return ESP_OK;
}

esp_err_t dlcfg_load(dlcfg_t *cfg)
{
    if (part() == NULL) {
        dlcfg_defaults(cfg);
        ESP_LOGE(TAG, "dlcfg partition missing, using defaults");
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_select(cfg);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        dlcfg_defaults(cfg);
    }
    return err;
}

esp_err_t dlcfg_save(const dlcfg_t *cfg)
{
    const esp_partition_t *p = part();
    dlcfg_t cur;
    esp_err_t err;

    if (p == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = read_select(&cur);
    if (err == ESP_OK) {
        int dst = dlcfg_write_sector(s_selected);
        uint32_t seq = s_seq + 1u;
        uint8_t *rec = s_buf[dst];
        size_t n = dlcfg_build(cfg, seq, rec, DLCFG_SECTOR_SIZE);
        size_t base = (size_t)dst * DLCFG_SECTOR_SIZE;
        const size_t hdr = sizeof(dlcfg_header_t);

        err = esp_partition_erase_range(p, base, DLCFG_SECTOR_SIZE);
        if (err == ESP_OK) { /* body first, header last: a torn write never looks valid */
            err = esp_partition_write(p, base + hdr, rec + hdr, n - hdr);
        }
        if (err == ESP_OK) {
            err = esp_partition_write(p, base, rec, hdr);
        }
        if (err == ESP_OK) {
            s_selected = dst;
            s_seq = seq;
        }
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

uint32_t dlcfg_sequence(void)
{
    return s_seq;
}
