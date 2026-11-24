/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
// This file is compiled into the UAC library, but calls the original ring APIs.
#undef xRingbufferCreate
#undef vRingbufferDelete
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "usb_audio.h"

// The managed UAC driver uses these hooks for its audio staging rings. USB DMA
// transfer buffers remain under the driver's allocator.
RingbufHandle_t ftx_uac_ring_create(size_t size, RingbufferType_t type)
{
    if (size != USB_AUDIO_TX_RING_BYTES || type != RINGBUF_TYPE_BYTEBUF) {
        return xRingbufferCreate(size, type);
    }

    StaticRingbuffer_t *control = heap_caps_malloc(sizeof(*control), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    RingbufHandle_t ring = control && data ? xRingbufferCreateStatic(size, type, data, control) : NULL;
    if (!ring) {
        heap_caps_free(data);
        heap_caps_free(control);
        ESP_LOGW("usb_audio", "Could not allocate TX ring in PSRAM");
    } else {
        ESP_LOGI("usb_audio", "TX ring: %u bytes in PSRAM", (unsigned)size);
    }
    return ring;
}

void ftx_uac_ring_delete(RingbufHandle_t ring)
{
    uint8_t *data;
    StaticRingbuffer_t *control;
    const bool custom = xRingbufferGetStaticBuffer(ring, &data, &control) == pdTRUE;
    vRingbufferDelete(ring);
    if (custom) {
        heap_caps_free(data);
        heap_caps_free(control);
    }
}
