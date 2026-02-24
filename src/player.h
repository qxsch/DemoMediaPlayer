/*
 * player.h – Fullscreen player window
 *
 * Manages the borderless topmost window that hosts mpv video
 * output and dispatches keyboard controls.
 */
#ifndef DMP_PLAYER_H
#define DMP_PLAYER_H

#include <windows.h>
#include "playback.h"

/* Create a borderless, topmost, fullscreen window covering the
   given screen rectangle.  Returns NULL on failure. */
HWND player_create(HINSTANCE hi, const RECT *screen_rect);

/* Attach a Playback instance so the player window can handle
   mpv events and keyboard controls.  Must be called after
   playback_create(). */
void player_set_playback(HWND player, Playback *pb);

/* Set the number of source-video pixels to crop from the bottom.
   Must be called before the first mpv event so the crop is applied
   as soon as video dimensions become available. */
void player_set_crop(HWND player, int crop_bottom_px);

#endif /* DMP_PLAYER_H */
