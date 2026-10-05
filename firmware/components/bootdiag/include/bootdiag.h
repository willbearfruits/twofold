/*
 * bootdiag.h: why did we reboot, and what were we doing?
 *
 * The console is on UART0, which is usually not wired, so abnormal resets (brownout, panic, watchdog) are
 * reported on the device itself: modules leave a short breadcrumb in RTC memory (survives a reset) before risky
 * steps; at the next boot bootdiag_note() returns e.g. "BROWNOUT: WIFI START", shown as a toast by the UI.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void        bootdiag_init(void);                 /* first thing in app_main */
void        bootdiag_crumb(const char *step);    /* <= 15 chars; NULL clears */
const char *bootdiag_note(void);                 /* NULL after a normal boot */

#ifdef __cplusplus
}
#endif
