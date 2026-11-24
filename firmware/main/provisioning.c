#include "provisioning.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define SETUP_DURATION_US INT64_C(600000000)
#define MAX_SETUP_NETWORKS 16

typedef struct {
    uint32_t id;
    uint8_t ssid[sizeof(((wifi_ap_record_t *)0)->ssid)];
    int8_t rssi;
    wifi_auth_mode_t authmode;
} setup_network_t;

static portMUX_TYPE setup_lock = portMUX_INITIALIZER_UNLOCKED;
static nearic_setup_status_t session;
static int64_t deadline;
static bool cancelled;
static nearic_setup_phase_t after_close;
static wifi_sta_config_t pending;
static setup_network_t networks[MAX_SETUP_NETWORKS];
static uint16_t network_count;
static uint32_t last_network_id;
// ESP-IDF's complete scan records are larger than the data shown in setup.
// Only the owner task uses this buffer.
static wifi_ap_record_t scan_records[MAX_SETUP_NETWORKS];

static bool active(nearic_setup_phase_t phase)
{
    return phase >= NEARIC_SETUP_OPENING && phase <= NEARIC_SETUP_CLOSING;
}

static nearic_setup_phase_t close_requested(int64_t now)
{
    if (cancelled) {
        return NEARIC_SETUP_CANCELLED;
    }
    if (now >= deadline) {
        return NEARIC_SETUP_EXPIRED;
    }
    return NEARIC_SETUP_IDLE;
}

void nearic_provisioning_get_status(nearic_setup_status_t *status)
{
    portENTER_CRITICAL(&setup_lock);
    *status = session;
    portEXIT_CRITICAL(&setup_lock);
}

void nearic_provisioning_cancel(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&setup_lock);
    if (active(session.phase) && session.phase != NEARIC_SETUP_CLOSING &&
        (session.phase == NEARIC_SETUP_OPENING || now < deadline)) {
        cancelled = true;
    }
    portEXIT_CRITICAL(&setup_lock);
}

static void begin_close(nearic_setup_phase_t terminal)
{
    portENTER_CRITICAL(&setup_lock);
    session.phase = NEARIC_SETUP_CLOSING;
    after_close = terminal;
    explicit_bzero(&session.ap, sizeof(session.ap));
    explicit_bzero(&pending, sizeof(pending));
    explicit_bzero(networks, sizeof(networks));
    network_count = 0;
    portEXIT_CRITICAL(&setup_lock);
}

static esp_err_t close_setup(void)
{
    esp_err_t error = nearic_network_close_setup_ap();
    if (error == ESP_OK) {
        portENTER_CRITICAL(&setup_lock);
        session.phase = after_close;
        portEXIT_CRITICAL(&setup_lock);
    }
    return error;
}

static nearic_setup_phase_t pending_close(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&setup_lock);
    nearic_setup_phase_t terminal = close_requested(now);
    portEXIT_CRITICAL(&setup_lock);
    return terminal;
}

static bool cancel_trial(void *context)
{
    (void)context;
    return pending_close() != NEARIC_SETUP_IDLE;
}

static bool seen_ssid(const uint8_t *ssid)
{
    for (uint16_t i = 0; i < network_count; ++i) {
        if (strncmp((const char *)networks[i].ssid, (const char *)ssid,
                    sizeof(pending.ssid)) == 0) {
            return true;
        }
    }
    return false;
}

static void scan_networks(void)
{
    uint16_t found = MAX_SETUP_NETWORKS;
    esp_err_t error = nearic_network_scan_setup_wifi(scan_records, &found);
    portENTER_CRITICAL(&setup_lock);
    network_count = 0;
    if (error == ESP_OK) {
        for (uint16_t i = 0; i < found; ++i) {
            const wifi_ap_record_t *record = &scan_records[i];
            if (record->ssid[0] == 0 || seen_ssid(record->ssid)) {
                continue;
            }
            if (++last_network_id == 0) {
                ++last_network_id;
            }
            setup_network_t *entry = &networks[network_count++];
            entry->id = last_network_id;
            memcpy(entry->ssid, record->ssid, sizeof(entry->ssid));
            entry->rssi = record->rssi;
            entry->authmode = record->authmode;
        }
        session.phase = NEARIC_SETUP_READY;
    } else {
        session.phase = NEARIC_SETUP_RETRY;
    }
    portEXIT_CRITICAL(&setup_lock);
    explicit_bzero(scan_records, sizeof(scan_records));
}

