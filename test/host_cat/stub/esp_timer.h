#pragma once
#include <stdint.h>
extern int64_t g_now_us;
static inline int64_t esp_timer_get_time(void){return g_now_us;}
typedef void *esp_timer_handle_t;
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
static inline int esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h)
{ (void)a; (void)h; return 0; }
static inline int esp_timer_start_periodic(esp_timer_handle_t h, uint64_t us)
{ (void)h; (void)us; return 0; }
