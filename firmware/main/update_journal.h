#pragma once

#include "update.h"

typedef enum {
    NEARIC_ROLLBACK_NONE = 0,
    NEARIC_ROLLBACK_PENDING = 1,
    NEARIC_ROLLBACK_ACKNOWLEDGED = 2,
} nearic_rollback_state_t;

// All fields are bytes in the same NVS blob. Update operations serialize access.
typedef struct {
    uint8_t format_version;
    uint8_t source_slot;
    uint8_t target_slot;
    uint8_t rollback;
    uint8_t attempt_sha256[NEARIC_UPDATE_SHA256_SIZE];
    uint8_t rejected_sha256[NEARIC_UPDATE_SHA256_SIZE];
} nearic_update_journal_t;

esp_err_t nearic_update_journal_load(nearic_update_journal_t *journal);
esp_err_t nearic_update_journal_store(const nearic_update_journal_t *journal);
