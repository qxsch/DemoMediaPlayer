/*
 * panzoom.c – Video zoom and pan state management
 */
#include "panzoom.h"
#include "constants.h"

#include <math.h>

/* ── Helpers ─────────────────────────────────────────────────── */

/*
 * Compute the maximum pan value for one axis.
 *
 *   zoom = current linear zoom factor (1.0 – 4.0)
 *   r    = ratio of (video display size) / (window size) at zoom=1
 *          • 1.0 for the axis the video fills exactly
 *          • < 1.0 for the letterboxed / pillarboxed axis
 *
 * Returns 0 if the video still doesn't fill the window in that axis
 * (even after zooming), otherwise the symmetric pan limit.
 */
static double max_pan_for_axis(double zoom, double r)
{
    double eff = zoom * r;          /* effective zoom for this axis */
    if (eff <= 1.0) return 0.0;    /* still has black bars → no pan */
    return (eff - 1.0) / (2.0 * eff);
}

/* Clamp pan values so the video edge never moves past the window
   edge, using video dimensions from the playback module. */
static void clamp_pan(PanZoom *pz, Playback *pb)
{
    if (pz->zoom <= ZOOM_MIN) {
        pz->pan_x = 0.0;
        pz->pan_y = 0.0;
        return;
    }

    double mx, my;
    PlaybackVideoDims dims;

    if (playback_get_video_dims(pb, &dims)) {
        double vid_aspect = (double)dims.vid_w / (double)dims.vid_h;
        double osd_aspect = (double)dims.osd_w / (double)dims.osd_h;

        double r_x, r_y;
        if (vid_aspect >= osd_aspect) {
            /* Video wider than window: fills width, letterboxed */
            r_x = 1.0;
            r_y = osd_aspect / vid_aspect;
        } else {
            /* Video taller than window: fills height, pillarboxed */
            r_y = 1.0;
            r_x = vid_aspect / osd_aspect;
        }
        mx = max_pan_for_axis(pz->zoom, r_x);
        my = max_pan_for_axis(pz->zoom, r_y);
    } else {
        /* Fallback when dimensions are unavailable */
        mx = (pz->zoom - 1.0) / (2.0 * pz->zoom);
        my = mx;
    }

    if (pz->pan_x >  mx) pz->pan_x =  mx;
    if (pz->pan_x < -mx) pz->pan_x = -mx;
    if (pz->pan_y >  my) pz->pan_y =  my;
    if (pz->pan_y < -my) pz->pan_y = -my;
}

/* ── Public API ──────────────────────────────────────────────── */

void panzoom_init(PanZoom *pz)
{
    pz->zoom  = 1.0;
    pz->pan_x = 0.0;
    pz->pan_y = 0.0;
}

void panzoom_apply(PanZoom *pz, Playback *pb)
{
    clamp_pan(pz, pb);
    double log2_zoom = log(pz->zoom) / log(2.0);
    playback_set_zoom_pan(pb, log2_zoom, pz->pan_x, pz->pan_y);
}

void panzoom_change_zoom(PanZoom *pz, Playback *pb, double delta)
{
    pz->zoom += delta;
    if (pz->zoom < ZOOM_MIN) pz->zoom = ZOOM_MIN;
    if (pz->zoom > ZOOM_MAX) pz->zoom = ZOOM_MAX;
    panzoom_apply(pz, pb);
}

void panzoom_change_pan(PanZoom *pz, Playback *pb,
                         double dx, double dy)
{
    /* Only allow panning when zoomed in */
    if (pz->zoom <= ZOOM_MIN) return;
    /* Scale step inversely with zoom so movement feels the same
       in screen-pixels regardless of zoom level. */
    pz->pan_x += dx / pz->zoom;
    pz->pan_y += dy / pz->zoom;
    panzoom_apply(pz, pb);
}

void panzoom_reset(PanZoom *pz, Playback *pb)
{
    panzoom_init(pz);
    panzoom_apply(pz, pb);
}
