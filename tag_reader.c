/*
 * tag_reader.c - Implementation of tag_reader.h.
 *
 * All four formats are parsed from a single bounded header buffer (up to
 * TAG_HEADER_BUF bytes). MP3 additionally peeks at the last 128 bytes for
 * an ID3v1 trailer. No codec is instantiated — this module only depends
 * on the C runtime + Win32.
 *
 * Encoding handling:
 *   ID3v2 text frames declare their own encoding (0=ISO-8859-1, 1=UTF-16
 *   with BOM, 2=UTF-16BE, 3=UTF-8); we decode each accordingly.
 *   Vorbis comments are always UTF-8. RIFF INFO strings are CP1252 (Windows
 *   Latin-1 superset). All output strings are converted to wchar_t.
 *
 * Duration strategy:
 *   MP3  : estimated from file size + first frame bitrate (CBR approximation)
 *   FLAC : exact (total_samples / sample_rate from STREAMINFO)
 *   WAV  : exact (data chunk size / byte_rate from fmt)
 *   OGG  : exact (last page's granule position / sample_rate, read from the
 *          file tail; average bitrate derived from file size / duration)
 */
#include "tag_reader.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TAG_HEADER_BUF 65536   /* max header bytes to slurp per file */

/* ---- little/big-endian readers --------------------------------------- */
/* 从内存中读取小端序 32 位无符号整数 */
static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
/* 从内存中读取小端序 16 位无符号整数 */
static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
/* 从内存中读取大端序 32 位无符号整数 */
static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ---- string conversion helpers --------------------------------------- */
/* 将 UTF-8 字节序列（无需 NUL 结尾）转换为 NUL 结尾的 wchar_t* 字符串 */
static wchar_t *utf8_to_wstr(const char *src, size_t len) {
    if (!src || len == 0) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, src, (int)len, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *r = (wchar_t *)malloc((size_t)(n + 1) * sizeof(wchar_t));
    if (!r) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, src, (int)len, r, n);
    r[n] = 0;
    return r;
}

/* 将 CP1252（Windows Latin-1 超集）字节序列转换为 wchar_t* 字符串 */
static wchar_t *cp1252_to_wstr(const char *src, size_t len) {
    if (!src || len == 0) return NULL;
    int n = MultiByteToWideChar(1252, 0, src, (int)len, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *r = (wchar_t *)malloc((size_t)(n + 1) * sizeof(wchar_t));
    if (!r) return NULL;
    MultiByteToWideChar(1252, 0, src, (int)len, r, n);
    r[n] = 0;
    return r;
}

/* 将 UTF-16LE 字节序列转换为 wchar_t* 字符串 */
static wchar_t *utf16le_to_wstr(const uint8_t *src, size_t len) {
    if (!src || len < 2) return NULL;
    int units = (int)(len / 2);
    wchar_t *r = (wchar_t *)malloc((size_t)(units + 1) * sizeof(wchar_t));
    if (!r) return NULL;
    for (int i = 0; i < units; i++)
        r[i] = (wchar_t)(src[2 * i] | (src[2 * i + 1] << 8));
    r[units] = 0;
    return r;
}

/* ---- file helpers ---------------------------------------------------- */
/* 通过 Win32 API 获取文件大小（字节），失败返回 0 */
static uint64_t get_file_size(const wchar_t *path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return 0;
    return ((uint64_t)fa.nFileSizeHigh << 32) | (uint64_t)fa.nFileSizeLow;
}

/* 读取文件头部最多 max_bytes 字节，调用者负责 free()。失败返回 NULL */
static uint8_t *slurp_head(const wchar_t *path, size_t max_bytes,
                           size_t *out_size) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    uint8_t *buf = (uint8_t *)malloc(max_bytes);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, max_bytes, f);
    fclose(f);
    *out_size = got;
    return buf;
}

