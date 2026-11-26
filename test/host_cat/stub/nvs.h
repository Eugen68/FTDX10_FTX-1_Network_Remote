#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int nvs_handle_t;
enum { NVS_READONLY, NVS_READWRITE };
static inline esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h)
{ (void)ns; (void)mode; (void)h; return ESP_FAIL; }
static inline esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *out)
{ (void)h; (void)k; (void)out; return ESP_FAIL; }
static inline esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t value)
{ (void)h; (void)k; (void)value; return ESP_FAIL; }
static inline esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static inline void nvs_close(nvs_handle_t h) { (void)h; }
