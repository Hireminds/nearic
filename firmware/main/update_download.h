#pragma once

#include "device_https.h"
#include "update.h"

// Use an endpoint from the device configuration after establishing trusted UTC.
// A successful response stages the image. HTTP status is zero until headers arrive.
esp_err_t nearic_update_download(const nearic_update_endpoint_t *endpoint, const char *path,
                                 const nearic_update_image_t *image, esp_partition_subtype_t *slot,
                                 int *http_status);
