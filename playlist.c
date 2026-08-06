/*
 * playlist.c - Playlist storage and traversal.
 *
 * A simple growable array of PlaylistEntry. Path utilities are implemented
 * locally (no shlwapi dependency). Folder enumeration is recursive.
 */
#include "mp_player.h"
#include "playlist.h"
#include "audio_decode.h"   /* detect_format */
#include "tag_reader.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>

Playlist g_playlist;

/* Paths can get long when recursing; use a generous local buffer. */
#define PATH_BUF 1024

/* Initialize the playlist to an empty state. */
void playlist_init(void)
{
    g_playlist.items      = NULL;
    g_playlist.count      = 0;
    g_playlist.capacity   = 0;
    g_playlist.tag_thread  = NULL;
    g_playlist.tag_cancel  = 0;
    g_playlist.tag_running = 0;
    g_playlist.tag_restart = 0;
    g_playlist.tag_suspended = 0;
    g_playlist.tag_epoch   = 0;
}

/* Free the playlist: stop the background tag thread first (so it does not
 * access entries during deallocation), then release all entries. */
void playlist_free(void)
{
    playlist_stop_tag_thread();
    playlist_clear();
}

/* Grow the array so at least one more entry fits. */
static int ensure_capacity(void)
{
    if (g_playlist.count < g_playlist.capacity)
        return 1;
    int newcap = g_playlist.capacity ? g_playlist.capacity * 2 : 16;
    PlaylistEntry *p = (PlaylistEntry *)realloc(
        g_playlist.items, (size_t)newcap * sizeof(PlaylistEntry));
    if (!p)
        return 0;
    g_playlist.items    = p;
    g_playlist.capacity = newcap;
    return 1;
}

/* Case-insensitive path comparison to avoid duplicate entries. */
static int path_equals(const wchar_t *a, const wchar_t *b)
{
    return a && b && _wcsicmp(a, b) == 0;
}

/* Fast add: fill path / name / format / file_size only (no file I/O
 * beyond a single GetFileAttributesExW). tag_loaded stays 0 so the
 * background thread will pick it up later. */
int playlist_add_file_fast(const wchar_t *path)
{
    if (!path || !*path)
        return -1;

    int fmt = detect_format(path);
    if (fmt == FMT_UNKNOWN)
        return -1;

    /* De-duplicate. */
    for (int i = 0; i < g_playlist.count; i++)
        if (path_equals(g_playlist.items[i].path, path))
            return i;

    if (!ensure_capacity())
        return -1;

    PlaylistEntry *e = &g_playlist.items[g_playlist.count];
    memset(e, 0, sizeof(*e));
    e->path = _wcsdup(path);
    if (!e->path)
        return -1;
    wchar_t namebuf[PATH_BUF];
    playlist_make_name(path, namebuf, PATH_BUF);
    e->name   = _wcsdup(namebuf);
    e->format = fmt;
    if (!e->name) { free(e->path); e->path = NULL; return -1; }

    /* file_size via a single attribute query — no file content read. */
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
        e->file_size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;

    e->tag_loaded = 0;
    return g_playlist.count++;
}

/* Synchronously read tags for entry `index`. Used by playlist_add_file
 * (single-file command-line open) and could be used for on-demand reading. */
void playlist_fill_tag(int index)
{
    if (index < 0 || index >= g_playlist.count)
        return;
    PlaylistEntry *e = &g_playlist.items[index];
    if (e->tag_loaded)
        return;

    TagInfo ti;
    if (tag_read(e->path, e->format, &ti) == 0) {
        e->title       = ti.title;       /* takes ownership of heap strings */
        e->artist      = ti.artist;
        e->album       = ti.album;
        e->duration    = ti.duration;
        e->bitrate     = ti.bitrate;
        e->sample_rate = ti.sample_rate;
        e->channels    = ti.channels;
        if (e->file_size == 0)
            e->file_size = ti.file_size;
    }
    e->tag_loaded = 1;
}

/* Full add: fast path + synchronous tag read. Use for single-file opens
 * where the tag should be available immediately (e.g. command-line). */
int playlist_add_file(const wchar_t *path)
{
    int idx = playlist_add_file_fast(path);
    if (idx >= 0)
        playlist_fill_tag(idx);
    return idx;
}

