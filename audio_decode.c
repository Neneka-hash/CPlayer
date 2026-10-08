/*
 * audio_decode.c - Unified streaming decoder and the decoder worker thread.
 *
 * All four codec single-file libraries are pulled in here (and only here) so
 * the rest of the project never sees their large headers. Every format is
 * opened in streaming mode: PCM is read in bounded chunks via callbacks or
 * the codec's own file handle, never mmap'd whole.
 *
 * The decoder thread is the producer half of a two-thread pipeline. It
 * decodes PCM into free pool blocks and pushes them to the waveOut thread.
 * Seek / open / stop are coordinated through a flush handshake:
 *
 *   decoder raises `flushing`  ->  waveOut drains & reports `wo_idle`
 *   decoder clears `flushing`  ->  proceeds (seek / open / close)
 *
 * Auto-reset events are used throughout (no manual ResetEvent) so a signal
 * raised before a wait is never lost.
 */
#include "mp_player.h"
#include "audio_decode.h"
#include "audio_waveout.h"   /* pool_grab_free / pool_push_filled / waveout_* */
#include "playlist.h"

#include <stdlib.h>
#include <string.h>

/* ---- Codec implementations (compiled into this TU only) --------------- */
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
/* Disable minimp3's SSE2 SIMD path and use the scalar fallback. The scalar
 * path is smaller code (no extra SIMD routines — measured 512 bytes smaller
 * than the SIMD build on x64) and still fast enough for real-time MP3 decode;
 * this is a size-driven choice, not a correctness one. */
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO          /* we feed mp3dec_ex via callbacks */
#include "minimp3_ex.h"
/* minimp3 and stb_vorbis both define a static get_bits(); rename stb_vorbis's
 * copy via the preprocessor so both can live in this TU. */
#define get_bits stb_vorbis_get_bits
#include "stb_vorbis.c"
#undef get_bits


/* ---- Decoder (opaque in the header) ----------------------------------- */
struct Decoder {
    int      format;            /* audio format: FMT_MP3 / FMT_FLAC / FMT_WAV / FMT_OGG */
    int      channels;          /* source channels (1, 2, 6, ...)            */
    int      sample_rate;       /* source sample rate in Hz                  */
    uint64_t total_frames;      /* measured in stereo-output frames          */

    drflac       *flac;         /* FLAC handle (heap)                        */
    drwav        *wav;          /* WAV handle (heap, for uniform lifetime)   */
    mp3dec_ex_t  *mp3;          /* MP3 streaming state (heap; stable addr)   */
    mp3dec_io_t   mp3_io;       /* MP3 read/seek callbacks bound to `file`   */
    stb_vorbis   *ogg;          /* OGG handle (owns its own FILE*)           */

    FILE         *file;         /* MP3 streaming FILE*; NULL for others      */
    short        *scratch;      /* native int16 buffer used as downmix source*/
    int           scratch_capacity; /* in samples (frames * channels)       */
};


/* ====================================================================== */
/*  Format detection                                                      */
/* ====================================================================== */

/* Detect audio format from file extension. Returns FMT_* constant or FMT_UNKNOWN. */
int detect_format(const wchar_t *path)
{
    if (!path) return FMT_UNKNOWN;
    const wchar_t *dot = NULL;
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'.') dot = p;
    if (!dot || !dot[1]) return FMT_UNKNOWN;
    const wchar_t *ext = dot + 1;

    if (!_wcsicmp(ext, L"mp3"))  return FMT_MP3;
    if (!_wcsicmp(ext, L"flac")) return FMT_FLAC;
    if (!_wcsicmp(ext, L"wav")  || !_wcsicmp(ext, L"wave")) return FMT_WAV;
    if (!_wcsicmp(ext, L"ogg")  || !_wcsicmp(ext, L"oga"))  return FMT_OGG;
    return FMT_UNKNOWN;
}


/* ====================================================================== */
/*  Per-format open                                                       */
/* ====================================================================== */

