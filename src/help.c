/*
 * help.c – Help / usage window (RichEdit with inline-tag colouring)
 *
 * Tags:  <t>title</t>  <s>section</s>  <h>heading</h>
 *        <f>flag</f>    <k>key</k>      <x>example</x>
 *        <d>dim</d>
 *
 * The parser strips tags, builds plain text, and records
 * {start, end, style} spans for colouring after insertion.
 */
#include "help.h"
#include "constants.h"
#include "resource.h"
#include "monitors.h"   /* dpi_for_window */
#include "theme.h"      /* theme_apply_dark_mode */
#include <richedit.h>

/* Wide-string literal from macro value: LS(FOO) → L"<value>" */
#define _LS_STR(x) #x
#define _LS_CAT(x) L ## x
#define _LS_W(x)   _LS_CAT(x)
#define LS(x)      _LS_W(_LS_STR(x))

/* ── Colour palette ──────────────────────────────────────────── */
#define HELP_BG       RGB(30, 30, 30)
#define HELP_FG       RGB(204, 204, 204)
#define CLR_TITLE     RGB(86, 182, 255)     /* bright blue       */
#define CLR_SECTION   RGB(78, 201, 176)     /* teal              */
#define CLR_HEADING   RGB(220, 180, 80)     /* gold              */
#define CLR_FLAG      RGB(156, 220, 120)    /* green             */
#define CLR_KEY       RGB(206, 145, 255)    /* purple            */
#define CLR_EXAMPLE   RGB(120, 120, 120)    /* dim gray          */
#define CLR_DIM       RGB(100, 100, 100)    /* dimmer gray       */

/* ── RichEdit class name ─────────────────────────────────────── */
#ifndef MSFTEDIT_CLASS
#define MSFTEDIT_CLASS L"RICHEDIT50W"
#endif

/* ── Style IDs ───────────────────────────────────────────────── */
enum { ST_T, ST_S, ST_H, ST_F, ST_K, ST_X, ST_D, ST_COUNT };

static const struct { const wchar_t *open; const wchar_t *close;
                      COLORREF clr; BOOL bold; } s_styles[ST_COUNT] = {
    [ST_T] = { L"<t>", L"</t>", CLR_TITLE,   TRUE  },
    [ST_S] = { L"<s>", L"</s>", CLR_SECTION, TRUE  },
    [ST_H] = { L"<h>", L"</h>", CLR_HEADING, TRUE  },
    [ST_F] = { L"<f>", L"</f>", CLR_FLAG,    FALSE },
    [ST_K] = { L"<k>", L"</k>", CLR_KEY,     FALSE },
    [ST_X] = { L"<x>", L"</x>", CLR_EXAMPLE, FALSE },
    [ST_D] = { L"<d>", L"</d>", CLR_DIM,     FALSE },
};

/* ── Tagged help text ────────────────────────────────────────── */

