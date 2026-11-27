#pragma once
typedef enum { ESP_RST_POWERON, ESP_RST_SW } esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason(void);
void esp_restart(void);
