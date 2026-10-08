/*
 * win32_ui.c - Main window, child controls, DPI handling and the message
 * pump's window procedure.
 *
 * The UI thread owns every HWND and only ever touches player state through
 * the control API in audio_waveout.c (or by reading a few fields under
 * g_player.cs). All heavy work - decoding, waveOut submission, seeking -
 * happens on the two worker threads.
 *
 * DPI: Per-Monitor V2. On WM_DPICHANGED the font is rebuilt and every
 * control is relayouted with DPIX()-scaled metrics. The manifest (and a
 * runtime fallback in main.c) declares PER_MONITOR_AWARE_V2 so the window
 * receives real per-monitor DPI rather than a system-wide lie.
 */
#include "mp_player.h"
#include "win32_ui.h"
#include "playlist.h"
#include "audio_waveout.h"
#include "audio_decode.h"
#include "tag_reader.h"

#include <commctrl.h>
#include <commdlg.h>     /* GetOpenFileNameW */
#include <shellapi.h>    /* DragQueryFileW / WM_DROPFILES */
#include <shlobj.h>      /* SHBrowseForFolderW */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define MAIN_CLASS_NAME L"CPlayerMainWindowClass"

/* ======================================================================
 *  Modern design system
 * ======================================================================
 * A small, self-contained GDI toolkit that gives the player a flat,
 * web-like look without pulling in any heavy framework:
 *
 *   - a single palette (light theme, soft blue accent)
 *   - rounded rectangles with real anti-aliasing, produced by drawing
 *     into a 32-bit DIB section and scaling it down 4x on AlphaBlend
 *   - text drawn with GDI (ClearType) on top of the anti-aliased shapes
 *
 * Everything here costs only a handful of KB of code; the shapes are
 * cached per (size, radius) so repeated paints are cheap.
 * ====================================================================== */

/* ---- Palette ---------------------------------------------------------
 * Light premium system. Soft low-saturation neutrals + one blue accent.
 * All chrome reads macros only, so the whole look retunes from here.
 * Light client matches the system title bar / menu exactly, so the
 * window reads as one piece instead of two stacked styles. */
#define CLR_ACCENT        RGB( 55, 138, 221)   /* primary blue            */
#define CLR_ACCENT_DARK   RGB( 40, 110, 185)   /* pressed                 */
#define CLR_ACCENT_HOVER  RGB( 47, 124, 203)   /* primary blue, hovered   */
#define CLR_ACCENT_SOFT   RGB(230, 241, 251)   /* tinted fill             */
#define CLR_ACCENT_SOFT_PRESS RGB(212, 228, 246)
#define CLR_ACCENT_SOFT_HOVER RGB(219, 234, 248) /* tinted fill, hovered  */
#define CLR_ACCENT_EDGE   RGB(181, 212, 244)   /* tinted border           */
#define CLR_ACCENT_TEXT   RGB( 24,  95, 165)

#define CLR_SURFACE       RGB(255, 255, 255)   /* cards, list background  */
#define CLR_SURFACE2      RGB(237, 241, 247)   /* raised tile / ghost art */
#define CLR_PAGE          RGB(244, 246, 250)   /* window background       */
#define CLR_BORDER        RGB(226, 230, 236)   /* hairline borders        */
#define CLR_ROW_LINE      RGB(239, 242, 246)   /* list row separators     */
#define CLR_TRACK         RGB(225, 231, 240)   /* slider groove           */

#define CLR_TEXT          RGB( 31,  38,  48)   /* primary text            */
#define CLR_TEXT_MUTED    RGB(107, 117, 128)   /* secondary text          */
#define CLR_TEXT_FAINT    RGB(160, 168, 178)   /* placeholders / dashes   */

#define CLR_OK            RGB( 29, 158, 117)   /* status dot: playing     */
#define CLR_WARN          RGB(239, 159,  39)   /* status dot: paused      */
#define CLR_IDLE          RGB(180, 188, 198)   /* status dot: stopped     */

#define CLR_HOVER_BG      RGB(241, 245, 250)   /* row / button hover      */
#define CLR_SEL_BG        RGB(230, 241, 251)   /* selected / playing row  */

/* Horizontal gutter shared by the list's header labels and its row cells.
 *
 * The report view insets the text it draws inside a cell by roughly the same
 * margin on every side, but the header item rect spans the full column - it
 * carries none of that inset. Because the header is custom-drawn it has to
 * re-apply the margin itself, otherwise a label and the value beneath it end
 * up several pixels apart.
 *
 * COL_CELL_INSET is that margin, measured rather than guessed: rendering a
 * CJK-leading cell and a right-aligned numeric cell at 168 dpi showed the
 * control reserving 7 device px on each edge, i.e. 4 DIP. Both edges of the
 * header label use it, so the alignment edge of a label coincides with the
 * alignment edge of its column's text at any DPI.
 *
 * COL_GUTTER is the wider gutter the row custom draw uses for its cell text
 * and its separator hairline. */
#define COL_GUTTER        14                   /* in 96-dpi units         */
#define COL_CELL_INSET    4                    /* cell text inset, 96 dpi */

/* ---- Anti-aliased rounded rectangle ----------------------------------
 * GDI's RoundRect() has hard, jagged corners. To get a smooth radius we
 * render the shape supersampled 4x into an off-screen 32-bit DIB, then
 * AlphaBlend it down. The result is cached by (w, h, radius, colour) so
 * a button only ever pays the cost once per resize. */

typedef struct {
    int      w, h, r;          /* cached geometry (device px)             */
    COLORREF fill;             /* fill colour of the cached bitmap        */
    HBITMAP  bmp;              /* 32-bit premultiplied DIB                */
    HDC      memdc;
    HBITMAP  oldbmp;
} RoundedCache;

#define ROUNDED_CACHE_N 32
static RoundedCache g_rc[ROUNDED_CACHE_N];
static int          g_rc_next = 0;

/* ---- Corner tile cache ------------------------------------------------
 * Painting a large rounded surface (the playlist card) with the supersampled
 * whole-shape path would allocate a w*SS x h*SS 32-bit DIB - for the card
 * that is roughly 55 MB per shape, rebuilt on every window resize. Yet a
 * rounded rectangle only differs from a plain one inside its four corners.
 *
 * So for large shapes we draw the interior as flat fills and rasterise a
 * single (radius x radius) corner tile, then stamp it (mirrored as needed)
 * into each corner. The cache entry is then O(radius^2) - a few KB - no
 * matter how large the surface is, and no per-pixel loop over the interior.
 *
 * The tile is stored already split into four quadrant masks so each corner
 * can be blended without any runtime mirroring. */
typedef struct {
    int      r;                /* corner radius (device px)               */
    COLORREF fill;             /* surface fill colour                     */
    HDC      dc[4];            /* 0=TL 1=TR 2=BL 3=BR, 32-bit premult.    */
    HBITMAP  bmp[4];
    HBITMAP  old[4];
} CornerCache;

#define CORNER_CACHE_N 6
static CornerCache g_cc[CORNER_CACHE_N];
static int          g_cc_next = 0;


/* Drop every cached shape (called on DPI change / theme change). */
static void rounded_flush(void)
{
    for (int i = 0; i < ROUNDED_CACHE_N; i++) {
        if (g_rc[i].bmp) {
            if (g_rc[i].memdc && g_rc[i].oldbmp)
                SelectObject(g_rc[i].memdc, g_rc[i].oldbmp);
            if (g_rc[i].memdc) DeleteDC(g_rc[i].memdc);
            DeleteObject(g_rc[i].bmp);
        }
        memset(&g_rc[i], 0, sizeof(g_rc[i]));
    }
    g_rc_next = 0;

    for (int i = 0; i < CORNER_CACHE_N; i++) {
        for (int q = 0; q < 4; q++) {
            if (g_cc[i].dc[q] && g_cc[i].old[q])
                SelectObject(g_cc[i].dc[q], g_cc[i].old[q]);
            if (g_cc[i].dc[q])  DeleteDC(g_cc[i].dc[q]);
            if (g_cc[i].bmp[q]) DeleteObject(g_cc[i].bmp[q]);
        }
        memset(&g_cc[i], 0, sizeof(g_cc[i]));
    }
    g_cc_next = 0;
}

/* Fill a rectangle with a solid colour (small helper used a lot).
 *
 * The colour goes through the DC brush instead of a freshly created solid
 * brush. This runs dozens of times per paint - the row painter alone calls it
 * twice per visible row - and allocating and destroying a GDI object on every
 * call was pure handle churn. DC_BRUSH is a stock object, so nothing is
 * allocated and nothing needs deleting. Nothing else in this file depends on
 * the DC brush colour; the rounded-corner and slider paths all pass explicit
 * brushes of their own. */
static void fill_rect(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    if (w <= 0 || h <= 0) return;
    RECT rc = { x, y, x + w, y + h };
    SetDCBrushColor(dc, c);
    FillRect(dc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
}

/* Build (or fetch) the four corner quadrants for `radius` / `fill`.
 * Each quadrant is `radius x radius` device pixels and holds premultiplied
 * BGRA coverage of the rounded corner, drawn at 4x and downsampled by the
 * AlphaBlend that consumes it. Returns NULL only if GDI allocation failed. */
static CornerCache *corner_tile_get(int radius, COLORREF fill)
{
    if (radius < 1) return NULL;

    for (int i = 0; i < CORNER_CACHE_N; i++) {
        if (g_cc[i].bmp[0] && g_cc[i].r == radius && g_cc[i].fill == fill)
            return &g_cc[i];
    }

    CornerCache *slot = &g_cc[g_cc_next];
    g_cc_next = (g_cc_next + 1) % CORNER_CACHE_N;
    for (int q = 0; q < 4; q++) {
        if (slot->dc[q] && slot->old[q]) SelectObject(slot->dc[q], slot->old[q]);
        if (slot->dc[q])  DeleteDC(slot->dc[q]);
        if (slot->bmp[q]) DeleteObject(slot->bmp[q]);
    }
    memset(slot, 0, sizeof(*slot));

    const int SS = 4;
    int side = radius;                /* quadrant edge, device px         */
    int ss   = side * SS;             /* supersampled quadrant edge        */

    for (int q = 0; q < 4; q++) {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = ss;
        bi.bmiHeader.biHeight      = -ss;      /* top-down */
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void *bits = NULL;
        HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits,
                                       NULL, 0);
        if (!bmp || !bits) { if (bmp) DeleteObject(bmp); return NULL; }

        HDC mdc = CreateCompatibleDC(NULL);
        if (!mdc) { DeleteObject(bmp); return NULL; }
        HBITMAP old = (HBITMAP)SelectObject(mdc, bmp);

        RECT full = { 0, 0, ss, ss };
        FillRect(mdc, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));

        /* Paint the full rounded-rect at 4x, offset so that exactly the
         * requested quadrant lands in this tile. */
        HGDIOBJ ob = SelectObject(mdc, GetStockObject(WHITE_BRUSH));
        HGDIOBJ op = SelectObject(mdc, GetStockObject(NULL_PEN));
        int full_d = ss * 2;                    /* whole shape side    */
        int dx = (q == 1 || q == 3) ? -(full_d - ss) : 0;   /* right half  */
        int dy = (q == 2 || q == 3) ? -(full_d - ss) : 0;   /* bottom half */
        int dia = radius * SS * 2;
        RoundRect(mdc, dx, dy, dx + full_d, dy + full_d, dia, dia);
        SelectObject(mdc, op);
        SelectObject(mdc, ob);

        /* Coverage -> premultiplied fill colour. */
        unsigned char *p = (unsigned char *)bits;
        int r = GetRValue(fill), g = GetGValue(fill), b = GetBValue(fill);
        for (size_t n = 0, cnt = (size_t)ss * ss; n < cnt; n++) {
            unsigned char a = p[n * 4 + 0];     /* coverage in the B channel */
            p[n * 4 + 0] = (unsigned char)(b * a / 255);
            p[n * 4 + 1] = (unsigned char)(g * a / 255);
            p[n * 4 + 2] = (unsigned char)(r * a / 255);
            p[n * 4 + 3] = a;
        }

        slot->dc[q]  = mdc;
        slot->bmp[q] = bmp;
        slot->old[q] = old;
    }

    slot->r    = radius;
    slot->fill = fill;
    return slot;
}

/* Large rounded rectangles are painted as a nine-patch: a flat interior,
 * four flat edges and four corner tiles. Memory and time are then O(radius)
 * rather than O(width * height). */
static void draw_round_rect_ninepatch(HDC dc, int x, int y, int w, int h,
                                      int radius, COLORREF fill)
{
    CornerCache *cc = corner_tile_get(radius, fill);
    if (!cc) {                              /* fall back to a plain fill */
        fill_rect(dc, x, y, w, h, fill);
        return;
    }

    int r = radius;
    /* Interior + edges: three horizontal bands, each a solid fill. */
    fill_rect(dc, x + r,     y,         w - r * 2, r,         fill); /* top    */
    fill_rect(dc, x,         y + r,     w,         h - r * 2, fill); /* middle */
    fill_rect(dc, x + r,     y + h - r, w - r * 2, r,         fill); /* bottom */

    BLENDFUNCTION bf;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;

    int saved = SaveDC(dc);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    const int SS = 4;
    AlphaBlend(dc, x,         y,         r, r, cc->dc[0], 0, 0, r * SS, r * SS, bf);
    AlphaBlend(dc, x + w - r, y,         r, r, cc->dc[1], 0, 0, r * SS, r * SS, bf);
    AlphaBlend(dc, x,         y + h - r, r, r, cc->dc[2], 0, 0, r * SS, r * SS, bf);
    AlphaBlend(dc, x + w - r, y + h - r, r, r, cc->dc[3], 0, 0, r * SS, r * SS, bf);
    RestoreDC(dc, saved);
}

/* Above this many device pixels on the longer side, a rounded rectangle is
 * painted with the nine-patch path instead of a whole-shape supersampled
 * mask. Below it the supersampled cache is both higher quality (the mask
 * covers the whole edge, not just the corners) and cheap. */
#define ROUNDED_MASK_MAX 128

/* Draw a filled rounded rectangle with an anti-aliased border.
 * (x, y, w, h) is in device pixels. radius is in device pixels too. */
static void draw_round_rect(HDC dc, int x, int y, int w, int h,
                            int radius, COLORREF fill, COLORREF border)
{
    if (w <= 0 || h <= 0) return;

    /* Supersample factor: 4x in each axis. Used both when building the
     * coverage mask and when sampling it back in AlphaBlend. */
    const int SS = 4;

    /* Clamp radius so it never exceeds half of the shorter side. */
    int maxr = (w < h ? w : h) / 2;
    if (radius > maxr) radius = maxr;
    if (radius < 0) radius = 0;

    /* Radius 0 or a huge shape: fall back to plain rectangle fills - the
     * supersampled path is only worth it for visible corner curvature. */
    if (radius == 0) {
        HBRUSH b = CreateSolidBrush(fill);
        RECT rc = { x, y, x + w, y + h };
        FillRect(dc, &rc, b);
        DeleteObject(b);
        if (border != fill) {
            HBRUSH e = CreateSolidBrush(border);
            FrameRect(dc, &rc, e);
            DeleteObject(e);
        }
        return;
    }

    /* Large surfaces: paint as a nine-patch so the memory cost stays
     * O(radius^2) instead of allocating a supersampled bitmap the size of
     * the whole shape. The card alone is ~1200x720 device px, which at 4x
     * with 32-bit pixels would be ~55 MB - rebuilt on every resize, which
     * is exactly what made dragging the window feel heavy. */
    if (w > ROUNDED_MASK_MAX || h > ROUNDED_MASK_MAX) {
        draw_round_rect_ninepatch(dc, x, y, w, h, radius, fill);
        if (border != fill) {
            HGDIOBJ opn = SelectObject(dc, GetStockObject(NULL_BRUSH));
            HPEN pen = CreatePen(PS_SOLID, 1, border);
            HGDIOBJ open = SelectObject(dc, pen);
            int rr = radius * 2;
            RoundRect(dc, x, y, x + w, y + h, rr, rr);
            SelectObject(dc, open);
            SelectObject(dc, opn);
            DeleteObject(pen);
        }
        return;
    }

    /* --- Look up / build the cached supersampled shape --- */
    RoundedCache *slot = NULL;
    for (int i = 0; i < ROUNDED_CACHE_N; i++) {
        if (g_rc[i].bmp && g_rc[i].w == w && g_rc[i].h == h &&
            g_rc[i].r == radius && g_rc[i].fill == fill) {
            slot = &g_rc[i];
            break;
        }
    }
    if (!slot) {
        slot = &g_rc[g_rc_next];
        g_rc_next = (g_rc_next + 1) % ROUNDED_CACHE_N;
        if (slot->bmp) {
            if (slot->memdc && slot->oldbmp)
                SelectObject(slot->memdc, slot->oldbmp);
            if (slot->memdc) DeleteDC(slot->memdc);
            DeleteObject(slot->bmp);
            memset(slot, 0, sizeof(*slot));
        }

        /* Supersample factor: 4x in each axis (declared at the top). */
        int sw = w * SS, sh = h * SS;

        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = sw;
        bi.bmiHeader.biHeight      = -sh;      /* top-down */
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void *bits = NULL;
        HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits,
                                       NULL, 0);
        if (!bmp || !bits) {
            if (bmp) DeleteObject(bmp);
            /* Degrade gracefully to the hard-edged built-in. */
            HBRUSH b = CreateSolidBrush(fill);
            RECT rc = { x, y, x + w, y + h };
            FillRect(dc, &rc, b);
            DeleteObject(b);
            return;
        }

        HDC mdc = CreateCompatibleDC(NULL);
        if (!mdc) { DeleteObject(bmp); return; }
        HBITMAP old = (HBITMAP)SelectObject(mdc, bmp);

        /* Paint the supersampled coverage mask in white-on-black, then use
         * the coverage as an alpha value for the fill colour. Doing it with
         * a plain GDI round rect at 4x is more than enough fidelity for a
         * UI radius and keeps the code tiny. */
        RECT full = { 0, 0, sw, sh };
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        FillRect(mdc, &full, black);
        HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
        HGDIOBJ oldBr = SelectObject(mdc, white);
        HGDIOBJ oldPn = SelectObject(mdc, GetStockObject(NULL_PEN));
        RoundRect(mdc, 0, 0, sw, sh, radius * SS * 2, radius * SS * 2);
        SelectObject(mdc, oldPn);
        SelectObject(mdc, oldBr);

        /* Convert the coverage mask into premultiplied RGBA of `fill`.
         * The DIB is BGRA byte order on little-endian Windows. */
        unsigned char *p = (unsigned char *)bits;
        int r = GetRValue(fill), g = GetGValue(fill), b = GetBValue(fill);
        for (size_t i = 0, n = (size_t)sw * sh; i < n; i++) {
            unsigned char a = p[i * 4 + 0];          /* coverage in B chan */
            p[i * 4 + 0] = (unsigned char)(b * a / 255);
            p[i * 4 + 1] = (unsigned char)(g * a / 255);
            p[i * 4 + 2] = (unsigned char)(r * a / 255);
            p[i * 4 + 3] = a;
        }

        slot->w = w; slot->h = h; slot->r = radius; slot->fill = fill;
        slot->bmp = bmp; slot->memdc = mdc; slot->oldbmp = old;
    }

    /* --- Composite the cached shape at (x, y) ---
     * The mask lives on a 4x supersampled surface, so the *source* extent
     * handed to AlphaBlend must be the supersampled size; asking for w x h
     * would sample only the top-left quarter of the mask and blow it back
     * up, which turns every rounded corner into a giant warped arc. */
    BLENDFUNCTION bf;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;

    int saved = SaveDC(dc);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    AlphaBlend(dc, x, y, w, h, slot->memdc, 0, 0, w * SS, h * SS, bf);
    RestoreDC(dc, saved);

    /* --- Hairline border, drawn as a rounded frame --- */
    if (border != fill) {
        HGDIOBJ opn = SelectObject(dc, GetStockObject(NULL_BRUSH));
        HPEN pen = CreatePen(PS_SOLID, 1, border);
        HGDIOBJ open = SelectObject(dc, pen);
        /* Inset by half a pixel so the 1px pen lands inside the shape. */
        int rr = radius * 2;
        RoundRect(dc, x, y, x + w, y + h, rr, rr);
        SelectObject(dc, open);
        SelectObject(dc, opn);
        DeleteObject(pen);
    }
}

/* Draw a 1px horizontal hairline. */
static void hline(HDC dc, int x, int y, int w, COLORREF c)
{
    fill_rect(dc, x, y, w, 1, c);
}


