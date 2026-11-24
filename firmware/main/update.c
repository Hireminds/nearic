#include "update.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_secure_boot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"
#include "update_journal.h"

#if !CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT || !CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME
#error "Nearic OTA requires RSA signed app verification on update"
#endif

enum { CHUNK_SIZE = 1024, SIGNATURE_ALIGNMENT = 4096 };

static portMUX_TYPE update_lock = portMUX_INITIALIZER_UNLOCKED;
static bool update_busy;
static const esp_partition_t *staged_partition;
static uint8_t staged_sha256[NEARIC_UPDATE_SHA256_SIZE];

static bool start_action(void)
{
    portENTER_CRITICAL(&update_lock);
    bool acquired = !update_busy;
    update_busy = true;
    portEXIT_CRITICAL(&update_lock);
    return acquired;
}

static void finish_action(void)
{
    portENTER_CRITICAL(&update_lock);
    update_busy = false;
    portEXIT_CRITICAL(&update_lock);
}

static esp_err_t write_image(const esp_partition_t *target, const nearic_update_image_t *image,
                             nearic_update_read_t read, void *context, esp_ota_handle_t *ota)
{
    uint8_t *buffer = malloc(CHUNK_SIZE);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    esp_err_t error = mbedtls_sha256_starts(&hash, 0) == 0 ? ESP_OK : ESP_FAIL;
    size_t remaining = image->size;
    while (error == ESP_OK && remaining != 0) {
        size_t capacity = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;
        size_t received = 0;
        error = read(context, buffer, capacity, &received);
        if (error != ESP_OK) {
            break;
        }
        if (received == 0 || received > capacity) {
            error = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (mbedtls_sha256_update(&hash, buffer, received) != 0) {
            error = ESP_FAIL;
            break;
        }
        // Wait for the first bytes before erasing the inactive slot.
        if (*ota == 0) {
            error = esp_ota_begin(target, image->size, ota);
        }
        if (error == ESP_OK) {
            error = esp_ota_write(*ota, buffer, received);
        }
        remaining -= received;
    }

    if (error == ESP_OK) {
        size_t extra = 0;
        error = read(context, buffer, 1, &extra);
        if (error == ESP_OK && extra != 0) {
            error = ESP_ERR_INVALID_SIZE;
        }
    }
    if (error == ESP_OK) {
        uint8_t digest[sizeof(image->sha256)];
        if (mbedtls_sha256_finish(&hash, digest) != 0) {
            error = ESP_FAIL;
        } else if (memcmp(digest, image->sha256, sizeof(digest)) != 0) {
            error = ESP_ERR_INVALID_CRC;
        }
    }
    mbedtls_sha256_free(&hash);
    free(buffer);
    return error;
}

static esp_err_t validate_image_metadata(const esp_partition_t *target,
                                         const nearic_update_image_t *image)
{
    // The offered size limits image parsing even if flash past the download holds an older image.
    const esp_partition_pos_t downloaded = {.offset = target->address, .size = image->size};
    esp_image_metadata_t metadata;
    esp_err_t error = esp_image_get_metadata(&downloaded, &metadata);
    if (error != ESP_OK) {
        return error;
    }
    const size_t padded_size = (metadata.image_len + SIGNATURE_ALIGNMENT - 1) &
                               ~(size_t)(SIGNATURE_ALIGNMENT - 1);
    if (padded_size + sizeof(ets_secure_boot_signature_t) != image->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_app_desc_t description;
    error = esp_ota_get_partition_description(target, &description);
    if (error == ESP_OK && strncmp(description.version, image->version, sizeof(description.version)) != 0) {
        error = ESP_ERR_INVALID_VERSION;
    }
    return error;
}

static esp_err_t stage_image(const nearic_update_image_t *image, nearic_update_read_t read,
                             void *context, esp_partition_subtype_t *slot)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (staged_partition != NULL || esp_ota_get_boot_partition() != running) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(running);
    if (target == NULL || target == running) {
        return ESP_ERR_NOT_FOUND;
    }
    if (image->size > target->size || image->size % SIGNATURE_ALIGNMENT != 0 ||
        image->size <= sizeof(ets_secure_boot_signature_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    nearic_update_journal_t journal;
    esp_err_t error = nearic_update_journal_load(&journal);
    if (error != ESP_OK) {
        return error;
    }
    if (journal.target_slot != 0 || journal.rollback == NEARIC_ROLLBACK_PENDING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (journal.rollback == NEARIC_ROLLBACK_ACKNOWLEDGED &&
        memcmp(journal.rejected_sha256, image->sha256, sizeof(journal.rejected_sha256)) == 0) {
        return ESP_ERR_NOT_ALLOWED;
    }

    esp_ota_handle_t ota = 0;
    error = write_image(target, image, read, context, &ota);
    if (error == ESP_OK) {
        error = validate_image_metadata(target, image);
    }
    if (error != ESP_OK) {
        if (ota != 0) {
            esp_ota_abort(ota);
        }
        return error;
    }

    error = esp_ota_end(ota);
    if (error == ESP_OK) {
        staged_partition = target;
        memcpy(staged_sha256, image->sha256, sizeof(staged_sha256));
        *slot = target->subtype;
    }
    return error;
}

esp_err_t nearic_update_stage(const nearic_update_image_t *image, nearic_update_read_t read,
                              void *context, esp_partition_subtype_t *slot)
{
    if (image == NULL || image->version == NULL || image->version[0] == '\0' || read == NULL || slot == NULL ||
        strnlen(image->version, sizeof(esp_app_get_description()->version)) == sizeof(esp_app_get_description()->version)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!start_action()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = stage_image(image, read, context, slot);
    finish_action();
    return error;
}

static esp_err_t activate_image(void)
{
    if (staged_partition == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    nearic_update_journal_t journal;
    esp_err_t error = nearic_update_journal_load(&journal);
    if (error != ESP_OK) {
        return error;
    }
    if (boot == staged_partition && journal.target_slot == staged_partition->subtype &&
        journal.source_slot == running->subtype &&
        memcmp(journal.attempt_sha256, staged_sha256, sizeof(staged_sha256)) == 0) {
        // A previous call selected the slot before returning a flash error.
        return ESP_OK;
    }
    if (boot != running || journal.target_slot != 0 || journal.rollback == NEARIC_ROLLBACK_PENDING) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_ota_img_states_t state;
    error = esp_ota_get_state_partition(staged_partition, &state);
    if (error != ESP_ERR_NOT_FOUND) {
        return error == ESP_OK ? ESP_ERR_INVALID_STATE : error;
    }
    journal.source_slot = running->subtype;
    journal.target_slot = staged_partition->subtype;
    memcpy(journal.attempt_sha256, staged_sha256, sizeof(staged_sha256));
    error = nearic_update_journal_store(&journal);
    return error == ESP_OK ? esp_ota_set_boot_partition(staged_partition) : error;
}

esp_err_t nearic_update_activate(void)
{
    if (!start_action()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = activate_image();
    if (error == ESP_OK) {
        staged_partition = NULL;
    }
    finish_action();
    return error;
}

esp_err_t nearic_update_discard(void)
{
    if (!start_action()) {
        return ESP_ERR_INVALID_STATE;
    }
    staged_partition = NULL;
    finish_action();
    return ESP_OK;
}

esp_err_t nearic_update_read_rollback(uint8_t sha256[NEARIC_UPDATE_SHA256_SIZE])
{
    if (!start_action()) {
        return ESP_ERR_INVALID_STATE;
    }
    nearic_update_journal_t journal;
    esp_err_t error = nearic_update_journal_load(&journal);
    if (error == ESP_OK) {
        if (journal.rollback != NEARIC_ROLLBACK_PENDING) {
            error = ESP_ERR_NOT_FOUND;
        } else if (sha256 != NULL) {
            memcpy(sha256, journal.rejected_sha256, sizeof(journal.rejected_sha256));
        }
    }
    finish_action();
    return error;
}

esp_err_t nearic_update_acknowledge_rollback(const uint8_t sha256[NEARIC_UPDATE_SHA256_SIZE])
{
    if (sha256 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!start_action()) {
        return ESP_ERR_INVALID_STATE;
    }
    nearic_update_journal_t journal;
    esp_err_t error = nearic_update_journal_load(&journal);
    if (error == ESP_OK) {
        if (journal.rollback == NEARIC_ROLLBACK_NONE ||
            memcmp(journal.rejected_sha256, sha256, sizeof(journal.rejected_sha256)) != 0) {
            error = ESP_ERR_NOT_FOUND;
        } else if (journal.rollback == NEARIC_ROLLBACK_PENDING) {
            journal.rollback = NEARIC_ROLLBACK_ACKNOWLEDGED;
            error = nearic_update_journal_store(&journal);
        }
    }
    finish_action();
    return error;
}
