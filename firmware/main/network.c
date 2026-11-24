#include "network.h"

#include "wifi_settings.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define WIFI_ASSOCIATED BIT0
#define WIFI_HAS_ADDRESS BIT1
#define WIFI_STATION_STARTED BIT2
#define WIFI_STATION_STOPPED BIT3
#define WIFI_AUTH_FAILED BIT4
#define WIFI_AP_UNAVAILABLE BIT5
#define WIFI_SETUP_AP_STARTED BIT6
#define WIFI_SETUP_AP_STOPPED BIT7
#define WIFI_ASSOCIATION_TIMED_OUT BIT8
#define WIFI_CONNECTION_EVENTS (WIFI_ASSOCIATED | WIFI_HAS_ADDRESS | WIFI_AUTH_FAILED | \
                                WIFI_AP_UNAVAILABLE | WIFI_ASSOCIATION_TIMED_OUT)
#define WIFI_LIFECYCLE_TIMEOUT_MS 2000
#define WIFI_TRIAL_TIMEOUT_MS 20000
#define WIFI_CANCEL_POLL_MS 100

static EventGroupHandle_t network_events;
static esp_netif_t *station_netif;
static esp_netif_t *setup_netif;
static bool settings_need_migration;
static bool radio_running;
static bool setup_protected;
static wifi_mode_t radio_mode;
static EventBits_t pending_stop_events;
static bool station_configured;

static httpd_handle_t setup_server;
static const httpd_uri_t *setup_routes;
static uint16_t setup_route_count;

static wifi_sta_config_t candidate_settings;
static uint32_t last_attempt_id;
static uint32_t pending_attempt_id;

static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            xEventGroupClearBits(network_events, WIFI_STATION_STOPPED);
            xEventGroupSetBits(network_events, WIFI_STATION_STARTED);
        } else if (id == WIFI_EVENT_STA_STOP) {
            xEventGroupClearBits(network_events, WIFI_STATION_STARTED | WIFI_CONNECTION_EVENTS);
            xEventGroupSetBits(network_events, WIFI_STATION_STOPPED);
        } else if (id == WIFI_EVENT_AP_START) {
            xEventGroupClearBits(network_events, WIFI_SETUP_AP_STOPPED);
            xEventGroupSetBits(network_events, WIFI_SETUP_AP_STARTED);
        } else if (id == WIFI_EVENT_AP_STOP) {
            xEventGroupClearBits(network_events, WIFI_SETUP_AP_STARTED);
            xEventGroupSetBits(network_events, WIFI_SETUP_AP_STOPPED);
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            xEventGroupClearBits(network_events, WIFI_CONNECTION_EVENTS);
            xEventGroupSetBits(network_events, WIFI_ASSOCIATED);
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(network_events, WIFI_ASSOCIATED | WIFI_HAS_ADDRESS);
            const wifi_event_sta_disconnected_t *event = data;
            if (event != NULL) {
                if (event->reason == WIFI_REASON_AUTH_FAIL ||
                    event->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                    event->reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
                    xEventGroupSetBits(network_events, WIFI_AUTH_FAILED);
                } else if (event->reason == WIFI_REASON_NO_AP_FOUND) {
                    xEventGroupSetBits(network_events, WIFI_AP_UNAVAILABLE);
                } else if (event->reason == WIFI_REASON_AUTH_EXPIRE || event->reason == WIFI_REASON_ASSOC_EXPIRE) {
                    xEventGroupSetBits(network_events, WIFI_ASSOCIATION_TIMED_OUT);
                }
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        if (event != NULL && event->esp_netif == station_netif && event->ip_info.ip.addr != 0 &&
            (xEventGroupGetBits(network_events) & WIFI_ASSOCIATED)) {
            xEventGroupSetBits(network_events, WIFI_HAS_ADDRESS);
        }
    }
}

