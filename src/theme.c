/*
 * theme.c – Dark-mode theming, DWM setup, and owner-draw painting
 */
#include "theme.h"
#include "constants.h"
#include "monitors.h"   /* dpi_scale, dpi_create_font */

#include <dwmapi.h>
#include <uxtheme.h>

/* ── DPI convenience ─────────────────────────────────────────── */

int theme_dpi_scale(const ThemeCtx *tc, int value)
{
    return dpi_scale(value, tc->dpi);
}

/* ── Font creation ───────────────────────────────────────────── */

void theme_create_fonts(ThemeCtx *tc)
{
    if (tc->title_font) { DeleteObject(tc->title_font); tc->title_font = NULL; }
    if (tc->ui_font)    { DeleteObject(tc->ui_font);    tc->ui_font = NULL; }
    if (tc->label_font) { DeleteObject(tc->label_font); tc->label_font = NULL; }

    tc->title_font = CreateFontW(
        -dpi_scale(22, tc->dpi), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI Variable Display");

    tc->ui_font = CreateFontW(
        -dpi_scale(14, tc->dpi), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");

    tc->label_font = CreateFontW(
        -dpi_scale(12, tc->dpi), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
}

/* ── Brush creation ──────────────────────────────────────────── */

void theme_create_brushes(ThemeCtx *tc)
{
    if (!tc->br_bg)    tc->br_bg    = CreateSolidBrush(CLR_BG);
    if (!tc->br_input) tc->br_input = CreateSolidBrush(CLR_INPUT_BG);
}

/* ── Cleanup ─────────────────────────────────────────────────── */

void theme_destroy(ThemeCtx *tc)
{
    if (tc->br_bg)      { DeleteObject(tc->br_bg);      tc->br_bg = NULL; }
    if (tc->br_input)   { DeleteObject(tc->br_input);   tc->br_input = NULL; }
    if (tc->title_font) { DeleteObject(tc->title_font); tc->title_font = NULL; }
    if (tc->ui_font)    { DeleteObject(tc->ui_font);    tc->ui_font = NULL; }
    if (tc->label_font) { DeleteObject(tc->label_font); tc->label_font = NULL; }
    tc->hover_btn = NULL;
}

/* ── DWM dark-mode ───────────────────────────────────────────── */

void theme_apply_dark_mode(HWND hwnd)
{
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &dark, sizeof(dark));

    DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner, sizeof(corner));

    /* Match border & caption colours to our dark background. */
    COLORREF border_clr = CLR_BG;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR,
                          &border_clr, sizeof(border_clr));
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR,
                          &border_clr, sizeof(border_clr));
}

/* ── Button hover subclass ───────────────────────────────────── */

/* We store the ThemeCtx pointer in GWLP_USERDATA on each button so
   the subclass proc can reach the shared hover_btn field. */

static LRESULT CALLBACK btn_hover_proc(HWND hw, UINT msg,
                                        WPARAM wp, LPARAM lp)
{
    ThemeCtx *tc = (ThemeCtx *)(LONG_PTR)GetWindowLongPtrW(hw, GWLP_USERDATA);
    if (!tc) return DefWindowProcW(hw, msg, wp, lp);

    switch (msg) {
    case WM_MOUSEMOVE:
        if (tc->hover_btn != hw) {
            tc->hover_btn = hw;
            InvalidateRect(hw, NULL, FALSE);
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hw, 0};
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        if (tc->hover_btn == hw) {
            tc->hover_btn = NULL;
            InvalidateRect(hw, NULL, FALSE);
        }
        break;
    }
    return CallWindowProcW(tc->orig_btn_proc, hw, msg, wp, lp);
}

void theme_subclass_button(ThemeCtx *tc, HWND btn)
{
    SetWindowLongPtrW(btn, GWLP_USERDATA, (LONG_PTR)tc);
    WNDPROC old = (WNDPROC)SetWindowLongPtrW(
        btn, GWLP_WNDPROC, (LONG_PTR)btn_hover_proc);
    if (!tc->orig_btn_proc) tc->orig_btn_proc = old;
}

/* ── Owner-draw painting ─────────────────────────────────────── */

