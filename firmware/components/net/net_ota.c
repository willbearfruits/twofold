/*
 * net_ota.c: POST /api/ota, raw firmware .bin as the request body (application/octet-stream).
 *
 * Flash writes stall both CPUs, so the whole update is wrapped in dsp_engine_soft_mute(true):
 * the output ramps to digital silence before the first erase and stays silent until the reboot
 * (or is released again if the update fails). Erase happens sector by sector as data arrives
 * (OTA_WITH_SEQUENTIAL_WRITES), so there is no multi-second stall up front.
 * Checks before anything is written: image magic, chip id (ESP32-S3), app descriptor magic and
 * project name (must match the running firmware). esp_ota_end() verifies the image hash.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "dsp_engine.h"
#include "net_priv.h"

static const char *TAG = "net_ota";

#define OTA_CHUNK 4096

static volatile bool    s_active;
static volatile uint8_t s_pct;
static esp_timer_handle_t s_reboot_timer;

bool    net_ota_active(void) { return s_active; }
uint8_t net_ota_pct(void)    { return s_pct; }

static void reboot_cb(void *arg)
{
    ESP_LOGI(TAG, "rebooting into the new firmware");
    esp_restart();
}

static esp_err_t h_ota(httpd_req_t *req)
{
    if (s_active) return net_send_err(req, 409, "an update is already running");
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return net_send_err(req, 500, "no OTA partition in the partition table");
    const size_t hdr_need = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    size_t total = req->content_len;
    if (total < hdr_need) return net_send_err(req, 400, "file too small to be a firmware image");
    if (total > part->size) return net_send_err(req, 413, "image larger than the OTA partition");

    uint8_t *buf = heap_caps_malloc(OTA_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) return net_send_err(req, 500, "no memory");

    s_active = true;
    s_pct = 0;
    dsp_engine_soft_mute(true);
    for (int i = 0; i < 40 && !dsp_engine_is_silent(); i++) vTaskDelay(pdMS_TO_TICKS(5));

    esp_ota_handle_t h = 0;
    bool begun = false;
    const char *err = NULL;
    char newver[33] = "";
    size_t done = 0;
    int timeouts = 0;
    int64_t t0 = esp_timer_get_time();

    while (done < total && !err) {
        size_t want = total - done < OTA_CHUNK ? total - done : OTA_CHUNK, have = 0;
        while (have < want) {
            int n = httpd_req_recv(req, (char *)buf + have, want - have);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++timeouts < 5) continue;
                err = "upload timed out";
                break;
            }
            if (n <= 0) { err = "connection lost"; break; }
            have += (size_t)n;
        }
        if (err) break;
        if (!begun) {
            esp_image_header_t ih;
            esp_app_desc_t nd;
            memcpy(&ih, buf, sizeof ih);
            memcpy(&nd, buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof nd);
            const esp_app_desc_t *cur = esp_app_get_description();
            if (ih.magic != ESP_IMAGE_HEADER_MAGIC) { err = "not an ESP firmware image"; break; }
            if (ih.chip_id != ESP_CHIP_ID_ESP32S3) { err = "image is not built for the ESP32-S3"; break; }
            if (nd.magic_word != ESP_APP_DESC_MAGIC_WORD) { err = "image has no app descriptor"; break; }
            if (strncmp(nd.project_name, cur->project_name, sizeof nd.project_name) != 0) {
                err = "not a TWOFOLD firmware (project name differs)";
                break;
            }
            snprintf(newver, sizeof newver, "%.32s", nd.version);
            esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
            if (e != ESP_OK) { err = "esp_ota_begin failed"; break; }
            begun = true;
            ESP_LOGI(TAG, "OTA %s -> %s, %u bytes into %s", cur->version, newver, (unsigned)total, part->label);
        }
        if (esp_ota_write(h, buf, want) != ESP_OK) { err = "flash write failed"; break; }
        done += want;
        s_pct = (uint8_t)(done * 100 / total);
    }
    free(buf);

    if (!err && begun) {
        esp_err_t e = esp_ota_end(h);       /* frees the handle in every case */
        begun = false;
        if (e == ESP_ERR_OTA_VALIDATE_FAILED) err = "image validation failed (corrupt upload?)";
        else if (e != ESP_OK) err = "esp_ota_end failed";
        else if (esp_ota_set_boot_partition(part) != ESP_OK) err = "could not select the new partition";
    }
    if (begun) esp_ota_abort(h);

    if (err) {
        ESP_LOGE(TAG, "OTA failed after %u/%u bytes: %s", (unsigned)done, (unsigned)total, err);
        s_pct = 0;
        s_active = false;
        dsp_engine_soft_mute(false);
        return net_send_err(req, 400, err);
    }

    ESP_LOGI(TAG, "OTA ok (%u bytes in %lld ms)", (unsigned)total, (long long)((esp_timer_get_time() - t0) / 1000));
    s_pct = 100;
    char r[96];
    snprintf(r, sizeof r, "{\"ok\":true,\"version\":\"%s\",\"rebooting\":true}", newver);
    net_json_headers(req);
    httpd_resp_send(req, r, HTTPD_RESP_USE_STRLEN);
    /* Output stays soft-muted until the reset. */
    if (!s_reboot_timer) {
        const esp_timer_create_args_t a = { .callback = reboot_cb, .name = "ota_reboot" };
        esp_timer_create(&a, &s_reboot_timer);
    }
    if (!s_reboot_timer || esp_timer_start_once(s_reboot_timer, 1000 * 1000) != ESP_OK) reboot_cb(NULL);
    return ESP_OK;
}

esp_err_t net_ota_register(httpd_handle_t hd)
{
    static const httpd_uri_t u = { .uri = "/api/ota", .method = HTTP_POST, .handler = h_ota };
    return httpd_register_uri_handler(hd, &u);
}
