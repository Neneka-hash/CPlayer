/*
 * win32_ui.h - Declarations for the Win32 UI layer of CPlayer.
 *
 * This module owns the main window, all child controls (playlist ListView,
 * transport buttons, progress/volume sliders, status labels), DPI handling,
 * menu bar, and the language / i18n system. The heavy audio work is done
 * on separate worker threads; the UI thread communicates with them through
 * the control API in audio_waveout.c and through custom window messages
 * (WM_TRACK_LOADED, WM_TRACK_ENDED, WM_TAGS_LOADED, WM_PLAYER_ERROR).
 *
 * DPI: Per-Monitor V2 via the manifest. Fonts, control sizes, and layout
 * are scaled by DPIX()/DPIY() macros based on g_player.dpi.
 */
#ifndef WIN32_UI_H
#define WIN32_UI_H

#include <windows.h>

/* Control ids (used in WM_COMMAND). */
#define IDC_BTN_PREV    100
#define IDC_BTN_PLAY    101
#define IDC_BTN_STOP    102
#define IDC_BTN_NEXT    103
#define IDC_BTN_MODE    104
#define IDC_PROGRESS    105
#define IDC_VOLUME      106
#define IDC_LIST        107
#define IDC_LBL_TIME    108
#define IDC_LBL_VOL     109
#define IDC_LBL_STATUS  110

/* Modern themed UI primitives (GDI, anti-aliased via off-screen DIB).
 * All of these are internal to win32_ui.c but declared here so the design
 * tokens are visible in one place. */
#define UI_RADIUS_BTN   6       /* rounded corners for secondary buttons  */
#define UI_RADIUS_PILL  15      /* fully-rounded pill buttons/status chip  */
#define UI_RADIUS_CARD  10      /* playlist card corner radius             */

/* Menu command ids. */
#define IDM_FILE_ADD_FILES     2001
#define IDM_FILE_ADD_FOLDER    2002
#define IDM_FILE_CLEAR         2003
#define IDM_FILE_EXIT          2004
#define IDM_HELP_ABOUT         2005
#define IDM_FILE_LOAD_M3U      2006
#define IDM_FILE_SAVE_M3U      2007
#define IDM_LANG_FIRST          2008
#define IDM_LANG_ZH            (IDM_LANG_FIRST + 0)
#define IDM_LANG_EN            (IDM_LANG_FIRST + 1)
#define IDM_LANG_ES            (IDM_LANG_FIRST + 2)
#define IDM_LANG_FR            (IDM_LANG_FIRST + 3)
#define IDM_LANG_JA            (IDM_LANG_FIRST + 4)

/* Register the main window class and create the main window.
 * Returns the HWND on success, or NULL on failure. */
HWND ui_create_main(HINSTANCE hInst, int nCmdShow);

/* Recompute the layout and resize / reposition every child for the current
 * DPI. Called on WM_DPICHANGED and after window creation.
 * All dimensions are derived from DPIX()/DPIY() scaling. */
void ui_layout(void);

/* Recreate the UI font for the given DPI and apply it to all children.
 * Falls back to "MS Shell Dlg" if "Segoe UI" is not available. */
void ui_set_font_for_dpi(int dpi);

/* Refresh the playlist ListView from g_playlist.
 * Uses virtual-list mode (LVS_OWNERDATA) — only the item count is set;
 * visible rows request their data via LVN_GETDISPINFO. */
void ui_refresh_playlist(void);

/* Highlight the given index (or clear if -1).
 * Clears all existing selection/focus first, then selects the target row. */
void ui_select_current(int index);

/* Update the progress slider + time label from g_player.cur_frame.
 * Reads g_player state under the critical section. */
void ui_update_position(void);

/* Update the play/pause button label and status text from g_player.state.
 * Also refreshes the mode button label from g_player.play_mode. */
void ui_update_state_controls(void);

/* Refresh all UI text after a language switch.
 * Updates menus, buttons, status, volume label, column headers, and
 * window title to the current language strings. */
void ui_refresh_language(void);

/* Set the status bar text (wide format string).
 * Thread-safe for the UI thread; writes to g_player.hLblStatus. */
void ui_set_status(const wchar_t *fmt, ...);

/* Format seconds as M:SS or H:MM:SS into buf (up to cap characters).
 * Negative values are treated as 0. */
void ui_format_time(double seconds, wchar_t *buf, int cap);

/* Force a full repaint of the custom-drawn chrome (buttons, list background,
 * status chip). Call after a state change that alters colours. */
void ui_repaint(void);

/* Main window procedure. Handles all messages for the main window:
 * WM_CREATE, WM_COMMAND, WM_NOTIFY, WM_HSCROLL, WM_DROPFILES,
 * WM_DPICHANGED, worker-thread notifications, and cleanup on destroy. */
LRESULT CALLBACK main_wndproc(HWND, UINT, WPARAM, LPARAM);

#endif /* WIN32_UI_H */
