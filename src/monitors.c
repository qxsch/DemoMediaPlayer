/*
 * monitors.c – Monitor enumeration and DPI helpers
 */
#include "monitors.h"

#include <stdio.h>
#include <wchar.h>

/* ── Enumeration callback context ────────────────────────────── */

typedef struct {
    MonInfo *mons;
    int      count;
    int      max;
} EnumCtx;

static BOOL CALLBACK enum_mon_cb(HMONITOR hmon, HDC hdc,
                                  LPRECT rc, LPARAM lp)
{
    (void)hdc;
    EnumCtx *ctx = (EnumCtx *)lp;
    if (ctx->count >= ctx->max) return FALSE;

    MonInfo *m = &ctx->mons[ctx->count];
    m->hmon = hmon;
    m->rect = *rc;
    wsprintfW(m->label,
              L"Screen %d  (%d \u00D7 %d  at %d, %d)",
              ctx->count + 1,
              rc->right  - rc->left,
              rc->bottom - rc->top,
              rc->left, rc->top);
    ctx->count++;
    return TRUE;
}

int monitors_enumerate(MonInfo *mons, int max_count)
{
    EnumCtx ctx = { mons, 0, max_count };
    EnumDisplayMonitors(NULL, NULL, enum_mon_cb, (LPARAM)&ctx);
    return ctx.count;
}

/* ── DPI helpers ─────────────────────────────────────────────── */

/* We resolve API pointers once at runtime so we can build with
   older SDK headers and still use per-monitor DPI when available. */

UINT dpi_for_window(HWND hwnd)
{
    typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
    typedef UINT (WINAPI *PFN_GetDpiForSystem)(void);
    static PFN_GetDpiForWindow pfnWnd;
    static PFN_GetDpiForSystem pfnSys;
    static BOOL resolved;
    if (!resolved) {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (u32) {
            pfnWnd = (PFN_GetDpiForWindow)GetProcAddress(u32, "GetDpiForWindow");
            pfnSys = (PFN_GetDpiForSystem)GetProcAddress(u32, "GetDpiForSystem");
        }
        resolved = TRUE;
    }
    if (hwnd && pfnWnd) return pfnWnd(hwnd);
    if (pfnSys)         return pfnSys();
    return 96;
}

UINT dpi_for_monitor(HMONITOR hmon)
{
    typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT *, UINT *);
    static PFN_GetDpiForMonitor pfn;
    static BOOL resolved;
    if (!resolved) {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore)
            pfn = (PFN_GetDpiForMonitor)GetProcAddress(shcore, "GetDpiForMonitor");
        resolved = TRUE;
    }
    if (pfn && hmon) {
        UINT dpiX, dpiY;
        if (SUCCEEDED(pfn(hmon, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY)))
            return dpiX;
    }
    return dpi_for_window(NULL);
}

CursorWindowPos center_on_cursor(int base_w, int base_h)
{
    POINT cur;
    GetCursorPos(&cur);
    HMONITOR hmon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);

    UINT dpi = dpi_for_monitor(hmon);
    int  w   = MulDiv(base_w, (int)dpi, 96);
    int  h   = MulDiv(base_h, (int)dpi, 96);

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hmon, &mi);
    int mw = mi.rcWork.right  - mi.rcWork.left;
    int mh = mi.rcWork.bottom - mi.rcWork.top;

    CursorWindowPos pos;
    pos.x   = mi.rcWork.left + (mw - w) / 2;
    pos.y   = mi.rcWork.top  + (mh - h) / 2;
    pos.w   = w;
    pos.h   = h;
    pos.dpi = dpi;
    return pos;
}

int dpi_scale(int value, UINT dpi)
{
    return MulDiv(value, (int)dpi, 96);
}

HFONT dpi_create_font(const wchar_t *face, int base_pt,
                       int weight, UINT dpi)
{
    int h = -MulDiv(base_pt, (int)dpi, 96);
    return CreateFontW(h, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}
