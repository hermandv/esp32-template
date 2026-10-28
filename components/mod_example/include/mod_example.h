/* Template test module (SPEC §5.8). Publishes synthetic data; replace with the IC driver + its measurements. */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the module's channels and start its task.
 *
 * @requirement SPEC-5.8
 * @parent      -
 * @test        T3
 */
esp_err_t mod_example_start(void);

#ifdef __cplusplus
}
#endif
