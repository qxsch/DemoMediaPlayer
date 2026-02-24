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
#include "resource.h"
#include "theme.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    const RecCtlParams *p = ctx->params;
    int w = (int)(p->capture_rect.right  - p->capture_rect.left);
    int h = (int)(p->capture_rect.bottom - p->capture_rect.top);

    swprintf(ctx->info_line, 512,
             L"Screen %d  \u00B7  %d\u00D7%d  \u00B7  %d fps  \u00B7  H.265 CRF %d",
             p->screen_index + 1, w, h, p->fps, REC_DEFAULT_CRF);
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
    int btn_h = sdpi(ctx, 38);
    HWND c;

    /* ── Start/Stop button ────────────────────────────────────── */
    int y = sdpi(ctx, 60);
    int btn_w = (ew - gap) / 2;

    const wchar_t *start_label =
        (ctx->state == RS_IDLE) ? L"\u25CF  Record" : L"\u25A0  Stop";
    BOOL ss_disabled = (ctx->state == RS_STARTING);  /* can't stop during init */

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
               ? 0 : WS_DISABLED),  /* disabled in IDLE/STARTING/STOPPED */
            mx + btn_w + gap, y, ew - btn_w - gap, btn_h,
            hw, (HMENU)(intptr_t)IDC_REC_PAUSE, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)ctx->theme.ui_font, TRUE);
    theme_subclass_button(&ctx->theme, c);

    /* ── Mouse capture checkbox ────────────────────────────────── */
    int cb_y = y + btn_h + sdpi(ctx, 8);
    c = CreateWindowExW(0, L"BUTTON", L"Capture mouse cursor",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            mx, cb_y, ew, sdpi(ctx, 24),
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
        build_info(ctx);
        build_ui(hw, cs->hInstance, ctx);

        /* Start blink timer */
        SetTimer(hw, REC_TICK_TIMER, REC_TICK_INTERVAL, NULL);
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

    case WM_TIMER:
        if (wp == REC_TICK_TIMER) {
            ctx->blink_on = !ctx->blink_on;
            /* Invalidate just the indicator area */
            RECT dot_rc;
            dot_rc.left   = sdpi(ctx, 12);
            dot_rc.top    = sdpi(ctx, 8);
            dot_rc.right  = sdpi(ctx, 40);
            dot_rc.bottom = sdpi(ctx, 36);
            InvalidateRect(hw, &dot_rc, FALSE);

            /* Transition from STARTING to RECORDING once pipeline is active */
            if (ctx->state == RS_STARTING && ctx->rec &&
                recorder_active(ctx->rec)) {
                ctx->state = RS_RECORDING;
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
        }
        return 0;

    /* ── Owner-draw buttons ────────────────────────────────────── */
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        if (di->CtlType == ODT_BUTTON) {
            /* Use accent colour for the Record/Stop button */
            BOOL is_primary = (di->CtlID == IDC_REC_STARTSTOP);
            BOOL hover   = (ctx->theme.hover_btn == di->hwndItem);
            BOOL pressed = (di->itemState & ODS_SELECTED);
            BOOL disabled = (di->itemState & ODS_DISABLED);

            COLORREF bg, border;
            if (disabled) {
                bg = CLR_BTN_SEC_PRESS;
                border = CLR_BORDER;
            } else if (is_primary) {
                /* Red for stop, accent for record */
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

            HBRUSH br  = CreateSolidBrush(bg);
            HPEN   pen = CreatePen(PS_SOLID, 1, border);
            HBRUSH obr = (HBRUSH)SelectObject(di->hDC, br);
            HPEN   opn = (HPEN)SelectObject(di->hDC, pen);
            RoundRect(di->hDC,
                      di->rcItem.left, di->rcItem.top,
                      di->rcItem.right, di->rcItem.bottom,
                      s, s);
            SelectObject(di->hDC, obr);
            SelectObject(di->hDC, opn);
            DeleteObject(br);
            DeleteObject(pen);

            /* Label */
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

    /* ── Dark colour for labels / checkbox ─────────────────────── */
    case WM_CTLCOLORSTATIC:
        return theme_handle_ctlcolorstatic(&ctx->theme,
                                            (HDC)wp, (HWND)lp);

    /* ── Commands ──────────────────────────────────────────────── */
    case WM_COMMAND:
        switch (LOWORD(wp)) {

        case IDC_REC_STARTSTOP:
            if (ctx->state == RS_IDLE) {
                /* Start recording */
                ctx->rec = recorder_create(
                    &ctx->params->capture_rect,
                    ctx->params->output_u8,
                    ctx->params->fps,
                    ctx->params->audio_u8,
                    ctx->mouse_cap);

                if (!ctx->rec && ctx->params->audio_u8) {
                    /* Retry without audio */
                    ctx->rec = recorder_create(
                        &ctx->params->capture_rect,
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
                ctx->state = RS_PAUSED;
            } else if (ctx->state == RS_PAUSED) {
                recorder_resume(ctx->rec);
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
        theme_create_fonts(&ctx->theme);
        build_ui(hw, (HINSTANCE)GetWindowLongPtrW(hw, GWLP_HINSTANCE),
                 ctx);
        InvalidateRect(hw, NULL, TRUE);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hw, REC_TICK_TIMER);
        /* rec ownership is transferred to the stop thread;
           if still set here it means an abnormal path. */
        if (ctx->rec) {
            recorder_stop(ctx->rec);
            recorder_destroy(ctx->rec);
            ctx->rec = NULL;
        }
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
    ctx.params   = params;
    ctx.state    = RS_IDLE;
    ctx.blink_on = TRUE;
    ctx.mouse_cap = !params->no_mouse;

    /* Place window on the monitor the mouse is on. */
    POINT cur;
    GetCursorPos(&cur);
    HMONITOR hcur = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);

    UINT monDpi = dpi_for_monitor(hcur);
    int dw = MulDiv(440, (int)monDpi, 96);
    int dh = MulDiv(195, (int)monDpi, 96);

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hcur, &mi);
    int mx = mi.rcWork.left;
    int my = mi.rcWork.top;
    int mw = mi.rcWork.right  - mi.rcWork.left;
    int mh = mi.rcWork.bottom - mi.rcWork.top;

    HWND hw = CreateWindowExW(
        WS_EX_TOPMOST,
        RECCTL_CLASS, L"DemoMediaPlayer \u2014 Record",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        mx + (mw - dw) / 2, my + (mh - dh) / 2, dw, dh,
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
