#pragma once

#include "cJSON.h"
#include "update_download.h"

typedef struct {
    bool staged;
    size_t size;
    char version[32];
    uint8_t sha256[NEARIC_UPDATE_SHA256_SIZE];
} nearic_service_ota_t;

typedef struct {
    const char *const *capabilities;
    int capability_count;
    const cJSON *operation_result;
    const char *renew_registration_code;
    bool (*can_update)(void *context);
    void *context;
} nearic_service_sync_input_t;

typedef struct {
    cJSON *snapshot;
    int http_status;
    int file_status;
    esp_err_t ota_error;
    bool reboot_ready;
} nearic_service_sync_output_t;

// Use one owner for the endpoint, pending operation, HTTPS requests and staged image.
// On success the caller owns snapshot and decides how to present it and when to restart.
esp_err_t nearic_service_sync(nearic_service_ota_t *ota, const nearic_update_endpoint_t *endpoint,
                              const nearic_service_sync_input_t *input,
                              nearic_service_sync_output_t *output);
