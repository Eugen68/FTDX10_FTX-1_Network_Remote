#pragma once
#include "esp_err.h"
#define ESP_RETURN_ON_ERROR(x, tag, msg) do{ esp_err_t e_=(x); if(e_) return e_; }while(0)
#define ESP_RETURN_ON_FALSE(c, e, tag, msg) do{ if(!(c)) return e; }while(0)
