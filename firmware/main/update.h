#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"

enum { NEARIC_UPDATE_SHA256_SIZE = 32 };

typedef struct {
    const char *version;
    size_t size;
    uint8_t sha256[NEARIC_UPDATE_SHA256_SIZE];
} nearic_update_image_t;

// Return at most capacity bytes, using zero bytes only at clean end of stream.
typedef esp_err_t (*nearic_update_read_t)(void *context, uint8_t *buffer, size_t capacity,
                                         size_t *received);

// Verify a complete signed image in the inactive slot without selecting it for boot.
esp_err_t nearic_update_stage(const nearic_update_image_t *image, nearic_update_read_t read,
                              void *context, esp_partition_subtype_t *slot);

// Call when activation is safe. Records the attempt before changing the boot partition.
// The caller owns the restart.
esp_err_t nearic_update_activate(void);

// Release an unactivated candidate, for example when the update owner loses access.
esp_err_t nearic_update_discard(void);

// Reconcile the journal with the boot state. ESP_OK means a rollback needs to be sent.
// Pass NULL to reconcile without reading the hash. ESP_ERR_NOT_FOUND means no report.
esp_err_t nearic_update_read_rollback(uint8_t sha256[NEARIC_UPDATE_SHA256_SIZE]);

// Accept only the digest returned in an authenticated acknowledgement for this BOX.
esp_err_t nearic_update_acknowledge_rollback(const uint8_t sha256[NEARIC_UPDATE_SHA256_SIZE]);
