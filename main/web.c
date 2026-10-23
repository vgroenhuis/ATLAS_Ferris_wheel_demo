// Web server: the page and the game (embedded, gzipped), /data for them, and a small control API.
//   WS   /ws                   the same values pushed 50 times a second
//   GET  /data                 sensor values (the format the original pages use, plus "valid", "demo", "hz")
//   GET  /api/state            firmware, mode, calibration, problems
//   POST /api/zero             the current position becomes zero
//   POST /api/mode?demo=0|1    demo mode (zero at start-up) or the saved calibration; stored in flash
//   GET/POST /api/calibration  read / set {"k_axial","k_yaw","k_pitch","start_angle":[4]}; POST also saves it
#include "web.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#include "calib.h"
#include "cJSON.h"
#include "closer_demo.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "sensors.h"
#include "web_table.h"

static const char *TAG = "web";

// ---------------------------------------------------------------- clients (for the report)

#define MAX_CLIENTS 16
static struct { uint32_t ip; int64_t last_us; } seen[MAX_CLIENTS];

static void touch_client(httpd_req_t *req) {
    struct sockaddr_in6 addr;
    socklen_t len = sizeof addr;
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) != 0) return;
    uint32_t ip = addr.sin6_family == AF_INET6 ? addr.sin6_addr.un.u32_addr[3] : ((struct sockaddr_in *)&addr)->sin_addr.s_addr;
    int64_t now = esp_timer_get_time(), oldest = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (seen[i].ip == ip) { seen[i].last_us = now; return; }
        if (seen[i].last_us < seen[oldest].last_us) oldest = i;
    }
    seen[oldest].ip = ip;
    seen[oldest].last_us = now;
}

static int active_clients(void) {
    int64_t now = esp_timer_get_time();
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) n += seen[i].ip && now - seen[i].last_us < 60 * 1000000LL;
    return n;
}

// ---------------------------------------------------------------- static files

static esp_err_t static_get(httpd_req_t *req) {
    const char *uri = req->uri;
    char path[64];
    size_t n = strcspn(uri, "?");
    if (n >= sizeof path) return httpd_resp_send_404(req);
    memcpy(path, uri, n);
    path[n] = 0;
    if (strcmp(path, "/") == 0) strcpy(path, "/index.html");
    for (size_t i = 0; i < sizeof WEB_FILES / sizeof WEB_FILES[0]; i++) {
        if (strcmp(WEB_FILES[i].path, path) == 0) {
            if (strcmp(path, "/index.html") == 0 || strcmp(path, "/game.html") == 0) touch_client(req);
            httpd_resp_set_type(req, WEB_FILES[i].type);
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
            httpd_resp_set_hdr(req, "Cache-Control", strstr(path, "vendor") || strstr(path, ".min.") || strstr(path, ".jpg") ? "public, max-age=3600" : "no-cache");
            return httpd_resp_send(req, (const char *)WEB_FILES[i].start, WEB_FILES[i].end - WEB_FILES[i].start);
        }
    }
    return httpd_resp_send_404(req);
}

// ---------------------------------------------------------------- data

static int build_data_json(char *buf, size_t cap) {
    sensor_snapshot_t s;
    sensors_get(&s);
    int n = snprintf(buf, cap, "{\"channels\":{");
    for (int i = 0; i < N_CH && n < (int)cap; i++) {
        n += snprintf(buf + n, cap - n, "%s\"ch%d\":{\"angle\":%.2f,\"rope\":%.2f}", i ? "," : "", SENSOR_TCA_CHANNELS[i],
                      s.angle_deg[i], s.rope_mm[i]);
    }
    if (n < (int)cap) {
        n += snprintf(buf + n, cap - n, "},\"forces\":{\"axial\":%.2f,\"yaw\":%.2f,\"pitch\":%.2f},\"valid\":%s,\"degraded\":%s,\"missing\":%d,\"demo\":%s,\"hz\":%.0f}",
                      s.axial, s.yaw, s.pitch, s.valid ? "true" : "false", s.degraded ? "true" : "false",
                      s.degraded ? SENSOR_TCA_CHANNELS[s.missing] : 0, calib_demo_mode() ? "true" : "false", s.rate_hz);
    }
    return n < (int)cap ? n : (int)cap - 1;
}

