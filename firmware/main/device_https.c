#include "device_https.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#if !CONFIG_MBEDTLS_HAVE_TIME_DATE || !CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS
#error "Nearic service requests require HTTPS with certificate date checks"
#endif

bool nearic_https_valid_path(const char *path)
{
    if (path == NULL || path[0] != '/' || path[1] == '/') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)path; *p != '\0'; ++p) {
        if (*p == '%') {
            if (!isxdigit(p[1]) || !isxdigit(p[2])) {
                return false;
            }
            p += 2;
        } else if (*p > 0x7e || (!isalnum(*p) && strchr("-._~!$&'()*+,;=:@/", *p) == NULL)) {
            return false;
        }
    }
    return true;
}

static bool valid_access_token(const char *token)
{
    if (token == NULL || token[0] == '\0') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)token; *p != '\0'; ++p) {
        if (*p < 0x21 || *p > 0x7e) {
            return false;
        }
    }
    return true;
}

static esp_err_t set_access_token(esp_http_client_handle_t http, const char *token)
{
    static const char prefix[] = "Bearer ";
    const size_t token_size = strlen(token) + 1;
    const size_t size = sizeof(prefix) - 1 + token_size;
    char *authorization = malloc(size);
    if (authorization == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(authorization, prefix, sizeof(prefix) - 1);
    memcpy(authorization + sizeof(prefix) - 1, token, token_size);
    esp_err_t error = esp_http_client_set_header(http, "Authorization", authorization);
    explicit_bzero(authorization, size);
    free(authorization);
    return error;
}

bool nearic_https_valid_origin(const nearic_update_endpoint_t *endpoint)
{
    return endpoint != NULL && endpoint->host != NULL && endpoint->host[0] != '\0' &&
           endpoint->ca_pem != NULL && endpoint->ca_pem[0] != '\0' &&
           endpoint->timeout_ms >= 0;
}

bool nearic_https_valid_endpoint(const nearic_update_endpoint_t *endpoint)
{
    return nearic_https_valid_origin(endpoint) && valid_access_token(endpoint->access_token);
}

esp_err_t nearic_https_request(const nearic_update_endpoint_t *endpoint, const char *path,
                              esp_http_client_method_t method, esp_http_client_handle_t *client)
{
    if (!nearic_https_valid_origin(endpoint) || !nearic_https_valid_path(path) ||
        (endpoint->access_token != NULL && !valid_access_token(endpoint->access_token))) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_http_client_config_t config = {
        .host = endpoint->host,
        .port = endpoint->port,
        .path = path,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem = endpoint->ca_pem,
        .method = method,
        .disable_auto_redirect = true,
        .timeout_ms = endpoint->timeout_ms,
    };
    *client = esp_http_client_init(&config);
    if (*client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (endpoint->access_token == NULL) {
        return ESP_OK;
    }
    esp_err_t error = set_access_token(*client, endpoint->access_token);
    if (error != ESP_OK) {
        esp_http_client_cleanup(*client);
        *client = NULL;
    }
    return error;
}

enum { RESPONSE_LIMIT = 4096 };

bool nearic_json_unique_keys(const cJSON *object)
{
    if (!cJSON_IsObject(object)) {
        return false;
    }
    for (const cJSON *field = object->child; field != NULL; field = field->next) {
        if (cJSON_GetObjectItemCaseSensitive(object, field->string) != field) {
            return false;
        }
    }
    return true;
}

static bool has_null_escape(const char *body)
{
    for (const char *p = body; *p != '\0'; ++p) {
        if (*p == '\\' && p[1] != '\0') {
            if (strncmp(p + 1, "u0000", 5) == 0) {
                return true;
            }
            ++p;
        }
    }
    return false;
}

static esp_err_t read_json(esp_http_client_handle_t http, cJSON **snapshot)
{
    char *body = malloc(RESPONSE_LIMIT + 1);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t size = 0;
    int count;
    do {
        count = esp_http_client_read(http, body + size, RESPONSE_LIMIT + 1 - size);
        if (count > 0) {
            size += count;
        }
    } while (count > 0 && size <= RESPONSE_LIMIT);

    esp_err_t error = ESP_ERR_INVALID_RESPONSE;
    if (size > RESPONSE_LIMIT) {
        error = ESP_ERR_INVALID_SIZE;
    } else if (count == -ESP_ERR_HTTP_EAGAIN) {
        error = ESP_ERR_TIMEOUT;
    } else if (count == 0 && esp_http_client_is_complete_data_received(http) &&
               memchr(body, '\0', size) == NULL) {
        body[size] = '\0';
        cJSON *json = has_null_escape(body) ? NULL : cJSON_ParseWithLengthOpts(body, size + 1, NULL, true);
        if (nearic_json_unique_keys(json)) {
            *snapshot = json;
            error = ESP_OK;
        } else {
            cJSON_Delete(json);
        }
    }
    explicit_bzero(body, RESPONSE_LIMIT + 1);
    free(body);
    return error;
}

esp_err_t nearic_https_post_json(const nearic_update_endpoint_t *endpoint, const char *path,
                                const char *body, int *http_status, cJSON **reply)
{
    *http_status = 0;
    *reply = NULL;
    esp_http_client_handle_t http;
    esp_err_t error = nearic_https_request(endpoint, path, HTTP_METHOD_POST, &http);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_http_client_set_header(http, "Content-Type", "application/json");
    const int length = strlen(body);
    if (error == ESP_OK) {
        error = esp_http_client_open(http, length);
    }
    for (int sent = 0; error == ESP_OK && sent < length;) {
        const int count = esp_http_client_write(http, body + sent, length - sent);
        if (count <= 0) {
            error = ESP_ERR_HTTP_WRITE_DATA;
        } else {
            sent += count;
        }
    }
    if (error == ESP_OK) {
        const int64_t declared = esp_http_client_fetch_headers(http);
        if (declared < 0) {
            error = declared == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_ERR_HTTP_FETCH_HEADER;
        } else {
            *http_status = esp_http_client_get_status_code(http);
            if (*http_status != 200 && *http_status != 201) {
                error = ESP_ERR_INVALID_RESPONSE;
            } else if (declared > RESPONSE_LIMIT) {
                error = ESP_ERR_INVALID_SIZE;
            } else {
                error = read_json(http, reply);
            }
        }
    }
    esp_http_client_cleanup(http);
    return error;
}
