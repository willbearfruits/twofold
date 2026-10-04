/*
 * display.c: SSD1306 / SH1106 driver with dirty-band flush. See display.h.
 *
 * Transport: IDF 5.5 i2c_master in its default mode. Every transaction is interrupt driven (the ISR
 * refills the 32-byte hardware FIFO); the calling UI task sleeps on a semaphore meanwhile, so core 0 is
 * free for other tasks during a transfer. The S3 I2C peripheral has no DMA. (The driver's "async" mode is
 * flagged experimental by IDF and reports no bus errors, so it is not used.)
 *
 * Flush: per page, compare with the shadow of the last frame sent, find changed column spans, merge spans
 * separated by small gaps (cheaper than a new transaction header), then send each span as ONE transaction:
 *   0x80 B0|page  0x80 col_lo  0x80 col_hi  0x40 data...       (Co=1 command bytes, then a data stream)
 * SH1106: 132-column RAM, visible columns start at 2 (column offset), page addressing only.
 * SSD1306: put into page addressing mode (0x20 0x02) so both controllers take the same commands.
 *
 * Cost at 400 kHz: ~23 us per byte -> a full frame (8 x 135 B) ~ 25 ms, a typical value change (one
 * 20 x 16 px area) ~ 1.5 ms. With CONFIG_TWOFOLD_I2C_FAST (1 MHz) everything is 2.5x faster.
 */
#include <string.h>
#include "display.h"
#include "board_config.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "display";

#define PAGES            8
#define SPAN_MERGE_GAP   8        /* merge spans separated by < 8 unchanged bytes (header is 7 B + addr) */
#define XFER_TIMEOUT_MS  100
#define RETRY_US         2000000

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static display_type_t s_type;
static bool s_present, s_shadow_valid;
static int64_t s_next_retry_us;
static uint8_t s_shadow[DISPLAY_FB_SIZE];
static display_stats_t s_stats;
static uint32_t s_consec_err;

/* Brightness state (what the panel currently has; 0xFFFF = unknown) */
static uint8_t  s_level = 180;
static bool     s_on = true;
static uint16_t s_cur_contrast = 0xFFFF, s_cur_pre = 0xFFFF, s_cur_vcom = 0xFFFF;
static int      s_cur_on = -1;

/* ------------------------------------------------------------------------------------------------ */
static esp_err_t send_cmds(const uint8_t *cmds, size_t n)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[40];
    if (n + 1 > sizeof buf) return ESP_ERR_INVALID_SIZE;
    buf[0] = 0x00;                      /* Co=0, D/C=0: command stream */
    memcpy(buf + 1, cmds, n);
    esp_err_t err = i2c_master_transmit(s_dev, buf, n + 1, XFER_TIMEOUT_MS);
    if (err != ESP_OK) s_stats.errors++;
    return err;
}

static esp_err_t send_span(int page, int col, const uint8_t *data, size_t len)
{
    int const c = col + (s_type == DISPLAY_SH1106 ? 2 : 0);
    uint8_t hdr[7] = { 0x80, (uint8_t)(0xB0 | page), 0x80, (uint8_t)(0x00 | (c & 0x0F)), 0x80,
                       (uint8_t)(0x10 | ((c >> 4) & 0x0F)), 0x40 };
    i2c_master_transmit_multi_buffer_info_t bufs[2] = {
        { .write_buffer = hdr, .buffer_size = sizeof hdr },
        { .write_buffer = (uint8_t *)data, .buffer_size = len },
    };
    esp_err_t err = i2c_master_multi_buffer_transmit(s_dev, bufs, 2, XFER_TIMEOUT_MS);
    if (err != ESP_OK) s_stats.errors++;
    else s_stats.bytes += (uint32_t)(len + sizeof hdr);
    return err;
}