/* 读取文件尾部 n 字节到固定缓冲区，返回实际读取的字节数 */
static size_t read_tail(const wchar_t *path, uint8_t *dst, size_t n) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return 0;
    _fseeki64(f, 0, SEEK_END);
    int64_t total = _ftelli64(f);
    if (total <= 0 || (uint64_t)total < n) { fclose(f); return 0; }
    _fseeki64(f, -(int64_t)n, SEEK_END);
    size_t got = fread(dst, 1, n, f);
    fclose(f);
    return got;
}

/* 读取文件末尾最多 max_bytes 字节（文件小于 max_bytes 时读整个文件），
 * 调用者负责 free()。失败返回 NULL。 */
static uint8_t *slurp_tail(const wchar_t *path, size_t max_bytes,
                           size_t *out_size) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    _fseeki64(f, 0, SEEK_END);
    int64_t total = _ftelli64(f);
    if (total <= 0) { fclose(f); return NULL; }
    size_t n = ((uint64_t)total < max_bytes) ? (size_t)total : max_bytes;
    uint8_t *buf = (uint8_t *)malloc(n ? n : 1);
    if (!buf) { fclose(f); return NULL; }
    _fseeki64(f, -(int64_t)n, SEEK_END);
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    *out_size = got;
    return buf;
}

/* ---- ID3v2 text frame decoding --------------------------------------- */
/* 解码 ID3v2 文本帧：`data` 指向编码字节，`length` 包含该字节。
 * 支持四种编码方式：0=ISO-8859-1(CP1252)、1=UTF-16(with BOM)、2=UTF-16BE、3=UTF-8 */
static wchar_t *decode_id3_text(const uint8_t *data, size_t length) {
    if (length < 1) return NULL;
    uint8_t enc = data[0];
    const uint8_t *text = data + 1;
    size_t textlen = length - 1;
    /* Strip trailing NULs (frames often have one or two padding NULs).
     * UTF-16 编码必须按 2 字节（一个完整 wchar）为单位剥：逐字节剥会把
     * 正常字符的低字节 0x00 误删（如 ASCII 'X' 的 UTF-16LE 字节 58 00），
     * 产生奇数长度，utf16le_to_wstr 的 len/2 会丢掉半个字符。 */
    if (enc == 1 || enc == 2) {
        while (textlen >= 2 && text[textlen - 2] == 0 && text[textlen - 1] == 0)
            textlen -= 2;
    } else {
        while (textlen > 0 && text[textlen - 1] == 0) textlen--;
    }

    switch (enc) {
    case 0:  /* ISO-8859-1 -> treat as CP1252 (Windows superset) */
        return cp1252_to_wstr((const char *)text, textlen);
    case 1: {  /* UTF-16 with BOM */
        if (textlen < 2) return NULL;
        if (text[0] == 0xFF && text[1] == 0xFE)
            return utf16le_to_wstr(text + 2, textlen - 2);
        if (text[0] == 0xFE && text[1] == 0xFF) {
            /* UTF-16BE: byte-swap into LE then decode. */
            uint8_t *tmp = (uint8_t *)malloc(textlen);
            if (!tmp) return NULL;
            for (size_t i = 0; i + 1 < textlen; i += 2) {
                tmp[i] = text[i + 1];
                tmp[i + 1] = text[i];
            }
            wchar_t *r = utf16le_to_wstr(tmp, textlen);
            free(tmp);
            return r;
        }
        return utf16le_to_wstr(text, textlen);  /* no BOM: assume LE */
    }
    case 2: {  /* UTF-16BE without BOM */
        uint8_t *tmp = (uint8_t *)malloc(textlen);
        if (!tmp) return NULL;
        for (size_t i = 0; i + 1 < textlen; i += 2) {
            tmp[i] = text[i + 1];
            tmp[i + 1] = text[i];
        }
        wchar_t *r = utf16le_to_wstr(tmp, textlen);
        free(tmp);
        return r;
    }
    case 3:  /* UTF-8 */
        return utf8_to_wstr((const char *)text, textlen);
    default:
        return NULL;
    }
}

