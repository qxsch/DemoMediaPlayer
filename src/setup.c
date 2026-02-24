/*
 * setup.c – Interactive setup dialog
 */
#include "setup.h"
#include "constants.h"
#include "resource.h"
#include "monitors.h"
#include "theme.h"
#include "identify.h"
#include "util.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <string.h>

/* ── Per-dialog context (stored in GWLP_USERDATA) ────────────── */

typedef struct {
    /* Shared inputs */
    const MonInfo *monitors;
    int            nmons;

    /* Output / current selection */
    SetupResult   *result;

    /* Theme resources */
    ThemeCtx       theme;
} SetupCtx;

/* ── DPI convenience shim ────────────────────────────────────── */

static int sdpi(const SetupCtx *ctx, int v)
{
    return theme_dpi_scale(&ctx->theme, v);
}

/* ── Build all child controls ────────────────────────────────── */

static BOOL CALLBACK destroy_children_cb(HWND child, LPARAM lp)
{
    (void)lp;
    DestroyWindow(child);
    return TRUE;
}

static void build_ui(HWND hw, HINSTANCE hi, SetupCtx *ctx)
{
    RECT cr;
    GetClientRect(hw, &cr);
    int cw  = cr.right;
    int mx  = sdpi(ctx, 32);
    int ew  = cw - 2 * mx;
    int gap = sdpi(ctx, 12);
    HWND c;
    int y = sdpi(ctx, 24);

    /* ── App title ─────────────────────────────────────────── */
    c = CreateWindowExW(0, L"STATIC", APP_TITLE,
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            mx, y, ew, sdpi(ctx, 28),
            hw, (HMENU)(intptr_t)IDC_TITLE_LABEL, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)ctx->theme.title_font, TRUE);
    y += sdpi(ctx, 48);

    /* ── Video File section ────────────────────────────────── */
    c = CreateWindowExW(0, L"STATIC", L"VIDEO FILE",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            mx, y, ew, sdpi(ctx, 16), hw, NULL, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)ctx->theme.label_font, TRUE);
    y += sdpi(ctx, 24);

    int browse_w = sdpi(ctx, 100);
    int edit_w   = ew - browse_w - gap;

    c = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
            mx, y, edit_w, sdpi(ctx, 34),
            hw, (HMENU)(intptr_t)IDC_FILE_EDIT, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)ctx->theme.ui_font, TRUE);
    SendMessageW(c, EM_SETMARGINS,
                 EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(sdpi(ctx, 10), sdpi(ctx, 10)));

    c = CreateWindowExW(0, L"BUTTON", L"Browse\u2026",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            mx + edit_w + gap, y, browse_w, sdpi(ctx, 34),
            hw, (HMENU)(intptr_t)IDC_BROWSE, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)ctx->theme.ui_font, TRUE);
    theme_subclass_button(&ctx->theme, c);
    y += sdpi(ctx, 52);

    /* ── Display section ───────────────────────────────────── */
    c = CreateWindowExW(0, L"STATIC", L"DISPLAY",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            mx, y, ew, sdpi(ctx, 16), hw, NULL, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)ctx->theme.label_font, TRUE);
    y += sdpi(ctx, 24);

    c = CreateWindowExW(0, L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE
            | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED
            | CBS_HASSTRINGS | WS_VSCROLL,
            mx, y, ew, sdpi(ctx, 240),
            hw, (HMENU)(intptr_t)IDC_SCREEN_COMBO, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)ctx->theme.ui_font, TRUE);
    for (int i = 0; i < ctx->nmons; i++)
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)ctx->monitors[i].label);
    int sel = ctx->result->screen;
    SendMessageW(c, CB_SETCURSEL,
                 (sel >= 0 && sel < ctx->nmons) ? sel : 0, 0);
    SetWindowTheme(c, L"DarkMode_CFD", NULL);
    y += sdpi(ctx, 50);

    /* ── Muted checkbox ────────────────────────────────────── */
    c = CreateWindowExW(0, L"BUTTON", L"Start muted",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            mx, y, ew, sdpi(ctx, 24),
            hw, (HMENU)(intptr_t)IDC_MUTED, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)ctx->theme.ui_font, TRUE);
    SetWindowTheme(c, L"DarkMode_Explorer", NULL);
    if (ctx->result->muted)
        SendMessageW(c, BM_SETCHECK, BST_CHECKED, 0);
    y += sdpi(ctx, 44);

    /* ── Play + Identify buttons ───────────────────────────── */
    int btn_w = (ew - gap) / 2;

    c = CreateWindowExW(0, L"BUTTON", L"\u25B6  Play",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            mx, y, btn_w, sdpi(ctx, 42),
            hw, (HMENU)(intptr_t)IDC_PLAY, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)ctx->theme.ui_font, TRUE);
    theme_subclass_button(&ctx->theme, c);

    c = CreateWindowExW(0, L"BUTTON", L"Identify Screens",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            mx + btn_w + gap, y, ew - btn_w - gap, sdpi(ctx, 42),
            hw, (HMENU)(intptr_t)IDC_IDENTIFY, hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)ctx->theme.ui_font, TRUE);
    theme_subclass_button(&ctx->theme, c);

    /* Show pre-set file path (from partial CLI args). */
    if (ctx->result->file[0])
        SetDlgItemTextW(hw, IDC_FILE_EDIT, ctx->result->file);
}

