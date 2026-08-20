#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    int percent;          /* 0-100, or -1 when no battery is connected */
    bool charging;
    bool vbus;            /* USB power present */
    bool battery_present;
} fb_power_t;

esp_err_t fb_power_init(void);
esp_err_t fb_power_read(fb_power_t *out);