/* 解析 ID3v2.3 / 2.4 标签头部（buf 以 "ID3" 开头）。
 * 遍历所有帧，提取 TIT2（标题）、TPE1（艺术家）、TALB（专辑）三个文本帧。
 * 仅填充 out 中仍为 NULL 的字段，允许后续 ID3v1 降级补充。 */
static void parse_id3v2(const uint8_t *buf, size_t buf_size, TagInfo *out) {
    if (buf_size < 10 || memcmp(buf, "ID3", 3) != 0) return;
    uint8_t ver_major = buf[3];
    uint8_t flags     = buf[5];
    /* Tag size is synchsafe (7 bits per byte). */
    size_t tag_size = ((size_t)(buf[6] & 0x7F) << 21) |
                      ((size_t)(buf[7] & 0x7F) << 14) |
                      ((size_t)(buf[8] & 0x7F) << 7)  |
                       (size_t)(buf[9] & 0x7F);
    size_t pos = 10;

    /* Skip extended header if present. */
    if (flags & 0x40) {
        if (ver_major == 4) {
            if (buf_size < 14) return;
            /* v2.4 extended header size is synchsafe and includes itself. */
            size_t ext = ((size_t)(buf[10] & 0x7F) << 21) |
                         ((size_t)(buf[11] & 0x7F) << 14) |
                         ((size_t)(buf[12] & 0x7F) << 7)  |
                          (size_t)(buf[13] & 0x7F);
            pos += ext;
        } else {
            if (buf_size < 14) return;
            /* v2.3 extended header size is 4-byte BE, excludes itself. */
            size_t ext = rd_be32(buf + 10);
            pos += 4 + ext;
        }
    }

    size_t end = 10 + tag_size;
    if (end > buf_size) end = buf_size;

    while (pos + 10 <= end) {
        char id[5] = {0};
        memcpy(id, buf + pos, 4);
        if (id[0] == 0) break;  /* padding */

        size_t frame_size;
        if (ver_major == 4) {
            /* v2.4 frame size is synchsafe. */
            frame_size = ((size_t)(buf[pos + 4] & 0x7F) << 21) |
                         ((size_t)(buf[pos + 5] & 0x7F) << 14) |
                         ((size_t)(buf[pos + 6] & 0x7F) << 7)  |
                          (size_t)(buf[pos + 7] & 0x7F);
        } else {
            /* v2.3 frame size is 4-byte BE. */
            frame_size = rd_be32(buf + pos + 4);
        }
        size_t data_start = pos + 10;
        if (data_start + frame_size > end) break;

        if (frame_size > 0) {
            if (memcmp(id, "TIT2", 4) == 0 && !out->title)
                out->title = decode_id3_text(buf + data_start, frame_size);
            else if (memcmp(id, "TPE1", 4) == 0 && !out->artist)
                out->artist = decode_id3_text(buf + data_start, frame_size);
            else if (memcmp(id, "TALB", 4) == 0 && !out->album)
                out->album = decode_id3_text(buf + data_start, frame_size);
        }
        pos = data_start + frame_size;
    }
}

/* 解析 ID3v1 尾部标签（128 字节，以 "TAG" 开头）。
 * 仅作为 ID3v2 未覆盖字段的降级方案，提取标题、艺术家、专辑信息。 */
static void parse_id3v1(const uint8_t *buf, size_t buf_size, TagInfo *out) {
    if (buf_size < 128 || memcmp(buf, "TAG", 3) != 0) return;

    if (!out->title) {
        char tmp[31] = {0};
        memcpy(tmp, buf + 3, 30);
        out->title = cp1252_to_wstr(tmp, strnlen(tmp, 30));
    }
    if (!out->artist) {
        char tmp[31] = {0};
        memcpy(tmp, buf + 33, 30);
        out->artist = cp1252_to_wstr(tmp, strnlen(tmp, 30));
    }
    if (!out->album) {
        char tmp[31] = {0};
        memcpy(tmp, buf + 63, 30);
        out->album = cp1252_to_wstr(tmp, strnlen(tmp, 30));
    }
}

/* ---- MP3 first-frame parsing (bitrate / sample_rate / channels) ------
 * 以下三个表分别用于查找 MPEG Layer III 在不同版本下的比特率和采样率。 */
