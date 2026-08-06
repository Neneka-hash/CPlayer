/*
 * audio_waveout.c
 *
 * Windows waveOut 音频输出实现。管理 PCM 缓冲池、waveOut 设备以及 waveOut
 * 工作线程，同时提供 UI 层调用的播放控制 API。
 *
 * waveOut 线程是管线的消费者端：将已填充的数据块提交给设备，回收播放完成的
 * 数据块并通过信号通知解码器有空闲槽位。它还负责 flush 握手（参见 audio_decode.c）
 * 以及在解码器 EOF 且所有缓冲块播放完毕时发送 WM_TRACK_ENDED 消息。
 *
 * 音量通过 waveOutSetVolume 实现（0..100 映射为每通道 0..0xFFFF），
 * 不依赖采样率且不消耗 CPU。
 */
#include "mp_player.h"
#include "audio_waveout.h"
#include "audio_decode.h"   /* decoder_close for shutdown */
#include "playlist.h"

#include <stdlib.h>
#include <string.h>


/* ====================================================================== */
/*  缓冲池管理                                                             */
/* ====================================================================== */

/**
 * pool_alloc - 分配 PCM 缓冲池
 *
 * 为所有 NUM_BUFFERS 个数据块分配 PCM 数据缓冲区（每个缓冲区可容纳
 * BUFFER_FRAMES * OUTPUT_CHANNELS 个 short 样本）。
 * 若中途分配失败，已分配的部分会被全部释放。
 *
 * 返回: 1 成功，0 失败。
 */
int pool_alloc(void)
{
    for (int i = 0; i < NUM_BUFFERS; i++) {
        g_player.blocks[i].data = (short *)malloc(
            (size_t)BUFFER_FRAMES * OUTPUT_CHANNELS * sizeof(short));
        if (!g_player.blocks[i].data) {
            /* 释放所有已分配的内存 */
            for (int j = 0; j < i; j++) {
                free(g_player.blocks[j].data);
                g_player.blocks[j].data = NULL;
            }
            return 0;
        }
        g_player.blocks[i].frames = 0;
        g_player.blocks[i].state  = BS_FREE;
        memset(&g_player.blocks[i].hdr, 0, sizeof(WAVEHDR));
    }
    return 1;
}

/**
 * pool_free - 释放 PCM 缓冲池
 *
 * 释放所有数据块中的 PCM 数据缓冲区，并将指针置空。
 */
void pool_free(void)
{
    for (int i = 0; i < NUM_BUFFERS; i++) {
        free(g_player.blocks[i].data);
        g_player.blocks[i].data = NULL;
    }
}

/**
 * pool_grab_free - 获取一个空闲数据块
 *
 * 遍历缓冲池查找状态为 BS_FREE 的块，将其标记为 BS_DECODING 并返回。
 * 调用者必须持有 g_player.cs 锁。
 *
 * 返回: 空闲块指针，若无可用块则返回 NULL。
 */
PcmBlock *pool_grab_free(void)
{
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (g_player.blocks[i].state == BS_FREE) {
            g_player.blocks[i].state = BS_DECODING;
            return &g_player.blocks[i];
        }
    }
    return NULL;
}

/**
 * pool_push_filled - 将已解码完成的数据块提交给输出线程
 *
 * 将数据块状态设为 BS_FILLED，并唤醒 waveOut 线程进行提交。
 * 调用者必须持有 g_player.cs 锁。
 *
 * @b: 要提交的数据块指针
 */
void pool_push_filled(PcmBlock *b)
{
    b->state = BS_FILLED;
    SetEvent(g_player.wo_ctrl);
}


/* ====================================================================== */
/*  waveOut 设备管理                                                       */
/* ====================================================================== */

/**
 * waveout_open - 打开 waveOut 输出设备
 *
 * 使用 WAVE_MAPPER 打开默认音频输出设备，格式为立体声 16-bit PCM。
 * 设备通过事件回调（CALLBACK_EVENT）通知播放完成。
 * 音量不在此处设置（避免无锁写 g_player.volume），由调用方持 cs 调用
 * waveout_set_volume() 应用。
 *
 * @sample_rate: 采样率（Hz）
 * 返回: 1 成功，0 失败。
 */
