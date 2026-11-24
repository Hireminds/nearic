#include "wifi_settings.h"

#include <stdint.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#define WIFI_SETTINGS_VERSION 1

static const char *const settings_partition = "nearic_cfg";
static const char *const settings_namespace = "nearic";
static const char *const profile_keys[] = { "wifi_profile_a", "wifi_profile_b" };

typedef struct {
    uint8_t version;
    uint8_t ssid[32];
    uint8_t password[64];
} wifi_settings_record_t;

typedef struct {
    uint64_t generation;
    wifi_settings_record_t settings;
    uint8_t reserved[7];
} wifi_profile_slot_t;

_Static_assert(sizeof(wifi_settings_record_t) == 97, "NVS Wi-Fi record layout changed");
_Static_assert(sizeof(wifi_profile_slot_t) == 112, "NVS Wi-Fi slot layout changed");
_Static_assert(sizeof(((wifi_sta_config_t *)0)->ssid) == 32, "ESP-IDF SSID size changed");
_Static_assert(sizeof(((wifi_sta_config_t *)0)->password) == 64, "ESP-IDF password size changed");

static bool valid_record(const wifi_settings_record_t *record)
{
    return record->version == WIFI_SETTINGS_VERSION && record->ssid[0] != 0;
}

static void copy_profile(wifi_sta_config_t *profile, const wifi_settings_record_t *record)
{
    memcpy(profile->ssid, record->ssid, sizeof(profile->ssid));
    memcpy(profile->password, record->password, sizeof(profile->password));
}

static esp_err_t load_usb_profile(nvs_handle_t handle, wifi_sta_config_t *profile,
                                  bool *configured)
{
    char ssid[sizeof(profile->ssid) + 1] = { 0 };
    size_t ssid_size = sizeof(ssid);
    esp_err_t error = nvs_get_str(handle, "ssid", ssid, &ssid_size);
    if (error == ESP_OK && ssid_size > 1) {
        char password[sizeof(profile->password) + 1] = { 0 };
        size_t password_size = sizeof(password);
        error = nvs_get_str(handle, "wifi_password", password, &password_size);
        if (error == ESP_OK) {
            memcpy(profile->ssid, ssid, ssid_size - 1);
            memcpy(profile->password, password, password_size - 1);
            *configured = true;
        }
        explicit_bzero(password, sizeof(password));
    }
    return error == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : error;
}

static esp_err_t load_old_profile(nvs_handle_t handle, wifi_sta_config_t *profile,
                                  bool *configured)
{
    wifi_settings_record_t old = { 0 };
    size_t size = sizeof(old);
    esp_err_t error = nvs_get_blob(handle, "wifi_config", &old, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        error = load_usb_profile(handle, profile, configured);
    } else if (error == ESP_OK) {
        if (size == sizeof(old) && valid_record(&old)) {
            copy_profile(profile, &old);
            *configured = true;
        } else {
            error = ESP_ERR_INVALID_STATE;
        }
    }
    explicit_bzero(&old, sizeof(old));
    return error;
}

static esp_err_t find_active_slot(nvs_handle_t handle, wifi_profile_slot_t *active,
                                  unsigned *active_index)
{
    for (unsigned i = 0; i < sizeof(profile_keys) / sizeof(profile_keys[0]); ++i) {
        wifi_profile_slot_t slot = { 0 };
        size_t size = sizeof(slot);
        esp_err_t error = nvs_get_blob(handle, profile_keys[i], &slot, &size);
        if (error == ESP_OK && size == sizeof(slot) && valid_record(&slot.settings) &&
            slot.generation > active->generation) {
            *active = slot;
            *active_index = i;
        }
        explicit_bzero(&slot, sizeof(slot));

        if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND &&
            error != ESP_ERR_NVS_INVALID_LENGTH && error != ESP_ERR_NVS_TYPE_MISMATCH) {
            return error;
        }
    }
    return ESP_OK;
}

esp_err_t nearic_wifi_settings_load(wifi_sta_config_t *profile, bool *configured,
                                   bool *needs_migration)
{
    *configured = false;
    *needs_migration = false;
    esp_err_t error = nvs_flash_init_partition(settings_partition);
    if (error != ESP_OK) {
        return error;
    }

    nvs_handle_t handle;
    error = nvs_open_from_partition(settings_partition, settings_namespace, NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : error;
    }

    wifi_profile_slot_t active = { 0 };
    unsigned index;
    error = find_active_slot(handle, &active, &index);
    if (error == ESP_OK) {
        if (active.generation != 0) {
            copy_profile(profile, &active.settings);
            *configured = true;
        } else {
            error = load_old_profile(handle, profile, configured);
            *needs_migration = error == ESP_OK && *configured;
        }
    }
    explicit_bzero(&active, sizeof(active));
    nvs_close(handle);
    return error;
}

static esp_err_t write_slot(nvs_handle_t handle, const char *key, const wifi_profile_slot_t *slot,
                            bool *write_uncertain)
{
    // Reuse only the inactive slot. An insert under an empty key avoids NVS
    // having to remove an earlier value after writing the new profile.
    esp_err_t error = nvs_erase_key(handle, key);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
        if (error != ESP_OK) {
            return error;
        }
    } else if (error != ESP_ERR_NVS_NOT_FOUND) {
        return error;
    }

    error = nvs_set_blob(handle, key, slot, sizeof(*slot));
    if (error != ESP_OK) {
        // A flash I/O error can be reported after the new blob is readable.
        *write_uncertain = error != ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        return error;
    }

    error = nvs_commit(handle);
    if (error == ESP_OK) {
        wifi_profile_slot_t stored = { 0 };
        size_t size = sizeof(stored);
        error = nvs_get_blob(handle, key, &stored, &size);
        if (error == ESP_OK && (size != sizeof(stored) || memcmp(&stored, slot, size) != 0)) {
            error = ESP_ERR_INVALID_CRC;
        }
        explicit_bzero(&stored, sizeof(stored));
    }
    *write_uncertain = error != ESP_OK;
    return error;
}

esp_err_t nearic_wifi_settings_save(const wifi_sta_config_t *profile, bool *write_uncertain)
{
    *write_uncertain = false;
    if (profile->ssid[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t error = nvs_open_from_partition(settings_partition, settings_namespace, NVS_READWRITE,
                                             &handle);
    if (error != ESP_OK) {
        return error;
    }

    wifi_profile_slot_t active = { 0 };
    unsigned index;
    error = find_active_slot(handle, &active, &index);
    if (error == ESP_OK && active.generation == UINT64_MAX) {
        error = ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
    if (error == ESP_OK) {
        wifi_profile_slot_t slot = {
            .generation = active.generation + 1,
            .settings.version = WIFI_SETTINGS_VERSION,
        };
        memcpy(slot.settings.ssid, profile->ssid, sizeof(slot.settings.ssid));
        memcpy(slot.settings.password, profile->password, sizeof(slot.settings.password));
        unsigned target = active.generation == 0 ? 0 : 1 - index;
        error = write_slot(handle, profile_keys[target], &slot, write_uncertain);
        explicit_bzero(&slot, sizeof(slot));
    }
    explicit_bzero(&active, sizeof(active));
    nvs_close(handle);
    return error;
}