/* --- MP3: stream through FILE* callbacks (never mmap) ----------------- */
static size_t mp3_read_cb(void *buf, size_t size, void *user_data)
{
    FILE *f = (FILE *)user_data;
    return fread(buf, 1, size, f);
}
/* Seek callback for minimp3 streaming I/O. Returns 0 on success, -1 on error. */
static int mp3_seek_cb(uint64_t position, void *user_data)
{
    FILE *f = (FILE *)user_data;
    /* minimp3 treats a non-zero return as an I/O error. */
    return _fseeki64(f, (int64_t)position, SEEK_SET) == 0 ? 0 : -1;
}

/* Open an MP3 file for streaming via minimp3 with sample-accurate seeking. */
static Decoder *decoder_open_mp3(const wchar_t *path)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;

    mp3dec_ex_t *ex = (mp3dec_ex_t *)calloc(1, sizeof(mp3dec_ex_t));
    if (!ex) { fclose(f); return NULL; }

    Decoder *d = (Decoder *)calloc(1, sizeof(Decoder));
    if (!d) { free(ex); fclose(f); return NULL; }

    d->mp3_io.read      = mp3_read_cb;
    d->mp3_io.read_data = f;
    d->mp3_io.seek      = mp3_seek_cb;
    d->mp3_io.seek_data = f;
    d->file = f;

    /* MP3D_SEEK_TO_SAMPLE builds a sample-accurate index (one sequential
     * pass over the file) so total duration and seeking are precise. The
     * file is still read in 128 KB chunks, never loaded whole. */
    if (mp3dec_ex_open_cb(ex, &d->mp3_io, MP3D_SEEK_TO_SAMPLE) != 0) {
        /* minimp3_ex's open_cb returns early on error without releasing its
         * own 128 KB I/O buffer or a partially built sample index, so the
         * close call is what actually frees them. It is a no-op on the paths
         * that failed before allocating, and it never touches our FILE*. */
        mp3dec_ex_close(ex);
        free(ex); fclose(f); free(d);
        return NULL;
    }
    d->mp3         = ex;
    d->format      = FMT_MP3;
    d->channels    = ex->info.channels ? ex->info.channels : 2;
    d->sample_rate = ex->info.hz;
    /* ex->samples counts every channel sample; convert to frames. */
    d->total_frames = (d->channels > 0)
                      ? ex->samples / (uint64_t)d->channels
                      : 0;
    return d;
}

/* --- FLAC: dr_flac streams internally --------------------------------- */
/* Open a FLAC file for streaming via dr_flac. */
static Decoder *decoder_open_flac(const wchar_t *path)
{
    drflac *fl = drflac_open_file_w(path, NULL);
    if (!fl) return NULL;
    Decoder *d = (Decoder *)calloc(1, sizeof(Decoder));
    if (!d) { drflac_close(fl); return NULL; }
    d->flac         = fl;
    d->format       = FMT_FLAC;
    d->channels     = fl->channels ? fl->channels : 2;
    d->sample_rate  = (int)fl->sampleRate;
    d->total_frames = fl->totalPCMFrameCount;
    return d;
}

/* --- WAV: dr_wav streams internally (handles 8/16/24/32-bit + float) -- */
/* Open a WAV file for streaming via dr_wav (handles 8/16/24/32-bit + float). */
static Decoder *decoder_open_wav(const wchar_t *path)
{
    drwav *w = (drwav *)calloc(1, sizeof(drwav));
    if (!w) return NULL;
    if (!drwav_init_file_w(w, path, NULL)) { free(w); return NULL; }
    Decoder *d = (Decoder *)calloc(1, sizeof(Decoder));
    if (!d) { drwav_uninit(w); free(w); return NULL; }
    d->wav          = w;
    d->format       = FMT_WAV;
    d->channels     = (int)w->channels ? (int)w->channels : 2;
    d->sample_rate  = (int)w->sampleRate;
    d->total_frames = w->totalPCMFrameCount;
    return d;
}