int waveout_open(int sample_rate)
{
    WAVEFORMATEX wfx;
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = OUTPUT_CHANNELS;
    wfx.nSamplesPerSec  = (DWORD)sample_rate;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    /* 先写局部变量：waveOutOpen 在解码线程上执行，而 waveOut 线程在
     * cs 内读 g_player.hwo——直接写入全局会造成无锁数据竞争（x64 上
     * 原子但属 C11 UB）。句柄先在局部取得，成功后持锁提交。 */
    HWAVEOUT hwo = NULL;
    MMRESULT mr = waveOutOpen(&hwo, WAVE_MAPPER, &wfx,
                              (DWORD_PTR)g_player.wo_event, 0,
                              CALLBACK_EVENT);
    if (mr != MMSYSERR_NOERROR)
        return 0;
    /* 初始化为无信号状态，避免误处理上一次的完成事件 */
    ResetEvent(g_player.wo_event);

    EnterCriticalSection(&g_player.cs);
    g_player.hwo = hwo;
    LeaveCriticalSection(&g_player.cs);
    return 1;
}

/**
 * waveout_close - 关闭 waveOut 设备
 *
 * 先重置设备（终止所有未完成的缓冲区），再关闭设备句柄。
 */
void waveout_close(void)
{
    if (g_player.hwo) {
        waveOutReset(g_player.hwo);
        waveOutClose(g_player.hwo);
        g_player.hwo = NULL;
    }
}

/**
 * waveout_set_volume - 设置输出音量
 *
 * 将 0..100 的线性音量映射为 waveOut 的左右声道 16 位值（0..0xFFFF）。
 * 更新全局音量变量，若设备已打开则立即生效。
 *
 * @vol: 音量值 0..100，超出范围会被钳制。
 */
void waveout_set_volume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    g_player.volume = vol;
    if (!g_player.hwo) return;
    DWORD v = (DWORD)MulDiv(0xFFFF, vol, 100);
    waveOutSetVolume(g_player.hwo, MAKELONG(v, v));
}


/* ====================================================================== */
/*  播放控制 API（由 UI 线程调用）                                          */
/* ====================================================================== */

/**
 * player_open - 打开指定索引的曲目并开始播放
 *
 * @index: 播放列表中的曲目索引
 *
 * 在 UI 线程（持 cs）把曲目路径深拷贝到 g_player.open_path，解码线程
 * handle_open 只使用这份私有拷贝，绝不无锁解引用 g_playlist.items[]，
 * 从而消除"UI 清空/删除列表时解码线程读已释放内存"的竞态。播放列表的
 * 所有修改都在 UI 线程完成，而本函数也在 UI 线程运行，故拷贝本身安全。
 */
void player_open(int index)
{
    EnterCriticalSection(&g_player.cs);
    free(g_player.open_path);
    g_player.open_path = NULL;
    if (index >= 0 && index < g_playlist.count &&
        g_playlist.items[index].path) {
        g_player.open_path = _wcsdup(g_playlist.items[index].path);
    }
    g_player.open_index = index;
    InterlockedExchange(&g_player.cmd, CMD_OPEN);
    LeaveCriticalSection(&g_player.cs);
    SetEvent(g_player.dec_event);
}

/**
 * player_seek - 跳转到指定帧位置
 *
 * @frame: 目标帧号（绝对位置）
 */
void player_seek(uint64_t frame)
{
    EnterCriticalSection(&g_player.cs);
    InterlockedExchange64(&g_player.seek_frame, (LONG64)frame);
    InterlockedExchange(&g_player.cmd, CMD_SEEK);
    LeaveCriticalSection(&g_player.cs);
    SetEvent(g_player.dec_event);
}

/**
 * player_stop - 停止播放
 *
 * 停止解码和播放，释放解码器，但保持 waveOut 设备打开。
 */
void player_stop(void)
{
    EnterCriticalSection(&g_player.cs);
    InterlockedExchange(&g_player.cmd, CMD_STOP);
    LeaveCriticalSection(&g_player.cs);
    SetEvent(g_player.dec_event);
}

