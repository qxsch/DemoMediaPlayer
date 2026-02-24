/*
 * help.c – Help / usage window
 */
#include "help.h"
#include "constants.h"
#include "resource.h"
#include "monitors.h"   /* dpi_for_window */

/* ── Help text ───────────────────────────────────────────────── */

static const wchar_t s_help_text[] =
    L"DemoMediaPlayer \u2014 Minimal Fullscreen Media Player\r\n"
    L"\r\n"
    L"Usage:\r\n"
    L"  mediaplayer.exe                        Interactive mode (setup dialog)\r\n"
    L"  mediaplayer.exe --file <path>           Play file fullscreen\r\n"
    L"  mediaplayer.exe --file <path> --screen N\r\n"
    L"  mediaplayer.exe <path>                  Bare argument = file path\r\n"
    L"\r\n"
    L"Options:\r\n"
    L"  -f, --file <path>    Path to the media file to play\r\n"
    L"  -s, --screen <N>     Screen number (1-based, default: 1)\r\n"
    L"  -m, --mute           Start playback muted\r\n"
    L"  -p, --position <N>   Start at position N seconds\r\n"
    L"  -h, --help           Show this help message\r\n"
    L"\r\n"
    L"Taskbar options (screen recordings):\r\n"
    L"  --keep-taskbar-visible[=N]   Shrink window by N DPI units\r\n"
    L"                               (default 48) to reveal the\r\n"
    L"                               real taskbar underneath\r\n"
    L"  --crop-video-taskbar[=N]     Crop N pixels (default 48)\r\n"
    L"                               from the bottom of the source\r\n"
    L"                               video to hide the recorded\r\n"
    L"                               taskbar\r\n"
    L"  --fix-taskbar[=N]            Shorthand for both options\r\n"
    L"                               above with the same value\r\n"
    L"\r\n"
    L"Keyboard controls during playback:\r\n"
    L"  ESC          Quit\r\n"
    L"  S            Restart from beginning\r\n"
    L"  P / Space    Toggle pause\r\n"
    L"  R            Seek back 30 seconds\r\n"
    L"  F            Seek forward 30 seconds\r\n"
    L"  Left         Seek back 5 seconds\r\n"
    L"  Right        Seek forward 5 seconds\r\n"
    L"  Up           Speed +10% (max 300%)\r\n"
    L"  Down         Speed -10% (min 50%)\r\n"
    L"  Enter        Reset speed to 100%\r\n"
    L"  M            Toggle mute\r\n"
    L"\r\n"
    L"  4            Pan left\r\n"
    L"  6            Pan right\r\n"
    L"  8            Pan up\r\n"
    L"  2            Pan down\r\n"
    L"  9 / -        Zoom out 10% (min 100%)\r\n"
    L"  3 / +        Zoom in 10% (max 400%)\r\n"
    L"  0            Reset pan and zoom\r\n"
    L"  A            Reset all (zoom, pan, speed)\r\n";

/* ── Font helpers ────────────────────────────────────────────── */

static HFONT help_create_font(UINT dpi)
{
    int h = -MulDiv(HELP_FONT_BASE_PT, (int)dpi, 96);
    return CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       FIXED_PITCH | FF_MODERN, L"Consolas");
}

static void help_apply_font(HWND hwnd, UINT dpi)
{
    HFONT old = (HFONT)(LONG_PTR)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    HFONT fnt = help_create_font(dpi);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)fnt);
    HWND edit = GetWindow(hwnd, GW_CHILD);
    if (edit) SendMessageW(edit, WM_SETFONT, (WPARAM)fnt, TRUE);
    if (old)  DeleteObject(old);
}

/* ── Window procedure ────────────────────────────────────────── */

static LRESULT CALLBACK help_wnd_proc(HWND hwnd, UINT msg,
                                       WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: {
        HWND edit = GetWindow(hwnd, GW_CHILD);
        if (edit) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            MoveWindow(edit, 0, 0, rc.right, rc.bottom, TRUE);
        }
        return 0;
    }
    case WM_DPICHANGED: {
        UINT dpi = HIWORD(wp);
        help_apply_font(hwnd, dpi);
        const RECT *rc = (const RECT *)lp;
        SetWindowPos(hwnd, NULL, rc->left, rc->top,
                     rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY: {
        HFONT fnt = (HFONT)(LONG_PTR)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (fnt) DeleteObject(fnt);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ── Public API ──────────────────────────────────────────────── */

void help_show(void)
{
    static const wchar_t *cls = L"DMP_HelpWnd";
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = help_wnd_proc;
    wc.hInstance      = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm       = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);

    /* Scale window to system DPI and centre on primary monitor. */
    UINT sysDpi = dpi_for_window(NULL);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = MulDiv(HELP_WND_BASE_W, (int)sysDpi, 96);
    int wh = MulDiv(HELP_WND_BASE_H, (int)sysDpi, 96);

    HWND hwnd = CreateWindowExW(
        0, cls, L"DemoMediaPlayer \u2014 Help",
        WS_OVERLAPPEDWINDOW,
        (sw - ww) / 2, (sh - wh) / 2, ww, wh,
        NULL, NULL, wc.hInstance, NULL);

    /* Create a read-only multiline EDIT with DPI-scaled font. */
    RECT rc;
    GetClientRect(hwnd, &rc);
    HWND edit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", s_help_text,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL
        | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | ES_LEFT,
        0, 0, rc.right, rc.bottom,
        hwnd, NULL, wc.hInstance, NULL);

    UINT wndDpi = dpi_for_window(hwnd);
    HFONT mono  = help_create_font(wndDpi);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)mono);
    SendMessageW(edit, WM_SETFONT, (WPARAM)mono, TRUE);

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    /* Run a small message loop just for this window. */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
