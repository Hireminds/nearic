#include "device_bootstrap.h"

#include <stdio.h>
#include <string.h>

#include "device_access.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

enum { IDENTITY_FORMAT = 1, GRANT_FORMAT = 1, CHALLENGE_ID_SIZE = 16, CHALLENGE_SIZE = 32 };
static const char transcript_domain[] = "nearic-box-bootstrap-v1";

typedef struct {
    uint8_t version;
    nearic_device_grant_t grant;
} grant_record_t;

static int random_bytes(void *context, unsigned char *output, size_t size)
{
    (void)context;
    esp_fill_random(output, size);
    return 0;
}

static bool encode_url(const uint8_t *input, size_t size, char *out, size_t capacity)
{
    size_t written = 0;
    if (mbedtls_base64_encode((unsigned char *)out, capacity, &written, input, size) != 0) {
        return false;
    }
    for (size_t i = 0; i < written; ++i) {
        if (out[i] == '+') {
            out[i] = '-';
        } else if (out[i] == '/') {
            out[i] = '_';
        }
    }
    while (written > 0 && out[written - 1] == '=') {
        --written;
    }
    out[written] = '\0';
    return true;
}

static bool decode_url(const char *input, uint8_t *output, size_t size)
{
    const size_t length = (size * 8 + 5) / 6;
    if (input == NULL || strlen(input) != length) {
        return false;
    }
    char padded[45];
    char canonical[45];
    for (size_t i = 0; i < length; ++i) {
        const char c = input[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            padded[i] = c;
        } else if (c == '-' || c == '_') {
            padded[i] = c == '-' ? '+' : '/';
        } else {
            return false;
        }
    }
    size_t end = length;
    while (end % 4 != 0) {
        padded[end++] = '=';
    }
    size_t decoded = 0;
    const bool valid = mbedtls_base64_decode(output, size, &decoded, (unsigned char *)padded, end) == 0 &&
                       decoded == size && encode_url(output, size, canonical, sizeof(canonical)) &&
                       strcmp(input, canonical) == 0;
    explicit_bzero(padded, sizeof(padded));
    explicit_bzero(canonical, sizeof(canonical));
    return valid;
}

static bool valid_grant(const nearic_device_grant_t *grant)
{
    uint8_t raw[32];
    const bool valid = grant->device_id[sizeof(grant->device_id) - 1] == '\0' &&
                       grant->access_token[sizeof(grant->access_token) - 1] == '\0' &&
                       decode_url(grant->device_id, raw, 16) && decode_url(grant->access_token, raw, sizeof(raw));
    explicit_bzero(raw, sizeof(raw));
    return valid;
}

static esp_err_t load_grant(nearic_device_grant_t *grant)
{
    grant_record_t record;
    size_t size;
    esp_err_t error = nearic_device_access_read(&record, sizeof(record), &size);
    if (error == ESP_OK) {
        if (size == sizeof(record) && record.version == GRANT_FORMAT && valid_grant(&record.grant)) {
            *grant = record.grant;
        } else {
            error = ESP_ERR_INVALID_STATE;
        }
    }
    explicit_bzero(&record, sizeof(record));
    return error;
}

static esp_err_t load_or_create_identity(mbedtls_pk_context *key)
{
    uint8_t blob[257];
    size_t size;
    esp_err_t error = nearic_device_identity_read(blob, sizeof(blob), &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        if (mbedtls_pk_setup(key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
            mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*key), random_bytes, NULL) != 0) {
            error = ESP_FAIL;
        } else {
            const int length = mbedtls_pk_write_key_der(key, blob + 1, sizeof(blob) - 1);
            if (length > 0) {
                memmove(blob + 1, blob + sizeof(blob) - length, length);
                blob[0] = IDENTITY_FORMAT;
                error = nearic_device_identity_save(blob, 1 + length);
            } else {
                error = ESP_FAIL;
            }
        }
    } else if (error == ESP_OK && (size < 2 || blob[0] != IDENTITY_FORMAT ||
               mbedtls_pk_parse_key(key, blob + 1, size - 1, NULL, 0, random_bytes, NULL) != 0 ||
               mbedtls_pk_get_type(key) != MBEDTLS_PK_ECKEY ||
               mbedtls_ecp_keypair_get_group_id(mbedtls_pk_ec(*key)) != MBEDTLS_ECP_DP_SECP256R1)) {
        error = ESP_ERR_INVALID_STATE;
    }
    explicit_bzero(blob, sizeof(blob));
    return error;
}