static const int br_table_l3_v1[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
};
static const int br_table_l3_v2[16] = {
    0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
};
static const int sr_table_mpeg1[4]  = { 44100, 48000, 32000, 0 };
static const int sr_table_mpeg2[4]  = { 22050, 24000, 16000, 0 };
static const int sr_table_mpeg25[4] = { 11025, 12000,  8000, 0 };

/* 跳过可能存在的 ID3v2 标签，查找第一个 MPEG Layer III 同步帧，
 * 解析其比特率、采样率和声道数。成功返回 1，失败返回 0。
 * 采用 CBR 近似：用首帧比特率代表整个文件的比特率。 */
static int parse_mp3_first_frame(const uint8_t *buf, size_t buf_size,
                                 int *bitrate_kbps, int *sample_rate_hz,
                                 int *channels) {
    size_t start = 0;
    if (buf_size >= 10 && memcmp(buf, "ID3", 3) == 0) {
        size_t id3v2size = ((size_t)(buf[6] & 0x7F) << 21) |
                           ((size_t)(buf[7] & 0x7F) << 14) |
                           ((size_t)(buf[8] & 0x7F) << 7)  |
                            (size_t)(buf[9] & 0x7F);
        start = 10 + id3v2size;
        if (buf[5] & 0x10) start += 10;  /* footer */
    }

    for (size_t i = start; i + 4 <= buf_size; i++) {
        if (buf[i] != 0xFF) continue;
        uint8_t b1 = buf[i + 1];
        if ((b1 & 0xE0) != 0xE0) continue;

        /* Version: 00=2.5, 01=reserved, 10=2, 11=1. */
        int ver_bits = (b1 >> 3) & 0x3;
        if (ver_bits == 1) continue;
        int mpeg_version;
        if (ver_bits == 3)      mpeg_version = 1;
        else if (ver_bits == 2) mpeg_version = 2;
        else                    mpeg_version = 25;

        /* Layer: 01=Layer III. We only handle Layer III. */
        int layer_bits = (b1 >> 1) & 0x3;
        if (layer_bits != 1) continue;

        uint8_t b2  = buf[i + 2];
        uint8_t b3  = buf[i + 3];
        int br_idx = (b2 >> 4) & 0xF;
        int sr_idx = (b2 >> 2) & 0x3;
        int chan_idx = (b3 >> 6) & 0x3;

        if (br_idx == 0 || br_idx == 15) continue;
        if (sr_idx == 3) continue;

        int br_kbps = (mpeg_version == 1) ? br_table_l3_v1[br_idx]
                                          : br_table_l3_v2[br_idx];
        if (br_kbps == 0) continue;

        const int *sr_tab = (mpeg_version == 1) ? sr_table_mpeg1 :
                            (mpeg_version == 2) ? sr_table_mpeg2 :
                                                  sr_table_mpeg25;
        int sr = sr_tab[sr_idx];
        if (sr == 0) continue;

        *bitrate_kbps   = br_kbps;
        *sample_rate_hz = sr;
        *channels       = (chan_idx == 3) ? 1 : 2;
        return 1;
    }
    return 0;
}

/* 当 ID3v2 标签（通常因内嵌封面图）超过 TAG_HEADER_BUF 时，头部缓冲里
 * 扫描不到真实帧同步。直接从文件 seek 到标签末尾读取帧头解析。
 * I/O 有界：只读一小块（256 字节）。 */
static void parse_mp3_frame_after_big_tag(const wchar_t *path, size_t id3_total,
                                          int *bitrate_kbps, int *sample_rate_hz,
                                          int *channels) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return;
    if (_fseeki64(f, (__int64)id3_total, SEEK_SET) == 0) {
        uint8_t fh[256];   /* 帧头 4 字节 + 足够余量 */
        size_t n = fread(fh, 1, sizeof(fh), f);
        if (n >= 4)
            parse_mp3_first_frame(fh, n, bitrate_kbps, sample_rate_hz, channels);
    }
    fclose(f);
}