static esp_err_t data_get(httpd_req_t *req) {
    touch_client(req);
    char buf[640];
    int n = build_data_json(buf, sizeof buf);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

// ---------------------------------------------------------------- live stream (WebSocket)
// The page used to ask for /data a few times a second. Now the board pushes every reading at 50 Hz to whoever is connected.

#define MAX_WS_CLIENTS 4
#define STREAM_PERIOD_MS 20
static httpd_handle_t server;
static int ws_fds[MAX_WS_CLIENTS] = {-1, -1, -1, -1};
static SemaphoreHandle_t ws_lock;

static void ws_add(int fd) {
    xSemaphoreTake(ws_lock, portMAX_DELAY);
    bool known = false;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) known |= ws_fds[i] == fd;
    for (int i = 0; i < MAX_WS_CLIENTS && !known; i++) {
        if (ws_fds[i] < 0) { ws_fds[i] = fd; known = true; }
    }
    if (!known) httpd_sess_trigger_close(server, fd);          // too many viewers: the page falls back to polling
    xSemaphoreGive(ws_lock);
}

// The server answers the WebSocket handshake itself and calls this only for frames the page sends. The page sends one
// message when its socket opens ("stream"); that is what registers it as a viewer.
static esp_err_t ws_handler(httpd_req_t *req) {
    httpd_ws_frame_t frame = {0};
    uint8_t buf[32];
    frame.payload = buf;
    esp_err_t e = httpd_ws_recv_frame(req, &frame, sizeof buf);
    if (e != ESP_OK) return e;
    touch_client(req);
    ws_add(httpd_req_to_sockfd(req));
    return ESP_OK;
}

static void stream_task(void *arg) {
    char buf[640];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STREAM_PERIOD_MS));
        xSemaphoreTake(ws_lock, portMAX_DELAY);
        int fds[MAX_WS_CLIENTS];
        int count = 0;
        for (int i = 0; i < MAX_WS_CLIENTS; i++) if (ws_fds[i] >= 0) fds[count++] = ws_fds[i];
        xSemaphoreGive(ws_lock);
        if (!count) continue;
        int n = build_data_json(buf, sizeof buf);
        httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)buf, .len = n};
        for (int k = 0; k < count; k++) {
            bool alive = httpd_ws_get_fd_info(server, fds[k]) == HTTPD_WS_CLIENT_WEBSOCKET &&
                         httpd_ws_send_frame_async(server, fds[k], &frame) == ESP_OK;
            if (!alive) {
                xSemaphoreTake(ws_lock, portMAX_DELAY);
                for (int i = 0; i < MAX_WS_CLIENTS; i++) if (ws_fds[i] == fds[k]) ws_fds[i] = -1;
                xSemaphoreGive(ws_lock);
            }
        }
    }
}

static esp_err_t send_json(httpd_req_t *req, cJSON *j) {
    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!out) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(req, out);
    free(out);
    return e;
}

static esp_err_t state_get(httpd_req_t *req) {
    sensor_snapshot_t s;
    sensors_get(&s);
    char problem[40];
    sensors_problem(problem, sizeof problem);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "fw", closer_demo_version());
    cJSON_AddBoolToObject(j, "demo", calib_demo_mode());
    cJSON_AddBoolToObject(j, "calibrated", sensors_using_saved_calibration());
    cJSON_AddBoolToObject(j, "valid", s.valid);
    cJSON_AddBoolToObject(j, "degraded", s.degraded);
    cJSON_AddBoolToObject(j, "mux_ok", s.mux_ok);
    cJSON *oks = cJSON_AddArrayToObject(j, "encoders_ok");          // channels 2,3,4,5 in order
    for (int i = 0; i < N_CH; i++) cJSON_AddItemToArray(oks, cJSON_CreateBool(s.ok[i]));
    cJSON_AddStringToObject(j, "problem", problem);
    cJSON_AddNumberToObject(j, "hz", s.rate_hz);
    cJSON_AddNumberToObject(j, "i2c_errors", s.i2c_errors);
    return send_json(req, j);
}

static esp_err_t zero_post(httpd_req_t *req) {
    sensor_snapshot_t s;
    sensors_get(&s);
    if (!s.valid || s.degraded) {         // zero needs all four encoders, otherwise it would be wrong
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "all four sensors must be readable to set the zero");
    }
    sensors_zero();
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t mode_post(httpd_req_t *req) {
    char q[32], v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK || httpd_query_key_value(q, "demo", v, sizeof v) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "use ?demo=0 or ?demo=1");
    }
    calib_set_demo_mode(v[0] != '0');
    httpd_resp_sendstr(req, "ok, restarting");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();                                    // the mode is applied at start-up
    return ESP_OK;
}

