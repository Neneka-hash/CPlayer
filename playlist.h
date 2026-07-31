/*
 * playlist.h - Playlist storage and traversal.
 *
 * Defines the PlaylistEntry and Playlist structs, and declares functions
 * for managing the playlist, including background tag reading.
 */
#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <windows.h>

/* One playlist entry. path is the canonical wide path; name is a display
 * label (file name without extension). title/artist/album are read from
 * the file's tag and may be NULL. duration/bitrate/sample_rate/
 * channels/file_size are filled when the tag is read;
 * bitrate/sample_rate/channels may be 0 if parsing failed.
 *
 * tag_loaded: 0 = tag not yet read (background tag thread pending),
 *             1 = tag read complete (or failed, fields may still be NULL). */
typedef struct {
    wchar_t *path;
    wchar_t *name;        /* file name without extension (fallback title) */
    wchar_t *title;       /* tag title, may be NULL */
    wchar_t *artist;      /* tag artist, may be NULL */
    wchar_t *album;       /* tag album, may be NULL */
    int      format;      /* enum AudioFormat */
    double   duration;    /* seconds, 0 if unknown */
    int      bitrate;     /* kbps, 0 if unknown */
    int      sample_rate; /* Hz, 0 if unknown */
    int      channels;    /* 0 if unknown */
    uint64_t file_size;   /* bytes, 0 if unknown */
    int      tag_loaded;  /* 0 = tag not yet read, 1 = done */
} PlaylistEntry;

typedef struct {
    PlaylistEntry *items;      /* dynamic array of entries                    */
    int            count;      /* number of valid entries                     */
    int            capacity;   /* allocated size of items[]                   */
    /* Background tag reader thread. Started after batch additions
     * (folder drop, M3U load) to fill tag fields asynchronously.
     * All write access to PlaylistEntry tag fields happens on the
     * UI thread (via WM_TAGS_LOADED), so no lock is needed. */
    HANDLE         tag_thread;
    volatile LONG  tag_cancel;    /* 1 = request stop                   */
    volatile LONG  tag_running;   /* 1 = thread active                  */
    volatile LONG  tag_restart;   /* 1 = rescan after current pass       */
    volatile LONG  tag_epoch;     /* incremented on remove/clear; stale   */
                                   /* WM_TAGS_LOADED with old epoch are    */
                                   /* discarded by the UI handler          */
} Playlist;

extern Playlist g_playlist;

/* Init/free the playlist container (does not touch the player). */
void playlist_init(void);
void playlist_free(void);

/* Add a single file (by path) with full tag reading (synchronous).
 * Returns the new index, or -1 on failure / unsupported format / duplicate.
 * Use this for single-file scenarios (command-line open) where you want
 * the tag available immediately. */
int  playlist_add_file(const wchar_t *path);

/* Add a file without reading tags (fast path for batch operations).
 * Fills path / name / format / file_size only; tag_loaded = 0.
 * Caller should invoke playlist_start_tag_thread() afterwards to
 * fill the remaining fields asynchronously. */
int  playlist_add_file_fast(const wchar_t *path);

/* Synchronously read tags for a single entry (by index). */
void playlist_fill_tag(int index);

/* Start / stop the background tag reader thread. start is a no-op if
 * a thread is already running (it sets tag_restart instead so the
 * worker will rescan after its current pass). stop blocks until the
 * thread exits. */
void playlist_start_tag_thread(void);
void playlist_stop_tag_thread(void);

/* Recursively enumerate a directory and add supported audio files.
 * Returns the number added. */
int  playlist_add_folder(const wchar_t *dir);

/* Remove the entry at index. Safe no-op if out of range. */
void playlist_remove_at(int index);

/* Remove every entry. */
void playlist_clear(void);

/* Given the current index and the play mode, return the next index to play,
 * or -1 if the playlist is empty. */
int  playlist_next_index(int cur, int mode);
int  playlist_prev_index(int cur, int mode);

/* Build a display name (file name without extension) from a path. */
void playlist_make_name(const wchar_t *path, wchar_t *out, int cap);

/* Load/save M3U playlist files. UTF-8 with BOM, #EXTM3U header.
 * load_m3u resolves relative paths against the m3u file's directory,
 * appends to the current playlist, returns the count added.
 * save_m3u writes absolute paths; returns 1 on success, 0 on failure. */
int playlist_load_m3u(const wchar_t *m3u_path);
int playlist_save_m3u(const wchar_t *m3u_path);

#endif /* PLAYLIST_H */
