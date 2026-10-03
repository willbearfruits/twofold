/*
 * board_config.h: the single source of truth for TWOFOLD hardware wiring.
 *
 * Target board: Waveshare ESP32-S3-Zero (ESP32-S3FH4R2, 4 MB flash, 2 MB quad PSRAM).
 * Exposed GPIOs: 1-18, 21 (onboard WS2812), 38-45, 43/44 (UART0). 19/20 are the native USB D-/D+.
 * Avoided on purpose: strapping 0/3/45/46, USB 19/20, UART0 43/44 (console), JTAG 39-42, GPIO21 (LED only).
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
#define BOARD_PIN_I2S_MCLK      4   /* PIN: I2S MCLK  | PCM1808 SCKI          | internal clock mode only (256fs = 12.288 MHz) */
#define BOARD_PIN_I2S_BCLK      5   /* PIN: I2S BCK   | PCM1808 BCK, PCM5102A BCK | output (internal) / input (ext osc) */
#define BOARD_PIN_I2S_WS        6   /* PIN: I2S LRCK  | PCM1808 LRC, PCM5102A LCK | output (internal) / input (ext osc) */
#define BOARD_PIN_I2S_DOUT      7   /* PIN: I2S DOUT  | PCM5102A DIN          | to DAC */
#define BOARD_PIN_I2S_DIN       8   /* PIN: I2S DIN   | PCM1808 DOUT          | from ADC */
#define BOARD_PIN_DAC_XSMT      9   /* PIN: DAC XSMT  | PCM5102A XSMT         | soft mute, high = play (open the module's XSMT jumper) */

/* ---------------- Display (I2C) ---------------- */
#define BOARD_I2C_PORT          0
#define BOARD_PIN_I2C_SDA       1   /* PIN: OLED SDA  | OLED SDA              | 4.7k pull-up to 3V3 (usually on module) */
#define BOARD_PIN_I2C_SCL       2   /* PIN: OLED SCL  | OLED SCL              | 400 kHz (1 MHz with CONFIG_TWOFOLD_I2C_FAST) */

/* ---------------- Encoder (EC11) ---------------- */
#define BOARD_PIN_ENC_A         10  /* PIN: ENC A     | EC11 A                | internal pull-up, 10 nF to GND recommended */
#define BOARD_PIN_ENC_B         11  /* PIN: ENC B     | EC11 B                | internal pull-up, 10 nF to GND recommended; EC11 C -> GND */
#define BOARD_PIN_ENC_SW        12  /* PIN: ENC SW    | EC11 switch           | other switch leg -> GND, internal pull-up */

/* ---------------- Status LED ---------------- */
#define BOARD_PIN_LED_WS2812    21  /* PIN: LED       | onboard WS2812        | on the S3-Zero PCB, no wiring */

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
