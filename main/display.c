// 128x64 SSD1306 OLED, driven the way the Adafruit library did it in the original firmware: the five pins are
// toggled directly ("software SPI"), commands with DC low (parameters too), picture data with DC high.
// The IDF's esp_lcd/SPI route was tried first and left the panel white; this is the same sequence as the module worked with.
#include "display.h"

#include <stdio.h>
#include <string.h>

#include "calib.h"
#include "closer_demo.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "font5x7.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sensors.h"

static const char *TAG = "display";
#define W 128
#define H 64
#define COLS 21
#define PIN_MOSI CONFIG_ATLAS_OLED_MOSI_GPIO
#define PIN_CLK CONFIG_ATLAS_OLED_CLK_GPIO
#define PIN_DC CONFIG_ATLAS_OLED_DC_GPIO
#define PIN_CS CONFIG_ATLAS_OLED_CS_GPIO
#define PIN_RST CONFIG_ATLAS_OLED_RST_GPIO

static bool ready;
static uint8_t fb[W * H / 8];                 // page layout: byte = 8 vertical pixels, bit0 on top

static inline void level(int pin, int v) { gpio_set_level(pin, v); }

static void spi_byte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        level(PIN_CLK, 0);
        level(PIN_MOSI, (b >> i) & 1);
        level(PIN_CLK, 1);                      // the module samples on the rising edge
    }
    level(PIN_CLK, 0);
}

static void command(uint8_t c) {
    level(PIN_DC, 0);
    level(PIN_CS, 0);
    spi_byte(c);
    level(PIN_CS, 1);
}

static void command2(uint8_t c, uint8_t arg) {   // the argument of a command is sent as a command too
    command(c);
    command(arg);
}

static void flush(void) {
    if (!ready) return;
    sensors_bus_lock();                          // keep the I2C reads away from these fast pulses
    command2(0x22, 0);                           // page range 0..7 (Adafruit sends 0..0xFF, which clamps to 7)
    command(7);
    command2(0x21, 0);                           // column range 0..127
    command(W - 1);
    level(PIN_DC, 1);
    level(PIN_CS, 0);
    for (size_t i = 0; i < sizeof fb; i++) spi_byte(fb[i]);
    level(PIN_CS, 1);
    sensors_bus_unlock();
}

// A 5x7 glyph is five column bytes with bit0 at the top, which is exactly the page format: one line = one page.
static void put_line(int line, const char *text, bool invert) {
    uint8_t *row = fb + line * W;
    memset(row, invert ? 0xFF : 0x00, W);
    for (int i = 0; i < COLS && text[i]; i++) {
        for (int col = 0; col < 5; col++) {
            uint8_t g = font5x7[(uint8_t)text[i] * 5 + col];
            row[i * 6 + col] = invert ? (uint8_t)~g : g;
        }
    }
}


void display_boot_message(const char *line) {
    if (!ready) return;
    memset(fb, 0, sizeof fb);
    put_line(0, "ATLAS 3-DOF", false);
    put_line(2, line, false);
    flush();
}

static void task(void *arg) {
    for (;;) {
        closer_demo_status_t st;
        sensor_snapshot_t s;
        char problem[40], l[64];
        closer_demo_get_status(&st);
        sensors_get(&s);
        sensors_problem(problem, sizeof problem);
        memset(fb, 0, sizeof fb);

        snprintf(l, sizeof l, "ATLAS 3-DOF  v%s", closer_demo_version());
        put_line(0, l, false);
        if (st.connected) snprintf(l, sizeof l, "Wi-Fi %d dBm", st.rssi);
        else snprintf(l, sizeof l, "Wi-Fi connecting...");
        put_line(1, l, false);
        snprintf(l, sizeof l, "%s%s", st.connected ? "IP " : "", st.connected ? st.ip : "");
        put_line(2, l, false);
        if (st.updating) put_line(3, "UPDATING FIRMWARE", true);
        else put_line(3, st.note, false);
        if (problem[0]) put_line(4, problem, true);
        else { snprintf(l, sizeof l, "Sensors OK %.0f Hz", s.rate_hz); put_line(4, l, false); }
        snprintf(l, sizeof l, "Fz %7.1f N", s.axial);
        put_line(5, l, false);
        snprintf(l, sizeof l, "Yaw %.2f Pit %.2f", s.yaw, s.pitch);
        put_line(6, l, false);
        if (calib_demo_mode()) put_line(7, "DEMO MODE", true);
        else put_line(7, sensors_using_saved_calibration() ? "CALIBRATED" : "NOT CALIBRATED", !sensors_using_saved_calibration());
        flush();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void display_start(void) {
    const int pins[] = {PIN_MOSI, PIN_CLK, PIN_DC, PIN_CS, PIN_RST};
    for (size_t i = 0; i < sizeof pins / sizeof pins[0]; i++) {
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
    }
    level(PIN_CS, 1);
    level(PIN_CLK, 0);
    // Reset, as the Adafruit library does (high, low, high).
    level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(2));
    level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    // Adafruit_SSD1306::begin(SSD1306_SWITCHCAPVCC) for 128x64.
    command(0xAE);                       // display off
    command2(0xD5, 0x80);                // clock divide
    command2(0xA8, H - 1);               // multiplex ratio
    command2(0xD3, 0x00);                // display offset
    command(0x40);                       // start line 0
    command2(0x8D, 0x14);                // charge pump on (internal supply)
    command2(0x20, 0x00);                // horizontal addressing
#if CONFIG_ATLAS_OLED_ROTATE_180
    command(0xA0);                       // segment remap, turned the other way round
    command(0xC0);
#else
    command(0xA1);                       // segment remap: same orientation as the original firmware
    command(0xC8);                       // COM scan direction
#endif
    command2(0xDA, 0x12);                // COM pins for 64 rows
    command2(0x81, 0xCF);                // contrast
    command2(0xD9, 0xF1);                // pre-charge
    command2(0xDB, 0x40);                // VCOMH
    command(0xA4);                       // show the RAM contents
    command(0xA6);                       // not inverted
    command(0x2E);                       // no scrolling
    command(0xAF);                       // display on
    ready = true;
    ESP_LOGI(TAG, "OLED set up (MOSI=%d CLK=%d DC=%d CS=%d RST=%d), software SPI", PIN_MOSI, PIN_CLK, PIN_DC, PIN_CS, PIN_RST);
    memset(fb, 0, sizeof fb);
    flush();
}

// The status task reads the sensors and the Wi-Fi state, so it starts only when those exist.
void display_run(void) {
    ESP_LOGI(TAG, "status page %s", ready ? "starting" : "skipped");
    if (ready) xTaskCreate(task, "display", 4096, NULL, 2, NULL);
}
