#include <string.h>
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef void *cdc_acm_dev_hdl_t;
typedef enum { CDC_ACM_HOST_DEVICE_DISCONNECTED = 1 } cdc_evt_t;
typedef struct { cdc_evt_t type; } cdc_acm_host_dev_event_data_t;
typedef struct { uint16_t vid, pid; uint8_t interface_idx, dev_addr; uint32_t connection_timeout_ms; size_t out_buffer_size, in_buffer_size;
  void (*event_cb)(const cdc_acm_host_dev_event_data_t*, void*); bool (*data_cb)(const uint8_t*, size_t, void*); void *user_arg; } cdc_acm_host_open_config_t;
typedef struct { int driver_task_stack_size, driver_task_priority, xCoreID; void (*new_dev_cb)(void *); } cdc_acm_host_driver_config_t;
#define CDC_HOST_ANY_PID 0
extern char g_sent[1024];
extern void (*g_cdc_send_hook)(const char *, size_t);
extern uint32_t g_write_timeout_ms;
extern int64_t g_now_us, g_write_duration_us;
extern esp_err_t g_write_result;
static inline esp_err_t cdc_acm_host_data_tx_blocking(cdc_acm_dev_hdl_t d, const uint8_t *b, size_t n, uint32_t t)
{
    g_write_timeout_ms = t;
    g_now_us += g_write_duration_us;
    size_t l = strlen(g_sent);
    if (l + n < sizeof(g_sent)) {
        memcpy(g_sent + l, b, n);
        g_sent[l + n] = 0;
    }
    if (g_cdc_send_hook) g_cdc_send_hook((const char *)b, n);
    return g_write_result;
}
extern esp_err_t (*g_cdc_open_hook)(const cdc_acm_host_open_config_t *, cdc_acm_dev_hdl_t *);
extern esp_err_t (*g_cdc_close_hook)(cdc_acm_dev_hdl_t);
extern esp_err_t (*g_cdc_request_hook)(cdc_acm_dev_hdl_t, uint8_t, uint16_t, uint8_t *);
static inline esp_err_t cdc_acm_host_open(const cdc_acm_host_open_config_t *c, cdc_acm_dev_hdl_t *d)
{
    return g_cdc_open_hook ? g_cdc_open_hook(c, d) : ESP_ERR_NOT_FOUND;
}
static inline esp_err_t cdc_acm_host_close(cdc_acm_dev_hdl_t d)
{
    return g_cdc_close_hook ? g_cdc_close_hook(d) : ESP_OK;
}
static inline esp_err_t cdc_acm_host_install(const cdc_acm_host_driver_config_t *c){return 0;}
static inline esp_err_t cdc_acm_host_send_custom_request(cdc_acm_dev_hdl_t d, uint8_t a, uint8_t b, uint16_t c, uint16_t e, uint16_t f, uint8_t *g)
{
    (void)a; (void)c; (void)e;
    return g_cdc_request_hook ? g_cdc_request_hook(d, b, f, g) : ESP_OK;
}
