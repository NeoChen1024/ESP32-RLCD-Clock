#include "rtc_mgr.h"
#include "rtc_clock.h"
#include "sensors.h"
#include "sntp_mgr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include <string.h>
#include <sys/time.h>

/* PCF85063A register map and Control_1 bits (NXP data sheet, sections 7.1–7.3).
 * Register numbers double as indices in a snapshot read starting at 0x00. */
enum {
    PCF85063_I2C_ADDR = 0x51,
    RTC_REG_CONTROL_1 = 0x00,
    RTC_REG_RAM_BYTE = 0x03,
    RTC_REG_SECONDS = 0x04,
    RTC_CTRL_EXTERNAL_TEST = 1u << 7,
    RTC_CTRL_STOP = 1u << 5,
    RTC_CTRL_12_HOUR = 1u << 1,
    RTC_CTRL_UNUSABLE = RTC_CTRL_EXTERNAL_TEST | RTC_CTRL_STOP | RTC_CTRL_12_HOUR,
    RTC_SNAPSHOT_BYTES = RTC_REG_SECONDS + RTC_CLOCK_REGISTER_COUNT,
    RTC_I2C_SPEED_HZ = 400000,
    RTC_I2C_TIMEOUT_MS = 100,
    RTC_READBACK_TOLERANCE_S = 2,
    RTC_WRITER_STACK_BYTES = 4096,
    RTC_WRITER_PRIORITY = 4,
    ANCHOR_MIN_INTERVAL_S = 6 * 60 * 60,
};
/* Application-owned RAM marker, written only after a verified SNTP update.
 * It is not a PCF85063A-defined status value. */
#define RTC_RAM_VALID_MARKER 0xa5u
#define RTC_RAM_INVALID_MARKER 0u

static const char *TAG = "rtc_mgr";
static i2c_master_dev_handle_t s_rtc;
static nvs_handle_t s_nvs;
static bool s_nvs_open, s_boot_used;
static QueueHandle_t s_sync_queue;

static bool read_registers(uint8_t start, uint8_t *out, size_t count)
{
    return s_rtc && i2c_master_transmit_receive(s_rtc, &start, 1, out, count,
                                                RTC_I2C_TIMEOUT_MS) == ESP_OK;
}
static bool write_registers(uint8_t start, const uint8_t *data, size_t count)
{
    uint8_t buffer[1 + RTC_CLOCK_REGISTER_COUNT];
    if (!s_rtc || count > sizeof buffer - 1) return false;
    buffer[0] = start;
    memcpy(buffer + 1, data, count);
    return i2c_master_transmit(s_rtc, buffer, count + 1, RTC_I2C_TIMEOUT_MS) == ESP_OK;
}
static bool write_byte(uint8_t address, uint8_t value)
{
    return write_registers(address, &value, 1);
}
static bool last_anchor(int64_t *epoch)
{
    uint64_t value;
    if (!s_nvs_open || nvs_get_u64(s_nvs, "last_sync", &value) != ESP_OK || value > INT64_MAX)
        return false;
    *epoch = (int64_t)value;
    return true;
}

rtc_mgr_status_t rtc_mgr_status(void)
{
    rtc_mgr_status_t st = {.boot_used = s_boot_used};
    uint8_t r[RTC_SNAPSHOT_BYTES];
    if (!read_registers(RTC_REG_CONTROL_1, r, sizeof r)) return st;
    st.present = true;
    st.oscillator_stopped = (r[RTC_REG_SECONDS + RTC_CLOCK_SECONDS] & RTC_CLOCK_OS_FLAG) ||
                            (r[RTC_REG_CONTROL_1] & RTC_CTRL_STOP);
    st.marker_valid = r[RTC_REG_RAM_BYTE] == RTC_RAM_VALID_MARKER;
    st.calendar_valid = !(r[RTC_REG_CONTROL_1] & RTC_CTRL_UNUSABLE) &&
                        rtc_clock_decode(r + RTC_REG_SECONDS, &st.utc_sec);
    st.anchor_valid = last_anchor(&st.last_sync_sec);
    st.eligible = st.calendar_valid && st.marker_valid && st.anchor_valid &&
                  rtc_clock_eligible(st.utc_sec, st.last_sync_sec);
    return st;
}

