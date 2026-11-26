#pragma once
#include "esp_err.h"
#include <stdint.h>
#define USB_BM_REQUEST_TYPE_TYPE_VENDOR 0x40
#define USB_BM_REQUEST_TYPE_RECIP_INTERFACE 0x01
#define USB_BM_REQUEST_TYPE_DIR_OUT 0
static inline esp_err_t usb_host_device_addr_list_fill(int n, uint8_t *a, int *c){*c=0;return 0;}

typedef void *usb_device_handle_t;
typedef struct { uint8_t dev_addr; } usb_device_info_t;
static inline esp_err_t usb_host_device_info(usb_device_handle_t d, usb_device_info_t *info)
{
    info->dev_addr = (uint8_t)(uintptr_t)d;
    return ESP_OK;
}
