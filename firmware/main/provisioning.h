#pragma once

#include "network.h"

typedef enum {
    NEARIC_SETUP_IDLE,
    NEARIC_SETUP_OPENING,
    NEARIC_SETUP_SCANNING,
    NEARIC_SETUP_READY,
    NEARIC_SETUP_CONNECTING,
    NEARIC_SETUP_RETRY,
    NEARIC_SETUP_CLOSING,
    NEARIC_SETUP_CONNECTED,
    NEARIC_SETUP_CANCELLED,
    NEARIC_SETUP_EXPIRED,
    NEARIC_SETUP_ERROR,
} nearic_setup_phase_t;

typedef struct {
    nearic_setup_phase_t phase;
    nearic_setup_ap_t ap;
    bool has_trial_result;
    nearic_wifi_trial_result_t trial_result;
} nearic_setup_status_t;

// Both calls run on the network owner task. Poll while setup is active,
// including after a close error. Starting assumes the network is initialized.
esp_err_t nearic_provisioning_start(void);
esp_err_t nearic_provisioning_poll(void);

// Callable from other tasks. Status contains local AP credentials while setup
// is active. Keep it on the BOX and clear caller copies after use.
void nearic_provisioning_get_status(nearic_setup_status_t *status);
void nearic_provisioning_cancel(void);
