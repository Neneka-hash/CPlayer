/*
 * tag_cache.c - Persistent on-disk tag metadata cache.
 *
 * File layout (little-endian, packed):
 *   header: magic 'CPTC' (4 bytes) + version u32
 *   records: TCRec (packed fixed part) followed by three NUL-terminated
 *            UTF-16 strings (title / artist / album), each of len+1 wchars.
 *
 * The in-memory index holds {path hash, record offset} only (16 bytes per
 * record), so a 100k-track cache costs a couple of MB of RAM while the
 * strings stay on disk and are read on demand.
 */
#include "mp_player.h"
#include "tag_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>       /* _chsize_s, _fileno */

#define TC_MAGIC   0x43545043u   /* 'CPTC' */
#define TC_VERSION 1u
#define TC_MAX_STR 4096          /* max wchars per stored string */
#define TC_MAX_BYTES  (64u * 1024u * 1024u)   /* reset the file above this */

#pragma pack(push, 1)
typedef struct {
    uint64_t hash;
    uint64_t file_size;
    double   duration;
    int32_t  bitrate;
    int32_t  sample_rate;
    int32_t  channels;
    uint16_t len_title, len_artist, len_album;
} TCRec;
#pragma pack(pop)

static CRITICAL_SECTION tc_cs;
static int    tc_ready = 0;
static FILE  *tc_fp;

/* Index: open addressing over (hash, offset) pairs. */
static uint64_t *tc_hash;
static uint64_t *tc_off;
static int       tc_mask;      /* capacity - 1, power of two */
static int       tc_used;

static void tc_index_insert(uint64_t hash, uint64_t offset);

static void tc_index_grow(void)
{
    int want = 256;
    while (want < (tc_used + 1) * 2)
        want *= 2;
    uint64_t *nh = (uint64_t *)malloc((size_t)want * sizeof(uint64_t));
    uint64_t *no = (uint64_t *)malloc((size_t)want * sizeof(uint64_t));
    if (!nh || !no) {
        free(nh);
        free(no);
        return;
    }
    uint64_t *oh = tc_hash, *oo = tc_off;
    int omask = tc_mask, oused = tc_used;
    tc_hash = nh;
    tc_off  = no;
    tc_mask = want - 1;
    tc_used = 0;
    memset(nh, 0, (size_t)want * sizeof(uint64_t));
    memset(no, 0, (size_t)want * sizeof(uint64_t));
    /* First growth: the old table is still NULL (empty index), so there is
     * nothing to re-hash. Dereferencing oh[0] here was a NULL crash. */
    if (oh) {
        for (int i = 0; i <= omask; i++) {
            if (oh[i])
                tc_index_insert(oh[i], oo[i]);
        }
    }
    (void)oused;
    free(oh);
    free(oo);
}

static void tc_index_insert(uint64_t hash, uint64_t offset)
{
    if (hash == 0)
        hash = 1;   /* 0 marks an empty slot */
    if (tc_mask <= 0 || (tc_used + 1) * 10 >= (tc_mask + 1) * 7)
        tc_index_grow();
    if (tc_mask <= 0)
        return;
    int slot = (int)((uint32_t)hash & (uint32_t)tc_mask);
    while (tc_hash[slot] != 0) {
        if (tc_hash[slot] == hash && tc_off[slot] == offset)
            return;
        slot = (slot + 1) & tc_mask;
    }
    tc_hash[slot] = hash;
    tc_off[slot]  = offset;
    tc_used++;
}

static void tc_index_reset(void)
{
    free(tc_hash);
    free(tc_off);
    tc_hash = NULL;
    tc_off  = NULL;
    tc_mask = 0;
    tc_used = 0;
}

/* Build the cache file path under %APPDATA%\CPlayer. */
static void tc_file_path(wchar_t *out, int cap)
{
    wchar_t appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        wcsncpy(appdata, L".", MAX_PATH - 1);
    appdata[MAX_PATH - 1] = 0;
    _snwprintf(out, cap, L"%s\\CPlayer", appdata);
    out[cap - 1] = 0;
    CreateDirectoryW(out, NULL);
    size_t len = wcslen(out);
    _snwprintf(out + len, (size_t)cap - len, L"\\tagcache.dat");
    out[cap - 1] = 0;
}

