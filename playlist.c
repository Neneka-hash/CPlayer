/*
 * playlist.c - Playlist storage and traversal.
 *
 * Memory model: entries are compact records pointing into a packed path
 * arena; tag metadata is loaded on demand into bounded LRU-managed blocks.
 * See playlist.h for the rationale.
 *
 * Threading model (unchanged in spirit from the original design):
 *   - The UI thread is the only writer of the playlist array and the arena.
 *     Batch imports still run on the UI thread, just in bounded time slices,
 *     so the single-writer invariant holds.
 *   - The background metadata worker only READS entries, always under
 *     tag_lock, and delivers results through WM_TAGS_LOADED; the UI thread
 *     installs them (playlist_install_meta). tag_epoch invalidates results
 *     that were posted before a remove/clear.
 */
#include "mp_player.h"
#include "playlist.h"
#include "audio_decode.h"   /* detect_format */
#include "tag_reader.h"
#include "tag_cache.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>   /* offsetof */

Playlist g_playlist;

/* Metadata worker lifecycle (internal). */
static void playlist_start_tag_thread(void);
static void playlist_stop_tag_thread(void);
static void playlist_suspend_tag_thread(void);
static void playlist_resume_tag_thread(void);

/* Publication lock: guards g_playlist.items base/count, the path arena base,
 * per-entry flag/meta writes the worker observes, and the worker's view of
 * entries. Held only for pointer/field copies, never during file I/O. */
static CRITICAL_SECTION  tag_lock;
static volatile LONG     tag_lock_ready = 0;

/* ---- Path arena ------------------------------------------------------ *
 * All paths are packed back to back into one growable wchar buffer;
 * entries store a wchar offset. This removes one heap block per track and
 * keeps the text dense for display scans. Growth (realloc) happens under
 * tag_lock because the worker copies path text under the same lock. */
static wchar_t  *g_arena;
static uint32_t  g_arena_len;    /* used, in wchar units */
static uint32_t  g_arena_cap;

/* ---- Duplicate index ------------------------------------------------- *
 * Open-addressed hash of paths -> entry index, so a batch import of n files
 * costs O(n) lookups instead of the old O(n^2) full-scan compare. */
static uint32_t *g_dedup_hash;   /* meaningful only where g_dedup_idx != 0 */
static uint32_t *g_dedup_idx;    /* entry index + 1; 0 = empty slot        */
static int       g_dedup_mask;   /* capacity - 1 (capacity is a power of 2) */
static int       g_dedup_dirty;  /* rebuild before next use (after removes) */

/* ---- Metadata LRU ---------------------------------------------------- */
#define META_CAP 2048            /* bounded metadata working set           */
static EntryMeta   *g_meta_head; /* MRU */
static EntryMeta   *g_meta_tail; /* LRU */
static int          g_meta_count;
static EntryMeta   *g_meta_pin;  /* never evict (currently playing track)  */

/* ====================================================================== */
/*  Small helpers                                                          */
/* ====================================================================== */

static void lock_init(void)
{
    if (InterlockedCompareExchange(&tag_lock_ready, 1, 0) == 0) {
        InitializeCriticalSection(&tag_lock);
    }
}

static void lock(void)   { EnterCriticalSection(&tag_lock); }
static void unlock(void) { LeaveCriticalSection(&tag_lock); }

/* FNV-1a over the wide string, ASCII case-folded so that path lookups stay
 * case-insensitive like _wcsicmp (non-ASCII case variants may hash apart;
 * the equality compare afterwards is authoritative). */
static uint32_t path_hash(const wchar_t *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        wchar_t c = *s;
        if (c >= L'A' && c <= L'Z') c += 32;
        h ^= (uint16_t)c;
        h *= 16777619u;
    }
    return h ? h : 1;   /* never 0: slot 0 marks empty in the dedup table */
}

/* ---- Path arena ------------------------------------------------------ */

static const wchar_t *entry_path(const PlaylistEntry *e)
{
    return g_arena + e->path_off;
}

/* Ensure `extra` more wchar slots fit. Runs under tag_lock because the
 * realloc can move the buffer the worker reads from. */
static int arena_ensure(uint32_t extra)
{
    if (g_arena_len + extra <= g_arena_cap)
        return 1;
    uint32_t cap = g_arena_cap ? g_arena_cap : 4096;
    while (cap < g_arena_len + extra)
        cap *= 2;
    lock();
    wchar_t *p = (wchar_t *)realloc(g_arena, (size_t)cap * sizeof(wchar_t));
    int ok = 0;
    if (p) {
        g_arena = p;
        g_arena_cap = cap;
        ok = 1;
    }
    unlock();
    return ok;
}

