/*
 * panzoom.h – Video zoom and pan state management
 */
#ifndef DMP_PANZOOM_H
#define DMP_PANZOOM_H

#include "playback.h"

typedef struct {
    double zoom;     /* 1.0 = 100%  …  4.0 = 400% */
    double pan_x;
    double pan_y;
} PanZoom;

/* Initialise to defaults (no zoom, no pan). */
void panzoom_init(PanZoom *pz);

/* Adjust zoom by delta (clamped to ZOOM_MIN..ZOOM_MAX). */
void panzoom_change_zoom(PanZoom *pz, Playback *pb, double delta);

/* Adjust pan by (dx, dy).  Ignored if zoom <= 1.0.
   Step is scaled inversely with zoom for consistent feel. */
void panzoom_change_pan(PanZoom *pz, Playback *pb,
                         double dx, double dy);

/* Reset zoom and pan to defaults. */
void panzoom_reset(PanZoom *pz, Playback *pb);

/* Push current zoom/pan values to mpv (via Playback). */
void panzoom_apply(PanZoom *pz, Playback *pb);

#endif /* DMP_PANZOOM_H */
