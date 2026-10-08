/*
 * ui_task.c: UI task (core 0, prio 5). Owns the display (I2C ISR allocated on core 0 here).
 * Frame pacing: 60 fps (16 ms) while the UI animates, 30 fps otherwise; an input event pulls the next
 * frame in to 16 ms after the previous one. Each frame: drain input -> ui_update -> ui_render -> flush
 * (dirty spans only).
 */
#include "twofold.h"
#include "params.h"
#include "ui.h"
#include "gfx.h"
#include "display.h"
#include "input.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

#define FRAME_FAST_MS 16
#define FRAME_IDLE_MS 33

static uint8_t s_fb[GFX_PAGES][GFX_W];

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void ui_task(void *arg)
{
    display_init(params_get_int(P_UI_OLED) ? DISPLAY_SH1106 : DISPLAY_SSD1306);
    gfx_t g;
    gfx_init(&g, s_fb);
    ui_init(now_ms());
    QueueHandle_t q = input_queue();
    uint32_t last = now_ms();

    for (;;) {
        uint32_t const period = ui_is_animating() ? FRAME_FAST_MS : FRAME_IDLE_MS;
        uint32_t due = last + period;
        uint32_t t = now_ms();
        ui_event_t ev;
        while ((int32_t)(due - t) > 0) {
            if (q && xQueueReceive(q, &ev, pdMS_TO_TICKS(due - t) ? pdMS_TO_TICKS(due - t) : 1) == pdTRUE) {
                ui_event(&ev);
                if ((int32_t)(due - (last + FRAME_FAST_MS)) > 0) due = last + FRAME_FAST_MS;
            }
            t = now_ms();
        }
        while (q && xQueueReceive(q, &ev, 0) == pdTRUE) ui_event(&ev);

        /* Controller type may also be changed from the web editor (P_UI_OLED). */
        display_type_t const want = params_get_int(P_UI_OLED) ? DISPLAY_SH1106 : DISPLAY_SSD1306;
        if (want != display_get_type()) display_set_type(want);

        ui_update(t);
        ui_render(&g);
        display_flush((const uint8_t *)s_fb);
        last = t;
    }
}

esp_err_t ui_task_start(void)
{
    if (xTaskCreatePinnedToCoreWithCaps(ui_task, "ui", TF_UI_STACK, NULL, TF_UI_PRIO, NULL, TF_UI_CORE, (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
