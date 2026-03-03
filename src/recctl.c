/*
 * recctl.c – Recording control window
 *
 * Small dark-themed floating window with:
 *   - A blinking recording indicator (red/gray dot)
 *   - Start / Stop toggle button
 *   - Pause / Resume toggle button (enabled only while recording)
 *
 * DPI-aware: handles WM_DPICHANGED and scales all metrics.
 */
#include "recctl.h"
#include "constants.h"
#include "monitors.h"
#include "recorder.h"
#include "rectview.h"
#include "resource.h"
#include "theme.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ── Recording states ────────────────────────────────────────── */

typedef enum {
    RS_IDLE,        /* not yet started                         */
    RS_STARTING,    /* recorder created, pipeline booting      */
    RS_RECORDING,   /* actively recording                      */
    RS_PAUSED,      /* recording paused                        */
    RS_STOPPED      /* stopped / finalising                    */
} RecState;

/* ── Per-window context ──────────────────────────────────────── */

typedef struct {
    const RecCtlParams *params;
    Recorder           *rec;
    RecState            state;
    ThemeCtx            theme;
    BOOL                blink_on;   /* indicator blink phase    */
    BOOL                mouse_cap;  /* capture mouse cursor     */

    /* Display info */
    wchar_t             info_line[512];

    /* Elapsed time tracking */
    ULONGLONG           rec_start_ms;
    ULONGLONG           pause_start_ms;
    ULONGLONG           total_paused_ms;
    wchar_t             time_label[16];

    /* Source selection */
    int                 sel_mode;   /* 0..nmons-1 = screen, nmons = custom */
    RECT                cur_rect;   /* mutable capture rectangle */
    BOOL                custom_vis; /* custom-rect fields visible */
    HBITMAP             thumbs[DMP_MAX_MONITORS]; /* screen thumbnails */
    int                 thumb_w;    /* thumbnail width  (pixels) */
    int                 thumb_h;    /* thumbnail height (pixels) */
} RecCtlCtx;

/* ── Async stop (runs recorder_stop + destroy off the UI thread) ── */

typedef struct {
    HWND      hw;
    Recorder *rec;
} StopThreadData;

static DWORD WINAPI stop_thread_proc(LPVOID param)
{
    StopThreadData *d = (StopThreadData *)param;
    recorder_stop(d->rec);
    recorder_destroy(d->rec);
    PostMessageW(d->hw, WM_REC_STOP_DONE, 0, 0);
    free(d);
    return 0;
}

static void update_buttons(HWND hw, RecCtlCtx *ctx);   /* forward decl */
static BOOL CALLBACK destroy_children_cb(HWND child, LPARAM lp);
static void build_ui(HWND hw, HINSTANCE hi, RecCtlCtx *ctx);

static void begin_async_stop(HWND hw, RecCtlCtx *ctx)
{
    if (!ctx->rec) { DestroyWindow(hw); return; }
    ctx->state = RS_STOPPED;
    update_buttons(hw, ctx);

    StopThreadData *d = (StopThreadData *)malloc(sizeof(*d));
    d->hw  = hw;
    d->rec = ctx->rec;
    ctx->rec = NULL;   /* ownership transferred to thread */

    HANDLE h = CreateThread(NULL, 0, stop_thread_proc, d, 0, NULL);
    if (h) {
        CloseHandle(h);   /* fire-and-forget */
    } else {
        /* Thread creation failed — fall back to sync */
        recorder_stop(d->rec);
        recorder_destroy(d->rec);
        free(d);
        DestroyWindow(hw);
    }
}

/* ── DPI helper ──────────────────────────────────────────────── */

static int sdpi(const RecCtlCtx *ctx, int v)
{
    return theme_dpi_scale(&ctx->theme, v);
}

/* ── Build the info string for the status area ───────────────── */

static void build_info(RecCtlCtx *ctx)
{
    int w = (int)(ctx->cur_rect.right  - ctx->cur_rect.left);
    int h = (int)(ctx->cur_rect.bottom - ctx->cur_rect.top);

    if (ctx->sel_mode < ctx->params->nmons) {
        swprintf(ctx->info_line, 512,
                 L"Screen %d  \u00B7  %d\u00D7%d  \u00B7  %d fps  \u00B7  H.265 CRF %d",
                 ctx->sel_mode + 1, w, h, ctx->params->fps, REC_DEFAULT_CRF);
    } else {
        swprintf(ctx->info_line, 512,
                 L"Custom  \u00B7  %d\u00D7%d  at (%d,%d)  \u00B7  %d fps  \u00B7  H.265 CRF %d",
                 w, h, (int)ctx->cur_rect.left, (int)ctx->cur_rect.top,
                 ctx->params->fps, REC_DEFAULT_CRF);
    }
}

/* ── Capture screen thumbnails (once) ────────────────────────── */