/* ---- Language string table ------------------------------------------- */
typedef struct {
    /* Menu */
    const wchar_t *m_file, *m_lang, *m_help;
    const wchar_t *m_add_files, *m_add_folder;
    const wchar_t *m_load_m3u, *m_save_m3u;
    const wchar_t *m_clear, *m_exit, *m_about;
    /* Buttons */
    const wchar_t *b_prev, *b_play, *b_pause, *b_stop, *b_next;
    const wchar_t *mode_ll, *mode_sl, *mode_sh;
    /* Status */
    const wchar_t *s_playing, *s_paused, *s_stopped, *s_ready;
    const wchar_t *s_playing_title;     /* L"正在播放: %s" / "Playing: %s" */
    const wchar_t *s_added_files;       /* L"已添加 %d 个文件" */
    const wchar_t *s_dropped_files;     /* L"已拖入 %d 个文件" */
    const wchar_t *s_no_audio;          /* L"未找到支持的音频文件" */
    const wchar_t *s_loaded_m3u;        /* L"已从 M3U 读取 %d 个文件" */
    const wchar_t *s_no_audio_m3u;      /* L"M3U 中没有可识别的音频文件" */
    const wchar_t *s_saved_m3u;         /* L"已保存 M3U: %s" */
    /* Labels */
    const wchar_t *l_volume;            /* L"音量: %d%%" */
    const wchar_t *l_empty_hint;        /* playlist empty-state hint */
    /* Dialogs */
    const wchar_t *d_about_title, *d_about_content;
    const wchar_t *d_window_title;
    const wchar_t *d_empty_playlist, *d_save_failed;
    const wchar_t *d_reg_failed, *d_create_failed;
    const wchar_t *d_alloc_buf, *d_start_thread;
    const wchar_t *d_err_title, *d_info_title;
    const wchar_t *d_err_index, *d_err_open, *d_err_device, *d_err_output;
    const wchar_t *d_add_title, *d_folder_title;
    const wchar_t *d_load_m3u_title, *d_save_m3u_title;
    /* File dialog filters */
    const wchar_t *f_audio;
    const wchar_t *f_m3u_load, *f_m3u_save, *f_m3u_all;
    /* ListView columns */
    const wchar_t *c_title, *c_artist, *c_album, *c_duration;
    const wchar_t *c_bitrate, *c_format, *c_size;
} LangStrings;

/* 简体中文语言包。 */
static const LangStrings LANG_ZH_STRINGS = {
    /* Menu */
    L"文件(&F)", L"语言(&L)", L"帮助(&H)",
    L"添加文件...", L"添加文件夹...",
    L"读取列表...", L"保存列表...",
    L"清空列表", L"退出", L"关于...",
    /* Buttons */
    L"上一首", L"播放", L"暂停", L"停止", L"下一首",
    L"模式: 列表循环", L"模式: 单曲循环", L"模式: 随机播放",
    /* Status */
    L"正在播放", L"已暂停", L"已停止", L"就绪",
    L"正在播放: %s",
    L"已添加 %d 个文件",
    L"已拖入 %d 个文件",
    L"未找到支持的音频文件",
    L"已从 M3U 读取 %d 个文件",
    L"M3U 中没有可识别的音频文件",
    L"已保存 M3U: %s",
    /* Labels */
    L"音量: %d%%",
    L"播放列表为空 — 将音频文件拖入窗口，或通过「文件」菜单添加",
    /* Dialogs */
    L"关于 CPlayer",
    L"CPlayer - 极致轻量本地音乐播放器\n"
    L"纯 C99 + Win32 API\n"
    L"MP3 / FLAC / WAV / OGG",
    L"CPlayer - 音乐播放器",
    L"播放列表为空，无法保存。",
    L"保存 M3U 失败。",
    L"窗口类注册失败",
    L"窗口创建失败",
    L"无法分配音频缓冲区",
    L"无法启动音频线程",
    L"CPlayer", L"CPlayer",
    L"无效的曲目索引",
    L"无法打开文件（格式不支持或文件损坏）",
    L"无法打开音频输出设备",
    L"音频输出失败（设备可能已断开）",
    L"添加音频文件",
    L"选择包含音频文件的文件夹",
    L"读取 M3U 播放列表",
    L"保存 M3U 播放列表",
    /* File dialog filters */
    L"音频文件 (*.mp3;*.flac;*.wav;*.ogg)\0"
    L"*.mp3;*.flac;*.wav;*.ogg\0"
    L"MP3 (*.mp3)\0*.mp3\0"
    L"FLAC (*.flac)\0*.flac\0"
    L"WAV (*.wav;*.wave)\0*.wav;*.wave\0"
    L"OGG (*.ogg;*.oga)\0*.ogg;*.oga\0"
    L"所有文件 (*.*)\0*.*\0\0",
    L"M3U 播放列表 (*.m3u;*.m3u8)\0*.m3u;*.m3u8\0",
    L"M3U 播放列表 (*.m3u)\0*.m3u\0",
    L"所有文件 (*.*)\0*.*\0\0",
    /* ListView columns */
    L"标题", L"艺术家", L"专辑", L"时长",
    L"比特率", L"格式", L"大小",
};

/* English language pack. */
static const LangStrings LANG_EN_STRINGS = {
    /* Menu */
    L"File(&F)", L"Language(&L)", L"Help(&H)",
    L"Add files...", L"Add folder...",
    L"Load playlist...", L"Save playlist...",
    L"Clear list", L"Exit", L"About...",
    /* Buttons */
    L"Previous", L"Play", L"Pause", L"Stop", L"Next",
    L"Mode: List Loop", L"Mode: Single Loop", L"Mode: Shuffle",
    /* Status */
    L"Playing", L"Paused", L"Stopped", L"Ready",
    L"Playing: %s",
    L"Added %d file(s)",
    L"Dropped %d file(s)",
    L"No supported audio files found",
    L"Loaded %d file(s) from M3U",
    L"No audio files recognized in M3U",
    L"Saved M3U: %s",
    /* Labels */
    L"Volume: %d%%",
    L"Playlist is empty \x2014 drag audio files here, or use the File menu",
    /* Dialogs */
    L"About CPlayer",
    L"CPlayer - Ultra-lightweight Local Music Player\n"
    L"Pure C99 + Win32 API\n"
    L"MP3 / FLAC / WAV / OGG",
    L"CPlayer - Music Player",
    L"Playlist is empty, nothing to save.",
    L"Failed to save M3U.",
    L"Window class registration failed",
    L"Window creation failed",
    L"Failed to allocate audio buffer",
    L"Failed to start audio thread",
    L"CPlayer", L"CPlayer",
    L"Invalid track index",
    L"Cannot open file (unsupported format or corrupt file)",
    L"Cannot open audio output device",
    L"Audio output failed (device may be disconnected)",
    L"Add audio files",
    L"Select a folder containing audio files",
    L"Load M3U playlist",
    L"Save M3U playlist",
    /* File dialog filters */
    L"Audio Files (*.mp3;*.flac;*.wav;*.ogg)\0"
    L"*.mp3;*.flac;*.wav;*.ogg\0"
    L"MP3 (*.mp3)\0*.mp3\0"
    L"FLAC (*.flac)\0*.flac\0"
    L"WAV (*.wav;*.wave)\0*.wav;*.wave\0"
    L"OGG (*.ogg;*.oga)\0*.ogg;*.oga\0"
    L"All Files (*.*)\0*.*\0\0",
    L"M3U Playlist (*.m3u;*.m3u8)\0*.m3u;*.m3u8\0",
    L"M3U Playlist (*.m3u)\0*.m3u\0",
    L"All Files (*.*)\0*.*\0\0",
    /* ListView columns */
    L"Title", L"Artist", L"Album", L"Duration",
    L"Bitrate", L"Format", L"Size",
};

/* Spanish language pack. */
static const LangStrings LANG_ES_STRINGS = {
    /* Menu */
    L"Archivo(&F)", L"Idioma(&L)", L"Ayuda(&H)",
    L"Agregar archivos...", L"Agregar carpeta...",
    L"Cargar lista...", L"Guardar lista...",
    L"Limpiar lista", L"Salir", L"Acerca de...",
    /* Buttons */
    L"Anterior", L"Reproducir", L"Pausa", L"Detener", L"Siguiente",
    L"Modo: Lista", L"Modo: Una vez", L"Modo: Aleatorio",
    /* Status */
    L"Reproduciendo", L"En pausa", L"Detenido", L"Listo",
    L"Reproduciendo: %s",
    L"Agregado %d archivo(s)",
    L"Soltado %d archivo(s)",
    L"No se encontraron archivos de audio",
    L"Cargado %d archivo(s) desde M3U",
    L"No se reconocieron archivos de audio en M3U",
    L"Guardado M3U: %s",
    /* Labels */
    L"Volumen: %d%%",
    L"Lista vac\x00eda \x2014 arrastra archivos de audio aqu\x00ed o usa el men\x00fa Archivo",
    /* Dialogs */
    L"Acerca de CPlayer",
    L"CPlayer - Reproductor de m\x00fasica local ultraligero\n"
    L"C99 puro + Win32 API\n"
    L"MP3 / FLAC / WAV / OGG",
    L"CPlayer - Reproductor de m\x00fasica",
    L"La lista de reproducci\x00f3n est\x00e1 vac\x00ed" L"a, no hay nada que guardar.",
    L"Error al guardar M3U.",
    L"Error al registrar la clase de ventana",
    L"Error al crear la ventana",
    L"Error al asignar el b\x00fafer de audio",
    L"Error al iniciar el hilo de audio",
    L"CPlayer", L"CPlayer",
    L"Índice de pista no válido",
    L"No se puede abrir el archivo (formato no compatible o archivo dañado)",
    L"No se puede abrir el dispositivo de salida de audio",
    L"Error de salida de audio (el dispositivo puede estar desconectado)",
    L"Agregar archivos de audio",
    L"Seleccione una carpeta que contenga archivos de audio",
    L"Cargar lista de reproducci\x00f3n M3U",
    L"Guardar lista de reproducci\x00f3n M3U",
    /* File dialog filters */
    L"Archivos de audio (*.mp3;*.flac;*.wav;*.ogg)\0"
    L"*.mp3;*.flac;*.wav;*.ogg\0"
    L"MP3 (*.mp3)\0*.mp3\0"
    L"FLAC (*.flac)\0*.flac\0"
    L"WAV (*.wav;*.wave)\0*.wav;*.wave\0"
    L"OGG (*.ogg;*.oga)\0*.ogg;*.oga\0"
    L"Todos los archivos (*.*)\0*.*\0\0",
    L"Lista M3U (*.m3u;*.m3u8)\0*.m3u;*.m3u8\0",
    L"Lista M3U (*.m3u)\0*.m3u\0",
    L"Todos los archivos (*.*)\0*.*\0\0",
    /* ListView columns */
    L"T\x00edtulo", L"Artista", L"\x00c1lbum", L"Duraci\x00f3n",
    L"Tasa de bits", L"Formato", L"Tama\x00f1o",
};

/* French language pack. */
static const LangStrings LANG_FR_STRINGS = {
    /* Menu */
    L"Fichier(&F)", L"Langue(&L)", L"Aide(&H)",
    L"Ajouter des fichiers...", L"Ajouter un dossier...",
    L"Charger une liste...", L"Enregistrer la liste...",
    L"Vider la liste", L"Quitter", L"\x00c0 propos...",
    /* Buttons */
    L"Pr\x00e9" L"c\x00e9" L"dent", L"Lecture", L"Pause", L"Arr\x00ea" L"ter", L"Suivant",
    L"Mode: Liste", L"Mode: Piste unique", L"Mode: Al\x00e9" L"atoire",
    /* Status */
    L"En cours de lecture", L"En pause", L"Arr\x00ea" L"t\x00e9", L"Pr\x00ea" L"t",
    L"Lecture en cours: %s",
    L"Ajout\x00e9 %d fichier(s)",
    L"D\x00e9pos\x00e9 %d fichier(s)",
    L"Aucun fichier audio trouv\x00e9",
    L"Charg\x00e9 %d fichier(s) depuis M3U",
    L"Aucun fichier audio reconnu dans M3U",
    L"M3U enregistr\x00e9: %s",
    /* Labels */
    L"Volume: %d%%",
    L"Liste vide \x2014 d\x00e9posez des fichiers audio ici ou utilisez le menu Fichier",
    /* Dialogs */
    L"\x00c0 propos de CPlayer",
    L"CPlayer - Lecteur musical local ultra-l\x00e9ger\n"
    L"C99 pur + Win32 API\n"
    L"MP3 / FLAC / WAV / OGG",
    L"CPlayer - Lecteur de musique",
    L"La liste de lecture est vide, rien \x00e0 enregistrer.",
    L"\x00c9" L"chec de l'enregistrement M3U.",
    L"\x00c9" L"chec de l'enregistrement de la classe de fen\x00ea" L"tre",
    L"\x00c9" L"chec de la cr\x00e9" L"ation de la fen\x00ea" L"tre",
    L"\x00c9" L"chec de l'allocation du tampon audio",
    L"\x00c9" L"chec du d\x00e9marrage du thread audio",
    L"CPlayer", L"CPlayer",
    L"Index de piste invalide",
    L"Impossible d'ouvrir le fichier (format non pris en charge ou fichier corrompu)",
    L"Impossible d'ouvrir le périphérique de sortie audio",
    L"Échec de la sortie audio (le périphérique est peut-être déconnecté)",
    L"Ajouter des fichiers audio",
    L"S\x00e9lectionnez un dossier contenant des fichiers audio",
    L"Charger une liste de lecture M3U",
    L"Enregistrer la liste de lecture M3U",
    /* File dialog filters */
    L"Fichiers audio (*.mp3;*.flac;*.wav;*.ogg)\0"
    L"*.mp3;*.flac;*.wav;*.ogg\0"
    L"MP3 (*.mp3)\0*.mp3\0"
    L"FLAC (*.flac)\0*.flac\0"
    L"WAV (*.wav;*.wave)\0*.wav;*.wave\0"
    L"OGG (*.ogg;*.oga)\0*.ogg;*.oga\0"
    L"Tous les fichiers (*.*)\0*.*\0\0",
    L"Liste M3U (*.m3u;*.m3u8)\0*.m3u;*.m3u8\0",
    L"Liste M3U (*.m3u)\0*.m3u\0",
    L"Tous les fichiers (*.*)\0*.*\0\0",
    /* ListView columns */
    L"Titre", L"Artiste", L"Album", L"Dur\x00e9" L"e",
    L"D\x00e9bit binaire", L"Format", L"Taille",
};

/* Japanese language pack. */
static const LangStrings LANG_JA_STRINGS = {
    /* Menu */
    L"\x30d5\x30a1\x30a4\x30eb(&F)", L"\x8a00\x8a9e(&L)", L"\x30d8\x30eb\x30d7(&H)",
    L"\x30d5\x30a1\x30a4\x30eb\x3092\x8ffd\x52a0...", L"\x30d5\x30a9\x30eb\x30c0\x3092\x8ffd\x52a0...",
    L"\x30ea\x30b9\x30c8\x3092\x8aad\x307f\x8fbc\x3080...", L"\x30ea\x30b9\x30c8\x3092\x4fdd\x5b58...",
    L"\x30ea\x30b9\x30c8\x3092\x30af\x30ea\x30a2", L"\x7d42\x4e86", L"\x30d0\x30fc\x30b8\x30e7\x30f3\x60c5\x5831...",
    /* Buttons */
    L"\x524d\x3078", L"\x518d\x751f", L"\x4e00\x6642\x505c\x6b62", L"\x505c\x6b62", L"\x6b21\x3078",
    L"\x30e2\x30fc\x30c9: \x30ea\x30b9\x30c8", L"\x30e2\x30fc\x30c9: \x5358\x66f2", L"\x30e2\x30fc\x30c9: \x30b7\x30e3\x30c3\x30d5\x30eb",
    /* Status */
    L"\x518d\x751f\x4e2d", L"\x4e00\x6642\x505c\x6b62\x4e2d", L"\x505c\x6b62\x4e2d", L"\x6e96\x5099\x5b8c\x4e86",
    L"\x518d\x751f\x4e2d: %s",
    L"%d \x500b\x306e\x30d5\x30a1\x30a4\x30eb\x3092\x8ffd\x52a0\x3057\x307e\x3057\x305f",
    L"%d \x500b\x306e\x30d5\x30a1\x30a4\x30eb\x3092\x30c9\x30ed\x30c3\x30d7\x3057\x307e\x3057\x305f",
    L"\x5bfe\x5fdc\x3059\x308b\x30aa\x30fc\x30c7\x30a3\x30aa\x30d5\x30a1\x30a4\x30eb\x304c\x898b\x3064\x304b\x308a\x307e\x305b\x3093",
    L"M3U \x304b\x3089 %d \x500b\x306e\x30d5\x30a1\x30a4\x30eb\x3092\x8aad\x307f\x8fbc\x307f\x307e\x3057\x305f",
    L"M3U \x306b\x8a8d\x8b58\x3067\x304d\x308b\x30aa\x30fc\x30c7\x30a3\x30aa\x30d5\x30a1\x30a4\x30eb\x304c\x3042\x308a\x307e\x305b\x3093",
    L"M3U \x3092\x4fdd\x5b58\x3057\x307e\x3057\x305f: %s",
    /* Labels */
    L"\x97f3\x91cf: %d%%",
    L"\x30d7\x30ec\x30a4\x30ea\x30b9\x30c8\x306f\x7a7a\x3067\x3059 \x2014 \x97f3\x58f0\x30d5\x30a1\x30a4\x30eb\x3092\x3053\x3053\x306b\x30c9\x30e9\x30c3\x30b0\x3059\x308b\x304b\x3001[\x30d5\x30a1\x30a4\x30eb] \x30e1\x30cb\x30e5\x30fc\x304b\x3089\x8ffd\x52a0\x3057\x3066\x304f\x3060\x3055\x3044",
    /* Dialogs */
    L"CPlayer \x306e\x30d0\x30fc\x30b8\x30e7\x30f3\x60c5\x5831",
    L"CPlayer - \x8d85\x8efd\x91cf\x30ed\x30fc\x30ab\x30eb\x30df\x30e5\x30fc\x30b8\x30c3\x30af\x30d7\x30ec\x30fc\x30e4\x30fc\n"
    L"\x7d14\x7c97\x306a C99 + Win32 API\n"
    L"MP3 / FLAC / WAV / OGG",
    L"CPlayer - \x30df\x30e5\x30fc\x30b8\x30c3\x30af\x30d7\x30ec\x30fc\x30e4\x30fc",
    L"\x30d7\x30ec\x30a4\x30ea\x30b9\x30c8\x304c\x7a7a\x3067\x3059\x3002\x4fdd\x5b58\x3059\x308b\x3082\x306e\x306f\x3042\x308a\x307e\x305b\x3093\x3002",
    L"M3U \x306e\x4fdd\x5b58\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002",
    L"\x30a6\x30a3\x30f3\x30c9\x30a6\x30af\x30e9\x30b9\x306e\x767b\x9332\x306b\x5931\x6557\x3057\x307e\x3057\x305f",
    L"\x30a6\x30a3\x30f3\x30c9\x30a6\x306e\x4f5c\x6210\x306b\x5931\x6557\x3057\x307e\x3057\x305f",
    L"\x30aa\x30fc\x30c7\x30a3\x30aa\x30d0\x30c3\x30d5\x30a1\x306e\x5272\x308a\x5f53\x3066\x306b\x5931\x6557\x3057\x307e\x3057\x305f",
    L"\x30aa\x30fc\x30c7\x30a3\x30aa\x30b9\x30ec\x30c3\x30c9\x306e\x8d77\x52d5\x306b\x5931\x6557\x3057\x307e\x3057\x305f",
    L"CPlayer", L"CPlayer",
    L"無効なトラックインデックス",
    L"ファイルを開けません（非対応形式またはファイル破損）",
    L"オーディオ出力デバイスを開けません",
    L"オーディオ出力に失敗しました（デバイスが切断された可能性があります）",
    L"\x30aa\x30fc\x30c7\x30a3\x30aa\x30d5\x30a1\x30a4\x30eb\x3092\x8ffd\x52a0",
    L"\x30aa\x30fc\x30c7\x30a3\x30aa\x30d5\x30a1\x30a4\x30eb\x3092\x542b\x3080\x30d5\x30a9\x30eb\x30c0\x3092\x9078\x629e",
    L"M3U \x30d7\x30ec\x30a4\x30ea\x30b9\x30c8\x3092\x8aad\x307f\x8fbc\x3080",
    L"M3U \x30d7\x30ec\x30a4\x30ea\x30b9\x30c8\x3092\x4fdd\x5b58",
    /* File dialog filters */
    L"\x30aa\x30fc\x30c7\x30a3\x30aa\x30d5\x30a1\x30a4\x30eb (*.mp3;*.flac;*.wav;*.ogg)\0"
    L"*.mp3;*.flac;*.wav;*.ogg\0"
    L"MP3 (*.mp3)\0*.mp3\0"
    L"FLAC (*.flac)\0*.flac\0"
    L"WAV (*.wav;*.wave)\0*.wav;*.wave\0"
    L"OGG (*.ogg;*.oga)\0*.ogg;*.oga\0"
    L"\x3059\x3079\x3066\x306e\x30d5\x30a1\x30a4\x30eb (*.*)\0*.*\0\0",
    L"M3U \x30d7\x30ec\x30a4\x30ea\x30b9\x30c8 (*.m3u;*.m3u8)\0*.m3u;*.m3u8\0",
    L"M3U \x30d7\x30ec\x30a4\x30ea\x30b9\x30c8 (*.m3u)\0*.m3u\0",
    L"\x3059\x3079\x3066\x306e\x30d5\x30a1\x30a4\x30eb (*.*)\0*.*\0\0",
    /* ListView columns */
    L"\x30bf\x30a4\x30c8\x30eb", L"\x30a2\x30fc\x30c6\x30a3\x30b9\x30c8", L"\x30a2\x30eb\x30d0\x30e0", L"\x6642\x9593",
    L"\x30d3\x30c3\x30c8\x30ec\x30fc\x30c8", L"\x5f62\x5f0f", L"\x30b5\x30a4\x30ba",
};

