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

/*
 * WAV recording to the TF card. See muse_wav_record.h for the contract:
 * best-effort only, never breaks conversation, TF pins come from Kconfig
 * and are not guessed (they default to -1 = unconfigured).
 *
 * Storage layout on the card (FAT, /sdcard/muse/):
 *   muse_NNNNN_user.wav, muse_NNNNN_reply.wav, muse_NNNNN_reply.txt
 * NNNNN is a per-boot-persistent turn counter: at mount, the directory
 * is scanned for the highest existing number and counting resumes after
 * it, so a reboot never overwrites an earlier turn.
 *
 * WAV format is the pipeline's own: 16 kHz, 16-bit, mono PCM, 44-byte
 * header, byte rate 32000, block align 2. The header is written as a
 * placeholder at open and rewritten with the true sizes at close, so a
 * file that is still open (power cut mid-turn) has a placeholder header
 * and a reader can still find its data chunk after the 44 bytes.
 */

#include "muse_wav_record.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"  /* sdmmc_slot_config_t, SDMMC_SLOT_CONFIG_DEFAULT */

#if CONFIG_MUSE_WAV_RECORD

static const char *TAG = "muse_wav_record";

#define WAV_RATE 16000
#define WAV_DIR "/sdcard/muse"
#define WAV_MOUNT "/sdcard"

static bool s_mounted;
static sdmmc_card_t *s_card;
static unsigned s_next_turn = 1; /* set by scanning WAV_DIR at mount */
static unsigned s_turn;          /* 0 = no turn in progress */
static bool s_turn_active;

static FILE *s_user;
static char s_user_path[64];
static uint32_t s_user_bytes;
static bool s_user_failed;

static FILE *s_reply;
static char s_reply_path[64];
static uint32_t s_reply_bytes;

static FILE *s_txt;
static char s_txt_path[64];

static void put_le(uint8_t *p, uint32_t v, int n)
{
    for (int i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/* A real WAV header for `data_bytes` of 16 kHz PCM16 mono. */
static void wav_header(uint8_t h[44], uint32_t data_bytes)
{
    memcpy(h, "RIFF", 4);
    put_le(h + 4, 36 + data_bytes, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);
    put_le(h + 20, 1, 2); /* PCM */
    put_le(h + 22, 1, 2); /* mono */
    put_le(h + 24, WAV_RATE, 4);
    put_le(h + 28, WAV_RATE * 2, 4); /* byte rate */
    put_le(h + 32, 2, 2);            /* block align */
    put_le(h + 34, 16, 2);           /* bits per sample */
    memcpy(h + 36, "data", 4);
    put_le(h + 40, data_bytes, 4);
}

static void scan_next_turn(void)
{
    DIR *d = opendir(WAV_DIR);
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        unsigned n = 0;
        /* muse_00001_user.wav / muse_00001_reply.wav share the number. */
        if (sscanf(e->d_name, "muse_%u_", &n) == 1 && n >= s_next_turn) {
            s_next_turn = n + 1;
        }
    }
    closedir(d);
}

/* Mount once. format_if_failed=false is the boot path: a card that
 * fails to mount is left alone, never wiped by surprise (it may hold
 * photos, or be exFAT/NTFS, which this FatFs build cannot read).
 * format_if_failed=true is only for the explicit format command below:
 * on FR_NO_FILESYSTEM it runs f_mkfs (FM_ANY: FAT12/16/32 by card size,
 * never exFAT, which FF_FS_EXFAT=0 disables) and mounts the result. */
static esp_err_t mount_card(bool format_if_failed)
{
    int clk = CONFIG_MUSE_WAV_SD_CLK;
    int cmd = CONFIG_MUSE_WAV_SD_CMD;
    int d0 = CONFIG_MUSE_WAV_SD_D0;
    if (clk < 0 || cmd < 0 || d0 < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT; /* vendor: TF slot is 1-SD mode */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = (gpio_num_t)clk;
    slot.cmd = (gpio_num_t)cmd;
    slot.d0 = (gpio_num_t)d0;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = format_if_failed,
        .max_files = 3,
        .allocation_unit_size = 16 * 1024,
    };
    s_card = NULL;
    return esp_vfs_fat_sdmmc_mount(WAV_MOUNT, &host, &slot, &mount_cfg, &s_card);
}

static bool finish_mount(void)
{
    /* mkdir is fine if the directory already exists. */
    if (mkdir(WAV_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "cannot create %s: recording disabled", WAV_DIR);
        esp_vfs_fat_sdcard_unmount(WAV_MOUNT, s_card);
        s_card = NULL;
        return false;
    }
    scan_next_turn();
    s_mounted = true;
    ESP_LOGI(TAG, "TF mounted at %s, next turn %05u (SD 1-bit CLK %d CMD %d D0 %d)",
             WAV_MOUNT, s_next_turn, CONFIG_MUSE_WAV_SD_CLK, CONFIG_MUSE_WAV_SD_CMD,
             CONFIG_MUSE_WAV_SD_D0);
    return true;
}

void muse_wav_record_init(void)
{
    if (s_mounted) {
        return;
    }
    if (CONFIG_MUSE_WAV_SD_CLK < 0 || CONFIG_MUSE_WAV_SD_CMD < 0 || CONFIG_MUSE_WAV_SD_D0 < 0) {
        ESP_LOGW(TAG,
                 "TF recording disabled: SD pins not configured "
                 "(menuconfig: Muse -> WAV recording to TF card -> "
                 "SD CLK/CMD/D0). Conversation is unaffected; set the "
                 "real SZP TF wiring and rebuild to enable it.");
        return;
    }
    esp_err_t err = mount_card(false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TF mount failed (%s): recording disabled, conversation unaffected",
                 esp_err_to_name(err));
        return;
    }
    (void)finish_mount();
}

