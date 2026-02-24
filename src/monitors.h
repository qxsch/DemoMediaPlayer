/*
 * monitors.h – Monitor enumeration and DPI helpers
 */
#ifndef DMP_MONITORS_H
#define DMP_MONITORS_H

#include <windows.h>
#include "constants.h"

/* ── Monitor information ─────────────────────────────────────── */

typedef struct {
    HMONITOR hmon;
    RECT     rect;
    wchar_t  label[128];
} MonInfo;

/* Enumerate all display monitors into the provided array.
   Returns the number of monitors found (capped at DMP_MAX_MONITORS). */
int monitors_enumerate(MonInfo *mons, int max_count);

/* ── DPI helpers ─────────────────────────────────────────────── */

/* Get effective DPI for a window (falls back to system DPI). */
UINT dpi_for_window(HWND hwnd);

/* Get effective DPI for a monitor handle. */
UINT dpi_for_monitor(HMONITOR hmon);

/* Scale a base-96-DPI value to the given DPI. */
int dpi_scale(int value, UINT dpi);

/* Create a font scaled to the given DPI. */
HFONT dpi_create_font(const wchar_t *face, int base_pt,
                       int weight, UINT dpi);

#endif /* DMP_MONITORS_H */
