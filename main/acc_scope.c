/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

/*
 * The FTDX10's own spectrum scope from its ACC socket.
 *
 * While its control line is answered (ACC pins 11 and 12 linked by a wire, so the radio's once-a-second
 * "alive" comes straight back), the radio clocks out one 4096-byte frame 25 times a second as SPI: CS on
 * ACC pin 6 (low for a whole frame), CLOCK on pin 5 (about 2 MHz, idle high), data on DATA2, pin 4. This
 * module takes each CS-low period as one transfer with the ESP32-S3 as an SPI slave (mode 2), checks it,
 * and keeps the latest scope row with its tuning.
 *
 * Written for this project from the published description of the frame (the Yaesu ACC Spectrum project's
 * docs/protocol.md, section "The FTDX10", measured by Eugene YO9VA); no code was taken from it.
 *
 *   Bytes       FTDX10 frame
 *   0 - 849     MAIN scope row, 850 points, low frequency first, inverted (255 - b = level)
 *   2917        scope mode (0 3DSS, 4 Center, 7 Cursor, 10 Fix)
 *   3030        span (low 4 bits: 0 = 1k ... 9 = 1M) and placement (top 2 bits: 0 Center, 1 Cursor, 2 Fix)
 *   3034-3037   passband marker, Hz, big-endian
 *   3038-3041   VFO, Hz, big-endian, and again at 3042-3045
 *   3046-3049   left edge of the span, Hz, big-endian
 *   3050 on     zero
 *
 * Wiring (each signal through 470 ohm, ground direct; ACC pins 1, 7, 8, 9, 10 and 13 never connected):
 *   ACC 5 CLOCK -> GPIO10, ACC 4 DATA2 -> GPIO11, ACC 6 CS -> GPIO13, ACC 3 DATA1 -> GPIO14 (unused),
 *   ACC 11 / 12 -> GPIO39 / GPIO38, linked by a wire at the resistors (these GPIOs stay untouched inputs).
 *
 * The pins are set up only once CAT has identified an FTDX10; on any other radio nothing here runs.
 */

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_slave.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "acc_frame.h"
#include "acc_scope.h"
#include "cat.h"

#define ACC_PIN_CLOCK     10
#define ACC_PIN_DATA2     11
#define ACC_PIN_CS        13
#define ACC_SPI_HOST      SPI2_HOST
#define ACC_SPI_MODE      2           // clock idles high; the radio changes DATA2 on the edge mode 3 samples on
#define ACC_BUF_BYTES     4160        // a little more than a frame, so a longer one shows up as longer
#define ACC_QUEUED        3
#define ACC_TASK_PRIO     12          // below the USB audio tasks (19, 20)
#define ACC_TASK_CORE     1

static const char *TAG = "acc_scope";

static SemaphoreHandle_t s_lock;
static acc_scope_row_t s_row;          // latest accepted row (s_row.seq == 0: none yet)
static acc_scope_stats_t s_stats;
static int64_t s_last_accept_ms = -1;
static int64_t s_last_cat_match_ms = -1;
static uint8_t *s_bufs[ACC_QUEUED];
static spi_slave_transaction_t s_trans[ACC_QUEUED];

static void queue_buf(int i)
{
    memset(&s_trans[i], 0, sizeof(s_trans[i]));
    s_trans[i].length = ACC_BUF_BYTES * 8;
    s_trans[i].rx_buffer = s_bufs[i];
    s_trans[i].tx_buffer = NULL;          // the slave never drives a line
    s_trans[i].user = (void *)(intptr_t)i;
    spi_slave_queue_trans(ACC_SPI_HOST, &s_trans[i], portMAX_DELAY);
}

