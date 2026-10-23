// ATLAS 3-DoF force/torque sensor demo (ESP32-C5-DevKitC-1, ESP-IDF).
// Ported from the Arduino firmware by the student's repository (3-dof-atlas, branch demo): same sensors, same maths,
// same pages. New: reporting to the CloSer lab demos page, over-the-air updates with rollback, roaming, settings in
// flash instead of source, and no permanent halt on errors.
#include "closer_demo.h"
#include "display.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sensors.h"
#include "web.h"

void app_main(void) {
    display_start();
    display_boot_message("starting...");
    closer_demo_start("atlas");        // NVS, Wi-Fi, clock, reporting, updates (in the background)
    sensors_start();
    web_start();
    display_run();
    ESP_LOGI("atlas", "running, firmware %s", closer_demo_version());
}
