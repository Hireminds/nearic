#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_http_client.h"

typedef struct {
    const char *host;
    uint16_t port;
    const char *ca_pem;
    const char *access_token;
    int timeout_ms;
} nearic_update_endpoint_t;

bool nearic_https_valid_path(const char *path);
bool nearic_https_valid_origin(const nearic_update_endpoint_t *endpoint);
bool nearic_https_valid_endpoint(const nearic_update_endpoint_t *endpoint);
bool nearic_json_unique_keys(const cJSON *object);

// Initialize one request to the configured origin after the device has verified UTC.
esp_err_t nearic_https_request(const nearic_update_endpoint_t *endpoint, const char *path,
                              esp_http_client_method_t method, esp_http_client_handle_t *client);

// Read a bounded JSON response from HTTP 200 or 201. The caller owns the returned object.
esp_err_t nearic_https_post_json(const nearic_update_endpoint_t *endpoint, const char *path,
                                const char *body, int *http_status, cJSON **reply);