static void write_synced_time(void)
{
    struct timeval now;
    uint8_t encoded[RTC_CLOCK_REGISTER_COUNT], control;
    if (gettimeofday(&now, NULL) || !rtc_clock_encode(now.tv_sec, encoded) ||
        !read_registers(RTC_REG_CONTROL_1, &control, 1)) return;

    /* Invalidate the boot marker before any partial update. Keep capacitor
     * and interrupt settings, but force normal running 24-hour mode. */
    if (!write_byte(RTC_REG_RAM_BYTE, RTC_RAM_INVALID_MARKER) ||
        !write_byte(RTC_REG_CONTROL_1, control & ~RTC_CTRL_UNUSABLE) ||
        !write_registers(RTC_REG_SECONDS, encoded, sizeof encoded)) {
        ESP_LOGW(TAG, "RTC write failed");
        return;
    }
    uint8_t readback[RTC_CLOCK_REGISTER_COUNT];
    int64_t verified;
    if (!read_registers(RTC_REG_SECONDS, readback, sizeof readback) ||
        !rtc_clock_decode(readback, &verified) || verified < now.tv_sec ||
        verified > now.tv_sec + RTC_READBACK_TOLERANCE_S ||
        !write_byte(RTC_REG_RAM_BYTE, RTC_RAM_VALID_MARKER)) {
        ESP_LOGW(TAG, "RTC readback failed");
        return;
    }
    int64_t anchor = 0;
    if (s_nvs_open && (!last_anchor(&anchor) || now.tv_sec <= anchor ||
                       now.tv_sec - anchor >= ANCHOR_MIN_INTERVAL_S)) {
        esp_err_t err = nvs_set_u64(s_nvs, "last_sync", (uint64_t)now.tv_sec);
        if (err == ESP_OK) err = nvs_commit(s_nvs);
        if (err != ESP_OK) ESP_LOGW(TAG, "RTC last-sync checkpoint: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "RTC updated from SNTP at %lld UTC", (long long)verified);
}

static void rtc_task(void *arg)
{
    (void)arg;
    int64_t synced_epoch;
    for (;;) if (xQueueReceive(s_sync_queue, &synced_epoch, portMAX_DELAY) == pdTRUE)
        write_synced_time();
}

bool rtc_mgr_start(void)
{
    i2c_master_bus_handle_t bus = sensors_i2c_bus();
    if (!bus) { ESP_LOGW(TAG, "shared I2C bus unavailable"); return false; }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_I2C_ADDR,
        .scl_speed_hz = RTC_I2C_SPEED_HZ,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_rtc) != ESP_OK) return false;
    esp_err_t err = nvs_open("rtc", NVS_READWRITE, &s_nvs);
    s_nvs_open = err == ESP_OK;
    if (!s_nvs_open) ESP_LOGW(TAG, "NVS checkpoint unavailable: %s", esp_err_to_name(err));

    rtc_mgr_status_t st = rtc_mgr_status();
    if (st.eligible) {
        uint32_t age = (uint32_t)(st.utc_sec - st.last_sync_sec);
        struct timeval tv = {.tv_sec = (time_t)st.utc_sec};
        if (settimeofday(&tv, NULL) == 0 && sntp_mgr_seed_rtc(age)) {
            s_boot_used = true;
            ESP_LOGI(TAG, "boot clock from RTC, last SNTP sync %u s ago", age);
        } else ESP_LOGW(TAG, "RTC boot seed failed");
    } else ESP_LOGI(TAG, "RTC not eligible for boot trust (present=%d, OS=%d, marker=%d, anchor=%d)",
                    st.present, st.oscillator_stopped, st.marker_valid, st.anchor_valid);

    s_sync_queue = xQueueCreate(1, sizeof(int64_t));
    if (!s_sync_queue || xTaskCreate(rtc_task, "rtc_writer", RTC_WRITER_STACK_BYTES,
                                     NULL, RTC_WRITER_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "RTC writer task unavailable");
        return false;
    }
    return st.present;
}

void rtc_mgr_on_sync(int64_t utc_sec)
{
    if (s_sync_queue) xQueueOverwrite(s_sync_queue, &utc_sec);
}
