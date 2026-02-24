/*
 * playback.c – mpv lifecycle and playback control
 */
#include "playback.h"
#include "constants.h"

#include <mpv/client.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Internal struct (opaque via header) ─────────────────────── */

struct Playback {
    mpv_handle *mpv;
};

/* ── Helpers ─────────────────────────────────────────────────── */

static double clamp_speed(double speed)
{
    if (speed < SPEED_MIN) return SPEED_MIN;
    if (speed > SPEED_MAX) return SPEED_MAX;
    return speed;
}

static double normalize_speed_step(double speed)
{
    int tenths = (int)(speed * 10.0 + 0.5);
    return (double)tenths / 10.0;
}

/* ── mpv wakeup → Win32 message ──────────────────────────────── */

static void mpv_wakeup_cb(void *ctx)
{
    PostMessageW((HWND)ctx, WM_MPV_WAKEUP, 0, 0);
}

/* ── Lifecycle ───────────────────────────────────────────────── */

Playback *playback_create(HWND host, const char *path,
                           double start_pos)
{
    mpv_handle *mpv = mpv_create();
    if (!mpv) {
        MessageBoxW(host, L"mpv_create() failed.",
                    APP_TITLE, MB_ICONERROR);
        return NULL;
    }

    /* Embed mpv's video output inside our window. */
    int64_t wid = (int64_t)(intptr_t)host;
    mpv_set_option(mpv, "wid", MPV_FORMAT_INT64, &wid);

    /* Disable mpv's own input handling – we do it via WM_KEYDOWN. */
    mpv_set_option_string(mpv, "input-default-bindings", "no");
    mpv_set_option_string(mpv, "input-vo-keyboard",      "no");

    /* No on-screen controller (clean fullscreen). */
    mpv_set_option_string(mpv, "osc", "no");

    /* Minimal OSD: show seek position briefly on seek commands. */
    mpv_set_option_string(mpv, "osd-level",    "1");
    mpv_set_option_string(mpv, "osd-duration",  "1500");

    /* Use hardware decoding when available. */
    mpv_set_option_string(mpv, "hwdec", "auto");

    /* Set start position if requested. */
    if (start_pos > 0.0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.3f", start_pos);
        mpv_set_option_string(mpv, "start", buf);
    }

    if (mpv_initialize(mpv) < 0) {
        MessageBoxW(host, L"mpv_initialize() failed.",
                    APP_TITLE, MB_ICONERROR);
        mpv_destroy(mpv);
        return NULL;
    }

    /* Pause on the last frame instead of closing when file ends. */
    mpv_set_property_string(mpv, "keep-open", "always");

    /* Keep mpv alive even when nothing is playing (safety net). */
    mpv_set_property_string(mpv, "idle", "yes");

    /* Integrate mpv's event loop with our Win32 message loop. */
    mpv_set_wakeup_callback(mpv, mpv_wakeup_cb, (void *)host);

    /* Load and start playback. */
    const char *cmd[] = {"loadfile", path, NULL};
    if (mpv_command(mpv, cmd) < 0) {
        mpv_terminate_destroy(mpv);
        return NULL;
    }

    Playback *pb = (Playback *)calloc(1, sizeof(*pb));
    if (!pb) {
        mpv_terminate_destroy(mpv);
        return NULL;
    }
    pb->mpv = mpv;
    return pb;
}

void playback_destroy(Playback *pb)
{
    if (!pb) return;
    if (pb->mpv) {
        mpv_terminate_destroy(pb->mpv);
        pb->mpv = NULL;
    }
    free(pb);
}

/* ── Transport controls ──────────────────────────────────────── */

void playback_seek(Playback *pb, const char *offset, const char *mode)
{
    if (!pb || !pb->mpv) return;
    const char *cmd[] = {"seek", offset, mode, NULL};
    mpv_command_async(pb->mpv, 0, cmd);
}

void playback_toggle_pause(Playback *pb)
{
    if (!pb || !pb->mpv) return;
    const char *cmd[] = {"cycle", "pause", NULL};
    mpv_command_async(pb->mpv, 0, cmd);
}

void playback_restart(Playback *pb)
{
    if (!pb || !pb->mpv) return;
    /* Unpause (in case we're paused at EOF) and seek to start. */
    int pause = 0;
    mpv_set_property(pb->mpv, "pause", MPV_FORMAT_FLAG, &pause);
    const char *cmd[] = {"seek", "0", "absolute", NULL};
    mpv_command_async(pb->mpv, 0, cmd);
}

