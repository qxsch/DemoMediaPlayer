/*
 * DemoMediaPlayer – Minimal Fullscreen Media Player for Windows
 *
 * Built with C, libmpv, and the Win32 API.
 *
 * Interactive:  mediaplayer.exe
 * CLI:          mediaplayer.exe --file "video.mp4" [--screen 0]
 *
 * Keyboard controls while playing:
 *   ESC       – Quit
 *   S         – Start from beginning
 *   P / Space – Toggle pause
 *   R         – Seek back  30 s
 *   F         – Seek forward 30 s
 */

#include <windows.h>
#include <commdlg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <mpv/client.h>

/* ================================================================== */
/*  Constants                                                          */
/* ================================================================== */

#define APP_TITLE        L"DemoMediaPlayer"
#define PLAYER_CLASS     L"DMP_Player"
#define SETUP_CLASS      L"DMP_Setup"

#define WM_MPV_WAKEUP    (WM_USER + 1)

#define DMP_MAX_MONITORS 16
#define MAX_PATH_BUF     4096

/* Setup-dialog control IDs */
#define IDC_FILE_EDIT    201
#define IDC_BROWSE       202
#define IDC_SCREEN_COMBO 203
#define IDC_PLAY         204
#define IDC_MUTED        205
#define IDC_IDENTIFY     206

/* Icon resource ID (must match app.rc) */
#define IDI_APPICON      1

/* ================================================================== */
/*  Types                                                              */
/* ================================================================== */

typedef struct {
    HMONITOR hmon;
    RECT     rect;
    wchar_t  label[128];
} MonInfo;

/* ================================================================== */
/*  Globals                                                            */
/* ================================================================== */

static MonInfo     g_mons[DMP_MAX_MONITORS];
static int         g_nmons      = 0;

static mpv_handle *g_mpv        = NULL;
static HWND        g_player     = NULL;

/* Setup-dialog results */
static wchar_t     g_sel_file[MAX_PATH_BUF];
static int         g_sel_screen = 0;
static BOOL        g_sel_muted  = FALSE;
static BOOL        g_sel_ok     = FALSE;
static HFONT       g_ui_font    = NULL;

/* ================================================================== */
/*  Monitor enumeration                                                */
/* ================================================================== */

static BOOL CALLBACK enum_mon_cb(HMONITOR hmon, HDC hdc,
                                  LPRECT rc, LPARAM lp)
{
    (void)hdc; (void)lp;
    if (g_nmons >= DMP_MAX_MONITORS) return FALSE;

    MonInfo *m = &g_mons[g_nmons];
    m->hmon = hmon;
    m->rect = *rc;
    wsprintfW(m->label,
              L"Screen %d  (%d \u00D7 %d  at %d, %d)",
              g_nmons + 1,
              rc->right  - rc->left,
              rc->bottom - rc->top,
              rc->left, rc->top);
    g_nmons++;
    return TRUE;
}

static void enum_monitors(void)
{
    g_nmons = 0;
    EnumDisplayMonitors(NULL, NULL, enum_mon_cb, 0);
}

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */

/* Convert a wide string to a heap-allocated UTF-8 string. */
static char *to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *buf = (char *)malloc(n);
    if (buf) WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, n, NULL, NULL);
    return buf;
}

/* Show a standard "Open File" dialog for media files. */
static BOOL browse_file(HWND owner, wchar_t *buf, int buflen)
{
    buf[0] = L'\0';
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter  = L"Media Files\0"
                       L"*.mp4;*.mkv;*.avi;*.mov;*.wmv;*.flv;*.webm;"
                       L"*.m4v;*.mpg;*.mpeg;*.ts;*.vob;"
                       L"*.mp3;*.flac;*.wav;*.ogg;*.aac;*.m4a\0"
                       L"All Files\0*.*\0";
    ofn.lpstrFile    = buf;
    ofn.nMaxFile     = buflen;
    ofn.lpstrTitle   = L"Select Media File";
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameW(&ofn);
}

/* ================================================================== */
/*  mpv wakeup  ->  Win32 message                                     */
/* ================================================================== */

