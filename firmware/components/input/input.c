/*
 * input.c: EC11 via PCNT + switch debounce. See input.h.
 *
 * Encoder: one PCNT unit, two channels for full 4x quadrature decoding:
 *   ch0: edge = A, level = B: A rising -> -1, A falling -> +1, inverted while B is low
 *   ch1: edge = B, level = A: B rising -> +1, B falling -> -1, inverted while A is low
 * so every edge of either signal counts +-1 and contact bounce cancels out (+1 -1). The hardware glitch
 * filter drops pulses shorter than ~10 us. The task reads the count every 1 ms and turns it into detents
 * with a full-detent hysteresis: a detent is reported when the count has moved BOARD_ENC_COUNTS_PER_DETENT
 * away from the last reported position (i.e. when the knob snaps into the next rest position). For the
 * usual 4-counts-per-detent EC11 both contacts are open (A = B = 1) at rest, which is used to re-align
 * after missed edges.
 *
 * Switch: 1 ms polled integrator (INPUT_DEBOUNCE_MS samples to flip), timestamps in ms of the debounced edge.
 */
#include "input.h"
#include "board_config.h"
#include "ui.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

static const char *TAG = "input";

#define INPUT_TASK_CORE  0
#define INPUT_TASK_PRIO  9
#define INPUT_TASK_STACK 3072
#define PCNT_LIMIT       10000

static QueueHandle_t s_q;
static pcnt_unit_handle_t s_unit;

QueueHandle_t input_queue(void) { return s_q; }

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void post(ui_ev_type_t type, int16_t delta, uint32_t t)
{
    ui_event_t ev = { .type = type, .delta = delta, .t_ms = t };
    if (xQueueSend(s_q, &ev, 0) != pdTRUE) ESP_LOGD(TAG, "queue full, event dropped");
}

static void input_task(void *arg)
{
    int const cpd = BOARD_ENC_COUNTS_PER_DETENT;
    int base = 0, pending = 0;
    int sw_integ = 0;
    bool sw_down = false;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&wake, 1);
        uint32_t const t = now_ms();

        /* ---- encoder ---- */
        int count = 0;
        if (pcnt_unit_get_count(s_unit, &count) == ESP_OK) {
            int moved = count - base;
            while (moved >= cpd) { pending++; base += cpd; moved -= cpd; }
            while (moved <= -cpd) { pending--; base -= cpd; moved += cpd; }
            if (cpd == 4 && moved != 0 && gpio_get_level(BOARD_PIN_ENC_A) && gpio_get_level(BOARD_PIN_ENC_B))
                base = count;   /* at a rest position: absorb a partial (missed-edge) offset */
        }
        if (pending) {
            int16_t d = (int16_t)(BOARD_ENC_REVERSE ? -pending : pending);
            ui_event_t ev = { .type = UI_EV_ROTATE, .delta = d, .t_ms = t };
            if (xQueueSend(s_q, &ev, 0) == pdTRUE) pending = 0;   /* else keep accumulating, retry next ms */
        }

        /* ---- switch (active low) ---- */
        bool const raw = gpio_get_level(BOARD_PIN_ENC_SW) == 0;
        if (raw) { if (sw_integ < INPUT_DEBOUNCE_MS) sw_integ++; }
        else if (sw_integ > 0) sw_integ--;
        if (!sw_down && sw_integ >= INPUT_DEBOUNCE_MS) {
            sw_down = true;
            post(UI_EV_BTN_DOWN, 0, t);
        } else if (sw_down && sw_integ == 0) {
            sw_down = false;
            post(UI_EV_BTN_UP, 0, t);
        }
    }
}

esp_err_t input_init(void)
{
    s_q = xQueueCreate(INPUT_QUEUE_LEN, sizeof(ui_event_t));
    if (!s_q) return ESP_ERR_NO_MEM;

    const pcnt_unit_config_t uc = { .low_limit = -PCNT_LIMIT, .high_limit = PCNT_LIMIT, .flags.accum_count = 1 };
    esp_err_t err = pcnt_new_unit(&uc, &s_unit);
    if (err != ESP_OK) return err;
    const pcnt_glitch_filter_config_t gf = { .max_glitch_ns = 10000 };
    pcnt_unit_set_glitch_filter(s_unit, &gf);

    const pcnt_chan_config_t ca = { .edge_gpio_num = BOARD_PIN_ENC_A, .level_gpio_num = BOARD_PIN_ENC_B };
    const pcnt_chan_config_t cb = { .edge_gpio_num = BOARD_PIN_ENC_B, .level_gpio_num = BOARD_PIN_ENC_A };
    pcnt_channel_handle_t cha, chb;
    if ((err = pcnt_new_channel(s_unit, &ca, &cha)) != ESP_OK) return err;
    if ((err = pcnt_new_channel(s_unit, &cb, &chb)) != ESP_OK) return err;
    pcnt_channel_set_edge_action(cha, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(cha, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(chb, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(chb, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    /* accum_count needs watch points at the limits to fold overflows into the software accumulator. */
    pcnt_unit_add_watch_point(s_unit, PCNT_LIMIT);
    pcnt_unit_add_watch_point(s_unit, -PCNT_LIMIT);
    if ((err = pcnt_unit_enable(s_unit)) != ESP_OK) return err;
    pcnt_unit_clear_count(s_unit);
    if ((err = pcnt_unit_start(s_unit)) != ESP_OK) return err;

    const gpio_config_t sw = {
        .pin_bit_mask = 1ULL << BOARD_PIN_ENC_SW,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&sw);
    /* PCNT routes A/B through the GPIO matrix with pull-ups; keep them readable as plain inputs too. */
    gpio_input_enable(BOARD_PIN_ENC_A);
    gpio_input_enable(BOARD_PIN_ENC_B);

    if (xTaskCreatePinnedToCoreWithCaps(input_task, "input", INPUT_TASK_STACK, NULL, INPUT_TASK_PRIO, NULL, INPUT_TASK_CORE, (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