static const wchar_t s_help_tagged[] =
    L"<t>DemoMediaPlayer \u2014 Minimal Fullscreen Media Player</t>\r\n"
    L"\r\n"
    L"<s>\u2500\u2500 Playback \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500</s>\r\n"
    L"\r\n"
    L"<h>Usage:</h>\r\n"
    L"  <x>mediaplayer.exe</x>                         Interactive mode (setup dialog)\r\n"
    L"  <x>mediaplayer.exe --file <path></x>           Play file fullscreen\r\n"
    L"  <x>mediaplayer.exe --file <path> --screen N</x>\r\n"
    L"  <x>mediaplayer.exe <path></x>                  Bare argument = file path\r\n"
    L"\r\n"
    L"<h>Options:</h>\r\n"
    L"  <f>-f</f>, <f>--file <path></f>    Path to the media file to play\r\n"
    L"  <f>-s</f>, <f>--screen <N></f>     Screen number (1-based, default: 1)\r\n"
    L"  <f>-m</f>, <f>--mute</f>           Start playback muted\r\n"
    L"  <f>-p</f>, <f>--position <N></f>   Start at position N seconds\r\n"
    L"  <f>-h</f>, <f>--help</f>           Show this help message\r\n"
    L"\r\n"
    L"<h>Taskbar options:</h>\r\n"
    L"  <f>--keep-taskbar-visible[=N]</f>   Shrink window by N DPI units\r\n"
    L"                               <d>(default " LS(DEFAULT_TASKBAR_HEIGHT) L")</d> to reveal the\r\n"
    L"                               real taskbar underneath\r\n"
    L"  <f>--crop-video-taskbar[=N]</f>     Crop N pixels <d>(default " LS(DEFAULT_TASKBAR_HEIGHT) L")</d>\r\n"
    L"                               from the bottom of the source\r\n"
    L"                               video to hide the recorded\r\n"
    L"                               taskbar\r\n"
    L"  <f>--fix-taskbar[=N]</f>            Shorthand for both options\r\n"
    L"                               above with the same value\r\n"
    L"\r\n"
    L"<h>Keyboard controls:</h>\r\n"
    L"  <k>ESC</k>          Quit\r\n"
    L"  <k>S</k>            Restart from beginning\r\n"
    L"  <k>P</k> / <k>Space</k>    Toggle pause\r\n"
    L"  <k>R</k>            Seek back 30 seconds\r\n"
    L"  <k>F</k>            Seek forward 30 seconds\r\n"
    L"  <k>Left</k>         Seek back 5 seconds\r\n"
    L"  <k>Right</k>        Seek forward 5 seconds\r\n"
    L"  <k>Up</k>           Speed +" LS(SPEED_STEP_PCT) L"% <d>(max " LS(SPEED_MAX_PCT) L"%)</d>\r\n"
    L"  <k>Down</k>         Speed -" LS(SPEED_STEP_PCT) L"% <d>(min " LS(SPEED_MIN_PCT) L"%)</d>\r\n"
    L"  <k>Enter</k>        Reset speed to 100%\r\n"
    L"  <k>M</k>            Toggle mute\r\n"
    L"\r\n"
    L"  <k>4</k>            Pan left\r\n"
    L"  <k>6</k>            Pan right\r\n"
    L"  <k>8</k>            Pan up\r\n"
    L"  <k>2</k>            Pan down\r\n"
    L"  <k>9</k> / <k>-</k>        Zoom out " LS(ZOOM_STEP_PCT) L"% <d>(min " LS(ZOOM_MIN_PCT) L"%)</d>\r\n"
    L"  <k>3</k> / <k>+</k>        Zoom in " LS(ZOOM_STEP_PCT) L"% <d>(max " LS(ZOOM_MAX_PCT) L"%)</d>\r\n"
    L"  <k>0</k>            Reset pan and zoom\r\n"
    L"  <k>A</k>            Reset all <d>(zoom, pan, speed)</d>\r\n"
    L"\r\n"
    L"<s>\u2500\u2500 Screen Recording \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500</s>\r\n"
    L"\r\n"
    L"<h>Options:</h>\r\n"
    L"  <f>-r</f>, <f>--record</f>         Record a screen to H.264 MP4\r\n"
    L"  <f>-f</f>, <f>--file <path></f>    Output file path (required)\r\n"
    L"  <f>-s</f>, <f>--screen <N></f>     Screen to record <d>(1-based, default: 1)</d>\r\n"
    L"  <f>--fps <N></f>            Frame rate <d>(default: " LS(REC_DEFAULT_FPS) L")</d>\r\n"
    L"  <f>--no-audio</f>           Disable system audio capture\r\n"
    L"  <f>--audio-device <name></f>  Audio input device name\r\n"
    L"                       <d>(default: auto-detect loopback)</d>\r\n"
    L"  <f>--disable-mouse-capture</f>  Do not draw the mouse cursor\r\n"
    L"\r\n"
    L"<h>Control window:</h>\r\n"
    L"  A floating control window appears during recording:\r\n"
    L"    <k>\u23FA Record</k> / <k>\u23F9 Stop</k>    Start or stop the recording\r\n"
    L"    <k>Pause</k> / <k>Resume</k>       Pause or resume capture\r\n"
    L"    <k>[ ] Capture mouse</k>    Toggle mouse cursor in video\r\n"
    L"    <k>Blinking indicator</k>   <d>Red = recording, Amber = paused</d>\r\n"
    L"\r\n"
    L"<h>Encoding:</h>\r\n"
    L"  Video: <f>libx264</f> <d>(CRF " LS(REC_DEFAULT_CRF) L", ultrafast)</d> in MP4 container\r\n"
    L"  Audio: WASAPI loopback \u2192 <f>AAC 192 kbps</f>\r\n"
    L"\r\n"
    L"<h>Examples:</h>\r\n"
    L"  <x>mediaplayer.exe --record -s 1 -f recording.mp4</x>\r\n"
    L"  <x>mediaplayer.exe --record -s 2 -f out.mp4 --fps 60</x>\r\n"
    L"  <x>mediaplayer.exe --record -f out.mp4 --no-audio</x>\r\n"
    L"  <x>mediaplayer.exe --record -f out.mp4 --disable-mouse-capture</x>\r\n";