/* Explicit, destructive, never automatic. Called only from the serial
 * console's two-step tf.format / tf.format.confirm (see muse_input.c).
 * After a successful format the card holds a fresh FAT filesystem and
 * nothing else: /sdcard/muse is recreated empty, so numbering restarts
 * at muse_00001. Refuses while a turn is in progress, so no recording
 * is deleted mid-press. Do not press BOOT while this runs. */
bool muse_wav_record_format_card(void)
{
    if (s_turn_active) {
        ESP_LOGW(TAG, "TF format refused: a turn is in progress");
        return false;
    }
    ESP_LOGW(TAG, "TF format: ALL data on the card will be erased, new FAT filesystem");
    if (!s_mounted) {
        /* Try a plain mount first: if the card already has a readable
         * FAT (e.g. inserted after boot), that gives a card handle for
         * the wipe below. Only if that fails (unformatted, exFAT, NTFS,
         * corrupted) fall back to mount-with-format, which wipes it. */
        if (mount_card(false) == ESP_OK) {
            s_mounted = true;
        } else if (mount_card(true) == ESP_OK) {
            s_next_turn = 1;
            if (finish_mount()) {
                ESP_LOGI(TAG, "TF formatted to FAT (mount-time format), next turn 00001");
                return true;
            }
            return false;
        } else {
            ESP_LOGW(TAG, "TF format failed: card absent, bad contact, or unreadable");
            return false;
        }
    }
    /* Mounted: esp_vfs_fat_sdcard_format wipes and mounts the card back. */
    esp_err_t err = esp_vfs_fat_sdcard_format(WAV_MOUNT, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TF format failed (%s)", esp_err_to_name(err));
        /* The format API recycles resources if it cannot mount back. */
        s_mounted = false;
        s_card = NULL;
        return false;
    }
    s_next_turn = 1;
    if (!finish_mount()) {
        return false;
    }
    ESP_LOGI(TAG, "TF formatted to FAT, all data erased, next turn 00001");
    return true;
}

bool muse_wav_record_available(void)
{
    return s_mounted;
}

void muse_wav_record_begin_turn(void)
{
    if (!s_mounted) {
        return;
    }
    muse_wav_record_cancel_turn();
    s_turn = s_next_turn++;
    s_turn_active = true;
}

static FILE *open_wav(const char *path, uint32_t *bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "cannot open %s", path);
        return NULL;
    }
    uint8_t h[44];
    wav_header(h, 0); /* placeholder, rewritten at close */
    if (fwrite(h, 1, sizeof(h), f) != sizeof(h)) {
        ESP_LOGW(TAG, "cannot write header to %s", path);
        fclose(f);
        return NULL;
    }
    *bytes = 0;
    return f;
}

static void finish_wav(FILE **fp, const char *path, uint32_t bytes)
{
    FILE *f = *fp;
    *fp = NULL;
    if (!f) {
        return;
    }
    uint8_t h[44];
    wav_header(h, bytes);
    if (fseek(f, 0, SEEK_SET) == 0) {
        (void)fwrite(h, 1, sizeof(h), f);
    }
    fclose(f);
    ESP_LOGI(TAG, "saved %s (%u bytes PCM, %.1f s)", path, bytes, (double)bytes / (WAV_RATE * 2));
}