/* Append `path` to the arena; returns its wchar offset, or -1 on failure. */
static int arena_append(const wchar_t *path, uint16_t *out_len)
{
    size_t len = wcslen(path);
    if (len >= PL_PATH_MAX)
        len = PL_PATH_MAX - 1;
    if (!arena_ensure((uint32_t)len + 1))
        return -1;
    uint32_t off = g_arena_len;
    memcpy(g_arena + off, path, len * sizeof(wchar_t));
    g_arena[off + len] = 0;
    g_arena_len = off + (uint32_t)len + 1;
    if (out_len) *out_len = (uint16_t)len;
    return (int)off;
}

/* Rebuild the arena from the surviving entries, dropping the holes left by
 * removals. O(total path bytes); only worth it after heavy deletion. */
static void arena_compact(void)
{
    uint32_t live = 0;
    for (int i = 0; i < g_playlist.count; i++)
        live += g_playlist.items[i].path_len + 1;
    if (live == 0) {
        g_arena_len = 0;
        return;
    }
    if (g_arena_len < live + g_arena_len / 3)
        return;   /* less than a third wasted: keep the arena as is */

    wchar_t *dst = (wchar_t *)malloc((size_t)live * sizeof(wchar_t));
    if (!dst)
        return;
    uint32_t at = 0;
    for (int i = 0; i < g_playlist.count; i++) {
        PlaylistEntry *e = &g_playlist.items[i];
        memcpy(dst + at, g_arena + e->path_off,
               (size_t)(e->path_len + 1) * sizeof(wchar_t));
        e->path_off = at;
        at += e->path_len + 1;
    }
    lock();
    free(g_arena);
    g_arena = dst;
    g_arena_len = at;
    g_arena_cap = live;
    unlock();
}

/* ====================================================================== */
/*  Duplicate index                                                        */
/* ====================================================================== */

static int  g_dedup_used;

static void dedup_insert_raw(int index)
{
    const wchar_t *p = entry_path(&g_playlist.items[index]);
    uint32_t h = path_hash(p);
    int slot = (int)(h & (uint32_t)g_dedup_mask);
    while (g_dedup_idx[slot] != 0) {
        if (g_dedup_hash[slot] == h &&
            _wcsicmp(entry_path(&g_playlist.items[g_dedup_idx[slot] - 1]),
                     p) == 0)
            return;   /* already indexed */
        slot = (slot + 1) & g_dedup_mask;
    }
    g_dedup_hash[slot] = h;
    g_dedup_idx[slot]  = (uint32_t)index + 1;
    g_dedup_used++;
}

/* Rebuild from the live entries (also applies pending removals). */
static void dedup_rebuild(void)
{
    if (g_dedup_mask < 0)
        return;
    for (int i = 0; i <= g_dedup_mask; i++)
        g_dedup_idx[i] = 0;
    g_dedup_used = 0;
    for (int i = 0; i < g_playlist.count; i++)
        dedup_insert_raw(i);
    g_dedup_dirty = 0;
}

/* Resize to hold the current entries at < 70% load. */
static void dedup_grow(void)
{
    int want = 16;
    while (want < (g_playlist.count + 1) * 2)
        want *= 2;
    uint32_t *nh = (uint32_t *)malloc((size_t)want * sizeof(uint32_t));
    uint32_t *ni = (uint32_t *)calloc((size_t)want, sizeof(uint32_t));
    if (!nh || !ni) {
        free(nh);
        free(ni);
        return;   /* keep the old table; a missed duplicate is non-fatal */
    }
    free(g_dedup_hash);
    free(g_dedup_idx);
    g_dedup_hash = nh;
    g_dedup_idx  = ni;
    g_dedup_mask = want - 1;
    dedup_rebuild();
}

static void dedup_insert(int index)
{
    if (g_dedup_mask <= 0 || g_dedup_dirty)
        dedup_grow();
    if (g_dedup_mask <= 0)
        return;
    if ((g_dedup_used + 1) * 10 >= (g_dedup_mask + 1) * 7)
        dedup_grow();
    dedup_insert_raw(index);
}

static void dedup_ensure(void)
{
    if (g_dedup_dirty || g_dedup_mask <= 0)
        dedup_grow();
}

/* Entry index for `path`, or -1. */
static int dedup_find(const wchar_t *path)
{
    dedup_ensure();
    if (g_dedup_mask <= 0)
        return -1;
    uint32_t h = path_hash(path);
    int slot = (int)(h & (uint32_t)g_dedup_mask);
    while (g_dedup_idx[slot] != 0) {
        if (g_dedup_hash[slot] == h &&
            _wcsicmp(entry_path(&g_playlist.items[g_dedup_idx[slot] - 1]),
                     path) == 0)
            return (int)g_dedup_idx[slot] - 1;
        slot = (slot + 1) & g_dedup_mask;
    }
    return -1;
}

/* ====================================================================== */
/*  Metadata blocks + LRU                                                  */
/* ====================================================================== */

static const wchar_t *meta_str(const EntryMeta *m, int which)
{
    const wchar_t *title  = m->strs;
    const wchar_t *artist = title + m->len_title + 1;
    const wchar_t *album  = artist + m->len_artist + 1;
    if (which == 0) return m->len_title  ? title  : NULL;
    if (which == 1) return m->len_artist ? artist : NULL;
    return m->len_album ? album : NULL;
}