static void capture_thumbnails(RecCtlCtx *ctx)
{
    HDC hdc_scr = GetDC(NULL);
    for (int i = 0; i < ctx->params->nmons; i++) {
        const RECT *mr = &ctx->params->monitors[i].rect;
        int sw = (int)(mr->right  - mr->left);
        int sh = (int)(mr->bottom - mr->top);
        if (sw < 1 || sh < 1) continue;

        HDC hdc_full  = CreateCompatibleDC(hdc_scr);
        HBITMAP bfull  = CreateCompatibleBitmap(hdc_scr, sw, sh);
        HBITMAP ofull  = (HBITMAP)SelectObject(hdc_full, bfull);
        BitBlt(hdc_full, 0, 0, sw, sh,
               hdc_scr, mr->left, mr->top, SRCCOPY);

        HDC hdc_th     = CreateCompatibleDC(hdc_scr);
        HBITMAP bthumb  = CreateCompatibleBitmap(hdc_scr,
                                                  ctx->thumb_w,
                                                  ctx->thumb_h);
        HBITMAP othumb  = (HBITMAP)SelectObject(hdc_th, bthumb);
        SetStretchBltMode(hdc_th, HALFTONE);
        SetBrushOrgEx(hdc_th, 0, 0, NULL);
        StretchBlt(hdc_th, 0, 0, ctx->thumb_w, ctx->thumb_h,
                   hdc_full, 0, 0, sw, sh, SRCCOPY);

        SelectObject(hdc_th, othumb);
        SelectObject(hdc_full, ofull);
        DeleteObject(bfull);
        DeleteDC(hdc_full);
        DeleteDC(hdc_th);

        ctx->thumbs[i] = bthumb;
    }
    ReleaseDC(NULL, hdc_scr);
}

static void free_thumbnails(RecCtlCtx *ctx)
{
    for (int i = 0; i < DMP_MAX_MONITORS; i++) {
        if (ctx->thumbs[i]) {
            DeleteObject(ctx->thumbs[i]);
            ctx->thumbs[i] = NULL;
        }
    }
}

/* ── Show / hide custom rect fields & resize window ──────────── */