/* Recursive directory enumeration. */
static void enum_dir(const wchar_t *dir)
{
    wchar_t pattern[PATH_BUF];
    _snwprintf(pattern, PATH_BUF, L"%s\\*", dir);
    pattern[PATH_BUF - 1] = 0;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") == 0 ||
                wcscmp(fd.cFileName, L"..") == 0)
                continue;
            wchar_t subdir[PATH_BUF];
            _snwprintf(subdir, PATH_BUF, L"%s\\%s", dir, fd.cFileName);
            subdir[PATH_BUF - 1] = 0;
            enum_dir(subdir);
        } else {
            wchar_t path[PATH_BUF];
            _snwprintf(path, PATH_BUF, L"%s\\%s", dir, fd.cFileName);
            path[PATH_BUF - 1] = 0;
            playlist_add_file_fast(path);
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

/* Recursively enumerate a directory and add all supported audio files.
 * Returns the number added. */
int playlist_add_folder(const wchar_t *dir)
{
    if (!dir || !*dir)
        return 0;
    int before = g_playlist.count;
    enum_dir(dir);
    return g_playlist.count - before;
}

/* Remove the entry at index. The tag thread is stopped before removal
 * (to prevent it from posting a now-stale index) and restarted afterwards
 * so the remaining entries get their tags filled. */
void playlist_remove_at(int index)
{
    if (index < 0 || index >= g_playlist.count)
        return;
    /* Bump epoch so stale WM_TAGS_LOADED messages from the running
     * thread are discarded by the UI handler. */
    InterlockedIncrement(&g_playlist.tag_epoch);
    /* Stop the tag thread: it may be about to PostMessage an index
     * that will shift after removal. Blocking stop is cheap (thread
     * checks tag_cancel between each file). */
    playlist_stop_tag_thread();
    free(g_playlist.items[index].path);
    free(g_playlist.items[index].name);
    free(g_playlist.items[index].title);
    free(g_playlist.items[index].artist);
    free(g_playlist.items[index].album);
    /* Shift the tail down. */
    for (int i = index; i < g_playlist.count - 1; i++)
        g_playlist.items[i] = g_playlist.items[i + 1];
    g_playlist.count--;
    /* Restart tag reading for the remaining items — unless the caller is
     * in the middle of a batch deletion (tag_suspended), in which case the
     * thread is brought back once by playlist_resume_tag_thread(). */
    if (!InterlockedExchangeAdd(&g_playlist.tag_suspended, 0))
        playlist_start_tag_thread();
}

/* Remove all entries. The tag thread is stopped before freeing entries
 * to prevent it from accessing freed memory. */
void playlist_clear(void)
{
    InterlockedIncrement(&g_playlist.tag_epoch);
    playlist_stop_tag_thread();
    for (int i = 0; i < g_playlist.count; i++) {
        free(g_playlist.items[i].path);
        free(g_playlist.items[i].name);
        free(g_playlist.items[i].title);
        free(g_playlist.items[i].artist);
        free(g_playlist.items[i].album);
    }
    free(g_playlist.items);
    g_playlist.items    = NULL;
    g_playlist.count    = 0;
    g_playlist.capacity = 0;
}

/* 随机播放种子：首次使用随机模式时以系统时间为种子，避免每次启动
 * 的随机序列完全相同。 */
static void shuffle_seed_once(void)
{
    static volatile int seeded = 0;
    if (!seeded) {
        srand((unsigned)GetTickCount64() ^ (unsigned)(uintptr_t)&seeded);
        seeded = 1;
    }
}

/* Return the next index to play given the current index and play mode. */
int playlist_next_index(int cur, int mode)
{
    int n = g_playlist.count;
    if (n == 0)
        return -1;
    if (n == 1)
        return 0;            /* single track: every mode loops it */
    switch (mode) {
    case MODE_SINGLE_LOOP:
        return cur;
    case MODE_SHUFFLE: {
        shuffle_seed_once();
        int nxt;
        do {
            int r;
            /* 拒绝采样消除 rand()%n 的模偏差。 */
            do { r = rand(); } while (r >= RAND_MAX - RAND_MAX % n);
            nxt = r % n;
        } while (nxt == cur);  /* pick a random index different from current */
        return nxt;
    }
    case MODE_LIST_LOOP:
    default:
        return (cur < 0) ? 0 : (cur + 1) % n;
    }
}

/* Return the previous index to play given the current index and play mode. */
int playlist_prev_index(int cur, int mode)
{
    int n = g_playlist.count;
    if (n == 0)
        return -1;
    if (n == 1)
        return 0;
    switch (mode) {
    case MODE_SINGLE_LOOP:
        return cur;
    case MODE_SHUFFLE: {
        shuffle_seed_once();
        int prv;
        do {
            int r;
            /* 与 next 分支一致：拒绝采样消除 rand()%n 的模偏差。 */
            do { r = rand(); } while (r >= RAND_MAX - RAND_MAX % n);
            prv = r % n;
        } while (prv == cur);
        return prv;
    }
    case MODE_LIST_LOOP:
    default:
        return (cur <= 0) ? n - 1 : cur - 1;
    }
}

/* Build a display name (file name without extension) from a full path. */
void playlist_make_name(const wchar_t *path, wchar_t *out, int cap)
{
    if (!out || cap <= 0)
        return;
    out[0] = 0;

    /* Find the last separator. */
    const wchar_t *base = path;
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    /* If there is a drive "X:" prefix and no separator, skip it. */
    if (base == path && path[0] && path[1] == L':')
        base = path + 2;

    /* Copy the base name. */
    int len = 0;
    for (const wchar_t *p = base; *p && len < cap - 1; p++)
        out[len++] = *p;
    out[len] = 0;

    /* Strip the extension (last '.'). */
    for (int i = len - 1; i >= 0; i--) {
        if (out[i] == L'.') {
            out[i] = 0;
            break;
        }
    }
}

/* ---- Background tag reader thread ------------------------------------ */
/* Scans the playlist for entries with tag_loaded == 0 and reads their tags.
 * Results are delivered to the UI thread via WM_TAGS_LOADED (one message
 * per file). The UI thread writes the tag fields into PlaylistEntry, so no
 * lock is needed on the playlist side.
 *
 * Thread safety: playlist_clear / playlist_remove_at / playlist_free call
 * playlist_stop_tag_thread() before modifying the array, so the worker is
 * never active when items are freed or shifted. playlist_add_file_fast
 * only appends (never modifies existing entries), so it is safe to call
 * while the worker is running — the new entries will be picked up via
 * the tag_restart mechanism.
 *
 * Stale-message protection: playlist_remove_at / playlist_clear increment
 * tag_epoch before stopping the thread. The tag worker captures the epoch
 * at the start of each pass and stamps it into every TagInfo it sends.
 * The WM_TAGS_LOADED handler in the UI thread discards messages whose
 * epoch does not match the current tag_epoch, preventing stale messages
 * (posted before the stop but still in the queue) from writing wrong
 * tag data into shifted array positions. */
static DWORD WINAPI tag_worker_thread(LPVOID param)
{
    (void)param;
    do {
        InterlockedExchange(&g_playlist.tag_restart, 0);
        LONG epoch = InterlockedExchangeAdd(&g_playlist.tag_epoch, 0);
        int n = g_playlist.count;

        for (int i = 0; i < n; i++) {
            if (InterlockedExchangeAdd(&g_playlist.tag_cancel, 0))
                goto done;
            if (g_playlist.items[i].tag_loaded)
                continue;

            const wchar_t *path = g_playlist.items[i].path;
            int fmt = g_playlist.items[i].format;
            if (!path) continue;

            TagInfo ti;
            if (tag_read(path, fmt, &ti) == 0) {
                ti.epoch = epoch;
                TagInfo *heap = (TagInfo *)malloc(sizeof(TagInfo));
                if (heap) {
                    *heap = ti;
                    /* If the main window is gone (shutdown), the message
                     * cannot be delivered — release the payload now. */
                    if (PostMessage(g_player.hMain, WM_TAGS_LOADED,
                                    (WPARAM)i, (LPARAM)heap))
                        continue;
                    tag_info_free(heap);
                    free(heap);
                } else {
                    tag_info_free(&ti);
                }
            }
            /* tag_read failed or malloc failed: send an empty TagInfo so
             * the UI thread marks tag_loaded = 1 and stops retrying. */
            TagInfo *empty = (TagInfo *)calloc(1, sizeof(TagInfo));
            if (empty) {
                empty->epoch = epoch;
                if (!PostMessage(g_player.hMain, WM_TAGS_LOADED,
                                 (WPARAM)i, (LPARAM)empty))
                    free(empty);
            }
        }
    } while (InterlockedExchangeAdd(&g_playlist.tag_restart, 0));

done:
    InterlockedExchange(&g_playlist.tag_running, 0);
    return 0;
}

/* Start the background tag reader thread. If a thread is already running,
 * set the restart flag so it rescans after its current pass. */
void playlist_start_tag_thread(void)
{
    if (InterlockedExchangeAdd(&g_playlist.tag_running, 0)) {
        /* Already running — request a rescan to pick up newly added files. */
        InterlockedExchange(&g_playlist.tag_restart, 1);
        return;
    }
    InterlockedExchange(&g_playlist.tag_cancel, 0);
    InterlockedExchange(&g_playlist.tag_restart, 0);
    InterlockedExchange(&g_playlist.tag_running, 1);

    HANDLE h = CreateThread(NULL, 0, tag_worker_thread, NULL, 0, NULL);
    if (h) {
        if (g_playlist.tag_thread)
            CloseHandle(g_playlist.tag_thread);
        g_playlist.tag_thread = h;
    } else {
        InterlockedExchange(&g_playlist.tag_running, 0);
    }
}

/* Stop the background tag reader thread. Blocks until the thread exits. */
void playlist_stop_tag_thread(void)
{
    InterlockedExchange(&g_playlist.tag_cancel, 1);
    if (g_playlist.tag_thread) {
        WaitForSingleObject(g_playlist.tag_thread, INFINITE);
        CloseHandle(g_playlist.tag_thread);
        g_playlist.tag_thread = NULL;
    }
    InterlockedExchange(&g_playlist.tag_running, 0);
    InterlockedExchange(&g_playlist.tag_cancel, 0);
}

/* Suspend the tag reader across a batch of removals. */
void playlist_suspend_tag_thread(void)
{
    InterlockedExchange(&g_playlist.tag_suspended, 1);
    playlist_stop_tag_thread();
}

/* Resume the tag reader after a batch of removals. */
void playlist_resume_tag_thread(void)
{
    InterlockedExchange(&g_playlist.tag_suspended, 0);
    playlist_start_tag_thread();
}

/* ---- M3U playlist load/save ------------------------------------------ */

/* 读取 M3U/M3U8 文件，将每条路径追加到当前播放列表。
 * 编码检测，按优先级：
 *   1. UTF-8 BOM (EF BB BF) → 按 UTF-8 解码；
 *   2. 无 BOM → 先用严格 UTF-8 试解（合法 UTF-8 中文序列必然通过）；
 *      失败说明是 ANSI 编码（中文系统即 GBK），回退到系统代码页。
 * 这同时兼容 UTF-8（含无 BOM 的 UTF-8，如本程序旧版/第三方工具导出）
 * 与 GBK/ANSI 两种常见 M3U 编码。
 * 相对路径基于 m3u 文件所在目录解析。
 * 跳过 #EXTM3U / #EXTINF 等以 # 开头的行和空行。
 * 返回实际成功添加的曲目数。 */
int playlist_load_m3u(const wchar_t *m3u_path)
{
    if (!m3u_path || !*m3u_path)
        return 0;

    /* 取得 m3u 文件所在目录，用于解析相对路径。 */
    wchar_t dir[PATH_BUF];
    wcsncpy(dir, m3u_path, PATH_BUF - 1);
    dir[PATH_BUF - 1] = 0;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash)
        slash = wcsrchr(dir, L'/');
    if (slash)
        *slash = 0;
    else
        dir[0] = 0;

    /* 二进制读取、逐行按字节处理。cp == 0 表示编码未定（无 BOM），
     * 由第一行实际内容判定。 */
    FILE *fp = _wfopen(m3u_path, L"rb");
    if (!fp)
        return 0;

    int added = 0;
    int cp = 0;             /* 0 = 未定；否则 CP_UTF8 / CP_ACP */
    int first = 1;          /* 第一行用于 BOM 检测 */
    char raw[PATH_BUF * 3]; /* 字节行缓冲，足够容纳 PATH_BUF 个 wchar 的 UTF-8 */
    wchar_t line[PATH_BUF];

    while (fgets(raw, (int)sizeof(raw), fp)) {
        size_t blen = strlen(raw);
        if (first) {
            first = 0;
            if (blen >= 3 &&
                (unsigned char)raw[0] == 0xEF &&
                (unsigned char)raw[1] == 0xBB &&
                (unsigned char)raw[2] == 0xBF) {
                memmove(raw, raw + 3, blen - 2);  /* 跳过 BOM */
                blen -= 3;
                cp = CP_UTF8;
            }
            /* 无 BOM：cp 保持 0，进入下面的启发式判定。 */
        }

        /* 去掉行尾 \r\n（字节层面）。 */
        while (blen > 0 && (raw[blen-1] == '\n' || raw[blen-1] == '\r'))
            raw[--blen] = 0;
        if (blen == 0)
            continue;            /* 空行 */

        int n;
        if (cp == 0) {
            /* 无 BOM：先用严格 UTF-8 试解。含中文的 UTF-8 序列必然合法；
             * 失败（字节非法）则说明是 ANSI(GBK) 编码，回退代码页。 */
            n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    raw, (int)blen, line, PATH_BUF - 1);
            if (n > 0) {
                cp = CP_UTF8;
            } else {
                cp = CP_ACP;
                n = MultiByteToWideChar(CP_ACP, 0,
                                        raw, (int)blen, line, PATH_BUF - 1);
            }
            if (n <= 0)
                continue;        /* 解码失败，跳过该行 */
            line[n] = 0;
        } else {
            n = MultiByteToWideChar(cp, 0, raw, (int)blen, line, PATH_BUF - 1);
            if (n <= 0)
                continue;        /* 解码失败，跳过该行 */
            line[n] = 0;
        }
        if (line[0] == L'#')
            continue;            /* 跳过 #EXTM3U / #EXTINF */

        /* 解析路径：绝对路径直接用，相对路径拼到 m3u 目录。 */
        wchar_t full[PATH_BUF];
        if (line[1] == L':' || line[0] == L'\\' || line[0] == L'/') {
            /* 绝对路径 (X:\... 或 \\... 或 /...) */
            wcsncpy(full, line, PATH_BUF - 1);
            full[PATH_BUF - 1] = 0;
        } else if (dir[0]) {
            _snwprintf(full, PATH_BUF, L"%s\\%s", dir, line);
            full[PATH_BUF - 1] = 0;
        } else {
            wcsncpy(full, line, PATH_BUF - 1);
            full[PATH_BUF - 1] = 0;
        }
        if (playlist_add_file_fast(full) >= 0)
            added++;
    }

    fclose(fp);
    return added;
}

