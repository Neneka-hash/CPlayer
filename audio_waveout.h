/*
 * audio_waveout.h
 *
 * waveOut 设备管理、PCM 缓冲池和 waveOut 工作线程的接口声明，
 * 以及供 UI 层调用的播放控制 API。
 */
#ifndef AUDIO_WAVEOUT_H
#define AUDIO_WAVEOUT_H

#include <windows.h>
#include <stdint.h>

/* ---- 缓冲池管理 ------------------------------------------------------- */

/* 分配 PCM 数据缓冲池（NUM_BUFFERS 个块）。启动时调用，关闭时释放。 */
int  pool_alloc(void);
void pool_free(void);

/*
 * 缓冲池访问辅助函数。调用者必须持有 g_player.cs 锁。
 *
 * pool_grab_free 查找 BS_FREE 状态的数据块，标记为 BS_DECODING 并返回
 * （若无可用块则返回 NULL）。
 * pool_push_filled 将数据块标记为 BS_FILLED 并唤醒 waveOut 线程。
 */
PcmBlock *pool_grab_free(void);
void      pool_push_filled(PcmBlock *b);

/* ---- waveOut 设备管理 ------------------------------------------------- */

/* 打开/关闭 waveOut 设备，指定采样率（立体声 16-bit）。
 * 音量由调用方持锁调用 waveout_set_volume() 设置。
 * 成功返回 1。 */
int  waveout_open(int sample_rate);
void waveout_close(void);

/* 立即应用音量 0..100。 */
void waveout_set_volume(int vol);

/* ---- 播放控制 API（由 UI 线程调用） ----------------------------------- *
 * 这些函数设置命令/标志并唤醒工作线程，自身不会阻塞在音频操作上。
 * 唯一例外：player_shutdown 会等待线程结束。 */

/* 打开播放列表中指定索引的曲目并开始播放。 */
void player_open(int index);
/* 跳转到指定帧位置（绝对帧号）。 */
void player_seek(uint64_t frame);
/* 停止播放，释放解码器（保持 waveOut 设备打开）。 */
void player_stop(void);
/* 切换播放/暂停状态。 */
void player_toggle_pause(void);
/* 设置播放模式（PlayMode 枚举值）。 */
void player_set_mode(int mode);
/* 设置音量 0..100。 */
void player_set_volume(int vol);

/* 启动解码和 waveOut 两个工作线程。成功返回 1。 */
int  player_start_threads(void);
/* 设置退出标志，等待两个线程结束后释放所有音频资源。 */
void player_shutdown(void);

/* waveOut 工作线程入口。 */
DWORD WINAPI waveout_thread_proc(LPVOID param);

#endif /* AUDIO_WAVEOUT_H */