#include "display.h"
#include "clock_health.h"

#include <string.h>
#include <stdatomic.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "render.h"

static const char *TAG = "display";

static u8g2_st7305_t    s_dev;
static SemaphoreHandle_t s_lock;
static _Atomic uint32_t s_frame_count;

uint32_t display_frame_count(void) { return atomic_load(&s_frame_count); }

/*
 * Ink polarity: the RLCD panel renders bit 0 as ink (black) and bit 1 as
 * paper (white) — opposite of the host simulator where u8g2's 1 = ink.
 * The u8g2 buffer always stays in the host convention (1 = black); the
 * inversion to the panel's polarity happens once in the ST7305 DRAW_TILE
 * backend callback. Snapshots therefore export the same bytes as the host.
 *
 * Verified on hardware (2026-08-22): clear buffer (all 0) -> black screen;
 * drawn text (1) -> white. Hence 0 = black, 1 = white on this RLCD.
 */

bool display_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;

    u8g2_st7305_config_t config = u8g2_st7305_default_config();
    /* RLCD 4.2 pins (from the vendor example). */
    config.mosi_io = GPIO_NUM_12;
    config.sclk_io = GPIO_NUM_11;
    config.dc_io = GPIO_NUM_5;
    config.cs_io = GPIO_NUM_40;
    config.reset_io = GPIO_NUM_41;
    config.rotation = U8G2_R1;
    config.tile_buf_height = U8G2_ST7305_TILE_BUF_FULL;

    esp_err_t err = u8g2_st7305_init(&s_dev, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "u8g2_st7305_init failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "ST7305 initialized");
    return true;
}

u8g2_t *display_lock(void)
{
    if (!s_lock) return NULL;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return NULL;
    return u8g2_st7305_get_u8g2(&s_dev);
}

void display_unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* Current wall-clock time in ms (system clock, set by SNTP). */
static int64_t wall_ms_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

/* Align to wall-clock seconds, but bound each wait by monotonic time so
 * backward SNTP steps cannot stall the panel. Early tick wakes recheck the
 * original boundary instead of pushing it into the next second. */
static void display_task(void *arg)
{
    (void)arg;
    for (;;) {
        u8g2_t *g = display_lock();
        if (g) {
            render_frame(g);
            u8g2_SendBuffer(g);   /* triggers the ST7305 flush (inverted there) */
            atomic_fetch_add(&s_frame_count, 1);
            display_unlock();
        }
        int64_t wall_start = wall_ms_now();
        int64_t mono_start = esp_timer_get_time() / 1000;
        for (;;) {
            uint32_t wait_ms = clock_frame_wait_ms(wall_start, mono_start,
                wall_ms_now(), esp_timer_get_time() / 1000);
            if (!wait_ms) break;
            TickType_t ticks = pdMS_TO_TICKS(wait_ms);
            vTaskDelay(ticks ? ticks : 1);
        }
    }
}

/* Start the refresh task (call after display_start). */
void display_task_start(void)
{
    if (xTaskCreate(display_task, "display", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create display task");
    }
}
