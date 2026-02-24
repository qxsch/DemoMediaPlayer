/*
 * theme.h – Dark-mode theming, DWM setup, and owner-draw painting
 */
#ifndef DMP_THEME_H
#define DMP_THEME_H

#include <windows.h>

/* ── Theme resources (created / destroyed by setup dialog) ───── */

typedef struct {
    HBRUSH  br_bg;          /* CLR_BG background brush      */
    HBRUSH  br_input;       /* CLR_INPUT_BG brush            */
    HFONT   title_font;     /* 22 pt semibold (DPI-scaled)   */
    HFONT   ui_font;        /* 14 pt normal   (DPI-scaled)   */
    HFONT   label_font;     /* 12 pt semibold (DPI-scaled)   */
    HWND    hover_btn;      /* currently hovered button       */
    WNDPROC orig_btn_proc;  /* original button window proc    */
    UINT    dpi;            /* current DPI for scaling        */
} ThemeCtx;

/* Create / recreate all theme fonts for the given DPI. */
void theme_create_fonts(ThemeCtx *tc);

/* Create brushes. */
void theme_create_brushes(ThemeCtx *tc);

/* Destroy all theme resources. Safe to call multiple times. */
void theme_destroy(ThemeCtx *tc);

/* Apply DWM immersive-dark-mode attributes to a window. */
void theme_apply_dark_mode(HWND hwnd);

/* ── Owner-draw painting ─────────────────────────────────────── */

/* Paint an owner-draw button (IDC_PLAY gets accent colour). */
void theme_draw_button(const ThemeCtx *tc,
                        DRAWITEMSTRUCT *di);

/* Paint an owner-draw combo-box item. */
void theme_draw_combo_item(const ThemeCtx *tc,
                            DRAWITEMSTRUCT *di);

/* ── Colour message handlers ─────────────────────────────────── */

/* Handle WM_CTLCOLORSTATIC for the setup dialog.
   Returns the brush to use or NULL if unhandled. */
LRESULT theme_handle_ctlcolorstatic(const ThemeCtx *tc,
                                     HDC hdc, HWND ctrl);

/* Handle WM_CTLCOLORLISTBOX for the combo dropdown. */
LRESULT theme_handle_ctlcolorlistbox(const ThemeCtx *tc, HDC hdc);

/* ── Button hover subclass ───────────────────────────────────── */

/* Subclass a button to track hover state for owner-draw.
   ThemeCtx pointer must remain valid for the button lifetime. */
void theme_subclass_button(ThemeCtx *tc, HWND btn);

/* ── DPI convenience ─────────────────────────────────────────── */

/* Scale a value from 96 DPI to the ThemeCtx's current DPI. */
int theme_dpi_scale(const ThemeCtx *tc, int value);

#endif /* DMP_THEME_H */
