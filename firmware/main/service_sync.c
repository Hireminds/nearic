#include "service_sync.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"

static const char sync_path[] = "/device/v1/sync";
static const char hex[] = "0123456789abcdef";

typedef struct {
    bool pending;
    uint8_t sha256[NEARIC_UPDATE_SHA256_SIZE];
} rollback_report_t;

static bool add_json(cJSON *parent, const char *name, cJSON *child)
{
    if (cJSON_AddItemToObject(parent, name, child)) {
        return true;
    }
    cJSON_Delete(child);
    return false;
}

static cJSON *make_rollback(const rollback_report_t *report)
{
    if (!report->pending) {
        return cJSON_CreateNull();
    }
    char sha[NEARIC_UPDATE_SHA256_SIZE * 2 + 1];
    for (size_t i = 0; i < sizeof(report->sha256); ++i) {
        sha[i * 2] = hex[report->sha256[i] >> 4];
        sha[i * 2 + 1] = hex[report->sha256[i] & 15];
    }
    sha[sizeof(sha) - 1] = '\0';
    cJSON *json = cJSON_CreateObject();
    if (!cJSON_AddStringToObject(json, "sha256", sha) ||
        !cJSON_AddStringToObject(json, "outcome", "rolled_back")) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static char *make_request(const nearic_service_sync_input_t *input, const rollback_report_t *report)
{
    cJSON *json = cJSON_CreateObject();
    char *body = NULL;
    if (cJSON_AddStringToObject(json, "model", "esp32-s3-box-3b") &&
        cJSON_AddStringToObject(json, "firmwareVersion", esp_app_get_description()->version) &&
        add_json(json, "capabilities", input->capability_count == 0 ? cJSON_CreateArray() :
                 cJSON_CreateStringArray(input->capabilities, input->capability_count)) &&
        add_json(json, "result", input->operation_result == NULL ? cJSON_CreateNull() :
                 cJSON_Duplicate(input->operation_result, true)) &&
        add_json(json, "otaResult", make_rollback(report)) &&
        (input->renew_registration_code == NULL ||
         cJSON_AddStringToObject(json, "renewRegistrationCode", input->renew_registration_code))) {
        body = cJSON_PrintUnformatted(json);
    }
    cJSON_Delete(json);
    return body;
}

static esp_err_t post_sync(const nearic_update_endpoint_t *endpoint, const char *body,
                           nearic_service_sync_output_t *output)
{
    esp_err_t error = nearic_https_post_json(endpoint, sync_path, body, &output->http_status,
                                            &output->snapshot);
    if (error != ESP_OK) {
        return error;
    }

    cJSON *json = output->snapshot;
    const char *registration = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "registrationStatus"));
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(json, "command");
    if (output->http_status != 200 || registration == NULL ||
        (strcmp(registration, "pending") != 0 && strcmp(registration, "registered") != 0) ||
        (command != NULL && !cJSON_IsNull(command) && !nearic_json_unique_keys(command))) {
        cJSON_Delete(json);
        output->snapshot = NULL;
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static bool decode_sha256(const char *encoded, uint8_t sha[NEARIC_UPDATE_SHA256_SIZE])
{
    if (encoded == NULL || strlen(encoded) != NEARIC_UPDATE_SHA256_SIZE * 2) {
        return false;
    }
    for (size_t i = 0; i < NEARIC_UPDATE_SHA256_SIZE; ++i) {
        const char *high = strchr(hex, encoded[i * 2]);
        const char *low = strchr(hex, encoded[i * 2 + 1]);
        if (low == NULL || high == NULL) {
            return false;
        }
        sha[i] = (high - hex) * 16 + (low - hex);
    }
    return true;
}

static bool read_offer(const cJSON *offer, nearic_update_image_t *image, const char **path)
{
    if (!nearic_json_unique_keys(offer)) {
        return false;
    }
    image->version = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(offer, "version"));
    *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(offer, "path"));
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(offer, "size");
    if (image->version == NULL || strlen(image->version) == 0 ||
        strlen(image->version) >= sizeof(esp_app_get_description()->version) ||
        !nearic_https_valid_path(*path) || !cJSON_IsNumber(size) || !(size->valuedouble >= 1) ||
        size->valuedouble > UINT32_MAX) {
        return false;
    }
    image->size = (size_t)size->valuedouble;
    return image->size == size->valuedouble && decode_sha256(
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(offer, "sha256")), image->sha256);
}

