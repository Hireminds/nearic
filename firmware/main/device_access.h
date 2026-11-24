#pragma once

#include <stddef.h>

#include "esp_err.h"

#define NEARIC_DEVICE_ACCESS_MAX_SIZE 4096U

// The service task owns and serializes access to this record. The caller
// defines its format and verifies service responses before saving it.
// With valid arguments, a failed read clears the buffer and sets *size to zero.
esp_err_t nearic_device_access_read(void *record, size_t capacity, size_t *size);
esp_err_t nearic_device_access_save(const void *record, size_t size);
// Clear the local copy after a confirmed revocation. Repeated clears succeed.
esp_err_t nearic_device_access_clear(void);

// The local identity survives a cleared grant and OTA. Both operations use the service owner.
esp_err_t nearic_device_identity_read(void *record, size_t capacity, size_t *size);
esp_err_t nearic_device_identity_save(const void *record, size_t size);