/* --- OGG/Vorbis: stb_vorbis owns the FILE* (close_on_free) ----------- */
/* Open an OGG Vorbis file for streaming via stb_vorbis (owns the FILE*). */
static Decoder *decoder_open_ogg(const wchar_t *path)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    int err = 0;
    /* close_on_free=TRUE：stb_vorbis 打开失败时内部已 fclose(f)，
     * 此路径绝不能再次 fclose(f)（双重关闭是未定义行为）。 */
    stb_vorbis *v = stb_vorbis_open_file(f, TRUE, &err, NULL);
    if (!v) return NULL;
    Decoder *d = (Decoder *)calloc(1, sizeof(Decoder));
    if (!d) { stb_vorbis_close(v); return NULL; }
    stb_vorbis_info info = stb_vorbis_get_info(v);
    d->ogg         = v;
    d->format      = FMT_OGG;
    d->channels    = info.channels ? info.channels : 2;
    d->sample_rate = (int)info.sample_rate;
    d->total_frames = (uint64_t)stb_vorbis_stream_length_in_samples(v);
    return d;
}

/* Open a file by path, auto-detecting format from extension. Returns NULL on failure. */
Decoder *decoder_open(const wchar_t *path)
{
    if (!path) return NULL;
    int fmt = detect_format(path);
    Decoder *d = NULL;
    switch (fmt) {
        case FMT_MP3:  d = decoder_open_mp3(path);  break;
        case FMT_FLAC: d = decoder_open_flac(path); break;
        case FMT_WAV:  d = decoder_open_wav(path);  break;
        case FMT_OGG:  d = decoder_open_ogg(path);  break;
        default: return NULL;
    }
    if (!d) return NULL;

    /* Allocate the native int16 scratch buffer once: BUFFER_FRAMES frames
     * times source channels. Used by decoder_read as the downmix source. */
    d->scratch_capacity = BUFFER_FRAMES * d->channels;
    d->scratch = (short *)malloc((size_t)d->scratch_capacity * sizeof(short));
    if (!d->scratch) { decoder_close(d); return NULL; }
    return d;
}


/* ====================================================================== */
/*  Accessors                                                             */
/* ====================================================================== */

/* Return the source channel count (0 if decoder is NULL). */
int      decoder_channels(Decoder *d)      { return d ? d->channels : 0; }
/* Return the source sample rate (0 if decoder is NULL). */
int      decoder_rate(Decoder *d)          { return d ? d->sample_rate : 0; }
/* Return the total number of stereo-output frames (0 if decoder is NULL). */
uint64_t decoder_total_frames(Decoder *d)  { return d ? d->total_frames : 0; }


/* ====================================================================== */
/*  Downmix to stereo int16                                              */
/* ====================================================================== *
 * src: native interleaved int16 with `channels` samples per frame.
 * dst: stereo interleaved int16 (2 per frame).
 * Even-indexed channels sum into L, odd-indexed into R, then clip. */

/* Downmix multi-channel int16 audio to stereo int16.
 * Even-indexed channels sum into L, odd-indexed into R, then clip to int16 range. */
static void downmix_to_stereo(const short *src, short *dst, int frames, int channels)
{
    int i, c;
    if (channels == OUTPUT_CHANNELS) {
        memcpy(dst, src, (size_t)frames * OUTPUT_CHANNELS * sizeof(short));
        return;
    }
    if (channels == 1) {
        for (i = 0; i < frames; i++) {
            dst[i * 2]     = src[i];
            dst[i * 2 + 1] = src[i];
        }
        return;
    }
    for (i = 0; i < frames; i++) {
        int l = 0, r = 0, lc = 0, rc = 0;
        for (c = 0; c < channels; c++) {
            int s = src[i * channels + c];
            if (c & 1) { r += s; rc++; }
            else       { l += s; lc++; }
        }
        if (lc) l /= lc;
        if (rc) r /= rc;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        dst[i * 2]     = (short)l;
        dst[i * 2 + 1] = (short)r;
    }
}