/**
 * player_toggle_pause - 切换播放/暂停状态
 *
 * 暂停时调用 waveOutPause 暂停音频输出；恢复时调用 waveOutRestart，
 * 并唤醒解码线程和 waveOut 线程继续处理。
 */
void player_toggle_pause(void)
{
    EnterCriticalSection(&g_player.cs);
    if (g_player.state == STATE_PLAYING) {
        g_player.state = STATE_PAUSED;
        if (g_player.hwo) waveOutPause(g_player.hwo);
    } else if (g_player.state == STATE_PAUSED) {
        g_player.state = STATE_PLAYING;
        if (g_player.hwo) waveOutRestart(g_player.hwo);
        SetEvent(g_player.dec_event);   /* 恢复解码 */
        SetEvent(g_player.wo_ctrl);     /* 恢复提交 */
    }
    LeaveCriticalSection(&g_player.cs);
}

/**
 * player_set_mode - 设置播放模式
 *
 * @mode: 播放模式（参见 PlayMode 枚举）
 */
void player_set_mode(int mode)
{
    EnterCriticalSection(&g_player.cs);
    g_player.play_mode = mode;
    LeaveCriticalSection(&g_player.cs);
}

/**
 * player_set_volume - 设置播放音量
 *
 * @vol: 音量值 0..100
 */
void player_set_volume(int vol)
{
    EnterCriticalSection(&g_player.cs);
    waveout_set_volume(vol);
    LeaveCriticalSection(&g_player.cs);
}


/* ====================================================================== */
/*  线程生命周期管理                                                        */
/* ====================================================================== */

/**
 * player_start_threads - 创建工作线程和同步对象
 *
 * 初始化临界区，创建四个 auto-reset 事件（用于 waveOut 完成通知、
 * waveOut 控制、解码器唤醒、空闲通知），然后启动解码线程和 waveOut 线程。
 *
 * 返回: 1 成功，0 失败。
 */
int player_start_threads(void)
{
    InitializeCriticalSection(&g_player.cs);

    /* 四个事件均为 auto-reset 模式，确保信号在被等待前不会丢失 */
    g_player.wo_event   = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_player.wo_ctrl    = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_player.dec_event  = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_player.wo_idle_evt= CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_player.wo_event || !g_player.wo_ctrl ||
        !g_player.dec_event || !g_player.wo_idle_evt)
        return 0;

    g_player.quit = 0;
    g_player.cmd  = CMD_NONE;

    g_player.dec_thread = CreateThread(NULL, 0, decoder_thread_proc, NULL, 0, NULL);
    g_player.wo_thread  = CreateThread(NULL, 0, waveout_thread_proc,  NULL, 0, NULL);
    if (!g_player.dec_thread || !g_player.wo_thread) return 0;

    return 1;
}

/**
 * player_shutdown - 关闭播放器，释放所有资源
 *
 * 设置退出标志，唤醒两个线程，等待它们退出后释放解码器、waveOut 设备、
 * 所有事件句柄和临界区。
 */
void player_shutdown(void)
{
    /* 通知两个线程退出 */
    InterlockedExchange(&g_player.quit, 1);
    SetEvent(g_player.dec_event);
    SetEvent(g_player.wo_ctrl);
    SetEvent(g_player.wo_idle_evt);

    if (g_player.dec_thread) {
        WaitForSingleObject(g_player.dec_thread, INFINITE);
        CloseHandle(g_player.dec_thread);
        g_player.dec_thread = NULL;
    }
    if (g_player.wo_thread) {
        WaitForSingleObject(g_player.wo_thread, INFINITE);
        CloseHandle(g_player.wo_thread);
        g_player.wo_thread = NULL;
    }

    /* 释放解码器（如果仍打开） */
    if (g_player.dec) { decoder_close(g_player.dec); g_player.dec = NULL; }

    waveout_close();

    /* 释放未消费的待打开路径拷贝（player_open 分配） */
    free(g_player.open_path);
    g_player.open_path = NULL;

    if (g_player.wo_event)    { CloseHandle(g_player.wo_event);    g_player.wo_event = NULL; }
    if (g_player.wo_ctrl)     { CloseHandle(g_player.wo_ctrl);     g_player.wo_ctrl = NULL; }
    if (g_player.dec_event)   { CloseHandle(g_player.dec_event);   g_player.dec_event = NULL; }
    if (g_player.wo_idle_evt) { CloseHandle(g_player.wo_idle_evt); g_player.wo_idle_evt = NULL; }

    DeleteCriticalSection(&g_player.cs);
}