static cJSON *cal_to_json(const cal_t *c) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "k_axial", c->k_axial);
    cJSON_AddNumberToObject(j, "k_yaw", c->k_yaw);
    cJSON_AddNumberToObject(j, "k_pitch", c->k_pitch);
    cJSON *a = cJSON_AddArrayToObject(j, "start_angle");
    for (int i = 0; i < N_CH; i++) cJSON_AddItemToArray(a, cJSON_CreateNumber(c->start_angle[i]));
    return j;
}

static esp_err_t cal_get(httpd_req_t *req) {
    cal_t c;
    sensors_get_calibration(&c);
    return send_json(req, cal_to_json(&c));
}

static esp_err_t cal_post(httpd_req_t *req) {
    char body[256];
    if (req->content_len <= 0 || req->content_len >= (int)sizeof body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = 0;
    cJSON *j = cJSON_Parse(body);
    cal_t c;
    sensors_get_calibration(&c);
    cJSON *ka = cJSON_GetObjectItem(j, "k_axial"), *ky = cJSON_GetObjectItem(j, "k_yaw"), *kp = cJSON_GetObjectItem(j, "k_pitch");
    cJSON *sa = cJSON_GetObjectItem(j, "start_angle");
    bool ok = cJSON_IsNumber(ka) && cJSON_IsNumber(ky) && cJSON_IsNumber(kp) && cJSON_IsArray(sa) && cJSON_GetArraySize(sa) == N_CH;
    if (ok) {
        c.k_axial = ka->valuedouble;
        c.k_yaw = ky->valuedouble;
        c.k_pitch = kp->valuedouble;
        for (int i = 0; i < N_CH; i++) {
            cJSON *v = cJSON_GetArrayItem(sa, i);
            if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 4095) ok = false;
            else c.start_angle[i] = (uint16_t)v->valuedouble;
        }
    }
    cJSON_Delete(j);
    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need k_axial, k_yaw, k_pitch and start_angle[4] (0..4095)");
    if (!calib_save(&c)) return httpd_resp_send_500(req);
    sensors_set_calibration(&c);
    return httpd_resp_sendstr(req, calib_demo_mode() ? "saved; demo mode is on, so it is used after POST /api/mode?demo=0" : "saved and in use");
}

// ---------------------------------------------------------------- start

static void stats_task(void *arg) {
    for (;;) {
        sensor_snapshot_t s;
        sensors_get(&s);
        char problem[40];
        sensors_problem(problem, sizeof problem);
        closer_demo_set_clients(active_clients());
        closer_demo_set_stat("axial_n", s.axial);
        closer_demo_set_stat("yaw_nm", s.yaw);
        closer_demo_set_stat("pitch_nm", s.pitch);
        closer_demo_set_stat("rate_hz", s.rate_hz);
        closer_demo_set_stat("i2c_errors", s.i2c_errors);
        closer_demo_set_stat("sensors_ok", s.valid ? 1 : 0);
        closer_demo_set_stat("encoders_ok", s.ok[0] + s.ok[1] + s.ok[2] + s.ok[3]);
        if (problem[0] && strcmp(problem, "starting...") != 0) closer_demo_set_message(problem);
        else if (!problem[0]) closer_demo_set_message("");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void web_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 6144;
    httpd_handle_t srv;
    cfg.send_wait_timeout = 2;                                  // a stalled viewer must not hold the stream up for long
    ws_lock = xSemaphoreCreateMutex();
    esp_err_t started = httpd_start(&srv, &cfg);
    server = srv;
    if (started != ESP_OK) {
        ESP_LOGE(TAG, "web server did not start: %s", esp_err_to_name(started));   // carry on: sensors, display and reports still work
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/data", .method = HTTP_GET, .handler = data_get},
        {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true},
        {.uri = "/api/state", .method = HTTP_GET, .handler = state_get},
        {.uri = "/api/zero", .method = HTTP_POST, .handler = zero_post},
        {.uri = "/api/mode", .method = HTTP_POST, .handler = mode_post},
        {.uri = "/api/calibration", .method = HTTP_GET, .handler = cal_get},
        {.uri = "/api/calibration", .method = HTTP_POST, .handler = cal_post},
        {.uri = "/*", .method = HTTP_GET, .handler = static_get},      // last: the page files
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        if (httpd_register_uri_handler(srv, &uris[i]) != ESP_OK) ESP_LOGE(TAG, "could not register %s", uris[i].uri);
    }
    xTaskCreate(stats_task, "stats", 4096, NULL, 2, NULL);
    xTaskCreate(stream_task, "stream", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "web server up");
}
