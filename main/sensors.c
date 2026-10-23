// Four AS5600 magnetic encoders (one per rope spool) behind a TCA9548A I2C multiplexer.
// Rope travel from the spool angle, then axial force and yaw/pitch torque from the spring constants.
// The maths is that of the original Arduino firmware; what is new is that nothing here ever halts:
// a missing multiplexer or encoder is reported and retried.
#include "sensors.h"

#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "sensors";

#define TCA_ADDR 0x70
#define AS5600_ADDR 0x36
#define AS5600_REG_STATUS 0x0B
#define AS5600_REG_RAW_ANGLE 0x0C
#define SPOOL_RADIUS_MM ((float)CONFIG_ATLAS_SPOOL_RADIUS_MM)
#define I2C_TIMEOUT_MS 20

const uint8_t SENSOR_TCA_CHANNELS[N_CH] = {2, 3, 4, 5};      // the original wiring (channels next to each other)

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t tca, enc;
static SemaphoreHandle_t lock;
static SemaphoreHandle_t bus_lock;          // see sensors_bus_lock()
static sensor_snapshot_t snap;
static cal_t cal;
static bool using_saved;
static uint16_t raw_now[N_CH];
static bool zero_requested;
static uint32_t consecutive_fail[N_CH];
static bool init_failed;

static esp_err_t tca_select(uint8_t ch) {
    uint8_t b = 1u << ch;
    return i2c_master_transmit(tca, &b, 1, I2C_TIMEOUT_MS);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *data, size_t n) {
    return i2c_master_transmit_receive(enc, &reg, 1, data, n, I2C_TIMEOUT_MS);
}

static esp_err_t read_angle(uint16_t *raw) {
    uint8_t d[2];
    esp_err_t e = read_reg(AS5600_REG_RAW_ANGLE, d, 2);
    if (e == ESP_OK) *raw = ((d[0] << 8) | d[1]) & 0x0FFF;
    return e;
}

// AS5600 status register: bit5 magnet detected, bit4 too weak, bit3 too strong.
static uint8_t read_magnet(void) {
    uint8_t st;
    if (read_reg(AS5600_REG_STATUS, &st, 1) != ESP_OK) return 0;
    if (!(st & 0x20)) return 4;
    if (st & 0x10) return 2;
    if (st & 0x08) return 3;
    return 1;
}

static int16_t wrapped_delta(int16_t d) {
    if (d > 2048) d -= 4096;
    if (d < -2048) d += 4096;
    return d;
}

static bool mux_probe(void) {
    return i2c_master_probe(bus, TCA_ADDR, I2C_TIMEOUT_MS) == ESP_OK;
}

static void compute(sensor_snapshot_t *s, const cal_t *c) {
    float avg = 0;
    for (int i = 0; i < N_CH; i++) avg += s->rope_mm[i];
    avg /= N_CH;
    s->axial = c->k_axial * avg;
    // Opposite spools are bundled for accuracy; the common (axial) part is subtracted.
    float ns = (fabsf(s->rope_mm[0]) + fabsf(s->rope_mm[1])) * 0.5f;
    float ew = (fabsf(s->rope_mm[2]) + fabsf(s->rope_mm[3])) * 0.5f;
    s->yaw = c->k_yaw * (ns - avg);
    s->pitch = c->k_pitch * (ew - avg);
    if (s->rope_mm[0] < s->rope_mm[1]) s->yaw = -s->yaw;
    if (s->rope_mm[2] < s->rope_mm[3]) s->pitch = -s->pitch;
}

static void take_zero(void) {
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < N_CH; i++) cal.start_angle[i] = raw_now[i];
    xSemaphoreGive(lock);
}

// Four ropes, three degrees of freedom: for a rigid plate the opposite pairs (0,1) and (2,3) move by the same total,
// r0 + r1 = r2 + r3 (both are twice the axial travel). So one missing rope follows from the other three.
static float estimate_rope(int missing, const float r[N_CH]) {
    switch (missing) {
        case 0: return r[2] + r[3] - r[1];
        case 1: return r[2] + r[3] - r[0];
        case 2: return r[0] + r[1] - r[3];
        default: return r[0] + r[1] - r[2];
    }
}

#define DEAD_AFTER_ROUNDS 50           // an encoder that has failed this many rounds in a row (0.5 s) is treated as out

