/* sim.h: host simulator state shared by ui_sys_sim.c and sim_main.c */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "params.h"
#include "ui_sys.h"

typedef struct {
    uint32_t now;                 /* virtual ms */
    sys_status_t st;
    float music, input;           /* signal intensities */
    bool clip_in, clip_out;       /* one-shot requests */
    uint32_t in_clips, out_overs;
    bool has_creds;
    wifi_state_t wifi_target; uint32_t wifi_target_t;
    ble_state_t ble_target; uint32_t ble_target_t;
    float cho_phase[3];
    uint8_t disp_level; bool disp_on; uint32_t disp_calls;
    uint8_t last_cc, last_cc_val; uint32_t cc_count;
    uint32_t cmd_count;
} sim_t;

extern sim_t SIM;
void sim_reset(void);
