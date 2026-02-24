/*
 * identify.c – "Identify Screens" overlay
 */
#include "identify.h"
#include "constants.h"

/* ── Module state ────────────────────────────────────────────── */

static HWND s_id_wnd[DMP_MAX_MONITORS];
static int  s_id_count      = 0;
static BOOL s_id_registered = FALSE;
static HWND s_setup_combo   = NULL;   /* combo to update on click */
static int  s_nmons         = 0;

/* ── Dismiss all overlays ────────────────────────────────────── */

void identify_dismiss(void)
{
    for (int i = 0; i < s_id_count; i++) {
        if (s_id_wnd[i]) {
            DestroyWindow(s_id_wnd[i]);
            s_id_wnd[i] = NULL;
        }
    }
    s_id_count = 0;
}

/* ── Window procedure ────────────────────────────────────────── */

static LRESULT CALLBACK identify_proc(HWND hw, UINT msg,
                                       WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hw, &ps);

        RECT rc;
        GetClientRect(hw, &rc);

        /* Dark background */
        HBRUSH bg = CreateSolidBrush(RGB(32, 32, 32));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        /* Monitor number stored in GWLP_USERDATA */
        int num = (int)(intptr_t)GetWindowLongPtrW(hw, GWLP_USERDATA);
        wchar_t txt[8];
        wsprintfW(txt, L"%d", num);

        /* Big bold font – half the screen height */
        int fh = (rc.bottom - rc.top) / 2;
        if (fh < 100) fh = 100;
        HFONT big = CreateFontW(
            -fh, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
        HFONT oldf = (HFONT)SelectObject(hdc, big);

        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, txt, -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, oldf);
        DeleteObject(big);

        EndPaint(hw, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wp == IDENTIFY_TIMER) {
            KillTimer(hw, IDENTIFY_TIMER);
            identify_dismiss();
            return 0;
        }
        break;

    /* Click to select this screen and dismiss */
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        int idx = (int)(intptr_t)GetWindowLongPtrW(hw, GWLP_USERDATA) - 1;
        if (s_setup_combo && idx >= 0 && idx < s_nmons)
            SendMessageW(s_setup_combo, CB_SETCURSEL, (WPARAM)idx, 0);
        identify_dismiss();
        return 0;
    }
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

/* ── Show overlays ───────────────────────────────────────────── */

void identify_show(HINSTANCE hi,
                    const MonInfo *monitors, int nmons,
                    HWND setup_combo)
{
    identify_dismiss();

    s_setup_combo = setup_combo;
    s_nmons       = nmons;

    if (!s_id_registered) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = identify_proc;
        wc.hInstance      = hi;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = IDENTIFY_CLASS;
        RegisterClassExW(&wc);
        s_id_registered = TRUE;
    }

    for (int i = 0; i < nmons; i++) {
        RECT r = monitors[i].rect;
        HWND hw = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
            IDENTIFY_CLASS, L"",
            WS_POPUP | WS_VISIBLE,
            r.left, r.top,
            r.right  - r.left,
            r.bottom - r.top,
            NULL, NULL, hi, NULL);
        if (hw) {
            SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)(i + 1));
            SetLayeredWindowAttributes(hw, 0, 220, LWA_ALPHA);
            InvalidateRect(hw, NULL, TRUE);
            s_id_wnd[i] = hw;
        }
    }
    s_id_count = nmons;

    /* Auto-dismiss after timeout via timer on the first window */
    if (s_id_count > 0 && s_id_wnd[0])
        SetTimer(s_id_wnd[0], IDENTIFY_TIMER, IDENTIFY_TIMEOUT, NULL);
}
