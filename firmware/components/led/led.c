/* led.c: see led.h. */
#include <math.h>
#include "led.h"
#include "board_config.h"
#include "params.h"
#include "dsp_engine.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

static const char *TAG = "led";

#define LED_TASK_CORE   0
#define LED_TASK_PRIO   2
#define LED_TASK_STACK  3072
#define LED_PERIOD_MS   33
#define LED_MAX         12.f        /* 12/255 = 4.7 % per channel: hard cap */
#define BREATH_PERIOD_S 6.f
#define CLIP_FLASH_MS   150

static led_strip_handle_t s_strip;
static volatile bool s_wifi, s_ble;

void led_set_radio(bool wifi_on, bool ble_on)
{
    s_wifi = wifi_on;
    s_ble = ble_on;
}

typedef struct { float r, g, b; } rgb_t;   /* 0..1, scaled to LED_MAX */

static void show(rgb_t c, float k)
{
    float s = LED_MAX * k;
    uint32_t r = (uint32_t)lrintf(fminf(fmaxf(c.r * s, 0.f), LED_MAX));
    uint32_t g = (uint32_t)lrintf(fminf(fmaxf(c.g * s, 0.f), LED_MAX));
    uint32_t b = (uint32_t)lrintf(fminf(fmaxf(c.b * s, 0.f), LED_MAX));
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void led_task(void *arg)
{
    static const rgb_t WARM = { 1.0f, 0.62f, 0.28f }, BLUE = { 0.05f, 0.30f, 1.0f }, VIOLET = { 0.62f, 0.10f, 1.0f },
                       AMBER = { 1.0f, 0.40f, 0.0f }, RED = { 1.0f, 0.0f, 0.0f }, GREEN = { 0.0f, 1.0f, 0.15f };
    uint32_t clip_seen = 0;
    int64_t flash_until = 0;
    int last_mode = -1;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(LED_PERIOD_MS));
        int64_t const now = esp_timer_get_time();
        int const mode = params_get_int(P_UI_LED);
        dsp_meters_t m;
        dsp_engine_get_meters(&m, false);           /* non-destructive: the UI owns "take" */

        uint32_t const clips = m.in_clip[0] + m.in_clip[1] + m.out_over[0] + m.out_over[1];
        if (clips != clip_seen) {
            if (last_mode >= 0) flash_until = now + CLIP_FLASH_MS * 1000;
            clip_seen = clips;
        }
        last_mode = mode;

        if (mode == 0) {                            /* OFF */
            show(WARM, 0.f);
            continue;
        }
        if (now < flash_until) {
            show(RED, 1.f);
            continue;
        }
        if (mode == 2) {                            /* METER */
            float pk = fmaxf(m.out_peak[0], m.out_peak[1]);
            float db = pk > 1e-6f ? 20.f * log10f(pk) : -120.f;
            float x = (db + 60.f) / 60.f;           /* -60..0 dBFS -> 0..1 */
            x = fminf(fmaxf(x, 0.f), 1.f);
            rgb_t c;
            if (db > -1.f) c = RED;
            else if (x > 0.75f) c = AMBER;
            else c = GREEN;
            show(c, x <= 0.f ? 0.f : 0.1f + 0.9f * x * x);
            continue;
        }
        /* SUBTLE: slow breathing between 20 % and 100 % of the (already tiny) cap */
        if (params_get_bool(P_MUTE)) {
            show(AMBER, 0.5f);
            continue;
        }
        float const ph = (float)(now % (int64_t)(BREATH_PERIOD_S * 1e6f)) / (BREATH_PERIOD_S * 1e6f);
        float const k = 0.2f + 0.8f * (0.5f - 0.5f * cosf(2.f * (float)M_PI * ph));
        show(s_ble ? VIOLET : (s_wifi ? BLUE : WARM), k);
    }
}

esp_err_t led_init(void)
{
    const led_strip_config_t sc = {
        .strip_gpio_num = BOARD_PIN_LED_WS2812,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    const led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags = { .with_dma = false },
    };
    esp_err_t err = led_strip_new_rmt_device(&sc, &rc, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip: %s", esp_err_to_name(err));
        return err;
    }
    led_strip_clear(s_strip);
    if (xTaskCreatePinnedToCoreWithCaps(led_task, "led", LED_TASK_STACK, NULL, LED_TASK_PRIO, NULL, LED_TASK_CORE, (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