/* ====================================================================== */
/*  Read (with downmix to stereo int16)                                  */
/* ====================================================================== */

/* Read up to max_frames stereo int16 frames from the decoder, with automatic
 * downmix to stereo. Returns frames read, or 0 at EOF / error. */
int decoder_read(Decoder *d, short *out, int max_frames)
{
    if (!d || !out || max_frames <= 0) return 0;
    if (max_frames > BUFFER_FRAMES) max_frames = BUFFER_FRAMES;

    int got_frames = 0;

    switch (d->format) {
    case FMT_MP3: {
        /* mp3dec_ex_read counts in samples (channels included). */
        size_t want = (size_t)max_frames * (size_t)d->channels;
        size_t got = mp3dec_ex_read(d->mp3, d->scratch, want);
        got_frames = (d->channels > 0) ? (int)(got / (size_t)d->channels) : 0;
        break;
    }
    case FMT_FLAC:
        got_frames = (int)drflac_read_pcm_frames_s16(
            d->flac, (drflac_uint64)max_frames, d->scratch);
        break;
    case FMT_WAV:
        got_frames = (int)drwav_read_pcm_frames_s16(
            d->wav, (drwav_uint64)max_frames, d->scratch);
        break;
    case FMT_OGG:
        /* stb_vorbis_get_samples_short_interleaved returns frames. */
        got_frames = stb_vorbis_get_samples_short_interleaved(
            d->ogg, d->channels, d->scratch,
            max_frames * d->channels);
        break;
    default:
        return 0;
    }

    if (got_frames <= 0) return 0;       /* EOF */
    if (got_frames > max_frames) got_frames = max_frames;

    downmix_to_stereo(d->scratch, out, got_frames, d->channels);
    return got_frames;
}


/* ====================================================================== */
/*  Seek                                                                  */
/* ====================================================================== */

/* Seek to an absolute frame index (stereo-output frame). Returns 1 on success, 0 on failure. */
int decoder_seek(Decoder *d, uint64_t frame)
{
    if (!d) return 0;
    switch (d->format) {
    case FMT_MP3:
        /* mp3dec_ex_seek position is in samples (channels included). */
        return mp3dec_ex_seek(d->mp3, frame * (uint64_t)d->channels) == 0;
    case FMT_FLAC:
        return drflac_seek_to_pcm_frame(d->flac, (drflac_uint64)frame);
    case FMT_WAV:
        return drwav_seek_to_pcm_frame(d->wav, (drwav_uint64)frame);
    case FMT_OGG: {
        /* stb_vorbis takes a 32-bit sample number: clamp rather than
         * silently wrap for absurd (>4G frame) seeks. */
        if (frame > 0xFFFFFFFFu) return 0;
        return stb_vorbis_seek(d->ogg, (unsigned int)frame);
    }
    default:
        return 0;
    }
}


/* ====================================================================== */
/*  Close                                                                 */
/* ====================================================================== */

/* Close the decoder and free all associated resources (codec handles, FILE*, scratch buffer). */
void decoder_close(Decoder *d)
{
    if (!d) return;
    switch (d->format) {
    case FMT_MP3:
        if (d->mp3) mp3dec_ex_close(d->mp3);
        free(d->mp3);
        if (d->file) fclose(d->file);
        break;
    case FMT_FLAC:
        if (d->flac) drflac_close(d->flac);
        break;
    case FMT_WAV:
        if (d->wav) { drwav_uninit(d->wav); free(d->wav); }
        break;
    case FMT_OGG:
        if (d->ogg) stb_vorbis_close(d->ogg);   /* closes its own FILE* */
        break;
    default: break;
    }
    free(d->scratch);
    free(d);
}


