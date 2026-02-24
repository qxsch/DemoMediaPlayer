/*
 * util.c – Common utility functions
 */
#include "util.h"

#include <commdlg.h>
#include <stdlib.h>

/* ── UTF-8 conversion ────────────────────────────────────────── */

char *to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *buf = (char *)malloc(n);
    if (buf) WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, n, NULL, NULL);
    return buf;
}

/* ── File browser dialog ─────────────────────────────────────── */

BOOL browse_file(HWND owner, wchar_t *buf, int buflen)
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
