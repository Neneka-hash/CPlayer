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
#include <string.h>

#define MAIN_CLASS_NAME L"CPlayerMainWindowClass"

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
    /* Dialogs */
    const wchar_t *d_about_title, *d_about_content;
    const wchar_t *d_window_title;
    const wchar_t *d_empty_playlist, *d_save_failed;
    const wchar_t *d_reg_failed, *d_create_failed;
    const wchar_t *d_alloc_buf, *d_start_thread;
    const wchar_t *d_err_title, *d_info_title;
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
        if (p) return (int)p(hwnd);
    }
    HDC dc = GetDC(NULL);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return dpi ? dpi : 96;
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
void ui_set_font_for_dpi(int dpi)
{
    if (g_player.hFont) DeleteObject(g_player.hFont);
    int h = -MulDiv(9, dpi, 72);          /* 9 pt at this DPI */
    g_player.hFont = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!g_player.hFont) {
        g_player.hFont = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, L"MS Shell Dlg");
    }
    HWND ctrls[] = { g_player.hList, g_player.hProgress, g_player.hVolume,
                     g_player.hBtnPrev, g_player.hBtnPlay, g_player.hBtnStop,
                     g_player.hBtnNext, g_player.hBtnMode,
                     g_player.hLblTime, g_player.hLblVol, g_player.hLblStatus };
    for (int i = 0; i < (int)(sizeof(ctrls)/sizeof(ctrls[0])); i++) {
        if (ctrls[i])
            SendMessageW(ctrls[i], WM_SETFONT, (WPARAM)g_player.hFont, TRUE);
    }
}

/* ---- Layout ----------------------------------------------------------- */
void ui_layout(void)
{
    RECT rc;
    GetClientRect(g_player.hMain, &rc);
    int W = rc.right;
    int pad = DPIX(8);
    int btnH = DPIX(30);
    int btnW = DPIX(80);
    int sldH = DPIX(26);
    int lblH = DPIX(20);
    int rowH = DPIX(34);

    int y = pad;

    /* ListView occupies everything between the top and the button row.
     * MoveWindow 必须传 TRUE（立即重绘）：CS_HREDRAW|CS_VREDRAW 会让系统
     * 在窗口尺寸变化时擦整个客户区背景，若 ListView 不立即重绘到新位置，
     * 旧内容会残留在新客户区外（溢出到按钮/进度条区），造成视觉错位。 */
    int listBottom = rc.bottom - rowH * 3 - pad * 2;
    if (listBottom < pad + DPIX(60)) listBottom = pad + DPIX(60);
    if (g_player.hList)
        MoveWindow(g_player.hList, pad, y, W - pad * 2, listBottom - y, TRUE);
    y = listBottom + pad;

    /* Button row: four standard buttons + one wider mode button.
     * The mode button holds "模式: 列表循环" / "模式: 单曲循环" /
     * "模式: 随机播放" - far longer than "上一首"/"播放" etc., so it gets
     * its own wider metric. */
    int x = pad;
    int modeW = DPIX(120);   /* fits the longest mode label with margin */
    HWND stdBtns[] = { g_player.hBtnPrev, g_player.hBtnPlay,
                       g_player.hBtnStop,  g_player.hBtnNext };
    for (int i = 0; i < 4; i++) {
        if (stdBtns[i])
            MoveWindow(stdBtns[i], x, y, btnW, btnH, TRUE);
        x += btnW + pad;
    }
    if (g_player.hBtnMode)
        MoveWindow(g_player.hBtnMode, x, y, modeW, btnH, TRUE);
    x += modeW + pad;

    /* Status label fills the rest of the button row; keep a minimum width so
     * "正在播放" / "已暂停" are never clipped even on a narrow window. */
    if (g_player.hLblStatus) {
        int statusW = W - pad - x;
        if (statusW < DPIX(64)) statusW = DPIX(64);
        MoveWindow(g_player.hLblStatus, x, y + (btnH - lblH) / 2,
                   statusW, lblH, TRUE);
    }
    y += rowH;

    /* Progress slider + time label. */
    int labelW = DPIX(110);
    if (g_player.hProgress)
        MoveWindow(g_player.hProgress, pad, y, W - pad * 2 - labelW, sldH, TRUE);
    if (g_player.hLblTime)
        MoveWindow(g_player.hLblTime, W - pad - labelW,
                   y + (sldH - lblH) / 2, labelW, lblH, TRUE);
    y += rowH;

    /* Volume row: slider + label — 与进度行完全对齐。 */
    if (g_player.hVolume)
        MoveWindow(g_player.hVolume, pad, y, W - pad * 2 - labelW, sldH, TRUE);
    if (g_player.hLblVol)
        MoveWindow(g_player.hLblVol, W - pad - labelW,
                   y + (sldH - lblH) / 2, labelW, lblH, TRUE);
}

