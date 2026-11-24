#include "display.h"

#include "bsp/esp-bsp.h"
#include "lvgl.h"

static lv_obj_t *status_label;

esp_err_t nearic_display_init(const char *version)
{
    if (bsp_display_start() == NULL) {
        return ESP_FAIL;
    }
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101921), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xe8f2f4), 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Nearic");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -44);

    status_label = lv_label_create(screen);
    lv_obj_set_width(status_label, lv_pct(90));
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label, "Starting");
    lv_obj_center(status_label);

    lv_obj_t *firmware = lv_label_create(screen);
    lv_label_set_text_fmt(firmware, "Firmware %s", version);
    lv_obj_align(firmware, LV_ALIGN_BOTTOM_MID, 0, -16);

    bsp_display_unlock();
    return bsp_display_backlight_on();
}

esp_err_t nearic_display_status(const char *status)
{
    if (status_label == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    lv_label_set_text(status_label, status);
    bsp_display_unlock();
    return ESP_OK;
}