void muse_wav_record_write_user(const uint8_t *pcm, size_t bytes)
{
    if (!s_mounted || !s_turn_active || !pcm || !bytes || s_user_failed) {
        return;
    }
    if (!s_user) {
        snprintf(s_user_path, sizeof(s_user_path), WAV_DIR "/muse_%05u_user.wav", s_turn);
        s_user = open_wav(s_user_path, &s_user_bytes);
        if (!s_user) {
            s_user_failed = true;
            return;
        }
    }
    if (fwrite(pcm, 1, bytes, s_user) != bytes) {
        ESP_LOGW(TAG, "TF write failed on %s: stopping this recording", s_user_path);
        fclose(s_user);
        s_user = NULL;
        s_user_failed = true;
        return;
    }
    s_user_bytes += (uint32_t)bytes;
}

void muse_wav_record_finish_user(bool keep)
{
    if (s_user) {
        if (keep) {
            finish_wav(&s_user, s_user_path, s_user_bytes);
        } else {
            fclose(s_user);
            s_user = NULL;
            remove(s_user_path);
            ESP_LOGI(TAG, "discarded short press: %s", s_user_path);
        }
    }
    s_user_bytes = 0;
    s_user_failed = false;
}

void muse_wav_record_begin_reply(void)
{
    if (!s_mounted || !s_turn_active || s_reply) {
        return;
    }
    snprintf(s_reply_path, sizeof(s_reply_path), WAV_DIR "/muse_%05u_reply.wav", s_turn);
    s_reply = open_wav(s_reply_path, &s_reply_bytes);
}

void muse_wav_record_write_reply(const int16_t *pcm, size_t frames)
{
    if (!s_reply || !pcm || !frames) {
        return;
    }
    size_t bytes = frames * sizeof(int16_t);
    if (fwrite(pcm, 1, bytes, s_reply) != bytes) {
        ESP_LOGW(TAG, "TF write failed on %s: stopping reply recording", s_reply_path);
        fclose(s_reply);
        s_reply = NULL;
        return;
    }
    s_reply_bytes += (uint32_t)bytes;
}

void muse_wav_record_append_reply_text(const char *text, size_t len)
{
    if (!s_mounted || !s_turn_active || !text || !len) {
        return;
    }
    if (!s_txt) {
        snprintf(s_txt_path, sizeof(s_txt_path), WAV_DIR "/muse_%05u_reply.txt", s_turn);
        s_txt = fopen(s_txt_path, "wb");
        if (!s_txt) {
            ESP_LOGW(TAG, "cannot open %s", s_txt_path);
            return;
        }
    }
    if (fwrite(text, 1, len, s_txt) != len || fwrite("\n\n", 1, 2, s_txt) != 2) {
        ESP_LOGW(TAG, "TF write failed on %s", s_txt_path);
        fclose(s_txt);
        s_txt = NULL;
        return;
    }
}

void muse_wav_record_finish_reply(void)
{
    if (s_reply) {
        finish_wav(&s_reply, s_reply_path, s_reply_bytes);
        s_reply_bytes = 0;
    }
    if (s_txt) {
        fclose(s_txt);
        s_txt = NULL;
        ESP_LOGI(TAG, "saved %s", s_txt_path);
    }
    /* The turn's files are done; a later begin_turn() starts a new set. */
    s_turn_active = false;
    s_turn = 0;
}

void muse_wav_record_cancel_turn(void)
{
    if (s_user) {
        fclose(s_user);
        s_user = NULL;
        remove(s_user_path);
    }
    if (s_reply) {
        fclose(s_reply);
        s_reply = NULL;
        remove(s_reply_path);
    }
    if (s_txt) {
        fclose(s_txt);
        s_txt = NULL;
        remove(s_txt_path);
    }
    s_user_bytes = 0;
    s_reply_bytes = 0;
    s_user_failed = false;
    s_turn_active = false;
    s_turn = 0;
}

#else /* !CONFIG_MUSE_WAV_RECORD */

/* Recording compiled out: every call is a no-op, conversation untouched. */
void muse_wav_record_init(void)
{
}

bool muse_wav_record_available(void)
{
    return false;
}

void muse_wav_record_begin_turn(void)
{
}

void muse_wav_record_write_user(const uint8_t *pcm, size_t bytes)
{
    (void)pcm;
    (void)bytes;
}

void muse_wav_record_finish_user(bool keep)
{
    (void)keep;
}

void muse_wav_record_begin_reply(void)
{
}

void muse_wav_record_write_reply(const int16_t *pcm, size_t frames)
{
    (void)pcm;
    (void)frames;
}

void muse_wav_record_append_reply_text(const char *text, size_t len)
{
    (void)text;
    (void)len;
}

void muse_wav_record_finish_reply(void)
{
}

void muse_wav_record_cancel_turn(void)
{
}

bool muse_wav_record_format_card(void)
{
    return false;
}

#endif /* CONFIG_MUSE_WAV_RECORD */