/* ---- Vorbis comment block parsing (shared by FLAC and OGG) ----------- */
/* 解析 Vorbis Comment 数据块（FLAC 和 OGG 共用）。
 * `data` 指向"vorbis"魔数/供应商字段之后的位置，每条注释为 "KEY=VALUE" 格式的 UTF-8 文本。
 * 键名不区分大小写，提取 TITLE、ARTIST、ALBUM 三个字段。 */
static void parse_vorbis_comment(const uint8_t *data, size_t length,
                                 TagInfo *out) {
    if (length < 4) return;
    const uint8_t *p   = data;
    const uint8_t *end = data + length;

    uint32_t vendor_len = rd_le32(p); p += 4;
    if (vendor_len > (uint32_t)(end - p)) return;
    p += vendor_len;
    if (end - p < 4) return;
    uint32_t count = rd_le32(p); p += 4;

    for (uint32_t i = 0; i < count && (size_t)(end - p) >= 4; i++) {
        uint32_t clen = rd_le32(p); p += 4;
        if (clen > (uint32_t)(end - p)) break;
        const char *cs = (const char *)p;
        size_t eq = 0;
        while (eq < clen && cs[eq] != '=') eq++;
        if (eq < clen) {
            const char *key = cs;
            size_t key_len = eq;
            const char *val = cs + eq + 1;
            size_t val_len = clen - eq - 1;
            if (key_len == 5 && _strnicmp(key, "TITLE",  5) == 0 && !out->title)
                out->title  = utf8_to_wstr(val, val_len);
            else if (key_len == 6 && _strnicmp(key, "ARTIST", 6) == 0 && !out->artist)
                out->artist = utf8_to_wstr(val, val_len);
            else if (key_len == 5 && _strnicmp(key, "ALBUM",  5) == 0 && !out->album)
                out->album  = utf8_to_wstr(val, val_len);
        }
        p += clen;
    }
}

/* ---- FLAC parsing ---------------------------------------------------- */
/* 解析 FLAC 文件格式：以 "fLaC" 魔数开头，遍历元数据块。
 * - STREAMINFO（类型 0）：提取采样率、声道数、总样本数，计算精确时长
 * - Vorbis Comment（类型 4）：提取标签字段
 * 最后根据文件大小和时长推算平均比特率。 */
static void parse_flac(const uint8_t *buf, size_t buf_size, TagInfo *out) {
    if (buf_size < 8 || memcmp(buf, "fLaC", 4) != 0) return;
    size_t pos = 4;
    int got_streaminfo = 0;

    while (pos + 4 <= buf_size) {
        uint8_t  hdr        = buf[pos];
        int      block_type = hdr & 0x7F;
        int      is_last    = (hdr >> 7) & 1;
        uint32_t block_len  = ((uint32_t)buf[pos + 1] << 16) |
                              ((uint32_t)buf[pos + 2] << 8)  |
                               (uint32_t)buf[pos + 3];
        pos += 4;
        if (pos + block_len > buf_size) break;

        if (block_type == 0 && !got_streaminfo && block_len >= 18) {
            /* STREAMINFO: skip 10 bytes of min/max block + frame sizes. */
            const uint8_t *si = buf + pos;
            uint32_t sr   = ((uint32_t)si[10] << 12) |
                            ((uint32_t)si[11] << 4)  |
                            ((uint32_t)si[12] >> 4);
            int chan       = ((si[12] >> 1) & 0x7) + 1;
            uint64_t total = ((uint64_t)(si[13] & 0x0F) << 32) |
                             ((uint64_t)si[14] << 24) |
                             ((uint64_t)si[15] << 16) |
                             ((uint64_t)si[16] << 8)  |
                              (uint64_t)si[17];
            out->sample_rate = (int)sr;
            out->channels    = chan;
            if (sr > 0) out->duration = (double)total / sr;
            got_streaminfo = 1;
        } else if (block_type == 4) {
            parse_vorbis_comment(buf + pos, block_len, out);
        }

        pos += block_len;
        if (is_last) break;
    }
    /* Bitrate = file_size * 8 / duration. */
    if (out->duration > 0 && out->file_size > 0)
        out->bitrate = (int)((double)out->file_size * 8.0 / out->duration / 1000.0);
}