static int tc_read_header(FILE *fp)
{
    uint32_t magic = 0, ver = 0;
    if (fread(&magic, 4, 1, fp) != 1 || magic != TC_MAGIC)
        return 0;
    if (fread(&ver, 4, 1, fp) != 1 || ver != TC_VERSION)
        return 0;
    return 1;
}

static void tc_write_header(FILE *fp)
{
    uint32_t magic = TC_MAGIC, ver = TC_VERSION;
    fwrite(&magic, 4, 1, fp);
    fwrite(&ver, 4, 1, fp);
}

void tag_cache_open(void)
{
    if (tc_ready)
        return;
    InitializeCriticalSection(&tc_cs);
    tc_ready = 1;

    wchar_t path[MAX_PATH];
    tc_file_path(path, MAX_PATH);
    tc_fp = _wfopen(path, L"r+b");
    if (!tc_fp) {
        tc_fp = _wfopen(path, L"w+b");
        if (!tc_fp)
            return;
        tc_write_header(tc_fp);
        fflush(tc_fp);
        return;
    }

    /* Scan the file to rebuild the index; truncate any corrupt tail so
     * later appends stay on a clean boundary. */
    fseek(tc_fp, 0, SEEK_END);
    long long fsize = _ftelli64(tc_fp);
    if (fsize < 8 || (unsigned long long)fsize > (unsigned long long)TC_MAX_BYTES * 8) {
        fclose(tc_fp);
        DeleteFileW(path);
        tc_fp = _wfopen(path, L"w+b");
        if (tc_fp) {
            tc_write_header(tc_fp);
            fflush(tc_fp);
        }
        return;
    }
    fseek(tc_fp, 0, SEEK_SET);
    if (!tc_read_header(tc_fp)) {
        fclose(tc_fp);
        DeleteFileW(path);
        tc_fp = _wfopen(path, L"w+b");
        if (tc_fp) {
            tc_write_header(tc_fp);
            fflush(tc_fp);
        }
        return;
    }

    long long good = _ftelli64(tc_fp);
    for (;;) {
        long long rec = _ftelli64(tc_fp);
        TCRec r;
        if (fread(&r, sizeof(r), 1, tc_fp) != 1)
            break;
        if (r.len_title > TC_MAX_STR || r.len_artist > TC_MAX_STR ||
            r.len_album > TC_MAX_STR)
            break;
        size_t wchars = (size_t)r.len_title + 1 + r.len_artist + 1 +
                        r.len_album + 1;
        if (fseek(tc_fp, (long)(wchars * sizeof(wchar_t)), SEEK_CUR) != 0)
            break;
        tc_index_insert(r.hash, (uint64_t)rec);
        good = _ftelli64(tc_fp);
        if (tc_used > 400000) {   /* grown too large: start over */
            fclose(tc_fp);
            DeleteFileW(path);
            tc_index_reset();
            tc_fp = _wfopen(path, L"w+b");
            if (tc_fp) {
                tc_write_header(tc_fp);
                fflush(tc_fp);
            }
            return;
        }
    }
    /* Drop a truncated/corrupt tail. */
    fflush(tc_fp);
    _chsize_s(_fileno(tc_fp), good);
    fseek(tc_fp, 0, SEEK_END);
}

void tag_cache_close(void)
{
    if (!tc_ready)
        return;
    EnterCriticalSection(&tc_cs);
    if (tc_fp) {
        fflush(tc_fp);
        fclose(tc_fp);
        tc_fp = NULL;
    }
    tc_index_reset();
    LeaveCriticalSection(&tc_cs);
    DeleteCriticalSection(&tc_cs);
    tc_ready = 0;
}

