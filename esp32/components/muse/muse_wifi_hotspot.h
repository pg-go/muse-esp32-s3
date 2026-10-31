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

#include <stdbool.h>
#include <stddef.h>

/* Hotspot provisioning (T1-hotspot, additive only).
 *
 * The board opens an open (no-password, user-confirmed) Wi-Fi hotspot
 * named MuseGadget-XXXXXX, serves a small page at 192.168.4.1, and lets a
 * phone pick a home network and type its password there, instead of
 * typing on the small screen. Saving reuses the existing path:
 * muse_settings_set_wifi() + muse_wifi_apply(), so the keeper task joins
 * the new network exactly as it does after a screen or BLE change.
 * On success, or after 5 minutes with no success, the hotspot stops and
 * Wi-Fi returns to station-only mode. The old screen and BLE paths are
 * untouched and remain as fallbacks.
 */
bool muse_wifi_hotspot_start(void);
void muse_wifi_hotspot_stop(void);
bool muse_wifi_hotspot_running(void);
/* Copies the hotspot SSID out (empty when not running). */
void muse_wifi_hotspot_name(char *out, size_t cap);
