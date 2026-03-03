/*
 * main.c – DemoMediaPlayer entry point
 *
 * Orchestrates argument parsing, monitor enumeration, the setup
 * dialog (interactive mode), and fullscreen playback.
 */
#include <windows.h>
#include <stdlib.h>

#include "constants.h"
#include "args.h"
#include "help.h"
#include "monitors.h"
#include "playback.h"
#include "player.h"
#include "recorder.h"
#include "recctl.h"
#include "setup.h"
#include "util.h"

/* ================================================================== */
/*  Entry point                                                        */
/* ================================================================== */

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cl, int cs)
{
    (void)hp; (void)cl; (void)cs;

    /* ── Check --help first, before any other initialisation ───── */
    if (args_has_help()) {
        help_show();
        return 0;
    }

    /* ── Enumerate monitors ────────────────────────────────────── */
    MonInfo monitors[DMP_MAX_MONITORS];
    int nmons = monitors_enumerate(monitors, DMP_MAX_MONITORS);
    if (nmons == 0) {
        MessageBoxW(NULL, L"No display monitors detected.",
                    APP_TITLE, MB_ICONERROR);
        return 1;
    }

    /* ── Parse command line ────────────────────────────────────── */
    AppArgs args;
    args_parse(&args);

    /* ── Recording mode ───────────────────────────────────────── */
    if (args.record) {
        int scr = args.has_screen ? args.screen : 0;
        if (scr < 0 || scr >= nmons) scr = 0;

        /* Convert file path to UTF-8 for mpv (NULL if not given). */
        char *u8_out = NULL;
        if (args.has_file) {
            u8_out = to_utf8(args.file);
            if (!u8_out) {
                MessageBoxW(NULL, L"Invalid file path encoding.",
                            APP_TITLE, MB_ICONERROR);
                return 1;
            }
        }

        /* Detect audio loopback device. */
        char   *u8_audio = NULL;
        wchar_t adev_w[256] = {0};

        if (!args.no_audio) {
            wchar_t adev[256] = {0};
            if (args.has_audio_device)
                wcscpy(adev, args.audio_device);
            else
                recorder_find_audio_device(adev, 256);

            if (adev[0]) {
                u8_audio = to_utf8(adev);
                wcscpy(adev_w, adev);
            }
        }

        int fps = args.rec_fps > 0 ? args.rec_fps : REC_DEFAULT_FPS;

        RecCtlParams rp;
        ZeroMemory(&rp, sizeof(rp));
        rp.hi            = hi;
        rp.capture_rect  = monitors[scr].rect;
        rp.screen_index  = scr;
        rp.nmons         = nmons;
        rp.monitors      = monitors;
        rp.output_u8     = u8_out;
        rp.output_w      = args.has_file ? args.file : NULL;
        rp.fps           = fps;
        rp.audio_u8      = u8_audio;
        rp.audio_w       = adev_w[0] ? adev_w : NULL;
        rp.no_audio      = args.no_audio;
        rp.no_mouse      = args.no_mouse;

        int ret = recctl_run(&rp);

        free(u8_out);
        free(u8_audio);
        return ret;
    }

    /* ── Playback mode (file or interactive) ──────────────────── */
    wchar_t   *fpath;
    int        scr;
    BOOL       muted = args.mute;
    BOOL       keep_taskbar   = args.keep_taskbar;
    int        keep_taskbar_h = args.keep_taskbar_h;
    BOOL       crop_taskbar   = args.crop_taskbar;
    int        crop_taskbar_px = args.crop_taskbar_px;
    SetupResult sr;    /* declared here so it outlives the else block */

    if (args.has_file) {
        /* CLI mode: go straight to fullscreen playback. */
        fpath = args.file;
        scr   = args.has_screen ? args.screen : 0;
    } else {
        /* Interactive mode: show setup dialog. */
        if (!setup_run(hi, monitors, nmons,
                       NULL,                       /* no initial file */
                       args.has_screen ? args.screen : 0,
                       muted,
                       args.keep_taskbar,
                       args.keep_taskbar_h  ? args.keep_taskbar_h
                                            : DEFAULT_TASKBAR_HEIGHT,
                       args.crop_taskbar,
                       args.crop_taskbar_px ? args.crop_taskbar_px
                                            : DEFAULT_TASKBAR_HEIGHT,
                       &sr))
        {
            return 0;   /* user cancelled */
        }

        /* User chose Record from the system menu */
        if (sr.record) {
            int rec_scr = sr.screen;
            if (rec_scr < 0 || rec_scr >= nmons) rec_scr = 0;

            char   *u8_audio = NULL;
            wchar_t adev_w[256] = {0};
            wchar_t adev[256] = {0};
            recorder_find_audio_device(adev, 256);
            if (adev[0]) {
                u8_audio = to_utf8(adev);
                wcscpy(adev_w, adev);
            }

            RecCtlParams rp;
            ZeroMemory(&rp, sizeof(rp));
            rp.hi            = hi;
            rp.capture_rect  = monitors[rec_scr].rect;
            rp.screen_index  = rec_scr;
            rp.nmons         = nmons;
            rp.monitors      = monitors;
            rp.output_u8     = NULL;
            rp.output_w      = NULL;
            rp.fps           = REC_DEFAULT_FPS;
            rp.audio_u8      = u8_audio;
            rp.audio_w       = adev_w[0] ? adev_w : NULL;
            rp.no_audio      = FALSE;
            rp.no_mouse      = FALSE;

            int ret = recctl_run(&rp);
            free(u8_audio);
            return ret;
        }

        fpath          = sr.file;
        scr            = sr.screen;
        muted          = sr.muted;
        keep_taskbar   = sr.keep_taskbar;
        keep_taskbar_h = sr.keep_taskbar_h;
        crop_taskbar   = sr.crop_taskbar;
        crop_taskbar_px = sr.crop_taskbar_px;
    }

    if (scr < 0 || scr >= nmons) scr = 0;

    /* ── Keep-taskbar-visible: shrink playback area ─────────────── */
    RECT play_rect = monitors[scr].rect;
    if (keep_taskbar) {
        UINT dpi = dpi_for_monitor(monitors[scr].hmon);
        int  bar = dpi_scale(keep_taskbar_h, dpi);
        play_rect.bottom -= bar;
    }

    /* ── Convert file path to UTF-8 for mpv ────────────────────── */
    char *u8 = to_utf8(fpath);
    if (!u8) {
        MessageBoxW(NULL, L"Invalid file path encoding.",
                    APP_TITLE, MB_ICONERROR);
        return 1;
    }

    /* ── Create fullscreen window & start mpv ──────────────────── */
    HWND hw = player_create(hi, &play_rect);
    if (!hw) {
        free(u8);
        MessageBoxW(NULL, L"Could not create player window.",
                    APP_TITLE, MB_ICONERROR);
        return 1;
    }

    Playback *pb = playback_create(hw, u8,
                                    args.has_position ? args.position
                                                      : 0.0);
    free(u8);
    if (!pb) return 1;

    /* Wire up the player window to its Playback instance. */
    player_set_playback(hw, pb);

    /* Apply video crop if requested (deferred until dimensions available). */
    if (crop_taskbar)
        player_set_crop(hw, crop_taskbar_px);

    /* Apply initial mute state if requested. */
    if (muted)
        playback_set_volume(pb, 0);

    /* ── Main message loop ─────────────────────────────────────── */
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    /* ── Cleanup ───────────────────────────────────────────────── */
    playback_destroy(pb);
    return 0;
}