/* ====================================================================== */
/*  Decoder worker thread                                                 */
/* ====================================================================== *
 *
 * Synchronization contract (auto-reset events; never lose a wake-up):
 *
 *   dec_event  - signalled by: UI (cmd posted), waveOut (block freed),
 *                resume from pause, quit. Waited on by decoder when idle.
 *   wo_ctrl    - signalled by: decoder (new filled block / flush start /
 *                flush end), UI (pause/resume). Waited on by waveOut.
 *   wo_idle_evt- signalled by: waveOut (flush complete). Waited on by
 *                decoder during flush.
 *
 * All g_player fields below are touched under g_player.cs unless noted.
 */

/* Start a flush: ask waveOut to abort playback and report idle. cs held. */
static void flush_start(void)
{
    g_player.flushing = 1;
    g_player.wo_idle  = 0;
    SetEvent(g_player.wo_ctrl);     /* wake waveOut so it sees flushing */
}

/* Wait until waveOut reports idle. cs NOT held during the wait.
 * 若 quit 已置位（waveOut 线程可能已退出、永远不会报告 idle）则立即
 * 返回，避免 player_shutdown 时死锁；waveOut 线程退出前也会主动完成
 * flush 握手（见 waveout_thread_proc 顶部 quit 分支），双保险。 */
static void flush_wait_idle(void)
{
    while (!g_player.wo_idle && !g_player.quit)
        WaitForSingleObject(g_player.wo_idle_evt, INFINITE);
}

/* End a flush: clear the flag and release waveOut. cs held. */
static void flush_end(void)
{
    g_player.flushing = 0;
    SetEvent(g_player.wo_ctrl);     /* waveOut can resume scanning */
}

/* ---- Command handlers ------------------------------------------------- */

