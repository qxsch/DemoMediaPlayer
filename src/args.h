/*
 * args.h – Command-line argument parsing
 */
#ifndef DMP_ARGS_H
#define DMP_ARGS_H

#include <windows.h>
#include "constants.h"

typedef struct {
    wchar_t file[MAX_PATH_BUF];
    int     screen;
    double  position;
    BOOL    has_file;
    BOOL    has_screen;
    BOOL    has_position;
    BOOL    mute;
    BOOL    help;
} AppArgs;

/* Parse the process command line into an AppArgs struct.
   Zeroes the struct first, then fills it from argv. */
void args_parse(AppArgs *a);

/* Quick check: does the command line contain --help / -h?
   This avoids initialising the full argument parser. */
BOOL args_has_help(void);

#endif /* DMP_ARGS_H */
