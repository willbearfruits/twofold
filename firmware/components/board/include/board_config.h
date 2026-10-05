/*
 * board_config.h: the single source of truth for TWOFOLD hardware wiring.
 *
 * Target board: ESP32-S3 SuperMini (ESP32-S3FH4R2, 4 MB flash, 2 MB quad PSRAM, native USB-C), wired like the
 * "kit" / gesture-recorder builds (~/Projects/kit/docs/pinmap.md), so an existing kit board runs TWOFOLD unchanged:
 *   I2S 7/8/9/10, I2C 12/13, encoder 4/5/6, speaker-amp enable 11, onboard RGB LED 48, spare 21.
 * The kit's input is an INMP441 I2S microphone (mono, left slot: set INPUT = LEFT for both ears) and its
 * PCM5102A has SCK -> GND and XSMT bridged high. A PCM1808 ADC drops into the same DIN line and takes MCLK from
 * GPIO21 (internal clock mode).
 * Avoided on purpose: strapping 0/3/45/46, USB 19/20, UART0 43/44 (console), 48 is the LED; 1/2/3 are the kit's joystick.
 *
 * The README wiring table is generated from this file by tools/gen_wiring.py.
 * Keep the "PIN:" comment format: <signal> | <module pin> | <notes>.
 */
#pragma once

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

/* ---------------- Audio: one I2S peripheral, full duplex ---------------- */
#define BOARD_I2S_PORT          0
#define BOARD_PIN_I2S_MCLK      21  /* PIN: I2S MCLK  | PCM1808 SCKI          | only if a PCM1808 is fitted (internal clock mode, 256fs = 12.288 MHz); the kit's INMP441 needs none */
#define BOARD_PIN_I2S_BCLK      7   /* PIN: I2S BCK   | PCM5102A BCK, INMP441 SCK, MAX98357A BCLK (PCM1808 BCK) | output (internal) / input (ext osc) */
#define BOARD_PIN_I2S_WS        8   /* PIN: I2S LRCK  | PCM5102A LCK, INMP441 WS, MAX98357A LRC (PCM1808 LRC) | output (internal) / input (ext osc) */
#define BOARD_PIN_I2S_DOUT      9   /* PIN: I2S DOUT  | PCM5102A DIN, MAX98357A DIN | to DAC + speaker amp */
#define BOARD_PIN_I2S_DIN       10  /* PIN: I2S DIN   | INMP441 SD (or PCM1808 DOUT) | from mic / ADC */
#define BOARD_PIN_DAC_XSMT      (-1) /* PCM5102A XSMT is bridged high on the kit board: no soft-mute GPIO */
#define BOARD_PIN_AMP_SD        11  /* PIN: AMP SD    | MAX98357A SD          | speaker amp enable: driven high after the clocks are stable */

/* ---------------- Display (I2C) ---------------- */
#define BOARD_I2C_PORT          0
#define BOARD_PIN_I2C_SDA       12  /* PIN: OLED SDA  | OLED SDA              | 4.7k pull-up to 3V3 (usually on module); shared with the kit's IMU/ToF */
#define BOARD_PIN_I2C_SCL       13  /* PIN: OLED SCL  | OLED SCL              | 400 kHz (1 MHz with CONFIG_TWOFOLD_I2C_FAST) */

/* ---------------- Encoder (EC11) ---------------- */
#define BOARD_PIN_ENC_A         4   /* PIN: ENC A     | EC11 A (CLK)          | internal pull-up, 10 nF to GND recommended */
#define BOARD_PIN_ENC_B         5   /* PIN: ENC B     | EC11 B (DT)           | internal pull-up, 10 nF to GND recommended; EC11 C -> GND */
#define BOARD_PIN_ENC_SW        6   /* PIN: ENC SW    | EC11 switch           | other switch leg -> GND, internal pull-up */

/* ---------------- Status LED ---------------- */
#define BOARD_PIN_LED_WS2812    48  /* PIN: LED       | onboard RGB LED       | on the SuperMini PCB, no wiring */

/* ---------------- Derived configuration ---------------- */
#define BOARD_SAMPLE_RATE       48000
#define BOARD_MCLK_MULTIPLE     256         /* 12.288 MHz from PLL_F160M: 13 + 1/48 divider, see PLAN.md 4.2 */
#define BOARD_EXT_OSC_HZ        12288000

#if defined(CONFIG_TWOFOLD_CLOCK_EXT_OSC)
#define BOARD_CLOCK_EXT_OSC     1           /* PCM1808 is master; ESP32 + PCM5102A are slaves */
#else
#define BOARD_CLOCK_EXT_OSC     0           /* ESP32 is master and provides MCLK to PCM1808 */
#endif

#if defined(CONFIG_TWOFOLD_OLED_SH1106)
#define BOARD_OLED_DEFAULT_SH1106 1
#else
#define BOARD_OLED_DEFAULT_SH1106 0
#endif

#ifdef CONFIG_TWOFOLD_OLED_ADDR
#define BOARD_OLED_ADDR         CONFIG_TWOFOLD_OLED_ADDR
#else
#define BOARD_OLED_ADDR         0x3C
#endif

#if defined(CONFIG_TWOFOLD_I2C_FAST)
#define BOARD_I2C_HZ            1000000
#else
#define BOARD_I2C_HZ            400000
#endif

#ifdef CONFIG_TWOFOLD_ENC_COUNTS_PER_DETENT
#define BOARD_ENC_COUNTS_PER_DETENT CONFIG_TWOFOLD_ENC_COUNTS_PER_DETENT
#else
#define BOARD_ENC_COUNTS_PER_DETENT 4
#endif

#if defined(CONFIG_TWOFOLD_ENC_REVERSE)
#define BOARD_ENC_REVERSE       1
#else
#define BOARD_ENC_REVERSE       0
#endif

#define BOARD_OLED_W            128
#define BOARD_OLED_H            64
