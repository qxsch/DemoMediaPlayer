/*
 * rectview.h – Visual rectangle verification overlay
 *
 * Shows a translucent overlay highlighting a screen rectangle so the
 * user can visually verify the recording capture area.  The overlay
 * is movable (drag interior) and resizable (drag edges / corners).
 * Dismiss with ESC or right-click.
 *
 * When the user moves or resizes the overlay the new rectangle is
 * posted to `notify_hwnd` as a `WM_RECTVIEW_CHANGED` message with
 * lParam pointing to a static RECT (valid only during message
 * processing).
 */
#ifndef DMP_RECTVIEW_H
#define DMP_RECTVIEW_H

#include <windows.h>

/* Show the overlay.  `notify_hwnd` receives WM_RECTVIEW_CHANGED
   whenever the user drags or resizes.  Pass NULL to suppress. */
void rectview_show(HINSTANCE hi, const RECT *rect, HWND notify_hwnd);

/* Dismiss the overlay immediately (no-op if not visible). */
void rectview_dismiss(void);

/* TRUE if the overlay window is currently visible. */
BOOL rectview_visible(void);

/* Retrieve the current overlay rectangle (screen coords).  Returns
   FALSE if no overlay is visible. */
BOOL rectview_get_rect(RECT *out);

#endif /* DMP_RECTVIEW_H */
