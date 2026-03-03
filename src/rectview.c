/*
 * rectview.c – Interactive rectangle overlay
 *
 * Creates a borderless, topmost, semi-transparent popup window that
 * exactly covers the requested screen rectangle.  The user can:
 *   - Drag the interior to move the rectangle
 *   - Drag any edge or corner to resize it
 *   - Press ESC or right-click to dismiss
 *
 * Whenever the rectangle changes, a WM_RECTVIEW_CHANGED message is
 * posted to the notify window (lParam = pointer to a static RECT
 * that is valid only during message processing).
 */
#include "rectview.h"
#include "constants.h"
#include "monitors.h"   /* dpi_for_monitor, dpi_scale */

#include <stdio.h>

/* ── Resize grip size (device px, before DPI) ────────────────── */
#define GRIP_PX  8          /* width of the grab zone at each edge */
#define MIN_CX  32          /* minimum overlay width  (px)         */
#define MIN_CY  32          /* minimum overlay height (px)         */
#define IDLE_TIMEOUT 5000   /* ms – auto-dismiss after inactivity  */
#define IDLE_TIMER   4      /* timer id for inactivity dismiss     */

/* ── Module state ────────────────────────────────────────────── */

static HWND s_rv_wnd        = NULL;
static BOOL s_rv_registered = FALSE;
static RECT s_rv_rect;          /* screen coordinates of the rect  */
static HWND s_rv_notify     = NULL;   /* receives WM_RECTVIEW_CHANGED */

/* ── Reset inactivity timer ───────────────────────────────────── */

static void reset_idle_timer(void)
{
    if (s_rv_wnd) {
        KillTimer(s_rv_wnd, IDLE_TIMER);
        SetTimer(s_rv_wnd, IDLE_TIMER, IDLE_TIMEOUT, NULL);
    }
}

/* ── Helpers ─────────────────────────────────────────────────── */

/* Post current rect to the notify window */
static void notify_changed(void)
{
    if (s_rv_notify && IsWindow(s_rv_notify))
        SendMessageW(s_rv_notify, WM_RECTVIEW_CHANGED, 0,
                     (LPARAM)&s_rv_rect);
}

/* Sync s_rv_rect from actual window position */
static void sync_rect_from_window(HWND hw)
{
    RECT wr;
    GetWindowRect(hw, &wr);
    s_rv_rect = wr;
}

/* ── Dismiss ─────────────────────────────────────────────────── */

void rectview_dismiss(void)
{
    if (s_rv_wnd) {
        KillTimer(s_rv_wnd, IDLE_TIMER);
        DestroyWindow(s_rv_wnd);
        s_rv_wnd = NULL;
    }
}

/* ── Visibility query ────────────────────────────────────────── */

BOOL rectview_visible(void)
{
    return (s_rv_wnd != NULL && IsWindow(s_rv_wnd));
}

/* ── Retrieve current rect ───────────────────────────────────── */

BOOL rectview_get_rect(RECT *out)
{
    if (!s_rv_wnd || !out) return FALSE;
    *out = s_rv_rect;
    return TRUE;
}

/* ── Paint helper (unchanged visual style) ───────────────────── */