/* Language descriptor table — add new languages here. */
typedef struct {
    int         id;       /* LANG_ZH, LANG_EN, ... */
    const wchar_t *name;  /* display name in the menu */
    const LangStrings *strings;
} LangEntry;
/* Maps language ID to menu display name and string table pointer.
 * Order must match IDM_LANG_FIRST + index in the menu creation code. */
static const LangEntry LANG_TABLE[] = {
    { LANG_ZH, L"中文",           &LANG_ZH_STRINGS },
    { LANG_EN, L"English",        &LANG_EN_STRINGS },
    { LANG_ES, L"Espa\x00f1ol",   &LANG_ES_STRINGS },
    { LANG_FR, L"Fran\x00e7" L"ais",  &LANG_FR_STRINGS },
    { LANG_JA, L"\x65e5\x672c\x8a9e", &LANG_JA_STRINGS },
};
#define LANG_COUNT ((int)(sizeof(LANG_TABLE) / sizeof(LANG_TABLE[0])))
/* First entry is the default language. */
#define LANG_DEFAULT LANG_TABLE[0].id

static const LangStrings *ui_lang(void)
{
    for (int i = 0; i < LANG_COUNT; i++)
        if (LANG_TABLE[i].id == g_player.lang)
            return LANG_TABLE[i].strings;
    return LANG_TABLE[0].strings; /* fallback */
}

/* Older SDK headers may not define the trackbar class name; define if absent. */
#ifndef WC_TRACKBARCLASSW
#define WC_TRACKBARCLASSW L"msctls_trackbar32"
#endif

/* ---- DPI helper ------------------------------------------------------- */
/* GetDpiForWindow is Win10 1607+; fall back to GetDeviceCaps for older OS. */
static int ui_get_dpi(HWND hwnd)
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
        PFN_GetDpiForWindow p = (PFN_GetDpiForWindow)
            GetProcAddress(u32, "GetDpiForWindow");
        if (p && hwnd) {
            UINT d = p(hwnd);
            if (d) return (int)d;
        }
    }
    /* Fall back to the monitor the window is on. GetDpiForMonitor reports
     * the true per-monitor value even before the window has been shown,
     * whereas a raw screen DC only ever reports the system DPI. */
    if (hwnd && u32) {
        typedef UINT (WINAPI *PFN_GetDpiForSystem)(void);
        PFN_GetDpiForSystem ps = (PFN_GetDpiForSystem)
            GetProcAddress(u32, "GetDpiForSystem");
        if (ps) { UINT d = ps(); if (d) return (int)d; }
    }
    HDC dc = GetDC(NULL);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return dpi ? dpi : 96;
}

/* DPI for a window, or for the monitor nearest a point when hwnd is NULL.
 * GetDpiForMonitor is the only API that reports the real per-monitor value
 * before a window has ever been shown, so it is used as the primary path. */
static int ui_dpi_for_window_or_monitor(HWND hwnd)
{
    HMODULE sh = LoadLibraryW(L"shcore.dll");
    if (sh) {
        typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
        PFN_GetDpiForMonitor p = (PFN_GetDpiForMonitor)
            GetProcAddress(sh, "GetDpiForMonitor");
        if (p) {
            POINT pt = { 0, 0 };
            HMONITOR mon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
                                : MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
            UINT dx = 0, dy = 0;
            /* MDT_EFFECTIVE_DPI = 0 */
            if (SUCCEEDED(p(mon, 0, &dx, &dy)) && dx) return (int)dx;
        }
        FreeLibrary(sh);
    }
    return ui_get_dpi(hwnd);
}

/* ---- Time formatting -------------------------------------------------- */
void ui_format_time(double seconds, wchar_t *buf, int cap)
{
    if (seconds < 0) seconds = 0;
    int total = (int)(seconds + 0.5);
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;
    if (h > 0)
        _snwprintf(buf, cap, L"%d:%02d:%02d", h, m, s);
    else
        _snwprintf(buf, cap, L"%d:%02d", m, s);
    buf[cap - 1] = 0;
}

/* ---- Status text ------------------------------------------------------ */
void ui_set_status(const wchar_t *fmt, ...)
{
    wchar_t buf[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 256, fmt, ap);
    va_end(ap);
    buf[255] = 0;
    if (g_player.hLblStatus)
        SetWindowTextW(g_player.hLblStatus, buf);
}

/* ---- Font management -------------------------------------------------- */
/* Three weights: regular body text, a medium weight for headings/labels,
 * and a semibold for the primary transport button. Using a real family
 * (Segoe UI Variable when present, else Segoe UI) keeps text crisp at any
 * DPI and matches the flat look far better than the stock system font. */
