#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_wifi_types.h"

esp_err_t nearic_network_init(bool *configured);
bool nearic_network_wait_for_address(void);

typedef struct {
    char ssid[sizeof("Nearic-000000")];
    char password[17];
    char address[16];
} nearic_setup_ap_t;

// The network task opens and closes the AP and its HTTP server. Keep route
// descriptors and user contexts valid until close succeeds. HTTP handlers
// submit requests to the network task instead of changing the radio directly.
// The controller clears the returned credentials when the session ends.
esp_err_t nearic_network_open_setup_ap(nearic_setup_ap_t *setup,
                                      const httpd_uri_t *routes, uint16_t route_count);
esp_err_t nearic_network_close_setup_ap(void);
bool nearic_network_setup_ap_ready(void);

// Run on the network task during setup. Pass the capacity in *count and
// receive up to that many visible access points, strongest signal first.
esp_err_t nearic_network_scan_setup_wifi(wifi_ap_record_t *records, uint16_t *count);

typedef enum {
    NEARIC_WIFI_TRIAL_CONNECTED,
    NEARIC_WIFI_TRIAL_CANCELLED,
    NEARIC_WIFI_TRIAL_AUTH_FAILED,
    NEARIC_WIFI_TRIAL_NETWORK_UNAVAILABLE,
    NEARIC_WIFI_TRIAL_TIMED_OUT,
    NEARIC_WIFI_TRIAL_STORAGE_ERROR,
    NEARIC_WIFI_TRIAL_STORAGE_UNCERTAIN,
    NEARIC_WIFI_TRIAL_DRIVER_ERROR,
    NEARIC_WIFI_TRIAL_RESTORE_ERROR,
} nearic_wifi_trial_result_t;

// Run on the network owner task. The caller keeps the setup session across
// the radio restart and makes cancel() readable from the network task.
// Cancellation is accepted until the verified profile starts saving.
nearic_wifi_trial_result_t nearic_network_try_wifi_settings(
    const wifi_sta_config_t *configuration, bool (*cancel)(void *), void *context);