/* ── Window procedure ────────────────────────────────────────── */

static LRESULT CALLBACK setup_proc(HWND hw, UINT msg,
                                    WPARAM wp, LPARAM lp)
{
    SetupCtx *ctx;

    if (msg == WM_CREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        ctx = (SetupCtx *)cs->lpCreateParams;
        SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)ctx);

        /* Initialise theme resources */
        ctx->theme.dpi = dpi_for_window(hw);
        theme_create_brushes(&ctx->theme);
        theme_create_fonts(&ctx->theme);
        build_ui(hw, cs->hInstance, ctx);
        return 0;
    }

    ctx = (SetupCtx *)(LONG_PTR)GetWindowLongPtrW(hw, GWLP_USERDATA);
    if (!ctx) return DefWindowProcW(hw, msg, wp, lp);

    switch (msg) {

    /* Handle IsDialogMessage's DM_GETDEFID so Enter activates Play. */
    case DM_GETDEFID:
        return MAKELRESULT(IDC_PLAY, DC_HASDEFID);

    /* ── Dark background ───────────────────────────────────────── */
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hw, &rc);
        FillRect(hdc, &rc, ctx->theme.br_bg);

        /* Subtle separator below the title */
        HPEN sep = CreatePen(PS_SOLID, 1, CLR_SEPARATOR);
        HPEN old = (HPEN)SelectObject(hdc, sep);
        MoveToEx(hdc, sdpi(ctx, 32), sdpi(ctx, 62), NULL);
        LineTo(hdc, rc.right - sdpi(ctx, 32), sdpi(ctx, 62));
        SelectObject(hdc, old);
        DeleteObject(sep);
        return 1;
    }

    /* ── Owner-draw items ──────────────────────────────────────── */
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;

        if (di->CtlType == ODT_COMBOBOX &&
            di->CtlID == IDC_SCREEN_COMBO) {
            theme_draw_combo_item(&ctx->theme, di);
            return TRUE;
        }
        if (di->CtlType == ODT_BUTTON) {
            theme_draw_button(&ctx->theme, di);
            return TRUE;
        }
        break;
    }

    /* Set combo-box item height */
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lp;
        if (mi->CtlType == ODT_COMBOBOX) {
            mi->itemHeight = sdpi(ctx, 32);
            return TRUE;
        }
        break;
    }

    /* ── Dark colour for labels, read-only edit, checkbox ──────── */
    case WM_CTLCOLORSTATIC:
        return theme_handle_ctlcolorstatic(&ctx->theme,
                                            (HDC)wp, (HWND)lp);

    case WM_CTLCOLORLISTBOX:
        return theme_handle_ctlcolorlistbox(&ctx->theme, (HDC)wp);

    /* ── Commands ──────────────────────────────────────────────── */
    case WM_COMMAND:
        switch (LOWORD(wp)) {

        case IDC_BROWSE: {
            wchar_t tmp[MAX_PATH_BUF] = {0};
            if (browse_file(hw, tmp, MAX_PATH_BUF)) {
                wcscpy(ctx->result->file, tmp);
                SetDlgItemTextW(hw, IDC_FILE_EDIT, tmp);
            }
            return 0;
        }

        case IDC_PLAY:
            /* If no file selected yet, auto-open the browser. */
            if (!ctx->result->file[0]) {
                wchar_t tmp[MAX_PATH_BUF] = {0};
                if (browse_file(hw, tmp, MAX_PATH_BUF)) {
                    wcscpy(ctx->result->file, tmp);
                    SetDlgItemTextW(hw, IDC_FILE_EDIT, tmp);
                } else {
                    return 0;   /* user cancelled browse */
                }
            }
            ctx->result->screen = (int)SendDlgItemMessageW(
                hw, IDC_SCREEN_COMBO, CB_GETCURSEL, 0, 0);
            if (ctx->result->screen < 0) ctx->result->screen = 0;
            ctx->result->muted = (SendDlgItemMessageW(
                hw, IDC_MUTED, BM_GETCHECK, 0, 0) == BST_CHECKED);
            ctx->result->confirmed = TRUE;
            DestroyWindow(hw);
            return 0;

        case IDC_IDENTIFY:
            identify_show(
                (HINSTANCE)GetWindowLongPtrW(hw, GWLP_HINSTANCE),
                ctx->monitors, ctx->nmons,
                GetDlgItem(hw, IDC_SCREEN_COMBO));
            return 0;

        case IDCANCEL:              /* ESC via IsDialogMessage */
            ctx->result->confirmed = FALSE;
            DestroyWindow(hw);
            return 0;
        }
        break;

    case WM_CLOSE:
        ctx->result->confirmed = FALSE;
        DestroyWindow(hw);
        return 0;

    case WM_DPICHANGED: {
        /* Save current UI state */
        int sel = (int)SendDlgItemMessageW(hw, IDC_SCREEN_COMBO,
                                           CB_GETCURSEL, 0, 0);
        BOOL muted = (SendDlgItemMessageW(hw, IDC_MUTED,
                                          BM_GETCHECK, 0, 0)
                      == BST_CHECKED);
        ctx->theme.hover_btn = NULL;

        /* Resize window FIRST */
        const RECT *rc = (const RECT *)lp;
        SetWindowPos(hw, NULL, rc->left, rc->top,
                     rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);

        /* Destroy all child controls and recreate at new DPI */
        EnumChildWindows(hw, destroy_children_cb, 0);
        ctx->theme.dpi = HIWORD(wp);
        theme_create_fonts(&ctx->theme);
        build_ui(hw, (HINSTANCE)GetWindowLongPtrW(hw, GWLP_HINSTANCE),
                 ctx);

        /* Restore state */
        SendDlgItemMessageW(hw, IDC_SCREEN_COMBO, CB_SETCURSEL,
                            (sel >= 0 ? sel : 0), 0);
        if (muted)
            SendDlgItemMessageW(hw, IDC_MUTED, BM_SETCHECK,
                                BST_CHECKED, 0);

        InvalidateRect(hw, NULL, TRUE);
        return 0;
    }

    case WM_DESTROY:
        identify_dismiss();
        theme_destroy(&ctx->theme);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

