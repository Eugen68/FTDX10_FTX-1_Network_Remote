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
typedef struct { int driver_task_stack_size, driver_task_priority, xCoreID; void *new_dev_cb; } cdc_acm_host_driver_config_t;
#define CDC_HOST_ANY_PID 0
extern char g_sent[1024];
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
    return g_write_result;
}
static inline esp_err_t cdc_acm_host_open(const cdc_acm_host_open_config_t *c, cdc_acm_dev_hdl_t *d){return -1;}
static inline esp_err_t cdc_acm_host_close(cdc_acm_dev_hdl_t d){return 0;}
static inline esp_err_t cdc_acm_host_install(const cdc_acm_host_driver_config_t *c){return 0;}
static inline esp_err_t cdc_acm_host_send_custom_request(cdc_acm_dev_hdl_t d, uint8_t a, uint8_t b, uint16_t c, uint16_t e, uint16_t f, uint8_t *g){return 0;}