static HFONT ui_make_font(int dpi, int pt, int weight)
{
    int h = -MulDiv(pt, dpi, 72);
    HFONT f = CreateFontW(h, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
    if (!f)
        f = CreateFontW(h, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!f)
        f = CreateFontW(h, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"MS Shell Dlg");
    return f;
}

/* Segoe MDL2 Assets (present on every Windows 10/11 install) gives us real
 * vector glyphs for the transport / volume icons.  Falls back to a symbol
 * font, then to the UI face, so a missing font degrades instead of blanking. */
static HFONT ui_make_icon_font(int dpi, int pt)
{
    int h = -MulDiv(pt, dpi, 72);
    HFONT f = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
    if (!f)
        f = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");
    return f;
}

/* The ListView's header child. ui_layout() rounds its corners so the card's
 * top edge stays smooth, and the header's custom-draw handler needs it too;
 * both run before the WM_CREATE assignment is reached textually, so the
 * handle lives here. */
static HWND g_hHeader = NULL;

/* Oversized icon face for the playlist's empty-state hint. Rebuilt with the
 * rest of the fonts on every DPI change; owned by this file. */
static HFONT g_hFontEmpty = NULL;

void ui_set_font_for_dpi(int dpi)
{
    if (g_player.hFont)      DeleteObject(g_player.hFont);
    if (g_player.hFontBold)  DeleteObject(g_player.hFontBold);
    if (g_player.hFontSm)    DeleteObject(g_player.hFontSm);
    if (g_player.hFontIcon)  DeleteObject(g_player.hFontIcon);
    if (g_hFontEmpty)        DeleteObject(g_hFontEmpty);

    g_player.hFont     = ui_make_font(dpi,  9, FW_NORMAL);
    g_player.hFontBold = ui_make_font(dpi,  9, FW_SEMIBOLD);
    g_player.hFontSm   = ui_make_font(dpi,  8, FW_NORMAL);
    g_player.hFontIcon = ui_make_icon_font(dpi, 10);
    g_hFontEmpty       = ui_make_icon_font(dpi, 26);

    /* Child controls only need the regular face; the custom-drawn chrome
     * picks its own weight per element. */
    HWND ctrls[] = { g_player.hList, g_player.hProgress, g_player.hVolume,
                     g_player.hBtnPrev, g_player.hBtnPlay, g_player.hBtnStop,
                     g_player.hBtnNext, g_player.hBtnMode,
                     g_player.hLblTime, g_player.hLblVol, g_player.hLblStatus };
    for (int i = 0; i < (int)(sizeof(ctrls)/sizeof(ctrls[0])); i++) {
        if (ctrls[i])
            SendMessageW(ctrls[i], WM_SETFONT, (WPARAM)g_player.hFont, TRUE);
    }
    /* The header is not in the list above (it does not exist yet when the
     * fonts are first built during ui_create_main), but once WM_CREATE has
     * fetched it, every DPI change must re-seat its font too - otherwise the
     * column labels keep the old scale after a monitor switch. */
    if (g_hHeader)
        SendMessageW(g_hHeader, WM_SETFONT, (WPARAM)g_player.hFont, TRUE);

    /* Any cached rounded shape is invalidated by a DPI change. */
    rounded_flush();
}

/* ---- Per-DPI list metrics ----------------------------------------------
 * The row-height spacer image list and the header strip height are device-
 * pixel quantities computed once at WM_CREATE. Both must be rebuilt when the
 * DPI changes, otherwise after a move to a monitor at a different scale the
 * rows and the header keep the old scale while fonts and columns use the new
 * one. The old image list is destroyed: ListView_SetImageList returns the
 * previous list and does not take ownership, so dropping it would leak one
 * image list per DPI change. */
static HIMAGELIST g_hRowIL = NULL;

static void ui_apply_list_metrics(void)
{
    if (g_player.hList) {
        HIMAGELIST hil = ImageList_Create(1, DPIX(30), ILC_COLOR32, 1, 1);
        if (hil) {
            HIMAGELIST old = ListView_SetImageList(g_player.hList, hil,
                                                   LVSIL_SMALL);
            if (old) ImageList_Destroy(old);
            g_hRowIL = hil;
        }
    }
    if (g_hHeader)
        SetWindowPos(g_hHeader, NULL, 0, 0, 0, DPIX(32),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ---- Layout -----------------------------------------------------------
 * The window is composed as a vertical stack of three zones:
 *
 *   +--------------------------------------------------+
 *   |  playlist card (rounded, 1px border)             |
 *   +--------------------------------------------------+
 *   |  seek slider + time readout                      |
 *   +--------------------------------------------------+
 *   |  transport row:  [prev][play][stop][next]  [mode] | chip |
 *   +--------------------------------------------------+
 *
 * Every child keeps its stock HWND (so all existing message handling is
 * untouched); only the geometry and painting change. The seek/volume
 * sliders are drawn by us, so they are thin strips; the transport buttons
 * are owner-draw pills.
 */
void ui_layout(void)
{
    RECT rc;
    GetClientRect(g_player.hMain, &rc);
    int W = rc.right;
    int H = rc.bottom;
    int pad  = DPIX(14);        /* outer margin around the card           */
    int gap  = DPIX(10);        /* vertical gap between zones             */
    int btnH = DPIX(34);        /* transport button height                */
    int btnW = DPIX(58);        /* secondary transport button width        */
    int playW = DPIX(76);       /* primary (play/pause) button width      */
    int modeW = DPIX(112);      /* mode pill width                        */
    int sldH = DPIX(22);        /* slider hit area                        */
    int chipH = DPIX(26);       /* status chip height                     */
    int timeW = DPIX(96);       /* "m:ss / m:ss" readout width            */

    /* Bottom-up: reserve the transport row, then the seek row, and give
     * whatever remains to the playlist card. */
    int bottomRowY = H - pad - btnH;
    int seekRowY   = bottomRowY - gap - sldH;

    int cardY = pad;
    int cardH = seekRowY - gap - cardY;
    int minH  = DPIX(80);
    if (cardH < minH) cardH = minH;

    /* --- Playlist card ---
     * The card is drawn by the parent (ui_compose_card): a white rounded
     * panel with a hairline border. The ListView sits inside it with a
     * uniform inset, so the panel shows as a crisp white frame around the
     * list - the same way a modern web card pads its table.
     *
     * The inset must be at least the card's corner radius. A ListView can
     * only be clipped by SetWindowRgn, which produces hard, aliased corner
     * steps; those steps read as a ragged second corner sitting inside the
     * smooth border arc. Keeping the whole control inside the straight part
     * of the border means the rounding is done once, by the supersampled
     * card, and the list never has to be clipped at all. */
    if (g_player.hList) {
        int cp  = DPIX(10);                /* = card radius, keeps corners clear */
        int lx  = pad + cp;
        int ly  = cardY + cp;
        int lw  = W - pad * 2 - cp * 2;
        int lh  = cardH - cp * 2;
        if (lw < DPIX(40)) lw = DPIX(40);
        if (lh < DPIX(40)) lh = DPIX(40);

        /* A stale rounded clip from an older build would cut the corners, so
         * it is released once. Doing this on every layout would force a full
         * window-region recalculation plus repaint on each mouse-move of a
         * resize drag, which is the single most expensive thing this routine
         * could do - hence the one-shot flag. */
        static int region_cleared = 0;
        if (!region_cleared) {
            region_cleared = 1;
            SetWindowRgn(g_player.hList, NULL, TRUE);
            if (g_hHeader) SetWindowRgn(g_hHeader, NULL, TRUE);
        }

        MoveWindow(g_player.hList, lx, ly, lw, lh, FALSE);

        /* Distribute the available width across the seven columns so the
         * set always fills the card exactly, at any window size and DPI.
         * The weights favour the text columns and give the numeric ones a
         * little more than their bare glyph width so '--:--' and
         * '320 kbps' never ellipsise. The last column simply takes the
         * remainder, which keeps the right edge flush with the card.
         *
         * ListView_SetColumnWidth invalidates the whole control, and the
         * repaint it triggers is queued synchronously - so seven calls cost
         * seven full passes over every visible row. Measured on a 447-item
         * playlist that loop alone accounted for ~8 ms of a ~13 ms relayout,
         * which is what made dragging the window feel sticky. WM_SETREDRAW
         * collapses the seven passes into a single one; the width cache still
         * skips the loop entirely whenever the available width is unchanged.
         * */
        {
            static int last_avail = -1;
            static int last_dpi   = -1;
            /* Size the columns from the list's client width, not from the
             * rectangle handed to MoveWindow. Once the playlist is long
             * enough to scroll, the report view reports a client area one
             * scrollbar narrower (30 device px at 168 dpi) because its
             * vertical bar lives inside that client area. Sizing against the
             * outer rectangle therefore made the seven columns overflow, and
             * the overflow clipped the last column and raised a horizontal
             * scrollbar beneath the rows. */
            RECT lrc;
            GetClientRect(g_player.hList, &lrc);
            int avail = lrc.right - DPIX(2);   /* hairline for the border */
            if (avail < DPIX(200)) avail = DPIX(200);

            if (avail != last_avail || g_player.dpi != last_dpi) {
                last_avail = avail;
                last_dpi   = g_player.dpi;

                static const int weight[7] = { 30, 16, 16, 10, 11, 8, 9 };
                const int total_w = 100;
                int cw[7];
                int used = 0;

                for (int c = 0; c < 6; c++) {
                    cw[c] = avail * weight[c] / total_w;
                    if (cw[c] < DPIX(36)) cw[c] = DPIX(36);
                    used += cw[c];
                }
                cw[6] = avail - used;
                if (cw[6] < DPIX(40)) cw[6] = DPIX(40);

                /* Every column has a floor so its content stays legible, and
                 * near the minimum window width those floors stop fitting all
                 * at once: the 8% column is lifted above its weighted share,
                 * which leaves the remainder column below its own floor, so it
                 * is lifted too - and the seven widths then add up to more than
                 * the item area. That clipped the last column and raised a
                 * horizontal scrollbar beneath the rows.
                 *
                 * Reconcile once, after the fact, rather than clamping in
                 * place: hand the overshoot back to the title column, which
                 * carries the largest share and therefore the most slack. The
                 * floors need 6 * 36 + 40 = 256 DIP in total, which is well
                 * under the item area even at the 480 DIP minimum window, so
                 * the title column always absorbs it without reaching its own
                 * floor. */
                {
                    int over = cw[0] + cw[1] + cw[2] + cw[3] +
                               cw[4] + cw[5] + cw[6] - avail;
                    if (over > 0) {
                        cw[0] -= over;
                        if (cw[0] < DPIX(36)) cw[0] = DPIX(36);
                    }
                }

                SendMessageW(g_player.hList, WM_SETREDRAW, FALSE, 0);
                for (int c = 0; c < 7; c++)
                    ListView_SetColumnWidth(g_player.hList, c, cw[c]);
                /* Re-state the per-column justification. LVM_SETCOLUMNWIDTH
                 * clears the format bits, so the LVCFMT_RIGHT requested when
                 * the columns were created does not survive the first width
                 * change: every numeric column silently fell back to
                 * left-justified, which is precisely why the header labels
                 * and the values below them did not line up. Still inside
                 * WM_SETREDRAW(FALSE), so this costs no extra repaint. */
                {
                    static const int justify[7] = {
                        LVCFMT_LEFT,  LVCFMT_LEFT,  LVCFMT_LEFT,
                        LVCFMT_RIGHT, LVCFMT_RIGHT, LVCFMT_RIGHT, LVCFMT_RIGHT
                    };
                    for (int c = 0; c < 7; c++) {
                        LVCOLUMNW sc;
                        memset(&sc, 0, sizeof(sc));
                        sc.mask = LVCF_FMT;
                        sc.fmt  = justify[c];
                        SendMessageW(g_player.hList, LVM_SETCOLUMNW, c,
                                     (LPARAM)&sc);
                    }
                }
                SendMessageW(g_player.hList, WM_SETREDRAW, TRUE, 0);
                /* WM_SETREDRAW(TRUE) does not repaint by itself; the control
                 * was left invalid, so ask for exactly one redraw now. */
                InvalidateRect(g_player.hList, NULL, FALSE);
            }
        }
    }

    /* --- Seek slider: time readout on the right, slider takes the rest --- */
    int sldW = W - pad * 2 - timeW - gap;
    if (sldW < DPIX(80)) sldW = DPIX(80);
    int seekMidY = seekRowY + sldH / 2;
    if (g_player.hProgress)
        MoveWindow(g_player.hProgress, pad, seekRowY, sldW, sldH, FALSE);
    if (g_player.hLblTime)
        MoveWindow(g_player.hLblTime, pad + sldW + gap,
                   seekMidY - chipH / 2, timeW, chipH, FALSE);

    /* --- Transport row ---
     * Left cluster: prev / play / stop / next, then the mode pill.
     * Right side: the volume slider, its percentage readout, and the status
     * chip.
     *
     * The row has to survive very narrow windows, so the fixed-width items
     * degrade in priority order before anything is allowed to overlap:
     *   1. the mode pill shrinks from its full label to a short form,
     *   2. the volume percentage readout is dropped,
     *   3. the status chip is hidden entirely.
     * Only the five transport controls and the volume slider never give way. */
    int volPctW = DPIX(96);
    int volW    = DPIX(78);
    int volTotal = volW + DPIX(6) + volPctW;

    /* Space the left cluster needs before the right cluster is even placed. */
    int btnRowW = btnW * 3 + playW + DPIX(6) * 4;
    int modeW_use = modeW;
    int showVolPct = 1;
    int showChip   = 1;

    /* Total width budget: everything on the row plus the gaps between the
     * three groups. If it does not fit, degrade as described above. */
    for (;;) {
        int need = pad * 2 + btnRowW + DPIX(6) + modeW_use + gap
                 + volTotal + gap + DPIX(120);   /* 120 = comfortable chip */
        if (need <= W) break;
        if (modeW_use > DPIX(64)) {             /* 1: shrink the mode pill */
            modeW_use = DPIX(64);
            continue;
        }
        if (showVolPct) {                       /* 2: drop the volume label */
            showVolPct = 0;
            volTotal   = volW;
            continue;
        }
        if (showChip) {                         /* 3: hide the status chip */
            showChip = 0;
            continue;
        }
        break;                                  /* nothing left to give */
    }

    int x = pad;
    if (g_player.hBtnPrev) {
        MoveWindow(g_player.hBtnPrev, x, bottomRowY, btnW, btnH, FALSE);
        x += btnW + DPIX(6);
    }
    if (g_player.hBtnPlay) {
        MoveWindow(g_player.hBtnPlay, x, bottomRowY, playW, btnH, FALSE);
        x += playW + DPIX(6);
    }
    if (g_player.hBtnStop) {
        MoveWindow(g_player.hBtnStop, x, bottomRowY, btnW, btnH, FALSE);
        x += btnW + DPIX(6);
    }
    if (g_player.hBtnNext) {
        MoveWindow(g_player.hBtnNext, x, bottomRowY, btnW, btnH, FALSE);
        x += btnW + DPIX(6);
    }
    if (g_player.hBtnMode) {
        MoveWindow(g_player.hBtnMode, x, bottomRowY, modeW_use, btnH, FALSE);
        x += modeW_use + gap;
    }

    /* Volume cluster pinned to the right edge, vertically centred against the
     * transport buttons so the whole bottom row shares one baseline. */
    int volX = W - pad - volTotal;
    if (volX < x) volX = x;                 /* never overlap the mode pill */
    if (g_player.hVolume && g_player.hLblVol) {
        int vy = bottomRowY + (btnH - sldH) / 2;
        MoveWindow(g_player.hVolume, volX, vy, volW, sldH, FALSE);
        if (showVolPct) {
            MoveWindow(g_player.hLblVol, volX + volW + DPIX(6),
                       bottomRowY + (btnH - chipH) / 2, volPctW, chipH, FALSE);
            ShowWindow(g_player.hLblVol, SW_SHOW);
        } else {
            ShowWindow(g_player.hLblVol, SW_HIDE);
        }
    }

    /* Status chip fills the gap between the mode pill and the volume cluster;
     * it is hidden outright once the window is too narrow to hold it. */
    if (g_player.hLblStatus) {
        int chipX = x;
        int chipW = volX - gap - chipX;
        if (showChip && chipW >= DPIX(70)) {
            ShowWindow(g_player.hLblStatus, SW_SHOW);
            MoveWindow(g_player.hLblStatus, chipX,
                       bottomRowY + (btnH - chipH) / 2, chipW, chipH, FALSE);
        } else {
            ShowWindow(g_player.hLblStatus, SW_HIDE);
        }
    }

    /* Every MoveWindow above used bRepaint=FALSE. Passing TRUE would make
     * each of the eleven calls repaint its child synchronously, and a resize
     * drag runs this function once per mouse move; one consolidated
     * invalidation at the end costs a single paint pass for the whole chrome
     * instead of eleven. (The heavyweight child here is the ListView - its own
     * repaint is triggered separately, and only when the column widths above
     * actually changed.) */
    RedrawWindow(g_player.hMain, NULL, NULL,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
}

/* Force the custom-drawn chrome to repaint (after a colour/state change). */
void ui_repaint(void)
{
    HWND btns[] = { g_player.hBtnPrev, g_player.hBtnPlay, g_player.hBtnStop,
                    g_player.hBtnNext, g_player.hBtnMode };
    for (int i = 0; i < 5; i++) {
        if (btns[i])
            InvalidateRect(btns[i], NULL, FALSE);
    }
    if (g_player.hLblStatus) InvalidateRect(g_player.hLblStatus, NULL, FALSE);
    if (g_player.hLblTime)   InvalidateRect(g_player.hLblTime, NULL, FALSE);
    if (g_player.hLblVol)    InvalidateRect(g_player.hLblVol, NULL, FALSE);
    if (g_player.hProgress)  InvalidateRect(g_player.hProgress, NULL, FALSE);
    if (g_player.hVolume)    InvalidateRect(g_player.hVolume, NULL, FALSE);
    if (g_player.hList)      InvalidateRect(g_player.hList, NULL, FALSE);
}

/* Hover sweep: repaint only the two buttons whose state changed instead
 * of the whole chrome (ui_repaint touches 10 HWNDs). */
static void ui_repaint_btn(int id)
{
    HWND h = NULL;
    switch (id) {
    case IDC_BTN_PREV: h = g_player.hBtnPrev; break;
    case IDC_BTN_PLAY: h = g_player.hBtnPlay; break;
    case IDC_BTN_STOP: h = g_player.hBtnStop; break;
    case IDC_BTN_NEXT: h = g_player.hBtnNext; break;
    case IDC_BTN_MODE: h = g_player.hBtnMode; break;
    default: break;
    }
    if (h) InvalidateRect(h, NULL, FALSE);
}

/* ---- Playlist ListView ------------------------------------------------ */

void ui_refresh_playlist(void)
{
    if (!g_player.hList) return;
    /* 虚拟列表模式：只需告知总数，控件自动对可见行请求 LVN_GETDISPINFO。 */
    ListView_SetItemCountEx(g_player.hList, g_playlist.count, LVSICF_NOSCROLL);

    /* Re-run the layout: the first item is also what makes the vertical
     * scrollbar appear, and that narrows the width the columns may use. The
     * column sizing only re-runs when its cached width changes, so without
     * this the columns would keep the width they were given while the list
     * was still empty - wider than the item area, which clipped the last
     * column and produced a horizontal scrollbar. */
    ui_layout();

    /* cur_index 由解码线程持锁写入，UI 线程读取同样要持锁——
     * 无锁读是正式的数据竞争（与 WM_TRACK_LOADED 处的处理一致）。 */
    int cur;
    EnterCriticalSection(&g_player.cs);
    cur = g_player.cur_index;
    LeaveCriticalSection(&g_player.cs);
    ui_select_current(cur);
}

void ui_select_current(int index)
{
    if (!g_player.hList) return;
    if (index < 0 || index >= g_playlist.count) return;

    /* "Now playing" is painted from cur_index (the custom-draw accent rail),
     * not from selection. So auto-advance / refresh must NOT wipe a user's
     * multi-selection. Only adopt the row as the selection when nothing is
     * selected (so the list always has a clear focus target); otherwise just
     * move focus and scroll the playing row into view, leaving selection be. */
    if (ListView_GetSelectedCount(g_player.hList) == 0) {
        ListView_SetItemState(g_player.hList, index,
                              LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    } else {
        ListView_SetItemState(g_player.hList, -1, 0, LVIS_FOCUSED);
        ListView_SetItemState(g_player.hList, index, LVIS_FOCUSED, LVIS_FOCUSED);
    }
    ListView_EnsureVisible(g_player.hList, index, FALSE);
}

/* ---- Position / state UI --------------------------------------------- */
/* Last pushed slider/label state: the 200 ms timer must not touch the
 * controls when nothing changed (each TBM_SETPOS + SetWindowText costs a
 * full repaint of the slider + time pill, even for identical content). */
static int     g_last_pos = -1;
static wchar_t g_last_lbl[80] = L"";
static int     g_last_has_track = -1;

void ui_update_position(void)
{
    uint64_t cur, total;
    int rate, state;
    EnterCriticalSection(&g_player.cs);
    cur   = g_player.cur_frame;
    total = g_player.total_frames;
    rate  = g_player.sample_rate;
    state = g_player.state;
    LeaveCriticalSection(&g_player.cs);

    int pos = -1;
    int has_track = (total > 0);
    /* While the user drags the thumb, the slider position and the time
     * label belong to the drag handler (TB_THUMBTRACK live preview).
     * Updating them here as well made the label flicker between the
     * preview position and the decoder position at 5 Hz for the whole
     * duration of the drag. */
    if (g_player.seeking)
        return;
    if (has_track) {
        pos = (int)(cur * 1000 / total);
        if (pos < 0) pos = 0;
        if (pos > 1000) pos = 1000;
    } else if (state == STATE_STOPPED) {
        /* 停止后 total_frames 归零，上面的分支不再执行，进度条会保留
         * 上一次的位置——既与"已停止"状态不符，也让用户仍能拖动一个
         * 已经不属于任何曲目的位置。这里在停止态显式归零。 */
        pos = 0;
    }
    if (pos >= 0 && (pos != g_last_pos || has_track != g_last_has_track)) {
        g_last_pos = pos;
        g_last_has_track = has_track;
        SendMessageW(g_player.hProgress, TBM_SETPOS, TRUE, pos);
    }

    /* Time label "cur / total". */
    wchar_t t1[32], t2[32], lbl[80];
    double cur_s  = rate > 0 ? (double)cur  / rate : 0;
    double tot_s  = rate > 0 ? (double)total / rate : 0;
    ui_format_time(cur_s, t1, 32);
    ui_format_time(tot_s, t2, 32);
    _snwprintf(lbl, 80, L"%s / %s", t1, t2);
    lbl[79] = 0;
    if (wcscmp(lbl, g_last_lbl) != 0) {
        wcsncpy(g_last_lbl, lbl, 79);
        g_last_lbl[79] = 0;
        SetWindowTextW(g_player.hLblTime, lbl);
    }
}

/* Build the mode button label for the current play mode. */
static const wchar_t *mode_label(int mode)
{
    const LangStrings *L = ui_lang();
    switch (mode) {
    case MODE_SINGLE_LOOP: return L->mode_sl;
    case MODE_SHUFFLE:     return L->mode_sh;
    case MODE_LIST_LOOP:
    default:               return L->mode_ll;
    }
}

void ui_update_state_controls(void)
{
    int state, mode;
    const LangStrings *L = ui_lang();
    EnterCriticalSection(&g_player.cs);
    state = g_player.state;
    mode  = g_player.play_mode;
    LeaveCriticalSection(&g_player.cs);

    SetWindowTextW(g_player.hBtnPlay,
                   state == STATE_PLAYING ? L->b_pause : L->b_play);
    SetWindowTextW(g_player.hBtnMode, mode_label(mode));

    /* 状态改变会换掉状态点的颜色，需要重绘这些自绘控件。 */
    ui_repaint();

    switch (state) {
    case STATE_PLAYING:
        ui_set_status(L->s_playing);
        break;
    case STATE_PAUSED:
        ui_set_status(L->s_paused);
        break;
    default:
        ui_set_status(L->s_stopped);
        break;
    }
}

/* ---- File / folder dialogs ------------------------------------------- */

/* Arm the chunked-import timer (if any work was queued) and refresh once so
 * the first batch of rows shows immediately. Import runs in bounded slices on
 * TIMER_ID_IMPORT, keeping the UI responsive for very large drops. */
static void ui_run_import(void)
{
    ui_refresh_playlist();
    if (playlist_import_active())
        SetTimer(g_player.hMain, TIMER_ID_IMPORT, 10, NULL);
}

static void add_files_dialog(void)
{
    /* OFN_ALLOWMULTISELECT needs a generous buffer. */
    static wchar_t buf[65536];
    buf[0] = 0;
    const LangStrings *L = ui_lang();

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_player.hMain;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = 65536;
    ofn.lpstrFilter = L->f_audio;
    ofn.nFilterIndex       = 1;
    ofn.lpstrTitle         = L->d_add_title;
    ofn.Flags              = OFN_EXPLORER | OFN_ALLOWMULTISELECT |
                             OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetOpenFileNameW(&ofn)) return;

    /* With multiselect, buf = "dir\0file1\0file2\0...\0\0".
     * If there is only one file, buf = "fullpath\0". */
    const wchar_t *p = buf;
    const wchar_t *dir = p;
    p += wcslen(p) + 1;
    if (!*p) {
        /* Single file - buf is the full path. Add it immediately (sync tag). */
        playlist_add_file(buf);
    } else {
        wchar_t full[PL_PATH_MAX];
        while (*p) {
            _snwprintf(full, PL_PATH_MAX, L"%s\\%s", dir, p);
            full[PL_PATH_MAX - 1] = 0;
            playlist_import_path(full);
            p += wcslen(p) + 1;
        }
    }
    ui_run_import();
}

static void add_folder_dialog(void)
{
    const LangStrings *L = ui_lang();
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner      = g_player.hMain;
    bi.lpszTitle      = L->d_folder_title;
    bi.ulFlags        = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    bi.lpfn           = NULL;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;

    wchar_t path[MAX_PATH];
    if (SHGetPathFromIDListW(pidl, path)) {
        /* The folder walk runs in bounded slices; the count is reported by the
         * import timer when the session drains. */
        playlist_import_path(path);
        ui_run_import();
    }
    CoTaskMemFree(pidl);
}

/* 读取 M3U 播放列表文件，追加到当前列表。 */
static void load_m3u_dialog(void)
{
    const LangStrings *L = ui_lang();
    wchar_t buf[MAX_PATH];
    buf[0] = 0;

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_player.hMain;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter = L->f_m3u_load;
    ofn.nFilterIndex       = 1;
    ofn.lpstrTitle         = L->d_load_m3u_title;
    ofn.Flags              = OFN_EXPLORER | OFN_FILEMUSTEXIST |
                             OFN_HIDEREADONLY;

    if (!GetOpenFileNameW(&ofn)) return;

    if (playlist_import_m3u(buf)) {
        /* The M3U is parsed in bounded slices; the count is reported by the
         * import timer when the session drains. */
        ui_run_import();
    } else {
        ui_set_status(L->s_no_audio_m3u);
    }
}

/* 保存当前播放列表为 M3U 文件。 */
static void save_m3u_dialog(void)
{
    const LangStrings *L = ui_lang();
    if (g_playlist.count == 0) {
        MessageBoxW(g_player.hMain, L->d_empty_playlist,
                    L->d_info_title, MB_OK | MB_ICONINFORMATION);
        return;
    }

    wchar_t buf[MAX_PATH];
    buf[0] = 0;

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_player.hMain;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter = L->f_m3u_save;
    ofn.nFilterIndex       = 1;
    ofn.lpstrTitle         = L->d_save_m3u_title;
    ofn.lpstrDefExt        = L"m3u";
    ofn.Flags              = OFN_EXPLORER | OFN_OVERWRITEPROMPT |
                             OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;

    if (!GetSaveFileNameW(&ofn)) return;

    if (playlist_save_m3u(buf))
        ui_set_status(L->s_saved_m3u, buf);
    else
        MessageBoxW(g_player.hMain, L->d_save_failed,
                    L->d_err_title, MB_OK | MB_ICONERROR);
}

/* ---- ListView keyboard shortcuts (Ctrl+A / Del) ----------------------- */
/* Original ListView window procedure; restored implicitly at process exit. */
static WNDPROC g_list_orig_proc = NULL;

/* Remove every selected row from the playlist and keep cur_index in sync.
 * If the currently playing track is among the removed rows, playback is
 * stopped first so the decoder never outlives its playlist entry. */
static void delete_selected_items(void)
{
    if (!g_player.hList) return;
    int total = ListView_GetItemCount(g_player.hList);
    if (total <= 0) return;

    int *sel = (int *)malloc((size_t)total * sizeof(int));
    if (!sel) return;
    int nsel = 0;
    int idx = -1;
    while ((idx = ListView_GetNextItem(g_player.hList, idx, LVNI_SELECTED)) != -1) {
        if (nsel < total) sel[nsel++] = idx;
    }
    if (nsel == 0) { free(sel); return; }

    int cur, state;
    EnterCriticalSection(&g_player.cs);
    cur   = g_player.cur_index;
    state = g_player.state;
    LeaveCriticalSection(&g_player.cs);

    int deleting_current = 0;
    for (int i = 0; i < nsel; i++)
        if (sel[i] == cur) { deleting_current = 1; break; }

    if (deleting_current && state != STATE_STOPPED) {
        player_stop();
        /* player_stop 是异步的（只置 CMD_STOP）。此处主动把 state 置为
         * STOPPED，否则解码线程尚未处理 CMD_STOP 时，队列里可能已有
         * 本条曲目的 WM_TRACK_ENDED；on_track_ended 会看到 state 仍为
         * PLAYING 且 cur_index 刚被置 -1，从而 playlist_next_index(-1,…)
         * 返回 0，导致"删掉正在播放的歌，却自动开始播另一首"。 */
        EnterCriticalSection(&g_player.cs);
        g_player.state = STATE_STOPPED;
        /* 让代数前进：任何已经入队的 WM_TRACK_ENDED 都会被丢弃。 */
        InterlockedIncrement(&g_player.track_gen);
        LeaveCriticalSection(&g_player.cs);
    }

    /* 批量删除：一次压缩完成（sel 已按升序收集）。内部会挂起/恢复 tag 线程
     * 并推进 epoch，远比逐条 playlist_remove_at 划算。 */
    playlist_remove_indices(sel, nsel);

    /* Recompute cur_index: shift down by the number of removed rows that sat
     * above it; if it was itself removed, invalidate it. */
    EnterCriticalSection(&g_player.cs);
    if (deleting_current) {
        g_player.cur_index = -1;
    } else if (cur >= 0) {
        int above = 0;
        for (int i = 0; i < nsel; i++)
            if (sel[i] < cur) above++;
        g_player.cur_index = cur - above;
    }
    LeaveCriticalSection(&g_player.cs);

    /* The removed rows were exactly the selected ones. A virtual list keeps
     * selection as an index set and won't remap after the compaction, so clear
     * it to avoid phantom selection on the rows that shifted up. */
    ListView_SetItemState(g_player.hList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);

    free(sel);
    ui_refresh_playlist();
    ui_update_state_controls();
}

/* ---- Header hover / press tracking ------------------------------------
 * The header is custom-drawn (see the WM_NOTIFY branch in the list
 * subclass), but its own mouse messages never pass through that branch,
 * so the hovered / pressed column is tracked here in a dedicated subclass.
 * Both indices are UI-thread-only state, like hover_row; -1 means none.
 * Only the affected item rect is invalidated, so a sweep across the header
 * costs one small repaint per crossing instead of a full header pass. */
static WNDPROC g_hdr_orig_proc = NULL;
static int     g_hdr_hover     = -1;
static int     g_hdr_press     = -1;

static void hdr_invalidate_item(HWND hh, int col)
{
    if (col < 0 || !hh) return;
    RECT rc;
    if (Header_GetItemRect(hh, col, &rc))
        InvalidateRect(hh, &rc, FALSE);
}

/* Hit-test the header, ignoring the resize dividers (a divider drag must
 * not light up the column beside it as if it were clickable).
 *
 * The divider test is done against the item rectangle rather than through
 * HDM_HITTEST's flags: the flag values the common control actually returns
 * do not match the documented HDHT_* constants on every OS build (measured:
 * on-item arrives as 0x0002, divider as 0x0004), so trusting them made every
 * plain hover look like a divider grab. The right edge of the item rect is
 * where the divider lives on all of them. */
static int hdr_hit_column(HWND hh, LPARAM lParam)
{
    HDHITTESTINFO hti;
    memset(&hti, 0, sizeof(hti));
    hti.pt.x = (int)(short)LOWORD(lParam);
    hti.pt.y = (int)(short)HIWORD(lParam);
    SendMessageW(hh, HDM_HITTEST, 0, (LPARAM)&hti);
    if (hti.iItem < 0) return -1;
    RECT ir;
    if (Header_GetItemRect(hh, hti.iItem, &ir) &&
        hti.pt.x >= ir.right - DPIX(4))
        return -1;
    return hti.iItem;
}

static LRESULT CALLBACK header_subclass_proc(HWND hwnd, UINT msg,
                                             WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_MOUSEMOVE) {
        int col = hdr_hit_column(hwnd, lParam);
        if (col != g_hdr_hover) {
            int old = g_hdr_hover;
            g_hdr_hover = col;
            hdr_invalidate_item(hwnd, old);
            hdr_invalidate_item(hwnd, col);
        }
        TRACKMOUSEEVENT tme;
        tme.cbSize    = sizeof(tme);
        tme.dwFlags   = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
    } else if (msg == WM_MOUSELEAVE) {
        if (g_hdr_hover >= 0) {
            int old = g_hdr_hover;
            g_hdr_hover = -1;
            hdr_invalidate_item(hwnd, old);
        }
        if (g_hdr_press >= 0) {
            int old = g_hdr_press;
            g_hdr_press = -1;
            hdr_invalidate_item(hwnd, old);
        }
    } else if (msg == WM_LBUTTONDOWN) {
        int col = hdr_hit_column(hwnd, lParam);
        if (col >= 0 && col != g_hdr_press) {
            int old = g_hdr_press;
            g_hdr_press = col;
            hdr_invalidate_item(hwnd, old);
            hdr_invalidate_item(hwnd, col);
        }
    } else if (msg == WM_LBUTTONUP) {
        if (g_hdr_press >= 0) {
            int old = g_hdr_press;
            g_hdr_press = -1;
            hdr_invalidate_item(hwnd, old);
        }
    }
    return CallWindowProcW(g_hdr_orig_proc, hwnd, msg, wParam, lParam);
}

/* Subclass procedure for the playlist ListView.
 * Handles Ctrl+A (select all), Del (delete selected rows), forwards
 * drag-drop to the parent window, and custom-draws the header with a
 * white background (bypassing the themed gradient). */
static LRESULT CALLBACK list_subclass_proc(HWND hwnd, UINT msg,
                                           WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_DROPFILES) {
        /* ListView 有自己的 WS_EX_ACCEPTFILES，WM_DROPFILES 发给 ListView，
         * 转发给主窗口处理。 */
        return SendMessageW(GetParent(hwnd), WM_DROPFILES, wParam, lParam);
    }

    if (msg == WM_MOUSEMOVE) {
        /* Track which row the cursor is over so the custom draw can tint it. */
        LVHITTESTINFO hti;
        memset(&hti, 0, sizeof(hti));
        hti.pt.x = (int)(short)LOWORD(lParam);
        hti.pt.y = (int)(short)HIWORD(lParam);
        int row = ListView_HitTest(hwnd, &hti) ? hti.iItem : -1;
        if (row != g_player.hover_row) {
            int old = g_player.hover_row;
            g_player.hover_row = row;
            if (old >= 0) ListView_Update(hwnd, old);
            if (row >= 0) ListView_Update(hwnd, row);
        }
        /* Ask for a WM_MOUSELEAVE so the hover tint clears when the cursor
         * leaves the control entirely. */
        TRACKMOUSEEVENT tme;
        tme.cbSize    = sizeof(tme);
        tme.dwFlags   = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
    } else if (msg == WM_MOUSELEAVE) {
        if (g_player.hover_row >= 0) {
            ListView_Update(hwnd, g_player.hover_row);
            g_player.hover_row = -1;
        }
    } else if (msg == WM_KEYDOWN) {        if (wParam == VK_DELETE) {
            delete_selected_items();
            return 0;
        }
        if (wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            /* iItem = -1 applies the state to every item. */
            ListView_SetItemState(hwnd, -1, LVIS_SELECTED, LVIS_SELECTED);
            return 0;
        }
    } else if (msg == WM_CHAR) {
        /* Swallow the 0x01 byte Ctrl+A produces so the list-view does not
         * start a type-ahead search on it. */
        if (wParam == 1 && (GetKeyState(VK_CONTROL) & 0x8000))
            return 0;
    } else if (msg == WM_NOTIFY) {
        LPNMHDR nm = (LPNMHDR)lParam;

        /* --- Header: flat tinted strip with accent text ---
         * The header is a child of the ListView, so its NM_CUSTOMDRAW
         * arrives with hwndFrom == the header (not the list). It must be
         * handled before the list-row branch below. */
        if (nm->code == NM_CUSTOMDRAW && g_hHeader && nm->hwndFrom == g_hHeader) {
            LPNMCUSTOMDRAW cd = (LPNMCUSTOMDRAW)nm;
            if (cd->dwDrawStage == CDDS_PREPAINT) {
                FillRect(cd->hdc, &cd->rc, (HBRUSH)GetStockObject(NULL_BRUSH));
                return CDRF_NOTIFYITEMDRAW;
            }
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                /* Hover / press follow the same accent ramp as the buttons:
                 * resting tinted -> hover one step deeper -> press deepest,
                 * with the underline and the label ink darkening along. */
                int hcol = (int)cd->dwItemSpec;
                int h_press = (g_hdr_press == hcol);
                int h_hover = !h_press && (g_hdr_hover == hcol);
                COLORREF hfill  = h_press ? CLR_ACCENT_SOFT_PRESS
                                : h_hover ? CLR_ACCENT_SOFT_HOVER
                                          : CLR_ACCENT_SOFT;
                COLORREF hline  = h_press ? CLR_ACCENT_DARK
                                : h_hover ? CLR_ACCENT
                                          : CLR_ACCENT_EDGE;
                COLORREF htext  = h_press ? CLR_ACCENT_DARK : CLR_ACCENT_TEXT;

                fill_rect(cd->hdc, cd->rc.left, cd->rc.top,
                          cd->rc.right - cd->rc.left,
                          cd->rc.bottom - cd->rc.top, hfill);
                /* 2 DIP accent underline for a bit of weight. */
                fill_rect(cd->hdc, cd->rc.left, cd->rc.bottom - DPIX(2),
                          cd->rc.right - cd->rc.left, DPIX(2),
                          hline);
                HDITEMW hi; wchar_t text[64];
                memset(&hi, 0, sizeof(hi));
                hi.mask = HDI_TEXT;
                hi.pszText = text;
                hi.cchTextMax = 64;
                Header_GetItem(g_hHeader, cd->dwItemSpec, &hi);
                SetBkMode(cd->hdc, TRANSPARENT);
                SetTextColor(cd->hdc, htext);
                HFONT oldF = (HFONT)SelectObject(cd->hdc, g_player.hFont);
                RECT tr = cd->rc;
                /* Inset both edges by the same cell margin the report view
                 * applies to the values, so a label's alignment edge lands
                 * exactly on its column's text. Text columns are left-aligned
                 * and the four numeric ones right-aligned, matching the
                 * LVCFMT_* justification set on the columns themselves - the
                 * header does not inherit it because we paint the label by
                 * hand. */
                int right_aligned = (cd->dwItemSpec == 3 || cd->dwItemSpec == 4 ||
                                     cd->dwItemSpec == 5 || cd->dwItemSpec == 6);
                tr.left  += DPIX(COL_CELL_INSET);
                tr.right -= DPIX(COL_CELL_INSET);
                DrawTextW(cd->hdc, text ? text : L"", -1, &tr,
                          (right_aligned ? DT_RIGHT : DT_LEFT) |
                          DT_VCENTER | DT_SINGLELINE);
                SelectObject(cd->hdc, oldF);
                return CDRF_SKIPDEFAULT;
            }
            return CDRF_DODEFAULT;
        }

        if (nm->hwndFrom == hwnd && nm->code == NM_CUSTOMDRAW) {
            /* --- List rows: every pixel of a row is painted here ----------
             * Column text still arrives through LVN_GETDISPINFO, which the
             * parent handles, but the control draws no part of a row itself:
             * the item stage answers CDRF_SKIPDEFAULT, so the background, the
             * separators, the marker rail and all seven cells are ours.
             *
             * That is deliberate. Mixing an owner-drawn background with
             * control-drawn text does not work: a background filled during
             * one subitem's notification was repainted by the control's own
             * item fill for the next, which is why a strip of the row kept
             * the system selection colour no matter what was filled in.
             * Taking the whole row removes the ordering question entirely.
             *
             * Cell rectangles are accumulated from the column widths rather
             * than taken from LVM_GETSUBITEMRECT, which answers with the whole
             * row rectangle when asked for subitem zero. */
            LPNMLVCUSTOMDRAW rowcd = (LPNMLVCUSTOMDRAW)nm;
            /* Read once per paint pass. Reading them per row would repeat
             * seven sends for every visible item. */
            static int colw[7];
            static int colw_total;
            /* Snapshot per paint pass: current track + DPI-derived metrics.
             * The old code took the critical section and sent
             * LVM_GETITEMSTATE once per visible row, and re-queried every
             * cell's text with LVM_GETITEMTEXTW (which re-enters
             * LVN_GETDISPINFO). ~20 rows = ~160 cross-control Sends per
             * paint. Now: one lock per paint, zero Sends per row. */
            static int paint_cur = -1;
            static int m_gutter = 0, m_inset = 0, m_railw = 0;

            if (rowcd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                /* Empty playlist: paint a calm centred hint instead of an
                 * expanse of blank white. The control sends PREPAINT even
                 * with zero items, and CDRF_SKIPDEFAULT suppresses its own
                 * background, so the surface fill is ours too. */
                if (g_playlist.count == 0) {
                    HDC dc  = rowcd->nmcd.hdc;
                    RECT crc;
                    GetClientRect(hwnd, &crc);
                    fill_rect(dc, 0, 0, crc.right, crc.bottom, CLR_SURFACE);

                    const LangStrings *Ls = ui_lang();
                    int cx = crc.right / 2;
                    int cy = crc.bottom / 2;
                    SetBkMode(dc, TRANSPARENT);

                    RECT tr;
                    if (g_hFontEmpty && crc.bottom > DPIX(120)) {
                        /* Large audio glyph above the hint line. */
                        tr.left   = cx - DPIX(60);
                        tr.top    = cy - DPIX(44);
                        tr.right  = cx + DPIX(60);
                        tr.bottom = cy - DPIX(2);
                        SetTextColor(dc, CLR_ACCENT_EDGE);
                        HFONT of = (HFONT)SelectObject(dc, g_hFontEmpty);
                        wchar_t glyph = L'\xE8D6';
                        DrawTextW(dc, &glyph, 1, &tr,
                                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                        SelectObject(dc, of);
                        tr.left   = DPIX(20);
                        tr.top    = cy + DPIX(4);
                        tr.right  = crc.right - DPIX(20);
                        tr.bottom = cy + DPIX(4) + DPIX(24);
                    } else {
                        /* Too short for the glyph: text only, still centred. */
                        tr.left   = DPIX(20);
                        tr.top    = cy - DPIX(12);
                        tr.right  = crc.right - DPIX(20);
                        tr.bottom = cy + DPIX(12);
                    }
                    SetTextColor(dc, CLR_TEXT_FAINT);
                    HFONT of = (HFONT)SelectObject(dc, g_player.hFont);
                    DrawTextW(dc, Ls->l_empty_hint, -1, &tr,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                              DT_END_ELLIPSIS);
                    SelectObject(dc, of);
                    return CDRF_SKIPDEFAULT;
                }
                colw_total = 0;
                for (int c = 0; c < 7; c++) {
                    colw[c] = (int)SendMessageW(hwnd, LVM_GETCOLUMNWIDTH, c, 0);
                    colw_total += colw[c];
                }
                EnterCriticalSection(&g_player.cs);
                paint_cur = g_player.cur_index;
                LeaveCriticalSection(&g_player.cs);
                m_gutter = DPIX(COL_GUTTER);
                m_inset  = DPIX(COL_CELL_INSET);
                m_railw  = DPIX(4);
                /* Prefetch metadata for the rows about to be painted. Runs once
                 * per paint pass (not per row); the request is idempotent and
                 * keeps the lazy loader warming exactly the visible window.
                 * Custom-draw skips the default text path, so LVN_GETDISPINFO
                 * does not fire during paint and cannot be relied on here. */
                {
                    int top = ListView_GetTopIndex(hwnd);
                    int end = top + ListView_GetCountPerPage(hwnd) + 1;
                    if (end > g_playlist.count) end = g_playlist.count;
                    for (int r = (top < 0 ? 0 : top); r < end; r++)
                        playlist_meta_request(r);
                }
                return CDRF_NOTIFYITEMDRAW;
            }
            /* CDDS_ITEMPOSTPAINT and any stage added later must not fall
             * through to the default proc with this notification, or the
             * control would forward it back to the parent and the parent
             * would route it here again. */
            if (rowcd->nmcd.dwDrawStage != CDDS_ITEMPREPAINT)
                return CDRF_DODEFAULT;

            {
                int row = (int)rowcd->nmcd.dwItemSpec;
                if (row < 0 || row >= g_playlist.count || colw_total <= 0)
                    return CDRF_SKIPDEFAULT;

                HDC dc = rowcd->nmcd.hdc;
                RECT rr = rowcd->nmcd.rc;      /* the whole row */

                int cur = paint_cur;
                /* Selection MUST be queried per row: nmcd.uItemState is not
                 * reliable for LVS_OWNERDATA items (it reads selected for
                 * every row, painting the whole list tinted). One
                 * LVM_GETITEMSTATE per visible row is cheap; the expensive
                 * part was the 7 text round-trips, which stay eliminated. */
                int sel = (ListView_GetItemState(hwnd, row, LVIS_SELECTED)
                           & LVIS_SELECTED) != 0;
                int now = (row == cur);
                int rowW = rr.right - rr.left;
                int rowH = rr.bottom - rr.top;

                COLORREF bg = CLR_SURFACE;
                if (sel)                           bg = CLR_SEL_BG;
                else if (now)                      bg = CLR_SEL_BG;
                else if (row == g_player.hover_row) bg = CLR_HOVER_BG;

                fill_rect(dc, rr.left, rr.top, rowW, rowH, bg);

                /* Now-playing rail: full-height 4 DIP accent bar flush
                 * against the row's leading edge. Inset-free, so the first
                 * column's text margin (and header alignment) is untouched. */
                if (now)
                    fill_rect(dc, rr.left, rr.top, m_railw,
                              rowH, CLR_ACCENT);

                /* Row separator, held back from the card edge. */
                hline(dc, rr.left + m_gutter, rr.bottom - 1,
                      rowW - m_gutter, CLR_ROW_LINE);

                SetBkMode(dc, TRANSPARENT);
                HFONT oldF = (HFONT)SelectObject(dc, g_player.hFont);

                /* Direct playlist read: the playlist is only ever mutated
                 * on this (UI) thread, so custom-draw -- also on this
                 * thread -- may use the entry pointers directly. This
                 * replaces 7 LVM_GETITEMTEXTW Sends (each re-entering
                 * LVN_GETDISPINFO) per row with plain memory reads. */
                PlaylistRowView v;
                wchar_t name_b[256];
                playlist_row_view(row, &v, name_b, 256);
                wchar_t dur_b[32], br_b[32], sz_b[32];
                const wchar_t *cell_t[7];
                cell_t[0] = v.title;
                cell_t[1] = v.artist;
                cell_t[2] = v.album;
                if (v.duration > 0) {
                    ui_format_time(v.duration, dur_b, 32);
                    cell_t[3] = dur_b;
                } else cell_t[3] = L"--:--";
                if (v.bitrate > 0) {
                    _snwprintf(br_b, 32, L"%d kbps", v.bitrate);
                    br_b[31] = 0;
                    cell_t[4] = br_b;
                } else cell_t[4] = L"-";
                switch (v.format) {
                case 1:  cell_t[5] = L"MP3";  break;
                case 2:  cell_t[5] = L"FLAC"; break;
                case 3:  cell_t[5] = L"WAV";  break;
                case 4:  cell_t[5] = L"OGG";  break;
                default: cell_t[5] = L"?";    break;
                }
                if (v.file_size > 0) {
                    if (v.file_size < (uint64_t)1024 * 1024)
                        _snwprintf(sz_b, 32, L"%.1f KB",
                                   (double)v.file_size / 1024.0);
                    else
                        _snwprintf(sz_b, 32, L"%.1f MB",
                                   (double)v.file_size / 1048576.0);
                    sz_b[31] = 0;
                    cell_t[6] = sz_b;
                } else cell_t[6] = L"-";

                int x = rr.left;
                for (int c = 0; c < 7 && x < rr.right; c++) {
                    /* Both edges carry the same margin the header applies to
                     * its label, so a value sits exactly under its heading and
                     * the two never drift apart at any DPI. */
                    RECT tr = { x, rr.top, x + colw[c], rr.bottom };
                    tr.left  += m_inset;
                    tr.right -= m_inset;

                    const wchar_t *txt = cell_t[c];

                    /* Placeholder cells - the "-" and "--:--" of missing
                     * metadata - recede. There are a lot of them, and at full
                     * strength they competed with the values that matter. */
                    int placeholder = (txt[0] == L'-' || txt[0] == 0 ||
                                       txt[0] == L'?');
                    if (placeholder && txt[1] != 0 && txt[1] != L'-' &&
                        txt[1] != L':')
                        placeholder = 0;

                    COLORREF tc = placeholder    ? CLR_TEXT_FAINT
                                : (c == 0 && now)  ? CLR_ACCENT_TEXT
                                : (c == 0)         ? CLR_TEXT
                                :                    CLR_TEXT_MUTED;
                    SetTextColor(dc, tc);
                    /* Now-playing title is semibold: the row is readable
                     * as "the one that's playing" even without colour. */
                    HFONT cellF = (c == 0 && now && g_player.hFontBold)
                                ? g_player.hFontBold : g_player.hFont;
                    SelectObject(dc, cellF);
                    DrawTextW(dc, txt, -1, &tr,
                              (c >= 3 ? DT_RIGHT : DT_LEFT) |
                              DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    x += colw[c];
                }
                SelectObject(dc, oldF);
                return CDRF_SKIPDEFAULT;
            }
        }

    }
    return CallWindowProcW(g_list_orig_proc, hwnd, msg, wParam, lParam);
}

/* ---- Modern slider painting -------------------------------------------
 * The stock trackbar is replaced by our own flat rendering: a 4px groove
 * with the filled portion tinted in the accent colour and a circular
 * thumb with a soft halo. Hit testing / dragging still come from the
 * trackbar itself (TBM_GETCHANNELRECT / TBM_GETTHUMBRECT), so the
 * interaction model is unchanged - only the pixels are ours.
 *
 * `fill_ratio` is 0..1 for the progress bar and 0..1 for volume too.
 */
static void paint_slider(HWND hwnd, HDC dc, int lo_ratio_pct)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int midY = H / 2;
    int grooveH = DPIX(6);
    if (grooveH < 4) grooveH = 4;
    if (grooveH > DPIX(8)) grooveH = DPIX(8);
    int r = DPIX(8);                       /* thumb radius */

    /* Groove geometry comes from the control itself (TBM_GETCHANNELRECT),
     * so the painted groove and thumb share the exact mapping the
     * trackbar uses for hit-testing and dragging. The old W-r estimate
     * drifted a few px from the native thumb, so clicks landing on the
     * painted thumb's edge fell outside the native thumb rect and jumped
     * instead of grabbing for a drag. */
    int x0 = r;
    int x1 = W - r;
    {
        RECT ch;
        memset(&ch, 0, sizeof(ch));
        if (SendMessageW(hwnd, TBM_GETCHANNELRECT, 0, (LPARAM)&ch) &&
            ch.right > ch.left && ch.bottom > ch.top) {
            x0 = ch.left;
            x1 = ch.right;
            midY = (ch.top + ch.bottom) / 2;
        }
    }
    if (x1 <= x0) { x0 = r; x1 = W - r; }
    if (x1 <= x0) x1 = x0 + 1;

    int gy = midY - grooveH / 2;
    int gr = grooveH / 2;
    /* Static groove: fixed geometry per layout, so the rounded cache hits
     * every frame. Only the fill width below varies with playback. */
    draw_round_rect(dc, x0, gy, x1 - x0, grooveH,
                    gr, CLR_TRACK, CLR_TRACK);

    /* Filled portion: flat bar + fixed left cap. The old code passed the
     * live width into draw_round_rect, whose cache key is (w,h,r,fill) --
     * a width that slides every 200 ms never hits, so each timer tick paid
     * a full 4x DIB rebuild + per-pixel premultiply + destroy. This path
     * uses zero allocations: one cached cap + two solid fills. */
    int fw = (int)((x1 - x0) * (long long)lo_ratio_pct / 100);
    if (fw > 0) {
        if (fw > gr) {
            fill_rect(dc, x0 + gr, gy, fw - gr, grooveH, CLR_ACCENT);
            draw_round_rect(dc, x0, gy, gr * 2, grooveH,
                            gr, CLR_ACCENT, CLR_ACCENT);
        } else {
            fill_rect(dc, x0, gy, fw, grooveH, CLR_ACCENT);
        }
        /* Playhead tick at the fill edge: 2px accent-bright line. */
        if (fw >= 2)
            fill_rect(dc, x0 + fw - DPIX(1), gy - DPIX(2),
                      DPIX(1), grooveH + DPIX(4), CLR_ACCENT_HOVER);
    }

    /* Thumb: halo + disc via Ellipse with stock DC brush/pen (no DIB,
     * no Create/Delete in the hot path). */
    int tx = x0 + fw;
    if (tx < x0) tx = x0;
    if (tx > x1) tx = x1;
    int rr = r - DPIX(2);
    HGDIOBJ oldBr = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldPn = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, CLR_ACCENT_SOFT);
    SetDCPenColor(dc, CLR_ACCENT_SOFT);
    Ellipse(dc, tx - rr - DPIX(2), midY - rr - DPIX(2),
            tx + rr + DPIX(2), midY + rr + DPIX(2));
    SetDCBrushColor(dc, CLR_SURFACE);
    SetDCPenColor(dc, CLR_ACCENT);
    Ellipse(dc, tx - rr, midY - rr, tx + rr, midY + rr);
    /* Hot core dot so the thumb reads at small sizes. */
    SetDCBrushColor(dc, CLR_ACCENT);
    SetDCPenColor(dc, CLR_ACCENT);
    int core = rr / 2;
    if (core < 1) core = 1;
    Ellipse(dc, tx - core, midY - core, tx + core, midY + core);
    SelectObject(dc, oldPn);
    SelectObject(dc, oldBr);
}

/* Compute the fill percentage for a trackbar from its current position. */
static int slider_fill_pct(HWND hwnd)
{
    /* Ranges are fixed at creation (progress 0..1000, volume 0..100) and
     * never change afterwards, so only the position needs a round-trip
     * per paint instead of three Sends. */
    int lo = 0;
    int hi = (hwnd == g_player.hVolume) ? 100 : 1000;
    int pos = (int)SendMessageW(hwnd, TBM_GETPOS, 0, 0);
    if (hi <= lo) return 0;
    int pct = MulDiv(pos - lo, 100, hi - lo);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

/* Trackbar 默认点击轨道只按 page step 移动滑块，不会跳到鼠标位置。子类化后：
 * 点中滑块本体则交回原逻辑（用户仍可拖动），点中轨道则精确跳到点击处。
 * channel 矩形是轨道槽的真实可滑动范围，用它做比例换算最准确。 */
static WNDPROC g_progress_orig_proc = NULL;
static WNDPROC g_volume_orig_proc   = NULL;

/* Subclass procedure for the progress seekbar (TBM).
 * Trackbar defaults to page-step movement on click; this overrides that
 * so clicking anywhere on the track jumps to the exact position, while
 * dragging the thumb still works normally. */
static LRESULT CALLBACK progress_subclass_proc(HWND hwnd, UINT msg,
                                               WPARAM wParam, LPARAM lParam)
{
    /* Custom flat rendering: erase to the page colour, then draw our own
     * groove / fill / thumb. The stock control never gets to paint. */
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        fill_rect(dc, 0, 0, rc.right, rc.bottom, CLR_PAGE);
        paint_slider(hwnd, dc, slider_fill_pct(hwnd));
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND)
        return 1;                       /* background handled in WM_PAINT */

    if (msg == WM_LBUTTONDOWN) {
        /* 停止态下没有可跳转的曲目：不接受点击，否则会把进度条拖到一个
         * 无意义的位置（总帧数为 0，既不会真正 seek，界面状态也不一致）。 */
        int state, total;
        EnterCriticalSection(&g_player.cs);
        state = g_player.state;
        total = (int)(g_player.total_frames > 0);
        LeaveCriticalSection(&g_player.cs);
        if (state == STATE_STOPPED || !total)
            return 0;

        int x = (int)(short)LOWORD(lParam);
        int y = (int)(short)HIWORD(lParam);
        RECT thumb;
        SendMessageW(hwnd, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
        /* 点中滑块本体：交给原逻辑，用户可继续拖动。 */
        if (x >= thumb.left && x <= thumb.right &&
            y >= thumb.top  && y <= thumb.bottom) {
            LRESULT r = CallWindowProcW(g_progress_orig_proc, hwnd, msg, wParam, lParam);
            InvalidateRect(hwnd, NULL, FALSE);
            return r;
        }
        /* 点中轨道：精确跳到点击位置。 */
        RECT ch;
        SendMessageW(hwnd, TBM_GETCHANNELRECT, 0, (LPARAM)&ch);
        int lo = (int)SendMessageW(hwnd, TBM_GETRANGEMIN, 0, 0);
        int hi = (int)SendMessageW(hwnd, TBM_GETRANGEMAX, 0, 0);
        int cx = x;
        if (cx < ch.left)  cx = ch.left;
        if (cx > ch.right) cx = ch.right;
        int pos = (ch.right > ch.left)
            ? MulDiv(cx - ch.left, hi - lo, ch.right - ch.left) + lo
            : lo;
        SendMessageW(hwnd, TBM_SETPOS, TRUE, pos);
        InvalidateRect(hwnd, NULL, FALSE);
        /* 复用父窗口已有的 seek 逻辑（WM_HSCROLL 的 TB_THUMBPOSITION 分支）。 */
        SendMessageW(GetParent(hwnd), WM_HSCROLL,
                     MAKELONG(TB_THUMBPOSITION, 0), (LPARAM)hwnd);
        return 0;
    }

    /* Any position change must repaint our custom groove. */
    if (msg == TBM_SETPOS || msg == WM_MOUSEMOVE ||
        msg == WM_LBUTTONUP || msg == WM_KEYDOWN) {
        LRESULT r = CallWindowProcW(g_progress_orig_proc, hwnd, msg, wParam, lParam);
        InvalidateRect(hwnd, NULL, FALSE);
        return r;
    }
    return CallWindowProcW(g_progress_orig_proc, hwnd, msg, wParam, lParam);
}

/* Subclass procedure for the volume slider (TBM).
 * Same click-to-position behavior as the progress bar — clicking the
 * track jumps to the exact position; dragging the thumb is forwarded
 * to the default handler. */
static LRESULT CALLBACK volume_subclass_proc(HWND hwnd, UINT msg,
                                             WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        fill_rect(dc, 0, 0, rc.right, rc.bottom, CLR_PAGE);
        paint_slider(hwnd, dc, slider_fill_pct(hwnd));
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND)
        return 1;

    if (msg == WM_LBUTTONDOWN) {
        int x = (int)(short)LOWORD(lParam);
        int y = (int)(short)HIWORD(lParam);
        RECT thumb;
        SendMessageW(hwnd, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
        if (x >= thumb.left && x <= thumb.right &&
            y >= thumb.top  && y <= thumb.bottom) {
            LRESULT r = CallWindowProcW(g_volume_orig_proc, hwnd, msg, wParam, lParam);
            InvalidateRect(hwnd, NULL, FALSE);
            return r;
        }
        RECT ch;
        SendMessageW(hwnd, TBM_GETCHANNELRECT, 0, (LPARAM)&ch);
        int lo = (int)SendMessageW(hwnd, TBM_GETRANGEMIN, 0, 0);
        int hi = (int)SendMessageW(hwnd, TBM_GETRANGEMAX, 0, 0);
        int cx = x;
        if (cx < ch.left)  cx = ch.left;
        if (cx > ch.right) cx = ch.right;
        int pos = (ch.right > ch.left)
            ? MulDiv(cx - ch.left, hi - lo, ch.right - ch.left) + lo
            : lo;
        SendMessageW(hwnd, TBM_SETPOS, TRUE, pos);
        InvalidateRect(hwnd, NULL, FALSE);
        SendMessageW(GetParent(hwnd), WM_HSCROLL,
                     MAKELONG(TB_THUMBPOSITION, 0), (LPARAM)hwnd);
        return 0;
    }
    if (msg == TBM_SETPOS || msg == WM_MOUSEMOVE ||
        msg == WM_LBUTTONUP || msg == WM_KEYDOWN) {
        LRESULT r = CallWindowProcW(g_volume_orig_proc, hwnd, msg, wParam, lParam);
        InvalidateRect(hwnd, NULL, FALSE);
        return r;
    }
    return CallWindowProcW(g_volume_orig_proc, hwnd, msg, wParam, lParam);
}

/* ---- Custom "chip" and "readout" labels -------------------------------
 * The static controls are subclassed so we can draw them ourselves:
 *   - hLblStatus  -> a rounded status chip with a coloured state dot
 *   - hLblTime    -> a right-aligned monospace-ish readout
 *   - hLblVol     -> a tiny caption above the volume slider
 * Text still lives in the HWND (so GetWindowTextW keeps working), we just
 * paint it with our own font/colour instead of the stock grey. */
static WNDPROC g_status_orig_proc = NULL;
static WNDPROC g_time_orig_proc   = NULL;
static WNDPROC g_vol_orig_proc    = NULL;

static void paint_status_chip(HWND hwnd, HDC dc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;

    /* Chip background: white surface with a hairline border so it reads as a
     * discrete pill against the page colour. */
    draw_round_rect(dc, 0, 0, W, H, H / 2, CLR_SURFACE, CLR_BORDER);

    /* State dot: green when playing, amber when paused, grey when idle.
     * A 6 DIP disc with a soft ring of the same hue reads as a status LED
     * rather than a bullet, and stays legible at every DPI without
     * outweighing the label next to it. */
    int state;
    EnterCriticalSection(&g_player.cs);
    state = g_player.state;
    LeaveCriticalSection(&g_player.cs);
    COLORREF dot = (state == STATE_PLAYING) ? CLR_OK
                 : (state == STATE_PAUSED)  ? CLR_WARN
                                            : CLR_IDLE;
    /* Soft halo in a washed-out version of the state colour. */
    COLORREF halo = RGB((GetRValue(dot) + 255 * 3) / 4,
                        (GetGValue(dot) + 255 * 3) / 4,
                        (GetBValue(dot) + 255 * 3) / 4);
    int d  = DPIX(6);
    int cy = H / 2;
    int dotX = DPIX(12);
    draw_round_rect(dc, dotX - DPIX(2), cy - d / 2 - DPIX(2),
                    d + DPIX(4), d + DPIX(4), (d + DPIX(4)) / 2, halo, halo);
    draw_round_rect(dc, dotX, cy - d / 2, d, d, d / 2, dot, dot);

    /* Label text, truncated with an ellipsis when the window is narrow.
     * Read from our own buffer rather than the control: a stock static
     * would draw its own copy of the text on WM_SETTEXT. */
    wchar_t buf[256];
    wcsncpy(buf, g_player.lbl_status, 255);
    buf[255] = 0;
    int n = (int)wcslen(buf);
    RECT tr;
    tr.left   = dotX + d + DPIX(9);
    tr.right  = W - DPIX(12);
    tr.top    = 0;
    tr.bottom = H;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, CLR_TEXT_MUTED);
    HFONT oldF = (HFONT)SelectObject(dc, g_player.hFontSm);
    DrawTextW(dc, buf, n, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldF);
}

static void paint_time_readout(HWND hwnd, HDC dc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    /* Soft tinted pill so the readout reads as a discrete element. */
    draw_round_rect(dc, 0, 0, W, H, H / 2, CLR_ACCENT_SOFT, CLR_ACCENT_EDGE);

    wchar_t buf[128];
    wcsncpy(buf, g_player.lbl_time, 127);
    buf[127] = 0;
    int n = (int)wcslen(buf);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, CLR_ACCENT_TEXT);
    HFONT oldF = (HFONT)SelectObject(dc, g_player.hFontSm);
    DrawTextW(dc, buf, n, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldF);
}