/* ── Public API ──────────────────────────────────────────────── */

BOOL setup_run(HINSTANCE hi,
               const MonInfo *monitors, int nmons,
               const wchar_t *initial_file,
               int initial_screen,
               BOOL initial_muted,
               SetupResult *result)
{
    /* Enable modern visual styles (ComCtl32 v6). */
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = setup_proc;
    wc.hInstance      = hi;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconW(hi, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm       = LoadIconW(hi, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hbrBackground = NULL;   /* we paint the background ourselves */
    wc.lpszClassName = SETUP_CLASS;
    RegisterClassExW(&wc);

    /* Initialise result / context */
    ZeroMemory(result, sizeof(*result));
    if (initial_file && initial_file[0])
        wcscpy(result->file, initial_file);
    result->screen = initial_screen;
    result->muted  = initial_muted;

    SetupCtx ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    ctx.monitors = monitors;
    ctx.nmons    = nmons;
    ctx.result   = result;

    /* Place the dialog on whichever monitor the mouse cursor is on. */
    POINT cur;
    GetCursorPos(&cur);
    HMONITOR hcur = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);

    int dw = 560, dh = 410;
    UINT monDpi = dpi_for_monitor(hcur);
    dw = MulDiv(dw, (int)monDpi, 96);
    dh = MulDiv(dh, (int)monDpi, 96);

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hcur, &mi);
    int mx = mi.rcWork.left;
    int my = mi.rcWork.top;
    int mw = mi.rcWork.right  - mi.rcWork.left;
    int mh = mi.rcWork.bottom - mi.rcWork.top;

    HWND dlg = CreateWindowExW(
        0, SETUP_CLASS, APP_TITLE,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        mx + (mw - dw) / 2, my + (mh - dh) / 2, dw, dh,
        NULL, NULL, hi, &ctx);

    if (!dlg) return FALSE;

    /* Apply DWM dark-mode before showing. */
    theme_apply_dark_mode(dlg);

    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        if (IsDialogMessageW(dlg, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    return result->confirmed;
}
