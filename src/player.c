/*
 * player.c – Fullscreen player window
 */
#include "player.h"
#include "constants.h"
#include "panzoom.h"
#include "resource.h"

#include <stdlib.h>

/* ── Per-window context (stored in GWLP_USERDATA) ────────────── */

typedef struct {
    Playback *pb;
    PanZoom   pz;
    int       crop_bottom_px;  /* source pixels to crop (0=none)  */
    BOOL      crop_applied;    /* TRUE once crop was sent to mpv  */
} PlayerCtx;

/* ── Window procedure ────────────────────────────────────────── */

static LRESULT CALLBACK player_proc(HWND hw, UINT msg,
                                     WPARAM wp, LPARAM lp)
{
    PlayerCtx *ctx =
        (PlayerCtx *)(LONG_PTR)GetWindowLongPtrW(hw, GWLP_USERDATA);

    switch (msg) {

    /* ── mpv event pump ────────────────────────────────────────── */
    case WM_MPV_WAKEUP:
        if (ctx && ctx->pb) {
            /* Apply deferred crop once video dimensions are known. */
            if (!ctx->crop_applied && ctx->crop_bottom_px > 0) {
                playback_set_video_crop(ctx->pb, ctx->crop_bottom_px);
                PlaybackVideoDims dims;
                if (playback_get_video_dims(ctx->pb, &dims) &&
                    dims.vid_h > 0)
                    ctx->crop_applied = TRUE;
            }
            BOOL eof = FALSE;
            if (playback_pump_events(ctx->pb, hw, &eof)) {
                /* Error – quit */
                PostQuitMessage(0);
                return 0;
            }
            /* On normal EOF the last frame stays visible;
               the user can press S to restart or ESC to quit. */
        }
        return 0;

    /* ── Keyboard controls ─────────────────────────────────────── */
    case WM_KEYDOWN:
        if (!ctx || !ctx->pb) break;

        switch (wp) {

        /* ── Quit ────────────────────────────────────────────── */
        case VK_ESCAPE:
            PostQuitMessage(0);
            return 0;

        /* ── Transport ───────────────────────────────────────── */
        case 'S':
            playback_restart(ctx->pb);
            return 0;
        case 'P':
        case VK_SPACE:
            playback_toggle_pause(ctx->pb);
            return 0;
        case 'R':
            playback_seek(ctx->pb, "-30", "relative+exact");
            return 0;
        case 'F':
            playback_seek(ctx->pb, "30", "relative+exact");
            return 0;
        case VK_LEFT:
            playback_seek(ctx->pb, "-5", "relative+exact");
            return 0;
        case VK_RIGHT:
            playback_seek(ctx->pb, "5", "relative+exact");
            return 0;

        /* ── Speed ───────────────────────────────────────────── */
        case VK_UP:
            playback_change_speed(ctx->pb, SPEED_STEP);
            return 0;
        case VK_DOWN:
            playback_change_speed(ctx->pb, -SPEED_STEP);
            return 0;
        case VK_RETURN:
            playback_set_speed(ctx->pb, 1.0);
            return 0;

        /* ── Mute toggle ─────────────────────────────────────── */
        case 'M':
            playback_cycle_mute(ctx->pb);
            return 0;

        /* ── Pan ─────────────────────────────────────────────── */
        case '4': case VK_NUMPAD4:
            panzoom_change_pan(&ctx->pz, ctx->pb, PAN_STEP, 0);
            return 0;
        case '6': case VK_NUMPAD6:
            panzoom_change_pan(&ctx->pz, ctx->pb, -PAN_STEP, 0);
            return 0;
        case '8': case VK_NUMPAD8:
            panzoom_change_pan(&ctx->pz, ctx->pb, 0, PAN_STEP);
            return 0;
        case '2': case VK_NUMPAD2:
            panzoom_change_pan(&ctx->pz, ctx->pb, 0, -PAN_STEP);
            return 0;

        /* ── Zoom ────────────────────────────────────────────── */
        case '9': case VK_NUMPAD9:
        case VK_OEM_MINUS: case VK_SUBTRACT:
            panzoom_change_zoom(&ctx->pz, ctx->pb, -ZOOM_STEP);
            return 0;
        case '3': case VK_NUMPAD3:
        case VK_OEM_PLUS: case VK_ADD:
            panzoom_change_zoom(&ctx->pz, ctx->pb, ZOOM_STEP);
            return 0;
        case '0': case VK_NUMPAD0:
            panzoom_reset(&ctx->pz, ctx->pb);
            return 0;

        /* ── Reset all ───────────────────────────────────────── */
        case 'A':
            panzoom_reset(&ctx->pz, ctx->pb);
            playback_set_speed(ctx->pb, 1.0);
            return 0;
        }
        break;

    /* ── Hide cursor over the video area ───────────────────────── */
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        break;

    /* ── Cleanup ───────────────────────────────────────────────── */
    case WM_DESTROY:
        /* Free per-window context (Playback is owned by main). */
        if (ctx) {
            free(ctx);
            SetWindowLongPtrW(hw, GWLP_USERDATA, 0);
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hw, msg, wp, lp);
}

/* ── Public API ──────────────────────────────────────────────── */

HWND player_create(HINSTANCE hi, const RECT *screen_rect)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = player_proc;
    wc.hInstance      = hi;
    wc.hIcon          = LoadIconW(hi, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm        = LoadIconW(hi, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName  = PLAYER_CLASS;
    RegisterClassExW(&wc);

    HWND hw = CreateWindowExW(
        WS_EX_TOPMOST,
        PLAYER_CLASS, APP_TITLE,
        WS_POPUP | WS_VISIBLE,
        screen_rect->left,  screen_rect->top,
        screen_rect->right  - screen_rect->left,
        screen_rect->bottom - screen_rect->top,
        NULL, NULL, hi, NULL);

    if (!hw) return NULL;

    /* Allocate and attach per-window context. */
    PlayerCtx *ctx = (PlayerCtx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        DestroyWindow(hw);
        return NULL;
    }
    panzoom_init(&ctx->pz);
    SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)ctx);

    SetForegroundWindow(hw);
    SetFocus(hw);
    return hw;
}

void player_set_playback(HWND player, Playback *pb)
{
    PlayerCtx *ctx =
        (PlayerCtx *)(LONG_PTR)GetWindowLongPtrW(player, GWLP_USERDATA);
    if (ctx) ctx->pb = pb;
}

void player_set_crop(HWND player, int crop_bottom_px)
{
    PlayerCtx *ctx =
        (PlayerCtx *)(LONG_PTR)GetWindowLongPtrW(player, GWLP_USERDATA);
    if (ctx) {
        ctx->crop_bottom_px = crop_bottom_px;
        ctx->crop_applied   = FALSE;
    }
}
