/*
 * identify.h – "Identify Screens" overlay
 */
#ifndef DMP_IDENTIFY_H
#define DMP_IDENTIFY_H

#include <windows.h>
#include "monitors.h"

/* Show a large screen-number overlay on every monitor.
   Clicking an overlay selects that screen in the combo box
   referenced by setup_combo.  Auto-dismisses after 5 seconds.
   monitors/nmons provide the monitor list. */
void identify_show(HINSTANCE hi,
                    const MonInfo *monitors, int nmons,
                    HWND setup_combo);

/* Dismiss all overlays immediately. */
void identify_dismiss(void);

#endif /* DMP_IDENTIFY_H */
