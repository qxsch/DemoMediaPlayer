/*
 * args.c – Command-line argument parsing
 */
#include "args.h"

#include <string.h>
#include <stdlib.h>

/* ── Full argument parser ────────────────────────────────────── */

void args_parse(AppArgs *a)
{
    ZeroMemory(a, sizeof(*a));

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

/* ── Quick --help check ──────────────────────────────────────── */

BOOL args_has_help(void)
{
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return FALSE;

    BOOL found = FALSE;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) {
            found = TRUE;
            break;
        }
    }
    LocalFree(argv);
    return found;
}