static bool station_has_address(void)
{
    if (network_events == NULL || station_netif == NULL) {
        return false;
    }

    const EventBits_t ready = WIFI_ASSOCIATED | WIFI_HAS_ADDRESS;
    esp_netif_ip_info_t address;
    return (xEventGroupGetBits(network_events) & ready) == ready &&
           esp_netif_is_netif_up(station_netif) &&
           esp_netif_get_ip_info(station_netif, &address) == ESP_OK &&
           address.ip.addr != 0;
}

static bool candidate_is_connected(void)
{
    wifi_ap_record_t access_point;
    if (!station_has_address() || esp_wifi_sta_get_ap_info(&access_point) != ESP_OK) {
        return false;
    }

    // ESP-IDF returns the associated SSID as a string. The stored SSID may
    // occupy all 32 bytes without a trailing NUL.
    if (strncmp((const char *)access_point.ssid, (const char *)candidate_settings.ssid,
                sizeof(candidate_settings.ssid)) != 0) {
        return false;
    }

    wifi_config_t current = { 0 };
    bool matched = esp_wifi_get_config(WIFI_IF_STA, &current) == ESP_OK &&
                   memcmp(current.sta.ssid, candidate_settings.ssid,
                          sizeof(candidate_settings.ssid)) == 0 &&
                   memcmp(current.sta.password, candidate_settings.password,
                          sizeof(candidate_settings.password)) == 0;
    explicit_bzero(&current, sizeof(current));
    return matched && station_has_address();
}

