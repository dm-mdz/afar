/**
 ******************************************************************************
 * @file    raw_stream.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   零 <uv.h> 异步原始字节流抽象（DD-18 §1.1.4，入 libdm_net.a）：
 *          复用主循环非阻塞驱动已纳管的 TCP / POSIX Pipe 描述符，0 额外线程。
 ******************************************************************************
 */
#ifndef DM_RAW_STREAM_H
#define DM_RAW_STREAM_H

#include <dm/type.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct dm_raw_stream dm_raw_stream_st;

/* 读回调：iErr == DM_SUCCESS 表示收到 u32Len 字节；iErr < 0（DM_CONN_GONE=EOF /
 * DM_IO=错误）表示对端关闭或出错 */
typedef void (*dm_raw_read_cb)(dm_in dm_raw_stream_st *pstS, dm_in const uint8 *pu8Data,
                               dm_in uint32 u32Len, dm_in int iErr, dm_in void *pvUser);

/* 纳管原生流描述符至主循环：返回 DM_SUCCESS 时 tFd 所有权移交 libdm；失败时调用方仍保有 tFd */
DM_API int dm_raw_stream_adopt_fd(dm_in dm_peer_fd_t tFd, dm_in bool_t bIsPipe,
                                  dm_in dm_raw_read_cb pfOnRead, dm_in void *pvUser,
                                  dm_out dm_raw_stream_st **ppstOut);

DM_API int dm_raw_stream_write(dm_io dm_raw_stream_st *pstS, dm_in const void *pvData,
                               dm_in uint32 u32Len);

DM_API uint32 dm_raw_stream_write_queue(dm_in const dm_raw_stream_st *pstS);

DM_API void dm_raw_stream_close(dm_io dm_raw_stream_st *pstS);

#ifdef __cplusplus
}
#endif

#endif /* DM_RAW_STREAM_H */