void playback_cycle_mute(Playback *pb)
{
    if (!pb || !pb->mpv) return;
    const char *cmd[] = {"cycle", "mute", NULL};
    mpv_command_async(pb->mpv, 0, cmd);
}

/* ── Speed ───────────────────────────────────────────────────── */

void playback_set_speed(Playback *pb, double speed)
{
    if (!pb || !pb->mpv) return;
    double clamped = clamp_speed(speed);
    mpv_set_property(pb->mpv, "speed", MPV_FORMAT_DOUBLE, &clamped);
}

void playback_change_speed(Playback *pb, double delta)
{
    if (!pb || !pb->mpv) return;

    double speed = 1.0;
    if (mpv_get_property(pb->mpv, "speed", MPV_FORMAT_DOUBLE, &speed) < 0)
        speed = 1.0;

    speed = normalize_speed_step(speed + delta);
    playback_set_speed(pb, speed);
}

/* ── Mute ────────────────────────────────────────────────────── */

void playback_set_mute(Playback *pb, int muted)
{
    if (!pb || !pb->mpv) return;
    mpv_set_property(pb->mpv, "mute", MPV_FORMAT_FLAG, &muted);
}

/* ── Zoom / pan properties ───────────────────────────────────── */

BOOL playback_get_video_dims(Playback *pb, PlaybackVideoDims *dims)
{
    if (!pb || !pb->mpv || !dims) return FALSE;

    dims->vid_w = dims->vid_h = 0;
    dims->osd_w = dims->osd_h = 0;
    mpv_get_property(pb->mpv, "video-params/w", MPV_FORMAT_INT64, &dims->vid_w);
    mpv_get_property(pb->mpv, "video-params/h", MPV_FORMAT_INT64, &dims->vid_h);
    mpv_get_property(pb->mpv, "osd-width",      MPV_FORMAT_INT64, &dims->osd_w);
    mpv_get_property(pb->mpv, "osd-height",     MPV_FORMAT_INT64, &dims->osd_h);

    return (dims->vid_w > 0 && dims->vid_h > 0 &&
            dims->osd_w > 0 && dims->osd_h > 0);
}

void playback_set_zoom_pan(Playback *pb,
                            double log2_zoom,
                            double pan_x,
                            double pan_y)
{
    if (!pb || !pb->mpv) return;
    mpv_set_property(pb->mpv, "video-zoom",  MPV_FORMAT_DOUBLE, &log2_zoom);
    mpv_set_property(pb->mpv, "video-pan-x", MPV_FORMAT_DOUBLE, &pan_x);
    mpv_set_property(pb->mpv, "video-pan-y", MPV_FORMAT_DOUBLE, &pan_y);
}

void playback_set_video_crop(Playback *pb, int crop_bottom)
{
    if (!pb || !pb->mpv) return;
    if (crop_bottom <= 0) {
        mpv_set_property_string(pb->mpv, "video-crop", "");
        return;
    }
    /* We only know the crop height; query the video dimensions
       so we can express it as WxH+X+Y (full width, reduced height,
       starting at 0,0).  If dimensions are unavailable yet we
       cannot set the crop; the caller should retry after playback
       starts. */
    PlaybackVideoDims dims;
    if (!playback_get_video_dims(pb, &dims) || dims.vid_h <= 0)
        return;
    int cw = (int)dims.vid_w;
    int ch = (int)dims.vid_h - crop_bottom;
    if (ch < 1) ch = 1;
    char buf[64];
    snprintf(buf, sizeof(buf), "%dx%d+0+0", cw, ch);
    mpv_set_property_string(pb->mpv, "video-crop", buf);
}

/* ── Event pump ──────────────────────────────────────────────── */

BOOL playback_pump_events(Playback *pb, HWND hw, BOOL *eof)
{
    if (eof) *eof = FALSE;
    if (!pb || !pb->mpv) return FALSE;

    while (1) {
        mpv_event *e = mpv_wait_event(pb->mpv, 0);
        if (e->event_id == MPV_EVENT_NONE)
            break;

        if (e->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file *ef = (mpv_event_end_file *)e->data;
            if (ef && ef->reason == MPV_END_FILE_REASON_ERROR) {
                MessageBoxW(hw, L"Failed to play the selected file.",
                            APP_TITLE, MB_ICONERROR);
                return TRUE;  /* signal error → caller should quit */
            }
            /* Normal EOF: pause on last frame. */
            int pause = 1;
            mpv_set_property(pb->mpv, "pause", MPV_FORMAT_FLAG, &pause);
            if (eof) *eof = TRUE;
        }
    }
    return FALSE;
}
