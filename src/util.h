/*
 * util.h – Common utility functions
 */
#ifndef DMP_UTIL_H
#define DMP_UTIL_H

#include <windows.h>

/* Convert a wide string to a heap-allocated UTF-8 string.
   Caller must free() the result. Returns NULL on failure. */
char *to_utf8(const wchar_t *w);

/* Show a standard "Open File" dialog for media files.
   Returns TRUE if the user selected a file; result in buf. */
BOOL browse_file(HWND owner, wchar_t *buf, int buflen);

#endif /* DMP_UTIL_H */