/* ====================================================================== */
/*  waveOut 工作线程                                                        */
/* ====================================================================== */
/*
 * 每次循环（在 cs 锁保护下）：
 *   1. Flush 处理：若正在刷新，中止播放、回收所有飞行中的数据块、报告空闲，
 *      然后等待解码器结束 flush。
 *   2. 回收已完成的（WHDR_DONE）数据块 -> 标记 BS_FREE，唤醒解码器。
 *   3. 提交已填充的数据块 -> 通过 waveOutWrite 标记为 BS_PLAYING。
 *   4. 曲目结束检测：若解码器已 EOF 且无缓冲数据，发送 WM_TRACK_ENDED（仅一次）。
 * 然后等待事件：若有数据块正在播放，等待 wo_event（完成通知）和 wo_ctrl（控制信号）；
 * 否则仅等待 wo_ctrl。
 */

/**
 * has_pending_audio - 检查是否仍有数据块在播放或等待提交
 *
 * 调用者必须持有 g_player.cs 锁。
 *
 * 返回: 若有 BS_PLAYING 或 BS_FILLED 状态的数据块则返回 1，否则返回 0。
 */
static int has_pending_audio(void)
{
    for (int i = 0; i < NUM_BUFFERS; i++) {
        int s = g_player.blocks[i].state;
        if (s == BS_PLAYING || s == BS_FILLED) return 1;
    }
    return 0;
}

/**
 * waveout_thread_proc - waveOut 工作线程入口
 *
 * 持续循环，处理 flush 同步、回收已完成的数据块、提交新数据块和曲目结束检测。
 * 收到 quit 标志时退出。
 */