static void mpv_wakeup_cb(void *ctx)
{
    PostMessageW((HWND)ctx, WM_MPV_WAKEUP, 0, 0);
}

/* ================================================================== */
/*  Player window                                                      */
/* ================================================================== */

static LRESULT CALLBACK player_proc(HWND hw, UINT msg,
                                     WPARAM wp, LPARAM lp)
{
    switch (msg) {

    /* ── mpv event pump ────────────────────────────────────────── */
    case WM_MPV_WAKEUP:
        while (g_mpv) {
            mpv_event *e = mpv_wait_event(g_mpv, 0);
            if (e->event_id == MPV_EVENT_NONE)
                break;
            /* We intentionally ignore MPV_EVENT_SHUTDOWN here.
               Only the user (ESC / window close) should exit. */
            if (e->event_id == MPV_EVENT_END_FILE) {
                mpv_event_end_file *ef = (mpv_event_end_file *)e->data;
                if (ef && ef->reason == MPV_END_FILE_REASON_ERROR) {
                    MessageBoxW(hw, L"Failed to play the selected file.",
                                APP_TITLE, MB_ICONERROR);
                    PostQuitMessage(0);
                    return 0;
                }
                /* Normal EOF: force pause so the last frame stays
                   visible.  The user can press S to restart or
                   ESC to quit. */
                {
                    int pause = 1;
                    mpv_set_property(g_mpv, "pause",
                                     MPV_FORMAT_FLAG, &pause);
                }
            }
        }
        return 0;

    /* ── keyboard controls ─────────────────────────────────────── */
    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE:
            PostQuitMessage(0);
            return 0;
        case 'S': {
            /* Unpause (in case we're paused at EOF) and restart. */
            int pause = 0;
            if (g_mpv) mpv_set_property(g_mpv, "pause",
                                         MPV_FORMAT_FLAG, &pause);
            const char *cmd[] = {"seek", "0", "absolute", NULL};
            if (g_mpv) mpv_command_async(g_mpv, 0, cmd);
            return 0;
        }
        case 'P':
        case VK_SPACE: {
            const char *cmd[] = {"cycle", "pause", NULL};
            if (g_mpv) mpv_command_async(g_mpv, 0, cmd);
            return 0;
        }
        case 'R': {
            const char *cmd[] = {"seek", "-30", "relative+exact", NULL};
            if (g_mpv) mpv_command_async(g_mpv, 0, cmd);
            return 0;
        }
        case 'F': {
            const char *cmd[] = {"seek", "30", "relative+exact", NULL};
            if (g_mpv) mpv_command_async(g_mpv, 0, cmd);
            return 0;
        }
        case VK_LEFT: {
            const char *cmd[] = {"seek", "-5", "relative+exact", NULL};
            if (g_mpv) mpv_command_async(g_mpv, 0, cmd);
            return 0;
        }
        case VK_RIGHT: {
            const char *cmd[] = {"seek", "5", "relative+exact", NULL};
            if (g_mpv) mpv_command_async(g_mpv, 0, cmd);
            return 0;
        }
        case 'M': {
            const char *cmd[] = {"cycle", "mute", NULL};
            if (g_mpv) mpv_command_async(g_mpv, 0, cmd);
            return 0;
        }
        }
        break;

    /* ── hide cursor over the video area ───────────────────────── */
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

/* Create a borderless, topmost, fullscreen window on the given
   monitor and bring it to the foreground. */
static HWND create_player(HINSTANCE hi, int scr)
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

    if (scr < 0 || scr >= g_nmons) scr = 0;
    RECT r = g_mons[scr].rect;

    HWND hw = CreateWindowExW(
        WS_EX_TOPMOST,
        PLAYER_CLASS, APP_TITLE,
        WS_POPUP | WS_VISIBLE,
        r.left, r.top,
        r.right  - r.left,
        r.bottom - r.top,
        NULL, NULL, hi, NULL);

    if (hw) {
        SetForegroundWindow(hw);
        SetFocus(hw);
    }
    return hw;
}

/* ================================================================== */
/*  Start playback via libmpv                                          */
/* ================================================================== */