/* ---- brightness ---- */
static void level_to_regs(uint8_t level, uint8_t *contrast, uint8_t *pre, uint8_t *vcom)
{
    /* Perceptual level -> relative luminance (gamma 2.2), then pick the register set. The dim set (lower
     * precharge and VCOMH) is assumed ~0.35x as bright at equal contrast; it is used only where the normal
     * set would need contrast < 16, so the low end gets ~3x more usable steps. Tune on hardware. */
    float const t = (float)level / 255.f;
    float const lum = t * t * (0.4f + 0.6f * t);          /* ~t^2.2, cheap */
    float c_norm = lum * 256.f - 1.f;
    bool const sh = s_type == DISPLAY_SH1106;
    if (c_norm >= 16.f) {
        *contrast = (uint8_t)(c_norm > 255.f ? 255.f : c_norm + 0.5f);
        *pre  = sh ? 0x22 : 0xF1;
        *vcom = sh ? 0x35 : 0x40;
    } else {
        float c_dim = lum * 256.f / 0.35f - 1.f;
        *contrast = (uint8_t)(c_dim < 0.f ? 0.f : (c_dim > 255.f ? 255.f : c_dim + 0.5f));
        *pre  = sh ? 0x11 : 0x22;
        *vcom = 0x00;
    }
}

static void apply_level(void)
{
    if (!s_present) return;
    uint8_t c, p, v;
    level_to_regs(s_level, &c, &p, &v);
    uint8_t cmds[8];
    size_t n = 0;
    if (c != s_cur_contrast) { cmds[n++] = 0x81; cmds[n++] = c; }
    if (p != s_cur_pre)      { cmds[n++] = 0xD9; cmds[n++] = p; }
    if (v != s_cur_vcom)     { cmds[n++] = 0xDB; cmds[n++] = v; }
    if ((int)s_on != s_cur_on) cmds[n++] = s_on ? 0xAF : 0xAE;
    if (!n) return;
    if (send_cmds(cmds, n) == ESP_OK) {
        s_cur_contrast = c;
        s_cur_pre = p;
        s_cur_vcom = v;
        s_cur_on = s_on;
    }
}

void display_set_level(uint8_t level, bool on)
{
    s_level = level;
    s_on = on;
    apply_level();
}

/* ---- init ---- */
static esp_err_t controller_init(void)
{
    static const uint8_t ssd1306[] = {
        0xAE,             /* display off */
        0xD5, 0x80,       /* clock divide / oscillator */
        0xA8, 0x3F,       /* multiplex 64 */
        0xD3, 0x00,       /* display offset */
        0x40,             /* start line 0 */
        0x8D, 0x14,       /* charge pump on */
        0x20, 0x02,       /* page addressing mode */
        0xA1,             /* segment remap (column 127 -> SEG0) */
        0xC8,             /* COM scan descending */
        0xDA, 0x12,       /* COM pins: alternative, no remap */
        0xA4,             /* output follows RAM */
        0xA6,             /* normal (not inverted) */
        0x2E,             /* scrolling off */
    };
    static const uint8_t sh1106[] = {
        0xAE,
        0xD5, 0x80,
        0xA8, 0x3F,
        0xD3, 0x00,
        0x40,
        0xAD, 0x8B,       /* DC-DC on */
        0x32,             /* pump voltage 8.0 V */
        0xA1,
        0xC8,
        0xDA, 0x12,
        0xA4,
        0xA6,
    };
    esp_err_t err = s_type == DISPLAY_SH1106 ? send_cmds(sh1106, sizeof sh1106) : send_cmds(ssd1306, sizeof ssd1306);
    if (err != ESP_OK) return err;
    s_cur_contrast = s_cur_pre = s_cur_vcom = 0xFFFF;
    s_cur_on = 0;
    /* Clear the whole RAM (SH1106: all 132 columns, so the 4 hidden ones hold no garbage). */
    static const uint8_t zeros[132];
    for (int p = 0; p < PAGES && err == ESP_OK; p++) {
        uint8_t hdr[7] = { 0x80, (uint8_t)(0xB0 | p), 0x80, 0x00, 0x80, 0x10, 0x40 };
        i2c_master_transmit_multi_buffer_info_t bufs[2] = {
            { .write_buffer = hdr, .buffer_size = sizeof hdr },
            { .write_buffer = (uint8_t *)zeros, .buffer_size = s_type == DISPLAY_SH1106 ? 132 : 128 },
        };
        err = i2c_master_multi_buffer_transmit(s_dev, bufs, 2, XFER_TIMEOUT_MS);
    }
    if (err != ESP_OK) return err;
    memset(s_shadow, 0, sizeof s_shadow);
    s_shadow_valid = true;
    s_present = true;
    apply_level();          /* contrast/precharge/VCOMH + display on */
    return ESP_OK;
}