static void sampler(void *arg) {
    TickType_t wake = xTaskGetTickCount();
    int64_t rate_t0 = esp_timer_get_time();
    int rate_n = 0;
    uint32_t cycle = 0;
    bool got_first = false;
    bool was_dead[N_CH] = {false};
    int64_t last_recover = 0;
    for (;;) {
        sensor_snapshot_t s;
        xSemaphoreTake(lock, portMAX_DELAY);
        s = snap;
        cal_t c = cal;
        xSemaphoreGive(lock);

        s.mux_ok = true;
        int n_ok = 0, n_dead = 0, dead_i = -1;
        xSemaphoreTake(bus_lock, portMAX_DELAY);
        for (int i = 0; i < N_CH; i++) {
            uint16_t raw = 0;
            esp_err_t e = tca_select(SENSOR_TCA_CHANNELS[i]);
            if (e == ESP_OK) {
                esp_rom_delay_us(50);
                e = read_angle(&raw);
            }
            if (e != ESP_OK) {
                s.ok[i] = false;
                s.i2c_errors++;
                consecutive_fail[i]++;
                continue;
            }
            consecutive_fail[i] = 0;
            s.ok[i] = true;
            n_ok++;
            raw_now[i] = raw;
            if (cycle % 100 == 0) s.magnet[i] = read_magnet();          // about once a second
            if (!got_first && !using_saved) c.start_angle[i] = raw;     // demo mode: where it is now is zero
            int16_t d = wrapped_delta((int16_t)raw - (int16_t)c.start_angle[i]);
            s.angle_deg[i] = d * 360.0f / 4096.0f;
            s.rope_mm[i] = s.angle_deg[i] * (float)M_PI / 180.0f * SPOOL_RADIUS_MM;
        }
        xSemaphoreGive(bus_lock);
        bool dead[N_CH];
        for (int i = 0; i < N_CH; i++) {
            dead[i] = !s.ok[i] && consecutive_fail[i] >= DEAD_AFTER_ROUNDS;
            if (dead[i]) { n_dead++; dead_i = i; }
        }
        bool all_ok = n_ok == N_CH;
        // Exactly one encoder out for good and the other three fine: carry on with an estimate for the missing one.
        bool degraded = !all_ok && n_ok == N_CH - 1 && n_dead == 1 && !s.ok[dead_i];

        // An encoder that comes back after being out: in demo mode its zero is chosen so that it agrees with the other three.
        if (!using_saved && got_first && all_ok) {
            for (int i = 0; i < N_CH; i++) {
                if (!was_dead[i]) continue;
                float est = estimate_rope(i, s.rope_mm);
                int counts = (int)lroundf(est / SPOOL_RADIUS_MM / (2.0f * (float)M_PI) * 4096.0f);
                c.start_angle[i] = (uint16_t)(((int)raw_now[i] - counts) & 0x0FFF);
                int16_t d = wrapped_delta((int16_t)raw_now[i] - (int16_t)c.start_angle[i]);
                s.angle_deg[i] = d * 360.0f / 4096.0f;
                s.rope_mm[i] = s.angle_deg[i] * (float)M_PI / 180.0f * SPOOL_RADIUS_MM;
                xSemaphoreTake(lock, portMAX_DELAY);
                cal.start_angle[i] = c.start_angle[i];
                xSemaphoreGive(lock);
                ESP_LOGI(TAG, "encoder ch%d is back; zero set to match the others", SENSOR_TCA_CHANNELS[i]);
            }
        }
        for (int i = 0; i < N_CH; i++) was_dead[i] = dead[i];

        if ((all_ok || degraded) && !got_first) {
            got_first = true;
            xSemaphoreTake(lock, portMAX_DELAY);
            cal = c;                                   // the zero taken from the first usable reading
            xSemaphoreGive(lock);
            if (degraded) ESP_LOGW(TAG, "starting with encoder ch%d out: its rope travel is estimated", SENSOR_TCA_CHANNELS[dead_i]);
        }
        if (degraded) {
            s.rope_mm[dead_i] = estimate_rope(dead_i, s.rope_mm);
            s.angle_deg[dead_i] = s.rope_mm[dead_i] / SPOOL_RADIUS_MM * 180.0f / (float)M_PI;
        }
        if (n_ok < N_CH) {
            // Is the multiplexer gone, or just an encoder? Try to bring the bus back, at most every 2 s.
            s.mux_ok = n_ok > 0 || mux_probe();
            int64_t now = esp_timer_get_time();
            if (now - last_recover > 2000000) {
                last_recover = now;
                if (!s.mux_ok) {
                    ESP_LOGW(TAG, "I2C multiplexer not answering, resetting the bus");
                    i2c_master_bus_reset(bus);
                }
            }
        }
        if (zero_requested && all_ok) {
            zero_requested = false;
            take_zero();
            continue;                                 // recompute from the new zero on the next round
        }
        s.degraded = degraded;
        s.missing = degraded ? dead_i : -1;
        s.valid = (all_ok || degraded) && got_first;
        if (got_first && s.valid) compute(&s, &c);
        if (++rate_n >= 100) {
            int64_t now = esp_timer_get_time();
            s.rate_hz = rate_n * 1e6f / (now - rate_t0);
            rate_t0 = now;
            rate_n = 0;
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        snap = s;
        xSemaphoreGive(lock);
        cycle++;
        if (cycle <= 3 || cycle % 500 == 0) {
            ESP_LOGI(TAG, "round %u: ok=%d%d%d%d mux=%d valid=%d degraded=%d raw=%u,%u,%u,%u errors=%u rate=%.0f Hz", (unsigned)cycle,
                     s.ok[0], s.ok[1], s.ok[2], s.ok[3], s.mux_ok, s.valid, s.degraded, raw_now[0], raw_now[1], raw_now[2], raw_now[3],
                     (unsigned)s.i2c_errors, s.rate_hz);
        }
        // Never spin: if the round took longer than the period (slow or failing I2C), still give other tasks a tick.
        TickType_t period = pdMS_TO_TICKS(CONFIG_ATLAS_SAMPLE_MS);
        if (period < 1) period = 1;
        if (!xTaskDelayUntil(&wake, period)) {
            wake = xTaskGetTickCount();
            vTaskDelay(1);
        }
    }
}

void sensors_start(void) {
    lock = xSemaphoreCreateMutex();
    bus_lock = xSemaphoreCreateMutex();
    calib_defaults(&cal);
    using_saved = !calib_demo_mode() && calib_load(&cal);
    if (!using_saved) calib_defaults(&cal);
    ESP_LOGI(TAG, "%s", using_saved ? "using the saved calibration" : "demo mode: zero at start-up, default spring constants");

    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_ATLAS_SDA_GPIO,
        .scl_io_num = CONFIG_ATLAS_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TCA_ADDR, .scl_speed_hz = CONFIG_ATLAS_I2C_KHZ * 1000};
    esp_err_t e = i2c_new_master_bus(&bc, &bus);
    if (e == ESP_OK) e = i2c_master_bus_add_device(bus, &dc, &tca);
    dc.device_address = AS5600_ADDR;
    if (e == ESP_OK) e = i2c_master_bus_add_device(bus, &dc, &enc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2C set-up failed: %s; no sensor readings", esp_err_to_name(e));
        init_failed = true;
        return;
    }
    ESP_LOGI(TAG, "I2C bus up (SDA=%d, SCL=%d, %d kHz)", CONFIG_ATLAS_SDA_GPIO, CONFIG_ATLAS_SCL_GPIO, CONFIG_ATLAS_I2C_KHZ);
    xTaskCreate(sampler, "sampler", 4096, NULL, 3, NULL);
}