static void toggle_custom_fields(HWND hw, RecCtlCtx *ctx, BOOL show)
{
    if (ctx->custom_vis == show) return;
    ctx->custom_vis = show;
    ctx->theme.hover_btn = NULL;

    /* Compute new client height: 200 base (non-custom) or 240 (custom) */
    int base_ch = show ? 240 : 200;
    RECT adj = {0, 0, 100,
                MulDiv(base_ch, (int)ctx->theme.dpi, 96)};
    AdjustWindowRectEx(&adj,
                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                       FALSE, WS_EX_TOPMOST);
    int new_h = adj.bottom - adj.top;

    RECT wr;
    GetWindowRect(hw, &wr);
    SetWindowPos(hw, NULL, 0, 0, wr.right - wr.left, new_h,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    /* Rebuild all controls at correct positions */
    EnumChildWindows(hw, destroy_children_cb, 0);
    build_ui(hw, (HINSTANCE)GetWindowLongPtrW(hw, GWLP_HINSTANCE), ctx);
    InvalidateRect(hw, NULL, TRUE);
}

/* ── Read custom-rect fields into cur_rect ───────────────────── */

static void read_custom_fields(HWND hw, RecCtlCtx *ctx)
{
    wchar_t buf[32];
    GetDlgItemTextW(hw, IDC_REC_CUSTOM_X, buf, 32);
    int cx = _wtoi(buf);
    GetDlgItemTextW(hw, IDC_REC_CUSTOM_Y, buf, 32);
    int cy = _wtoi(buf);
    GetDlgItemTextW(hw, IDC_REC_CUSTOM_W, buf, 32);
    int cw = _wtoi(buf);
    GetDlgItemTextW(hw, IDC_REC_CUSTOM_H, buf, 32);
    int ch = _wtoi(buf);
    if (cw < 16) cw = 16;
    if (ch < 16) ch = 16;
    ctx->cur_rect.left   = cx;
    ctx->cur_rect.top    = cy;
    ctx->cur_rect.right  = cx + cw;
    ctx->cur_rect.bottom = cy + ch;
}

/* ── Rebuild child controls at current DPI ───────────────────── */

static BOOL CALLBACK destroy_children_cb(HWND child, LPARAM lp)
{
    (void)lp;
    DestroyWindow(child);
    return TRUE;
}

static void build_ui(HWND hw, HINSTANCE hi, RecCtlCtx *ctx)
{
    RECT cr;
    GetClientRect(hw, &cr);
    int cw = cr.right;
    int mx = sdpi(ctx, 20);
    int ew = cw - 2 * mx;
    int gap = sdpi(ctx, 10);
    HWND c;
    BOOL recording = (ctx->state != RS_IDLE);

    /* Running y cursor — starts below the painted status / info area */
    int y = sdpi(ctx, 62);

    /* ── Source combo box ─────────────────────────────────────── */
    int combo_vis_h = sdpi(ctx, 36);  /* matches WM_MEASUREITEM */
    int combo_drop_h = combo_vis_h * (ctx->params->nmons + 2);

    c = CreateWindowExW(0, L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE
            | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED
            | CBS_HASSTRINGS | WS_VSCROLL
            | (recording ? WS_DISABLED : 0),
            mx, y, ew, combo_drop_h,
            hw, (HMENU)(intptr_t)IDC_REC_SOURCE, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)ctx->theme.ui_font, TRUE);
    SetWindowTheme(c, L"DarkMode_CFD", NULL);

    /* Populate: one item per screen + "Custom" */
    for (int i = 0; i < ctx->params->nmons; i++) {
        const RECT *mr = &ctx->params->monitors[i].rect;
        wchar_t label[128];
        swprintf(label, 128, L"Screen %d  (%d \u00D7 %d)",
                 i + 1,
                 (int)(mr->right - mr->left),
                 (int)(mr->bottom - mr->top));
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)label);
    }
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Custom\u2026");
    SendMessageW(c, CB_SETCURSEL, (WPARAM)ctx->sel_mode, 0);

    y += combo_vis_h + sdpi(ctx, 10);  /* advance past collapsed combo */

    /* ── Custom rect fields (hidden unless sel==custom) ───────── */
    int lbl_w = sdpi(ctx, 18);
    int ed_w  = sdpi(ctx, 48);
    int fld_gap = sdpi(ctx, 6);
    int fld_h = sdpi(ctx, 26);
    DWORD vis  = ctx->custom_vis ? WS_VISIBLE : 0;
    int fx = mx;

    const wchar_t *labels[] = { L"X:", L"Y:", L"W:", L"H:" };
    int edit_ids[] = { IDC_REC_CUSTOM_X, IDC_REC_CUSTOM_Y,
                       IDC_REC_CUSTOM_W, IDC_REC_CUSTOM_H };
    int lbl_ids[] = { 3200, 3201, 3202, 3203 };
    int vals[4];
    vals[0] = (int)ctx->cur_rect.left;
    vals[1] = (int)ctx->cur_rect.top;
    vals[2] = (int)(ctx->cur_rect.right  - ctx->cur_rect.left);
    vals[3] = (int)(ctx->cur_rect.bottom - ctx->cur_rect.top);

    for (int i = 0; i < 4; i++) {
        /* Label */
        c = CreateWindowExW(0, L"STATIC", labels[i],
                WS_CHILD | vis | SS_RIGHT,
                fx, y + sdpi(ctx, 3), lbl_w, fld_h,
                hw, (HMENU)(intptr_t)lbl_ids[i], hi, NULL);
        SendMessageW(c, WM_SETFONT, (WPARAM)ctx->theme.label_font, TRUE);
        fx += lbl_w + sdpi(ctx, 2);

        /* Edit */
        wchar_t val[16];
        swprintf(val, 16, L"%d", vals[i]);
        c = CreateWindowExW(0, L"EDIT", val,
                WS_CHILD | vis | ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER
                | (recording ? WS_DISABLED : 0),
                fx, y, ed_w, fld_h,
                hw, (HMENU)(intptr_t)edit_ids[i], hi, NULL);
        SendMessageW(c, WM_SETFONT, (WPARAM)ctx->theme.ui_font, TRUE);
        fx += ed_w + fld_gap;
    }

    /* Preview button */
    int pv_w = sdpi(ctx, 80);
    c = CreateWindowExW(0, L"BUTTON", L"Preview \u25B7",
            WS_CHILD | vis | BS_OWNERDRAW
            | (recording ? WS_DISABLED : 0),
            fx, y, pv_w, fld_h,
            hw, (HMENU)(intptr_t)IDC_REC_PREVIEW, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)ctx->theme.ui_font, TRUE);
    theme_subclass_button(&ctx->theme, c);

    if (ctx->custom_vis)
        y += fld_h + sdpi(ctx, 10);  /* advance past custom row */

    /* ── Start/Stop button ────────────────────────────────────── */
    int btn_h = sdpi(ctx, 38);
    int btn_w = (ew - gap) / 2;

    const wchar_t *start_label =
        (ctx->state == RS_IDLE) ? L"\u25CF  Record" : L"\u25A0  Stop";
    BOOL ss_disabled = (ctx->state == RS_STARTING);

    c = CreateWindowExW(0, L"BUTTON", start_label,
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW
            | (ss_disabled ? WS_DISABLED : 0),
            mx, y, btn_w, btn_h,
            hw, (HMENU)(intptr_t)IDC_REC_STARTSTOP, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)ctx->theme.ui_font, TRUE);
    theme_subclass_button(&ctx->theme, c);

    /* ── Pause/Resume button ──────────────────────────────────── */
    const wchar_t *pause_label =
        (ctx->state == RS_PAUSED) ? L"Resume" : L"Pause";

    c = CreateWindowExW(0, L"BUTTON", pause_label,
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW
            | ((ctx->state == RS_RECORDING || ctx->state == RS_PAUSED)
               ? 0 : WS_DISABLED),
            mx + btn_w + gap, y, ew - btn_w - gap, btn_h,
            hw, (HMENU)(intptr_t)IDC_REC_PAUSE, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)ctx->theme.ui_font, TRUE);
    theme_subclass_button(&ctx->theme, c);

    y += btn_h + sdpi(ctx, 12);

    /* ── Mouse capture checkbox ────────────────────────────────── */
    c = CreateWindowExW(0, L"BUTTON", L"Capture mouse cursor",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            mx, y, ew, sdpi(ctx, 24),
            hw, (HMENU)(intptr_t)IDC_REC_MOUSE, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)ctx->theme.ui_font, TRUE);
    SetWindowTheme(c, L"DarkMode_Explorer", NULL);
    if (ctx->mouse_cap)
        SendMessageW(c, BM_SETCHECK, BST_CHECKED, 0);
}

