/*
 * net_dns.c: captive-portal DNS. Answers every A query with the SoftAP address, every other
 * query type with an empty NOERROR answer (so phones fall back to A). Runs only in AP setup mode.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "net_priv.h"

static const char *TAG = "net_dns";

static TaskHandle_t      s_task;
static volatile bool     s_run;
static uint32_t          s_ip;            /* network byte order */
static StaticSemaphore_t s_done_buf;
static SemaphoreHandle_t s_done;

#define DNS_PORT    53
#define DNS_MAXPKT  512
#define DNS_HDR     12

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* Builds the answer in place. Returns the response length, or 0 to drop the packet. */
static int dns_answer(uint8_t *pkt, int len, int cap)
{
    if (len < DNS_HDR) return 0;
    uint16_t flags = rd16(pkt + 2);
    if (flags & 0x8000) return 0;                 /* a response, not a query */
    if (((flags >> 11) & 0xF) != 0) return 0;     /* opcode != QUERY */
    if (rd16(pkt + 4) != 1) return 0;             /* exactly one question */

    /* walk the QNAME (labels only; compression is not valid in a question) */
    int p = DNS_HDR, total = 0;
    for (;;) {
        if (p >= len) return 0;
        uint8_t l = pkt[p];
        if (l == 0) { p++; break; }
        if (l & 0xC0) return 0;
        total += l + 1;
        if (total > 255) return 0;
        p += 1 + l;
    }
    if (p + 4 > len) return 0;
    uint16_t qtype = rd16(pkt + p), qclass = rd16(pkt + p + 2);
    p += 4;                                        /* end of question; drop any EDNS/additional */

    bool answer = (qtype == 1 /* A */ || qtype == 255 /* ANY */) && (qclass & 0x7FFF) == 1;
    wr16(pkt + 2, (uint16_t)(0x8000 | 0x0400 | (flags & 0x0100)));   /* QR, AA, copy RD, NOERROR */
    wr16(pkt + 6, answer ? 1 : 0);                /* ANCOUNT */
    wr16(pkt + 8, 0);                             /* NSCOUNT */
    wr16(pkt + 10, 0);                            /* ARCOUNT */
    if (!answer) return p;
    if (p + 16 > cap) return 0;
    uint8_t *a = pkt + p;
    wr16(a, 0xC00C);                              /* name: pointer to the question */
    wr16(a + 2, 1);                               /* TYPE A */
    wr16(a + 4, 1);                               /* CLASS IN */
    a[6] = 0; a[7] = 0; a[8] = 0; a[9] = 10;      /* TTL 10 s */
    wr16(a + 10, 4);
    memcpy(a + 12, &s_ip, 4);                     /* already network order */
    return p + 16;
}

static void dns_task(void *arg)
{
    uint8_t pkt[DNS_MAXPKT];
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock >= 0) {
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(DNS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        if (bind(sock, (struct sockaddr *)&a, sizeof a) != 0) {
            ESP_LOGE(TAG, "bind failed");
            close(sock);
            sock = -1;
        }
    }
    while (sock >= 0 && s_run) {
        struct sockaddr_storage from;
        socklen_t fl = sizeof from;
        int n = recvfrom(sock, pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;                      /* timeout: re-check s_run */
        int r = dns_answer(pkt, n, sizeof pkt);
        if (r > 0) sendto(sock, pkt, (size_t)r, 0, (struct sockaddr *)&from, fl);
    }
    if (sock >= 0) close(sock);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

esp_err_t net_dns_start(uint32_t ip_be)
{
    if (s_task) return ESP_OK;
    if (!s_done) s_done = xSemaphoreCreateBinaryStatic(&s_done_buf);
    s_ip = ip_be;
    s_run = true;
    if (xTaskCreatePinnedToCore(dns_task, "net_dns", 3072, NULL, 4, &s_task, 0) != pdPASS) {
        s_task = NULL;
        s_run = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "captive DNS on :53");
    return ESP_OK;
}

void net_dns_stop(void)
{
    if (!s_task) return;
    s_run = false;
    xSemaphoreTake(s_done, pdMS_TO_TICKS(2000));   /* task exits within one 200 ms recv timeout */
    s_task = NULL;
}
