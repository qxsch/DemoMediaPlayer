/*
 * constants.h – All compile-time constants for DemoMediaPlayer
 */
#ifndef DMP_CONSTANTS_H
#define DMP_CONSTANTS_H

/* ── Application metadata ────────────────────────────────────── */
#define APP_TITLE        L"DemoMediaPlayer"
#define PLAYER_CLASS     L"DMP_Player"
#define SETUP_CLASS      L"DMP_Setup"
#define IDENTIFY_CLASS   L"DMP_Identify"

/* ── Custom Windows messages ─────────────────────────────────── */
#define WM_MPV_WAKEUP    (WM_USER + 1)

/* ── Limits ──────────────────────────────────────────────────── */
#define DMP_MAX_MONITORS 16
#define MAX_PATH_BUF     4096

/* ── Playback speed (integer % is the source of truth) ─────── */
#define SPEED_MIN_PCT    50
#define SPEED_MAX_PCT    300
#define SPEED_STEP_PCT   10
#define SPEED_MIN        (SPEED_MIN_PCT  / 100.0)
#define SPEED_MAX        (SPEED_MAX_PCT  / 100.0)
#define SPEED_STEP       (SPEED_STEP_PCT / 100.0)

/* ── Volume (0–100 %) ────────────────────────────────────────── */
#define VOLUME_MIN_PCT   0
#define VOLUME_MAX_PCT   100
#define VOLUME_STEP_PCT  10

/* ── Zoom & pan (integer % is the source of truth) ───────────── */
#define ZOOM_MIN_PCT     100
#define ZOOM_MAX_PCT     400
#define ZOOM_STEP_PCT    10
#define ZOOM_MIN         (ZOOM_MIN_PCT  / 100.0)
#define ZOOM_MAX         (ZOOM_MAX_PCT  / 100.0)
#define ZOOM_STEP        (ZOOM_STEP_PCT / 100.0)
#define PAN_STEP         0.01

/* ── Setup-dialog control IDs ────────────────────────────────── */
#define IDC_FILE_EDIT    201
#define IDC_BROWSE       202
#define IDC_SCREEN_COMBO 203
#define IDC_PLAY         204
#define IDC_MUTED        205
#define IDC_IDENTIFY     206
#define IDC_KEEP_TASKBAR 207
#define IDC_CROP_TASKBAR 208
#define IDC_TITLE_LABEL  210

/* ── Taskbar hiding ──────────────────────────────────────────── */
#define DEFAULT_TASKBAR_HEIGHT 48   /* DPI-base / pixel default     */

/* ── Screen recording defaults ───────────────────────────────── */
#define REC_DEFAULT_FPS     30
#define REC_DEFAULT_CODEC   "libx264"
#define REC_DEFAULT_CRF     23
#define REC_DEFAULT_PRESET  "ultrafast"

/* ── Recording control window ────────────────────────────────── */
#define RECCTL_CLASS      L"DMP_RecCtl"
#define IDC_REC_STARTSTOP 301
#define IDC_REC_PAUSE     302
#define IDC_REC_INDICATOR 303
#define IDC_REC_MOUSE     304
#define WM_REC_TICK       (WM_USER + 10)
#define REC_TICK_TIMER    2
#define REC_TICK_INTERVAL 500   /* ms – blink interval */
#define WM_REC_STOP_DONE  (WM_USER + 11)  /* async stop completed */
#define HOTKEY_REC_TOGGLE 1                /* Ctrl+F9  start/stop   */
#define HOTKEY_REC_PAUSE  2                /* Ctrl+F10 pause/resume */

#define CLR_REC_ACTIVE    RGB(220, 38, 38)    /* red indicator    */
#define CLR_REC_PAUSED    RGB(250, 190, 50)   /* amber / paused   */
#define CLR_REC_INACTIVE  RGB(80, 80, 80)     /* gray indicator   */

/* ── Identify overlay ────────────────────────────────────────── */
#define IDENTIFY_TIMER   1
#define IDENTIFY_TIMEOUT 5000   /* ms */

/* ── Help window (base dimensions at 96 DPI) ─────────────────── */
#define HELP_FONT_BASE_PT  14
#define HELP_WND_BASE_W   800
#define HELP_WND_BASE_H   500

/* ── DWM constants (may be missing in older SDK / mingw) ─────── */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#define DWMWCP_ROUND 2

#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

/* ── Modern dark UI colour palette ───────────────────────────── */
#define CLR_BG            RGB(32, 32, 32)
#define CLR_SURFACE       RGB(45, 45, 45)
#define CLR_TEXT          RGB(255, 255, 255)
#define CLR_TEXT_DIM      RGB(155, 155, 155)
#define CLR_ACCENT        RGB(0, 120, 212)
#define CLR_ACCENT_HOV    RGB(30, 145, 235)
#define CLR_ACCENT_PRESS  RGB(0, 95, 170)
#define CLR_INPUT_BG      RGB(50, 50, 50)
#define CLR_BTN_SEC       RGB(55, 55, 55)
#define CLR_BTN_SEC_HOV   RGB(70, 70, 70)
#define CLR_BTN_SEC_PRESS RGB(40, 40, 40)
#define CLR_SEPARATOR     RGB(60, 60, 60)
#define CLR_BORDER        RGB(70, 70, 70)

#endif /* DMP_CONSTANTS_H */
