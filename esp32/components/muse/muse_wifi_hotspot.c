/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_wifi_hotspot.h"

#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "muse_settings.h"
#include "muse_wifi.h"

static const char *TAG = "muse_hotspot";

#define HOTSPOT_IP "192.168.4.1"
#define HOTSPOT_TIMEOUT_S 300
#define MAX_APS 16

static httpd_handle_t s_server;
static esp_netif_t *s_ap_netif;
static bool s_running;
static bool s_trying;
static char s_ap_name[32];
static char s_try_ssid[MUSE_SSID_MAX + 1];
static char s_last_error[64];

static void url_decode(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < cap; i++) {
        if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char h[3] = {src[i + 1], src[i + 2], 0};
            dst[o++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[o++] = ' ';
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

static bool form_value(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    for (const char *p = body; *p; p++) {
        if ((p == body || p[-1] == '&') && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            char raw[128];
            size_t n = 0;
            while (v[n] && v[n] != '&' && n + 1 < sizeof(raw)) {
                raw[n] = v[n];
                n++;
            }
            raw[n] = '\0';
            url_decode(out, cap, raw);
            return true;
        }
    }
    out[0] = '\0';
    return false;
}

static void html_escape(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < cap; i++) {
        const char *rep = NULL;
        if (src[i] == '<') rep = "&lt;";
        else if (src[i] == '>') rep = "&gt;";
        else if (src[i] == '&') rep = "&amp;";
        else if (src[i] == '"') rep = "&quot;";
        else if (src[i] == '\'') rep = "&#39;";
        if (rep) {
            size_t n = strlen(rep);
            if (o + n >= cap) break;
            memcpy(dst + o, rep, n);
            o += n;
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

/* JS string escape for the onclick fill: backslash, quote, newline. */
static void js_escape(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < cap; i++) {
        if (src[i] == '\'' || src[i] == '\\') {
            if (o + 2 >= cap) break;
            dst[o++] = '\\';
            dst[o++] = src[i];
        } else if (src[i] == '\n' || src[i] == '\r') {
            if (o + 2 >= cap) break;
            dst[o++] = '\\';
            dst[o++] = 'n';
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

static void url_encode(char *dst, size_t cap, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else if (o + 3 < cap) {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 15];
        }
    }
    dst[o] = '\0';
}

static esp_err_t root_handler(httpd_req_t *req)
{
    /* Previously the SSID link relied on req->uri carrying the query
     * string; ESP httpd strips it into a separate query buffer, so the
     * click never filled the field. Parse the query properly, and also
     * fill from the dropdown / link with JS so no reload is needed at all. */
    char qssid[64] = "";
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0) {
        char *query = malloc(qlen + 1);
        if (query && httpd_req_get_url_query_str(req, query, qlen + 1) == ESP_OK) {
            char val[64];
            if (httpd_query_key_value(query, "ssid", val, sizeof(val)) == ESP_OK) {
                url_decode(qssid, sizeof(qssid), val);
            }
        }
        free(query);
    }
    if (!qssid[0]) {
        const char *q = strstr(req->uri, "ssid=");
        if (q) url_decode(qssid, sizeof(qssid), q + 5);
    }
    char esc_q[96];
    html_escape(esc_q, sizeof(esc_q), qssid);

    /* 8K: the old 4K buffer truncated the form when many APs were listed,
     * which is why manual typing sometimes had no usable input at all. */
    char page[8192];
    int n = snprintf(page, sizeof(page),
        "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
        "content=\"width=device-width,initial-scale=1\"><title>Muse 配网</title>"
        "<style>body{font-family:sans-serif;padding:16px}input,select{font-size:16px;width:100%%;box-sizing:border-box;margin:6px 0;padding:8px}button{font-size:16px;width:100%%;padding:10px}</style></head>"
        "<body><h2>Muse 配网</h2><p>热点：%s，配网地址：%s</p>",
        s_ap_name, HOTSPOT_IP);
    muse_wifi_ap_t aps[MAX_APS];
    uint32_t gen = 0;
    int count = muse_wifi_scan_results(aps, MAX_APS, &gen);
    if (count > 0) {
        n += snprintf(page + n, (size_t)(sizeof(page) - n > 0 ? sizeof(page) - n : 0),
            "<p>附近的网络（点一个，自动填到下面）：</p><ul>");
        for (int i = 0; i < count && n < (int)sizeof(page) - 400; i++) {
            char esc[96], js[96], enc[96];
            html_escape(esc, sizeof(esc), aps[i].ssid);
            js_escape(js, sizeof(js), aps[i].ssid);
            url_encode(enc, sizeof(enc), aps[i].ssid);
            n += snprintf(page + n, (size_t)(sizeof(page) - n > 0 ? sizeof(page) - n : 0),
                "<li><a href=\"/?ssid=%s\" onclick=\"document.getElementById('ssid').value='%s';return false;\">%s</a> (%d dBm%s)</li>",
                enc, js, esc, aps[i].rssi, aps[i].secure ? "" : "，开放");
        }
        n += snprintf(page + n, (size_t)(sizeof(page) - n > 0 ? sizeof(page) - n : 0), "</ul>");
        n += snprintf(page + n, (size_t)(sizeof(page) - n > 0 ? sizeof(page) - n : 0),
            "<p>或从下拉框选：<select id=\"ap\" onchange=\"if(this.value)document.getElementById('ssid').value=this.value;\">"
            "<option value=\"\">-- 选一个网络 --</option>");
        for (int i = 0; i < count && n < (int)sizeof(page) - 400; i++) {
            char esc[96];
            html_escape(esc, sizeof(esc), aps[i].ssid);
            n += snprintf(page + n, (size_t)(sizeof(page) - n > 0 ? sizeof(page) - n : 0),
                "<option value=\"%s\">%s</option>", esc, esc);
        }
        n += snprintf(page + n, (size_t)(sizeof(page) - n > 0 ? sizeof(page) - n : 0), "</select></p>");
    } else {
        n += snprintf(page + n, (size_t)(sizeof(page) - n > 0 ? sizeof(page) - n : 0),
            "<p>还没扫到网络，点刷新再看。</p>");
    }
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(page)) n = (int)sizeof(page) - 1;
    n += snprintf(page + n, (size_t)(sizeof(page) - n > 0 ? sizeof(page) - n : 0),
        "<form method=\"POST\" action=\"/save\">"
        "网络名：<input type=\"text\" id=\"ssid\" name=\"ssid\" value=\"%s\" maxlength=\"32\" autocomplete=\"off\"><br>"
        "密码：<input type=\"password\" id=\"pass\" name=\"pass\" maxlength=\"64\" autocomplete=\"off\"><br>"
        "<button type=\"submit\">保存并连接</button></form>"
        "<p><a href=\"/\">刷新列表</a> | <a href=\"/status\">状态</a></p>"
        "<p>当前：%s %s%s%s%s</p></body></html>",
        esc_q, w.state == MUSE_WIFI_CONNECTED ? "已连上" : "未连上",
        w.ssid, s_trying ? "（正在试连 " : "", s_trying ? s_try_ssid : "",
        s_trying ? "）" : "");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

/* Captive-portal probe: phone asks this when it joins a Wi-Fi with no
 * internet; answering 302 to the provisioning page makes it pop the
 * page automatically, like Xiaozhi does. */
static esp_err_t captive_handler(httpd_req_t *req)
{
    (void)req;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"running\":true,\"connected\":%s,\"ssid\":\"%s\",\"trying\":%s,\"error\":\"%s\"}",
        w.state == MUSE_WIFI_CONNECTED ? "true" : "false", w.ssid,
        s_trying ? "true" : "false", s_last_error);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_handler(httpd_req_t *req)
{
    char body[512] = "";
    size_t total = 0;
    int len;
    while (total < sizeof(body) - 1) {
        len = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (len <= 0) break;
        total += (size_t)len;
        if (total >= (size_t)req->content_len) break;
    }
    if (total == 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
    }
    body[total] = '\0';
    char ssid[MUSE_SSID_MAX + 1], pass[MUSE_PASS_MAX + 1];
    form_value(body, "ssid", ssid, sizeof(ssid));
    form_value(body, "pass", pass, sizeof(pass));
    if (!ssid[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
    }
    strlcpy(s_try_ssid, ssid, sizeof(s_try_ssid));
    s_trying = true;
    s_last_error[0] = '\0';
    muse_settings_set_wifi_on(true);
    muse_settings_set_wifi(ssid, pass);
    muse_wifi_apply();
    const char *msg =
        "<!doctype html><html><head><meta charset=utf-8></head><body>"
        "<h2>已保存，正在连接</h2>"
        "<p>板子正在连你选的网络，连上后热点会自动关闭，请连回你家 Wi-Fi 再看板子屏幕。</p>"
        "<p><a href='/status'>查看状态</a></p></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

/* Tiny DNS server: every A query answers 192.168.4.1, so a phone that
 * joins the hotspot and asks for captive.apple.com / connectivitycheck
 * etc. lands on this board instead of timing out with no internet. */
static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) vTaskDelete(NULL);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(53);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        vTaskDelete(NULL);
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[512];
    while (s_running) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n < 12) continue;
        /* Question section ends at the first zero label, +4 for type/class. */
        int qend = 12;
        while (qend < n && buf[qend] != 0) qend += (int)buf[qend] + 1;
        if (qend + 5 > n) continue;
        uint16_t qtype = (uint16_t)((buf[qend + 1] << 8) | buf[qend + 2]);
        if (qtype != 1) continue;  /* only A records get an answer */
        uint8_t resp[512];
        memcpy(resp, buf, (size_t)(qend + 5));
        resp[2] = 0x81;  /* QR=1, RD kept */
        resp[3] = 0x80;  /* RA=1, no error */
        resp[6] = 0; resp[7] = 1;  /* ANCOUNT=1 */
        resp[8] = 0; resp[9] = 0;  /* NSCOUNT=0 */
        resp[10] = 0; resp[11] = 0;  /* ARCOUNT=0 */
        int o = qend + 5;
        resp[o++] = 0xC0; resp[o++] = 0x0C;  /* name pointer to question */
        resp[o++] = 0; resp[o++] = 1;        /* TYPE A */
        resp[o++] = 0; resp[o++] = 1;        /* CLASS IN */
        resp[o++] = 0; resp[o++] = 0; resp[o++] = 0; resp[o++] = 60;  /* TTL 60 */
        resp[o++] = 0; resp[o++] = 4;        /* RDLENGTH 4 */
        resp[o++] = 192; resp[o++] = 168; resp[o++] = 4; resp[o++] = 1;
        sendto(sock, resp, (size_t)o, 0, (struct sockaddr *)&from, from_len);
    }
    close(sock);
    vTaskDelete(NULL);
}

static void watcher_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < HOTSPOT_TIMEOUT_S && s_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_trying && muse_wifi_connected()) {
            ESP_LOGI(TAG, "hotspot: STA connected, stopping hotspot");
            vTaskDelay(pdMS_TO_TICKS(2000));
            break;
        }
    }
    if (s_running) {
        if (s_trying && !muse_wifi_connected()) {
            snprintf(s_last_error, sizeof(s_last_error), "连接失败，热点已关闭，可重开再试");
        }
        muse_wifi_hotspot_stop();
    }
    vTaskDelete(NULL);
}

