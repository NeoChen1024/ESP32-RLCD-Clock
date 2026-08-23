#include "sensors.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sensors";

/* ---- pins (vendor example) ---- */
#define I2C_SDA_PIN  GPIO_NUM_13
#define I2C_SCL_PIN  GPIO_NUM_14
#define BATT_ADC_CH  ADC_CHANNEL_3   /* GPIO4; divider x3 */
#define BATT_ADC_ATTEN ADC_ATTEN_DB_12
#define BATT_DIVIDER  3.0f

/* ---- SHTC3 ---- */
#define SHTC3_ADDR        0x70
#define SHTC3_CMD_MEAS    0x7866   /* read T first, polling, no clock stretch */
#define SHTC3_CMD_SLEEP   0xB098
#define SHTC3_CMD_WAKEUP  0x3517
#define SHTC3_CRC_POLY    0x31
#define SHTC3_TEMP_OFFSET 4.0f     /* vendor calibration constant */

static i2c_master_bus_handle_t      s_i2c_bus;
static i2c_master_dev_handle_t      s_shtc3;
static adc_oneshot_unit_handle_t    s_adc;
static adc_cali_handle_t            s_adc_cali;
static bool                         s_shtc3_present;

/* ---- SHTC3 helpers ---- */

static uint8_t shtc3_crc8(const uint8_t *data, size_t n)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ SHTC3_CRC_POLY) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static bool shtc3_write_cmd(uint16_t cmd)
{
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xff) };
    return i2c_master_transmit(s_shtc3, buf, sizeof buf, 100) == ESP_OK;
}

/* ---- public API ---- */

bool sensors_start(void)
{
    /* I2C master bus (SDA=13, SCL=14), shared for future peripherals. */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_i2c_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed");
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHTC3_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_shtc3) != ESP_OK) {
        ESP_LOGE(TAG, "i2c add SHTC3 failed");
        return false;
    }

    /* Probe SHTC3: wake + ID read would need a second transaction; a simple
     * write of WAKEUP succeeds iff the device answers. */
    s_shtc3_present = shtc3_write_cmd(SHTC3_CMD_WAKEUP);
    if (s_shtc3_present) {
        /* SHTC3 needs ~240us after wakeup before measuring. */
        vTaskDelay(pdMS_TO_TICKS(1));
        ESP_LOGI(TAG, "SHTC3 present");
    } else {
        ESP_LOGW(TAG, "SHTC3 not detected (I2C no-ack)");
    }

    /* Battery ADC: oneshot + curve-fitted calibration. */
    adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&adc_cfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed");
        return false;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BATT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_oneshot_config_channel(s_adc, BATT_ADC_CH, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "adc config channel failed");
        return false;
    }
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = BATT_ADC_CH,
        .atten = BATT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) != ESP_OK) {
        ESP_LOGE(TAG, "adc calibration failed");
        return false;
    }
    ESP_LOGI(TAG, "battery ADC ready");
    return true;
}

bool sensors_read_temp_humi(float *temp_c, float *rh_pct)
{
    if (!s_shtc3_present || !s_shtc3) return false;

    /* Wake, then poll measurement (T first, 6 bytes + 2 CRCs). */
    if (!shtc3_write_cmd(SHTC3_CMD_WAKEUP)) return false;
    if (!shtc3_write_cmd(SHTC3_CMD_MEAS)) return false;
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t buf[6];
    if (i2c_master_receive(s_shtc3, buf, sizeof buf, 100) != ESP_OK) {
        return false;
    }
    /* Back to sleep after the read to save power (sensor is battery-fed). */
    shtc3_write_cmd(SHTC3_CMD_SLEEP);

    if (shtc3_crc8(buf, 2) != buf[2] || shtc3_crc8(buf + 3, 2) != buf[5]) {
        ESP_LOGW(TAG, "SHTC3 CRC mismatch");
        return false;
    }

    uint16_t raw_t = (uint16_t)((buf[0] << 8) | buf[1]);
    uint16_t raw_h = (uint16_t)((buf[3] << 8) | buf[4]);
    *temp_c = 175.0f * (float)raw_t / 65536.0f - 45.0f - SHTC3_TEMP_OFFSET;
    *rh_pct = 100.0f * (float)raw_h / 65536.0f;
    return true;
}

float sensors_read_batt_v(void)
{
    int raw = 0;
    if (adc_oneshot_read(s_adc, BATT_ADC_CH, &raw) != ESP_OK) {
        return 0.0f;
    }
    int mv = 0;
    if (adc_cali_raw_to_voltage(s_adc_cali, raw, &mv) != ESP_OK) {
        return 0.0f;
    }
    return BATT_DIVIDER * (float)mv / 1000.0f;
}