/* ---- OGG / Vorbis parsing -------------------------------------------- */
/* 解析 OGG Vorbis 文件：遍历 Ogg 页面，读取前两个包头数据包。
 * - 数据包 0（Identification）：提取声道数和采样率
 * - 数据包 1（Comment）：提取标签字段
 * 时长和比特率在 tag_read() 中通过读取文件尾部的最后一页补齐
 * （见 ogg_duration_from_tail）。 */
static void parse_ogg(const uint8_t *buf, size_t buf_size, TagInfo *out) {
    size_t pos = 0;
    int packet_idx = 0;

    while (pos + 27 <= buf_size) {
        if (memcmp(buf + pos, "OggS", 4) != 0) break;
        uint8_t segs = buf[pos + 26];
        if (pos + 27 + segs > buf_size) break;

        size_t payload_size = 0;
        for (int i = 0; i < segs; i++)
            payload_size += buf[pos + 27 + i];
        size_t payload_start = pos + 27 + segs;
        if (payload_start + payload_size > buf_size) break;

        const uint8_t *payload = buf + payload_start;
        if (packet_idx == 0 && payload_size >= 30 &&
            payload[0] == 1 && memcmp(payload + 1, "vorbis", 6) == 0) {
            /* Identification header: channels at byte 11, sample_rate LE32 at 12. */
            out->channels    = payload[11];
            out->sample_rate = (int)rd_le32(payload + 12);
        } else if (packet_idx == 1 && payload_size >= 7 &&
                   payload[0] == 3 && memcmp(payload + 1, "vorbis", 6) == 0) {
            /* Comment header: skip packet_type byte + "vorbis" magic (7 bytes). */
            parse_vorbis_comment(payload + 7, payload_size - 7, out);
        }

        pos = payload_start + payload_size;
        packet_idx++;
        if (packet_idx >= 2) break;
    }
}

/* 从文件尾部缓冲向前定位最后一个有效 Ogg 页面，读取其 granule position
 * （页头偏移 6，64 位小端），得到 Ogg 流的总样本数，从而在 tag 阶段就算出
 * 精确时长：duration = granule / sample_rate，并推算平均比特率 =
 * file_size * 8 / duration。*/
static void ogg_duration_from_tail(const uint8_t *buf, size_t n, TagInfo *out)
{
    if (!buf || n < 27 || out->sample_rate <= 0) return;
    /* 从后向前扫描：找到的最后一个通过校验的页面即最后页面。
     * 校验：页魔数 OggS + 版本必须为 0 + 保留位（页头字节 5 的高 4 位）
     * 必须清零，避免 Vorbis 注释文本内嵌的 "OggS" 字节串被误判为页头。 */
    for (size_t i = n; i-- > 0; ) {
        if (buf[i] != (uint8_t)'O' || i + 27 > n) continue;
        if (memcmp(buf + i, "OggS", 4) != 0) continue;
        if (buf[i + 4] != 0) continue;              /* version == 0 */
        if (buf[i + 5] & 0xF8) continue;            /* 保留位清零 */
        uint8_t segs = buf[i + 26];
        if (i + 27 + segs > n) continue;   /* segment table 越界，放弃此页 */
        uint64_t granule = (uint64_t)buf[i + 6] |
                           ((uint64_t)buf[i + 7] << 8) |
                           ((uint64_t)buf[i + 8] << 16) |
                           ((uint64_t)buf[i + 9] << 24) |
                           ((uint64_t)buf[i + 10] << 32) |
                           ((uint64_t)buf[i + 11] << 40) |
                           ((uint64_t)buf[i + 12] << 48) |
                           ((uint64_t)buf[i + 13] << 56);
        if (granule == 0) continue;        /* 首页 granule 为 0，跳过 */
        /* 合理性下限：时长不超过 24 小时，防御误匹配产生的天文数字。 */
        if (granule / (uint64_t)out->sample_rate > 24ULL * 3600ULL)
            continue;
        out->duration = (double)granule / out->sample_rate;
        break;
    }
    /* 平均比特率 = 文件总字节 * 8 / 时长（秒）。 */
    if (out->duration > 0 && out->file_size > 0)
        out->bitrate = (int)((double)out->file_size * 8.0 / out->duration / 1000.0);
}