esp_err_t nearic_provisioning_poll(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&setup_lock);
    nearic_setup_phase_t phase = session.phase;
    nearic_setup_phase_t terminal = close_requested(now);
    portEXIT_CRITICAL(&setup_lock);

    if (!active(phase) || phase == NEARIC_SETUP_OPENING) {
        return ESP_OK;
    }
    if (phase == NEARIC_SETUP_CLOSING) {
        return close_setup();
    }
    if (terminal != NEARIC_SETUP_IDLE) {
        begin_close(terminal);
        return close_setup();
    }
    if (phase == NEARIC_SETUP_SCANNING) {
        scan_networks();
        terminal = pending_close();
        if (terminal != NEARIC_SETUP_IDLE || !nearic_network_setup_ap_ready()) {
            begin_close(terminal == NEARIC_SETUP_IDLE ? NEARIC_SETUP_ERROR : terminal);
            return close_setup();
        }
        return ESP_OK;
    }
    if (phase != NEARIC_SETUP_CONNECTING) {
        return ESP_OK;
    }

    wifi_sta_config_t candidate;
    portENTER_CRITICAL(&setup_lock);
    candidate = pending;
    explicit_bzero(&pending, sizeof(pending));
    portEXIT_CRITICAL(&setup_lock);
    nearic_wifi_trial_result_t result = nearic_network_try_wifi_settings(&candidate, cancel_trial, NULL);
    explicit_bzero(&candidate, sizeof(candidate));

    now = esp_timer_get_time();
    bool ap_ready = nearic_network_setup_ap_ready();
    portENTER_CRITICAL(&setup_lock);
    session.has_trial_result = true;
    session.trial_result = result;
    terminal = result == NEARIC_WIFI_TRIAL_CONNECTED ? NEARIC_SETUP_CONNECTED : close_requested(now);
    if (terminal == NEARIC_SETUP_IDLE && !ap_ready) {
        terminal = NEARIC_SETUP_ERROR;
    }
    if (terminal == NEARIC_SETUP_IDLE) {
        session.phase = NEARIC_SETUP_RETRY;
    }
    portEXIT_CRITICAL(&setup_lock);
    if (terminal == NEARIC_SETUP_IDLE) {
        return ESP_OK;
    }
    begin_close(terminal);
    return close_setup();
}

static bool local_host(httpd_req_t *request)
{
    char address[sizeof(session.ap.address)];
    portENTER_CRITICAL(&setup_lock);
    memcpy(address, session.ap.address, sizeof(address));
    portEXIT_CRITICAL(&setup_lock);
    char host[sizeof(address) + sizeof(":80") - 1];
    if (address[0] == '\0' || httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }
    char address_with_port[sizeof(host)];
    snprintf(address_with_port, sizeof(address_with_port), "%s:80", address);
    return strcmp(host, address) == 0 || strcmp(host, address_with_port) == 0;
}

static const char *phase_name(nearic_setup_phase_t phase)
{
    static const char *const names[] = {
        [NEARIC_SETUP_IDLE] = "idle",
        [NEARIC_SETUP_OPENING] = "opening",
        [NEARIC_SETUP_SCANNING] = "scanning",
        [NEARIC_SETUP_READY] = "ready",
        [NEARIC_SETUP_CONNECTING] = "connecting",
        [NEARIC_SETUP_RETRY] = "retry",
        [NEARIC_SETUP_CLOSING] = "closing",
        [NEARIC_SETUP_CONNECTED] = "connected",
        [NEARIC_SETUP_CANCELLED] = "cancelled",
        [NEARIC_SETUP_EXPIRED] = "expired",
        [NEARIC_SETUP_ERROR] = "error",
    };
    return names[phase];
}

