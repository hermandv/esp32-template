/*
 * devlink retained-RTC block. Pure C, see dl_rtc.h.
 */
#include "dl_rtc.h"

#include <stddef.h>
#include <string.h>

uint32_t dl_crc32_update(uint32_t state, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    for (uint32_t i = 0u; i < len; i++) {
        state ^= p[i];
        for (int bit = 0; bit < 8; bit++) {
            state = (state >> 1) ^ (0xEDB88320u & (0u - (state & 1u)));
        }
    }
    return state;
}

uint32_t dl_crc32(const void *data, uint32_t len)
{
    return ~dl_crc32_update(DL_CRC32_INIT, data, len);
}

bool dl_rtc_valid(const dl_rtc_t *rtc)
{
    return (rtc->magic == DL_RTC_MAGIC) && (rtc->version == DL_RTC_VERSION) &&
           (rtc->crc == dl_crc32(rtc, (uint32_t)offsetof(dl_rtc_t, crc)));
}

void dl_rtc_seal(dl_rtc_t *rtc)
{
    rtc->magic = DL_RTC_MAGIC;
    rtc->version = DL_RTC_VERSION;
    rtc->crc = dl_crc32(rtc, (uint32_t)offsetof(dl_rtc_t, crc));
}

void dl_rtc_init(dl_rtc_t *rtc)
{
    (void)memset(rtc, 0, sizeof(*rtc));
    rtc->req = DL_RTC_REQ_NONE;
    dl_rtc_seal(rtc);
}