static void meta_unlink(EntryMeta *m)
{
    if (m->lru_prev) m->lru_prev->lru_next = m->lru_next;
    else             g_meta_head = m->lru_next;
    if (m->lru_next) m->lru_next->lru_prev = m->lru_prev;
    else             g_meta_tail = m->lru_prev;
    m->lru_prev = m->lru_next = NULL;
    g_meta_count--;
}

static void meta_push_head(EntryMeta *m)
{
    m->lru_prev = NULL;
    m->lru_next = g_meta_head;
    if (g_meta_head) g_meta_head->lru_prev = m;
    g_meta_head = m;
    if (!g_meta_tail) g_meta_tail = m;
    g_meta_count++;
}

static void meta_touch(EntryMeta *m)
{
    if (m == g_meta_head)
        return;
    meta_unlink(m);
    meta_push_head(m);
}

/* Free one block and drop the entry's reference to it. Caller holds no
 * assumptions about LRU position. */
static void meta_release(EntryMeta *m)
{
    meta_unlink(m);
    if (m->owner) {
        lock();
        if (m->owner->meta == m)
            m->owner->meta = NULL;
        unlock();
    }
    if (m == g_meta_pin)
        g_meta_pin = NULL;
    free(m);
}

/* Trim the working set down to META_CAP. */
static void meta_evict(void)
{
    while (g_meta_count > META_CAP && g_meta_tail) {
        EntryMeta *victim = g_meta_tail;
        while (victim && (victim == g_meta_pin || victim->owner == NULL))
            victim = victim->lru_prev;
        if (!victim)
            break;
        meta_release(victim);
    }
}

static void entry_free_meta(PlaylistEntry *e)
{
    if (e->meta) {
        meta_release(e->meta);
        e->meta = NULL;
    }
}

/* Entry pointers move whenever items[] reallocs or compacts; refresh the
 * back-pointers the LRU eviction path uses. */
static void items_moved(void)
{
    for (int i = 0; i < g_playlist.count; i++) {
        if (g_playlist.items[i].meta)
            g_playlist.items[i].meta->owner = &g_playlist.items[i];
    }
}

int playlist_install_meta(int index, TagInfo *ti)
{
    if (index < 0 || index >= g_playlist.count || !ti)
        return 0;

    size_t lt = ti->title  ? wcslen(ti->title)  : 0;
    size_t la = ti->artist ? wcslen(ti->artist) : 0;
    size_t lb = ti->album  ? wcslen(ti->album)  : 0;
    if (lt > 4095) lt = 4095;
    if (la > 4095) la = 4095;
    if (lb > 4095) lb = 4095;

    size_t bytes = offsetof(EntryMeta, strs) +
                   (lt + 1 + la + 1 + lb + 1) * sizeof(wchar_t);
    EntryMeta *m = (EntryMeta *)calloc(1, bytes);
    if (!m)
        return 0;

    wchar_t *w = m->strs;
    if (lt) { memcpy(w, ti->title,  lt * sizeof(wchar_t)); }
    w[lt] = 0;
    if (la) { memcpy(w + lt + 1, ti->artist, la * sizeof(wchar_t)); }
    w[lt + 1 + la] = 0;
    if (lb) { memcpy(w + lt + 1 + la + 1, ti->album, lb * sizeof(wchar_t)); }
    w[lt + 1 + la + 1 + lb] = 0;
    m->len_title  = (uint16_t)lt;
    m->len_artist = (uint16_t)la;
    m->len_album  = (uint16_t)lb;
    m->duration   = ti->duration;
    m->bitrate    = ti->bitrate;
    m->sample_rate = ti->sample_rate;
    m->channels   = ti->channels;

    PlaylistEntry *e = &g_playlist.items[index];
    lock();
    if (e->meta) {          /* a concurrent fill already won; keep that one */
        unlock();
        free(m);
        return 0;
    }
    m->owner  = e;
    e->meta   = m;
    e->flags &= (uint8_t)~PLF_WANTED;
    unlock();

    meta_push_head(m);
    meta_evict();
    return 1;
}

void playlist_meta_request(int index)
{
    if (index < 0 || index >= g_playlist.count)
        return;
    PlaylistEntry *e = &g_playlist.items[index];
    lock();
    if (e->meta) {
        EntryMeta *m = e->meta;
        unlock();
        meta_touch(m);
        return;
    }
    e->flags |= PLF_WANTED;
    unlock();

    if (!InterlockedExchangeAdd(&g_playlist.tag_suspended, 0)) {
        playlist_start_tag_thread();
        if (g_playlist.tag_wake)
            SetEvent(g_playlist.tag_wake);
    }
}

void playlist_pin_meta(int index)
{
    if (index < 0 || index >= g_playlist.count) {
        g_meta_pin = NULL;
        return;
    }
    g_meta_pin = g_playlist.items[index].meta;
}

