/* Built-in app channels (SPEC §5.8): sys.temp (die temperature, low absolute accuracy), sys.heap, sys.rssi. */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the sys.* channels and start sampling them (only while a client is subscribed).
 *
 * @requirement SPEC-5.8
 * @parent      -
 * @test        T3
 */
esp_err_t devlink_sysmon_start(void);

#ifdef __cplusplus
}
#endif
