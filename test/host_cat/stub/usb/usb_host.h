#pragma once
#include "esp_err.h"
#include <stdint.h>
#define USB_BM_REQUEST_TYPE_TYPE_VENDOR 0x40
#define USB_BM_REQUEST_TYPE_RECIP_INTERFACE 0x01
#define USB_BM_REQUEST_TYPE_DIR_OUT 0
static inline esp_err_t usb_host_device_addr_list_fill(int n, uint8_t *a, int *c){*c=0;return 0;}