void playlist_meta_load_sync(int index)
{
    if (index < 0 || index >= g_playlist.count)
        return;
    wchar_t path[PL_PATH_MAX];
    int fmt;
    uint32_t h;
    uint64_t fsize;
    lock();
    PlaylistEntry *e = &g_playlist.items[index];
    if (e->meta) {
        unlock();
        return;
    }
    wcsncpy(path, entry_path(e), PL_PATH_MAX - 1);
    path[PL_PATH_MAX - 1] = 0;
    fmt = e->format;
    fsize = e->file_size;
    unlock();
    h = path_hash(path);

    TagInfo ti;
    memset(&ti, 0, sizeof(ti));
    if (!tag_cache_lookup(h, fsize, &ti)) {
        if (tag_read(path, fmt, &ti) != 0)
            memset(&ti, 0, sizeof(ti));
        else
            tag_cache_store(h, fsize, &ti);
    }
    playlist_install_meta(index, &ti);
    tag_info_free(&ti);
}

void playlist_row_view(int index, PlaylistRowView *v,
                       wchar_t *namebuf, int namecap)
{
    memset(v, 0, sizeof(*v));
    v->title  = L"";
    v->artist = L"-";
    v->album  = L"-";
    if (namebuf && namecap > 0)
        namebuf[0] = 0;
    if (index < 0 || index >= g_playlist.count)
        return;

    PlaylistEntry *e = &g_playlist.items[index];
    v->format    = e->format;
    v->file_size = e->file_size;
    if (namebuf && namecap > 0) {
        playlist_display_name(index, namebuf, namecap);
        if (namebuf[0])
            v->title = namebuf;
    }
    EntryMeta *m = e->meta;
    if (!m)
        return;

    v->has_meta    = 1;
    v->duration    = m->duration;
    v->bitrate     = m->bitrate;
    v->sample_rate = m->sample_rate;
    v->channels    = m->channels;
    const wchar_t *t = meta_str(m, 0);
    const wchar_t *a = meta_str(m, 1);
    const wchar_t *b = meta_str(m, 2);
    if (t && *t) v->title  = t;
    if (a && *a) v->artist = a;
    if (b && *b) v->album  = b;
}

/* ====================================================================== */
/*  Container lifecycle                                                    */
/* ====================================================================== */

void playlist_init(void)
{
    lock_init();
    tag_cache_open();
    g_playlist.items      = NULL;
    g_playlist.count      = 0;
    g_playlist.capacity   = 0;
    g_playlist.tag_thread  = NULL;
    g_playlist.tag_wake    = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_playlist.tag_cancel  = 0;
    g_playlist.tag_running = 0;
    g_playlist.tag_suspended = 0;
    g_playlist.tag_epoch   = 0;
}

void playlist_free(void)
{
    playlist_stop_tag_thread();
    playlist_import_abort();
    playlist_clear();
    free(g_arena);
    g_arena = NULL;
    g_arena_len = g_arena_cap = 0;
    free(g_dedup_hash);
    free(g_dedup_idx);
    g_dedup_hash = NULL;
    g_dedup_idx  = NULL;
    g_dedup_mask = 0;
    if (g_playlist.tag_wake) {
        CloseHandle(g_playlist.tag_wake);
        g_playlist.tag_wake = NULL;
    }
    tag_cache_close();
    if (InterlockedExchange(&tag_lock_ready, 0) != 0)
        DeleteCriticalSection(&tag_lock);
}

/* Grow the entry array so one more entry fits. */
static int ensure_capacity(void)
{
    if (g_playlist.count < g_playlist.capacity)
        return 1;
    int newcap = g_playlist.capacity ? g_playlist.capacity * 2 : 256;
    PlaylistEntry *p = (PlaylistEntry *)realloc(
        g_playlist.items, (size_t)newcap * sizeof(PlaylistEntry));
    if (!p)
        return 0;
    /* Publish the new base inside the lock: the metadata worker reads
     * entries[] under the same lock and must never see a freed base. */
    lock();
    g_playlist.items    = p;
    g_playlist.capacity = newcap;
    unlock();
    items_moved();
    return 1;
}

/* ---- Adding ---------------------------------------------------------- */

int playlist_find(const wchar_t *path)
{
    if (!path || !*path)
        return -1;
    return dedup_find(path);
}

int playlist_add_file_fast(const wchar_t *path)
{
    if (!path || !*path)
        return -1;

    int fmt = detect_format(path);
    if (fmt == FMT_UNKNOWN)
        return -1;

    if (dedup_find(path) >= 0)
        return -1;   /* duplicate: not added */

    if (!ensure_capacity())
        return -1;

    PlaylistEntry *e = &g_playlist.items[g_playlist.count];
    memset(e, 0, sizeof(*e));
    e->format = (uint8_t)fmt;

    uint16_t plen = 0;
    int off = arena_append(path, &plen);
    if (off < 0)
        return -1;
    e->path_off = (uint32_t)off;
    e->path_len = plen;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
        e->file_size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;

    /* Publish: the whole entry is written before the count is bumped, so the
     * worker never observes a half-initialised slot. */
    lock();
    int idx = g_playlist.count++;
    unlock();

    dedup_insert(idx);
    return idx;
}