static int play(HWND hw, const char *path, double start_pos)
{
    g_mpv = mpv_create();
    if (!g_mpv) {
        MessageBoxW(hw, L"mpv_create() failed.", APP_TITLE, MB_ICONERROR);
        return -1;
    }

    /* Embed mpv's video output inside our window. */
    int64_t wid = (int64_t)(intptr_t)hw;
    mpv_set_option(g_mpv, "wid", MPV_FORMAT_INT64, &wid);

    /* Disable mpv's own input handling – we do it in WM_KEYDOWN. */
    mpv_set_option_string(g_mpv, "input-default-bindings", "no");
    mpv_set_option_string(g_mpv, "input-vo-keyboard",      "no");

    /* No on-screen controller (clean fullscreen). */
    mpv_set_option_string(g_mpv, "osc", "no");

    /* Minimal OSD: show seek position briefly on seek commands. */
    mpv_set_option_string(g_mpv, "osd-level",    "1");
    mpv_set_option_string(g_mpv, "osd-duration",  "1500");

    /* Use hardware decoding when available. */
    mpv_set_option_string(g_mpv, "hwdec", "auto");

    /* Set start position if requested. */
    if (start_pos > 0.0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.3f", start_pos);
        mpv_set_option_string(g_mpv, "start", buf);
    }

    if (mpv_initialize(g_mpv) < 0) {
        MessageBoxW(hw, L"mpv_initialize() failed.", APP_TITLE, MB_ICONERROR);
        mpv_destroy(g_mpv);
        g_mpv = NULL;
        return -1;
    }

    /* Pause on the last frame instead of closing when the file ends. */
    mpv_set_property_string(g_mpv, "keep-open", "always");

    /* Keep mpv alive even when nothing is playing (safety net). */
    mpv_set_property_string(g_mpv, "idle", "yes");

    /* Integrate mpv's event loop with our Win32 message loop. */
    mpv_set_wakeup_callback(g_mpv, mpv_wakeup_cb, (void *)hw);

    /* Load and start playback. */
    const char *cmd[] = {"loadfile", path, NULL};
    return mpv_command(g_mpv, cmd);
}

/* ================================================================== */
/*  Identify-screens overlay                                           */
/* ================================================================== */

#define IDENTIFY_CLASS   L"DMP_Identify"
#define IDENTIFY_TIMER   1
#define IDENTIFY_TIMEOUT 5000   /* ms */

static HWND g_id_wnd[DMP_MAX_MONITORS];
static int  g_id_count      = 0;
static BOOL g_id_registered = FALSE;
static HWND g_setup_hwnd    = NULL;   /* setup dialog, for combo update */

static void dismiss_identify(void)
{
    for (int i = 0; i < g_id_count; i++) {
        if (g_id_wnd[i]) {
            DestroyWindow(g_id_wnd[i]);
            g_id_wnd[i] = NULL;
        }
    }
    g_id_count = 0;
}

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

        /* Big bold font \u2013 half the screen height */
        int fh = (rc.bottom - rc.top) / 2;
        if (fh < 100) fh = 100;
        HFONT big = CreateFontW(
            -fh, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
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
            dismiss_identify();
            return 0;
        }
        break;

    /* Click to select this screen and dismiss */
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        int idx = (int)(intptr_t)GetWindowLongPtrW(hw, GWLP_USERDATA) - 1;
        if (g_setup_hwnd && idx >= 0 && idx < g_nmons)
            SendDlgItemMessageW(g_setup_hwnd, IDC_SCREEN_COMBO,
                                CB_SETCURSEL, (WPARAM)idx, 0);
        dismiss_identify();
        return 0;
    }
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

static void show_identify(HINSTANCE hi)
{
    dismiss_identify();

    if (!g_id_registered) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = identify_proc;
        wc.hInstance      = hi;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = IDENTIFY_CLASS;
        RegisterClassExW(&wc);
        g_id_registered = TRUE;
    }

    for (int i = 0; i < g_nmons; i++) {
        RECT r = g_mons[i].rect;
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
            g_id_wnd[i] = hw;
        }
    }
    g_id_count = g_nmons;

    /* Auto-dismiss after timeout via timer on the first window */
    if (g_id_count > 0 && g_id_wnd[0])
        SetTimer(g_id_wnd[0], IDENTIFY_TIMER, IDENTIFY_TIMEOUT, NULL);
}

