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

static esp_err_t root_handler(httpd_req_t *req)
{
    char page[4096];
    int n = snprintf(page, sizeof(page),
        "<!doctype html><html><head><meta charset=utf-8><meta name=viewport "
        "content='width=device-width,initial-scale=1'><title>Muse 配网</title></head>"
        "<body><h2>Muse 配网</h2><p>热点：%s，配网地址：%s</p>",
        s_ap_name, HOTSPOT_IP);
    muse_wifi_ap_t aps[MAX_APS];
    uint32_t gen = 0;
    int count = muse_wifi_scan_results(aps, MAX_APS, &gen);
    if (count > 0) {
        n += snprintf(page + n, sizeof(page) - n, "<p>附近的网络（点一个填密码）：</p><ul>");
        for (int i = 0; i < count && n < (int)sizeof(page) - 200; i++) {
            char esc[64];
            html_escape(esc, sizeof(esc), aps[i].ssid);
            n += snprintf(page + n, sizeof(page) - n,
                "<li><a href='/?ssid=%s'>%s</a> (%d dBm%s)</li>",
                esc, esc, aps[i].rssi, aps[i].secure ? "" : "，开放");
        }
        n += snprintf(page + n, sizeof(page) - n, "</ul>");
    } else {
        n += snprintf(page + n, sizeof(page) - n, "<p>还没扫到网络，点刷新再看。</p>");
    }
    char qssid[64] = "";
    {
        const char *q = strstr(req->uri, "ssid=");
        if (q) url_decode(qssid, sizeof(qssid), q + 5);
    }
    char esc_q[64];
    html_escape(esc_q, sizeof(esc_q), qssid);
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    n += snprintf(page + n, sizeof(page) - n,
        "<form method=post action=/save>"
        "网络名：<input name=ssid value='%s' maxlength=32><br>"
        "密码：<input type=password name=pass maxlength=64><br>"
        "<input type=submit value='保存并连接'></form>"
        "<p><a href='/'>刷新列表</a> | <a href='/status'>状态</a></p>"
        "<p>当前：%s %s%s%s%s</p></body></html>",
        esc_q, w.state == MUSE_WIFI_CONNECTED ? "已连上" : "未连上",
        w.ssid, s_trying ? "（正在试连 " : "", s_trying ? s_try_ssid : "",
        s_trying ? "）" : "");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
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
    char body[256] = "";
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
    }
    body[len] = '\0';
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
    cfg.max_uri_handlers = 8;
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

    s_running = true;
    s_trying = false;
    s_last_error[0] = '\0';
    ESP_LOGI(TAG, "hotspot on: %s at %s (open, %ds timeout)", s_ap_name, HOTSPOT_IP, HOTSPOT_TIMEOUT_S);
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
