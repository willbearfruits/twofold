/*
 * bootdiag.h: why did we reboot, and what were we doing?
 *
 * The console is on UART0, which is usually not wired, so abnormal resets (brownout, panic, watchdog) are
 * reported on the device itself: modules leave a short breadcrumb in RTC memory (survives a reset) before risky
 * steps; at the next boot bootdiag_note() returns e.g. "CRASH: WIFI START", shown as a toast by the UI.
 * Panics are captured in full (reason / abort message + 8-deep backtrace, via a --wrap of esp_panic_handler) and
 * can be read from a host over USB-MIDI SysEx: send F0 7D 01 F7, the device answers F0 7D 02 <ascii report> F7.
 * Decode the PCs with xtensa-esp32s3-elf-addr2line -pfiaC -e build/twofold.elf <pc...>.
 */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void        bootdiag_init(void);                 /* first thing in app_main */
void        bootdiag_crumb(const char *step);    /* <= 15 chars; NULL clears */
const char *bootdiag_note(void);                 /* NULL after a normal boot */
/* Plain-ASCII report of the last abnormal reset (reason, crumb, panic text, backtrace). Returns length. */
size_t      bootdiag_report(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