static esp_err_t start_spi(void)
{
    for (int i = 0; i < ACC_QUEUED; i++) {
        s_bufs[i] = heap_caps_aligned_alloc(4, ACC_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_bufs[i] == NULL) {
            ESP_LOGE(TAG, "No DMA memory for the ACC buffers");
            return ESP_ERR_NO_MEM;
        }
    }
    const spi_bus_config_t bus = {
        .mosi_io_num = ACC_PIN_DATA2,
        .miso_io_num = -1,
        .sclk_io_num = ACC_PIN_CLOCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ACC_BUF_BYTES,
    };
    const spi_slave_interface_config_t slave = {
        .spics_io_num = ACC_PIN_CS,
        .queue_size = ACC_QUEUED,
        .mode = ACC_SPI_MODE,
    };
    esp_err_t err = spi_slave_initialize(ACC_SPI_HOST, &bus, &slave, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI slave init failed: %s", esp_err_to_name(err));
        return err;
    }
    for (int i = 0; i < ACC_QUEUED; i++) {
        queue_buf(i);
    }
    ESP_LOGI(TAG, "FTDX10: reading the ACC scope (CLOCK GPIO%d, DATA2 GPIO%d, CS GPIO%d, SPI mode %d)",
             ACC_PIN_CLOCK, ACC_PIN_DATA2, ACC_PIN_CS, ACC_SPI_MODE);
    return ESP_OK;
}

static void acc_task(void *arg)
{
    // Wait for an FTDX10. The pins are not touched before.
    for (;;) {
        cat_state_t st;
        cat_get_state(&st);
        if (st.connected && st.rig == CAT_RIG_FTDX10) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (start_spi() != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stats.enabled = true;
    xSemaphoreGive(s_lock);

    bool first_row = true;
    uint32_t window_accepted = 0;
    const int64_t ready_ms = esp_timer_get_time() / 1000;
    int64_t window_start_ms = ready_ms;
    acc_scope_row_t row;
    for (;;) {
        spi_slave_transaction_t *done = NULL;
        const bool got = spi_slave_get_trans_result(ACC_SPI_HOST, &done, pdMS_TO_TICKS(100)) == ESP_OK;
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (got) {
            const int i = (int)(intptr_t)done->user;
            const size_t len = done->trans_len / 8;
            cat_state_t st;
            cat_get_state(&st);
            // The radio's scope follows the active VFO (A/B), so compare with that one
            const uint32_t cat_hz = !(st.connected && st.rig == CAT_RIG_FTDX10) ? 0
                                  : st.rx_vfo_known && st.rx_vfo_sub ? st.freq_sub : st.freq_main;
            const acc_reject_t why = acc_parse_frame(s_bufs[i], len, cat_hz, now_ms, &s_last_cat_match_ms, &row);
            queue_buf(i);                    // hand the buffer back before the slower bookkeeping
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_stats.frames++;
            s_stats.last_len = (uint32_t)len;
            if (why == ACC_OK) {
                row.seq = s_row.seq + 1;
                row.ms = (uint32_t)now_ms;
                s_row = row;
                s_stats.accepted++;
                s_last_accept_ms = now_ms;
                window_accepted++;
            } else if (why == ACC_REJ_VFO_RANGE && st.connected && st.rig == CAT_RIG_FTDX10 &&
                       (st.tx_cat || st.tx_radio)) {
                // Track the overlap with TX without exposing an invalid scope row.
                s_stats.vfo_range_tx++;
            } else {
                s_stats.rejected++;
                if ((int)why < 7) {
                    s_stats.rej[why]++;
                }
                strlcpy(s_stats.reason, acc_reject_text(why), sizeof(s_stats.reason));
            }
            xSemaphoreGive(s_lock);
            if (why == ACC_OK && first_row) {
                first_row = false;
                ESP_LOGI(TAG, "First ACC frame accepted in %lld ms", (long long)(now_ms - ready_ms));
            }
        }
        if (now_ms - window_start_ms >= 1000) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_stats.fps = window_accepted;
            xSemaphoreGive(s_lock);
            window_accepted = 0;
            window_start_ms = now_ms;
        }
    }
}

esp_err_t acc_scope_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(acc_task, "acc_scope", 4096, NULL, ACC_TASK_PRIO, NULL, ACC_TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool acc_scope_get_row(acc_scope_row_t *out)
{
    if (s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool have = s_row.seq != 0;
    if (have) {
        *out = s_row;
    }
    xSemaphoreGive(s_lock);
    return have;
}

uint32_t acc_scope_seq(void)
{
    return s_row.seq;                  // a 32-bit read: atomic on this CPU
}

void acc_scope_get_stats(acc_scope_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_stats;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    out->age_ms = s_last_accept_ms < 0 ? UINT32_MAX : (uint32_t)(now_ms - s_last_accept_ms);
    if (out->age_ms > 1500) {
        out->fps = 0;                    // the window counter only updates while the task loops
    }
    xSemaphoreGive(s_lock);
}
