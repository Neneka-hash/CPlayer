/*
 * main.c - Entry point, DPI-awareness setup, common-control init and the
 * message pump. The global Player instance lives here.
 *
 * The main thread does nothing but run the message loop; all audio work is
 * performed by the decoder and waveOut worker threads started in
 * player_start_threads().
 */
#include "mp_player.h"
#include "win32_ui.h"
#include "playlist.h"
#include "audio_decode.h"
#include "audio_waveout.h"

#include <commctrl.h>
#include <objbase.h>     /* CoInitialize for SHBrowseForFolder */

/* The single global player instance. Zero-initialised at load time. */
Player g_player;

/* ---- DPI awareness (Per-Monitor V2) ----------------------------------- *
 * Declared in the manifest as well; this runtime call is a belt-and-
 * suspenders fallback in case the manifest is ever stripped. We try the
 * Win10 API first, then the Win8.1 one, then the legacy Vista
 * SetProcessDPIAware. */
static void enable_dpi_awareness(void)
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef BOOL (WINAPI *PFN_SetCtx)(HANDLE);
        PFN_SetCtx p = (PFN_SetCtx)
            GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (p) {
            /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == ((DPI_CONTEXT_HANDLE)-4) */
            if (p((HANDLE)-4)) return;
            /* Fall back to V1 if V2 is rejected. */
            if (p((HANDLE)-3)) return;
        }
    }
    HMODULE shc = LoadLibraryW(L"shcore.dll");
    if (shc) {
        typedef HRESULT (WINAPI *PFN_SetProc)(int);
        PFN_SetProc p = (PFN_SetProc)GetProcAddress(shc, "SetProcessDpiAwareness");
        if (p) {
            /* PROCESS_PER_MONITOR_DPI_AWARE == 2 */
            if (SUCCEEDED(p(2))) { FreeLibrary(shc); return; }
        }
        FreeLibrary(shc);
    }
    /* Legacy Vista fallback - loaded dynamically since not every downlevel
     * SDK header declares it. */
    if (u32) {
        typedef BOOL (WINAPI *PFN_SetDPIAware)(void);
        PFN_SetDPIAware p = (PFN_SetDPIAware)
            GetProcAddress(u32, "SetProcessDPIAware");
        if (p) p();
    }
}

/* ---- Entry point ------------------------------------------------------ *
 * Registers DPI awareness, initialises common controls, COM, playlist and
 * audio buffers, starts decoder / waveOut worker threads, creates the main
 * window, then runs the message loop until quit. */
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmd, int nShow)
{
    (void)hPrev;
    (void)lpCmd;

    /* ---- Initialisation ---- */

    enable_dpi_awareness();

    /* Common controls (trackbar + listview). */
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC   = ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    /* COM is needed for the modern folder picker. */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    /* Initialise the playlist data structures. */
    playlist_init();

    /* Allocate the PCM block ring-buffer pool. */
    if (!pool_alloc()) {
        MessageBoxW(NULL, g_player.lang == LANG_EN
                    ? L"Failed to allocate audio buffer"
                    : L"无法分配音频缓冲区",
                    L"CPlayer", MB_ICONERROR);
        return 1;
    }

    /* Start the decoder and waveOut worker threads. */
    if (!player_start_threads()) {
        MessageBoxW(NULL, g_player.lang == LANG_EN
                    ? L"Failed to start audio thread"
                    : L"无法启动音频线程",
                    L"CPlayer", MB_ICONERROR);
        pool_free();
        CoUninitialize();
        return 1;
    }

    /* Create the main application window and all child controls. */
    HWND hwnd = ui_create_main(hInstance, nShow);
    if (!hwnd) {
        player_shutdown();
        pool_free();
        playlist_free();
        CoUninitialize();
        return 1;
    }

    /* If a file was passed on the command line, add and play it. */
    if (lpCmd && *lpCmd) {
        /* Skip leading quotes / whitespace. */
        wchar_t path[MAX_PATH];
        wcsncpy(path, lpCmd, MAX_PATH - 1);
        path[MAX_PATH - 1] = 0;
        /* Strip surrounding quotes.
         * Source is path+1: it holds wcslen(path)-1 characters plus the
         * terminating NUL, so copy wcslen(path) elements to move both the
         * text and the NUL. Using wcslen(path) is correct here; copying
         * fewer would leave the buffer unterminated, and the old code's
         * length was off by one element (an out-of-bounds read). */
        if (path[0] == L'"') {
            memmove(path, path + 1, wcslen(path) * sizeof(wchar_t));
            wchar_t *q = wcschr(path, L'"');
            if (q) *q = 0;
        }
        if (path[0]) {
            int idx = playlist_add_file(path);
            if (idx >= 0) {
                ui_refresh_playlist();
                player_open(idx);
            }
        }
    }

    /* ---- Message loop ---- *
     * Runs until WM_QUIT is posted. Dialog messages are filtered so that
     * keyboard navigation (Tab, arrow keys, etc.) works inside the common
     * controls. All audio work happens on worker threads, so the main thread
     * stays responsive to UI input. */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* Route dialog messages so the common dialogs work. */
        if (g_player.hMain && IsDialogMessageW(g_player.hMain, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* ---- Shutdown ---- */
    player_shutdown();
    pool_free();
    playlist_free();
    CoUninitialize();
    return (int)msg.wParam;
}
