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

#endif /* DMP_PLAYER_H */