DWORD WINAPI waveout_thread_proc(LPVOID param)
{
    (void)param;

    for (;;) {
        if (g_player.quit) {
            /* 退出前若正处于 flush 握手期间，必须先把 wo_idle 置位并唤醒
             * 解码线程；否则解码线程会永远等在 flush_wait_idle() 上，
             * 使 player_shutdown 的 WaitForSingleObject(dec_thread) 死锁。
             * 若不在 flush 期间，直接退出即可（wo_idle 无关紧要）。 */
            EnterCriticalSection(&g_player.cs);
            if (g_player.flushing) {
                g_player.flushing = 0;
                g_player.wo_idle  = 1;
                LeaveCriticalSection(&g_player.cs);
                SetEvent(g_player.wo_idle_evt);
            } else {
                LeaveCriticalSection(&g_player.cs);
            }
            break;
        }

        EnterCriticalSection(&g_player.cs);

        /* --- 1. Flush 握手 --------------------------------------------- */
        if (g_player.flushing) {
            HWAVEOUT hwo = g_player.hwo;
            if (hwo) {
                waveOutReset(hwo);          /* 中止所有未完成缓冲区 -> WHDR_DONE */
                for (int i = 0; i < NUM_BUFFERS; i++) {
                    PcmBlock *b = &g_player.blocks[i];
                    if (b->state == BS_PLAYING) {
                        waveOutUnprepareHeader(hwo, &b->hdr, sizeof(WAVEHDR));
                        b->state = BS_FREE;
                    }
                }
            }
            /* 丢弃可能已填充的新数据块 */
            for (int i = 0; i < NUM_BUFFERS; i++) {
                if (g_player.blocks[i].state == BS_FILLED)
                    g_player.blocks[i].state = BS_FREE;
            }
            /* 排空可能的完成事件，避免 flush 后误唤醒 */
            while (WaitForSingleObject(g_player.wo_event, 0) == WAIT_OBJECT_0) {}

            g_player.wo_idle = 1;
            LeaveCriticalSection(&g_player.cs);
            SetEvent(g_player.wo_idle_evt);     /* 解码器正在等待此信号 */

            /* 等待解码器结束 flush */
            WaitForSingleObject(g_player.wo_ctrl, INFINITE);
            continue;
        }

        /* --- 2. 回收已完成的数据块 -------------------------------------- */
        HWAVEOUT hwo = g_player.hwo;
        int freed = 0;
        if (hwo) {
            for (int i = 0; i < NUM_BUFFERS; i++) {
                PcmBlock *b = &g_player.blocks[i];
                if (b->state == BS_PLAYING &&
                    (b->hdr.dwFlags & WHDR_DONE)) {
                    waveOutUnprepareHeader(hwo, &b->hdr, sizeof(WAVEHDR));
                    b->state = BS_FREE;
                    freed = 1;
                }
            }
        }
        if (freed)
            SetEvent(g_player.dec_event);       /* 有空闲槽位，通知解码器 */

        /* --- 3. 提交已填充的数据块 -------------------------------------- */
        int have_playing = 0;
        int submit_fail  = 0;
        int submit_ok    = 0;
        int err_code     = 0;
        if (hwo && g_player.state == STATE_PLAYING) {
            for (int i = 0; i < NUM_BUFFERS; i++) {
                PcmBlock *b = &g_player.blocks[i];
                if (b->state != BS_FILLED) continue;
                memset(&b->hdr, 0, sizeof(WAVEHDR));
                b->hdr.lpData         = (LPSTR)b->data;
                b->hdr.dwBufferLength = (DWORD)b->frames *
                                        OUTPUT_CHANNELS * sizeof(short);
                if (waveOutPrepareHeader(hwo, &b->hdr, sizeof(WAVEHDR))
                        != MMSYSERR_NOERROR) {
                    b->state = BS_FREE;          /* 准备失败，放弃该数据块 */
                    submit_fail = 1;
                    continue;
                }
                if (waveOutWrite(hwo, &b->hdr, sizeof(WAVEHDR))
                        != MMSYSERR_NOERROR) {
                    waveOutUnprepareHeader(hwo, &b->hdr, sizeof(WAVEHDR));
                    b->state = BS_FREE;
                    submit_fail = 1;
                    continue;
                }
                b->state = BS_PLAYING;
                have_playing = 1;
                submit_ok   = 1;
            }
        }
        for (int i = 0; i < NUM_BUFFERS; i++)
            if (g_player.blocks[i].state == BS_PLAYING) have_playing = 1;

        /* --- 4. 曲目结束检测 ------------------------------------------- */
        /* 携带当前 track_gen：若消息入队期间用户已切到新曲目，UI 收到后
         * 会因代数不匹配而丢弃，避免"旧曲结束"误触发对新曲的自动跳歌。 */
        LONG ended_gen = -1;
        if (g_player.eof && !g_player.ended_posted && !has_pending_audio()) {
            g_player.ended_posted = 1;
            ended_gen = g_player.track_gen;
        }

        /* --- 提交失败处理 ----------------------------------------------- */
        /* 被放弃的块已回到 BS_FREE，必须唤醒解码器，否则 8 块全部提交
         * 失败后解码线程会永远睡在 dec_event 上（静默死锁）。错误弹窗
         * 只报一次，直到某次提交恢复成功才允许再次上报，避免刷屏。 */
        if (submit_fail)
            SetEvent(g_player.dec_event);
        if (submit_ok)
            g_player.wo_err_posted = 0;
        else if (submit_fail && !g_player.wo_err_posted) {
            g_player.wo_err_posted = 1;
            err_code = PLAYER_ERR_DEVICE_WRITE;
        }

        LeaveCriticalSection(&g_player.cs);

        if (ended_gen >= 0)
            PostMessage(g_player.hMain, WM_TRACK_ENDED,
                        (WPARAM)ended_gen, 0);
        if (err_code)
            PostMessage(g_player.hMain, WM_PLAYER_ERROR,
                        (WPARAM)err_code, 0);

        /* --- 等待下一个感兴趣的事件 ------------------------------------ */
        if (have_playing) {
            HANDLE waits[2] = { g_player.wo_event, g_player.wo_ctrl };
            WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        } else {
            WaitForSingleObject(g_player.wo_ctrl, INFINITE);
        }
    }

    return 0;
}