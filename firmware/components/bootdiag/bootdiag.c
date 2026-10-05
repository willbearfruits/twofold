/* bootdiag.c: see bootdiag.h */
#include "bootdiag.h"
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_system.h"

#define CRUMB_MAGIC 0x7f0c2a91u

RTC_NOINIT_ATTR static uint32_t s_magic;
RTC_NOINIT_ATTR static char     s_crumb[16];
static char        s_note[32];
static const char *s_note_p;

void bootdiag_init(void)
{
    const char *why = NULL;
    switch (esp_reset_reason()) {
    case ESP_RST_BROWNOUT: why = "BROWNOUT"; break;
    case ESP_RST_PANIC:    why = "CRASH"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      why = "WATCHDOG"; break;
    default: break;
    }
    bool const have = s_magic == CRUMB_MAGIC && s_crumb[0] && memchr(s_crumb, 0, sizeof s_crumb);
    if (why) {
        if (have) snprintf(s_note, sizeof s_note, "%s: %s", why, s_crumb);
        else snprintf(s_note, sizeof s_note, "RESET: %s", why);
        s_note_p = s_note;
    }
    s_magic = CRUMB_MAGIC;
    s_crumb[0] = 0;
}

void bootdiag_crumb(const char *step)
{
    s_magic = CRUMB_MAGIC;
    if (!step) { s_crumb[0] = 0; return; }
    strncpy(s_crumb, step, sizeof s_crumb - 1);
    s_crumb[sizeof s_crumb - 1] = 0;
}

const char *bootdiag_note(void) { return s_note_p; }
