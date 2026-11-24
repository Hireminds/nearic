#include "device_access.h"

#include <stdint.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

static const char *const partition = "nearic_cfg";
static const char *const access_namespace = "device_access";
static const char *const record_key = "grant";
static const char *const identity_key = "identity";

static esp_err_t open_access(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    esp_err_t error = nvs_flash_init_partition(partition);
    return error == ESP_OK ? nvs_open_from_partition(partition, access_namespace, mode, handle)
                           : error;
}

static esp_err_t read_record(const char *key, void *record, size_t capacity, size_t *size)
{
    if (record == NULL || capacity == 0 || size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *size = 0;
    nvs_handle_t handle;
    esp_err_t error = open_access(NVS_READONLY, &handle);
    if (error == ESP_OK) {
        size_t length = capacity < NEARIC_DEVICE_ACCESS_MAX_SIZE ? capacity
                                                               : NEARIC_DEVICE_ACCESS_MAX_SIZE;
        error = nvs_get_blob(handle, key, record, &length);
        nvs_close(handle);
        if (error == ESP_OK && length != 0) {
            *size = length;
        } else if (error == ESP_OK) {
            error = ESP_ERR_INVALID_SIZE;
        }
    }

    // Remove stale bytes from the previous read, even after a partial flash read.
    explicit_bzero((uint8_t *)record + *size, capacity - *size);
    return error;
}

static esp_err_t save_record(const char *key, const void *record, size_t size)
{
    if (record == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (size > NEARIC_DEVICE_ACCESS_MAX_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t handle;
    esp_err_t error = open_access(NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, key, record, size);
        if (error == ESP_OK) {
            error = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    return error;
}

esp_err_t nearic_device_access_read(void *record, size_t capacity, size_t *size)
{
    return read_record(record_key, record, capacity, size);
}

esp_err_t nearic_device_access_save(const void *record, size_t size)
{
    return save_record(record_key, record, size);
}

esp_err_t nearic_device_identity_read(void *record, size_t capacity, size_t *size)
{
    return read_record(identity_key, record, capacity, size);
}

esp_err_t nearic_device_identity_save(const void *record, size_t size)
{
    return save_record(identity_key, record, size);
}

esp_err_t nearic_device_access_clear(void)
{
    nvs_handle_t handle;
    esp_err_t error = open_access(NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_erase_key(handle, record_key);
        if (error == ESP_OK) {
            error = nvs_commit(handle);
        } else if (error == ESP_ERR_NVS_NOT_FOUND) {
            error = ESP_OK;
        }
        nvs_close(handle);
    }
    return error;
}