/* ================================================================== */
/*  Setup dialog                                                       */
/* ================================================================== */

static LRESULT CALLBACK setup_proc(HWND hw, UINT msg,
                                    WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE: {
        HINSTANCE hi = ((CREATESTRUCTW *)lp)->hInstance;
        g_setup_hwnd = hw;

        g_ui_font = CreateFontW(
            -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        HWND c;
        int y = 20;

        /* ── Video File label ─────────────────────────────────── */
        c = CreateWindowExW(0, L"STATIC", L"Video File:",
                WS_CHILD | WS_VISIBLE,
                20, y, 390, 18, hw, NULL, hi, NULL);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        y += 22;

        /* ── File path edit + Browse button ───────────────────── */
        c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                20, y, 305, 26,
                hw, (HMENU)(intptr_t)IDC_FILE_EDIT, hi, NULL);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_ui_font, TRUE);

        c = CreateWindowExW(0, L"BUTTON", L"Browse\u2026",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                332, y, 80, 26,
                hw, (HMENU)(intptr_t)IDC_BROWSE, hi, NULL);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        y += 38;

        /* ── Display label ────────────────────────────────────── */
        c = CreateWindowExW(0, L"STATIC", L"Display:",
                WS_CHILD | WS_VISIBLE,
                20, y, 390, 18, hw, NULL, hi, NULL);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        y += 22;

        /* ── Monitor combo box ────────────────────────────────── */
        c = CreateWindowExW(0, L"COMBOBOX", NULL,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                20, y, 392, 200,
                hw, (HMENU)(intptr_t)IDC_SCREEN_COMBO, hi, NULL);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        for (int i = 0; i < g_nmons; i++)
            SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)g_mons[i].label);
        SendMessageW(c, CB_SETCURSEL,
                     (g_sel_screen >= 0 && g_sel_screen < g_nmons)
                         ? g_sel_screen : 0, 0);
        y += 40;

        /* ── Muted checkbox ────────────────────────────────────── */
        c = CreateWindowExW(0, L"BUTTON", L"Start muted",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                20, y, 392, 22,
                hw, (HMENU)(intptr_t)IDC_MUTED, hi, NULL);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        if (g_sel_muted)
            SendMessageW(c, BM_SETCHECK, BST_CHECKED, 0);
        y += 32;

        /* ── Play + Identify buttons ────────────────────────── */
        c = CreateWindowExW(0, L"BUTTON", L"Play",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                50, y, 140, 30,
                hw, (HMENU)(intptr_t)IDC_PLAY, hi, NULL);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_ui_font, TRUE);

        c = CreateWindowExW(0, L"BUTTON", L"Identify Screens",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                210, y, 175, 30,
                hw, (HMENU)(intptr_t)IDC_IDENTIFY, hi, NULL);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_ui_font, TRUE);

        /* If a file was already set (e.g. by command line), show it. */
        if (g_sel_file[0])
            SetDlgItemTextW(hw, IDC_FILE_EDIT, g_sel_file);

        return 0;
    }

    /* Handle IsDialogMessage's DM_GETDEFID so Enter activates Play. */
    case DM_GETDEFID:
        return MAKELRESULT(IDC_PLAY, DC_HASDEFID);

    case WM_COMMAND:
        switch (LOWORD(wp)) {

        case IDC_BROWSE: {
            wchar_t tmp[MAX_PATH_BUF] = {0};
            if (browse_file(hw, tmp, MAX_PATH_BUF)) {
                wcscpy(g_sel_file, tmp);
                SetDlgItemTextW(hw, IDC_FILE_EDIT, tmp);
            }
            return 0;
        }

        case IDC_PLAY:
            /* If no file selected yet, auto-open the browser. */
            if (!g_sel_file[0]) {
                wchar_t tmp[MAX_PATH_BUF] = {0};
                if (browse_file(hw, tmp, MAX_PATH_BUF)) {
                    wcscpy(g_sel_file, tmp);
                    SetDlgItemTextW(hw, IDC_FILE_EDIT, tmp);
                } else {
                    return 0;   /* user cancelled browse */
                }
            }
            g_sel_screen = (int)SendDlgItemMessageW(
                               hw, IDC_SCREEN_COMBO, CB_GETCURSEL, 0, 0);
            if (g_sel_screen < 0) g_sel_screen = 0;
            g_sel_muted = (SendDlgItemMessageW(
                               hw, IDC_MUTED, BM_GETCHECK, 0, 0)
                           == BST_CHECKED);
            g_sel_ok = TRUE;
            DestroyWindow(hw);
            return 0;

        case IDC_IDENTIFY:
            show_identify((HINSTANCE)GetWindowLongPtrW(hw, GWLP_HINSTANCE));
            return 0;

        case IDCANCEL:              /* ESC via IsDialogMessage */
            g_sel_ok = FALSE;
            DestroyWindow(hw);
            return 0;
        }
        break;

    case WM_CLOSE:
        g_sel_ok = FALSE;
        DestroyWindow(hw);
        return 0;

    case WM_DESTROY:
        dismiss_identify();
        g_setup_hwnd = NULL;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

