#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

// These operations are serialized by the network task.
esp_err_t nearic_wifi_settings_load(wifi_sta_config_t *profile, bool *configured,
                                   bool *needs_migration);
esp_err_t nearic_wifi_settings_save(const wifi_sta_config_t *profile, bool *write_uncertain);