static bool make_proof(mbedtls_pk_context *key, const cJSON *challenge, const char *id,
                       const uint8_t *spki, size_t spki_size, const uint8_t *enrollment,
                       char *proof, size_t proof_size)
{
    const size_t domain_size = sizeof(transcript_domain) - 1;
    uint8_t transcript[sizeof(transcript_domain) - 1 + CHALLENGE_ID_SIZE + CHALLENGE_SIZE + 64];
    memcpy(transcript, transcript_domain, domain_size);
    uint8_t *challenge_input = transcript + domain_size + CHALLENGE_ID_SIZE;
    uint8_t *digests = challenge_input + CHALLENGE_SIZE;
    uint8_t hash[32];
    uint8_t signature[72];
    size_t signature_size = 0;
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(challenge, "challenge"));
    bool ok = decode_url(id, transcript + domain_size, CHALLENGE_ID_SIZE) &&
              decode_url(value, challenge_input, CHALLENGE_SIZE) &&
              mbedtls_sha256(spki, spki_size, digests, 0) == 0 &&
              mbedtls_sha256(enrollment, NEARIC_ENROLLMENT_SIZE, digests + 32, 0) == 0 &&
              mbedtls_sha256(transcript, sizeof(transcript), hash, 0) == 0 &&
              mbedtls_pk_sign(key, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature, sizeof(signature),
                              &signature_size, random_bytes, NULL) == 0 &&
              encode_url(signature, signature_size, proof, proof_size);
    explicit_bzero(transcript, sizeof(transcript));
    explicit_bzero(hash, sizeof(hash));
    explicit_bzero(signature, sizeof(signature));
    return ok;
}

static esp_err_t save_grant(cJSON *reply, nearic_device_grant_t *grant)
{
    grant_record_t record = {.version = GRANT_FORMAT};
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "deviceId"));
    char *token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "accessToken"));
    esp_err_t error = ESP_ERR_INVALID_RESPONSE;
    if (id != NULL && strlen(id) == sizeof(record.grant.device_id) - 1 &&
        token != NULL && strlen(token) == sizeof(record.grant.access_token) - 1) {
        strcpy(record.grant.device_id, id);
        strcpy(record.grant.access_token, token);
        if (valid_grant(&record.grant)) {
            error = nearic_device_access_save(&record, sizeof(record));
            if (error == ESP_OK) {
                *grant = record.grant;
            }
        }
    }
    if (token != NULL) {
        explicit_bzero(token, strlen(token));
    }
    explicit_bzero(&record, sizeof(record));
    return error;
}

static esp_err_t request_grant(const nearic_update_endpoint_t *origin, mbedtls_pk_context *key,
                               const uint8_t *enrollment, nearic_device_grant_t *grant, int *http_status)
{
    uint8_t der[160];
    int length = mbedtls_pk_write_pubkey_der(key, der, sizeof(der));
    if (length <= 0) {
        return ESP_FAIL;
    }
    const uint8_t *spki = der + sizeof(der) - length;
    char public_key[128];
    char ticket[45];
    if (!encode_url(spki, length, public_key, sizeof(public_key)) ||
        !encode_url(enrollment, NEARIC_ENROLLMENT_SIZE, ticket, sizeof(ticket))) {
        return ESP_FAIL;
    }

    cJSON *reply = NULL;
    char payload[400];
    char proof[100];
    snprintf(payload, sizeof(payload), "{\"enrollment\":\"%s\",\"publicKey\":\"%s\"}", ticket, public_key);
    esp_err_t error = nearic_https_post_json(origin, "/device/v1/bootstrap/challenge", payload, http_status, &reply);
    if (error == ESP_OK) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "challengeId"));
        if (*http_status != 200 || !make_proof(key, reply, id, spki, length, enrollment, proof, sizeof(proof))) {
            error = ESP_ERR_INVALID_RESPONSE;
        } else {
            snprintf(payload, sizeof(payload),
                     "{\"enrollment\":\"%s\",\"publicKey\":\"%s\",\"challengeId\":\"%s\",\"proof\":\"%s\"}",
                     ticket, public_key, id, proof);
            cJSON_Delete(reply);
            error = nearic_https_post_json(origin, "/device/v1/bootstrap", payload, http_status, &reply);
            if (error == ESP_OK) {
                error = save_grant(reply, grant);
            }
        }
    }
    cJSON_Delete(reply);
    explicit_bzero(payload, sizeof(payload));
    explicit_bzero(ticket, sizeof(ticket));
    explicit_bzero(proof, sizeof(proof));
    return error;
}

esp_err_t nearic_device_get_access(const nearic_update_endpoint_t *endpoint,
                                   const uint8_t enrollment[NEARIC_ENROLLMENT_SIZE],
                                   nearic_device_grant_t *grant, int *http_status)
{
    if (grant == NULL || http_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    explicit_bzero(grant, sizeof(*grant));
    *http_status = 0;
    esp_err_t error = load_grant(grant);
    if (error != ESP_ERR_NVS_NOT_FOUND || enrollment == NULL) {
        return error;
    }
    if (!nearic_https_valid_origin(endpoint)) {
        return ESP_ERR_INVALID_ARG;
    }
    nearic_update_endpoint_t origin = *endpoint;
    origin.access_token = NULL;
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    error = load_or_create_identity(&key);
    if (error == ESP_OK) {
        error = request_grant(&origin, &key, enrollment, grant, http_status);
    }
    mbedtls_pk_free(&key);
    return error;
}