/* 将当前播放列表保存为 M3U 文件（UTF-8 with BOM，含 #EXTM3U 头）。
 * 路径写绝对路径，确保跨目录移动 M3U 仍可用。
 * 返回 1 成功，0 失败。 */
int playlist_save_m3u(const wchar_t *m3u_path)
{
    if (!m3u_path || !*m3u_path)
        return 0;

    FILE *fp = _wfopen(m3u_path, L"wb");
    if (!fp)
        return 0;

    /* UTF-8 BOM + #EXTM3U 头。 */
    fwrite("\xEF\xBB\xBF", 1, 3, fp);
    fwrite("#EXTM3U\n", 1, 8, fp);

    int ok = 1;
    for (int i = 0; i < g_playlist.count; i++) {
        const PlaylistEntry *e = &g_playlist.items[i];

        /* #EXTINF:<duration>,<title> */
        wchar_t wextinf[1024];
        int dur = (int)(e->duration > 0 ? e->duration : 0);
        _snwprintf(wextinf, 1024, L"#EXTINF:%d,%s\n", dur,
                   e->title ? e->title : (e->name ? e->name : L""));
        wextinf[1023] = 0;
        char extinf[4096];
        int elen = WideCharToMultiByte(CP_UTF8, 0, wextinf, -1, extinf,
                                       sizeof(extinf), NULL, NULL);
        if (elen > 0)
            fwrite(extinf, 1, (size_t)elen - 1, fp);   /* 不写结尾 \0 */

        /* 路径行 */
        char path_utf8[PATH_BUF * 3];
        int plen = WideCharToMultiByte(CP_UTF8, 0, e->path, -1, path_utf8,
                                       sizeof(path_utf8), NULL, NULL);
        if (plen > 0) {
            fwrite(path_utf8, 1, (size_t)plen - 1, fp);
            fwrite("\n", 1, 1, fp);
        } else {
            ok = 0;
            break;
        }
    }

    fclose(fp);
    return ok;
}