/* Show the setup dialog and run its message loop.
   Returns TRUE if the user confirmed playback. */
static BOOL run_setup(HINSTANCE hi)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = setup_proc;
    wc.hInstance      = hi;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconW(hi, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm       = LoadIconW(hi, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = SETUP_CLASS;
    RegisterClassExW(&wc);

    int dw = 450, dh = 250;

    /* Place the dialog on whichever monitor the mouse cursor is on. */
    POINT cur;
    GetCursorPos(&cur);
    HMONITOR hcur = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hcur, &mi);
    int mx = mi.rcWork.left;
    int my = mi.rcWork.top;
    int mw = mi.rcWork.right  - mi.rcWork.left;
    int mh = mi.rcWork.bottom - mi.rcWork.top;

    HWND dlg = CreateWindowExW(
        0, SETUP_CLASS, APP_TITLE,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        mx + (mw - dw) / 2, my + (mh - dh) / 2, dw, dh,
        NULL, NULL, hi, NULL);

    if (!dlg) return FALSE;

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        /* IsDialogMessage handles Tab, Enter, Escape for us. */
        if (IsDialogMessageW(dlg, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_ui_font) { DeleteObject(g_ui_font); g_ui_font = NULL; }
    return g_sel_ok;
}

/* ================================================================== */
/*  Command-line parsing                                               */
/* ================================================================== */

typedef struct {
    wchar_t file[MAX_PATH_BUF];
    int     screen;
    double  position;
    BOOL    has_file;
    BOOL    has_screen;
    BOOL    has_position;
    BOOL    mute;
    BOOL    help;
} Args;

static void parse_args(Args *a)
{
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;

    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) {
            a->help = TRUE;
        }
        else if ((!wcscmp(argv[i], L"--file") || !wcscmp(argv[i], L"-f"))
            && i + 1 < argc)
        {
            wcscpy(a->file, argv[++i]);
            a->has_file = TRUE;
        }
        else if ((!wcscmp(argv[i], L"--screen") || !wcscmp(argv[i], L"-s"))
                 && i + 1 < argc)
        {
            a->screen = _wtoi(argv[++i]) - 1;  /* user provides 1-based */
            a->has_screen = TRUE;
        }
        else if (!wcscmp(argv[i], L"--mute") || !wcscmp(argv[i], L"-m")) {
            a->mute = TRUE;
        }
        else if ((!wcscmp(argv[i], L"--position") || !wcscmp(argv[i], L"-p"))
                 && i + 1 < argc)
        {
            a->position = _wtof(argv[++i]);
            a->has_position = TRUE;
        }
        else if (!a->has_file) {
            /* A bare argument (no flag) is treated as the file path. */
            wcscpy(a->file, argv[i]);
            a->has_file = TRUE;
        }
    }
    LocalFree(argv);
}

/* ================================================================== */
/*  Help / usage text                                                  */
/* ================================================================== */

