#pragma once

#include "esp_err.h"

esp_err_t nearic_display_init(const char *version);
esp_err_t nearic_display_status(const char *status);