static esp_err_t reject_request(httpd_req_t *request, httpd_err_code_t error)
{
    // A successful ESP-IDF handler causes the server to drain unread body data.
    httpd_resp_send_err(request, error, NULL);
    return ESP_FAIL;
}

static esp_err_t send_status(httpd_req_t *request)
{
    if (!local_host(request)) {
        return reject_request(request, HTTPD_403_FORBIDDEN);
    }
    if (request->content_len != 0) {
        return reject_request(request, HTTPD_400_BAD_REQUEST);
    }
    int64_t now = esp_timer_get_time();
    setup_network_t snapshot[MAX_SETUP_NETWORKS];
    portENTER_CRITICAL(&setup_lock);
    nearic_setup_phase_t phase = session.phase;
    if (phase != NEARIC_SETUP_CLOSING && close_requested(now) != NEARIC_SETUP_IDLE) {
        phase = NEARIC_SETUP_CLOSING;
    }
    int result = session.has_trial_result ? (int)session.trial_result : -1;
    uint16_t count = phase == NEARIC_SETUP_CLOSING ? 0 : network_count;
    memcpy(snapshot, networks, count * sizeof(snapshot[0]));
    portEXIT_CRITICAL(&setup_lock);

    char output[160];
    snprintf(output, sizeof(output), "{\"state\":\"%s\",\"trial\":%d,\"networks\":[", phase_name(phase), result);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t error = httpd_resp_sendstr_chunk(request, output);
    for (uint16_t i = 0; i < count && error == ESP_OK; ++i) {
        static const char hex[] = "0123456789abcdef";
        char ssid_hex[2 * sizeof(pending.ssid) + 1];
        size_t length = strnlen((const char *)snapshot[i].ssid, sizeof(pending.ssid));
        for (size_t j = 0; j < length; ++j) {
            ssid_hex[j * 2] = hex[snapshot[i].ssid[j] >> 4];
            ssid_hex[j * 2 + 1] = hex[snapshot[i].ssid[j] & 15];
        }
        ssid_hex[length * 2] = '\0';
        snprintf(output, sizeof(output), "%s{\"id\":%" PRIu32 ",\"ssid_hex\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                 i == 0 ? "" : ",", snapshot[i].id, ssid_hex, (int)snapshot[i].rssi, (int)snapshot[i].authmode);
        error = httpd_resp_sendstr_chunk(request, output);
    }
    if (error == ESP_OK) {
        error = httpd_resp_sendstr_chunk(request, "]}");
    }
    return error == ESP_OK ? httpd_resp_sendstr_chunk(request, NULL) : error;
}

static bool accepts_commands(int64_t now)
{
    return (session.phase == NEARIC_SETUP_READY || session.phase == NEARIC_SETUP_RETRY) &&
           close_requested(now) == NEARIC_SETUP_IDLE;
}

static esp_err_t action_response(httpd_req_t *request, bool accepted)
{
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_status(request, accepted ? "202 Accepted" : "409 Conflict");
    return httpd_resp_send(request, NULL, 0);
}

static bool command_type(httpd_req_t *request)
{
    char content_type[sizeof("application/octet-stream")];
    return httpd_req_get_hdr_value_str(request, "Content-Type", content_type, sizeof(content_type)) == ESP_OK &&
           strcmp(content_type, "application/octet-stream") == 0;
}

static esp_err_t request_scan(httpd_req_t *request)
{
    if (!local_host(request)) {
        return reject_request(request, HTTPD_403_FORBIDDEN);
    }
    if (request->content_len != 0 || !command_type(request)) {
        return reject_request(request, HTTPD_400_BAD_REQUEST);
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&setup_lock);
    bool accepted = accepts_commands(now);
    if (accepted) {
        session.phase = NEARIC_SETUP_SCANNING;
        session.has_trial_result = false;
        network_count = 0;
    }
    portEXIT_CRITICAL(&setup_lock);
    return action_response(request, accepted);
}

static uint32_t requested_network(httpd_req_t *request)
{
    char id[sizeof("4294967295")];
    if (httpd_req_get_hdr_value_str(request, "X-Nearic-Network", id, sizeof(id)) != ESP_OK) {
        return 0;
    }
    uint64_t value = 0;
    for (const char *digit = id; *digit != '\0'; ++digit) {
        if (*digit < '0' || *digit > '9') {
            return 0;
        }
        value = value * 10 + (unsigned)(*digit - '0');
    }
    return value <= UINT32_MAX ? (uint32_t)value : 0;
}

static esp_err_t read_password(httpd_req_t *request, uint8_t *password)
{
    size_t received = 0;
    while (received < request->content_len) {
        if (pending_close() != NEARIC_SETUP_IDLE) {
            return ESP_ERR_INVALID_STATE;
        }
        int amount = httpd_req_recv(request, (char *)password + received, request->content_len - received);
        if (amount <= 0) {
            return ESP_ERR_INVALID_ARG;
        }
        received += (size_t)amount;
    }
    return memchr(password, '\0', received) == NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static bool stage_connection(uint32_t id, const uint8_t *password, int64_t now)
{
    if (!accepts_commands(now)) {
        return false;
    }
    for (uint16_t i = 0; i < network_count; ++i) {
        if (networks[i].id == id) {
            wifi_auth_mode_t authmode = networks[i].authmode;
            if (password[0] == 0 && authmode != WIFI_AUTH_OPEN && authmode != WIFI_AUTH_OWE) {
                session.phase = NEARIC_SETUP_RETRY;
                session.has_trial_result = true;
                session.trial_result = NEARIC_WIFI_TRIAL_AUTH_FAILED;
                return true;
            }
            memcpy(pending.ssid, networks[i].ssid, sizeof(pending.ssid));
            memcpy(pending.password, password, sizeof(pending.password));
            session.phase = NEARIC_SETUP_CONNECTING;
            session.has_trial_result = false;
            return true;
        }
    }
    return false;
}

static esp_err_t request_connection(httpd_req_t *request)
{
    if (!local_host(request)) {
        return reject_request(request, HTTPD_403_FORBIDDEN);
    }
    uint32_t id = requested_network(request);
    uint8_t password[sizeof(pending.password)] = { 0 };
    if (id == 0 || request->content_len > sizeof(password) || !command_type(request)) {
        return reject_request(request, HTTPD_400_BAD_REQUEST);
    }
    esp_err_t read_error = read_password(request, password);
    if (read_error != ESP_OK) {
        explicit_bzero(password, sizeof(password));
        if (read_error == ESP_ERR_INVALID_STATE) {
            action_response(request, false);
            return ESP_FAIL;
        }
        return reject_request(request, HTTPD_400_BAD_REQUEST);
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&setup_lock);
    bool accepted = stage_connection(id, password, now);
    portEXIT_CRITICAL(&setup_lock);
    explicit_bzero(password, sizeof(password));
    return action_response(request, accepted);
}

esp_err_t nearic_provisioning_start(void)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/api/wifi", .method = HTTP_GET, .handler = send_status },
        { .uri = "/api/wifi/scan", .method = HTTP_POST, .handler = request_scan },
        { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = request_connection },
    };
    portENTER_CRITICAL(&setup_lock);
    if (active(session.phase)) {
        portEXIT_CRITICAL(&setup_lock);
        return ESP_ERR_INVALID_STATE;
    }
    session = (nearic_setup_status_t){ .phase = NEARIC_SETUP_OPENING };
    cancelled = false;
    portEXIT_CRITICAL(&setup_lock);

    nearic_setup_ap_t ap = { 0 };
    esp_err_t error = nearic_network_open_setup_ap(&ap, routes, sizeof(routes) / sizeof(routes[0]));
    if (error == ESP_OK) {
        int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&setup_lock);
        session.ap = ap;
        deadline = now + SETUP_DURATION_US;
        session.phase = NEARIC_SETUP_SCANNING;
        portEXIT_CRITICAL(&setup_lock);
    } else {
        begin_close(NEARIC_SETUP_ERROR);
        close_setup();
    }
    explicit_bzero(&ap, sizeof(ap));
    return error;
}