static void paint_overlay(HWND hw)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hw, &ps);
    RECT rc;
    GetClientRect(hw, &rc);

    /* Semi-transparent dark fill */
    HBRUSH bg = CreateSolidBrush(RGB(20, 20, 20));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    /* Determine DPI for the monitor this window sits on */
    HMONITOR hmon = MonitorFromWindow(hw, MONITOR_DEFAULTTONEAREST);
    UINT dpi = dpi_for_monitor(hmon);

    /* Bright accent border (3 px scaled) */
    int bw = dpi_scale(3, dpi);
    if (bw < 2) bw = 2;
    HPEN pen = CreatePen(PS_SOLID, bw, CLR_ACCENT);
    HBRUSH nbr = (HBRUSH)GetStockObject(NULL_BRUSH);
    HPEN   opn = (HPEN)SelectObject(hdc, pen);
    HBRUSH obr = (HBRUSH)SelectObject(hdc, nbr);
    Rectangle(hdc, bw / 2, bw / 2,
              rc.right - bw / 2, rc.bottom - bw / 2);
    SelectObject(hdc, opn);
    SelectObject(hdc, obr);
    DeleteObject(pen);

    /* Dimension + position text */
    int rw = (int)(s_rv_rect.right  - s_rv_rect.left);
    int rh = (int)(s_rv_rect.bottom - s_rv_rect.top);
    wchar_t line1[64], line2[64];
    swprintf(line1, 64, L"%d \u00D7 %d", rw, rh);
    swprintf(line2, 64, L"at (%d, %d)",
             (int)s_rv_rect.left, (int)s_rv_rect.top);

    int font_h = dpi_scale(22, dpi);
    HFONT font = CreateFontW(
        -font_h, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    HFONT oldf = (HFONT)SelectObject(hdc, font);

    SetTextColor(hdc, CLR_TEXT);
    SetBkMode(hdc, TRANSPARENT);

    /* Centre the two lines vertically */
    SIZE sz1, sz2;
    GetTextExtentPoint32W(hdc, line1, (int)wcslen(line1), &sz1);
    GetTextExtentPoint32W(hdc, line2, (int)wcslen(line2), &sz2);

    int total_h = sz1.cy + dpi_scale(4, dpi) + sz2.cy;
    int ty = (rc.bottom - total_h) / 2;
    if (ty < bw + 4) ty = bw + 4;

    RECT tr1 = { 0, ty, rc.right, ty + sz1.cy };
    DrawTextW(hdc, line1, -1, &tr1,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    int small_h = dpi_scale(16, dpi);
    HFONT sfont = CreateFontW(
        -small_h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    SelectObject(hdc, sfont);

    SetTextColor(hdc, CLR_TEXT_DIM);
    RECT tr2 = { 0, ty + sz1.cy + dpi_scale(4, dpi),
                  rc.right,
                  ty + sz1.cy + dpi_scale(4, dpi) + sz2.cy };
    DrawTextW(hdc, line2, -1, &tr2,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, oldf);
    DeleteObject(font);
    DeleteObject(sfont);

    /* Corner markers – small L-shaped brackets */
    int cm = dpi_scale(16, dpi);   /* marker length  */
    int ct = dpi_scale(2, dpi);    /* marker thickness */
    HBRUSH mark_br = CreateSolidBrush(CLR_TEXT);

    /* Top-left */
    RECT mtl1 = { bw, bw, bw + cm, bw + ct };
    RECT mtl2 = { bw, bw, bw + ct, bw + cm };
    FillRect(hdc, &mtl1, mark_br);
    FillRect(hdc, &mtl2, mark_br);

    /* Top-right */
    RECT mtr1 = { rc.right - bw - cm, bw, rc.right - bw, bw + ct };
    RECT mtr2 = { rc.right - bw - ct, bw, rc.right - bw, bw + cm };
    FillRect(hdc, &mtr1, mark_br);
    FillRect(hdc, &mtr2, mark_br);

    /* Bottom-left */
    RECT mbl1 = { bw, rc.bottom - bw - ct, bw + cm, rc.bottom - bw };
    RECT mbl2 = { bw, rc.bottom - bw - cm, bw + ct, rc.bottom - bw };
    FillRect(hdc, &mbl1, mark_br);
    FillRect(hdc, &mbl2, mark_br);

    /* Bottom-right */
    RECT mbr1 = { rc.right - bw - cm, rc.bottom - bw - ct,
                  rc.right - bw, rc.bottom - bw };
    RECT mbr2 = { rc.right - bw - ct, rc.bottom - bw - cm,
                  rc.right - bw, rc.bottom - bw };
    FillRect(hdc, &mbr1, mark_br);
    FillRect(hdc, &mbr2, mark_br);

    DeleteObject(mark_br);
    EndPaint(hw, &ps);
}

/* ── Window procedure ────────────────────────────────────────── */

static LRESULT CALLBACK rectview_proc(HWND hw, UINT msg,
                                       WPARAM wp, LPARAM lp)
{
    switch (msg) {

    /* ── Hit-test: edges/corners → resize, interior → move ── */
    case WM_NCHITTEST: {
        RECT wr;
        GetWindowRect(hw, &wr);
        int mx = (int)(short)LOWORD(lp);   /* screen x */
        int my = (int)(short)HIWORD(lp);   /* screen y */
        int g  = GRIP_PX;

        BOOL atLeft   = (mx - wr.left   < g);
        BOOL atRight  = (wr.right  - mx < g);
        BOOL atTop    = (my - wr.top    < g);
        BOOL atBottom = (wr.bottom - my < g);

        if (atTop    && atLeft)  return HTTOPLEFT;
        if (atTop    && atRight) return HTTOPRIGHT;
        if (atBottom && atLeft)  return HTBOTTOMLEFT;
        if (atBottom && atRight) return HTBOTTOMRIGHT;
        if (atLeft)              return HTLEFT;
        if (atRight)             return HTRIGHT;
        if (atTop)               return HTTOP;
        if (atBottom)            return HTBOTTOM;

        return HTCAPTION;        /* interior → drag to move */
    }

    /* ── Enforce minimum size while sizing ───────────────── */
    case WM_SIZING: {
        RECT *pr = (RECT *)lp;
        int cx = pr->right  - pr->left;
        int cy = pr->bottom - pr->top;

        if (cx < MIN_CX) {
            if (wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT ||
                wp == WMSZ_BOTTOMLEFT)
                pr->left = pr->right - MIN_CX;
            else
                pr->right = pr->left + MIN_CX;
        }
        if (cy < MIN_CY) {
            if (wp == WMSZ_TOP || wp == WMSZ_TOPLEFT ||
                wp == WMSZ_TOPRIGHT)
                pr->top = pr->bottom - MIN_CY;
            else
                pr->bottom = pr->top + MIN_CY;
        }
        return TRUE;
    }

    /* ── Track position/size changes → notify caller ─────── */
    case WM_WINDOWPOSCHANGED: {
        const WINDOWPOS *wpos = (const WINDOWPOS *)lp;
        /* Ignore pure z-order / visibility changes */
        if (!(wpos->flags & SWP_NOMOVE) || !(wpos->flags & SWP_NOSIZE)) {
            sync_rect_from_window(hw);
            InvalidateRect(hw, NULL, TRUE);
            notify_changed();
            reset_idle_timer();   /* user is interacting */
        }
        /* Must call DefWindowProc so WM_SIZE/WM_MOVE still fire */
        break;
    }

    /* ── Re-assert topmost after drag / resize ends ──────── */
    case WM_EXITSIZEMOVE:
        SetWindowPos(hw, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;

    case WM_PAINT:
        paint_overlay(hw);
        return 0;

    /* ── Inactivity auto-dismiss ─────────────────────────── */
    case WM_TIMER:
        if (wp == IDLE_TIMER) {
            KillTimer(hw, IDLE_TIMER);
            rectview_dismiss();
            return 0;
        }
        break;

    /* ── Dismiss on right-click, double-click, or Escape ──── */
    case WM_NCLBUTTONDBLCLK:
        rectview_dismiss();
        return 0;

    case WM_RBUTTONDOWN:
    case WM_NCRBUTTONDOWN:
        rectview_dismiss();
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            rectview_dismiss();
            return 0;
        }
        break;

    /* ── Cursor shape depending on hit location ──────────── */
    case WM_SETCURSOR: {
        WORD ht = LOWORD(lp);
        LPCWSTR cur = IDC_ARROW;
        switch (ht) {
            case HTTOP:         case HTBOTTOM:      cur = IDC_SIZENS;   break;
            case HTLEFT:        case HTRIGHT:       cur = IDC_SIZEWE;   break;
            case HTTOPLEFT:     case HTBOTTOMRIGHT: cur = IDC_SIZENWSE; break;
            case HTTOPRIGHT:    case HTBOTTOMLEFT:  cur = IDC_SIZENESW; break;
            case HTCAPTION:                          cur = IDC_SIZEALL;  break;
        }
        SetCursor(LoadCursorW(NULL, cur));
        return TRUE;
    }
    }

    return DefWindowProcW(hw, msg, wp, lp);
}

/* ── Show overlay ────────────────────────────────────────────── */

void rectview_show(HINSTANCE hi, const RECT *rect, HWND notify_hwnd)
{
    rectview_dismiss();
    s_rv_rect   = *rect;
    s_rv_notify = notify_hwnd;

    if (!s_rv_registered) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = rectview_proc;
        wc.hInstance      = hi;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = RECTVIEW_CLASS;
        RegisterClassExW(&wc);
        s_rv_registered = TRUE;
    }

    int w = (int)(rect->right  - rect->left);
    int h = (int)(rect->bottom - rect->top);
    if (w < MIN_CX) w = MIN_CX;
    if (h < MIN_CY) h = MIN_CY;

    s_rv_wnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        RECTVIEW_CLASS, L"",
        WS_POPUP | WS_VISIBLE,
        rect->left, rect->top, w, h,
        NULL, NULL, hi, NULL);

    if (s_rv_wnd) {
        /* 70 % opaque – see through to the desktop content */
        SetLayeredWindowAttributes(s_rv_wnd, 0, 180, LWA_ALPHA);
        InvalidateRect(s_rv_wnd, NULL, TRUE);
        /* Start inactivity timer – dismiss after IDLE_TIMEOUT ms */
        SetTimer(s_rv_wnd, IDLE_TIMER, IDLE_TIMEOUT, NULL);
    }
}
