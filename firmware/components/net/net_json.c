/* net_json.c: see net_json.h */
#include "net_json.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void jw_init(jw_t *w, char *buf, size_t cap, httpd_req_t *req)
{
    memset(w, 0, sizeof *w);
    w->buf = buf;
    w->cap = cap;
    w->req = req;
}

static void flush(jw_t *w)
{
    if (!w->req || w->err) { w->err = true; return; }
    if (w->len && httpd_resp_send_chunk(w->req, w->buf, (ssize_t)w->len) != ESP_OK) w->err = true;
    w->chunked = true;
    w->len = 0;
}

void jw_raw(jw_t *w, const char *s, size_t n)
{
    while (n && !w->err) {
        /* keep one byte spare for the terminating NUL in buffer mode */
        size_t room = w->cap - 1 - w->len;
        if (room == 0) { flush(w); continue; }
        size_t k = n < room ? n : room;
        memcpy(w->buf + w->len, s, k);
        w->len += k;
        s += k;
        n -= k;
    }
}

static inline void putc_(jw_t *w, char c) { jw_raw(w, &c, 1); }

static void put_escaped(jw_t *w, const char *s)
{
    putc_(w, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char e[8];
        switch (*p) {
        case '"':  jw_raw(w, "\\\"", 2); break;
        case '\\': jw_raw(w, "\\\\", 2); break;
        case '\n': jw_raw(w, "\\n", 2); break;
        case '\r': jw_raw(w, "\\r", 2); break;
        case '\t': jw_raw(w, "\\t", 2); break;
        default:
            if (*p < 0x20 || *p == 0x7f) {
                int n = snprintf(e, sizeof e, "\\u%04x", *p);
                jw_raw(w, e, (size_t)n);
            } else {
                putc_(w, (char)*p);
            }
        }
    }
    putc_(w, '"');
}

static void prefix(jw_t *w, const char *key)
{
    if (w->depth > 0) {
        uint32_t bit = 1u << (w->depth & 31);
        if (w->comma & bit) putc_(w, ',');
        w->comma |= bit;
    }
    if (key) {
        put_escaped(w, key);
        putc_(w, ':');
    }
}

static void open_(jw_t *w, const char *key, char c)
{
    prefix(w, key);
    putc_(w, c);
    if (w->depth < 16) w->depth++; else w->err = true;
    w->comma &= ~(1u << (w->depth & 31));
}

static void close_(jw_t *w, char c)
{
    putc_(w, c);
    if (w->depth > 0) w->depth--;
}

void jw_obj(jw_t *w, const char *key) { open_(w, key, '{'); }
void jw_obj_end(jw_t *w) { close_(w, '}'); }
void jw_arr(jw_t *w, const char *key) { open_(w, key, '['); }
void jw_arr_end(jw_t *w) { close_(w, ']'); }

void jw_str(jw_t *w, const char *key, const char *val)
{
    prefix(w, key);
    if (val) put_escaped(w, val); else jw_raw(w, "null", 4);
}

void jw_num(jw_t *w, const char *key, double v, int prec)
{
    prefix(w, key);
    if (!isfinite(v)) { jw_raw(w, "null", 4); return; }
    char b[32];
    int n = snprintf(b, sizeof b, "%.*g", prec, v);
    if (n > 0) jw_raw(w, b, (size_t)n);
}

void jw_fix(jw_t *w, const char *key, double v, int decimals)
{
    prefix(w, key);
    if (!isfinite(v)) { jw_raw(w, "null", 4); return; }
    char b[32];
    int n = snprintf(b, sizeof b, "%.*f", decimals, v);
    if (n > 0) jw_raw(w, b, (size_t)n);
}

void jw_int(jw_t *w, const char *key, long v)
{
    prefix(w, key);
    char b[24];
    int n = snprintf(b, sizeof b, "%ld", v);
    if (n > 0) jw_raw(w, b, (size_t)n);
}

void jw_bool(jw_t *w, const char *key, bool v)
{
    prefix(w, key);
    if (v) jw_raw(w, "true", 4); else jw_raw(w, "false", 5);
}

void jw_null(jw_t *w, const char *key)
{
    prefix(w, key);
    jw_raw(w, "null", 4);
}

esp_err_t jw_finish(jw_t *w)
{
    if (!w->req) {
        w->buf[w->len < w->cap ? w->len : w->cap - 1] = 0;
        return w->err ? ESP_FAIL : ESP_OK;
    }
    if (w->err) {
        /* mid-stream failure: terminate the chunked response so the socket is not left hanging */
        if (w->chunked) httpd_resp_send_chunk(w->req, NULL, 0);
        return ESP_FAIL;
    }
    if (!w->chunked) return httpd_resp_send(w->req, w->buf, (ssize_t)w->len);
    if (w->len && httpd_resp_send_chunk(w->req, w->buf, (ssize_t)w->len) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(w->req, NULL, 0);
}