/* ── Tag parser ──────────────────────────────────────────────── */

typedef struct { LONG start; LONG end; int style; } Span;

/*
 * Parse s_help_tagged → plain text + colour spans.
 * Returns the plain text (caller must free).
 * *out_spans receives a malloc'd array, *out_count its length.
 */
static wchar_t *parse_tags(const wchar_t *src,
                           Span **out_spans, int *out_count)
{
    size_t slen = wcslen(src);
    wchar_t *plain = (wchar_t *)malloc((slen + 1) * sizeof(wchar_t));
    Span    *spans = (Span *)malloc(256 * sizeof(Span));
    int      cap   = 256, cnt = 0;
    LONG     wp    = 0;          /* write position in plain */
    size_t   rp    = 0;          /* read position in src    */

    /*
     * RichEdit converts every \r\n pair to a single \r internally,
     * so we must do the same here to keep span positions in sync.
     */
#define COPY_CHAR() do {                           \
        plain[wp++] = src[rp++];                   \
        if (src[rp-1]==L'\r' && rp<slen && src[rp]==L'\n') rp++; \
    } while(0)

    while (rp < slen) {
        if (src[rp] == L'<') {
            /* Try to match an opening or closing tag. */
            BOOL matched = FALSE;
            for (int s = 0; s < ST_COUNT; s++) {
                /* opening tag */
                size_t olen = wcslen(s_styles[s].open);
                if (wcsncmp(src + rp, s_styles[s].open, olen) == 0) {
                    rp += olen;
                    LONG span_start = wp;
                    /* find closing tag */
                    size_t clen = wcslen(s_styles[s].close);
                    while (rp < slen) {
                        if (src[rp] == L'<'
                            && wcsncmp(src + rp, s_styles[s].close, clen) == 0) {
                            rp += clen;
                            break;
                        }
                        COPY_CHAR();
                    }
                    if (cnt == cap) {
                        cap *= 2;
                        spans = (Span *)realloc(spans, (size_t)cap * sizeof(Span));
                    }
                    spans[cnt++] = (Span){ span_start, wp, s };
                    matched = TRUE;
                    break;
                }
            }
            if (!matched) { COPY_CHAR(); }
        } else {
            COPY_CHAR();
        }
    }
#undef COPY_CHAR
    plain[wp] = L'\0';
    *out_spans = spans;
    *out_count = cnt;
    return plain;
}

/* ── RichEdit formatting helpers ─────────────────────────────── */