/* The volume cluster: a Segoe MDL2 Assets glyph (volume / mute) followed by
 * the percentage readout, so the pair reads as one unit beside the slider.
 *
 *   E767  Volume       (speaker + waves)
 *   E74F  Mute         (speaker + cross)
 *   E992  Volume 0     (speaker, no waves)
 *   E993  Volume 1     (speaker + one wave)
 *   E994  Volume 2     (speaker + two waves)
 *   E995  Volume 3     (speaker + three waves)
 */
static void paint_vol_readout(HWND hwnd, HDC dc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);

    int vol;
    EnterCriticalSection(&g_player.cs);
    vol = g_player.volume;
    LeaveCriticalSection(&g_player.cs);

    wchar_t glyph;
    if (vol <= 0)       glyph = L'\xE74F';   /* mute               */
    else if (vol < 34)  glyph = L'\xE993';   /* volume 1           */
    else if (vol < 67)  glyph = L'\xE994';   /* volume 2           */
    else                glyph = L'\xE995';   /* volume 3           */

    COLORREF ink = (vol <= 0) ? CLR_IDLE : CLR_TEXT_MUTED;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ink);

    /* Glyph on the left, centred vertically against the row. */
    int iconW = DPIX(18);
    RECT ir = rc;
    ir.right = ir.left + iconW;
    HFONT oldF = (HFONT)SelectObject(dc, g_player.hFontIcon);
    DrawTextW(dc, &glyph, 1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldF);

    /* Readout: show just the number and the percent sign.  The word
     * "volume" is redundant next to a speaker glyph, and dropping it keeps
     * the cluster compact in every language pack (the translated prefix
     * varies a lot in width: "Volume:" / "Volumen:" / "音量:" ...). */
    wchar_t raw[64];
    wcsncpy(raw, g_player.lbl_vol, 63);
    raw[63] = 0;
    wchar_t buf[64];
    int  bi = 0;
    for (int i = 0; raw[i] && bi < 60; i++) {
        if ((raw[i] >= L'0' && raw[i] <= L'9') || raw[i] == L'%')
            buf[bi++] = raw[i];
    }
    buf[bi] = 0;
    if (bi == 0) {                       /* unexpected format: show as-is */
        wcsncpy(buf, raw, 60);
        buf[60] = 0;
    }

    SetTextColor(dc, CLR_TEXT_MUTED);
    oldF = (HFONT)SelectObject(dc, g_player.hFontSm);
    RECT tr = rc;
    tr.left += iconW;
    DrawTextW(dc, buf, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldF);
}

