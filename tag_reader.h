/*
 * tag_reader.h - Independent audio metadata reader.
 *
 * Reads tag fields (title / artist / album) and technical info (duration /
 * bitrate / sample_rate / channels / file_size) directly from file headers,
 * without instantiating the codec decoders. This keeps tag extraction cheap
 * (a bounded header I/O per file) and decoupled from the codec APIs.
 *
 * Supported formats:
 *   MP3  : ID3v2.3 / 2.4 text frames (TIT2 / TPE1 / TALB) + ID3v1 trailer
 *   FLAC : STREAMINFO block + Vorbis Comment block
 *   OGG  : Vorbis identification header + comment header packet
 *   WAV  : RIFF fmt + data + LIST/INFO sub-chunks (INAM / IART / IPRD)
 *
 * `format` parameter takes one of the FMT_* constants from mp_player.h
 * (FMT_MP3=1, FMT_FLAC=2, FMT_WAV=3, FMT_OGG=4).
 */
#ifndef TAG_READER_H
#define TAG_READER_H

#include <windows.h>
#include <stdint.h>

typedef struct {
    wchar_t *title;        /* 标题，可能为 NULL -> 调用者降级为文件名 */
    wchar_t *artist;       /* 艺术家，可能为 NULL */
    wchar_t *album;        /* 专辑，可能为 NULL */
    double   duration;     /* 时长（秒），0.0 表示未知 */
    int      bitrate;      /* 比特率（kbps），0 表示未知 */
    int      sample_rate;  /* 采样率（Hz），0 表示未知 */
    int      channels;     /* 声道数，0 表示未知 */
    uint64_t file_size;    /* 文件大小（字节），0 表示未知 */
    LONG     epoch;        /* 播放列表生成计数器，用于判断异步消息是否过期 */
} TagInfo;

/* 从 `path` 读取指定 `format` 的标签信息和技术参数。
 * 成功返回 0（out 字段已填充，缺少的字段为 NULL/0），
 * 失败返回 -1（out 已清零）。调用者必须调用 tag_info_free()
 * 释放 out 中的堆字符串。 */
int  tag_read(const wchar_t *path, int format, TagInfo *out);

/* 释放 `info` 中的所有堆字符串（不释放 `info` 本身）。 */
void tag_info_free(TagInfo *info);

#endif /* TAG_READER_H */
