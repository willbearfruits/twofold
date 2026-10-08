/* dsp_port.h: platform glue so the DSP library builds for the ESP32-S3 and for the host. */
#pragma once
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_cpu.h"
#define DSP_HOT            /* whole dsp component is placed in IRAM via linker.lf (noflash) */
#define DSP_ALIGN16        __attribute__((aligned(16)))
static inline uint32_t dsp_cycles(void) { return esp_cpu_get_cycle_count(); }
#define DSP_CPU_HZ         240000000u
#else
#define DSP_HOT
#define DSP_ALIGN16        __attribute__((aligned(16)))
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
static inline uint32_t dsp_cycles(void) { return (uint32_t)__rdtsc(); }
#else
static inline uint32_t dsp_cycles(void) { return 0; }
#endif
#define DSP_CPU_HZ         240000000u   /* used only to express host numbers in "S3 percent" units */
#endif

/* Sub-stage cycle accounting for on-device profiling (read + reset via dsp_engine_get_dbg). */
#define DSP_DBG_N 20
extern uint32_t g_dsp_dbg[DSP_DBG_N];
#define DSP_DBG_ADD(i, since) (g_dsp_dbg[(i)] += dsp_cycles() - (since))

#define DSP_FS             48000
#define DSP_BLOCK_MAX      64           /* engine accepts any n <= DSP_BLOCK_MAX per call */