int playlist_add_file(const wchar_t *path)
{
    int idx = playlist_add_file_fast(path);
    if (idx < 0) {
        idx = dedup_find(path);   /* already present: return its index */
        return idx;
    }
    playlist_meta_load_sync(idx);
    return idx;
}

/* ---- Display helpers -------------------------------------------------- */

void playlist_make_name(const wchar_t *path, wchar_t *out, int cap)
{
    if (!out || cap <= 0)
        return;
    out[0] = 0;

    const wchar_t *base = path;
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    if (base == path && path[0] && path[1] == L':')
        base = path + 2;

    int len = 0;
    for (const wchar_t *p = base; *p && len < cap - 1; p++)
        out[len++] = *p;
    out[len] = 0;

    for (int i = len - 1; i >= 0; i--) {
        if (out[i] == L'.') {
            out[i] = 0;
            break;
        }
    }
}

int playlist_display_name(int index, wchar_t *out, int cap)
{
    if (index < 0 || index >= g_playlist.count) {
        if (out && cap > 0) out[0] = 0;
        return 0;
    }
    playlist_make_name(entry_path(&g_playlist.items[index]), out, cap);
    return out ? (int)wcslen(out) : 0;
}

wchar_t *playlist_dup_path(int index)
{
    if (index < 0 || index >= g_playlist.count)
        return NULL;
    const wchar_t *p = entry_path(&g_playlist.items[index]);
    return _wcsdup(p);
}

/* ---- Removing -------------------------------------------------------- */

void playlist_remove_indices(const int *indices, int n)
{
    if (!indices || n <= 0 || g_playlist.count == 0)
        return;

    /* Stale WM_TAGS_LOADED messages carry indices from before the shift. */
    InterlockedIncrement(&g_playlist.tag_epoch);
    playlist_suspend_tag_thread();

    /* Mark survivors in a bitmap, then compact in a single pass. */
    int total = g_playlist.count;
    unsigned char *kill = (unsigned char *)calloc((size_t)total, 1);
    if (!kill) {
        playlist_resume_tag_thread();
        return;
    }
    int killed = 0;
    for (int k = 0; k < n; k++) {
        int i = indices[k];
        if (i < 0 || i >= total || kill[i])
            continue;
        kill[i] = 1;
        killed++;
    }
    if (killed == 0) {
        free(kill);
        playlist_resume_tag_thread();
        return;
    }

    int w = 0;
    for (int r = 0; r < total; r++) {
        if (kill[r]) {
            entry_free_meta(&g_playlist.items[r]);
            continue;
        }
        if (w != r)
            g_playlist.items[w] = g_playlist.items[r];
        w++;
    }
    memset(&g_playlist.items[w], 0, sizeof(PlaylistEntry) * (size_t)(total - w));
    g_playlist.count = w;
    free(kill);

    items_moved();
    arena_compact();
    g_dedup_dirty = 1;

    playlist_resume_tag_thread();
}

void playlist_remove_at(int index)
{
    if (index < 0 || index >= g_playlist.count)
        return;
    playlist_remove_indices(&index, 1);
}

void playlist_clear(void)
{
    InterlockedIncrement(&g_playlist.tag_epoch);
    playlist_stop_tag_thread();
    playlist_import_abort();
    for (int i = 0; i < g_playlist.count; i++)
        entry_free_meta(&g_playlist.items[i]);
    free(g_playlist.items);
    g_playlist.items    = NULL;
    g_playlist.count    = 0;
    g_playlist.capacity = 0;
    g_arena_len = 0;
    g_dedup_dirty = 1;
    g_meta_pin = NULL;
}

/* ---- Traversal -------------------------------------------------------- */

static void shuffle_seed_once(void)
{
    static volatile int seeded = 0;
    if (!seeded) {
        srand((unsigned)GetTickCount64() ^ (unsigned)(uintptr_t)&seeded);
        seeded = 1;
    }
}

int playlist_next_index(int cur, int mode)
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
        int nxt;
        do {
            int r;
            do { r = rand(); } while (r >= RAND_MAX - RAND_MAX % n);
            nxt = r % n;
        } while (nxt == cur);
        return nxt;
    }
    case MODE_LIST_LOOP:
    default:
        return (cur < 0) ? 0 : (cur + 1) % n;
    }
}

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

/* ====================================================================== */
/*  Background metadata worker                                            */
/* ====================================================================== */

/* Claim one work item: copy the path out under the lock and clear the
 * WANTED flag so the same entry is not loaded twice. Returns 0 if entry i
 * has nothing to do. */
