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

    wchar_t   *fpath;
    int        scr;
    BOOL       muted = args.mute;
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
                       &sr))
        {
            return 0;   /* user cancelled */
        }
        fpath = sr.file;
        scr   = sr.screen;
        muted = sr.muted;
    }

    if (scr < 0 || scr >= nmons) scr = 0;

    /* ── Convert file path to UTF-8 for mpv ────────────────────── */
    char *u8 = to_utf8(fpath);
    if (!u8) {
        MessageBoxW(NULL, L"Invalid file path encoding.",
                    APP_TITLE, MB_ICONERROR);
        return 1;
    }

    /* ── Create fullscreen window & start mpv ──────────────────── */
    HWND hw = player_create(hi, &monitors[scr].rect);
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

    /* Apply initial mute state if requested. */
    if (muted)
        playback_set_mute(pb, 1);

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