void sensors_bus_lock(void) { if (bus_lock) xSemaphoreTake(bus_lock, portMAX_DELAY); }
void sensors_bus_unlock(void) { if (bus_lock) xSemaphoreGive(bus_lock); }

void sensors_get(sensor_snapshot_t *out) {
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = snap;
    xSemaphoreGive(lock);
}

void sensors_get_calibration(cal_t *out) {
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = cal;
    xSemaphoreGive(lock);
}

void sensors_set_calibration(const cal_t *c) {
    xSemaphoreTake(lock, portMAX_DELAY);
    cal = *c;
    using_saved = true;
    xSemaphoreGive(lock);
}

void sensors_zero(void) { zero_requested = true; }
bool sensors_using_saved_calibration(void) { return using_saved; }

void sensors_problem(char *out, size_t n) {
    sensor_snapshot_t s;
    sensors_get(&s);
    out[0] = 0;
    if (init_failed) { snprintf(out, n, "I2C set-up failed"); return; }
    if (s.rate_hz == 0 && s.i2c_errors == 0 && !s.valid) { snprintf(out, n, "starting..."); return; }
    if (!s.mux_ok) { snprintf(out, n, "I2C mux not found"); return; }
    if (s.degraded) { snprintf(out, n, "3/4 sensors: ch%d out", SENSOR_TCA_CHANNELS[s.missing]); return; }
    char list[24] = "";
    int off = 0;
    for (int i = 0; i < N_CH; i++) {
        if (!s.ok[i]) { off++; snprintf(list + strlen(list), sizeof list - strlen(list), " %d", SENSOR_TCA_CHANNELS[i]); }
    }
    if (off == N_CH) { snprintf(out, n, "mux ok, no encoders"); return; }     // the mux answers but no encoder does
    if (off) { snprintf(out, n, "Enc offline:%s", list); return; }
    static const char *const magnet[] = {"", "", "magnet too weak", "magnet too strong", "no magnet"};
    for (int i = 0; i < N_CH; i++) {
        if (s.magnet[i] >= 2) { snprintf(out, n, "ch%d %s", SENSOR_TCA_CHANNELS[i], magnet[s.magnet[i]]); return; }
    }
}
