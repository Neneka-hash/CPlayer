/*
 * mp_player.h - Shared types, globals and configuration for the CPlayer player.
 *
 * This header is the single point of truth shared by all six translation
 * units (main.c, win32_ui.c, playlist.c, audio_decode.c, audio_waveout.c,
 * tag_reader.c). It intentionally contains no code, only declarations.
 */
#ifndef MP_PLAYER_H
#define MP_PLAYER_H

/* ---- Compile environment ---------------------------------------------- */
/* Expose the modern C runtime / Win32 headers cleanly. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
/* NOMINMAX keeps windows.h from defining min/max macros that would collide
 * with the third party codec headers. */
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef UNICODE
#define UNICODE 1
#endif
#ifndef _UNICODE
#define _UNICODE 1
#endif

#include <windows.h>
#include <mmsystem.h>   /* WAVEHDR / HWAVEOUT (excluded by LEAN_AND_MEAN) */
#include <commctrl.h>
#include <stdio.h>
#include <stdint.h>

/* Older SDK headers may predate per-monitor DPI; define the message if absent. */
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

/* ---- Tunable configuration ------------------------------------------- */
/* PCM ring buffer: the producer (decoder) fills free blocks, the consumer
 * (waveOut) drains filled blocks. 8 blocks * 32 KB = 256 KB peak PCM RAM. */
#define NUM_BUFFERS         8
/* Frames per block. 8192 stereo frames = 32 KB = ~186 ms @ 44.1 kHz,
 * ~43 ms @ 192 kHz. Bounded memory regardless of sample rate. */
#define BUFFER_FRAMES       8192
/* Output is always stereo 16-bit so the waveOut pipeline is uniform; source
 * material is down-mixed in audio_decode.c. */
#define OUTPUT_CHANNELS     2

#define TIMER_ID_POSITION   1001
#define TIMER_INTERVAL_MS   200

/* ---- Player state machine -------------------------------------------- */
/* Tracks whether the player is stopped, actively decoding+outputting, or
 * paused (output silenced, decoder suspended). */
enum PlayerState { STATE_STOPPED = 0, STATE_PLAYING, STATE_PAUSED };

/* Language selection. */
#define LANG_ZH 0
#define LANG_EN 1
#define LANG_ES 2
#define LANG_FR 3
#define LANG_JA 4

/* Three play modes. Default is list loop.
 * MODE_LIST_LOOP  - iterate through the playlist, wrap around at the end
 * MODE_SINGLE_LOOP- repeat the current track indefinitely
 * MODE_SHUFFLE    - pick a random track from the playlist each time */
enum PlayMode { MODE_LIST_LOOP = 0, MODE_SINGLE_LOOP, MODE_SHUFFLE };

/* Source format tags. FMT_UNKNOWN triggers an open error.
 * Determined at open time by inspecting the file header (not the extension). */
enum AudioFormat { FMT_UNKNOWN = 0, FMT_MP3, FMT_FLAC, FMT_WAV, FMT_OGG };

/* Commands posted by the UI thread to the decoder thread. All accesses go
 * through g_player.cs unless noted.
 * CMD_OPEN  - start decoding the track at g_player.open_index
 * CMD_SEEK  - seek to g_player.seek_frame frames
 * CMD_STOP  - stop decoding and flush the output pipeline */
enum DecoderCmd { CMD_NONE = 0, CMD_OPEN, CMD_SEEK, CMD_STOP };

/* Lifecycle states of a single PCM block in the pool.
 * BS_FREE     - block is available for the decoder to fill
 * BS_DECODING - decoder is currently writing frames into this block
 * BS_FILLED   - block contains valid audio data, ready for waveOut
 * BS_PLAYING  - block has been submitted to waveOut, waiting for completion */
enum BlockState { BS_FREE = 0, BS_DECODING, BS_FILLED, BS_PLAYING };

/* A single PCM block in the ring buffer. data is heap-allocated once at
 * init and reused across the pool lifetime. */
typedef struct {
    short  *data;       /* BUFFER_FRAMES * OUTPUT_CHANNELS int16 samples   */
    int     frames;     /* number of valid frames in data (0..BUFFER_FRAMES)*/
    WAVEHDR hdr;        /* prepared/unprepared per submission to waveOut   */
    int     state;      /* enum BlockState: FREE→DECODING→FILLED→PLAYING→FREE */
} PcmBlock;

/* Opaque decoder type (full definition lives in audio_decode.h). */
typedef struct Decoder Decoder;

/* Messages posted from worker threads (decoder, tag_reader) to the main
 * window. Handled in win32_ui.c WndProc. */
#define WM_TRACK_LOADED    (WM_USER + 1)   /* decoder thread: a new track finished opening  */
#define WM_TRACK_ENDED     (WM_USER + 2)   /* decoder thread: decoder hit EOF, playback stopped;
                                            * wParam = track_gen of the track that ended, so the
                                            * UI can discard stale messages from a previous track */
#define WM_PLAYER_ERROR    (WM_USER + 3)   /* any thread: wParam = PLAYER_ERR_* code, lParam = 0.
                                            * The UI resolves the text from the current language
                                            * pack, so the message follows runtime language
                                            * switches. Kept as a fallback: if wParam is 0 the
                                            * UI treats lParam as a static string literal the
                                            * caller owns and must NOT free. */