bool muse_wifi_hotspot_start(void)
{
    if (s_running) return true;
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        ESP_LOGE(TAG, "no STA mac, wifi not started yet");
        return false;
    }
    snprintf(s_ap_name, sizeof(s_ap_name), "MuseGadget-%02X%02X%02X", mac[3], mac[4], mac[5]);

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            ESP_LOGE(TAG, "create AP netif failed");
            return false;
        }
        esp_netif_ip_info_t ip = {0};
        ip.ip.addr = ESP_IP4TOADDR(192, 168, 4, 1);
        ip.gw.addr = ESP_IP4TOADDR(192, 168, 4, 1);
        ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
        esp_netif_dhcps_stop(s_ap_netif);
        esp_netif_set_ip_info(s_ap_netif, &ip);
        /* Hand out this board as DNS too, otherwise the phone never asks
         * our DNS server and the captive pop-up never fires. */
        {
            esp_netif_dns_info_t dns = {0};
            dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 4, 1);
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                   ESP_NETIF_DOMAIN_NAME_SERVER, &dns, sizeof(dns));
        }
        esp_netif_dhcps_start(s_ap_netif);
    }
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_name, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_name);
    ap.ap.channel = 6;
    ap.ap.authmode = WIFI_AUTH_OPEN;  /* user-confirmed: no password */
    ap.ap.max_connection = 4;
    ap.ap.beacon_interval = 100;
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK
        || esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) {
        ESP_LOGE(TAG, "set AP config failed");
        return false;
    }
    muse_wifi_scan();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 16;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return false;
    }
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_handler};
    httpd_uri_t status = {.uri = "/status", .method = HTTP_GET, .handler = status_handler};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_handler};
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &status);
    httpd_register_uri_handler(s_server, &save);
    /* Captive-portal probes, iOS / Android / Windows / Huawei. */
    static const char *probes[] = {
        "/generate_204", "/hotspot-detect.html", "/connecttest.txt",
        "/redirect", "/ncsi.txt", "/fwlink", "/generate_204/", NULL};
    for (int i = 0; probes[i]; i++) {
        httpd_uri_t p = {.uri = probes[i], .method = HTTP_GET, .handler = captive_handler};
        httpd_register_uri_handler(s_server, &p);
    }

    s_running = true;
    s_trying = false;
    s_last_error[0] = '\0';
    ESP_LOGI(TAG, "hotspot on: %s at %s (open, %ds timeout)", s_ap_name, HOTSPOT_IP, HOTSPOT_TIMEOUT_S);
    xTaskCreate(dns_task, "muse_dns", 4096, NULL, 4, NULL);
    xTaskCreate(watcher_task, "muse_hotspot", 4096, NULL, 4, NULL);
    return true;
}

void muse_wifi_hotspot_stop(void)
{
    if (!s_running) return;
    s_running = false;
    s_trying = false;
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "hotspot off, back to STA only");
}

bool muse_wifi_hotspot_running(void)
{
    return s_running;
}

void muse_wifi_hotspot_name(char *out, size_t cap)
{
    if (!out || cap == 0) return;
    strlcpy(out, s_running ? s_ap_name : "", cap);
}