/* Handle CMD_OPEN: flush pipeline, close old decoder, open new file, (re)init waveOut, start playback. */
static void handle_open(void)
{
    /* 1. Flush the waveOut pipeline so all blocks return to BS_FREE. */
    EnterCriticalSection(&g_player.cs);
    flush_start();
    LeaveCriticalSection(&g_player.cs);

    flush_wait_idle();

    /* 2. Close the previous decoder (if any). */
    EnterCriticalSection(&g_player.cs);
    flush_end();
    Decoder *old = g_player.dec;
    g_player.dec = NULL;
    LeaveCriticalSection(&g_player.cs);
    if (old) decoder_close(old);

    /* 3. Take a private copy of the pending-open path under cs, then open
     * the file outside cs. Never dereference g_playlist.items[] here: the
     * UI thread may clear/remove entries at any moment. player_open() (UI
     * thread) already copied the path into g_player.open_path.
     * index 与 path 在同一个临界区内快照，保证二者来自同一次 player_open
     * 调用——避免快速连点切歌时"新路径 + 旧索引"的撕裂。 */
    EnterCriticalSection(&g_player.cs);
    int index            = g_player.open_index;
    const wchar_t *src   = g_player.open_path;
    wchar_t *path        = src ? _wcsdup(src) : NULL;
    LeaveCriticalSection(&g_player.cs);

    if (!path) {
        /* Same housekeeping as the open-failure branch below: the previous
         * decoder is already closed and the pipeline flushed here, so
         * leaving state at PLAYING would strand the UI on "playing" with
         * silence and no WM_TRACK_LOADED to refresh it. */
        EnterCriticalSection(&g_player.cs);
        g_player.state        = STATE_STOPPED;
        g_player.cur_frame    = 0;
        g_player.total_frames = 0;
        g_player.eof          = 0;
        g_player.ended_posted = 1;
        LeaveCriticalSection(&g_player.cs);
        PostMessage(g_player.hMain, WM_PLAYER_ERROR,
                    (WPARAM)PLAYER_ERR_INVALID_INDEX, 0);
        PostMessage(g_player.hMain, WM_TRACK_LOADED, 0, 0);
        return;
    }
    Decoder *d = decoder_open(path);
    free(path);   /* 局部拷贝用完即释放；open_path 本体由下一次 player_open 或 shutdown 释放 */
    if (!d) {
        /* 打开失败：清理播放状态，避免界面卡在"正在播放"但无音频。 */
        EnterCriticalSection(&g_player.cs);
        g_player.state        = STATE_STOPPED;
        g_player.cur_frame    = 0;
        g_player.total_frames = 0;
        g_player.eof          = 0;
        g_player.ended_posted = 1;   /* 旧缓冲清空后不再报曲目结束 */
        LeaveCriticalSection(&g_player.cs);
        PostMessage(g_player.hMain, WM_PLAYER_ERROR,
                    (WPARAM)PLAYER_ERR_OPEN_FAILED, 0);
        PostMessage(g_player.hMain, WM_TRACK_LOADED, 0, 0);  /* 刷新 UI 状态 */
        return;
    }

    /* 4. Install the decoder, reset positions, (re)open waveOut if needed. */
    EnterCriticalSection(&g_player.cs);
    g_player.dec          = d;
    g_player.cur_index    = index;
    g_player.cur_frame    = 0;
    g_player.eof          = 0;
    g_player.ended_posted = 0;
    g_player.total_frames = decoder_total_frames(d);
    int rate              = decoder_rate(d);
    g_player.sample_rate  = rate;

    int need_reopen = (g_player.wo_rate != rate);
    HWAVEOUT oldhwo = NULL;
    if (need_reopen) {
        oldhwo        = g_player.hwo;
        g_player.hwo  = NULL;
        g_player.wo_rate = 0;
    }
    LeaveCriticalSection(&g_player.cs);

    /* 4b. (Re)open the waveOut device outside cs (may take time). */
    if (oldhwo) { waveOutReset(oldhwo); waveOutClose(oldhwo); }
    int ok = 1;
    if (need_reopen)
        ok = waveout_open(rate);          /* sets g_player.hwo */

    /* 4c. Commit, or roll back on waveOut failure. */
    EnterCriticalSection(&g_player.cs);
    if (ok) {
        g_player.wo_rate = rate;
        g_player.state   = STATE_PLAYING;
        /* 新曲目代数：waveOut 发的 WM_TRACK_ENDED 若携带旧代数会被 UI 丢弃，
         * 防止"上一首结束"误触发对新曲目的自动跳歌。 */
        InterlockedIncrement(&g_player.track_gen);
    } else {
        /* waveOut 打开失败（无音频设备/设备忙）：关闭刚装的解码器并回到
         * 停止态，否则解码器会持续填充、waveOut 不消费，形成静音死锁。 */
        g_player.dec          = NULL;
        g_player.state        = STATE_STOPPED;
        g_player.sample_rate  = 0;
        g_player.total_frames = 0;
        g_player.cur_frame    = 0;
        g_player.eof          = 1;
        g_player.ended_posted = 1;
        g_player.wo_rate      = 0;
        LeaveCriticalSection(&g_player.cs);
        decoder_close(d);
        PostMessage(g_player.hMain, WM_PLAYER_ERROR,
                    (WPARAM)PLAYER_ERR_DEVICE_OPEN, 0);
        PostMessage(g_player.hMain, WM_TRACK_LOADED, 0, 0);  /* 刷新 UI 状态 */
        return;
    }
    waveout_set_volume(g_player.volume);
    LeaveCriticalSection(&g_player.cs);

    /* 5. Kick the pipeline: decoder starts filling, UI refreshes. */
    SetEvent(g_player.dec_event);
    PostMessage(g_player.hMain, WM_TRACK_LOADED, 0, 0);
}

