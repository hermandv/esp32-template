/*
 * devlink test app template (SPEC §3.5, §5): devlink boot steps first, then the network services, then the test
 * modules. Copy this project for a new board or IC.
 */
#include "devlink.h"
#include "devlink_sysmon.h"
#include "mod_example.h"
#include "nvs_flash.h"

void app_main(void)
{
    /* 1. devlink boot steps, before anything else (SPEC §3.5 rule 7): RTC WDT take-over, boot log, identity. */
    devlink_boot_early(DEVLINK_MODE_APP);

    /* 2. The app's own NVS (devlink does not use it; Wi-Fi PHY calibration data lives here). */
    esp_err_t err = nvs_flash_init();
    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        (void)nvs_flash_erase();
        (void)nvs_flash_init();
    }

    /* 3. USB console (Improv + devlink lines) and network services. The self-test marks a new app valid once the
     *    API is up and the network is usable; otherwise it is rolled back after 60 s. */
    devlink_console_start(NULL);
    devlink_start(DEVLINK_MODE_APP);

    /* 4. Built-in sys.* channels, then the test modules (copy mod_example for each IC under test). */
    devlink_sysmon_start();
    mod_example_start();
}