static esp_err_t stage_wifi_settings(const wifi_sta_config_t *configuration, uint32_t *attempt_id)
{
    if (configuration == NULL || attempt_id == NULL || configuration->ssid[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pending_attempt_id != 0) {
        return ESP_ERR_INVALID_STATE;
    }

    ++last_attempt_id;
    if (last_attempt_id == 0) {
        ++last_attempt_id;
    }

    memcpy(candidate_settings.ssid, configuration->ssid, sizeof(candidate_settings.ssid));
    memcpy(candidate_settings.password, configuration->password, sizeof(candidate_settings.password));
    pending_attempt_id = last_attempt_id;
    *attempt_id = pending_attempt_id;
    return ESP_OK;
}

static esp_err_t finish_wifi_settings(uint32_t attempt_id, bool address_acquired,
                                      bool *write_uncertain)
{
    *write_uncertain = false;
    if (attempt_id == 0 || attempt_id != pending_attempt_id) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = ESP_OK;
    if (address_acquired) {
        error = candidate_is_connected()
            ? nearic_wifi_settings_save(&candidate_settings, write_uncertain)
            : ESP_ERR_INVALID_STATE;
    }
    explicit_bzero(&candidate_settings, sizeof(candidate_settings));
    pending_attempt_id = 0;
    return error;
}

static void migrate_verified_settings(void)
{
    // Keep earlier records readable and retry migration on the next boot
    // if the new profile could not be saved.
    settings_need_migration = false;
    wifi_config_t current = { 0 };
    uint32_t attempt_id;
    bool write_uncertain;
    esp_err_t error = esp_wifi_get_config(WIFI_IF_STA, &current);
    if (error == ESP_OK) {
        error = stage_wifi_settings(&current.sta, &attempt_id);
    }
    explicit_bzero(&current, sizeof(current));
    if (error == ESP_OK) {
        error = finish_wifi_settings(attempt_id, true, &write_uncertain);
    }
    if (error != ESP_OK) {
        ESP_LOGW("nearic_wifi", "Could not save Wi-Fi settings: %s", esp_err_to_name(error));
    }
}

static bool wait_for_wifi_event(EventBits_t events, unsigned timeout_ms)
{
    return (xEventGroupWaitBits(network_events, events, pdFALSE, pdTRUE,
                                pdMS_TO_TICKS(timeout_ms)) & events) == events;
}

static esp_err_t set_radio_mode(wifi_mode_t mode)
{
    esp_err_t error = esp_wifi_set_mode(mode);
    if (error == ESP_OK) {
        radio_mode = mode;
    }
    return error;
}

static esp_err_t stop_setup_http(void)
{
    if (setup_server == NULL) {
        return ESP_OK;
    }
    esp_err_t error = httpd_stop(setup_server);
    if (error == ESP_OK) {
        setup_server = NULL;
    }
    return error;
}

static esp_err_t start_setup_http(void)
{
    struct ifreq interface = { 0 };
    ESP_RETURN_ON_ERROR(esp_netif_get_netif_impl_name(setup_netif, interface.ifr_name),
                        "nearic_wifi", "Read setup interface name");
    if (interface.ifr_name[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.if_name = &interface;
    config.max_uri_handlers = setup_route_count;
    ESP_RETURN_ON_ERROR(httpd_start(&setup_server, &config), "nearic_wifi", "Start setup HTTP server");
    for (uint16_t i = 0; i < setup_route_count; ++i) {
        esp_err_t error = httpd_register_uri_handler(setup_server, &setup_routes[i]);
        if (error != ESP_OK) {
            esp_err_t cleanup = stop_setup_http();
            if (cleanup != ESP_OK) {
                ESP_LOGW("nearic_wifi", "Setup HTTP cleanup failed: %s", esp_err_to_name(cleanup));
            }
            return error;
        }
    }
    return ESP_OK;
}

static esp_err_t start_radio(void)
{
    if (radio_mode == WIFI_MODE_APSTA && !setup_protected) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = esp_wifi_start();
    if (error != ESP_OK) {
        return error;
    }
    radio_running = true;
    EventBits_t started = WIFI_STATION_STARTED;
    if (radio_mode == WIFI_MODE_APSTA) {
        started |= WIFI_SETUP_AP_STARTED;
    }
    if (!wait_for_wifi_event(started, WIFI_LIFECYCLE_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    if (radio_mode == WIFI_MODE_APSTA) {
        esp_netif_dhcp_status_t dhcp;
        if (!esp_netif_is_netif_up(setup_netif) ||
            esp_netif_dhcps_get_status(setup_netif, &dhcp) != ESP_OK || dhcp != ESP_NETIF_DHCP_STARTED) {
            return ESP_ERR_INVALID_STATE;
        }
        return start_setup_http();
    }
    return ESP_OK;
}

static esp_err_t stop_radio(void)
{
    // lwIP can assign a new index when the AP netif is added again.
    ESP_RETURN_ON_ERROR(stop_setup_http(), "nearic_wifi", "Stop setup HTTP server before radio restart");
    if (radio_running) {
        EventBits_t stopped = WIFI_STATION_STOPPED;
        if (radio_mode == WIFI_MODE_APSTA) {
            stopped |= WIFI_SETUP_AP_STOPPED;
        }
        xEventGroupClearBits(network_events, stopped);
        ESP_RETURN_ON_ERROR(esp_wifi_stop(), "nearic_wifi", "Stop Wi-Fi radio");
        radio_running = false;
        pending_stop_events = stopped;
    }

    // The default netif handler runs before ours. Wait for it to tear down
    // the station and the setup AP before accepting events for a new start.
    if (pending_stop_events == 0) {
        return ESP_OK;
    }
    if (!wait_for_wifi_event(pending_stop_events, WIFI_LIFECYCLE_TIMEOUT_MS) ||
        esp_netif_is_netif_up(station_netif) ||
        (setup_netif != NULL && esp_netif_is_netif_up(setup_netif))) {
        return ESP_ERR_TIMEOUT;
    }
    pending_stop_events = 0;
    return ESP_OK;
}

static esp_err_t switch_station(wifi_config_t *profile)
{
    ESP_RETURN_ON_ERROR(stop_radio(), "nearic_wifi", "Stop station for profile change");
    xEventGroupClearBits(network_events, WIFI_CONNECTION_EVENTS);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, profile),
                        "nearic_wifi", "Set station profile");
    return start_radio();
}

static void make_setup_config(wifi_config_t *config)
{
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    _Static_assert(sizeof(alphabet) == 33, "Setup password uses a 32-symbol alphabet");
    uint8_t random[3 + sizeof(((nearic_setup_ap_t *)0)->password) - 1];
    esp_fill_random(random, sizeof(random));
    config->ap.ssid_len = snprintf((char *)config->ap.ssid, sizeof(config->ap.ssid),
                                  "Nearic-%02X%02X%02X", (unsigned)random[0],
                                  (unsigned)random[1], (unsigned)random[2]);
    for (size_t i = 3; i < sizeof(random); ++i) {
        config->ap.password[i - 3] = alphabet[random[i] & 31U];
    }
    explicit_bzero(random, sizeof(random));
    config->ap.channel = 1;
    config->ap.authmode = WIFI_AUTH_WPA2_PSK;
    config->ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
    config->ap.max_connection = 1;
}

esp_err_t nearic_network_close_setup_ap(void)
{
    if (network_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (radio_mode == WIFI_MODE_STA && radio_running &&
        (xEventGroupGetBits(network_events) & WIFI_STATION_STARTED)) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(stop_radio(), "nearic_wifi", "Stop setup AP");
    setup_routes = NULL;
    setup_route_count = 0;
    esp_err_t clear_error = ESP_OK;
    if (radio_mode == WIFI_MODE_APSTA) {
        // Replace the session key while the radio is stopped. The next start
        // is in STA mode, so this placeholder is never broadcast.
        setup_protected = false;
        wifi_config_t empty = { .ap = { .ssid = "Nearic", .channel = 1, .max_connection = 1 } };
        clear_error = esp_wifi_set_config(WIFI_IF_AP, &empty);
        ESP_RETURN_ON_ERROR(set_radio_mode(WIFI_MODE_STA), "nearic_wifi", "Restore station mode");
    }
    ESP_RETURN_ON_ERROR(start_radio(), "nearic_wifi", "Restart station");
    if (station_configured) {
        esp_wifi_connect();
    }
    return clear_error;
}

esp_err_t nearic_network_open_setup_ap(nearic_setup_ap_t *setup,
                                      const httpd_uri_t *routes, uint16_t route_count)
{
    if (setup == NULL || routes == NULL || route_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (radio_mode != WIFI_MODE_STA || !radio_running) {
        return ESP_ERR_INVALID_STATE;
    }
    explicit_bzero(setup, sizeof(*setup));
    if (setup_netif == NULL) {
        setup_netif = esp_netif_create_default_wifi_ap();
        if (setup_netif == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    wifi_config_t config = { 0 };
    make_setup_config(&config);
    esp_err_t error = stop_radio();
    if (error == ESP_OK) {
        error = set_radio_mode(WIFI_MODE_APSTA);
    }
    if (error == ESP_OK) {
        error = esp_wifi_set_config(WIFI_IF_AP, &config);
        setup_protected = error == ESP_OK;
    }
    if (error == ESP_OK) {
        setup_routes = routes;
        setup_route_count = route_count;
        error = start_radio();
    }

    esp_netif_ip_info_t address;
    if (error == ESP_OK &&
        (esp_netif_get_ip_info(setup_netif, &address) != ESP_OK || address.ip.addr == 0)) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK) {
        memcpy(setup->ssid, config.ap.ssid, sizeof(setup->ssid));
        memcpy(setup->password, config.ap.password, sizeof(setup->password));
        snprintf(setup->address, sizeof(setup->address), IPSTR, IP2STR(&address.ip));
    } else {
        esp_err_t cleanup = nearic_network_close_setup_ap();
        if (cleanup != ESP_OK) {
            ESP_LOGW("nearic_wifi", "Setup AP cleanup failed: %s", esp_err_to_name(cleanup));
        }
    }
    explicit_bzero(&config, sizeof(config));
    return error;
}

bool nearic_network_setup_ap_ready(void)
{
    return radio_mode == WIFI_MODE_APSTA && radio_running && setup_protected && setup_server != NULL &&
           (xEventGroupGetBits(network_events) & WIFI_SETUP_AP_STARTED) && esp_netif_is_netif_up(setup_netif);
}

esp_err_t nearic_network_scan_setup_wifi(wifi_ap_record_t *records, uint16_t *count)
{
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t found = *count;
    *count = 0;
    if (records == NULL || found == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nearic_network_setup_ap_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = esp_wifi_scan_start(NULL, true);
    if (error != ESP_OK) {
        if (error == ESP_ERR_WIFI_TIMEOUT) {
            esp_wifi_scan_stop();
            esp_wifi_clear_ap_list();
        }
        return error;
    }
    error = esp_wifi_scan_get_ap_records(&found, records);
    if (error != ESP_OK) {
        esp_wifi_clear_ap_list();
        return error;
    }
    *count = found;
    return ESP_OK;
}

static esp_err_t init_wifi(wifi_config_t *config)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), "nearic_wifi", "Initialize interfaces");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), "nearic_wifi", "Initialize event loop");
    station_netif = esp_netif_create_default_wifi_sta();
    if (station_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }
    network_events = xEventGroupCreate();
    if (network_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), "nearic_wifi", "Initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), "nearic_wifi", "Set Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START,
                                                   network_event, NULL),
                        "nearic_wifi", "Register station start handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
                                                   network_event, NULL),
                        "nearic_wifi", "Register association handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                   network_event, NULL),
                        "nearic_wifi", "Register disconnect handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_STOP,
                                                   network_event, NULL),
                        "nearic_wifi", "Register station stop handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START,
                                                   network_event, NULL),
                        "nearic_wifi", "Register setup AP start handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP,
                                                   network_event, NULL),
                        "nearic_wifi", "Register setup AP stop handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   network_event, NULL),
                        "nearic_wifi", "Register address handler");
    ESP_RETURN_ON_ERROR(set_radio_mode(WIFI_MODE_STA), "nearic_wifi", "Set station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, config),
                        "nearic_wifi", "Set Wi-Fi configuration");
    return start_radio();
}

