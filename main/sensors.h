#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "calib.h"

typedef struct {
    float angle_deg[N_CH];
    float rope_mm[N_CH];
    float axial, yaw, pitch;       // N, Nm, Nm
    bool ok[N_CH];                 // the encoder answered on the last read
    uint8_t magnet[N_CH];          // 0 unknown, 1 good, 2 too weak, 3 too strong, 4 missing
    bool mux_ok;                   // the I2C multiplexer answers
    bool valid;                    // the forces can be used: all four encoders readable, or three (see degraded)
    bool degraded;                 // one encoder is out; its rope travel is estimated from the other three
    int8_t missing;                // index (0..3) of the encoder that is out, -1 if none
    float rate_hz;                 // samples per second actually achieved
    uint32_t i2c_errors;           // failed reads since start
} sensor_snapshot_t;

extern const uint8_t SENSOR_TCA_CHANNELS[N_CH];     // the multiplexer channels of the four encoders

void sensors_start(void);                           // I2C setup and the sampling task; never gives up
void sensors_get(sensor_snapshot_t *out);
void sensors_get_calibration(cal_t *out);
void sensors_set_calibration(const cal_t *c);       // takes effect immediately; save it with calib_save
void sensors_zero(void);                            // the current position becomes zero
bool sensors_using_saved_calibration(void);
// One-line description of the first thing wrong ("" when all is well).
void sensors_problem(char *out, size_t n);

// The display's pins switch quickly and can disturb the I2C wires: it holds this while it refreshes
// and the sensor task waits (no-ops before sensors_start).
void sensors_bus_lock(void);
void sensors_bus_unlock(void);
