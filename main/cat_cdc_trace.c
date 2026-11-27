/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <stdbool.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "usb/usb_host.h"
#include "cat_cdc_trace.h"

static const char *TAG = "cat_usb";
static _Atomic(usb_transfer_cb_t) s_callback;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_probing;
static cat_cdc_probe_t s_probe;

void cat_cdc_probe_begin(void)
{
    portENTER_CRITICAL(&s_lock);
    s_probe = (cat_cdc_probe_t){0};
    s_probing = true;
    portEXIT_CRITICAL(&s_lock);
}

cat_cdc_probe_t cat_cdc_probe_end(void)
{
    portENTER_CRITICAL(&s_lock);
    const cat_cdc_probe_t snapshot = s_probe;
    s_probing = false;
    portEXIT_CRITICAL(&s_lock);
    return snapshot;
}

static void cat_cdc_in_done(usb_transfer_t *transfer)
{
    const bool completed = transfer->status == USB_TRANSFER_STATUS_COMPLETED;
    const bool failed = !completed && transfer->status != USB_TRANSFER_STATUS_CANCELED &&
                        transfer->status != USB_TRANSFER_STATUS_NO_DEVICE;
    if (completed || failed) {
        const unsigned bytes = transfer->actual_num_bytes > 0 ? (unsigned)transfer->actual_num_bytes : 0;
        portENTER_CRITICAL(&s_lock);
        if (s_probing) {
            s_probe.completed += completed;
            s_probe.short_packets += completed && transfer->actual_num_bytes < transfer->num_bytes;
            s_probe.failed += failed;
            if (completed) {
                s_probe.bytes += bytes;
                for (unsigned i = 0; i < bytes && i < transfer->data_buffer_size &&
                     s_probe.sample_len < sizeof(s_probe.sample); ++i) {
                    s_probe.sample[s_probe.sample_len++] = transfer->data_buffer[i];
                }
            }
        }
        portEXIT_CRITICAL(&s_lock);
        if (failed) {
            ESP_LOGW(TAG, "CDC IN ep=0x%02x status=%d actual=%d requested=%d",
                     transfer->bEndpointAddress, transfer->status,
                     transfer->actual_num_bytes, transfer->num_bytes);
        }
    }
    atomic_load(&s_callback)(transfer);
}

// Only the managed CDC component is redirected here. CP210x uses one bulk IN
// callback and has no CDC interrupt notification endpoint.
esp_err_t ftx_cat_cdc_submit(usb_transfer_t *transfer)
{
    if (!transfer || !(transfer->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) ||
        !transfer->callback) return usb_host_transfer_submit(transfer);

    bool traced = transfer->callback == cat_cdc_in_done;
    if (!traced) {
        usb_transfer_cb_t expected = NULL;
        traced = atomic_compare_exchange_strong(&s_callback, &expected, transfer->callback) ||
                 expected == transfer->callback;
        if (traced) transfer->callback = cat_cdc_in_done;
    }
    const esp_err_t err = usb_host_transfer_submit(transfer);
    if (traced && err != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        if (s_probing) ++s_probe.submit_failed;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "CDC IN ep=0x%02x submit failed: %s",
                 transfer->bEndpointAddress, esp_err_to_name(err));
    }
    return err;
}