esp_err_t nearic_network_init(bool *configured)
{
    station_configured = false;
    explicit_bzero(&candidate_settings, sizeof(candidate_settings));
    pending_attempt_id = 0;
    wifi_config_t wifi = { 0 };
    esp_err_t error = nearic_wifi_settings_load(&wifi.sta, configured, &settings_need_migration);
    if (error == ESP_OK) {
        error = init_wifi(&wifi);
        station_configured = error == ESP_OK && *configured;
    }
    explicit_bzero(&wifi, sizeof(wifi));
    return error;
}

bool nearic_network_wait_for_address(void)
{
    if (!station_configured) {
        return false;
    }
    if (!(xEventGroupGetBits(network_events) & WIFI_ASSOCIATED)) {
        esp_wifi_connect();
    }
    wait_for_wifi_event(WIFI_HAS_ADDRESS, 5000);
    bool connected = station_has_address();
    if (connected && settings_need_migration) {
        migrate_verified_settings();
    }
    return connected;
}

static nearic_wifi_trial_result_t wait_for_trial(bool (*cancel)(void *), void *context)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(WIFI_TRIAL_TIMEOUT_MS);
    bool retried = false;
    while (xTaskGetTickCount() - start < timeout) {
        if (cancel != NULL && cancel(context)) {
            return NEARIC_WIFI_TRIAL_CANCELLED;
        }

        EventBits_t observed = xEventGroupGetBits(network_events);
        if (observed & WIFI_HAS_ADDRESS) {
            if (candidate_is_connected()) {
                return NEARIC_WIFI_TRIAL_CONNECTED;
            }
        } else if (observed & WIFI_AUTH_FAILED) {
            return NEARIC_WIFI_TRIAL_AUTH_FAILED;
        } else if (observed & WIFI_AP_UNAVAILABLE) {
            return NEARIC_WIFI_TRIAL_NETWORK_UNAVAILABLE;
        } else if (observed & WIFI_ASSOCIATION_TIMED_OUT) {
            if (retried) {
                return NEARIC_WIFI_TRIAL_TIMED_OUT;
            }
            xEventGroupClearBits(network_events, WIFI_ASSOCIATION_TIMED_OUT);
            if (esp_wifi_connect() != ESP_OK) {
                return NEARIC_WIFI_TRIAL_DRIVER_ERROR;
            }
            retried = true;
        }
        vTaskDelay(pdMS_TO_TICKS(WIFI_CANCEL_POLL_MS));
    }
    return NEARIC_WIFI_TRIAL_TIMED_OUT;
}