/* ---- Playlist ListView ------------------------------------------------ */

void ui_refresh_playlist(void)
{
    if (!g_player.hList) return;
    /* 虚拟列表模式：只需告知总数，控件自动对可见行请求 LVN_GETDISPINFO。 */
    ListView_SetItemCountEx(g_player.hList, g_playlist.count, LVSICF_NOSCROLL);

    ui_select_current(g_player.cur_index);
}

void ui_select_current(int index)
{
    if (!g_player.hList) return;
    /* -1 批量清除所有项的选中/焦点状态（无需逐项循环）。 */
    ListView_SetItemState(g_player.hList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (index >= 0 && index < g_playlist.count) {
        ListView_SetItemState(g_player.hList, index,
                              LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_player.hList, index, FALSE);
    }
}

/* ---- Position / state UI --------------------------------------------- */
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

    if (!g_player.seeking && total > 0) {
        int pos = (int)(cur * 1000 / total);
        if (pos < 0) pos = 0;
        if (pos > 1000) pos = 1000;
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
    SetWindowTextW(g_player.hLblTime, lbl);
    (void)state;
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
        /* Single file - buf is the full path. */
        playlist_add_file(buf);
    } else {
        wchar_t full[MAX_PATH];
        while (*p) {
            _snwprintf(full, MAX_PATH, L"%s\\%s", dir, p);
            full[MAX_PATH - 1] = 0;
            playlist_add_file_fast(full);
            p += wcslen(p) + 1;
        }
    }
    ui_refresh_playlist();
    playlist_start_tag_thread();
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
        int added = playlist_add_folder(path);
        ui_refresh_playlist();
        playlist_start_tag_thread();
        ui_set_status(L->s_added_files, added);
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

    int added = playlist_load_m3u(buf);
    ui_refresh_playlist();
    playlist_start_tag_thread();
    if (added > 0)
        ui_set_status(L->s_loaded_m3u, added);
    else
        ui_set_status(L->s_no_audio_m3u);
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
static HWND    g_hHeader        = NULL;  /* ListView header (white-bg custom draw) */

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

    if (deleting_current && state != STATE_STOPPED)
        player_stop();

    /* Remove highest index first so earlier indices stay valid. */
    for (int i = nsel - 1; i >= 0; i--)
        playlist_remove_at(sel[i]);

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

    free(sel);
    ui_refresh_playlist();
    ui_update_state_controls();
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

    if (msg == WM_KEYDOWN) {
        if (wParam == VK_DELETE) {
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
        /* ListView header custom draw: white background + black text,
         * overriding the themed gradient. */
        LPNMHDR nm = (LPNMHDR)lParam;
        if (nm->code == NM_CUSTOMDRAW && g_hHeader && nm->hwndFrom == g_hHeader) {
            LPNMCUSTOMDRAW cd = (LPNMCUSTOMDRAW)nm;
            if (cd->dwDrawStage == CDDS_PREPAINT) {
                FillRect(cd->hdc, &cd->rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
                return CDRF_NOTIFYITEMDRAW;
            }
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                FillRect(cd->hdc, &cd->rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
                HBRUSH eb = CreateSolidBrush(RGB(200,200,200));
                FrameRect(cd->hdc, &cd->rc, eb);
                DeleteObject(eb);
                HDITEMW hi; wchar_t text[64];
                memset(&hi, 0, sizeof(hi));
                hi.mask = HDI_TEXT;
                hi.pszText = text;
                hi.cchTextMax = 64;
                Header_GetItem(g_hHeader, cd->dwItemSpec, &hi);
                SetBkMode(cd->hdc, TRANSPARENT);
                SetTextColor(cd->hdc, RGB(0,0,0));
                HFONT oldF = (HFONT)SelectObject(cd->hdc, g_player.hFont);
                RECT tr = cd->rc;
                tr.left += DPIX(6);
                DrawTextW(cd->hdc, text ? text : L"", -1, &tr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                SelectObject(cd->hdc, oldF);
                return CDRF_SKIPDEFAULT;
            }
            return CDRF_DODEFAULT;
        }
    }
    return CallWindowProcW(g_list_orig_proc, hwnd, msg, wParam, lParam);
}

/* ---- Progress bar click-to-seek --------------------------------------- */
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
    if (msg == WM_LBUTTONDOWN) {
        int x = (int)(short)LOWORD(lParam);
        int y = (int)(short)HIWORD(lParam);
        RECT thumb;
        SendMessageW(hwnd, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
        /* 点中滑块本体：交给原逻辑，用户可继续拖动。 */
        if (x >= thumb.left && x <= thumb.right &&
            y >= thumb.top  && y <= thumb.bottom)
            return CallWindowProcW(g_progress_orig_proc, hwnd, msg, wParam, lParam);
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
        /* 复用父窗口已有的 seek 逻辑（WM_HSCROLL 的 TB_THUMBPOSITION 分支）。 */
        SendMessageW(GetParent(hwnd), WM_HSCROLL,
                     MAKELONG(TB_THUMBPOSITION, 0), (LPARAM)hwnd);
        return 0;
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
    if (msg == WM_LBUTTONDOWN) {
        int x = (int)(short)LOWORD(lParam);
        int y = (int)(short)HIWORD(lParam);
        RECT thumb;
        SendMessageW(hwnd, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
        if (x >= thumb.left && x <= thumb.right &&
            y >= thumb.top  && y <= thumb.bottom)
            return CallWindowProcW(g_volume_orig_proc, hwnd, msg, wParam, lParam);
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
        SendMessageW(GetParent(hwnd), WM_HSCROLL,
                     MAKELONG(TB_THUMBPOSITION, 0), (LPARAM)hwnd);
        return 0;
    }
    return CallWindowProcW(g_volume_orig_proc, hwnd, msg, wParam, lParam);
}

/* ---- Forward declaration --------------------------------------------- */
static void on_track_ended(void);

/* ---- Main window procedure ------------------------------------------- */
LRESULT CALLBACK main_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    case WM_CREATE: {
        g_player.hMain = hwnd;
        g_player.dpi   = ui_get_dpi(hwnd);

        /* ListView (virtual mode: LVS_OWNERDATA — 控件不存储项数据，
         * 只对可见行请求 LVN_GETDISPINFO，大量项时性能恒定)。 */
        g_player.hList = CreateWindowExW(WS_EX_CLIENTEDGE | WS_EX_ACCEPTFILES,
            WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
            0, 0, 0, 0, hwnd, (HMENU)IDC_LIST, NULL, NULL);
        ListView_SetExtendedListViewStyle(g_player.hList,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        /* ListView 必须显式设色：Mica 透明窗口下系统默认色不可靠，
         * 不设色会显示黑色背景。 */
        ListView_SetBkColor(g_player.hList, RGB(255,255,255));
        ListView_SetTextBkColor(g_player.hList, RGB(255,255,255));
        ListView_SetTextColor(g_player.hList, RGB(0,0,0));

        /* Seven columns: 标题 / 艺术家 / 专辑 / 时长 / 比特率 / 格式 / 大小.
         * Widths are 96dpi pixels; DPIX() scales them per-monitor. The user
         * can resize columns at runtime via the header dividers.
         * 'name' is a placeholder — the actual header text is set from
         * col_names below. */
        struct { const wchar_t *name; int w; } cols[] = {
            { L"", 280 },
            { L"", 140 },
            { L"", 140 },
            { L"", 60  },
            { L"", 70  },
            { L"", 70  },
            { L"", 80  },
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
         * building from the last column forward yields the correct order. */
        for (int i = 6; i >= 0; i--) {
            LVCOLUMNW col;
            memset(&col, 0, sizeof(col));
            col.mask    = LVCF_TEXT | LVCF_WIDTH;
            col.cx       = DPIX(cols[i].w);
            col.pszText  = (LPWSTR)col_names[i];
            ListView_InsertColumn(g_player.hList, 0, &col);
        }

        /* Subclass the list so Ctrl+A (select all) and Del (delete every
         * selected row) are handled by the control that owns keyboard focus;
         * the main window never sees these keys. */
        g_list_orig_proc = (WNDPROC)SetWindowLongPtrW(g_player.hList,
                                GWLP_WNDPROC, (LONG_PTR)list_subclass_proc);

        /* ListView header: grab HWND for white-background custom draw. */
        g_hHeader = ListView_GetHeader(g_player.hList);

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

        /* Labels. */
        g_player.hLblTime   = CreateWindowExW(0, L"static", L"0:00 / 0:00",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 0,0,0,0, hwnd, (HMENU)IDC_LBL_TIME, NULL, NULL);
        g_player.hLblVol    = CreateWindowExW(0, L"static", L"",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 0,0,0,0, hwnd, (HMENU)IDC_LBL_VOL, NULL, NULL);
        g_player.hLblStatus = CreateWindowExW(0, L"static", L_init->s_ready,
            WS_CHILD | WS_VISIBLE | SS_LEFT, 0,0,0,0, hwnd, (HMENU)IDC_LBL_STATUS, NULL, NULL);

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
        /* 静态控件 + trackbar 背景刷：显式白底黑字。
         * Mica 透明窗口下若不提供背景刷，系统返回 NULL/黑色，
         * trackbar 进度条背景会变成纯黑——这是 Win32+Mica 的固有行为，
         * 必须显式提供白色刷子。 */
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(0,0,0));
        SetBkColor(hdc, RGB(255,255,255));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }

    case WM_PAINT: {
        /* 父窗口客户区无自绘内容（全部由子控件绘制）。
         * BeginPaint/EndPaint 配对验证 dirty 区域，背景由 hbrBackground
         * 自动填充。 */
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DRAWITEM: {
        /* 自绘按钮：白底 + 细灰边框 + 黑字（Win11 扁平风格，无阴影）。 */
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
        if (dis->CtlType != ODT_BUTTON)
            break;
        RECT rc = dis->rcItem;
        /* 背景：按下时微暗，否则白色。 */
        COLORREF bg = (dis->itemState & ODS_SELECTED) ? RGB(229,229,229)
                                                      : RGB(255,255,255);
        HBRUSH br = CreateSolidBrush(bg);
        FillRect(dis->hDC, &rc, br);
        DeleteObject(br);
        /* 1px 灰色边框。 */
        HBRUSH eb = CreateSolidBrush(RGB(173,173,173));
        FrameRect(dis->hDC, &rc, eb);
        DeleteObject(eb);
        /* 文字：黑色、居中、透明背景；按下时偏移 1px。 */
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, RGB(0,0,0));
        if (dis->itemState & ODS_DISABLED)
            SetTextColor(dis->hDC, RGB(160,160,160));
        wchar_t text[64];
        int n = GetWindowTextW(dis->hwndItem, text, 64);
        HFONT oldFont = (HFONT)SelectObject(dis->hDC, g_player.hFont);
        RECT tr = rc;
        if (dis->itemState & ODS_SELECTED) { tr.left += 1; tr.top += 1; }
        DrawTextW(dis->hDC, text, n, &tr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dis->hDC, oldFont);
        /* 焦点虚线框。 */
        if (dis->itemState & ODS_FOCUS) {
            RECT fr = rc;
            InflateRect(&fr, -3, -3);
            DrawFocusRect(dis->hDC, &fr);
        }
        return TRUE;
    }

    case WM_DPICHANGED: {
        g_player.dpi = LOWORD(wParam);
        LPRECT pr = (LPRECT)lParam;
        SetWindowPos(hwnd, NULL, pr->left, pr->top,
                     pr->right - pr->left, pr->bottom - pr->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ui_set_font_for_dpi(g_player.dpi);
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
        /* 立即布局：标准 Win32 控件的 MoveWindow 很轻量，拖拽边缘时每秒
         * 60+ 次 WM_SIZE 完全无压力。早期版本曾用 16ms 定时器节流，但
         * 那是 D2D 自绘列表时代的教训（已删除）；现在不延迟反而更稳——
         * 延迟期间系统擦背景（CS_HREDRAW|CS_VREDRAW）会让 ListView 旧内容
         * 溢出到按钮/进度条区，最大化恢复和快速拉伸时尤其明显。 */
        if (wParam != SIZE_MINIMIZED)
            ui_layout();
        return 0;

    /* Drag & drop: files are added directly, folders are recursed into. */
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        UINT n = DragQueryFileW(hDrop, 0xFFFFFFFFU, NULL, 0);
        int added = 0;
        const LangStrings *L = ui_lang();
        for (UINT i = 0; i < n; i++) {
            UINT len = DragQueryFileW(hDrop, i, NULL, 0);
            wchar_t *path = (wchar_t *)malloc((size_t)(len + 1) * sizeof(wchar_t));
            if (!path) continue;
            DragQueryFileW(hDrop, i, path, len + 1);
            DWORD attr = GetFileAttributesW(path);
            if (attr != INVALID_FILE_ATTRIBUTES &&
                (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                added += playlist_add_folder(path);
            } else {
                if (playlist_add_file_fast(path) >= 0) added++;
            }
            free(path);
        }
        DragFinish(hDrop);
        ui_refresh_playlist();
        playlist_start_tag_thread();
        ui_select_current(g_player.cur_index);
        if (added > 0)
            ui_set_status(L->s_dropped_files, added);
        else
            ui_set_status(L->s_no_audio);
        return 0;
    }

    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        const LangStrings *L = ui_lang();
        switch (id) {
        case IDC_BTN_PLAY:
            if (g_player.state == STATE_STOPPED && g_playlist.count > 0) {
                /* Start playing the selected item (or current, or first). */
                int sel = ListView_GetNextItem(g_player.hList, -1, LVNI_SELECTED);
                if (sel < 0) sel = g_player.cur_index;
                if (sel < 0) sel = 0;
                player_open(sel);
            } else {
                player_toggle_pause();
            }
            ui_update_state_controls();
            break;
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
            playlist_clear();
            player_stop();
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
                PlaylistEntry *e = &g_playlist.items[i];

                static wchar_t cell[7][64];  /* one buf per column */
                wchar_t *buf = cell[c];

                switch (c) {
                case 0:  /* 标题 — prefer title tag, fall back to filename. */
                    pdi->item.pszText = (e->title ? e->title :
                                         (e->name ? e->name : L""));
                    break;
                case 1:  /* 艺术家 — show dash if missing. */
                    pdi->item.pszText = e->artist ? e->artist : L"-";
                    break;
                case 2:  /* 专辑 — show dash if missing. */
                    pdi->item.pszText = e->album ? e->album : L"-";
                    break;
                case 3:  /* 时长 — format seconds to M:SS / H:MM:SS; show dash while untagged. */
                    if (e->duration > 0) {
                        ui_format_time(e->duration, buf, 64);
                        pdi->item.pszText = buf;
                    } else {
                        pdi->item.pszText = L"--:--";
                    }
                    break;
                case 4:  /* 比特率 — append "kbps" suffix; show dash if unknown. */
                    if (e->bitrate > 0) {
                        _snwprintf(buf, 64, L"%d kbps", e->bitrate);
                        buf[63] = 0;
                        pdi->item.pszText = buf;
                    } else {
                        pdi->item.pszText = L"-";
                    }
                    break;
                case 5:  /* 格式 — map internal format enum to display string. */
                    switch (e->format) {
                    case 1:  pdi->item.pszText = L"MP3";  break;
                    case 2:  pdi->item.pszText = L"FLAC"; break;
                    case 3:  pdi->item.pszText = L"WAV";  break;
                    case 4:  pdi->item.pszText = L"OGG";  break;
                    default: pdi->item.pszText = L"?";    break;
                    }
                    break;
                case 6:  /* 大小 — show KB (< 1 MB) or MB, auto-choice. */
                    if (e->file_size > 0) {
                        if (e->file_size < (uint64_t)1024 * 1024)
                            _snwprintf(buf, 64, L"%.1f KB",
                                       (double)e->file_size / 1024.0);
                        else
                            _snwprintf(buf, 64, L"%.1f MB",
                                       (double)e->file_size / 1048576.0);
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
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_TIMER:
        if (wParam == TIMER_ID_POSITION)
            ui_update_position();
        return 0;

    /* ---- Worker-thread notifications ---- */
    case WM_TRACK_LOADED: {
        const LangStrings *L = ui_lang();
        /* A track's metadata (tags) finished loading in the background.
         * Do NOT rebuild the list (SetItemCountEx would lose scroll position).
         * Just refresh selection/focus and update the status text. */
        ui_select_current(g_player.cur_index);
        ui_update_state_controls();
        ui_update_position();

        if (g_player.cur_index >= 0 && g_player.cur_index < g_playlist.count) {
            /* 标题优先于文件名；标签缺失时回退到 name。 */
            const PlaylistEntry *e = &g_playlist.items[g_player.cur_index];
            const wchar_t *title = e->title ? e->title : (e->name ? e->name : L"");
            ui_set_status(L->s_playing_title, title);
        }
        return 0;
    }

    case WM_TRACK_ENDED:
        /* Track finished playing; auto-advance or loop depending on mode. */
        on_track_ended();
        return 0;

    case WM_TAGS_LOADED: {
        /* Background tag reader finished one file. Transfer the heap
         * TagInfo into the PlaylistEntry (UI thread = only writer),
         * then refresh that single row in the ListView.
         * If the index is out of range (list was cleared or trimmed)
         * or the tag was already loaded (synchronous fill_tag raced),
         * or the epoch does not match (stale message from before a
         * remove/clear), free the TagInfo strings and discard. */
        int index = (int)wParam;
        TagInfo *ti = (TagInfo *)lParam;
        if (index >= 0 && index < g_playlist.count &&
            !g_playlist.items[index].tag_loaded &&
            ti->epoch == g_playlist.tag_epoch) {
            PlaylistEntry *e = &g_playlist.items[index];
            e->title       = ti->title;
            e->artist      = ti->artist;
            e->album       = ti->album;
            e->duration    = ti->duration;
            e->bitrate     = ti->bitrate;
            e->sample_rate = ti->sample_rate;
            e->channels    = ti->channels;
            if (e->file_size == 0)
                e->file_size = ti->file_size;
            e->tag_loaded  = 1;
            ListView_Update(g_player.hList, index);
        } else {
            tag_info_free(ti);
        }
        free(ti);
        return 0;
    }

    case WM_PLAYER_ERROR:
        MessageBoxW(hwnd, (const wchar_t *)lParam, L"播放器错误",
                    MB_OK | MB_ICONWARNING);
        ui_update_state_controls();
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID_POSITION);
        /* 释放 DPI 字体（每次 WM_DPICHANGED 都会重建一个新字体，旧的已
         * 在 ui_set_font_for_dpi 里 DeleteObject；这里只释放当前字体）。 */
        if (g_player.hFont) {
            DeleteObject(g_player.hFont);
            g_player.hFont = NULL;
        }
        PostQuitMessage(0);
        return 0;

    default:
        /* Unhandled messages go to the default window procedure. */
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;  /* not reached – suppresses MSVC C4715 */
}

/* ---- Track-ended handling -------------------------------------------- */
static void on_track_ended(void)
{
    int mode, cur, state;
    EnterCriticalSection(&g_player.cs);
    mode  = g_player.play_mode;
    cur   = g_player.cur_index;
    state = g_player.state;
    LeaveCriticalSection(&g_player.cs);

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

    /* Update menu items. */
    ModifyMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, 0, L->m_file);
    ModifyMenuW(hMenu, 1, MF_BYPOSITION | MF_STRING, 1, L->m_lang);
    ModifyMenuW(hMenu, 2, MF_BYPOSITION | MF_STRING, 2, L->m_help);
    HMENU hFile = GetSubMenu(hMenu, 0);
    HMENU hLangMenu = GetSubMenu(hMenu, 1);
    HMENU hHelp = GetSubMenu(hMenu, 2);
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
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = main_wndproc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
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

    /* g_player.dpi is still 0 here (global zero-init), but DPIX() depends on
     * it. Query the system DPI now so the initial window is sized correctly;
     * WM_CREATE / WM_DPICHANGED refine it for the actual window later.
     * GetDpiForWindow(NULL) is unreliable on NULL, so use the screen DC. */
    HDC dc = GetDC(NULL);
    if (dc) {
        g_player.dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
    }
    if (g_player.dpi <= 0) g_player.dpi = 96;

    int W = DPIX(600);
    int H = DPIX(440);

    HWND hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES, MAIN_CLASS_NAME, L_init->d_window_title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, W, H,
        NULL, hMenu, hInst, NULL);

    if (!hwnd) {
        MessageBoxW(NULL, L_init->d_create_failed, L_init->d_err_title, MB_ICONERROR);
        return NULL;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    return hwnd;
}