static LRESULT CALLBACK help_wnd_proc(HWND hwnd, UINT msg,
                                      WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void show_help(void)
{
    static const wchar_t text[] =
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
        L"Keyboard controls during playback:\r\n"
        L"  ESC          Quit\r\n"
        L"  S            Restart from beginning\r\n"
        L"  P / Space    Toggle pause\r\n"
        L"  R            Seek back 30 seconds\r\n"
        L"  F            Seek forward 30 seconds\r\n"
        L"  Left         Seek back 5 seconds\r\n"
        L"  Right        Seek forward 5 seconds\r\n"
        L"  M            Toggle mute\r\n";

    /* Register a simple window class for the help dialog. */
    static const wchar_t *cls = L"DMP_HelpWnd";
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = help_wnd_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm       = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);

    /* Centre a 640x440 window on the primary monitor. */
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = 640, wh = 440;
    HWND hwnd = CreateWindowExW(
        0, cls, L"DemoMediaPlayer \u2014 Help",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sw - ww) / 2, (sh - wh) / 2, ww, wh,
        NULL, NULL, wc.hInstance, NULL);

    /* Create a read-only multiline EDIT with a fixed-width font. */
    RECT rc;
    GetClientRect(hwnd, &rc);
    HWND edit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", text,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL
        | ES_MULTILINE | ES_READONLY | ES_LEFT,
        0, 0, rc.right, rc.bottom,
        hwnd, NULL, wc.hInstance, NULL);

    HFONT mono = CreateFontW(
        -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(edit, WM_SETFONT, (WPARAM)mono, TRUE);

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    /* Run a small message loop just for this window. */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteObject(mono);
}

/* ================================================================== */
/*  Entry point                                                        */
/* ================================================================== */

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cl, int cs)
{
    (void)hp; (void)cl; (void)cs;

    /* ── Check --help first, before any other initialization ─────── */
    {
        int argc;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; i++) {
                if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) {
                    LocalFree(argv);
                    show_help();
                    return 0;
                }
            }
            LocalFree(argv);
        }
    }

    enum_monitors();
    if (g_nmons == 0) {
        MessageBoxW(NULL, L"No display monitors detected.", APP_TITLE,
                    MB_ICONERROR);
        return 1;
    }

    /* ── Parse command line ──────────────────────────────────────── */
    Args args;
    ZeroMemory(&args, sizeof(args));
    parse_args(&args);

    if (args.help) {
        /* Already handled above; shouldn't reach here. */
        show_help();
        return 0;
    }

    wchar_t *fpath;
    int      scr;

    if (args.mute)
        g_sel_muted = TRUE;

    if (args.has_file) {
        /* CLI mode: go straight to fullscreen playback. */
        fpath = args.file;
        scr   = args.has_screen ? args.screen : 0;
    } else {
        /* Interactive mode: pre-populate from any partial CLI args. */
        if (args.has_screen)
            g_sel_screen = args.screen;
        if (!run_setup(hi))
            return 0;          /* user cancelled */
        fpath = g_sel_file;
        scr   = g_sel_screen;
    }

    if (scr < 0 || scr >= g_nmons) scr = 0;

    /* ── Convert file path to UTF-8 for mpv ──────────────────────── */
    char *u8 = to_utf8(fpath);
    if (!u8) {
        MessageBoxW(NULL, L"Invalid file path encoding.", APP_TITLE,
                    MB_ICONERROR);
        return 1;
    }

    /* ── Create fullscreen window & start mpv ────────────────────── */
    g_player = create_player(hi, scr);
    if (!g_player) {
        free(u8);
        MessageBoxW(NULL, L"Could not create player window.", APP_TITLE,
                    MB_ICONERROR);
        return 1;
    }

    if (play(g_player, u8, args.has_position ? args.position : 0.0) < 0) {
        free(u8);
        return 1;
    }
    free(u8);

    /* Apply initial mute state if requested. */
    if (g_sel_muted && g_mpv) {
        int mute = 1;
        mpv_set_property(g_mpv, "mute", MPV_FORMAT_FLAG, &mute);
    }

    /* ── Main message loop ───────────────────────────────────────── */
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    /* ── Cleanup ─────────────────────────────────────────────────── */
    if (g_mpv) {
        mpv_terminate_destroy(g_mpv);
        g_mpv = NULL;
    }

    return 0;
}
