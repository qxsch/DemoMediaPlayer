/*
 * playback.h – mpv lifecycle and playback control
 *
 * All mpv interaction is routed through this module so that no
 * other file needs to include <mpv/client.h>.
 */
#ifndef DMP_PLAYBACK_H
#define DMP_PLAYBACK_H

#include <windows.h>
#include <stdint.h>

/* Opaque to callers – definition lives in playback.c */
typedef struct Playback Playback;

/* ── Lifecycle ───────────────────────────────────────────────── */

/* Create and initialise mpv, embed in host window, start file.
   Returns NULL on failure (shows an error MessageBox). */
Playback *playback_create(HWND host, const char *path,
                           double start_pos);

/* Tear down mpv cleanly. Safe to call with NULL. */
void playback_destroy(Playback *pb);

/* ── Transport controls ──────────────────────────────────────── */

void playback_seek(Playback *pb, const char *offset, const char *mode);
void playback_toggle_pause(Playback *pb);
void playback_restart(Playback *pb);
void playback_cycle_mute(Playback *pb);

/* ── Speed ───────────────────────────────────────────────────── */

void playback_set_speed(Playback *pb, double speed);
void playback_change_speed(Playback *pb, double delta);

/* ── Mute ────────────────────────────────────────────────────── */

void playback_set_mute(Playback *pb, int muted);

/* ── Zoom / pan properties (called by panzoom module) ────────── */

typedef struct {
    int64_t vid_w, vid_h;   /* source video dimensions      */
    int64_t osd_w, osd_h;   /* on-screen display dimensions */
} PlaybackVideoDims;

/* Query video and OSD dimensions.  Returns FALSE if unavailable. */
BOOL playback_get_video_dims(Playback *pb, PlaybackVideoDims *dims);

/* Set the video-zoom (log₂), video-pan-x, and video-pan-y. */
void playback_set_zoom_pan(Playback *pb,
                            double log2_zoom,
                            double pan_x,
                            double pan_y);

/* ── Event pump ──────────────────────────────────────────────── */

/* Drain the mpv event queue.  Should be called in response to
   WM_MPV_WAKEUP.  Returns TRUE if playback encountered an error
   (the caller should quit).  Sets *eof to TRUE on normal EOF. */
BOOL playback_pump_events(Playback *pb, HWND hw, BOOL *eof);

#endif /* DMP_PLAYBACK_H */
