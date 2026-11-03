/*
 * Thin C wrapper around the Xiaozhi WifiConfigurationAp port.
 * Kept deliberately small: start/stop/running/name for the settings
 * screen, plus the 5-minute abandon timeout (Xiaozhi has no timeout
 * because its board is in config mode; Muse can already be on Wi-Fi,
 * so an abandoned hotspot must not stay open forever). Success closes
 * via Xiaozhi's own /done.html -> /exit path, not via this watcher.
 * The watcher never stops on STA-connected: Xiaozhi's ConnectToWifi
 * deliberately test-connects in APSTA mode, and that must not kill
 * the portal mid-submit.
 */
#include "muse_wifi_hotspot.h"
#include "muse_wifi_config_ap.h"
extern "C" {
#include "muse_settings.h"
#include "muse_wifi.h"
}

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static WifiConfigurationAp *s_ap;
static bool s_running;
static TaskHandle_t s_watcher;

static void watcher_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 300 && s_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (s_running) {
        muse_wifi_hotspot_stop();
    }
    s_watcher = NULL;
    vTaskDelete(NULL);
}

bool muse_wifi_hotspot_start(void)
{
    if (s_running) {
        return true;
    }
    s_ap = new WifiConfigurationAp();
    s_ap->SetSsidPrefix("MuseGadget");
    s_ap->SetLanguage("zh-CN");
    s_ap->SetShowOtaConfig(false);
    s_ap->SetShowSleepConfig(false);
    s_ap->OnExitRequested([]() { muse_wifi_hotspot_stop(); });
    s_ap->Start();
    s_running = true;
    xTaskCreate(watcher_task, "hotspot_watch", 4096, NULL, 5, &s_watcher);
    return true;
}

void muse_wifi_hotspot_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;
    if (s_ap) {
        s_ap->Stop();
        delete s_ap;
        s_ap = NULL;
    }
    /* Back to station-only: rejoin whatever settings now hold (the new
     * network after a save, or the previous one after an abandon). */
    muse_wifi_apply();
}

bool muse_wifi_hotspot_running(void)
{
    return s_running;
}

void muse_wifi_hotspot_name(char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (s_ap && s_running) {
        std::string ssid = s_ap->GetSsid();
        strncpy(out, ssid.c_str(), cap - 1);
        out[cap - 1] = '\0';
    }
}