/* Generic subclass body shared by the three labels. */
static LRESULT CALLBACK label_paint_proc(HWND hwnd, UINT msg,
                                         WPARAM wParam, LPARAM lParam,
                                         WNDPROC orig, int kind)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        fill_rect(dc, 0, 0, rc.right, rc.bottom, CLR_PAGE);
        if (kind == 0)      paint_status_chip(hwnd, dc);
        else if (kind == 1) paint_time_readout(hwnd, dc);
        else                paint_vol_readout(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;

    /* The stock static would draw its own text whenever the caption is set,
     * which shows up as a faded ghost underneath our custom pill. Capture
     * the string ourselves, keep the control's caption empty, and repaint.
     * Returning without forwarding means the class proc never renders it. */
    if (msg == WM_SETTEXT) {
        const wchar_t *src = (const wchar_t *)lParam;
        wchar_t *dst = kind == 0 ? g_player.lbl_status
                    : kind == 1 ? g_player.lbl_time
                                : g_player.lbl_vol;
        int cap     = kind == 0 ? 256 : kind == 1 ? 128 : 64;
        if (src) { wcsncpy(dst, src, cap - 1); dst[cap - 1] = 0; }
        else     { dst[0] = 0; }
        InvalidateRect(hwnd, NULL, FALSE);
        return TRUE;
    }
    if (msg == WM_GETTEXTLENGTH) {
        const wchar_t *src = kind == 0 ? g_player.lbl_status
                           : kind == 1 ? g_player.lbl_time
                                       : g_player.lbl_vol;
        return (LRESULT)wcslen(src);
    }
    if (msg == WM_GETTEXT) {
        const wchar_t *src = kind == 0 ? g_player.lbl_status
                           : kind == 1 ? g_player.lbl_time
                                       : g_player.lbl_vol;
        int cap = (int)wParam;
        if (cap <= 0) return 0;
        wcsncpy((wchar_t *)lParam, src, (size_t)cap - 1);
        ((wchar_t *)lParam)[cap - 1] = 0;
        return (LRESULT)wcslen((wchar_t *)lParam);
    }

    /* Off-screen renderers ask for the client area explicitly; paint the
     * same content we would in WM_PAINT so screenshots match the screen. */
    if (msg == WM_PRINTCLIENT) {
        HDC dc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        fill_rect(dc, 0, 0, rc.right, rc.bottom, CLR_PAGE);
        if (kind == 0)      paint_status_chip(hwnd, dc);
        else if (kind == 1) paint_time_readout(hwnd, dc);
        else                paint_vol_readout(hwnd, dc);
        return 0;
    }
    return CallWindowProcW(orig, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK status_subclass_proc(HWND hwnd, UINT msg,
                                             WPARAM wParam, LPARAM lParam)
{
    return label_paint_proc(hwnd, msg, wParam, lParam, g_status_orig_proc, 0);
}

static LRESULT CALLBACK time_subclass_proc(HWND hwnd, UINT msg,
                                           WPARAM wParam, LPARAM lParam)
{
    return label_paint_proc(hwnd, msg, wParam, lParam, g_time_orig_proc, 1);
}

static LRESULT CALLBACK vol_subclass_proc(HWND hwnd, UINT msg,
                                          WPARAM wParam, LPARAM lParam)
{
    return label_paint_proc(hwnd, msg, wParam, lParam, g_vol_orig_proc, 2);
}

/* ---- Transport button hover / press tracking --------------------------
 * The buttons are owner-draw, so they paint from WM_DRAWITEM. To make the
 * hover state known we subclass them and forward WM_MOUSEMOVE /
 * WM_MOUSELEAVE to the parent, which keeps a single "hovered control id"
 * and repaints. Returning 0 lets the button still handle the click. */
static WNDPROC g_btn_orig_proc = NULL;

static LRESULT CALLBACK button_subclass_proc(HWND hwnd, UINT msg,
                                             WPARAM wParam, LPARAM lParam)
{
    int id = GetDlgCtrlID(hwnd);
    if (msg == WM_MOUSEMOVE) {
        if (g_player.hover_btn != id) {
            int old = g_player.hover_btn;
            g_player.hover_btn = id;
            ui_repaint_btn(old);
            ui_repaint_btn(id);
        }
        TRACKMOUSEEVENT tme;
        tme.cbSize    = sizeof(tme);
        tme.dwFlags   = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
    } else if (msg == WM_MOUSELEAVE) {
        if (g_player.hover_btn == id) {
            int old = g_player.hover_btn;
            g_player.hover_btn = 0;
            ui_repaint_btn(old);
        }
    } else if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) {
        LRESULT r = CallWindowProcW(g_btn_orig_proc, hwnd, msg, wParam, lParam);
        InvalidateRect(hwnd, NULL, FALSE);
        return r;
    }
    return CallWindowProcW(g_btn_orig_proc, hwnd, msg, wParam, lParam);
}

/* ---- Forward declaration --------------------------------------------- */
static void on_track_ended(LONG gen);

/* ---- Card composition -------------------------------------------------
 * Paints the page background and the playlist card: a white rounded panel
 * with a hairline border, drawn behind the ListView.
 *
 * The card's corners are rounded natively by clipping the ListView's own
 * window region (ui_layout calls SetWindowRgn), so the control keeps
 * painting its own pixels - scrollbar, selection, keyboard and all. The
 * control is inset by 1 DIP, so the panel shows as a crisp white ring
 * around the list and the region-clip stair-stepping stays hidden behind
 * that ring. */

static void ui_card_rect(HWND hwnd, RECT *out)
{
    int pad  = DPIX(14);
    int gap  = DPIX(10);
    int btnH = DPIX(34);
    int sldH = DPIX(22);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int H = rc.bottom;

    int bottomRowY = H - pad - btnH;
    int seekRowY   = bottomRowY - gap - sldH;
    int cardY      = pad;
    int cardH      = seekRowY - gap - cardY;
    int minH       = DPIX(80);
    if (cardH < minH) cardH = minH;

    out->left   = pad;
    out->top    = cardY;
    out->right  = rc.right - pad;
    out->bottom = cardY + cardH;
}

static void ui_compose_card(HWND hwnd, HDC dc, int cw, int ch)
{
    fill_rect(dc, 0, 0, cw, ch, CLR_PAGE);

    if (!g_player.hList) return;
    RECT cr;
    ui_card_rect(hwnd, &cr);
    int cwd = cr.right - cr.left;
    int chg = cr.bottom - cr.top;
    if (cwd <= 0 || chg <= 0) return;

    /* White panel + hairline border, both with the card radius. */
    int r = DPIX(10);
    draw_round_rect(dc, cr.left, cr.top, cwd, chg, r, CLR_SURFACE, CLR_BORDER);
}

/* ---- Main window procedure ------------------------------------------- */
LRESULT CALLBACK main_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    case WM_CREATE: {
        g_player.hMain = hwnd;
        g_player.dpi   = ui_get_dpi(hwnd);

        /* The global is zero-initialised, which leaves cur_index at 0 and
         * hover_row at 0 while both are documented as "-1 when nothing
         * applies". Left alone, a freshly loaded playlist shows row 0 as
         * selected, tinted and carrying the now-playing rail although
         * nothing plays, and row 0 looks hovered before the pointer ever
         * enters the list. */
        EnterCriticalSection(&g_player.cs);
        g_player.cur_index = -1;
        LeaveCriticalSection(&g_player.cs);
        g_player.hover_row = -1;

        /* ListView (virtual mode: LVS_OWNERDATA — 控件不存储项数据，
         * 只对可见行请求 LVN_GETDISPINFO，大量项时性能恒定)。 */
        g_player.hList = CreateWindowExW(WS_EX_ACCEPTFILES,
            WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
            0, 0, 0, 0, hwnd, (HMENU)IDC_LIST, NULL, NULL);
        ListView_SetExtendedListViewStyle(g_player.hList,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        /* Explicit colours: the player paints a white surface regardless of
         * the window theme, so the list matches the card look. */
        ListView_SetBkColor(g_player.hList, CLR_SURFACE);
        ListView_SetTextBkColor(g_player.hList, CLR_SURFACE);
        ListView_SetTextColor(g_player.hList, CLR_TEXT);

        /* Seven columns: 标题 / 艺术家 / 专辑 / 时长 / 比特率 / 格式 / 大小.
         * Widths are 96dpi pixels; DPIX() scales them per-monitor. The user
         * can resize columns at runtime via the header dividers.
         * Rows carry DPIX(14) of left padding and DPIX(8) of right padding
         * on top of these widths, so the narrow numeric columns need a
         * little more room than the bare text would suggest ('--:--' must
         * not ellipsise down to '-:-').
         * 'name' is a placeholder — the actual header text is set from
         * col_names below. */
        struct { const wchar_t *name; int w; } cols[] = {
            { L"", 210 },
            { L"", 110 },
            { L"", 110 },
            { L"", 62  },
            { L"", 72  },
            { L"", 58  },
            { L"", 68  },
        };
        /* Resolve column header names from the current language. */
        const LangStrings *L_init = ui_lang();
        const wchar_t *col_names[] = {
            L_init->c_title, L_init->c_artist, L_init->c_album,
            L_init->c_duration, L_init->c_bitrate, L_init->c_format,
            L_init->c_size
        };
        /* Insert columns in reverse order (right-to-left) because
         * ListView_InsertColumn at index 0 shifts existing columns right;
         * building from the last column forward yields the correct order.
         * The four numeric columns carry LVCFMT_RIGHT so their header labels
         * share the same right edge as the values painted below them. */
        for (int i = 6; i >= 0; i--) {
            LVCOLUMNW col;
            memset(&col, 0, sizeof(col));
            col.mask    = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            col.cx       = DPIX(cols[i].w);
            col.pszText  = (LPWSTR)col_names[i];
            col.fmt      = (i >= 3) ? LVCFMT_RIGHT : LVCFMT_LEFT;
            ListView_InsertColumn(g_player.hList, 0, &col);
        }

        /* Subclass the list so Ctrl+A (select all) and Del (delete every
         * selected row) are handled by the control that owns keyboard focus;
         * the main window never sees these keys. */
        g_list_orig_proc = (WNDPROC)SetWindowLongPtrW(g_player.hList,
                                GWLP_WNDPROC, (LONG_PTR)list_subclass_proc);

        /* ListView header: grab HWND for custom draw, subclass it so the
         * hovered / pressed column can be tracked, then apply the per-DPI
         * metrics: a 1px spacer image list buys generous 30 DIP rows, and a
         * slightly taller header (32 DIP) gives the column labels room to
         * breathe. ui_apply_list_metrics() is re-run on WM_DPICHANGED. */
        g_hHeader = ListView_GetHeader(g_player.hList);
        if (g_hHeader) {
            HFONT hf = g_player.hFont ? g_player.hFont
                                      : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            SendMessageW(g_hHeader, WM_SETFONT, (WPARAM)hf, TRUE);
            g_hdr_orig_proc = (WNDPROC)SetWindowLongPtrW(g_hHeader, GWLP_WNDPROC,
                                    (LONG_PTR)header_subclass_proc);
        }
        ui_apply_list_metrics();

        /* Owner-draw buttons: white background + thin grey border + black
         * text (Win11-flat style, no shadows). */
        g_player.hBtnPrev = CreateWindowExW(0, L"button", L_init->b_prev,
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0,0,0,0, hwnd, (HMENU)IDC_BTN_PREV, NULL, NULL);
        g_player.hBtnPlay = CreateWindowExW(0, L"button", L_init->b_play,
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0,0,0,0, hwnd, (HMENU)IDC_BTN_PLAY, NULL, NULL);
        g_player.hBtnStop = CreateWindowExW(0, L"button", L_init->b_stop,
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0,0,0,0, hwnd, (HMENU)IDC_BTN_STOP, NULL, NULL);
        g_player.hBtnNext = CreateWindowExW(0, L"button", L_init->b_next,
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0,0,0,0, hwnd, (HMENU)IDC_BTN_NEXT, NULL, NULL);
        g_player.hBtnMode = CreateWindowExW(0, L"button", mode_label(MODE_LIST_LOOP),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0,0,0,0, hwnd, (HMENU)IDC_BTN_MODE, NULL, NULL);

        /* Subclass every transport button so it can report hover / press
         * (used only to enrich the owner-draw painting). */
        {
            HWND btns[] = { g_player.hBtnPrev, g_player.hBtnPlay,
                            g_player.hBtnStop, g_player.hBtnNext,
                            g_player.hBtnMode };
            for (int i = 0; i < 5; i++) {
                WNDPROC op = (WNDPROC)SetWindowLongPtrW(btns[i],
                                GWLP_WNDPROC, (LONG_PTR)button_subclass_proc);
                if (!g_btn_orig_proc) g_btn_orig_proc = op;  /* all identical */
            }
            g_player.hover_btn = 0;
        }

        /* Progress + volume sliders. */
        g_player.hProgress = CreateWindowExW(0, WC_TRACKBARCLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            0,0,0,0, hwnd, (HMENU)IDC_PROGRESS, NULL, NULL);
        SendMessageW(g_player.hProgress, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
        SendMessageW(g_player.hProgress, TBM_SETPOS, TRUE, 0);
        /* 子类化进度条：点击轨道精确跳转到鼠标位置（原生只按 page step 移动）。 */
        g_progress_orig_proc = (WNDPROC)SetWindowLongPtrW(g_player.hProgress,
                                GWLP_WNDPROC, (LONG_PTR)progress_subclass_proc);

        g_player.hVolume = CreateWindowExW(0, WC_TRACKBARCLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            0,0,0,0, hwnd, (HMENU)IDC_VOLUME, NULL, NULL);
        SendMessageW(g_player.hVolume, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendMessageW(g_player.hVolume, TBM_SETPOS, TRUE, 75);
        g_volume_orig_proc = (WNDPROC)SetWindowLongPtrW(g_player.hVolume,
                                GWLP_WNDPROC, (LONG_PTR)volume_subclass_proc);

        /* Labels (custom-painted: status chip / time pill / volume caption).
         * They are created empty -- the visible caption lives in the Player
         * buffers and is painted by our subclass, so the stock static never
         * renders its own text behind ours. */
        g_player.hLblTime   = CreateWindowExW(0, L"static", L"",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 0,0,0,0, hwnd, (HMENU)IDC_LBL_TIME, NULL, NULL);
        g_player.hLblVol    = CreateWindowExW(0, L"static", L"",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 0,0,0,0, hwnd, (HMENU)IDC_LBL_VOL, NULL, NULL);
        g_player.hLblStatus = CreateWindowExW(0, L"static", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 0,0,0,0, hwnd, (HMENU)IDC_LBL_STATUS, NULL, NULL);

        g_status_orig_proc = (WNDPROC)SetWindowLongPtrW(g_player.hLblStatus,
                                GWLP_WNDPROC, (LONG_PTR)status_subclass_proc);
        g_time_orig_proc = (WNDPROC)SetWindowLongPtrW(g_player.hLblTime,
                                GWLP_WNDPROC, (LONG_PTR)time_subclass_proc);
        g_vol_orig_proc = (WNDPROC)SetWindowLongPtrW(g_player.hLblVol,
                                GWLP_WNDPROC, (LONG_PTR)vol_subclass_proc);

        /* Seed the paint buffers (the subclass owns them from here on). */
        wcsncpy(g_player.lbl_time, L"0:00 / 0:00", 127);
        g_player.lbl_time[127] = 0;
        wcsncpy(g_player.lbl_status, L_init->s_ready, 255);
        g_player.lbl_status[255] = 0;

        g_player.volume = 75;
        {
            wchar_t lbl[32];
            _snwprintf(lbl, 32, L_init->l_volume, 75);
            lbl[31] = 0;
            SetWindowTextW(g_player.hLblVol, lbl);
        }

        ui_set_font_for_dpi(g_player.dpi);
        ui_layout();

        /* 显式注册拖放接受。主窗口与 ListView 在 CreateWindowExW 时已带
         * WS_EX_ACCEPTFILES，这里再调一次 DragAcceptFiles 是保险——确保
         * 即使扩展样式因某种原因被剥离，拖放仍可用。 */
        DragAcceptFiles(hwnd, TRUE);
        DragAcceptFiles(g_player.hList, TRUE);

        /* 以管理员权限运行时，UIPI 会阻止来自普通权限资源管理器的
         * WM_DROPFILES。用 ChangeWindowMessageFilterEx 放行该消息。 */
        {
            typedef BOOL (WINAPI *PFN_ChangeMsgFilterEx)(HWND, UINT, DWORD, void *);
            PFN_ChangeMsgFilterEx pFilter = (PFN_ChangeMsgFilterEx)
                GetProcAddress(GetModuleHandleW(L"user32.dll"),
                               "ChangeWindowMessageFilterEx");
            if (pFilter) {
                /* MSGFLT_ALLOW = 1 */
                pFilter(hwnd, WM_DROPFILES, 1, NULL);
                pFilter(g_player.hList, WM_DROPFILES, 1, NULL);
            }
        }

        SetTimer(hwnd, TIMER_ID_POSITION, TIMER_INTERVAL_MS, NULL);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        /* The static controls are subclassed and paint themselves, so this
         * only matters for any stray static/checkbox the system creates.
         * Return the page brush so the window blends with the theme. */
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, CLR_TEXT);
        SetBkColor(hdc, CLR_PAGE);
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }

    case WM_ERASEBKGND: {
        /* Fill the client area with the page colour so gaps between the
         * card and the transport row read as intentional whitespace. */
        RECT rc;
        GetClientRect(hwnd, &rc);
        fill_rect((HDC)wParam, 0, 0, rc.right, rc.bottom, CLR_PAGE);
        return 1;
    }

    case WM_PAINT: {
        /* Page background, then the playlist card.
         *
         * The ListView is a child HWND, so it always wants to paint a hard
         * rectangle that would square off the card's corners and (with a
         * native scrollbar) clash with the flat look. Instead of letting it
         * paint directly, we capture its content with WM_PRINTCLIENT into a
         * memory DC and composite it back through a rounded alpha mask.
         * The control keeps its styles (scroll, selection, keyboard) - we
         * only take over where its pixels land. */
        static int in_compose = 0;      /* guard against nested paints */
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        if (in_compose) {               /* nested: just fill and bail */
            fill_rect(dc, 0, 0, rc.right, rc.bottom, CLR_PAGE);
            EndPaint(hwnd, &ps);
            return 0;
        }
        in_compose = 1;
        ui_compose_card(hwnd, dc, rc.right, rc.bottom);
        in_compose = 0;
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_PRINTCLIENT: {
        /* Same composition, but into the caller's DC. Needed by screen
         * readers, remote desktop, and off-screen capture tools. */
        HDC dc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        ui_compose_card(hwnd, dc, rc.right, rc.bottom);
        return 0;
    }

    case WM_DRAWITEM: {
        /* Modern pill buttons.
         *  - the play/pause button is the primary action: solid accent fill
         *    with white, semibold text
         *  - prev/stop/next are secondary: tinted surface with an accent
         *    border and accent text
         *  - the mode button is a second secondary style, slightly wider
         * All states (normal / hover / pressed / disabled) get their own
         * colour so the control feels alive the way a web button does. */
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
        if (dis->CtlType != ODT_BUTTON)
            break;

        /* rcItem is client-relative for the normal WM_PAINT path.  Some
         * off-screen renderers (PrintWindow / WM_PRINT, used by screenshot,
         * accessibility and remote-desktop tools) hand us screen coordinates
         * instead, which would paint the pill outside the target bitmap.
         * Translate back in that case so we always draw where the caller
         * expects. */
        RECT rc = dis->rcItem;
        if (dis->hwndItem && (rc.left != 0 || rc.top != 0)) {
            POINT org = { 0, 0 };
            ClientToScreen(dis->hwndItem, &org);
            rc.left   -= org.x;
            rc.top    -= org.y;
            rc.right  -= org.x;
            rc.bottom -= org.y;
        }
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return TRUE;

        /* The button carries no background brush of its own, so anything the
         * previous frame left behind would show through the anti-aliased
         * corners. Clear the full client rect with the page colour first. */
        fill_rect(dis->hDC, rc.left, rc.top, w, h, CLR_PAGE);

        int is_primary = (dis->CtlID == IDC_BTN_PLAY);
        int is_press   = (dis->itemState & ODS_SELECTED) != 0;
        int is_dis     = (dis->itemState & ODS_DISABLED) != 0;
        int is_hover   = (g_player.hover_btn == (int)dis->CtlID);

        /* Hover deepens the fill one step and press one step further, so the
         * three states read as a single ramp rather than as unrelated colours.
         * The hover flag comes from button_subclass_proc, which repaints on
         * every crossing; before this the fill ignored it, so the pointer paid
         * for a repaint that changed nothing. */
        COLORREF fill, edge, text;
        if (is_dis) {
            fill = CLR_ACCENT_SOFT;  edge = CLR_BORDER;      text = CLR_TEXT_FAINT;
        } else if (is_primary) {
            fill = is_press ? CLR_ACCENT_DARK
                            : (is_hover ? CLR_ACCENT_HOVER : CLR_ACCENT);
            edge = fill;
            text = RGB(255, 255, 255);
        } else {
            fill = is_press ? CLR_ACCENT_SOFT_PRESS
                            : (is_hover ? CLR_ACCENT_SOFT_HOVER
                                        : CLR_ACCENT_SOFT);
            edge = CLR_ACCENT_EDGE;
            text = CLR_ACCENT_TEXT;
        }

        /* Fully-rounded pill: the radius must be half the button height so
         * the end caps are true semicircles at any DPI. Using the design
         * token directly would leave a 15px radius on a 60px button, which
         * reads as a rounded rectangle rather than a pill. */
        int radius = h / 2;
        draw_round_rect(dis->hDC, rc.left, rc.top, w, h, radius, fill, edge);

        /* Text: icon glyph + label, centred as one unit. The MDL2 icon
         * makes transport readable in every language; the text keeps the
         * five i18n packs visible. Played from cached fonts, no GDI
         * allocation in this path. */
        wchar_t label[64];
        int n = GetWindowTextW(dis->hwndItem, label, 64);
        HFONT f = (is_primary && g_player.hFontBold) ? g_player.hFontBold
                                                     : g_player.hFont;
        wchar_t glyph = 0;
        if (dis->CtlID == IDC_BTN_PREV)      glyph = L'\xE892';
        else if (dis->CtlID == IDC_BTN_NEXT) glyph = L'\xE893';
        else if (dis->CtlID == IDC_BTN_STOP) glyph = L'\xE71A';
        else if (dis->CtlID == IDC_BTN_PLAY) {
            int st;
            EnterCriticalSection(&g_player.cs);
            st = g_player.state;
            LeaveCriticalSection(&g_player.cs);
            glyph = (st == STATE_PLAYING) ? L'\xE769' : L'\xE768';
        } else if (dis->CtlID == IDC_BTN_MODE) {
            int md;
            EnterCriticalSection(&g_player.cs);
            md = g_player.play_mode;
            LeaveCriticalSection(&g_player.cs);
            glyph = (md == MODE_SINGLE_LOOP) ? L'\xE8ED'
                  : (md == MODE_SHUFFLE)     ? L'\xE8B1' : L'\xE8EE';
        }
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, text);
        RECT tr = rc;
        if (is_press) { tr.left += 1; tr.top += 1; }
        if (glyph && g_player.hFontIcon && n > 0) {
            HFONT fi = g_player.hFontIcon;
            SIZE tsz = { 0, 0 }, gsz = { 0, 0 };
            HDC mdc = CreateCompatibleDC(dis->hDC);
            if (mdc) {
                HFONT of = (HFONT)SelectObject(mdc, f);
                GetTextExtentPoint32W(mdc, label, n, &tsz);
                SelectObject(mdc, fi);
                GetTextExtentPoint32W(mdc, &glyph, 1, &gsz);
                SelectObject(mdc, of);
                DeleteDC(mdc);
                int gap = DPIX(6);
                int total = gsz.cx + gap + tsz.cx;
                if (total > w - DPIX(8)) {
                    /* Too tight for glyph+label (e.g. shrunk mode pill):
                     * fall back to centred text so the i18n label and the
                     * mode state stay readable. */
                    HFONT oo = (HFONT)SelectObject(dis->hDC, f);
                    DrawTextW(dis->hDC, label, n, &tr,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                              DT_END_ELLIPSIS);
                    SelectObject(dis->hDC, oo);
                } else {
                int bx = rc.left + (w - total) / 2;
                if (bx < rc.left + DPIX(6)) bx = rc.left + DPIX(6);
                RECT gr2 = { bx, tr.top, bx + gsz.cx, tr.bottom };
                HFONT og = (HFONT)SelectObject(dis->hDC, fi);
                DrawTextW(dis->hDC, &glyph, 1, &gr2,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dis->hDC, og);
                tr.left = bx + gsz.cx + gap;
                HFONT oo = (HFONT)SelectObject(dis->hDC, f);
                DrawTextW(dis->hDC, label, n, &tr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                          DT_END_ELLIPSIS);
                SelectObject(dis->hDC, oo);
                }
            } else {
                HFONT oldF = (HFONT)SelectObject(dis->hDC, f);
                DrawTextW(dis->hDC, label, n, &tr,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                          DT_END_ELLIPSIS);
                SelectObject(dis->hDC, oldF);
            }
        } else {
            HFONT oldF = (HFONT)SelectObject(dis->hDC, f);
            DrawTextW(dis->hDC, label, n, &tr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dis->hDC, oldF);
        }

        /* Focus ring: 1px accent frame, allocation-free (DC_BRUSH). */
        if ((dis->itemState & ODS_FOCUS) && !is_press) {
            RECT fr = rc;
            InflateRect(&fr, -DPIX(3), -DPIX(3));
            SetDCBrushColor(dis->hDC, edge);
            FrameRect(dis->hDC, &fr,
                      (HBRUSH)GetStockObject(DC_BRUSH));
        }
        return TRUE;
    }

    /* Track the mouse over the transport buttons so they can render a
     * hover state. The messages arrive from the buttons themselves via the
     * TME_LEAVE set up in button_subclass_proc. */
    case WM_MOUSEMOVE: {
        POINT pt;
        GetCursorPos(&pt);
        HWND hit = WindowFromPoint(pt);
        int id = hit ? GetDlgCtrlID(hit) : 0;
        if (id != IDC_BTN_PREV && id != IDC_BTN_PLAY && id != IDC_BTN_STOP &&
            id != IDC_BTN_NEXT && id != IDC_BTN_MODE)
            id = 0;
        if (id != g_player.hover_btn) {
            int old = g_player.hover_btn;
            g_player.hover_btn = id;
            ui_repaint_btn(old);
            ui_repaint_btn(id);
        }
        break;
    }
    case WM_MOUSELEAVE: {
        if (g_player.hover_btn) {
            int old = g_player.hover_btn;
            g_player.hover_btn = 0;
            ui_repaint_btn(old);
        }
        break;
    }

    case WM_DPICHANGED: {
        /* Trust the message, but cross-check against the window/monitor API:
         * a spurious WM_DPICHANGED carrying 96 can arrive while the window
         * is being shown, and blindly accepting it would leave every child
         * laid out at 100% on a high-DPI monitor. */
        int dpi = (int)LOWORD(wParam);
        int real = ui_get_dpi(hwnd);
        if (real > 0 && real != dpi) {
            /* The monitor we are actually on is the source of truth. */
            dpi = real;
        }
        if (dpi > 0 && dpi != g_player.dpi) {
            g_player.dpi = dpi;
            ui_set_font_for_dpi(g_player.dpi);
            /* Row height and header strip are device-pixel metrics: rebuild
             * them at the new scale, or rows stay sized for the old monitor
             * after a per-monitor DPI switch. */
            ui_apply_list_metrics();
        }
        LPRECT pr = (LPRECT)lParam;
        if (pr && (pr->right - pr->left) > 0 && (pr->bottom - pr->top) > 0) {
            SetWindowPos(hwnd, NULL, pr->left, pr->top,
                         pr->right - pr->left, pr->bottom - pr->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ui_layout();
        return 0;
    }

    case WM_GETMINMAXINFO: {
        /* 限制最小窗口尺寸，防止用户把窗口缩到 0x0 导致控件重叠不可用。
         * 按 DPI 缩放：96dpi 下 480x360，高 DPI 自动放大。 */
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        mmi->ptMinTrackSize.x = DPIX(480);
        mmi->ptMinTrackSize.y = DPIX(360);
        return 0;
    }

    case WM_SIZE:
        /* Relayout only on a real size change. The handler itself is now cheap:
         * ui_layout() batches every child MoveWindow() with bRepaint=FALSE and
         * issues a single consolidated invalidation at the end, so a resize
         * drag pays one paint pass instead of one per child control. */
        if (wParam != SIZE_MINIMIZED) {
            ui_layout();
        }
        return 0;

    /* Drag & drop: files and folders are queued for chunked import. */
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        UINT n = DragQueryFileW(hDrop, 0xFFFFFFFFU, NULL, 0);
        for (UINT i = 0; i < n; i++) {
            UINT len = DragQueryFileW(hDrop, i, NULL, 0);
            wchar_t *path = (wchar_t *)malloc((size_t)(len + 1) * sizeof(wchar_t));
            if (!path) continue;
            DragQueryFileW(hDrop, i, path, len + 1);
            /* playlist_import_path handles both a single file and a folder
             * (walked in slices); no attribute probe needed here. */
            playlist_import_path(path);
            free(path);
        }
        DragFinish(hDrop);
        ui_run_import();
        return 0;
    }

    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        const LangStrings *L = ui_lang();
        switch (id) {
        case IDC_BTN_PLAY: {
            /* state 由解码线程写，需持锁读（无锁读是正式的数据竞争）。 */
            int state, cur;
            EnterCriticalSection(&g_player.cs);
            state = g_player.state;
            cur   = g_player.cur_index;
            LeaveCriticalSection(&g_player.cs);
            if (state == STATE_STOPPED && g_playlist.count > 0) {
                /* Start playing the selected item (or current, or first). */
                int sel = ListView_GetNextItem(g_player.hList, -1, LVNI_SELECTED);
                if (sel < 0) sel = cur;
                if (sel < 0) sel = 0;
                player_open(sel);
            } else {
                player_toggle_pause();
            }
            ui_update_state_controls();
            break;
        }
        case IDC_BTN_STOP:
            player_stop();
            break;
        case IDC_BTN_PREV: {
            int cur, mode;
            EnterCriticalSection(&g_player.cs);
            cur  = g_player.cur_index;
            mode = g_player.play_mode;
            LeaveCriticalSection(&g_player.cs);
            int prev = playlist_prev_index(cur, mode);
            if (prev >= 0) player_open(prev);
            break;
        }
        case IDC_BTN_NEXT: {
            int cur, mode;
            EnterCriticalSection(&g_player.cs);
            cur  = g_player.cur_index;
            mode = g_player.play_mode;
            LeaveCriticalSection(&g_player.cs);
            int next = playlist_next_index(cur, mode);
            if (next >= 0) player_open(next);
            break;
        }
        case IDC_BTN_MODE: {
            int mode;
            EnterCriticalSection(&g_player.cs);
            mode = g_player.play_mode = (g_player.play_mode + 1) % 3;
            LeaveCriticalSection(&g_player.cs);
            player_set_mode(mode);
            SetWindowTextW(g_player.hBtnMode, mode_label(mode));
            break;
        }
        case IDM_FILE_ADD_FILES:
            add_files_dialog();
            break;
        case IDM_FILE_ADD_FOLDER:
            add_folder_dialog();
            break;
        case IDM_FILE_LOAD_M3U:
            load_m3u_dialog();
            break;
        case IDM_FILE_SAVE_M3U:
            save_m3u_dialog();
            break;
        case IDM_FILE_CLEAR:
            /* 顺序：先发停止命令（异步），再清空列表。player_stop 只设置
             * CMD_STOP，解码线程处理它时不访问播放列表，因此 clear 与
             * handle_stop 并发安全；而 handle_open 只使用 player_open 在
             * UI 线程拷贝的 open_path，同样不触碰 g_playlist。 */
            player_stop();
            playlist_clear();
            /* 清空后 cur_index 指向已释放/不存在的条目，必须重置，否则
             * 之后"播放"按钮会拿着悬空索引去选曲。 */
            EnterCriticalSection(&g_player.cs);
            g_player.cur_index = -1;
            LeaveCriticalSection(&g_player.cs);
            ui_refresh_playlist();
            ui_update_state_controls();
            break;
        case IDM_FILE_EXIT:
            DestroyWindow(hwnd);
            break;
        case IDM_HELP_ABOUT:
            MessageBoxW(hwnd,
                L->d_about_content,
                L->d_about_title, MB_OK | MB_ICONINFORMATION);
            break;
        default:
            if (id >= IDM_LANG_FIRST && id < IDM_LANG_FIRST + LANG_COUNT) {
                int idx = (int)(id - IDM_LANG_FIRST);
                if (LANG_TABLE[idx].id != g_player.lang) {
                    g_player.lang = LANG_TABLE[idx].id;
                    /* Uncheck all, check the selected one. */
                    for (int j = 0; j < LANG_COUNT; j++)
                        CheckMenuItem(GetMenu(hwnd),
                                      IDM_LANG_FIRST + j,
                                      (j == idx) ? MF_CHECKED : MF_UNCHECKED);
                    ui_refresh_language();
                }
            }
            break;
        }
        return 0;
    }

    /* Trackbar notifications: progress seek + volume. */
    case WM_HSCROLL: {
        HWND hBar = (HWND)lParam;
        if (hBar == g_player.hProgress) {
            int code = LOWORD(wParam);
            /* 停止态（或无曲目）下不处理进度条：total_frames 为 0 时
             * 既无法换算出目标帧，也不应该把 UI 置于"拖动中"状态。 */
            int p_state, p_has;
            EnterCriticalSection(&g_player.cs);
            p_state = g_player.state;
            p_has   = (g_player.total_frames > 0);
            LeaveCriticalSection(&g_player.cs);
            if (p_state == STATE_STOPPED || !p_has)
                return 0;
            if (code == TB_THUMBTRACK) {
                g_player.seeking = 1;
                /* Live-update the time label while dragging. */
                int pos = (int)SendMessageW(g_player.hProgress, TBM_GETPOS, 0, 0);
                uint64_t total, rate;
                EnterCriticalSection(&g_player.cs);
                total = g_player.total_frames;
                rate  = g_player.sample_rate;
                LeaveCriticalSection(&g_player.cs);
                double s = (total > 0 && rate > 0)
                           ? (double)total * pos / 1000.0 / rate : 0;
                wchar_t t1[32], t2[32], lbl[80];
                ui_format_time(s, t1, 32);
                ui_format_time((rate > 0 ? (double)total / rate : 0), t2, 32);
                _snwprintf(lbl, 80, L"%s / %s", t1, t2);
                lbl[79] = 0;
                SetWindowTextW(g_player.hLblTime, lbl);
            } else if (code == TB_THUMBPOSITION || code == TB_ENDTRACK) {
                int pos = (int)SendMessageW(g_player.hProgress, TBM_GETPOS, 0, 0);
                uint64_t total;
                EnterCriticalSection(&g_player.cs);
                total = g_player.total_frames;
                LeaveCriticalSection(&g_player.cs);
                g_player.seeking = 0;
                if (total > 0)
                    player_seek(total * (uint64_t)pos / 1000);
            }
        } else if (hBar == g_player.hVolume) {
            int pos = (int)SendMessageW(g_player.hVolume, TBM_GETPOS, 0, 0);
            player_set_volume(pos);
            const LangStrings *L = ui_lang();
            wchar_t lbl[32];
            _snwprintf(lbl, 32, L->l_volume, pos);
            lbl[31] = 0;
            SetWindowTextW(g_player.hLblVol, lbl);
        }
        return 0;
    }

    /* Double-click / virtual-list data request. */
    case WM_NOTIFY: {
        LPNMHDR nmh = (LPNMHDR)lParam;
        if (nmh->code == NM_CUSTOMDRAW &&
            nmh->hwndFrom == g_player.hList) {
            /* The report view addresses its own custom-draw notification to
             * the parent window rather than to its own window procedure, so
             * it never reaches the list subclass on its own. Hand it over
             * there, where the row painting lives. */
            return list_subclass_proc(g_player.hList, WM_NOTIFY,
                                      wParam, lParam);
        }
        if (nmh->idFrom == IDC_LIST) {
            if (nmh->code == NM_DBLCLK) {
                int sel = ListView_GetNextItem(g_player.hList, -1, LVNI_SELECTED);
                if (sel >= 0) player_open(sel);
            }
            else if (nmh->code == LVN_GETDISPINFOW) {
                /* 虚拟列表：控件请求第 i 行 c 列的显示文本。
                 * 每列有独立 static 缓冲区，避免相邻请求互相覆盖。
                 * 返回的指针在本次调用期间有效；控件立即复制。 */
                NMLVDISPINFOW *pdi = (NMLVDISPINFOW *)nmh;
                int i = pdi->item.iItem;
                int c = pdi->item.iSubItem;
                if (i < 0 || i >= g_playlist.count) break;
                if (!(pdi->item.mask & LVIF_TEXT)) break;
                if (c < 0 || c > 6) break;

                /* Lazy metadata: rows are filled from the bounded LRU. If this
                 * row has no meta yet, request a background load so it fills in
                 * shortly (WM_TAGS_LOADED repaints just this row). */
                playlist_meta_request(i);

                PlaylistRowView v;
                static wchar_t name_b[7][256];   /* one name buf per column   */
                wchar_t *nb = name_b[c];
                playlist_row_view(i, &v, nb, 256);

                static wchar_t cell[7][64];  /* one buf per column */
                wchar_t *buf = cell[c];

                switch (c) {
                case 0:  /* 标题 — prefer title tag, fall back to filename. */
                    pdi->item.pszText = (wchar_t *)v.title;
                    break;
                case 1:  /* 艺术家 — show dash if missing. */
                    pdi->item.pszText = (wchar_t *)v.artist;
                    break;
                case 2:  /* 专辑 — show dash if missing. */
                    pdi->item.pszText = (wchar_t *)v.album;
                    break;
                case 3:  /* 时长 — format seconds to M:SS / H:MM:SS; show dash while untagged. */
                    if (v.duration > 0) {
                        ui_format_time(v.duration, buf, 64);
                        pdi->item.pszText = buf;
                    } else {
                        pdi->item.pszText = L"--:--";
                    }
                    break;
                case 4:  /* 比特率 — append "kbps" suffix; show dash if unknown. */
                    if (v.bitrate > 0) {
                        _snwprintf(buf, 64, L"%d kbps", v.bitrate);
                        buf[63] = 0;
                        pdi->item.pszText = buf;
                    } else {
                        pdi->item.pszText = L"-";
                    }
                    break;
                case 5:  /* 格式 — map internal format enum to display string. */
                    switch (v.format) {
                    case 1:  pdi->item.pszText = L"MP3";  break;
                    case 2:  pdi->item.pszText = L"FLAC"; break;
                    case 3:  pdi->item.pszText = L"WAV";  break;
                    case 4:  pdi->item.pszText = L"OGG";  break;
                    default: pdi->item.pszText = L"?";    break;
                    }
                    break;
                case 6:  /* 大小 — show KB (< 1 MB) or MB, auto-choice. */
                    if (v.file_size > 0) {
                        if (v.file_size < (uint64_t)1024 * 1024)
                            _snwprintf(buf, 64, L"%.1f KB",
                                       (double)v.file_size / 1024.0);
                        else
                            _snwprintf(buf, 64, L"%.1f MB",
                                       (double)v.file_size / 1048576.0);
                        buf[63] = 0;
                        pdi->item.pszText = buf;
                    } else {
                        pdi->item.pszText = L"-";
                    }
                    break;
                default:
                    pdi->item.pszText = L"";
                    break;
                }
            }
            else if (nmh->code == LVN_ODCACHEHINT) {
                /* The control is about to show this index range: prefetch its
                 * metadata so it is warm by the time those rows paint. */
                NMLVCACHEHINT *ch = (NMLVCACHEHINT *)nmh;
                int from = ch->iFrom, to = ch->iTo;
                if (from < 0) from = 0;
                if (to >= g_playlist.count) to = g_playlist.count - 1;
                for (int r = from; r <= to; r++)
                    playlist_meta_request(r);
            }
            else if (nmh->code == LVN_ODFINDITEM) {
                /* Type-to-select for a virtual list: return the first entry
                 * whose title starts with the typed prefix (case-insensitive),
                 * wrapping from iStart. Without this the list ignores keys. */
                NMLVFINDITEMW *fi = (NMLVFINDITEMW *)nmh;
                int found = -1;
                if ((fi->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) &&
                    fi->lvfi.psz && *fi->lvfi.psz && g_playlist.count > 0) {
                    const wchar_t *target = fi->lvfi.psz;
                    size_t tlen = wcslen(target);
                    int n = g_playlist.count;
                    int start = fi->iStart;
                    if (start < 0 || start >= n) start = 0;
                    wchar_t name[256];
                    for (int k = 0; k < n; k++) {
                        int idx = start + k;
                        if (idx >= n) idx -= n;          /* wrap around */
                        PlaylistRowView v;
                        playlist_row_view(idx, &v, name, 256);
                        if (_wcsnicmp(v.title, target, tlen) == 0) {
                            found = idx;
                            break;
                        }
                    }
                }
                return found;
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_TIMER:
        if (wParam == TIMER_ID_IMPORT) {
            /* Drive one bounded slice of the pending import. Refresh the list
             * count periodically (not every tick) so rows appear live without
             * paying a full repaint at 100 Hz. */
            int more = playlist_import_step(IMPORT_SLICE_MS);
            static ULONGLONG imp_last_refresh;
            ULONGLONG now = GetTickCount64();
            if (!more || now - imp_last_refresh >= 120) {
                imp_last_refresh = now;
                ui_refresh_playlist();
            }
            if (!more) {
                KillTimer(hwnd, TIMER_ID_IMPORT);
                const LangStrings *L = ui_lang();
                int added = playlist_import_added();
                if (added > 0) ui_set_status(L->s_added_files, added);
                else           ui_set_status(L->s_no_audio);
            }
            return 0;
        }
        if (wParam == TIMER_ID_POSITION)
            ui_update_position();
        return 0;

    /* ---- Worker-thread notifications ---- */
    case WM_TRACK_LOADED: {
        const LangStrings *L = ui_lang();
        /* A track's metadata (tags) finished loading in the background.
         * Do NOT rebuild the list (SetItemCountEx would lose scroll position).
         * Just refresh selection/focus and update the status text. */
        /* cur_index 由解码线程（handle_open 持锁）写入，此处持锁读取，
         * 避免无锁读的数据竞争。 */
        int cur;
        EnterCriticalSection(&g_player.cs);
        cur = g_player.cur_index;
        LeaveCriticalSection(&g_player.cs);

        ui_select_current(cur);
        ui_update_state_controls();
        ui_update_position();

        /* If the user stopped while this open was in flight, the decoder's
         * WM_TRACK_LOADED can land after the stop and would otherwise
         * overwrite the "stopped" status with a stale "playing: title".
         * Only show the title when the player is actually playing. */
        int st_now;
        EnterCriticalSection(&g_player.cs);
        st_now = g_player.state;
        LeaveCriticalSection(&g_player.cs);
        if (st_now != STATE_STOPPED &&
            cur >= 0 && cur < g_playlist.count) {
            /* 标题优先于文件名；标签缺失时回退到 name。 */
            PlaylistRowView v;
            wchar_t nb[256];
            playlist_row_view(cur, &v, nb, 256);
            ui_set_status(L->s_playing_title, v.title);
        }
        return 0;
    }

    case WM_TRACK_ENDED:
        /* Track finished playing; auto-advance or loop depending on mode.
         * wParam 携带该曲目的 track_gen：若期间用户已切到其他曲目，代数
         * 不匹配即为过期消息，直接丢弃，避免误跳歌。 */
        on_track_ended((LONG)wParam);
        return 0;

    case WM_TAGS_LOADED: {
        /* Background tag reader finished one file. Install the metadata into
         * the entry (copied into its packed EntryMeta block; the UI thread is
         * the only writer), then refresh that single row in the ListView.
         * If the epoch does not match (stale message from before a
         * remove/clear), or the entry is gone / already filled, the result is
         * discarded. install_meta copies the strings, so the TagInfo and its
         * heap strings are always ours to free. */
        int index = (int)wParam;
        TagInfo *ti = (TagInfo *)lParam;
        if (!ti) return 0;
        int installed = 0;
        if (ti->epoch == g_playlist.tag_epoch)
            installed = playlist_install_meta(index, ti);
        tag_info_free(ti);
        free(ti);
        if (installed)
            ListView_Update(g_player.hList, index);
        return 0;
    }

    case WM_PLAYER_ERROR: {
        const LangStrings *L = ui_lang();
        /* Not named `msg`: that is the window procedure's own message
         * parameter, and shadowing it made /W4 complain for no gain. */
        const wchar_t *text = NULL;
        switch ((int)wParam) {
        case PLAYER_ERR_INVALID_INDEX: text = L->d_err_index;  break;
        case PLAYER_ERR_OPEN_FAILED:   text = L->d_err_open;   break;
        case PLAYER_ERR_DEVICE_OPEN:   text = L->d_err_device; break;
        case PLAYER_ERR_DEVICE_WRITE:  text = L->d_err_output; break;
        default:                       text = (const wchar_t *)lParam; break;
        }
        MessageBoxW(hwnd, text, L->d_err_title, MB_OK | MB_ICONWARNING);
        ui_update_state_controls();
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID_POSITION);
        /* Release every GDI object this file owns: the four DPI fonts plus
         * the empty-state icon face, the shape caches, and the row-height
         * image list (the ListView never took ownership of it). The process
         * is exiting anyway, but leaving these to the OS made real leak
         * audits noisy - anything still alive at WM_DESTROY is on us. */
        if (g_player.hFont)     { DeleteObject(g_player.hFont);     g_player.hFont = NULL; }
        if (g_player.hFontBold) { DeleteObject(g_player.hFontBold); g_player.hFontBold = NULL; }
        if (g_player.hFontSm)   { DeleteObject(g_player.hFontSm);   g_player.hFontSm = NULL; }
        if (g_player.hFontIcon) { DeleteObject(g_player.hFontIcon); g_player.hFontIcon = NULL; }
        if (g_hFontEmpty)       { DeleteObject(g_hFontEmpty);       g_hFontEmpty = NULL; }
        if (g_hRowIL)           { ImageList_Destroy(g_hRowIL);      g_hRowIL = NULL; }
        rounded_flush();
        PostQuitMessage(0);
        return 0;

    default:
        /* Unhandled messages go to the default window procedure. */
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;  /* not reached – suppresses MSVC C4715 */
}

/* ---- Track-ended handling -------------------------------------------- */
static void on_track_ended(LONG gen)
{
    int mode, cur, state;
    LONG tgen;
    EnterCriticalSection(&g_player.cs);
    mode  = g_player.play_mode;
    cur   = g_player.cur_index;
    state = g_player.state;
    tgen  = g_player.track_gen;
    LeaveCriticalSection(&g_player.cs);

    /* 过期消息：曲目已切换（代数不匹配），丢弃，避免对当前曲目误跳歌。 */
    if (gen != tgen) return;

    /* If the user stopped, don't auto-advance. */
    if (state == STATE_STOPPED) return;

    if (mode == MODE_SINGLE_LOOP) {
        player_seek(0);          /* restart the same track */
    } else {
        int next = playlist_next_index(cur, mode);
        if (next >= 0) player_open(next);
    }
}

/* ---- Language refresh ------------------------------------------------ */
void ui_refresh_language(void)
{
    const LangStrings *L = ui_lang();
    HWND hMain = g_player.hMain;
    HMENU hMenu = GetMenu(hMain);

    /* Update menu items.
     * 关键：顶级三项是 MF_POPUP（各自挂子菜单）。ModifyMenu 若不带 MF_POPUP
     * 会把该项改成普通字符串项并销毁其子菜单（微软文档明确该行为），导致
     * 切换语言一次后文件/语言/帮助菜单全部失效。因此必须保留 MF_POPUP 并
     * 把取到的子菜单句柄回传。 */
    HMENU hFile = GetSubMenu(hMenu, 0);
    HMENU hLangMenu = GetSubMenu(hMenu, 1);
    HMENU hHelp = GetSubMenu(hMenu, 2);
    ModifyMenuW(hMenu, 0, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hFile, L->m_file);
    ModifyMenuW(hMenu, 1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hLangMenu, L->m_lang);
    ModifyMenuW(hMenu, 2, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hHelp, L->m_help);
    if (hFile) {
        ModifyMenuW(hFile, IDM_FILE_ADD_FILES, MF_BYCOMMAND | MF_STRING,
                    IDM_FILE_ADD_FILES, L->m_add_files);
        ModifyMenuW(hFile, IDM_FILE_ADD_FOLDER, MF_BYCOMMAND | MF_STRING,
                    IDM_FILE_ADD_FOLDER, L->m_add_folder);
        ModifyMenuW(hFile, IDM_FILE_LOAD_M3U, MF_BYCOMMAND | MF_STRING,
                    IDM_FILE_LOAD_M3U, L->m_load_m3u);
        ModifyMenuW(hFile, IDM_FILE_SAVE_M3U, MF_BYCOMMAND | MF_STRING,
                    IDM_FILE_SAVE_M3U, L->m_save_m3u);
        ModifyMenuW(hFile, IDM_FILE_CLEAR, MF_BYCOMMAND | MF_STRING,
                    IDM_FILE_CLEAR, L->m_clear);
        ModifyMenuW(hFile, IDM_FILE_EXIT, MF_BYCOMMAND | MF_STRING,
                    IDM_FILE_EXIT, L->m_exit);
    }
    if (hLangMenu) {
        /* Update checkmarks. */
        for (int j = 0; j < LANG_COUNT; j++)
            CheckMenuItem(hLangMenu, IDM_LANG_FIRST + j,
                          (LANG_TABLE[j].id == g_player.lang)
                          ? MF_CHECKED : MF_UNCHECKED);
    }
    if (hHelp) {
        ModifyMenuW(hHelp, IDM_HELP_ABOUT, MF_BYCOMMAND | MF_STRING,
                    IDM_HELP_ABOUT, L->m_about);
    }

    /* Force immediate repaint of the menu bar. */
    DrawMenuBar(hMain);

    /* Update window title. */
    SetWindowTextW(hMain, L->d_window_title);

    /* Update buttons. */
    SetWindowTextW(g_player.hBtnPrev, L->b_prev);
    {
        int state, mode;
        EnterCriticalSection(&g_player.cs);
        state = g_player.state;
        mode  = g_player.play_mode;
        LeaveCriticalSection(&g_player.cs);
        SetWindowTextW(g_player.hBtnPlay,
                       state == STATE_PLAYING ? L->b_pause : L->b_play);
        SetWindowTextW(g_player.hBtnMode, mode_label(mode));
    }
    SetWindowTextW(g_player.hBtnStop, L->b_stop);
    SetWindowTextW(g_player.hBtnNext, L->b_next);

    /* Update status. */
    ui_update_state_controls();

    /* Update volume label. */
    {
        int vol = g_player.volume;
        wchar_t lbl[32];
        _snwprintf(lbl, 32, L->l_volume, vol);
        lbl[31] = 0;
        SetWindowTextW(g_player.hLblVol, lbl);
    }

    /* Update ListView column headers to match the new language.
     * Unlike the creation path (which inserts in reverse), here we
     * update columns by index directly, so forward iteration is fine. */
    {
        HWND hHeader = ListView_GetHeader(g_player.hList);
        if (hHeader) {
            const wchar_t *col_names[] = {
                L->c_title, L->c_artist, L->c_album, L->c_duration,
                L->c_bitrate, L->c_format, L->c_size
            };
            for (int i = 0; i < 7; i++) {
                LVCOLUMNW col;
                memset(&col, 0, sizeof(col));
                col.mask = LVCF_TEXT;
                col.pszText = (LPWSTR)col_names[i];
                ListView_SetColumn(g_player.hList, i, &col);
            }
        }
    }
}

/* ---- Window creation -------------------------------------------------- */
HWND ui_create_main(HINSTANCE hInst, int nCmdShow)
{
    const LangStrings *L_init = ui_lang();
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = 0;
    wc.lpfnWndProc   = main_wndproc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;   /* painted explicitly in WM_ERASEBKGND */
    wc.lpszClassName = MAIN_CLASS_NAME;
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L_init->d_reg_failed, L_init->d_err_title, MB_ICONERROR);
        return NULL;
    }

    /* Menu bar. */
    HMENU hMenu  = CreateMenu();
    HMENU hFile  = CreatePopupMenu();
    AppendMenuW(hFile, MF_STRING, IDM_FILE_ADD_FILES,  L_init->m_add_files);
    AppendMenuW(hFile, MF_STRING, IDM_FILE_ADD_FOLDER, L_init->m_add_folder);
    AppendMenuW(hFile, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hFile, MF_STRING, IDM_FILE_LOAD_M3U,   L_init->m_load_m3u);
    AppendMenuW(hFile, MF_STRING, IDM_FILE_SAVE_M3U,   L_init->m_save_m3u);
    AppendMenuW(hFile, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hFile, MF_STRING, IDM_FILE_CLEAR,      L_init->m_clear);
    AppendMenuW(hFile, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hFile, MF_STRING, IDM_FILE_EXIT,       L_init->m_exit);
    HMENU hLang = CreatePopupMenu();
    for (int i = 0; i < LANG_COUNT; i++) {
        UINT flags = MF_STRING;
        if (LANG_TABLE[i].id == g_player.lang) flags |= MF_CHECKED;
        AppendMenuW(hLang, flags, IDM_LANG_FIRST + i, LANG_TABLE[i].name);
    }
    HMENU hHelp  = CreatePopupMenu();
    AppendMenuW(hHelp, MF_STRING, IDM_HELP_ABOUT,      L_init->m_about);
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFile, L_init->m_file);
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hLang, L_init->m_lang);
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hHelp, L_init->m_help);

    /* The initial size must reflect the monitor's real DPI. A raw screen DC
     * reports the *system* DPI (96) for a Per-Monitor-V2 process, and
     * GetDpiForWindow can still answer 96 before the window has been shown,
     * so both would create the window far too small and then leave every
     * child undersized once WM_CREATE switched to the per-monitor value.
     * Ask the target monitor directly instead. */
    g_player.dpi = ui_dpi_for_window_or_monitor(NULL);

    HWND hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES, MAIN_CLASS_NAME, L_init->d_window_title,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT, DPIX(780), DPIX(540),
        NULL, hMenu, hInst, NULL);

    if (!hwnd) {
        MessageBoxW(NULL, L_init->d_create_failed, L_init->d_err_title, MB_ICONERROR);
        DestroyMenu(hMenu);
        return NULL;
    }

    ShowWindow(hwnd, nCmdShow);

    /* Now that the window is on its monitor, re-read the DPI from the
     * window itself and force the designed client size. */
    {
        int dpi = ui_get_dpi(hwnd);
        if (dpi > 0) g_player.dpi = dpi;
    }
    ui_set_font_for_dpi(g_player.dpi);
    {
        RECT want = { 0, 0, DPIX(780), DPIX(540) };
        AdjustWindowRectEx(&want, WS_OVERLAPPEDWINDOW, TRUE,
                           WS_EX_ACCEPTFILES);
        RECT cur;
        GetWindowRect(hwnd, &cur);
        SetWindowPos(hwnd, NULL, cur.left, cur.top,
                     want.right - want.left, want.bottom - want.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ui_layout();


    UpdateWindow(hwnd);
    return hwnd;
}
