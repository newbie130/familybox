/* AXP2101 battery telemetry.
 *
 * Waveshare's own example drives this through XPowersLib, a C++ library.
 * We need three registers, so we read them directly over the I2C bus the BSP
 * already opened rather than pulling C++ into the build. Register meanings
 * are taken from XPowersLib's AXP2101 implementation.
 */
#include "fb_power.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "fb_power";

#define AXP2101_ADDR 0x34
#define REG_STATUS1 0x00   /* bit5 VBUS good, bit3 battery connected */
#define REG_STATUS2 0x01   /* bits 7:5 charge state, 0x01 == charging */
#define REG_BAT_PERCENT 0xA4

static i2c_master_dev_handle_t s_dev;

esp_err_t fb_power_init(void)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_dev),
                        TAG, "could not attach to AXP2101");

    fb_power_t p;
    if (fb_power_read(&p) == ESP_OK) {
        ESP_LOGI(TAG, "AXP2101 ready: battery=%s %d%% charging=%d vbus=%d",
                 p.battery_present ? "yes" : "no", p.percent,
                 (int)p.charging, (int)p.vbus);
    }
    return ESP_OK;
}

static esp_err_t read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, 200);
}

esp_err_t fb_power_read(fb_power_t *out)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t s1 = 0, s2 = 0, pct = 0;
    ESP_RETURN_ON_ERROR(read_reg(REG_STATUS1, &s1), TAG, "status1 read failed");
    ESP_RETURN_ON_ERROR(read_reg(REG_STATUS2, &s2), TAG, "status2 read failed");

    out->vbus = (s1 >> 5) & 1;
    out->battery_present = (s1 >> 3) & 1;
    out->charging = ((s2 >> 5) & 0x07) == 0x01;

    if (!out->battery_present) {
        out->percent = -1;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(read_reg(REG_BAT_PERCENT, &pct), TAG, "percent read failed");
    out->percent = (pct > 100) ? 100 : pct;
    return ESP_OK;
}
