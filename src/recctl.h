/*
 * recctl.h – Recording control window (Start/Stop, Pause/Resume)
 *
 * A small dark-themed, DPI-aware floating window that provides
 * recording controls and a blinking recording indicator.
 * The window runs its own message loop and blocks until the
 * user stops the recording.
 */
#ifndef DMP_RECCTL_H
#define DMP_RECCTL_H

#include <windows.h>
#include "monitors.h"
#include "recorder.h"

/* Parameters for the recording control window. */
typedef struct {
    HINSTANCE   hi;
    RECT        capture_rect;   /* screen region to record      */
    int         screen_index;   /* 0-based screen number        */
    int         nmons;          /* total monitor count           */
    const MonInfo *monitors;
    const char *output_u8;      /* UTF-8 output path (or NULL)  */
    const wchar_t *output_w;    /* wide output path (or NULL)   */
    int         fps;
    const char *audio_u8;       /* UTF-8 audio device or NULL   */
    const wchar_t *audio_w;     /* wide audio label or NULL     */
    BOOL        no_audio;
    BOOL        no_mouse;       /* --disable-mouse-capture      */
} RecCtlParams;

/* Show the recording control window and run its message loop.
   Blocks until the user clicks Stop (or an error occurs).
   Returns 0 on success, non-zero on error. */
int recctl_run(const RecCtlParams *params);

#endif /* DMP_RECCTL_H */
