#include "update_download.h"

typedef struct {
    const nearic_update_endpoint_t *endpoint;
    const char *path;
    size_t expected_size;
    esp_http_client_handle_t http;
    int status;
} download_t;

static esp_err_t open_download(download_t *download)
{
    esp_err_t error = nearic_https_request(download->endpoint, download->path, HTTP_METHOD_GET,
                                           &download->http);
    if (error == ESP_OK) {
        error = esp_http_client_open(download->http, 0);
    }
    if (error != ESP_OK) {
        return error;
    }

    int64_t length = esp_http_client_fetch_headers(download->http);
    if (length < 0) {
        return length == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_ERR_HTTP_FETCH_HEADER;
    }
    download->status = esp_http_client_get_status_code(download->http);
    if (download->status != 200) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!esp_http_client_is_chunked_response(download->http) && length != (int64_t)download->expected_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t read_download(void *context, uint8_t *buffer, size_t capacity, size_t *received)
{
    download_t *download = context;
    if (download->http == NULL) {
        esp_err_t error = open_download(download);
        if (error != ESP_OK) {
            return error;
        }
    }
    int count = esp_http_client_read(download->http, (char *)buffer, (int)capacity);
    if (count < 0) {
        return count == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    if (count == 0 && !esp_http_client_is_complete_data_received(download->http)) {
        return ESP_ERR_HTTP_INCOMPLETE_DATA;
    }
    *received = (size_t)count;
    return ESP_OK;
}

esp_err_t nearic_update_download(const nearic_update_endpoint_t *endpoint, const char *path,
                                 const nearic_update_image_t *image, esp_partition_subtype_t *slot,
                                 int *http_status)
{
    if (http_status != NULL) {
        *http_status = 0;
    }
    if (!nearic_https_valid_endpoint(endpoint) || !nearic_https_valid_path(path) || image == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    download_t download = {.endpoint = endpoint, .path = path, .expected_size = image->size};
    esp_err_t error = nearic_update_stage(image, read_download, &download, slot);
    if (download.http != NULL) {
        esp_http_client_cleanup(download.http);
    }
    if (http_status != NULL) {
        *http_status = download.status;
    }
    return error;
}
