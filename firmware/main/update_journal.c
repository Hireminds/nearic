#include "update_journal.h"

#include <stdbool.h>
#include <string.h>

#include "esp_ota_ops.h"
#include "nvs.h"
#include "nvs_flash.h"

enum { JOURNAL_FORMAT = 1 };

static const char *const partition = "nearic_cfg";
static const char *const journal_namespace = "ota_journal";
static const char *const journal_key = "state";

static bool valid_slot(uint8_t subtype)
{
    return subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX;
}

static esp_err_t open_journal(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    esp_err_t error = nvs_flash_init_partition(partition);
    return error == ESP_OK ? nvs_open_from_partition(partition, journal_namespace, mode, handle) : error;
}

esp_err_t nearic_update_journal_store(const nearic_update_journal_t *journal)
{
    nvs_handle_t handle;
    esp_err_t error = open_journal(NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, journal_key, journal, sizeof(*journal));
        if (error == ESP_OK) {
            error = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    return error;
}

static esp_err_t reconcile_attempt(nearic_update_journal_t *journal)
{
    const esp_partition_t *target = esp_partition_find_first(ESP_PARTITION_TYPE_APP, journal->target_slot, NULL);
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (target == NULL || running == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_ota_img_states_t state;
    esp_err_t error = esp_ota_get_state_partition(target, &state);
    if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) {
        return error;
    }
    if (running == target) {
        if (error != ESP_OK || (state != ESP_OTA_IMG_VALID && state != ESP_OTA_IMG_PENDING_VERIFY)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            return ESP_OK;
        }
    } else if (running->subtype != journal->source_slot) {
        return ESP_ERR_INVALID_STATE;
    } else if (error == ESP_OK) {
        if (state == ESP_OTA_IMG_NEW && esp_ota_get_boot_partition() == target) {
            return ESP_OK;
        }
        if (state != ESP_OTA_IMG_ABORTED && state != ESP_OTA_IMG_INVALID) {
            return ESP_ERR_INVALID_STATE;
        }
        journal->rollback = NEARIC_ROLLBACK_PENDING;
        memcpy(journal->rejected_sha256, journal->attempt_sha256, sizeof(journal->rejected_sha256));
    } else if (esp_ota_get_boot_partition() != running) {
        return ESP_ERR_INVALID_STATE;
    }

    // The new image is confirmed, the rollback is known, or the slot was never selected.
    journal->source_slot = 0;
    journal->target_slot = 0;
    memset(journal->attempt_sha256, 0, sizeof(journal->attempt_sha256));
    return nearic_update_journal_store(journal);
}

esp_err_t nearic_update_journal_load(nearic_update_journal_t *journal)
{
    nvs_handle_t handle;
    size_t size = sizeof(*journal);
    esp_err_t error = open_journal(NVS_READONLY, &handle);
    if (error == ESP_OK) {
        error = nvs_get_blob(handle, journal_key, journal, &size);
        nvs_close(handle);
    }
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        *journal = (nearic_update_journal_t){.format_version = JOURNAL_FORMAT};
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    if (size != sizeof(*journal) || journal->format_version != JOURNAL_FORMAT ||
        journal->rollback > NEARIC_ROLLBACK_ACKNOWLEDGED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (journal->target_slot == 0) {
        return journal->source_slot == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (!valid_slot(journal->target_slot) || !valid_slot(journal->source_slot) ||
        journal->target_slot == journal->source_slot || journal->rollback == NEARIC_ROLLBACK_PENDING) {
        return ESP_ERR_INVALID_STATE;
    }
    return reconcile_attempt(journal);
}
