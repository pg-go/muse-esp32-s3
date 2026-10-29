/* Boot log to TF card, for debugging only.
 *
 * Install early (muse_glue_start, before Link runs) so logs from
 * that point on are captured. Before the TF card is mounted,
 * lines are kept in a PSRAM ring buffer; once muse_wav_record
 * mounts the card, muse_log_file_start() opens /sdcard/log.txt,
 * flushes the ring, and then writes every further line to the
 * file as it arrives (fflush + fsync per line, so pulling the
 * card after a failure still shows the last lines, even without
 * a reboot; fflush alone leaves FatFs showing 0 bytes on a live
 * pull, which is why the first log build looked empty). Best-effort like WAV recording: no card, no mount,
 * or a write failure only disables the file, never boot or
 * conversation. The serial console and the PSRAM diagnostic
 * ring are untouched: this hook chains to the previous vprintf.
 *
 * File layout: append per boot with a boot header, so one
 * failure survives a later reboot. If the file is already over
 * 512 KiB at open, it is truncated and restarted.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void muse_log_file_install(void);
void muse_log_file_start(void);

#ifdef __cplusplus
}
#endif
