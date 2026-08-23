#include "display.h"

#include <string.h>
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

/* Periodic refresh: render current state and flush to the panel, aligned to
 * the start of each wall-clock second so the face updates crisply at the
 * second boundary.
 *
 * vTaskDelay counts FreeRTOS ticks, NOT wall-clock ms, and the two can drift
 * by a ms or two (systick vs NTP-adjusted gettimeofday). A raw delay to the
 * computed boundary can therefore wake slightly BEFORE the real second
 * boundary, and gettimeofday would still return the previous second — that
 * frame then shows the old second for a full second (51->52->52->54). So we
 * re-check the wall clock after waking and keep sleeping until it has
 * actually crossed the boundary, then render. */
static void display_task(void *arg)
{
    (void)arg;
    for (;;) {
        u8g2_t *g = display_lock();
        if (g) {
            render_frame(g);
            u8g2_SendBuffer(g);   /* triggers the ST7305 flush (inverted there) */
            display_unlock();
        }
        /* Sleep until just after the next wall-clock second boundary, then
         * render. The target is computed ONCE; after waking we only check
         * whether the wall clock has crossed it. Recomputing "next" each
         * iteration would keep pushing the goal forward when systick and the
         * wall clock drift (wake lands 1-2ms past the boundary, sleep stays
         * ~998ms, loop never exits). Early wakes (systick fast) re-sleep the
         * remaining gap; late wakes render immediately with the new second. */
        int64_t target = (wall_ms_now() / 1000 + 1) * 1000;
        for (;;) {
            int64_t now_ms = wall_ms_now();
            if (now_ms >= target) break;
            int64_t remain = target - now_ms;
            vTaskDelay((TickType_t)(remain > 0 ? remain : 1));
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
