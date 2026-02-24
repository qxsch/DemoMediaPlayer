/*
 * recorder.h – Screen recording via Win32 GDI capture + mpv stream callback
 *
 * Captures the desktop using BitBlt and feeds raw BGRA frames through
 * mpv's stream callback API (mpv_stream_cb_add_ro).  mpv's rawvideo
 * demuxer reads the frames and encodes to H.265 MP4 using the built-in
 * FFmpeg encoding stack.  No external binaries required.
 */
#ifndef DMP_RECORDER_H
#define DMP_RECORDER_H

#include <windows.h>

/* Opaque recorder handle. */
typedef struct Recorder Recorder;

/* ── Lifecycle ───────────────────────────────────────────────── */

/* Create a screen recorder capturing the given rectangle.
   output_path and audio_device must be UTF-8 (or NULL for no audio).
   fps <= 0 means use the compile-time default (30).
   capture_mouse: if TRUE, draw the mouse cursor in captured frames.
   Returns NULL on failure. */
Recorder *recorder_create(const RECT *capture_rect,
                          const char *output_path,
                          int fps,
                          const char *audio_device,
                          BOOL capture_mouse);

/* Poll for recorder events.
   Returns: 0 = recording in progress,
            1 = finished normally,
           -1 = error encountered. */
int recorder_poll(Recorder *rec);

/* Signal the recorder to stop and finalize the output MP4. */
void recorder_stop(Recorder *rec);

/* Pause recording (frames are dropped while paused). */
void recorder_pause(Recorder *rec);

/* Resume recording after a pause. */
void recorder_resume(Recorder *rec);

/* Enable or disable mouse cursor capture at runtime. */
void recorder_set_mouse_capture(Recorder *rec, BOOL enable);

/* Returns TRUE if mouse cursor capture is currently enabled. */
BOOL recorder_get_mouse_capture(Recorder *rec);

/* Returns TRUE if the recorder is currently paused. */
BOOL recorder_is_paused(Recorder *rec);

/* Returns TRUE once mpv’s encoding pipeline is active —
   i.e. the first video frame is being read by the encoder.
   Use this to distinguish “initialising” from “recording”. */
BOOL recorder_active(Recorder *rec);

/* Return the last error/diagnostic text (UTF-8, may be empty). */
const char *recorder_last_error(Recorder *rec);

/* Return the error message from the last failed recorder_create() call.
   Useful when recorder_create() returns NULL. */
const char *recorder_create_error(void);

/* Destroy the recorder and free all resources.
   Calls recorder_stop() internally if still recording. */
void recorder_destroy(Recorder *rec);

/* ── Audio device detection ──────────────────────────────────── */

/* Search for a system audio loopback device (Stereo Mix, etc.).
   Returns TRUE if found; fills device_name (wide string).
   max_len is the buffer size in wchar_t units. */
BOOL recorder_find_audio_device(wchar_t *device_name, int max_len);

#endif /* DMP_RECORDER_H */