static esp_err_t discard_offer(nearic_service_ota_t *ota)
{
    if (!ota->staged) {
        return ESP_OK;
    }
    esp_err_t error = nearic_update_discard();
    if (error == ESP_OK) {
        ota->staged = false;
    }
    return error;
}

static esp_err_t apply_update(nearic_service_ota_t *ota, const nearic_update_endpoint_t *endpoint,
                              const nearic_service_sync_input_t *input,
                              const rollback_report_t *report, nearic_service_sync_output_t *output)
{
    if (report->pending) {
        uint8_t ack[NEARIC_UPDATE_SHA256_SIZE];
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(output->snapshot, "acknowledgedOtaSha256");
        if (!decode_sha256(cJSON_GetStringValue(item), ack) || memcmp(ack, report->sha256, sizeof(ack)) != 0) {
            return ESP_OK;
        }
        return nearic_update_acknowledge_rollback(ack);
    }

    const cJSON *offer = cJSON_GetObjectItemCaseSensitive(output->snapshot, "update");
    if (offer == NULL || cJSON_IsNull(offer)) {
        return discard_offer(ota);
    }
    nearic_update_image_t image;
    const char *path;
    if (!read_offer(offer, &image, &path)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (ota->staged && (ota->size != image.size || strcmp(ota->version, image.version) != 0 ||
                        memcmp(ota->sha256, image.sha256, sizeof(ota->sha256)) != 0)) {
        esp_err_t error = discard_offer(ota);
        if (error != ESP_OK) {
            return error;
        }
    }

    const cJSON *command = cJSON_GetObjectItemCaseSensitive(output->snapshot, "command");
    if ((command != NULL && !cJSON_IsNull(command)) || input->operation_result != NULL ||
        input->can_update == NULL || !input->can_update(input->context)) {
        return ESP_OK;
    }
    if (ota->staged) {
        const esp_err_t error = nearic_update_activate();
        if (error == ESP_OK) {
            ota->staged = false;
            output->reboot_ready = true;
        }
        return error;
    }

    esp_partition_subtype_t slot;
    const esp_err_t error = nearic_update_download(endpoint, path, &image, &slot, &output->file_status);
    if (error == ESP_OK) {
        ota->staged = true;
        ota->size = image.size;
        strcpy(ota->version, image.version);
        memcpy(ota->sha256, image.sha256, sizeof(ota->sha256));
    }
    return error;
}

esp_err_t nearic_service_sync(nearic_service_ota_t *ota, const nearic_update_endpoint_t *endpoint,
                              const nearic_service_sync_input_t *input,
                              nearic_service_sync_output_t *output)
{
    if (output == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *output = (nearic_service_sync_output_t){0};
    if (ota == NULL || !nearic_https_valid_endpoint(endpoint) || input == NULL ||
        input->capability_count < 0 || (input->capability_count > 0 && input->capabilities == NULL) ||
        (input->operation_result != NULL && !cJSON_IsObject(input->operation_result))) {
        return ESP_ERR_INVALID_ARG;
    }

    rollback_report_t report = {0};
    const esp_err_t journal = nearic_update_read_rollback(report.sha256);
    report.pending = journal == ESP_OK;
    if (journal != ESP_ERR_NOT_FOUND && journal != ESP_OK) {
        output->ota_error = journal;
    }
    char *body = make_request(input, &report);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t error = post_sync(endpoint, body, output);
    cJSON_free(body);
    if (output->http_status == 401 || output->http_status == 403) {
        output->ota_error = discard_offer(ota);
    } else if (error == ESP_OK && output->ota_error == ESP_OK) {
        output->ota_error = apply_update(ota, endpoint, input, &report, output);
    }
    return error;
}