void theme_draw_button(const ThemeCtx *tc,
                        DRAWITEMSTRUCT *di)
{
    BOOL hover   = (tc->hover_btn == di->hwndItem);
    BOOL pressed = (di->itemState & ODS_SELECTED);
    COLORREF bg, border;

    if (di->CtlID == IDC_PLAY) {
        bg = pressed ? CLR_ACCENT_PRESS
           : hover   ? CLR_ACCENT_HOV
           :           CLR_ACCENT;
        border = bg;
    } else {
        bg = pressed ? CLR_BTN_SEC_PRESS
           : hover   ? CLR_BTN_SEC_HOV
           :           CLR_BTN_SEC;
        border = CLR_BORDER;
    }

    int s = theme_dpi_scale(tc, 8);
    int s3 = theme_dpi_scale(tc, 3);
    int s6 = theme_dpi_scale(tc, 6);

    /* Fill corners with parent background so the rounded
       rect blends cleanly. */
    FillRect(di->hDC, &di->rcItem, tc->br_bg);

    /* Rounded rectangle */
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

    /* Button label */
    SetTextColor(di->hDC, CLR_TEXT);
    SetBkMode(di->hDC, TRANSPARENT);
    SelectObject(di->hDC, tc->ui_font);
    wchar_t txt[64];
    GetWindowTextW(di->hwndItem, txt, 64);
    DrawTextW(di->hDC, txt, -1, &di->rcItem,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* Keyboard-focus indicator */
    if (di->itemState & ODS_FOCUS) {
        RECT fr = di->rcItem;
        InflateRect(&fr, -s3, -s3);
        HPEN fp    = CreatePen(PS_DOT, 1, RGB(200, 200, 200));
        HPEN ofp   = (HPEN)SelectObject(di->hDC, fp);
        HBRUSH nul = (HBRUSH)GetStockObject(NULL_BRUSH);
        HBRUSH on  = (HBRUSH)SelectObject(di->hDC, nul);
        RoundRect(di->hDC, fr.left, fr.top,
                  fr.right, fr.bottom, s6, s6);
        SelectObject(di->hDC, ofp);
        SelectObject(di->hDC, on);
        DeleteObject(fp);
    }
}

void theme_draw_combo_item(const ThemeCtx *tc,
                            DRAWITEMSTRUCT *di)
{
    BOOL selected = (di->itemState & ODS_SELECTED);
    COLORREF bg = selected ? CLR_ACCENT : CLR_INPUT_BG;
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(di->hDC, &di->rcItem, br);
    DeleteObject(br);

    if (di->itemID != (UINT)-1) {
        wchar_t buf[256];
        SendMessageW(di->hwndItem, CB_GETLBTEXT,
                     di->itemID, (LPARAM)buf);
        SetTextColor(di->hDC, CLR_TEXT);
        SetBkMode(di->hDC, TRANSPARENT);
        SelectObject(di->hDC, tc->ui_font);
        RECT tr = di->rcItem;
        tr.left += theme_dpi_scale(tc, 10);
        DrawTextW(di->hDC, buf, -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

/* ── Colour message handlers ─────────────────────────────────── */

LRESULT theme_handle_ctlcolorstatic(const ThemeCtx *tc,
                                     HDC hdc, HWND ctrl)
{
    int id = GetDlgCtrlID(ctrl);

    if (id == IDC_FILE_EDIT) {
        /* Read-only edit → dark input background */
        SetTextColor(hdc, CLR_TEXT);
        SetBkColor(hdc, CLR_INPUT_BG);
        return (LRESULT)tc->br_input;
    }

    SetBkMode(hdc, TRANSPARENT);
    if (id == IDC_TITLE_LABEL || id == IDC_MUTED)
        SetTextColor(hdc, CLR_TEXT);
    else
        SetTextColor(hdc, CLR_TEXT_DIM);
    return (LRESULT)tc->br_bg;
}

LRESULT theme_handle_ctlcolorlistbox(const ThemeCtx *tc, HDC hdc)
{
    SetTextColor(hdc, CLR_TEXT);
    SetBkColor(hdc, CLR_INPUT_BG);
    return (LRESULT)tc->br_input;
}
