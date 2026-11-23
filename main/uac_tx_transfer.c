/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#undef usb_host_transfer_submit
#include <stdatomic.h>
#include "usb/usb_host.h"
#include "usb_audio.h"

static _Atomic(usb_transfer_cb_t) s_uac_tx_callback;
static atomic_uint s_sent_bytes, s_completed_packets, s_failed_packets, s_pcm_peak;
static atomic_int s_submit_error;

static void tx_transfer_done(usb_transfer_t *transfer)
{
    unsigned bytes = 0, completed = 0, failed = 0;
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        const int16_t *pcm = (const int16_t *)transfer->data_buffer;
        unsigned peak = atomic_load(&s_pcm_peak);
        for (int i = 0; i < transfer->num_isoc_packets; ++i) {
            const usb_isoc_packet_desc_t *packet = &transfer->isoc_packet_desc[i];
            if (packet->status == USB_TRANSFER_STATUS_COMPLETED &&
                packet->actual_num_bytes == packet->num_bytes) {
                bytes += packet->actual_num_bytes;
                ++completed;
                // Verify that completed OUT packets contain a signal. Inspect
                // samples only until the first non-silent packet per tone.
                if (peak == 0) {
                    for (int j = 0; j < packet->actual_num_bytes / (int)sizeof(*pcm); ++j) {
                        const int sample = pcm[j];
                        const unsigned magnitude = sample < 0 ? -sample : sample;
                        if (magnitude > peak) {
                            peak = magnitude;
                        }
                    }
                    atomic_store(&s_pcm_peak, peak);
                }
            } else {
                ++failed;
            }
            pcm += packet->num_bytes / (int)sizeof(*pcm);
        }
    } else if (transfer->status != USB_TRANSFER_STATUS_CANCELED &&
               transfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        failed = transfer->num_isoc_packets;
    }
    atomic_fetch_add(&s_sent_bytes, bytes);
    atomic_fetch_add(&s_completed_packets, completed);
    atomic_fetch_add(&s_failed_packets, failed);
    atomic_load(&s_uac_tx_callback)(transfer);
}

// This hook is used only by the managed UAC component. The driver's single
// isochronous OUT completion callback can resubmit a transfer from inside itself.
esp_err_t ftx_uac_transfer_submit(usb_transfer_t *transfer)
{
    if (!transfer || transfer->num_isoc_packets <= 0 ||
        (transfer->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK)) {
        return usb_host_transfer_submit(transfer);
    }

    if (transfer->callback != tx_transfer_done) {
        usb_transfer_cb_t expected = NULL;
        if (!transfer->callback ||
            (!atomic_compare_exchange_strong(&s_uac_tx_callback, &expected, transfer->callback) &&
             expected != transfer->callback)) {
            atomic_store(&s_submit_error, ESP_ERR_NOT_SUPPORTED);
            return ESP_ERR_NOT_SUPPORTED;
        }
        transfer->callback = tx_transfer_done;
    }
    const esp_err_t err = usb_host_transfer_submit(transfer);
    if (err != ESP_OK) {
        atomic_store(&s_submit_error, err);
    }
    return err;
}

void usb_audio_tx_reset_transfer_stats(void)
{
    atomic_store(&s_sent_bytes, 0);
    atomic_store(&s_completed_packets, 0);
    atomic_store(&s_failed_packets, 0);
    atomic_store(&s_pcm_peak, 0);
    atomic_store(&s_submit_error, ESP_OK);
}

usb_tx_transfer_stats_t usb_audio_tx_transfer_stats(void)
{
    return (usb_tx_transfer_stats_t){
        .completed_bytes = atomic_load(&s_sent_bytes),
        .completed_packets = atomic_load(&s_completed_packets),
        .failed_packets = atomic_load(&s_failed_packets),
        .pcm_peak = atomic_load(&s_pcm_peak),
        .submit_error = atomic_load(&s_submit_error),
    };
}
