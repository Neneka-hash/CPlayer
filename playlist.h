/*
 * playlist.h - Playlist storage and traversal.
 *
 * Memory model (tuned for very large playlists, 100k+ entries):
 *
 *   - PlaylistEntry is a small record (24 bytes) holding a reference into a
 *     packed path arena plus file facts; the array itself costs ~2.4 MB per
 *     100k tracks.
 *   - Tag metadata (title / artist / album / duration / bitrate / ...) lives
 *     in a separate EntryMeta block that is loaded ON DEMAND and kept in a
 *     bounded LRU, so metadata memory stays constant no matter how long the
 *     list grows. Display code must tolerate meta == NULL (show the file
 *     name and dashes) and request a load with playlist_meta_request().
 *   - Duplicate detection uses a hash index instead of a linear scan, so
 *     batch imports are O(n) rather than O(n^2).
 *   - Imports run in bounded time slices on the UI thread (no second
 *     writer), keeping the window responsive during a 100k-file drop.
 */
#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <windows.h>
#include <stdint.h>
#include "tag_reader.h"   /* TagInfo (metadata transfer unit) */

/* Per-entry flags. */
#define PLF_WANTED   0x01   /* metadata requested; background worker fills it */

/* Longest path we store (wchars, including NUL). */
#define PL_PATH_MAX  1024

/* Metadata block for one entry. Strings are packed inline after the struct
 * as title\0 artist\0 album\0; a zero length means "absent". Owned by the
 * playlist, managed through the metadata LRU. */
typedef struct EntryMeta EntryMeta;
typedef struct PlaylistEntry PlaylistEntry;
struct EntryMeta {
    EntryMeta     *lru_prev, *lru_next;
    PlaylistEntry *owner;     /* entry this block fills (NULL once orphaned) */
    double   duration;      /* seconds, 0 if unknown */
    int      bitrate;       /* kbps, 0 if unknown    */
    int      sample_rate;
    int      channels;
    uint16_t len_title, len_artist, len_album;
    wchar_t  strs[1];       /* trailing packed strings */
};

/* One playlist entry. path text lives in the path arena at path_off. */
struct PlaylistEntry {
    uint32_t   path_off;   /* wchar offset into the path arena */
    uint16_t   path_len;   /* wchar count, excluding NUL       */
    uint8_t    format;     /* enum AudioFormat                 */
    uint8_t    flags;      /* PLF_*                            */
    uint64_t   file_size;
    EntryMeta *meta;       /* NULL until loaded                */
};

typedef struct {
    PlaylistEntry *items;      /* dynamic array of entries                    */
    int            count;      /* number of valid entries                     */
    int            capacity;   /* allocated size of items[]                   */

    /* Background metadata reader. Fills entries whose PLF_WANTED flag is
     * set. All writes to entries happen on the UI thread (via
     * WM_TAGS_LOADED), so the array itself needs no lock beyond the
     * publication lock below. */
    HANDLE         tag_thread;
    HANDLE         tag_wake;       /* auto-reset: poke the worker        */
    volatile LONG  tag_cancel;     /* 1 = request stop                   */
    volatile LONG  tag_running;    /* 1 = thread active                  */
    volatile LONG  tag_suspended;  /* 1 = don't auto-restart on request  */
    volatile LONG  tag_epoch;      /* incremented on remove/clear; stale
                                    * WM_TAGS_LOADED with an old epoch are
                                    * discarded by the UI handler         */
} Playlist;

extern Playlist g_playlist;

/* Init/free the playlist container (does not touch the player). */
void playlist_init(void);
void playlist_free(void);

/* ---- Adding ---------------------------------------------------------- */

/* Add a single file with synchronous metadata read (command-line open).
 * Returns the entry index (existing one on duplicates), or -1 on failure /
 * unsupported format. */
int  playlist_add_file(const wchar_t *path);

/* Fast add without metadata (batch path). Returns the new index, or -1 if
 * the file is unsupported or already present. */
int  playlist_add_file_fast(const wchar_t *path);

/* Index of a path already in the list (case-insensitive), or -1. */
int  playlist_find(const wchar_t *path);

/* ---- Chunked import --------------------------------------------------- *
 * A drop of 100k files must not freeze the UI, so folder walks and M3U
 * loads run as a session driven by playlist_import_step() from a UI timer.
 * Each call does at most budget_ms milliseconds of work. */

/* Queue a file or folder for import (starts/extends the session). */
void playlist_import_path(const wchar_t *path);

/* Queue an M3U file for import. Returns 1 if the file was opened. */
int  playlist_import_m3u(const wchar_t *m3u_path);

/* Process pending import work for up to budget_ms. Returns 1 while more
 * work remains, 0 when the session has finished. */
int  playlist_import_step(int budget_ms);

/* 1 while an import session is still pending. */
int  playlist_import_active(void);

/* Number of entries added by the current/last session. */
int  playlist_import_added(void);

/* Abandon the pending import work (clear / shutdown). */
void playlist_import_abort(void);

/* ---- Removing -------------------------------------------------------- */

/* Remove several entries in one compaction pass. `indices` must be sorted
 * ascending; out-of-range values are ignored. Far cheaper than repeated
 * playlist_remove_at() for large batches. */
void playlist_remove_indices(const int *indices, int n);

/* Remove the entry at index. Safe no-op if out of range. */
void playlist_remove_at(int index);

/* Remove every entry. */
void playlist_clear(void);

/* ---- Traversal ------------------------------------------------------- */
int  playlist_next_index(int cur, int mode);
int  playlist_prev_index(int cur, int mode);

/* ---- Path / display helpers (UI thread) ------------------------------- */

/* Build a display name (file name without extension) from a path. */
void playlist_make_name(const wchar_t *path, wchar_t *out, int cap);

/* Display name of entry `index` (file name without extension). Returns the
 * number of wchars written (0 if out of range). */
int  playlist_display_name(int index, wchar_t *out, int cap);

/* Heap copy of the entry's path (caller frees), NULL if out of range. */
wchar_t *playlist_dup_path(int index);

/* ---- Metadata (lazy) ------------------------------------------------- */

/* Request a background metadata load for `index`. Cheap; no-op if the
 * metadata is already present. */
void playlist_meta_request(int index);

/* Load metadata synchronously (command-line open). */
void playlist_meta_load_sync(int index);

/* Never evict this entry's metadata (the currently playing track). */
void playlist_pin_meta(int index);

/* Install a freshly read TagInfo into entry `index` (UI thread only).
 * The strings are copied into the entry's packed EntryMeta block; `ti` and
 * its heap strings remain owned by the caller (free with tag_info_free +
 * free). Returns 1 if installed, 0 if the entry is gone / already filled. */
int  playlist_install_meta(int index, TagInfo *ti);

/* Strings for `index` are resolved through this view; `namebuf` receives the
 * file name when no title is available. All returned pointers stay valid
 * until the next call that mutates the playlist on this thread. */
typedef struct {
    const wchar_t *title, *artist, *album;   /* never NULL */
    double   duration;
    int      bitrate, sample_rate, channels;
    uint64_t file_size;
    int      format;
    int      has_meta;
} PlaylistRowView;

void playlist_row_view(int index, PlaylistRowView *v,
                       wchar_t *namebuf, int namecap);

/* ---- M3U ------------------------------------------------------------- */
/* Load/save M3U playlist files. UTF-8 with BOM, #EXTM3U header.
 * save writes absolute paths with #EXTINF using loaded metadata where
 * available and the file name otherwise. */
int playlist_save_m3u(const wchar_t *m3u_path);

#endif /* PLAYLIST_H */