static int tag_take_work(int i, wchar_t *path, int cap, int *fmt,
                         uint32_t *hash, uint64_t *fsize)
{
    int ok = 0;
    lock();
    if (i >= 0 && i < g_playlist.count) {
        PlaylistEntry *e = &g_playlist.items[i];
        if ((e->flags & PLF_WANTED) && !e->meta) {
            e->flags &= (uint8_t)~PLF_WANTED;
            const wchar_t *src = entry_path(e);
            wcsncpy(path, src, cap - 1);
            path[cap - 1] = 0;
            *fmt   = e->format;
            *fsize = e->file_size;
            *hash  = path_hash(src);
            ok = 1;
        }
    }
    unlock();
    return ok;
}

static void tag_release_work(int i)
{
    /* The result could not be delivered: let a later pass retry. */
    lock();
    if (i >= 0 && i < g_playlist.count)
        g_playlist.items[i].flags |= PLF_WANTED;
    unlock();
}

static DWORD WINAPI tag_worker_thread(LPVOID param)
{
    (void)param;
    for (;;) {
        if (InterlockedExchangeAdd(&g_playlist.tag_cancel, 0))
            break;

        int loaded = 0;
        int n = 0;
        lock();
        n = g_playlist.count;
        unlock();

        for (int i = 0; i < n; i++) {
            if (InterlockedExchangeAdd(&g_playlist.tag_cancel, 0))
                return 0;
            /* The count grows while the pass runs; pick up new entries. */
            lock();
            n = g_playlist.count;
            unlock();

            wchar_t path[PL_PATH_MAX];
            int fmt = 0;
            uint32_t h = 0;
            uint64_t fsize = 0;
            if (!tag_take_work(i, path, PL_PATH_MAX, &fmt, &h, &fsize))
                continue;

            TagInfo ti;
            memset(&ti, 0, sizeof(ti));
            if (tag_cache_lookup(h, fsize, &ti)) {
                /* cache hit: strings already heap-allocated */
            } else if (tag_read(path, fmt, &ti) == 0) {
                tag_cache_store(h, fsize, &ti);
            } else {
                memset(&ti, 0, sizeof(ti));   /* empty result: stop retrying */
            }
            ti.epoch = InterlockedExchangeAdd(&g_playlist.tag_epoch, 0);

            TagInfo *heap = (TagInfo *)malloc(sizeof(TagInfo));
            if (heap) {
                *heap = ti;
                if (PostMessage(g_player.hMain, WM_TAGS_LOADED,
                                (WPARAM)i, (LPARAM)heap)) {
                    loaded = 1;
                    continue;
                }
                tag_info_free(heap);
                free(heap);
            } else {
                tag_info_free(&ti);
            }
            tag_release_work(i);
        }

        if (!loaded) {
            /* Nothing to do: sleep until the UI pokes us or we are stopped. */
            WaitForSingleObject(g_playlist.tag_wake, 250);
        }
    }
    return 0;
}

static void playlist_start_tag_thread(void)
{
    if (InterlockedExchangeAdd(&g_playlist.tag_running, 0)) {
        if (g_playlist.tag_wake)
            SetEvent(g_playlist.tag_wake);
        return;
    }
    if (g_playlist.tag_thread) {
        WaitForSingleObject(g_playlist.tag_thread, INFINITE);
        CloseHandle(g_playlist.tag_thread);
        g_playlist.tag_thread = NULL;
    }
    InterlockedExchange(&g_playlist.tag_cancel, 0);

    HANDLE h = CreateThread(NULL, 0, tag_worker_thread, NULL, 0, NULL);
    if (h) {
        g_playlist.tag_thread = h;
        InterlockedExchange(&g_playlist.tag_running, 1);
    }
}

static void playlist_stop_tag_thread(void)
{
    InterlockedExchange(&g_playlist.tag_cancel, 1);
    InterlockedExchange(&g_playlist.tag_running, 0);
    if (g_playlist.tag_wake)
        SetEvent(g_playlist.tag_wake);
    if (g_playlist.tag_thread) {
        WaitForSingleObject(g_playlist.tag_thread, INFINITE);
        CloseHandle(g_playlist.tag_thread);
        g_playlist.tag_thread = NULL;
    }
    InterlockedExchange(&g_playlist.tag_cancel, 0);
}

static void playlist_suspend_tag_thread(void)
{
    InterlockedExchange(&g_playlist.tag_suspended, 1);
    playlist_stop_tag_thread();
}

static void playlist_resume_tag_thread(void)
{
    InterlockedExchange(&g_playlist.tag_suspended, 0);
}

/* ====================================================================== */
/*  Chunked import session                                                 */
/* ====================================================================== */
/* Folder walks and M3U loads run in bounded time slices driven by a UI
 * timer (playlist_import_step), so dropping a 100k-file tree never freezes
 * the window. All state is touched on the UI thread only. */

#define ENUM_MAX_DEPTH 32

typedef struct {
    int      depth;
    wchar_t  path[PL_PATH_MAX];
} ImpDir;