/* ---- WAV / RIFF parsing ---------------------------------------------- */
/* 解析 WAV / RIFF 文件格式：以 "RIFF....WAVE" 开头，遍历各子块。
 * - fmt 子块：提取声道数、采样率、字节率，计算比特率
 * - data 子块：根据数据大小和字节率计算时长
 * - LIST/INFO 子块：提取 INAM（标题）、IART（艺术家）、IPRD（专辑）标签 */
static void parse_wav(const uint8_t *buf, size_t buf_size, TagInfo *out) {
    if (buf_size < 12 || memcmp(buf, "RIFF", 4) != 0 ||
        memcmp(buf + 8, "WAVE", 4) != 0) return;

    size_t pos = 12;
    while (pos + 8 <= buf_size) {
        char chunk_id[5] = {0};
        memcpy(chunk_id, buf + pos, 4);
        uint32_t chunk_size = rd_le32(buf + pos + 4);
        size_t data_start = pos + 8;
        if (data_start + chunk_size > buf_size) break;

        if (memcmp(chunk_id, "fmt ", 4) == 0 && chunk_size >= 16) {
            uint16_t chan      = rd_le16(buf + data_start + 2);
            uint32_t sr        = rd_le32(buf + data_start + 4);
            uint32_t byte_rate = rd_le32(buf + data_start + 8);
            out->channels    = chan;
            out->sample_rate = (int)sr;
            if (byte_rate > 0) out->bitrate = (int)(byte_rate * 8 / 1000);
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            /* Duration = data_size / byte_rate. byte_rate = bitrate * 1000 / 8. */
            if (out->bitrate > 0) {
                double byte_rate = (double)out->bitrate * 1000.0 / 8.0;
                if (byte_rate > 0)
                    out->duration = (double)chunk_size / byte_rate;
            }
        } else if (memcmp(chunk_id, "LIST", 4) == 0 && chunk_size >= 4 &&
                   memcmp(buf + data_start, "INFO", 4) == 0) {
            /* INFO sub-chunks: INAM (title) / IART (artist) / IPRD (album). */
            size_t sub_pos = data_start + 4;
            size_t sub_end = data_start + chunk_size;
            while (sub_pos + 8 <= sub_end && sub_pos + 8 <= buf_size) {
                char sid[5] = {0};
                memcpy(sid, buf + sub_pos, 4);
                uint32_t ssize = rd_le32(buf + sub_pos + 4);
                size_t sdata = sub_pos + 8;
                if (sdata + ssize > sub_end) break;
                if (ssize > 0) {
                    size_t slen = ssize;
                    while (slen > 0 && buf[sdata + slen - 1] == 0) slen--;
                    if (memcmp(sid, "INAM", 4) == 0 && !out->title)
                        out->title  = cp1252_to_wstr((const char *)buf + sdata, slen);
                    else if (memcmp(sid, "IART", 4) == 0 && !out->artist)
                        out->artist = cp1252_to_wstr((const char *)buf + sdata, slen);
                    else if (memcmp(sid, "IPRD", 4) == 0 && !out->album)
                        out->album  = cp1252_to_wstr((const char *)buf + sdata, slen);
                }
                sub_pos = sdata + ssize;
                if (sub_pos & 1) sub_pos++;   /* word-align sub-chunks */
            }
        }

        pos = data_start + chunk_size;
        if (pos & 1) pos++;   /* word-align top-level chunks */
    }
}