static void set_fmt(HWND r, LONG a, LONG b, COLORREF clr, BOOL bold)
{
    if (a >= b) return;
    CHARRANGE sel = { a, b };
    SendMessageW(r, EM_EXSETSEL, 0, (LPARAM)&sel);
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize      = sizeof(cf);
    cf.dwMask      = CFM_COLOR | CFM_BOLD;
    cf.crTextColor = clr;
    cf.dwEffects   = bold ? CFE_BOLD : 0;
    SendMessageW(r, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

#define HELP_FONT_PT 12
static void help_set_font(HWND rich, UINT mon_dpi)
{
    UINT sys_dpi = dpi_for_window(NULL);
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize      = sizeof(cf);
    cf.dwMask      = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BOLD;
    /* RichEdit50W converts twips using the system (primary) DPI,
       not the per-monitor DPI.  Scale the twips value to compensate. */
    cf.yHeight     = (LONG)MulDiv(HELP_FONT_PT * 20, (int)mon_dpi, (int)sys_dpi);
    cf.crTextColor = HELP_FG;
    cf.dwEffects   = 0;
    lstrcpynW(cf.szFaceName, L"Consolas", LF_FACESIZE);
    SendMessageW(rich, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
}

/* Apply font + colour spans to the RichEdit.
   Caller supplies the parsed spans array. */
static void help_apply_spans(HWND rich, const Span *spans, int count,
                             UINT mon_dpi)
{
    SendMessageW(rich, WM_SETREDRAW, FALSE, 0);
    help_set_font(rich, mon_dpi);

    for (int i = 0; i < count; i++) {
        int s = spans[i].style;
        set_fmt(rich, spans[i].start, spans[i].end,
                s_styles[s].clr, s_styles[s].bold);
    }

    CHARRANGE cr = { 0, 0 };
    SendMessageW(rich, EM_EXSETSEL, 0, (LPARAM)&cr);
    SendMessageW(rich, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(rich, NULL, TRUE);
    SendMessageW(rich, WM_VSCROLL, SB_TOP, 0);
}

/* ── Window-level data (stored via GWLP_USERDATA) ────────────── */

typedef struct {
    Span *spans;
    int   span_count;
} HelpData;

/* ── Window procedure ────────────────────────────────────────── */

static LRESULT CALLBACK help_wnd_proc(HWND hwnd, UINT msg,
                                       WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: {
        HWND child = GetWindow(hwnd, GW_CHILD);
        if (child) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            MoveWindow(child, 0, 0, rc.right, rc.bottom, TRUE);
        }
        return 0;
    }
    case WM_DPICHANGED: {
        HelpData *hd = (HelpData *)(LONG_PTR)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        HWND child = GetWindow(hwnd, GW_CHILD);
        /* After WM_DPICHANGED, RichEdit recalibrates its internal
           DPI context to the actual monitor DPI — no compensation
           needed, so pass sys_dpi to make the ratio 1:1. */
        if (child && hd)
            help_apply_spans(child, hd->spans, hd->span_count,
                             dpi_for_window(NULL));
        const RECT *rc = (const RECT *)lp;
        SetWindowPos(hwnd, NULL, rc->left, rc->top,
                     rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == EN_SETFOCUS) {
            HideCaret((HWND)lp);
            return 0;
        }
        break;
    case WM_DESTROY: {
        HelpData *hd = (HelpData *)(LONG_PTR)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (hd) { free(hd->spans); free(hd); }
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
    LoadLibraryW(L"Msftedit.dll");

    static const wchar_t *cls = L"DMP_HelpWnd";
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = help_wnd_proc;
    wc.hInstance      = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm       = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hbrBackground = CreateSolidBrush(HELP_BG);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);

    CursorWindowPos wp = center_on_cursor(HELP_WND_BASE_W, HELP_WND_BASE_H);

    HWND hwnd = CreateWindowExW(
        0, cls, L"DemoMediaPlayer \u2014 Help",
        WS_OVERLAPPEDWINDOW,
        wp.x, wp.y, wp.w, wp.h,
        NULL, NULL, wc.hInstance, NULL);

    theme_apply_dark_mode(hwnd);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    /* Create the RichEdit AFTER showing the parent so that the
       per-monitor DPI context is fully established.  This avoids
       the control using a stale / system-default DPI for its
       internal twips-to-pixels conversion. */
    RECT rc;
    GetClientRect(hwnd, &rc);
    HWND rich = CreateWindowExW(
        0, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL
        | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | ES_LEFT,
        0, 0, rc.right, rc.bottom,
        hwnd, NULL, wc.hInstance, NULL);

    SendMessageW(rich, EM_SETBKGNDCOLOR, 0, (LPARAM)HELP_BG);
    SendMessageW(rich, EM_SETEVENTMASK, 0, 0);

    /* Parse tagged text → plain + spans, insert plain. */
    Span *spans = NULL;
    int   span_count = 0;
    wchar_t *plain = parse_tags(s_help_tagged, &spans, &span_count);
    SetWindowTextW(rich, plain);
    free(plain);

    /* Store spans for re-application on DPI change. */
    HelpData *hd = (HelpData *)calloc(1, sizeof(HelpData));
    hd->spans      = spans;
    hd->span_count = span_count;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)hd);

    /* Apply font + colours. */
    help_apply_spans(rich, spans, span_count, wp.dpi);
    HideCaret(rich);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
