/* twofold.h: firmware-internal glue between app_main, the ctrl/sys tasks, the UI task and ui_sys_fw.c. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ui_sys.h"

/* Task table (PLAN.md 3). Core 1 runs only the audio task (components/audio). */
#define TF_CTRL_CORE     0
#define TF_CTRL_PRIO     8
#define TF_CTRL_STACK    4096
#define TF_CTRL_PERIOD_MS 2
#define TF_SYS_CORE      0
#define TF_SYS_PRIO      4
#define TF_SYS_STACK     6144       /* runs net_wifi_enable() / ble_midi_enable() / NVS writes */
#define TF_UI_CORE       0
#define TF_UI_PRIO       5
#define TF_UI_STACK      6144

/* ctrl.c */
esp_err_t ctrl_start(void);
/* Queue a knob CC (UI, MIDI knob mode) for the ctrl task. Never blocks. */
void      ctrl_send_cc(uint8_t cc, uint8_t value);
/* Queue a slow system command (radios, preset save, factory reset, reboot) for the sys task. */
void      sys_worker_post(sys_cmd_t cmd, int arg);

/* ui_task.c */
esp_err_t ui_task_start(void);
