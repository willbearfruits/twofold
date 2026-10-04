/*
 * input.h: EC11 encoder (PCNT, full 4x quadrature) + debounced push switch -> ui_event_t queue.
 * The input task runs on core 0 (prio 9, 1 ms poll). The UI task consumes the queue.
 */
#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INPUT_QUEUE_LEN  32
#define INPUT_DEBOUNCE_MS 5       /* switch integrator length */

/* Configure PCNT + GPIO and start the input task. */
esp_err_t     input_init(void);
/* Queue of ui_event_t (see ui.h). */
QueueHandle_t input_queue(void);

#ifdef __cplusplus
}
#endif
