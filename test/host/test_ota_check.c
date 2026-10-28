/*
 * Host unit tests for the OTA image checks (SPEC §7.1 step 3, acceptance test 21).
 * Optional: pass real .bin files as arguments; each must pass the header checks (and report its marker).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dl_ota_check.h"
#include "unity_lite.h"

UT_MAIN_DEFS

#define IMG_LEN 4096u

static const uint8_t k_magic[DL_MARKER_MAGIC_LEN] = DL_MARKER_MAGIC_BYTES;

typedef struct {
    uint8_t magic;
    uint16_t chip;
    uint16_t min_rev;
    uint16_t max_rev;
    uint8_t flash_nibble; /* 3 = 8 MB */
    uint32_t desc_magic;
    const char *project;
    long marker_at;       /* -1 = none */
} img_t;

static img_t good(void)
{
    return (img_t){.magic = 0xE9, .chip = 9, .min_rev = 0, .max_rev = 99, .flash_nibble = 3,
                   .desc_magic = DL_APP_DESC_MAGIC, .project = "bootlab", .marker_at = 1000};
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void build(const img_t *m, uint8_t *b)
{
    memset(b, 0x5A, IMG_LEN);
    memset(b, 0, DL_OTA_HEAD_LEN);
    b[0] = m->magic;
    b[1] = 4;
    b[3] = (uint8_t)((m->flash_nibble << 4) | 0x0F);
    wr16(b + 12, m->chip);
    wr16(b + 15, m->min_rev);
    wr16(b + 17, m->max_rev);
    uint8_t *d = b + 32;
    d[0] = (uint8_t)m->desc_magic;
    d[1] = (uint8_t)(m->desc_magic >> 8);
    d[2] = (uint8_t)(m->desc_magic >> 16);
    d[3] = (uint8_t)(m->desc_magic >> 24);
    strcpy((char *)d + 16, "1.2.3");
    strncpy((char *)d + 48, m->project, 31);
    strcpy((char *)d + 80, "12:34:56");
    strcpy((char *)d + 96, "Oct 28 2026");
    strcpy((char *)d + 112, "v6.0.3");
    if (m->marker_at >= 0) {
        memcpy(b + m->marker_at, k_magic, sizeof(k_magic));
    }
}

static dl_ota_expect_t expect(void)
{
    return (dl_ota_expect_t){.chip_id = 9, .chip_rev_full = 1, .flash_size = 8u << 20,
                             .partition_size = 0x300000, .project_prefix = "", .force = false};
}

/* Run a whole upload with the given chunking; returns the first error or the final verdict. Checks that nothing is
 * "written" before the header is accepted and that the written bytes equal the image. */
static dl_ota_result_t run(const uint8_t *img, size_t len, const dl_ota_expect_t *e, unsigned seed, size_t *written,
                           dl_ota_check_t *out)
{
    static uint8_t flash[IMG_LEN];
    dl_ota_check_t c;
    size_t w = 0;
    dl_ota_result_t r = dl_ota_check_begin(&c, e, (uint32_t)len);
    if (r != DL_OTA_OK) {
        *written = 0;
        return r;
    }
    srand(seed);
    size_t pos = 0;
    while (pos < len) {
        size_t n = 1 + (size_t)rand() % 700;
        if (n > len - pos) {
            n = len - pos;
        }
        bool flush;
        size_t off;
        r = dl_ota_check_feed(&c, img + pos, n, &flush, &off);
        if (r == DL_OTA_NEED_MORE) {
            pos += n;
            continue;
        }
        if (r != DL_OTA_OK) {
            *written = w;
            return r;
        }
        if (flush) {
            memcpy(flash + w, c.head, c.head_len);
            w += c.head_len;
        }
        memcpy(flash + w, img + pos + off, n - off);
        w += n - off;
        pos += n;
    }
    *written = w;
    if (out != NULL) {
        *out = c;
    }
    r = dl_ota_check_finish(&c);
    if ((r == DL_OTA_OK) && (memcmp(flash, img, len) != 0)) {
        return DL_OTA_E_LENGTH; /* written bytes differ from the image */
    }
    return r;
}

static void ota_good_image_any_chunking(void)
{
    uint8_t b[IMG_LEN];
    img_t m = good();
    build(&m, b);
    dl_ota_expect_t e = expect();
    for (unsigned seed = 0; seed < 200; seed++) {
        size_t w;
        dl_ota_check_t c;
        dl_ota_result_t r = run(b, IMG_LEN, &e, seed, &w, &c);
        if (r != DL_OTA_OK) {
            UT_EQ(r, DL_OTA_OK);
            break;
        }
        UT_EQ(w, IMG_LEN);
        if (seed == 0) {
            UT_CHECK(strcmp(c.version, "1.2.3") == 0);
            UT_CHECK(strcmp(c.project, "bootlab") == 0);
            UT_CHECK(strcmp(c.idf_ver, "v6.0.3") == 0);
            UT_CHECK(strcmp(c.date, "Oct 28 2026") == 0);
        }
    }
}

static void ota_marker_split_over_chunks(void)
{
    uint8_t b[IMG_LEN];
    img_t m = good();
    m.marker_at = IMG_LEN - DL_MARKER_MAGIC_LEN; /* at the very end */
    build(&m, b);
    dl_ota_expect_t e = expect();
    for (unsigned seed = 0; seed < 100; seed++) {
        size_t w;
        UT_EQ(run(b, IMG_LEN, &e, seed, &w, NULL), DL_OTA_OK);
    }
    /* A near-miss (first 15 bytes) followed by the real marker. */
    m.marker_at = 2000;
    build(&m, b);
    memcpy(b + 1984, k_magic, 15);
    UT_EQ(run(b, IMG_LEN, &e, 7, &(size_t){0}, NULL), DL_OTA_OK);
}

static dl_ota_result_t verdict(img_t m, dl_ota_expect_t e)
{
    uint8_t b[IMG_LEN];
    size_t w;
    build(&m, b);
    dl_ota_result_t r = run(b, IMG_LEN, &e, 1, &w, NULL);
    if ((r != DL_OTA_OK) && (r != DL_OTA_E_NO_MARKER) && (r != DL_OTA_E_LENGTH)) {
        UT_EQ(w, 0); /* header rejections happen before anything is written */
    }
    return r;
}

static void ota_t21_rejections(void)
{
    dl_ota_expect_t e = expect();
    img_t m;

    m = good(), m.magic = 0x7F; /* ELF */
    UT_EQ(verdict(m, e), DL_OTA_E_MAGIC);
    m = good(), m.chip = 5; /* ESP32-C3 */
    UT_EQ(verdict(m, e), DL_OTA_E_CHIP);
    m = good(), m.min_rev = 2; /* needs v0.2, chip is v0.1 */
    UT_EQ(verdict(m, e), DL_OTA_E_CHIP_REV);
    m = good(), m.max_rev = 0;
    UT_EQ(verdict(m, e), DL_OTA_E_CHIP_REV);
    m = good(), m.desc_magic = 0; /* bootloader / merged image */
    UT_EQ(verdict(m, e), DL_OTA_E_NOT_APP);
    m = good(), m.flash_nibble = 4; /* 16 MB image on 8 MB flash */
    UT_EQ(verdict(m, e), DL_OTA_E_FLASH_SIZE);
    m = good(), m.marker_at = -1; /* foreign image */
    UT_EQ(verdict(m, e), DL_OTA_E_NO_MARKER);
}

static void ota_force_and_prefix(void)
{
    dl_ota_expect_t e = expect();
    img_t m = good();
    m.marker_at = -1;
    e.force = true;
    UT_EQ(verdict(m, e), DL_OTA_OK); /* foreign image allowed with force=1 */

    e = expect();
    e.project_prefix = "devlink";
    UT_EQ(verdict(good(), e), DL_OTA_E_PROJECT);
    m = good();
    m.project = "devlink_app";
    UT_EQ(verdict(m, e), DL_OTA_OK);
    e.force = true;
    UT_EQ(verdict(good(), e), DL_OTA_OK);

    e = expect();
    e.force = true;
    m = good();
    m.chip = 5;
    UT_EQ(verdict(m, e), DL_OTA_E_CHIP); /* force never overrides chip/format checks */
}

static void ota_size_checks(void)
{
    dl_ota_check_t c;
    dl_ota_expect_t e = expect();
    UT_EQ(dl_ota_check_begin(&c, &e, 0), DL_OTA_E_SIZE);
    UT_EQ(dl_ota_check_begin(&c, &e, DL_OTA_HEAD_LEN - 1), DL_OTA_E_SIZE);
    UT_EQ(dl_ota_check_begin(&c, &e, 0x300001), DL_OTA_E_SIZE);
    UT_EQ(dl_ota_check_begin(&c, &e, 0x300000), DL_OTA_OK);

    /* More bytes than announced. */
    uint8_t b[IMG_LEN];
    img_t m = good();
    build(&m, b);
    UT_EQ(dl_ota_check_begin(&c, &e, IMG_LEN - 10), DL_OTA_OK);
    bool f;
    size_t off;
    UT_EQ(dl_ota_check_feed(&c, b, IMG_LEN, &f, &off), DL_OTA_E_LENGTH);
    /* Fewer bytes than announced. */
    UT_EQ(dl_ota_check_begin(&c, &e, IMG_LEN), DL_OTA_OK);
    UT_EQ(dl_ota_check_feed(&c, b, IMG_LEN - 1, &f, &off), DL_OTA_OK);
    UT_EQ(dl_ota_check_finish(&c), DL_OTA_E_LENGTH);
}

static void ota_names(void)
{
    UT_CHECK(strcmp(dl_ota_result_name(DL_OTA_E_CHIP), "wrong_chip") == 0);
    UT_CHECK(strcmp(dl_ota_result_name((dl_ota_result_t)99), "unknown") == 0);
}

static void check_real_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("  (skip %s: not found)\n", path);
        return;
    }
    static uint8_t buf[0x300000];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    dl_ota_expect_t e = expect();
    dl_ota_check_t c;
    UT_EQ(dl_ota_check_begin(&c, &e, (uint32_t)n), DL_OTA_OK);
    bool fl;
    size_t off;
    UT_EQ(dl_ota_check_feed(&c, buf, n, &fl, &off), DL_OTA_OK);
    printf("  %s: project=%s version=%s idf=%s marker=%s\n", path, c.project, c.version, c.idf_ver,
           c.marker_found ? "yes" : "NO");
    UT_CHECK(c.marker_found);
    UT_EQ(dl_ota_check_finish(&c), DL_OTA_OK);
}

int main(int argc, char **argv)
{
    UT_RUN(ota_good_image_any_chunking);
    UT_RUN(ota_marker_split_over_chunks);
    UT_RUN(ota_t21_rejections);
    UT_RUN(ota_force_and_prefix);
    UT_RUN(ota_size_checks);
    UT_RUN(ota_names);
    for (int i = 1; i < argc; i++) {
        ut_current = argv[i];
        check_real_file(argv[i]);
    }
    return UT_SUMMARY();
}