static esp_err_t try_init(void)
{
    s_present = false;
    if (i2c_master_probe(s_bus, BOARD_OLED_ADDR, 20) != ESP_OK) {
        s_next_retry_us = esp_timer_get_time() + RETRY_US;
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = controller_init();
    if (err != ESP_OK) {
        s_present = false;
        s_next_retry_us = esp_timer_get_time() + RETRY_US;
        return err;
    }
    s_consec_err = 0;
    ESP_LOGI(TAG, "%s at 0x%02X, %d Hz", s_type == DISPLAY_SH1106 ? "SH1106" : "SSD1306", BOARD_OLED_ADDR, BOARD_I2C_HZ);
    return ESP_OK;
}

esp_err_t display_init(display_type_t type)
{
    s_type = type == DISPLAY_SH1106 ? DISPLAY_SH1106 : DISPLAY_SSD1306;
    if (!s_bus) {
        const i2c_master_bus_config_t bc = {
            .i2c_port = BOARD_I2C_PORT,
            .sda_io_num = (gpio_num_t)BOARD_PIN_I2C_SDA,
            .scl_io_num = (gpio_num_t)BOARD_PIN_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,                 /* synchronous API, interrupt-driven transfers */
            .flags = { .enable_internal_pullup = 1 },   /* backup only; modules have 4.7k pull-ups */
        };
        esp_err_t err = i2c_new_master_bus(&bc, &s_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c bus: %s", esp_err_to_name(err));
            return err;
        }
        const i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = BOARD_OLED_ADDR,
            .scl_speed_hz = BOARD_I2C_HZ,
            .scl_wait_us = 0,
        };
        err = i2c_master_bus_add_device(s_bus, &dc, &s_dev);
        if (err != ESP_OK) return err;
    }
    esp_err_t err = try_init();
    if (err == ESP_ERR_NOT_FOUND) ESP_LOGW(TAG, "no OLED at 0x%02X (will retry)", BOARD_OLED_ADDR);
    return err;
}

esp_err_t display_set_type(display_type_t type)
{
    type = type == DISPLAY_SH1106 ? DISPLAY_SH1106 : DISPLAY_SSD1306;
    if (type == s_type && s_present) return ESP_OK;
    s_type = type;
    if (!s_bus) return display_init(type);
    return try_init();
}

display_type_t display_get_type(void) { return s_type; }
bool display_present(void) { return s_present; }
void display_invalidate(void) { s_shadow_valid = false; }

void display_get_stats(display_stats_t *s) { *s = s_stats; }

/* ---- flush ---- */
esp_err_t display_flush(const uint8_t fb[DISPLAY_FB_SIZE])
{
    if (!s_bus) return ESP_ERR_INVALID_STATE;
    if (!s_present) {
        if (esp_timer_get_time() < s_next_retry_us) return ESP_ERR_NOT_FOUND;
        if (try_init() != ESP_OK) return ESP_ERR_NOT_FOUND;
    }
    int64_t const t0 = esp_timer_get_time();
    esp_err_t res = ESP_OK;
    for (int p = 0; p < PAGES; p++) {
        const uint8_t *row = fb + p * DISPLAY_W;
        uint8_t *sh = s_shadow + p * DISPLAY_W;
        int x = 0;
        while (x < DISPLAY_W) {
            if (s_shadow_valid && row[x] == sh[x]) { x++; continue; }
            /* span start; extend while changes keep appearing within SPAN_MERGE_GAP */
            int const start = x;
            int end = x + 1, gap = 0;
            for (int i = x + 1; i < DISPLAY_W; i++) {
                if (!s_shadow_valid || row[i] != sh[i]) { end = i + 1; gap = 0; }
                else if (++gap >= SPAN_MERGE_GAP) break;
            }
            esp_err_t err = send_span(p, start, row + start, (size_t)(end - start));
            if (err == ESP_OK) {
                memcpy(sh + start, row + start, (size_t)(end - start));
            } else {
                res = err;
                break;
            }
            x = end;
        }
        if (res != ESP_OK) break;
    }
    if (res == ESP_OK) {
        s_shadow_valid = true;
        s_consec_err = 0;
        s_stats.frames++;
    } else {
        s_shadow_valid = false;     /* unknown panel state: resend everything next time */
        if (++s_consec_err >= 5) {
            ESP_LOGW(TAG, "OLED not responding, will re-init");
            s_present = false;
            s_next_retry_us = esp_timer_get_time() + RETRY_US;
        }
    }
    s_stats.last_flush_us = (uint32_t)(esp_timer_get_time() - t0);
    return res;
}
