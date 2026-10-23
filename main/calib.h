#pragma once
#include <stdbool.h>
#include <stdint.h>

#define N_CH 4

typedef struct {
    float k_axial;                 // N per mm of average rope travel
    float k_yaw, k_pitch;          // Nm per mm
    uint16_t start_angle[N_CH];    // raw encoder angle (0..4095) of the unloaded position
} cal_t;

// Spring constants used when there is no calibration (or in demo mode): from the original firmware.
void calib_defaults(cal_t *c);

// Saved calibration; false if there is none.
bool calib_load(cal_t *c);
bool calib_save(const cal_t *c);

// Demo mode: ignore the saved calibration and take the position at start-up (or when asked) as zero.
// This is what the original firmware did all the time; kept as the default.
bool calib_demo_mode(void);
void calib_set_demo_mode(bool demo);