typedef struct {
    int      active;
    ImpDir  *dirs;          /* pending directory queue */
    int      ndirs, dirs_cap;
    HANDLE   find;          /* active FindFirstFile scan, or NULL */
    WIN32_FIND_DATAW fd;
    int      have;          /* fd holds a pending entry */
    wchar_t  find_dir[PL_PATH_MAX];
    int      find_depth;    /* tree depth of find_dir */
    FILE    *m3u;           /* active M3U parse, or NULL */
    int      m3u_cp, m3u_first;
    wchar_t  m3u_dir[PL_PATH_MAX];
    int      added;
} ImportSession;

static ImportSession g_imp;

static void imp_push_dir(const wchar_t *path, int depth)
{
    if (g_imp.ndirs == g_imp.dirs_cap) {
        int cap = g_imp.dirs_cap ? g_imp.dirs_cap * 2 : 32;
        ImpDir *p = (ImpDir *)realloc(g_imp.dirs, (size_t)cap * sizeof(ImpDir));
        if (!p)
            return;
        g_imp.dirs = p;
        g_imp.dirs_cap = cap;
    }
    ImpDir *d = &g_imp.dirs[g_imp.ndirs++];
    d->depth = depth;
    wcsncpy(d->path, path, PL_PATH_MAX - 1);
    d->path[PL_PATH_MAX - 1] = 0;
}

static void imp_close_find(void)
{
    if (g_imp.find) {
        FindClose(g_imp.find);
        g_imp.find = NULL;
    }
    g_imp.have = 0;
}

void playlist_import_abort(void)
{
    imp_close_find();
    if (g_imp.m3u) {
        fclose(g_imp.m3u);
        g_imp.m3u = NULL;
    }
    free(g_imp.dirs);
    g_imp.dirs = NULL;
    g_imp.ndirs = g_imp.dirs_cap = 0;
    g_imp.active = 0;
    g_imp.added = 0;
}

int playlist_import_active(void)
{
    return g_imp.active;
}

int playlist_import_added(void)
{
    return g_imp.added;
}

void playlist_import_path(const wchar_t *path)
{
    if (!path || !*path)
        return;
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES)
        return;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        imp_push_dir(path, 0);
        g_imp.active = 1;
    } else {
        if (playlist_add_file_fast(path) >= 0)
            g_imp.added++;
    }
}

int playlist_import_m3u(const wchar_t *m3u_path)
{
    if (!m3u_path || !*m3u_path)
        return 0;
    if (g_imp.m3u) {
        fclose(g_imp.m3u);
        g_imp.m3u = NULL;
    }
    FILE *fp = _wfopen(m3u_path, L"rb");
    if (!fp)
        return 0;

    wcsncpy(g_imp.m3u_dir, m3u_path, PL_PATH_MAX - 1);
    g_imp.m3u_dir[PL_PATH_MAX - 1] = 0;
    wchar_t *slash = wcsrchr(g_imp.m3u_dir, L'\\');
    if (!slash)
        slash = wcsrchr(g_imp.m3u_dir, L'/');
    if (slash) *slash = 0; else g_imp.m3u_dir[0] = 0;

    g_imp.m3u      = fp;
    g_imp.m3u_cp   = 0;
    g_imp.m3u_first = 1;
    g_imp.active   = 1;
    return 1;
}

/* Read and add one M3U line. Encoding detection: UTF-8 BOM, else strict
 * UTF-8 test on the first content line, else the ANSI code page (GBK on a
 * Chinese system). Relative paths resolve against the M3U's directory. */
static void imp_m3u_step(void)
{
    char raw[PL_PATH_MAX * 3];
    wchar_t line[PL_PATH_MAX];

    if (!fgets(raw, (int)sizeof(raw), g_imp.m3u))
        return;   /* EOF handled by the caller on the next call */

    size_t blen = strlen(raw);
    if (g_imp.m3u_first) {
        g_imp.m3u_first = 0;
        if (blen >= 3 &&
            (unsigned char)raw[0] == 0xEF &&
            (unsigned char)raw[1] == 0xBB &&
            (unsigned char)raw[2] == 0xBF) {
            memmove(raw, raw + 3, blen - 2);
            blen -= 3;
            g_imp.m3u_cp = CP_UTF8;
        }
    }
    while (blen > 0 && (raw[blen-1] == '\n' || raw[blen-1] == '\r'))
        raw[--blen] = 0;
    if (blen == 0)
        return;

    int n;
    if (g_imp.m3u_cp == 0) {
        n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                raw, (int)blen, line, PL_PATH_MAX - 1);
        if (n > 0) {
            g_imp.m3u_cp = CP_UTF8;
        } else {
            g_imp.m3u_cp = CP_ACP;
            n = MultiByteToWideChar(CP_ACP, 0, raw, (int)blen,
                                    line, PL_PATH_MAX - 1);
        }
    } else {
        n = MultiByteToWideChar(g_imp.m3u_cp, 0, raw, (int)blen,
                                line, PL_PATH_MAX - 1);
    }
    if (n <= 0)
        return;
    line[n] = 0;
    if (line[0] == L'#')
        return;

    wchar_t full[PL_PATH_MAX];
    if (line[1] == L':' || line[0] == L'\\' || line[0] == L'/') {
        wcsncpy(full, line, PL_PATH_MAX - 1);
        full[PL_PATH_MAX - 1] = 0;
    } else if (g_imp.m3u_dir[0]) {
        _snwprintf(full, PL_PATH_MAX, L"%s\\%s", g_imp.m3u_dir, line);
        full[PL_PATH_MAX - 1] = 0;
    } else {
        wcsncpy(full, line, PL_PATH_MAX - 1);
        full[PL_PATH_MAX - 1] = 0;
    }
    if (playlist_add_file_fast(full) >= 0)
        g_imp.added++;
}

