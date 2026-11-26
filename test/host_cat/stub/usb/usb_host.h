#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#define USB_BM_REQUEST_TYPE_TYPE_VENDOR 0x40
#define USB_BM_REQUEST_TYPE_RECIP_INTERFACE 0x01
#define USB_BM_REQUEST_TYPE_DIR_OUT 0
typedef void *usb_host_client_handle_t;
typedef enum { USB_HOST_CLIENT_EVENT_NEW_DEV, USB_HOST_CLIENT_EVENT_DEV_REMOVED } usb_event_type_t;
typedef struct {
    usb_event_type_t event;
    struct { uint8_t address; } new_dev;
    struct { uint8_t address; } dev_removed;
} usb_host_client_event_msg_t;
typedef struct {
    int max_num_event_msg;
    struct { bool notify_dev_removed; } flags;
    struct { void (*client_event_callback)(const usb_host_client_event_msg_t *, void *); } async;
} usb_host_client_config_t;
extern uint8_t g_usb_addrs[10];
extern int g_usb_count;
static inline esp_err_t usb_host_device_addr_list_fill(int n, uint8_t *a, int *c)
{
    *c = g_usb_count < n ? g_usb_count : n;
    for (int i = 0; i < *c; ++i) a[i] = g_usb_addrs[i];
    return ESP_OK;
}
static inline esp_err_t usb_host_client_handle_events(usb_host_client_handle_t client, unsigned ticks)
{
    (void)client; (void)ticks;
    return ESP_ERR_TIMEOUT;
}
static inline esp_err_t usb_host_client_register(const usb_host_client_config_t *config, usb_host_client_handle_t *client)
{
    (void)config; *client = (void *)1;
    return ESP_OK;
}
typedef void *usb_device_handle_t;
typedef struct { uint16_t idVendor, idProduct; } usb_device_desc_t;
extern usb_device_desc_t g_usb_desc[128];
static inline esp_err_t usb_host_device_open(usb_host_client_handle_t client, uint8_t addr, usb_device_handle_t *dev)
{
    (void)client;
    *dev = (void *)(uintptr_t)addr;
    return ESP_OK;
}
static inline esp_err_t usb_host_get_device_descriptor(usb_device_handle_t dev, const usb_device_desc_t **desc)
{
    *desc = &g_usb_desc[(uintptr_t)dev];
    return ESP_OK;
}
static inline esp_err_t usb_host_device_close(usb_host_client_handle_t client, usb_device_handle_t dev)
{
    (void)client; (void)dev;
    return ESP_OK;
}