/* ---- Public entry point ---------------------------------------------- */
/* 读取指定音频文件的标签信息和技术参数。
 * 根据 format 参数（FMT_MP3=1, FMT_FLAC=2, FMT_WAV=3, FMT_OGG=4）选择对应的解析器。
 * 先读取文件头部（最多 TAG_HEADER_BUF 字节）到缓冲区，再分发给各格式解析函数。
 * 对 MP3 文件，还会尾部读取 128 字节作为 ID3v1 降级方案。
 * 对 OGG 文件，读取文件尾部以解析最后一页的 granule position，补齐时长和比特率。
 * 成功返回 0，失败返回 -1。调用者必须使用 tag_info_free() 释放 out 中的堆字符串。 */
int tag_read(const wchar_t *path, int format, TagInfo *out) {
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->file_size = get_file_size(path);

    size_t hdr_size = 0;
    uint8_t *hdr = slurp_head(path, TAG_HEADER_BUF, &hdr_size);
    if (!hdr) return -1;

    switch (format) {
    case 1: {  /* FMT_MP3 */
        parse_id3v2(hdr, hdr_size, out);
        /* ID3v2 标签总字节数（10 头 + 标签体 + 可选 footer）。 */
        size_t id3_total = 0;
        if (hdr_size >= 10 && memcmp(hdr, "ID3", 3) == 0) {
            size_t id3sz = ((size_t)(hdr[6] & 0x7F) << 21) |
                           ((size_t)(hdr[7] & 0x7F) << 14) |
                           ((size_t)(hdr[8] & 0x7F) << 7)  |
                            (size_t)(hdr[9] & 0x7F);
            id3_total = 10 + id3sz;
            if (hdr[5] & 0x10) id3_total += 10;  /* footer */
        }
        int br = 0, sr = 0, ch = 0;
        if (!parse_mp3_first_frame(hdr, hdr_size, &br, &sr, &ch) && id3_total > 0) {
            /* 超大 ID3v2 标签（内嵌封面图）超出头部缓冲，seek 到标签末尾重试。 */
            parse_mp3_frame_after_big_tag(path, id3_total, &br, &sr, &ch);
        }
        if (br > 0 && sr > 0) {
            out->bitrate     = br;
            out->sample_rate = sr;
            out->channels    = ch;
            /* CBR approximation: 音频字节数 × 8 / 比特率。
             * 扣掉 ID3v2 标签体积，更接近真实音频大小。 */
            long long audio_bytes = (long long)out->file_size - (long long)id3_total;
            if (audio_bytes > 0)
                out->duration = (double)audio_bytes * 8.0 / ((double)br * 1000.0);
        }
        /* ID3v1 fallback (only for fields ID3v2 didn't fill). */
        if ((!out->title || !out->artist || !out->album) && out->file_size >= 128) {
            uint8_t tail[128];
            if (read_tail(path, tail, 128) == 128)
                parse_id3v1(tail, 128, out);
        }
        break;
    }
    case 2:  parse_flac(hdr, hdr_size, out); break;  /* FMT_FLAC */
    case 3:  parse_wav (hdr, hdr_size, out); break;  /* FMT_WAV  */
    case 4: {                                        /* FMT_OGG  */
        parse_ogg(hdr, hdr_size, out);
        /* OGG 的精确时长记录在最后一个页面的 granule position 中，头部
         * 解析不到；读取文件尾部补齐时长，并推算平均比特率。 */
        if (out->sample_rate > 0 && out->file_size > 0) {
            size_t tsz = 0;
            uint8_t *tail = slurp_tail(path, TAG_HEADER_BUF, &tsz);
            if (tail) {
                ogg_duration_from_tail(tail, tsz, out);
                free(tail);
            }
        }
        break;
    }
    default:
        free(hdr);
        return -1;
    }

    free(hdr);
    return 0;
}

/* 释放 TagInfo 结构体中堆分配的字符串，并将结构体清零。
 * 不释放 info 指针本身——调用者负责管理 info 的生命周期。 */
void tag_info_free(TagInfo *info) {
    if (!info) return;
    free(info->title);
    free(info->artist);
    free(info->album);
    memset(info, 0, sizeof(*info));
}