/* ── Update button labels and enabled state ──────────────────── */

static void update_buttons(HWND hw, RecCtlCtx *ctx)
{
    HWND btn_ss = GetDlgItem(hw, IDC_REC_STARTSTOP);
    HWND btn_pa = GetDlgItem(hw, IDC_REC_PAUSE);
    if (!btn_ss || !btn_pa) return;

    const wchar_t *ss_text =
        (ctx->state == RS_IDLE) ? L"\u25CF  Record" : L"\u25A0  Stop";
    SetWindowTextW(btn_ss, ss_text);
    EnableWindow(btn_ss, ctx->state != RS_STARTING);

    const wchar_t *pa_text =
        (ctx->state == RS_PAUSED) ? L"Resume" : L"Pause";
    SetWindowTextW(btn_pa, pa_text);

    BOOL pa_enabled = (ctx->state == RS_RECORDING ||
                       ctx->state == RS_PAUSED);
    EnableWindow(btn_pa, pa_enabled);

    /* Disable source selection while recording */
    BOOL idle = (ctx->state == RS_IDLE);
    HWND combo = GetDlgItem(hw, IDC_REC_SOURCE);
    if (combo) EnableWindow(combo, idle);
    int edit_ids[] = { IDC_REC_CUSTOM_X, IDC_REC_CUSTOM_Y,
                       IDC_REC_CUSTOM_W, IDC_REC_CUSTOM_H };
    for (int i = 0; i < 4; i++) {
        HWND e = GetDlgItem(hw, edit_ids[i]);
        if (e) EnableWindow(e, idle);
    }
    HWND pv = GetDlgItem(hw, IDC_REC_PREVIEW);
    if (pv) EnableWindow(pv, idle);

    InvalidateRect(btn_ss, NULL, FALSE);
    InvalidateRect(btn_pa, NULL, FALSE);
    InvalidateRect(hw, NULL, TRUE);
}

/* ── Paint the custom indicator + info area ──────────────────── */

