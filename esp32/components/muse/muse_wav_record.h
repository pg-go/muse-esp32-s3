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
 * WAV recording to the TF card (T1 branch, pg-go fork).
 *
 * What it records, per push-to-talk turn:
 *   muse_NNNNN_user.wav   - the user's own speech, exactly the 16 kHz,
 *                           16-bit, mono PCM the voice-note pipeline sends
 *                           (dual mics mixed down, high-passed). One file
 *                           per BOOT press. Presses under 0.3 s are deleted,
 *                           mirroring Muse's "DIDN'T CATCH THAT" rule.
 *   muse_NNNNN_reply.wav  - Muse's reply audio, but only if a real TTS
 *                           implementation feeds decoded PCM through
 *                           decode() with silent=false. Today start_tts()
 *                           shows replies as captions (silent=true), so
 *                           this file is normally not created; the hook
 *                           is here so a future TTS wiring gets it free.
 *   muse_NNNNN_reply.txt  - Muse's reply text, one message per block.
 *                           This is written even when there is no reply
 *                           audio, so a turn is always archived as
 *                           user WAV + reply text.
 *
 * Failure model, on purpose: recording is best-effort and never breaks
 * conversation. If the TF card is absent, unformatted, or the SD pins are
 * not configured, muse_wav_record_init() logs that once and every other
 * function below becomes a no-op. A write error mid-turn logs once, closes
 * that file, and the turn still goes to Muse.
 *
 * TF pins are NOT guessed. The Lichuang SZP vendor pages list the slot as
 * "1-SD mode" but give no SDMMC GPIO numbers, and no SZP TF pin table was
 * found in the repo or in a public search. The three pins are therefore
 * Kconfig (menuconfig: Muse -> WAV recording to TF card):
 *   CONFIG_MUSE_WAV_SD_CLK / CMD / D0
 * All three default to -1 (unconfigured). Until they are set to the real
 * PCB wiring, init() will report "not configured" and record nothing.
 * Changing only those three values, then rebuilding, is the whole
 * bring-up step once the schematic / pin table is confirmed. Do not
 * "fix" this by inventing pins: a wrong CLK/CMD/D0 can clash with the
 * LCD SPI (40/41), the I2S clocks (13/14/38) or the I2C bus (1/2).
 *
 * Threading: init() runs on the app task at boot. Every other function
 * runs on the muse_chat session task only, so no lock is needed.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mount the TF card at /sdcard and prepare /sdcard/muse. Best-effort:
 * on any failure it logs and leaves recording disabled. Safe to call once. */
void muse_wav_record_init(void);

/* True only after a successful mount. */
bool muse_wav_record_available(void);

/* Start a new turn's file set (increments the turn number). Does not
 * create any file yet: files are opened lazily on first write, so a
 * turn that is interrupted before any audio arrives leaves no files. */
void muse_wav_record_begin_turn(void);

/* Append raw 16 kHz PCM16 mono bytes to the current turn's user WAV.
 * Call with exactly the mic bytes that go into the voice note, never
 * with the 44-byte WAV header (the recorder writes its own header). */
void muse_wav_record_write_user(const uint8_t *pcm, size_t bytes);

/* Finish the user WAV: keep=true rewrites its header with the true data
 * size and closes it; keep=false closes and deletes it (short press). */
void muse_wav_record_finish_user(bool keep);

/* Open the reply WAV for the current turn (lazy: only when real,
 * non-silent TTS audio arrives). Call from start_tts() when a message
 * becomes TTS_ACTIVE with silent=false. */
void muse_wav_record_begin_reply(void);

/* Append decoded 16 kHz PCM16 mono frames to the reply WAV. No-op if
 * no reply WAV is open. */
void muse_wav_record_write_reply(const int16_t *pcm, size_t frames);

/* Append one finished reply message's text to the reply .txt (lazy). */
void muse_wav_record_append_reply_text(const char *text, size_t len);

/* Close the reply WAV (true header) and the reply .txt, if open. */
void muse_wav_record_finish_reply(void);

/* Abandon the current turn: close and delete any user/reply files
 * opened for it, without writing a transcript. Idempotent. */
void muse_wav_record_cancel_turn(void);

#ifdef __cplusplus
}
#endif