nearic_wifi_trial_result_t nearic_network_try_wifi_settings(
    const wifi_sta_config_t *configuration, bool (*cancel)(void *), void *context)
{
    if (cancel != NULL && cancel(context)) {
        return NEARIC_WIFI_TRIAL_CANCELLED;
    }
    uint32_t attempt_id;
    if (network_events == NULL ||
        stage_wifi_settings(configuration, &attempt_id) != ESP_OK) {
        return NEARIC_WIFI_TRIAL_DRIVER_ERROR;
    }

    wifi_config_t previous = { 0 };
    bool configured, needs_migration, write_uncertain;
    if (nearic_wifi_settings_load(&previous.sta, &configured, &needs_migration) != ESP_OK) {
        finish_wifi_settings(attempt_id, false, &write_uncertain);
        explicit_bzero(&previous, sizeof(previous));
        return NEARIC_WIFI_TRIAL_STORAGE_ERROR;
    }

    station_configured = false;
    wifi_config_t trial = { .sta = *configuration };
    esp_err_t error = switch_station(&trial);
    explicit_bzero(&trial, sizeof(trial));
    nearic_wifi_trial_result_t result = NEARIC_WIFI_TRIAL_DRIVER_ERROR;
    if (error == ESP_OK && esp_wifi_connect() == ESP_OK) {
        result = wait_for_trial(cancel, context);
    }
    if (result == NEARIC_WIFI_TRIAL_CONNECTED && cancel != NULL && cancel(context)) {
        result = NEARIC_WIFI_TRIAL_CANCELLED;
    }

    error = finish_wifi_settings(attempt_id, result == NEARIC_WIFI_TRIAL_CONNECTED,
                                 &write_uncertain);
    if (write_uncertain) {
        result = NEARIC_WIFI_TRIAL_STORAGE_UNCERTAIN;
    } else if (error != ESP_OK) {
        result = error == ESP_ERR_INVALID_STATE ? NEARIC_WIFI_TRIAL_DRIVER_ERROR
                                                : NEARIC_WIFI_TRIAL_STORAGE_ERROR;
    }
    if (result == NEARIC_WIFI_TRIAL_CONNECTED) {
        station_configured = true;
        settings_need_migration = false;
    } else if (switch_station(&previous) == ESP_OK) {
        station_configured = configured;
        settings_need_migration = needs_migration;
        if (configured && esp_wifi_connect() != ESP_OK &&
            result != NEARIC_WIFI_TRIAL_STORAGE_UNCERTAIN) {
            result = NEARIC_WIFI_TRIAL_RESTORE_ERROR;
        }
    } else if (result != NEARIC_WIFI_TRIAL_STORAGE_UNCERTAIN) {
        result = NEARIC_WIFI_TRIAL_RESTORE_ERROR;
    }
    explicit_bzero(&previous, sizeof(previous));
    return result;
}