/* Handle CMD_SEEK: flush pipeline, seek decoder to target frame, resume decoding. */
static void handle_seek(uint64_t frame)
{
    EnterCriticalSection(&g_player.cs);
    flush_start();
    LeaveCriticalSection(&g_player.cs);

    flush_wait_idle();

    EnterCriticalSection(&g_player.cs);
    flush_end();
    Decoder *d = g_player.dec;
    LeaveCriticalSection(&g_player.cs);

    if (d) {
        decoder_seek(d, frame);             /* slow, outside cs */
        EnterCriticalSection(&g_player.cs);
        g_player.cur_frame    = frame;
        g_player.eof          = 0;
        g_player.ended_posted = 0;
        LeaveCriticalSection(&g_player.cs);
    }

    SetEvent(g_player.dec_event);           /* resume decoding */
}

/* Handle CMD_STOP: flush pipeline, close decoder, reset state, update UI. */
static void handle_stop(void)
{
    EnterCriticalSection(&g_player.cs);
    flush_start();
    LeaveCriticalSection(&g_player.cs);

    flush_wait_idle();

    EnterCriticalSection(&g_player.cs);
    flush_end();
    Decoder *old = g_player.dec;
    g_player.dec = NULL;
    g_player.state       = STATE_STOPPED;
    g_player.cur_frame   = 0;
    g_player.eof         = 0;
    g_player.ended_posted = 0;
    g_player.total_frames = 0;
    LeaveCriticalSection(&g_player.cs);

    if (old) decoder_close(old);

    PostMessage(g_player.hMain, WM_TRACK_LOADED, 0, 0);  /* refresh UI */
}

/* ---- Thread entry point: streaming decode loop ------------------------ */

DWORD WINAPI decoder_thread_proc(LPVOID param)
{
    (void)param;

    for (;;) {
        if (g_player.quit) break;

        EnterCriticalSection(&g_player.cs);

        /* Drain any pending command first. */
        int cmd = (int)InterlockedExchange(&g_player.cmd, CMD_NONE);
        /* Snapshot the seek target in the same critical section that
         * consumes the command, so a CMD_SEEK always pairs with the frame
         * value its sender published (a later seek cannot tear into it). */
        uint64_t seek_at = (uint64_t)g_player.seek_frame;
        if (cmd == CMD_NONE) {
            /* No command: try to produce one block if we're playing.
             * This is the core streaming decode loop — repeatedly grab a free
             * block, decode PCM into it, push it to the waveOut thread. */
            int can_decode = (g_player.state == STATE_PLAYING &&
                              g_player.dec != NULL &&
                              !g_player.eof);
            if (can_decode) {
                PcmBlock *blk = pool_grab_free();   /* marks BS_DECODING */
                if (blk) {
                    Decoder *dec = g_player.dec;
                    LeaveCriticalSection(&g_player.cs);

                    /* Decode outside the lock (potentially slow). */
                    int n = decoder_read(dec, blk->data, BUFFER_FRAMES);

                    EnterCriticalSection(&g_player.cs);
                    int stale = (g_player.cmd != CMD_NONE ||
                                 g_player.flushing || g_player.quit);
                    if (stale || n <= 0) {
                        /* Discard this block: a command intervened, or EOF. */
                        blk->state = BS_FREE;
                        if (!stale && n <= 0) {
                            /* Genuine EOF: let waveOut drain the tail, then
                             * it posts WM_TRACK_ENDED once buffers empty. */
                            g_player.eof = 1;
                        }
                    } else {
                        blk->frames = n;
                        pool_push_filled(blk);      /* marks BS_FILLED, wakes waveOut */
                        g_player.cur_frame += (uint64_t)n;
                    }
                    LeaveCriticalSection(&g_player.cs);
                    continue;                       /* immediately try next block */
                }
            }
            /* Either not playing, or no free block available. */
            LeaveCriticalSection(&g_player.cs);
            WaitForSingleObject(g_player.dec_event, INFINITE);
            continue;
        }

        LeaveCriticalSection(&g_player.cs);

        switch (cmd) {
        case CMD_OPEN: handle_open(); break;
        case CMD_SEEK: handle_seek(seek_at); break;
        case CMD_STOP: handle_stop(); break;
        default: break;
        }
    }

    return 0;
}
