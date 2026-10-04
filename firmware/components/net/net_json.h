/*
 * net_json.h: a tiny streaming JSON writer.
 *
 * Writes into a caller-provided buffer. With a request attached, a full buffer is flushed as an
 * HTTP chunk, so arbitrarily large responses (e.g. /api/params) need only a small buffer.
 * Without a request, overflow sets `err` (the caller decides what to do).
 * Nesting depth is limited to 16; commas are inserted automatically.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

typedef struct {
    char        *buf;
    size_t       cap, len;
    httpd_req_t *req;          /* NULL: fixed buffer mode */
    bool         err;
    bool         chunked;      /* at least one chunk has been sent */
    int          depth;
    uint32_t     comma;        /* bit d set: next value at depth d needs a leading comma */
} jw_t;

void jw_init(jw_t *w, char *buf, size_t cap, httpd_req_t *req);
void jw_obj(jw_t *w, const char *key);        /* key NULL: array element or root */
void jw_obj_end(jw_t *w);
void jw_arr(jw_t *w, const char *key);
void jw_arr_end(jw_t *w);
void jw_str(jw_t *w, const char *key, const char *val);   /* val NULL -> null */
void jw_num(jw_t *w, const char *key, double v, int prec); /* %.{prec}g, non-finite -> null */
void jw_fix(jw_t *w, const char *key, double v, int decimals); /* %.{decimals}f */
void jw_int(jw_t *w, const char *key, long v);
void jw_bool(jw_t *w, const char *key, bool v);
void jw_null(jw_t *w, const char *key);
void jw_raw(jw_t *w, const char *s, size_t n);  /* unchecked raw bytes */
/* Request mode: sends remaining bytes and terminates the chunked response (or sends the whole
 * buffer in one go if it never overflowed). Buffer mode: NUL-terminates; returns ESP_FAIL on overflow. */
esp_err_t jw_finish(jw_t *w);