/* Process one pending directory entry (found in g_imp.fd). */
static void imp_dir_entry(void)
{
    const WIN32_FIND_DATAW *fd = &g_imp.fd;
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (wcscmp(fd->cFileName, L".") == 0 ||
            wcscmp(fd->cFileName, L"..") == 0)
            return;
        if (fd->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            return;   /* junction loops would recurse forever */
        if (g_imp.find_depth + 1 > ENUM_MAX_DEPTH)
            return;
        wchar_t sub[PL_PATH_MAX];
        _snwprintf(sub, PL_PATH_MAX, L"%s\\%s", g_imp.find_dir, fd->cFileName);
        sub[PL_PATH_MAX - 1] = 0;
        imp_push_dir(sub, g_imp.find_depth + 1);
    } else {
        wchar_t path[PL_PATH_MAX];
        _snwprintf(path, PL_PATH_MAX, L"%s\\%s", g_imp.find_dir, fd->cFileName);
        path[PL_PATH_MAX - 1] = 0;
        if (playlist_add_file_fast(path) >= 0)
            g_imp.added++;
    }
}

int playlist_import_step(int budget_ms)
{
    if (!g_imp.active)
        return 0;

    ULONGLONG t0 = GetTickCount64();
    do {
        if (g_imp.find) {
            if (g_imp.have) {
                imp_dir_entry();
                g_imp.have = FindNextFileW(g_imp.find, &g_imp.fd);
                if (!g_imp.have)
                    imp_close_find();
            } else {
                imp_close_find();
            }
        } else if (g_imp.ndirs > 0) {
            ImpDir d = g_imp.dirs[--g_imp.ndirs];
            wchar_t pattern[PL_PATH_MAX];
            _snwprintf(pattern, PL_PATH_MAX, L"%s\\*", d.path);
            pattern[PL_PATH_MAX - 1] = 0;
            wcsncpy(g_imp.find_dir, d.path, PL_PATH_MAX - 1);
            g_imp.find_dir[PL_PATH_MAX - 1] = 0;
            g_imp.find_depth = d.depth;
            g_imp.find = FindFirstFileW(pattern, &g_imp.fd);
            if (g_imp.find == INVALID_HANDLE_VALUE) {
                g_imp.find = NULL;
                g_imp.have = 0;
            } else {
                g_imp.have = 1;
            }
        } else if (g_imp.m3u) {
            imp_m3u_step();
            if (feof(g_imp.m3u)) {
                fclose(g_imp.m3u);
                g_imp.m3u = NULL;
            }
        } else {
            g_imp.active = 0;
        }
    } while (g_imp.active && GetTickCount64() - t0 < (ULONGLONG)budget_ms);

    return g_imp.active;
}

/* ====================================================================== */
/*  M3U save                                                               */
/* ====================================================================== */

/* Write the current playlist as M3U (UTF-8 with BOM, absolute paths).
 * #EXTINF uses loaded metadata where available and the file name otherwise,
 * because metadata is loaded lazily. Returns 1 on success. */
int playlist_save_m3u(const wchar_t *m3u_path)
{
    if (!m3u_path || !*m3u_path)
        return 0;

    FILE *fp = _wfopen(m3u_path, L"wb");
    if (!fp)
        return 0;

    fwrite("\xEF\xBB\xBF", 1, 3, fp);
    fwrite("#EXTM3U\n", 1, 8, fp);

    int ok = 1;
    for (int i = 0; i < g_playlist.count; i++) {
        PlaylistRowView v;
        wchar_t name[PL_PATH_MAX];
        playlist_row_view(i, &v, name, PL_PATH_MAX);

        wchar_t wextinf[1024];
        int dur = (int)(v.duration > 0 ? v.duration : 0);
        _snwprintf(wextinf, 1024, L"#EXTINF:%d,%s\n", dur, v.title);
        wextinf[1023] = 0;
        char extinf[4096];
        int elen = WideCharToMultiByte(CP_UTF8, 0, wextinf, -1, extinf,
                                       sizeof(extinf), NULL, NULL);
        if (elen > 0)
            fwrite(extinf, 1, (size_t)elen - 1, fp);

        char path_utf8[PL_PATH_MAX * 3];
        const wchar_t *wpath = entry_path(&g_playlist.items[i]);
        int plen = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path_utf8,
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
