/*
 * audio_decode.h - Unified streaming decoder for MP3/FLAC/WAV/OGG and the
 * decoder worker thread.
 *
 * The Decoder struct is opaque here; its full definition (which references
 * the codec types drflac / drwav / mp3dec_ex_t / stb_vorbis) lives in
 * audio_decode.c so that other translation units do not need to pull in the
 * (large) codec headers.
 */
#ifndef AUDIO_DECODE_H
#define AUDIO_DECODE_H

#include <windows.h>
#include <stdint.h>
#include "mp_player.h"   /* Decoder typedef, enums, BUFFER_FRAMES */

/* ===== Public API for format detection, decoder lifecycle, and accessors ===== */

/* Detect format from a path's extension. Returns FMT_UNKNOWN if unsupported. */
int detect_format(const wchar_t *path);

/* Open path for streaming. Returns NULL on failure. On success the decoder's
 * channels / sample_rate / total_frames fields are valid (read via the
 * accessors below). */
Decoder *decoder_open(const wchar_t *path);

int      decoder_channels(Decoder *d);    /* source channel count            */
int      decoder_rate(Decoder *d);         /* source sample rate              */
uint64_t decoder_total_frames(Decoder *d); /* total stereo frames             */

/* Read up to max_frames stereo int16 frames into out (interleaved).
 * Returns the number of frames read; 0 at EOF. */
int  decoder_read(Decoder *d, short *out, int max_frames);

/* Seek to an absolute frame index. Returns 1 on success, 0 on failure. */
int  decoder_seek(Decoder *d, uint64_t frame);

/* Close and free the decoder (also closes any internal FILE*). */
void decoder_close(Decoder *d);

/* Worker thread entry point. */
DWORD WINAPI decoder_thread_proc(LPVOID param);

#endif /* AUDIO_DECODE_H */