static wchar_t *tc_read_string(FILE *fp, int len)
{
    wchar_t *s = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
    if (!s)
        return NULL;
    if (len > 0 && fread(s, sizeof(wchar_t), (size_t)len, fp) != (size_t)len) {
        free(s);
        return NULL;
    }
    s[len] = 0;
    wchar_t nul;
    if (fread(&nul, sizeof(wchar_t), 1, fp) != 1) {
        free(s);
        return NULL;
    }
    return s;
}

int tag_cache_lookup(uint64_t path_hash, uint64_t file_size, TagInfo *out)
{
    if (!tc_ready || !tc_fp || tc_mask <= 0)
        return 0;
    if (path_hash == 0)
        path_hash = 1;

    int hit = 0;
    EnterCriticalSection(&tc_cs);
    int slot = (int)((uint32_t)path_hash & (uint32_t)tc_mask);
    for (int probes = 0; probes <= tc_mask; probes++) {
        if (tc_hash[slot] == 0)
            break;
        if (tc_hash[slot] == path_hash) {
            long long save = _ftelli64(tc_fp);
            if (fseek(tc_fp, (long)tc_off[slot], SEEK_SET) == 0) {
                TCRec r;
                if (fread(&r, sizeof(r), 1, tc_fp) == 1 &&
                    r.hash == path_hash && r.file_size == file_size &&
                    r.len_title <= TC_MAX_STR && r.len_artist <= TC_MAX_STR &&
                    r.len_album <= TC_MAX_STR) {
                    memset(out, 0, sizeof(*out));
                    out->title  = tc_read_string(tc_fp, r.len_title);
                    out->artist = tc_read_string(tc_fp, r.len_artist);
                    out->album  = tc_read_string(tc_fp, r.len_album);
                    out->duration    = r.duration;
                    out->bitrate     = r.bitrate;
                    out->sample_rate = r.sample_rate;
                    out->channels    = r.channels;
                    out->file_size   = r.file_size;
                    hit = 1;
                }
            }
            fseek(tc_fp, (long)save, SEEK_SET);
            break;
        }
        slot = (slot + 1) & tc_mask;
    }
    LeaveCriticalSection(&tc_cs);
    return hit;
}

static void tc_write_string(FILE *fp, const wchar_t *s, int len)
{
    static const wchar_t nul = 0;
    if (len > 0)
        fwrite(s, sizeof(wchar_t), (size_t)len, fp);
    fwrite(&nul, sizeof(wchar_t), 1, fp);   /* binary: never use fputwc */
}

void tag_cache_store(uint64_t path_hash, uint64_t file_size, const TagInfo *ti)
{
    if (!tc_ready || !tc_fp || !ti)
        return;
    if (path_hash == 0)
        path_hash = 1;

    size_t lt = ti->title  ? wcslen(ti->title)  : 0;
    size_t la = ti->artist ? wcslen(ti->artist) : 0;
    size_t lb = ti->album  ? wcslen(ti->album)  : 0;
    if (lt > TC_MAX_STR) lt = TC_MAX_STR;
    if (la > TC_MAX_STR) la = TC_MAX_STR;
    if (lb > TC_MAX_STR) lb = TC_MAX_STR;

    EnterCriticalSection(&tc_cs);
    fseek(tc_fp, 0, SEEK_END);
    long long rec = _ftelli64(tc_fp);
    if (rec >= 0) {
        TCRec r;
        memset(&r, 0, sizeof(r));
        r.hash        = path_hash;
        r.file_size   = file_size;
        r.duration    = ti->duration;
        r.bitrate     = ti->bitrate;
        r.sample_rate = ti->sample_rate;
        r.channels    = ti->channels;
        r.len_title   = (uint16_t)lt;
        r.len_artist  = (uint16_t)la;
        r.len_album   = (uint16_t)lb;
        if (fwrite(&r, sizeof(r), 1, tc_fp) == 1) {
            tc_write_string(tc_fp, ti->title, (int)lt);
            tc_write_string(tc_fp, ti->artist, (int)la);
            tc_write_string(tc_fp, ti->album, (int)lb);
            fflush(tc_fp);
            tc_index_insert(path_hash, (uint64_t)rec);
        }
    }
    LeaveCriticalSection(&tc_cs);
}
