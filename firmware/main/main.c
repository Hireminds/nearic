#include <stdbool.h>

#include "display.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network.h"
#include "nvs_flash.h"
#include "update.h"

static const char *TAG = "nearic";

static void restart_unconfirmed_image(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t check_startup(bool *configured)
{
    esp_err_t error = nearic_display_init(esp_app_get_description()->version);
    if (error == ESP_OK) {
        error = nvs_flash_init();
    }
    if (error == ESP_OK) {
        error = nearic_network_init(configured);
    }
    return error;
}

void app_main(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    bool pending = esp_ota_get_state_partition(running, &state) == ESP_OK &&
                   state == ESP_OTA_IMG_PENDING_VERIFY;

    esp_timer_handle_t startup_deadline = NULL;
    esp_err_t error = ESP_OK;
    if (pending) {
        const esp_timer_create_args_t timer = {
            .callback = restart_unconfirmed_image,
            .name = "ota_startup",
        };
        error = esp_timer_create(&timer, &startup_deadline);
        if (error == ESP_OK) {
            error = esp_timer_start_once(startup_deadline, 15 * 1000 * 1000);
        }
    }

    bool configured = false;
    if (error == ESP_OK) {
        error = check_startup(&configured);
    }
    if (error == ESP_OK && pending) {
        error = configured ? esp_ota_mark_app_valid_cancel_rollback() : ESP_ERR_INVALID_STATE;
    }
    if (startup_deadline != NULL) {
        esp_timer_stop(startup_deadline);
        esp_timer_delete(startup_deadline);
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Startup check failed: %s", esp_err_to_name(error));
        nearic_display_status("Startup failed\nUSB recovery");
        if (pending && esp_ota_check_rollback_is_possible()) {
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        return;
    }

    error = nearic_update_read_rollback(NULL);
    if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "OTA journal unavailable: %s", esp_err_to_name(error));
    }

    if (!configured) {
        nearic_display_status("Wi-Fi setup required\nConnect USB");
        return;
    }

    bool last_connected = false;
    nearic_display_status("Connecting to Wi-Fi");
    while (true) {
        bool connected = nearic_network_wait_for_address();
        if (connected != last_connected &&
            nearic_display_status(connected ? "Wi-Fi connected" : "Connecting to Wi-Fi") == ESP_OK) {
            last_connected = connected;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