static void paint_window(HWND hw, RecCtlCtx *ctx)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hw, &ps);
    RECT cr;
    GetClientRect(hw, &cr);

    /* Background */
    FillRect(hdc, &cr, ctx->theme.br_bg);

    int mx = sdpi(ctx, 20);
    int y  = sdpi(ctx, 16);

    /* ── Recording indicator dot ──────────────────────────────── */
    int dot_r = sdpi(ctx, 8);
    int dot_cx = mx + dot_r;
    int dot_cy = y + dot_r;

    COLORREF dot_clr;
    switch (ctx->state) {
    case RS_RECORDING:    case RS_STARTING:        dot_clr = ctx->blink_on ? CLR_REC_ACTIVE : CLR_BG;
        break;
    case RS_PAUSED:
        dot_clr = ctx->blink_on ? CLR_REC_PAUSED : CLR_BG;
        break;
    default:
        dot_clr = CLR_REC_INACTIVE;
        break;
    }

    HBRUSH dot_br = CreateSolidBrush(dot_clr);
    HBRUSH old_br = (HBRUSH)SelectObject(hdc, dot_br);
    HPEN   npen   = (HPEN)GetStockObject(NULL_PEN);
    HPEN   old_pn = (HPEN)SelectObject(hdc, npen);
    Ellipse(hdc, dot_cx - dot_r, dot_cy - dot_r,
                 dot_cx + dot_r, dot_cy + dot_r);
    SelectObject(hdc, old_br);
    SelectObject(hdc, old_pn);
    DeleteObject(dot_br);

    /* ── Status text next to the dot ──────────────────────────── */
    const wchar_t *status_text;
    switch (ctx->state) {
    case RS_STARTING:  status_text = L"Starting\u2026";   break;
    case RS_RECORDING: status_text = L"Recording";  break;
    case RS_PAUSED:    status_text = L"Paused";     break;
    case RS_STOPPED:   status_text = L"Finalizing\u2026"; break;
    default:           status_text = L"Ready";       break;
    }

    SetTextColor(hdc, CLR_TEXT);
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, ctx->theme.ui_font);

    RECT tr;
    tr.left   = dot_cx + dot_r + sdpi(ctx, 10);
    tr.top    = y - sdpi(ctx, 2);
    tr.right  = cr.right - mx;
    tr.bottom = y + sdpi(ctx, 22);
    DrawTextW(hdc, status_text, -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* ── Info line below ──────────────────────────────────────── */
    SetTextColor(hdc, CLR_TEXT_DIM);
    SelectObject(hdc, ctx->theme.label_font);

    RECT ir;
    ir.left   = mx;
    ir.top    = y + sdpi(ctx, 24);
    ir.right  = cr.right - mx;
    ir.bottom = ir.top + sdpi(ctx, 16);
    DrawTextW(hdc, ctx->info_line, -1, &ir,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* ── Elapsed time (right-aligned on the status line) ────── */
    if (ctx->time_label[0] &&
        (ctx->state == RS_RECORDING || ctx->state == RS_PAUSED)) {
        SetTextColor(hdc, CLR_TEXT);
        SelectObject(hdc, ctx->theme.ui_font);
        RECT tmr;
        tmr.left   = cr.right / 2;
        tmr.top    = y - sdpi(ctx, 2);
        tmr.right  = cr.right - mx;
        tmr.bottom = y + sdpi(ctx, 22);
        DrawTextW(hdc, ctx->time_label, -1, &tmr,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    EndPaint(hw, &ps);
}

/* ── Window procedure ────────────────────────────────────────── */

static LRESULT CALLBACK recctl_proc(HWND hw, UINT msg,
                                     WPARAM wp, LPARAM lp)
{
    RecCtlCtx *ctx;

    if (msg == WM_CREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        ctx = (RecCtlCtx *)cs->lpCreateParams;
        SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)ctx);

        ctx->theme.dpi = dpi_for_window(hw);
        theme_create_brushes(&ctx->theme);
        theme_create_fonts(&ctx->theme);
        /* Scale thumbnail dimensions to current DPI */
        ctx->thumb_w = dpi_scale(64, ctx->theme.dpi);
        ctx->thumb_h = dpi_scale(36, ctx->theme.dpi);
        build_info(ctx);
        build_ui(hw, cs->hInstance, ctx);

        /* Start blink timer */
        SetTimer(hw, REC_TICK_TIMER, REC_TICK_INTERVAL, NULL);

        /* Register global hotkeys: Ctrl+F9 start/stop, Ctrl+F10 pause/resume */
        RegisterHotKey(hw, HOTKEY_REC_TOGGLE, MOD_CONTROL, VK_F9);
        RegisterHotKey(hw, HOTKEY_REC_PAUSE,  MOD_CONTROL, VK_F10);
        return 0;
    }

    ctx = (RecCtlCtx *)(LONG_PTR)GetWindowLongPtrW(hw, GWLP_USERDATA);
    if (!ctx) return DefWindowProcW(hw, msg, wp, lp);

    switch (msg) {

    case WM_PAINT:
        paint_window(hw, ctx);
        return 0;

    case WM_ERASEBKGND:
        return 1;   /* we paint the entire background in WM_PAINT */

    /* ── Global hotkeys (Ctrl+F9 / Ctrl+F10) ───────────────────── */
    case WM_HOTKEY:
        if (wp == HOTKEY_REC_TOGGLE)
            SendMessageW(hw, WM_COMMAND,
                         MAKEWPARAM(IDC_REC_STARTSTOP, BN_CLICKED), 0);
        else if (wp == HOTKEY_REC_PAUSE)
            SendMessageW(hw, WM_COMMAND,
                         MAKEWPARAM(IDC_REC_PAUSE, BN_CLICKED), 0);
        return 0;

    case WM_TIMER:
        if (wp == REC_TICK_TIMER) {
            ctx->blink_on = !ctx->blink_on;
            /* Invalidate the status area (dot + text + elapsed time) */
            {
                RECT top_rc;
                GetClientRect(hw, &top_rc);
                top_rc.bottom = sdpi(ctx, 36);
                InvalidateRect(hw, &top_rc, FALSE);
            }

            /* Transition from STARTING to RECORDING once pipeline is active */
            if (ctx->state == RS_STARTING && ctx->rec &&
                recorder_active(ctx->rec)) {
                ctx->state = RS_RECORDING;
                ctx->rec_start_ms    = GetTickCount64();
                ctx->total_paused_ms = 0;
                wcscpy(ctx->time_label, L"00:00:00");
                update_buttons(hw, ctx);
            }

            /* Poll the recorder for errors */
            if (ctx->rec && (ctx->state == RS_STARTING ||
                             ctx->state == RS_RECORDING ||
                             ctx->state == RS_PAUSED)) {
                int r = recorder_poll(ctx->rec);
                if (r != 0) {
                    ctx->state = RS_STOPPED;
                    update_buttons(hw, ctx);
                    if (r < 0) {
                        const char *err = recorder_last_error(ctx->rec);
                        wchar_t msg[768];
                        if (err && err[0]) {
                            wchar_t werr[512];
                            MultiByteToWideChar(CP_UTF8, 0, err, -1,
                                                werr, 512);
                            swprintf(msg, 768,
                                     L"Recording encountered an error.\n\n%s",
                                     werr);
                        } else {
                            wcscpy(msg,
                                   L"Recording encountered an error.");
                        }
                        MessageBoxW(hw, msg, APP_TITLE, MB_ICONERROR);
                    }
                }
            }

            /* Update elapsed time label */
            if (ctx->state == RS_RECORDING || ctx->state == RS_PAUSED) {
                ULONGLONG now = GetTickCount64();
                ULONGLONG elapsed = now - ctx->rec_start_ms
                                    - ctx->total_paused_ms;
                if (ctx->state == RS_PAUSED)
                    elapsed -= (now - ctx->pause_start_ms);
                int secs = (int)(elapsed / 1000);
                swprintf(ctx->time_label, 16, L"%02d:%02d:%02d",
                         secs / 3600, (secs % 3600) / 60, secs % 60);
            }
        }
        return 0;

    /* ── Owner-draw combo (source selector with thumbnails) ────── */
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lp;
        if (mi->CtlType == ODT_COMBOBOX) {
            mi->itemHeight = sdpi(ctx, 36);
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;

        /* ── Combo box items (screen thumbnails + text) ──────── */
        if (di->CtlType == ODT_COMBOBOX &&
            di->CtlID == IDC_REC_SOURCE) {
            BOOL selected = (di->itemState & ODS_SELECTED);
            COLORREF bg = selected ? CLR_ACCENT : CLR_INPUT_BG;
            HBRUSH br = CreateSolidBrush(bg);
            FillRect(di->hDC, &di->rcItem, br);
            DeleteObject(br);

            if (di->itemID != (UINT)-1) {
                int pad = sdpi(ctx, 4);
                int tx  = di->rcItem.left + pad;

                /* Draw thumbnail for screen items */
                int idx = (int)di->itemID;
                if (idx < ctx->params->nmons && ctx->thumbs[idx]) {
                    int th = (di->rcItem.bottom - di->rcItem.top) - 2 * pad;
                    int tw = MulDiv(th, 16, 9);  /* 16:9 aspect */
                    if (tw > ctx->thumb_w) tw = ctx->thumb_w;
                    if (th > ctx->thumb_h) th = ctx->thumb_h;
                    int ty = di->rcItem.top + pad;

                    HDC hdc_mem = CreateCompatibleDC(di->hDC);
                    HBITMAP old_bm = (HBITMAP)SelectObject(hdc_mem,
                                                          ctx->thumbs[idx]);
                    SetStretchBltMode(di->hDC, HALFTONE);
                    StretchBlt(di->hDC, tx, ty, tw, th,
                               hdc_mem, 0, 0,
                               ctx->thumb_w, ctx->thumb_h, SRCCOPY);
                    SelectObject(hdc_mem, old_bm);
                    DeleteDC(hdc_mem);

                    /* Thin border around thumbnail */
                    HPEN tp = CreatePen(PS_SOLID, 1, CLR_BORDER);
                    HPEN otp = (HPEN)SelectObject(di->hDC, tp);
                    HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
                    HBRUSH onb = (HBRUSH)SelectObject(di->hDC, nb);
                    Rectangle(di->hDC, tx, ty, tx + tw, ty + th);
                    SelectObject(di->hDC, otp);
                    SelectObject(di->hDC, onb);
                    DeleteObject(tp);

                    tx += tw + sdpi(ctx, 8);
                }

                /* Text label */
                wchar_t buf[256];
                SendMessageW(di->hwndItem, CB_GETLBTEXT,
                             di->itemID, (LPARAM)buf);
                SetTextColor(di->hDC, CLR_TEXT);
                SetBkMode(di->hDC, TRANSPARENT);
                SelectObject(di->hDC, ctx->theme.ui_font);
                RECT tr = di->rcItem;
                tr.left = tx;
                DrawTextW(di->hDC, buf, -1, &tr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            return TRUE;
        }

        /* ── Owner-draw buttons ──────────────────────────────── */
        if (di->CtlType == ODT_BUTTON) {
            BOOL is_primary = (di->CtlID == IDC_REC_STARTSTOP);
            BOOL hover   = (ctx->theme.hover_btn == di->hwndItem);
            BOOL pressed = (di->itemState & ODS_SELECTED);
            BOOL disabled = (di->itemState & ODS_DISABLED);

            COLORREF bg, border;
            if (disabled) {
                bg = CLR_BTN_SEC_PRESS;
                border = CLR_BORDER;
            } else if (is_primary) {
                COLORREF base = (ctx->state != RS_IDLE)
                                ? CLR_REC_ACTIVE : CLR_ACCENT;
                COLORREF hov  = (ctx->state != RS_IDLE)
                                ? RGB(200, 30, 30) : CLR_ACCENT_HOV;
                COLORREF prs  = (ctx->state != RS_IDLE)
                                ? RGB(160, 20, 20) : CLR_ACCENT_PRESS;
                bg = pressed ? prs : hover ? hov : base;
                border = bg;
            } else {
                bg = pressed ? CLR_BTN_SEC_PRESS
                   : hover   ? CLR_BTN_SEC_HOV
                   :           CLR_BTN_SEC;
                border = CLR_BORDER;
            }

            int s  = sdpi(ctx, 8);

            FillRect(di->hDC, &di->rcItem, ctx->theme.br_bg);

            HBRUSH bbr = CreateSolidBrush(bg);
            HPEN   bpn = CreatePen(PS_SOLID, 1, border);
            HBRUSH obr = (HBRUSH)SelectObject(di->hDC, bbr);
            HPEN   opn = (HPEN)SelectObject(di->hDC, bpn);
            RoundRect(di->hDC,
                      di->rcItem.left, di->rcItem.top,
                      di->rcItem.right, di->rcItem.bottom,
                      s, s);
            SelectObject(di->hDC, obr);
            SelectObject(di->hDC, opn);
            DeleteObject(bbr);
            DeleteObject(bpn);

            SetTextColor(di->hDC,
                         disabled ? CLR_TEXT_DIM : CLR_TEXT);
            SetBkMode(di->hDC, TRANSPARENT);
            SelectObject(di->hDC, ctx->theme.ui_font);
            wchar_t txt[64];
            GetWindowTextW(di->hwndItem, txt, 64);
            DrawTextW(di->hDC, txt, -1, &di->rcItem,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            return TRUE;
        }
        break;
    }

    /* ── Dark colour for labels / checkbox / edit ───────────────── */
    case WM_CTLCOLORSTATIC:
        return theme_handle_ctlcolorstatic(&ctx->theme,
                                            (HDC)wp, (HWND)lp);

    case WM_CTLCOLOREDIT: {
        HDC hdc_edit = (HDC)wp;
        SetTextColor(hdc_edit, CLR_TEXT);
        SetBkColor(hdc_edit, CLR_INPUT_BG);
        return (LRESULT)ctx->theme.br_input;
    }

    case WM_CTLCOLORLISTBOX:
        return theme_handle_ctlcolorlistbox(&ctx->theme, (HDC)wp);

    /* ── Commands ──────────────────────────────────────────────── */
    case WM_COMMAND:
        switch (LOWORD(wp)) {

        /* ── Source combo selection changed ──────────────────── */
        case IDC_REC_SOURCE:
            if (HIWORD(wp) == CBN_SELCHANGE && ctx->state == RS_IDLE) {
                int sel = (int)SendDlgItemMessageW(
                    hw, IDC_REC_SOURCE, CB_GETCURSEL, 0, 0);
                if (sel < 0) sel = 0;
                ctx->sel_mode = sel;

                if (sel < ctx->params->nmons) {
                    /* A full-screen monitor */
                    ctx->cur_rect = ctx->params->monitors[sel].rect;
                    toggle_custom_fields(hw, ctx, FALSE);
                } else {
                    /* Custom – read current edit values */
                    toggle_custom_fields(hw, ctx, TRUE);
                    read_custom_fields(hw, ctx);
                }
                build_info(ctx);
                InvalidateRect(hw, NULL, TRUE);
            }
            return 0;

        /* ── Custom-rect edits changed ─────────────────────────── */
        case IDC_REC_CUSTOM_X:
        case IDC_REC_CUSTOM_Y:
        case IDC_REC_CUSTOM_W:
        case IDC_REC_CUSTOM_H:
            if (HIWORD(wp) == EN_CHANGE && ctx->custom_vis) {
                read_custom_fields(hw, ctx);
                build_info(ctx);
                InvalidateRect(hw, NULL, TRUE);
            }
            return 0;

        /* ── Preview button – toggle rect overlay ──────────────── */
        case IDC_REC_PREVIEW:
            if (rectview_visible()) {
                rectview_dismiss();
            } else {
                if (ctx->custom_vis) {
                    read_custom_fields(hw, ctx);
                    build_info(ctx);
                    InvalidateRect(hw, NULL, TRUE);
                }
                rectview_show(ctx->params->hi, &ctx->cur_rect, hw);
            }
            return 0;

        case IDC_REC_STARTSTOP:
            if (ctx->state == RS_IDLE) {
                /* If custom mode, finalise rect from edit fields */
                if (ctx->sel_mode >= ctx->params->nmons)
                    read_custom_fields(hw, ctx);

                /* Start recording */
                ctx->rec = recorder_create(
                    &ctx->cur_rect,
                    ctx->params->output_u8,
                    ctx->params->fps,
                    ctx->params->audio_u8,
                    ctx->mouse_cap);

                if (!ctx->rec && ctx->params->audio_u8) {
                    /* Retry without audio */
                    ctx->rec = recorder_create(
                        &ctx->cur_rect,
                        ctx->params->output_u8,
                        ctx->params->fps,
                        NULL,
                        ctx->mouse_cap);
                }

                if (!ctx->rec) {
                    const char *err = recorder_create_error();
                    wchar_t msg[768];
                    if (err && err[0]) {
                        wchar_t werr[512];
                        MultiByteToWideChar(CP_UTF8, 0, err, -1,
                                            werr, 512);
                        swprintf(msg, 768,
                                 L"Could not start recording.\n\n%s",
                                 werr);
                    } else {
                        wcscpy(msg,
                               L"Could not start recording.\n"
                               L"Check debug.log for details.");
                    }
                    MessageBoxW(hw, msg, APP_TITLE, MB_ICONERROR);
                    return 0;
                }
                ctx->state = RS_STARTING;
            } else if (ctx->state != RS_STARTING) {
                /* Stop recording (async — keeps UI responsive) */
                begin_async_stop(hw, ctx);
                return 0;
            }
            update_buttons(hw, ctx);
            return 0;

        case IDC_REC_PAUSE:
            if (ctx->state == RS_RECORDING) {
                recorder_pause(ctx->rec);
                ctx->pause_start_ms = GetTickCount64();
                ctx->state = RS_PAUSED;
            } else if (ctx->state == RS_PAUSED) {
                recorder_resume(ctx->rec);
                ctx->total_paused_ms += GetTickCount64()
                                        - ctx->pause_start_ms;
                ctx->state = RS_RECORDING;
            }
            update_buttons(hw, ctx);
            return 0;

        case IDC_REC_MOUSE: {
            /* Toggle mouse cursor capture — works live during recording */
            HWND cb = GetDlgItem(hw, IDC_REC_MOUSE);
            ctx->mouse_cap = (SendMessageW(cb, BM_GETCHECK, 0, 0)
                              == BST_CHECKED);
            if (ctx->rec)
                recorder_set_mouse_capture(ctx->rec, ctx->mouse_cap);
            return 0;
        }

        case IDCANCEL:  /* ESC via IsDialogMessage */
            if (ctx->state == RS_STARTING || ctx->state == RS_RECORDING ||
                ctx->state == RS_PAUSED) {
                begin_async_stop(hw, ctx);
            } else if (ctx->state != RS_STOPPED) {
                DestroyWindow(hw);
            }
            return 0;
        }
        break;

    /* ── Overlay moved / resized → update custom fields ────────── */
    case WM_RECTVIEW_CHANGED: {
        const RECT *nr = (const RECT *)lp;
        if (nr) {
            ctx->cur_rect = *nr;
            /* Push values into the edit controls (if visible) */
            if (ctx->custom_vis) {
                wchar_t buf[32];
                swprintf(buf, 32, L"%d", (int)nr->left);
                SetDlgItemTextW(hw, IDC_REC_CUSTOM_X, buf);
                swprintf(buf, 32, L"%d", (int)nr->top);
                SetDlgItemTextW(hw, IDC_REC_CUSTOM_Y, buf);
                swprintf(buf, 32, L"%d", (int)(nr->right - nr->left));
                SetDlgItemTextW(hw, IDC_REC_CUSTOM_W, buf);
                swprintf(buf, 32, L"%d", (int)(nr->bottom - nr->top));
                SetDlgItemTextW(hw, IDC_REC_CUSTOM_H, buf);
            }
            build_info(ctx);
            InvalidateRect(hw, NULL, TRUE);
        }
        return 0;
    }

    case WM_CLOSE:
        if (ctx->state == RS_STARTING || ctx->state == RS_RECORDING ||
            ctx->state == RS_PAUSED) {
            begin_async_stop(hw, ctx);
        } else if (ctx->state != RS_STOPPED) {
            DestroyWindow(hw);
        }
        /* RS_STOPPED = async stop in progress — ignore close */
        return 0;

    case WM_REC_STOP_DONE:
        DestroyWindow(hw);
        return 0;

    case WM_DPICHANGED: {
        ctx->theme.hover_btn = NULL;
        const RECT *rc = (const RECT *)lp;
        SetWindowPos(hw, NULL, rc->left, rc->top,
                     rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);

        EnumChildWindows(hw, destroy_children_cb, 0);
        ctx->theme.dpi = HIWORD(wp);
        ctx->thumb_w = dpi_scale(64, ctx->theme.dpi);
        ctx->thumb_h = dpi_scale(36, ctx->theme.dpi);
        free_thumbnails(ctx);
        capture_thumbnails(ctx);
        theme_create_fonts(&ctx->theme);
        build_ui(hw, (HINSTANCE)GetWindowLongPtrW(hw, GWLP_HINSTANCE),
                 ctx);
        InvalidateRect(hw, NULL, TRUE);
        return 0;
    }

    case WM_DESTROY:
        rectview_dismiss();
        UnregisterHotKey(hw, HOTKEY_REC_TOGGLE);
        UnregisterHotKey(hw, HOTKEY_REC_PAUSE);
        KillTimer(hw, REC_TICK_TIMER);
        /* rec ownership is transferred to the stop thread;
           if still set here it means an abnormal path. */
        if (ctx->rec) {
            recorder_stop(ctx->rec);
            recorder_destroy(ctx->rec);
            ctx->rec = NULL;
        }
        free_thumbnails(ctx);
        theme_destroy(&ctx->theme);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hw, msg, wp, lp);
}

/* ── Public API ──────────────────────────────────────────────── */

int recctl_run(const RecCtlParams *params)
{
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = recctl_proc;
    wc.hInstance      = params->hi;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconW(params->hi, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm       = LoadIconW(params->hi, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hbrBackground = NULL;
    wc.lpszClassName = RECCTL_CLASS;
    RegisterClassExW(&wc);

    RecCtlCtx ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    ctx.params    = params;
    ctx.state     = RS_IDLE;
    ctx.blink_on  = TRUE;
    ctx.mouse_cap = !params->no_mouse;
    ctx.sel_mode  = params->screen_index;
    ctx.cur_rect  = params->capture_rect;
    ctx.thumb_w   = 64;
    ctx.thumb_h   = 36;
    capture_thumbnails(&ctx);

    CursorWindowPos wp = center_on_cursor(440, 200);

    /* center_on_cursor treats base_h as total window size, but we need
       200 as *client* height.  Adjust for the title bar / borders. */
    {
        DWORD style   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU
                        | WS_MINIMIZEBOX;
        int client_h  = MulDiv(200, (int)wp.dpi, 96);
        RECT adj      = {0, 0, wp.w, client_h};
        AdjustWindowRectEx(&adj, style, FALSE, WS_EX_TOPMOST);
        int real_h = adj.bottom - adj.top;
        wp.y -= (real_h - wp.h) / 2;   /* re-center vertically */
        wp.h  = real_h;
    }

    HWND hw = CreateWindowExW(
        WS_EX_TOPMOST,
        RECCTL_CLASS, L"DemoMediaPlayer \u2014 Record",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        wp.x, wp.y, wp.w, wp.h,
        NULL, NULL, params->hi, &ctx);

    if (!hw) return 1;

    theme_apply_dark_mode(hw);
    ShowWindow(hw, SW_SHOW);
    UpdateWindow(hw);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        if (IsDialogMessageW(hw, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    return 0;
}