#define WM_TAGS_LOADED     (WM_USER + 4)   /* tag_reader thread: background tag read finished for one file:
                                            * wParam = playlist index, lParam = TagInfo* (heap-allocated) */

/* Error codes for WM_PLAYER_ERROR (posted by decoder / waveOut threads). */
#define PLAYER_ERR_INVALID_INDEX 1  /* 曲目索引无效（列表被清空/越界） */
#define PLAYER_ERR_OPEN_FAILED   2  /* 无法打开文件（格式不支持或损坏） */
#define PLAYER_ERR_DEVICE_OPEN   3  /* 无法打开音频输出设备 */
#define PLAYER_ERR_DEVICE_WRITE  4  /* 音频输出失败（设备可能已断开） */

/* ---- Central player state -------------------------------------------- */
typedef struct {
    /* --- Playlist (contents live in playlist.c) --- */
    int cur_index;                 /* -1 when nothing loaded                 */

    /* --- State (protected by cs) --- */
    int      state;                  /* enum PlayerState                       */
    int      play_mode;              /* enum PlayMode                          */
    int      eof;             /* set when decoder hits EOF; cleared on CMD_OPEN  */
    int      ended_posted;    /* guards WM_TRACK_ENDED against double-posting    */

    /* --- Current track audio format --- */
    int      sample_rate;          /* output waveOut sample rate             */

    /* --- Decoder (owned by decoder thread) --- */
    Decoder *dec;
    uint64_t total_frames;          /* total frames in the current track      */
    uint64_t cur_frame;            /* decoder read position, in frames       */

    /* --- waveOut device --- */
    HWAVEOUT hwo;                  /* waveOut device handle, NULL if closed  */
    int      wo_rate;              /* rate hwo was opened at, 0 if closed    */
    int      volume;               /* 0..100, applied on open and on change  */
    int      wo_err_posted;        /* 1 after a device-write error was reported;
                                       cleared once a submit succeeds, so the
                                       error dialog is not spammed (protected by cs) */

    /* --- PCM buffer pool (protected by cs) --- */
    PcmBlock blocks[NUM_BUFFERS];

    /* --- Threading --- */
    HANDLE         dec_thread;
    HANDLE         wo_thread;
    CRITICAL_SECTION cs;
    HANDLE         wo_event;       /* auto-reset: signalled by waveOut on completion */
    HANDLE         wo_ctrl;        /* auto-reset: wake waveOut (filled/flush/quit)    */
    HANDLE         dec_event;      /* auto-reset: wake decoder (freed/cmd/resume/quit)*/
    HANDLE         wo_idle_evt;    /* auto-reset: waveOut reports flush finished      */

    /* --- Cross-thread control flags --- */
    volatile LONG quit;            /* set on shutdown                         */
    volatile LONG cmd;             /* enum DecoderCmd                         */
    volatile LONG open_index;      /* index for CMD_OPEN                      */
    volatile LONG64 seek_frame;    /* target frame for CMD_SEEK               */
    volatile LONG flushing;        /* decoder owns; waveOut observes          */
    volatile LONG wo_idle;         /* waveOut owns; decoder observes          */

    /* --- Pending-open path (owned by UI thread, read by decoder thread) ---
     * Set by player_open() under cs as a private _wcsdup copy of the
     * playlist entry's path, so the decoder thread (handle_open) never has
     * to dereference g_playlist.items[] without synchronization. The UI
     * thread is the only writer of the playlist, and player_open() also runs
     * on the UI thread, so the copy itself is race-free. The decoder thread
     * copies it again under cs and uses the local copy outside the lock. */
    wchar_t        *open_path;    /* NULL when no open is pending            */

    /* --- Track generation counter ---
     * Incremented (under cs) every time handle_open installs a new track.
     * waveOut stamps WM_TRACK_ENDED with the generation at post time; the
     * UI discards the message if the generation no longer matches, which
     * prevents a stale "track ended" from a previous track auto-advancing
     * past a track the user already switched to. */
    volatile LONG track_gen;       /* current track's generation number      */

    /* --- UI (main thread only, except dpi read in DPICHANGED) --- */
    HWND hMain;
    int  dpi;                      /* current system DPI for the main window */
    HWND hList, hProgress, hVolume;      /* playlist listview, seekbar, volume slider */
    HWND hBtnPrev, hBtnPlay, hBtnStop, hBtnNext, hBtnMode;  /* transport buttons */
    HWND hLblTime, hLblVol, hLblStatus;  /* time display, volume label, status bar */
    HFONT hFont;                   /* large font used for the status label   */
    int  seeking;                  /* 1 while the user drags the progress bar */
    int  lang;                     /* LANG_ZH or LANG_EN */
} Player;

/* Single global instance. Defined in main.c. */
extern Player g_player;

/* Convenience: scale a 96dpi pixel value to the current DPI. */
#define DPIX(v) MulDiv((v), g_player.dpi, 96)

#endif /* MP_PLAYER_H */
