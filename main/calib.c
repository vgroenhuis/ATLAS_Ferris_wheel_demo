#include "calib.h"

#include "nvs.h"

#define NS "atlas"

void calib_defaults(cal_t *c) {
    c->k_axial = -24.4201f;
    c->k_yaw = 0.3129f;
    c->k_pitch = 0.3129f;
    for (int i = 0; i < N_CH; i++) c->start_angle[i] = 0;
}

bool calib_load(cal_t *c) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = sizeof *c;
    cal_t tmp;
    bool ok = nvs_get_blob(h, "cal", &tmp, &n) == ESP_OK && n == sizeof tmp;
    nvs_close(h);
    if (ok) *c = tmp;
    return ok;
}

bool calib_save(const cal_t *c) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "cal", c, sizeof *c) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool calib_demo_mode(void) {
    nvs_handle_t h;
    uint8_t v = 1;                                  // demo mode unless someone switched it off
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "demo", &v);
        nvs_close(h);
    }
    return v != 0;
}

void calib_set_demo_mode(bool demo) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "demo", demo ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}
