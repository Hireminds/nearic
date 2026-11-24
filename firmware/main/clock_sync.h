#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    const char *host;
    uint16_t port;
    uint8_t root_key[32];
    uint32_t max_radius_seconds;
    uint32_t max_round_trip_ms;
} nearic_clock_source_t;

// Call from the service owner after Wi-Fi is active. ESP_OK means verified UTC was installed.
esp_err_t nearic_clock_sync(const nearic_clock_source_t *source);
