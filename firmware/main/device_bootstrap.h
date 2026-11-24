#pragma once

#include "device_https.h"

enum { NEARIC_ENROLLMENT_SIZE = 32 };

typedef struct {
    char device_id[23];
    char access_token[44];
} nearic_device_grant_t;

// Use with trusted UTC, an active Wi-Fi radio and one service owner.
// A saved grant is returned without network access or an enrollment secret.
// A fresh exchange commits the identity before sending a challenge and the grant before returning.
esp_err_t nearic_device_get_access(const nearic_update_endpoint_t *endpoint,
                                   const uint8_t enrollment[NEARIC_ENROLLMENT_SIZE],
                                   nearic_device_grant_t *grant, int *http_status);
