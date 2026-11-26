#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef int BaseType_t; typedef uint32_t TickType_t; typedef void *SemaphoreHandle_t, *QueueHandle_t, *TaskHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(x) (x)
static inline int xSemaphoreTake(void *s, uint32_t t){(void)s;(void)t;return 1;}
static inline int xSemaphoreGive(void *s){(void)s;return 1;}
static inline void *xSemaphoreCreateMutex(void){return (void*)1;}
static inline void xTaskNotifyGive(void *t){(void)t;}
static inline uint32_t ulTaskNotifyTake(int a, uint32_t t){
    (void)a;
#ifdef TEST_CAT_ADVANCE_TIME
    extern int64_t g_now_us;
    g_now_us += t * 1000LL;
#else
    (void)t;
#endif
    return 0;
}
static inline TickType_t xTaskGetTickCount(void){return 0;}
static inline void vTaskDelay(uint32_t t){(void)t;}
static inline void *xTaskGetCurrentTaskHandle(void){return 0;}
static inline int xQueueReceive(void *q, void *i, uint32_t t){(void)q;(void)i;(void)t;return 0;}
static inline int xQueueSend(void *q, const void *i, uint32_t t)
{
    (void)q; (void)i; (void)t;
#ifdef TEST_CAT_CAPTURE_QUEUE
    extern void (*g_queue_send_hook)(const void *);
    if (g_queue_send_hook) g_queue_send_hook(i);
#endif
    return 1;
}
static inline void *xQueueCreate(int n, int s){(void)n;(void)s;return (void*)1;}
static inline int xTaskCreate(void (*f)(void*), const char *n, int s, void *a, int p, void *h){return 1;}
static inline int xTaskCreatePinnedToCore(void (*f)(void*), const char *n, int s, void *a, int p, void *h, int c){return 1;}
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(m) do { (void)(m); } while (0)
#define taskEXIT_CRITICAL(m) do { (void)(m); } while (0)
