/* bootdiag.c: see bootdiag.h */
#include "bootdiag.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_private/panic_internal.h"
#include "xtensa_context.h"

#define CRUMB_MAGIC 0x7f0c2a91u
#define PANIC_MAGIC 0x5a17c0deu
#define BT_DEPTH    8

RTC_NOINIT_ATTR static uint32_t s_magic;
RTC_NOINIT_ATTR static char     s_crumb[16];
RTC_NOINIT_ATTR static uint32_t s_crumb_heap[2];   /* internal free / largest block when the crumb was set */
RTC_NOINIT_ATTR static struct {
    uint32_t magic;
    char     crumb[16];
    uint32_t heap[2];
    char     reason[64];
    uint32_t pc[BT_DEPTH];
    uint32_t n;
    int      core;
} s_pan;

static char        s_note[32];
static const char *s_note_p;
static char        s_why[12];
static char        s_last_crumb[16];
static char        s_prev_crumb[16];   /* last crumb of the previous boot, whatever the reset reason */
static int         s_reason;
static bool        s_have_panic;

extern bool  g_panic_abort;
extern char *g_panic_abort_details;
void __real_esp_panic_handler(panic_info_t *info);

static IRAM_ATTR void copy_str(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    if (src) for (; i + 1 < n && src[i]; i++) dst[i] = (src[i] >= 32 && src[i] < 127) ? src[i] : ' ';
    dst[i] = 0;
}

/* Runs in the panic context (cache may be off): IRAM code, RTC data only. */
void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    s_pan.magic = PANIC_MAGIC;
    s_pan.core = info->core;
    copy_str(s_pan.crumb, s_magic == CRUMB_MAGIC ? s_crumb : "", sizeof s_pan.crumb);
    s_pan.heap[0] = s_crumb_heap[0]; s_pan.heap[1] = s_crumb_heap[1];
    copy_str(s_pan.reason, (g_panic_abort && g_panic_abort_details) ? g_panic_abort_details : info->reason,
             sizeof s_pan.reason);
    s_pan.n = 0;
    const XtExcFrame *fr = (const XtExcFrame *)info->frame;
    if (fr) {
        esp_backtrace_frame_t f = { .pc = fr->pc, .sp = fr->a1, .next_pc = fr->a0, .exc_frame = fr };
        s_pan.pc[s_pan.n++] = esp_cpu_get_call_addr(esp_cpu_process_stack_pc(f.pc));
        while (s_pan.n < BT_DEPTH && f.next_pc && esp_backtrace_get_next_frame(&f))
            s_pan.pc[s_pan.n++] = esp_cpu_get_call_addr(esp_cpu_process_stack_pc(f.pc));
    }
    __real_esp_panic_handler(info);
}

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
    s_reason = (int)esp_reset_reason();
    snprintf(s_prev_crumb, sizeof s_prev_crumb, "%s", have ? s_crumb : "-");
    s_have_panic = why && s_pan.magic == PANIC_MAGIC && memchr(s_pan.reason, 0, sizeof s_pan.reason) &&
                   s_pan.n <= BT_DEPTH;
    if (why) {
        snprintf(s_why, sizeof s_why, "%s", why);
        snprintf(s_last_crumb, sizeof s_last_crumb, "%s", have ? s_crumb : "");
        if (have) snprintf(s_note, sizeof s_note, "%s: %s", why, s_crumb);
        else snprintf(s_note, sizeof s_note, "RESET: %s", why);
        s_note_p = s_note;
    }
    if (!s_have_panic) s_pan.magic = 0;
    s_magic = CRUMB_MAGIC;
    s_crumb[0] = 0;
}

void bootdiag_crumb(const char *step)
{
    s_magic = CRUMB_MAGIC;
    if (!step) { s_crumb[0] = 0; return; }
    s_crumb_heap[0] = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_crumb_heap[1] = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    strncpy(s_crumb, step, sizeof s_crumb - 1);
    s_crumb[sizeof s_crumb - 1] = 0;
}

const char *bootdiag_note(void) { return s_note_p; }

size_t bootdiag_report(char *buf, size_t n)
{
    if (!n) return 0;
    int k;
    if (!s_note_p) k = snprintf(buf, n, "OK: last boot normal (reset reason %d, previous boot's last step %s) | heap int %u (largest %u, min %u) psram %u",
                                s_reason, s_prev_crumb,
                                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    else if (!s_have_panic) k = snprintf(buf, n, "%s | step %s", s_why, s_last_crumb[0] ? s_last_crumb : "-");
    else {
        k = snprintf(buf, n, "%s | step %s (heap int %u, largest %u) | core %d | %s | bt", s_why,
                     s_pan.crumb[0] ? s_pan.crumb : "-", (unsigned)s_pan.heap[0], (unsigned)s_pan.heap[1],
                     s_pan.core, s_pan.reason);
        for (uint32_t i = 0; i < s_pan.n && k > 0 && (size_t)k < n; i++)
            k += snprintf(buf + k, n - (size_t)k, " 0x%08lx", (unsigned long)s_pan.pc[i]);
    }
    if (k < 0) k = 0;
    return (size_t)k < n ? (size_t)k : n - 1;
}
