/*
 * setup.h – Interactive setup dialog
 */
#ifndef DMP_SETUP_H
#define DMP_SETUP_H

#include <windows.h>
#include "constants.h"
#include "monitors.h"

/* Selections made in the setup dialog. */
typedef struct {
    wchar_t file[MAX_PATH_BUF];
    int     screen;
    BOOL    muted;
    BOOL    keep_taskbar;       /* show OS taskbar            */
    int     keep_taskbar_h;     /* DPI-base height            */
    BOOL    crop_taskbar;       /* crop taskbar from video    */
    int     crop_taskbar_px;    /* source-pixel height        */
    BOOL    confirmed;          /* TRUE if user clicked Play  */
    BOOL    record;             /* TRUE if user chose Record  */
} SetupResult;

/* Show the setup dialog and run its message loop.
   Pre-populate from initial_screen / initial_muted / initial_file.
   Blocks until the user confirms or cancels.
   Returns TRUE if the user confirmed playback (result filled in). */
BOOL setup_run(HINSTANCE hi,
               const MonInfo *monitors, int nmons,
               const wchar_t *initial_file,
               int initial_screen,
               BOOL initial_muted,
               BOOL initial_keep_taskbar, int keep_taskbar_h,
               BOOL initial_crop_taskbar, int crop_taskbar_px,
               SetupResult *result);

#endif /* DMP_SETUP_H */
