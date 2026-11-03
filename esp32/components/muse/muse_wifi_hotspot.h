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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

/*
 * Hotspot provisioning: Xiaozhi esp-wifi-connect port (see
 * muse_wifi_config_ap.cpp / muse_dns_server.cpp / muse_ssid_manager.cpp
 * and assets/wifi_configuration*.html, copied verbatim except for the
 * Muse-only adaptations noted in muse_wifi_config_ap.cpp).
 * This file is only the thin C wrapper the settings screen calls:
 * start/stop/running/name, plus a 5-minute abandon timeout. The portal
 * itself (DNS, captive probes, scan, submit, ConnectToWifi retry) is
 * Xiaozhi's, not a hand-written page.
 */
bool muse_wifi_hotspot_start(void);
void muse_wifi_hotspot_stop(void);
bool muse_wifi_hotspot_running(void);
/* Copies the hotspot SSID out (empty when not running). */
void muse_wifi_hotspot_name(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
