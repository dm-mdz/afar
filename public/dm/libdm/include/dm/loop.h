/**
 ******************************************************************************
 * @file    loop.h
 * @author  MDM
 * @version 2.1.0
 * @brief   DM 独立事件循环、跨线程异步唤醒与循环绑定定时器（不透明句柄，零底层头泄露）。
 *          绝不触碰 DM 框架托管默认主循环；严格单线程亲和（仅 dm_async_send 跨线程安全）。
 ******************************************************************************
 */
#ifndef DM_LOOP_H
#define DM_LOOP_H

#include <dm.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct dm_loop dm_loop_st;
typedef struct dm_async dm_async_st;
typedef struct dm_loop_timer dm_loop_timer_st;

typedef enum dm_loop_run_mode
{
  DM_LOOP_RUN_DEFAULT = 0, /* 持续运行至无活跃句柄/请求或调用 dm_loop_stop */
  DM_LOOP_RUN_ONCE = 1,    /* 至多阻塞等待并派发一轮 I/O 与回调 */
  DM_LOOP_RUN_NOWAIT = 2   /* 非阻塞轮询并派发已就绪回调后立即返回 */
} dm_loop_run_mode_et;

/* 句柄异步关闭完成回调（触发时句柄内存已回收，不可再访问句柄指针） */
typedef void (*dm_close_fn)(dm_io void *pvUser);

/* ============================ 独立事件循环 ============================ */

/* 创建全新独立事件循环（DM_MEM_TAG_LOOP）并绑定当前线程为所属线程。严禁与 DM 默认主循环混用。 */
DM_API int dm_loop_create(dm_out dm_loop_st **ppstLoop);

/* 销毁独立事件循环（须在所属线程调用，跨线程回 DM_WRONG_THREAD）。
 * 若循环上仍有未关闭的活跃句柄（async/timer/tcp/udp/fs_event），返回 DM_BUSY 且不释放 pstLoop，
 * 调用方须先 close 全部句柄并调用 dm_loop_run(..., DM_LOOP_RUN_DEFAULT) 排空后再重试销毁。 */
DM_API int dm_loop_destroy(dm_io dm_loop_st *pstLoop);

/* 运行事件循环（须在所属线程调用，跨线程回 DM_WRONG_THREAD）。
 * 返回值：0 = 已无活跃句柄退出；>0 = 因 dm_loop_stop 或 ONCE/NOWAIT 仍有活跃句柄；<0 = DM 错误码。
 */
DM_API int dm_loop_run(dm_io dm_loop_st *pstLoop, dm_in dm_loop_run_mode_et eMode);

/* 请求停止事件循环（当前轮次结束后尽快从 dm_loop_run 返回；须在所属线程调用） */
DM_API void dm_loop_stop(dm_io dm_loop_st *pstLoop);

/* 查询循环是否仍有活跃句柄或请求 */
DM_API bool_t dm_loop_alive(dm_in const dm_loop_st *pstLoop);

/* 获取循环当前缓存的单调时间戳（毫秒） */
DM_API uint64 dm_loop_now(dm_in const dm_loop_st *pstLoop);

/* 主动刷新循环当前单调时间戳（长耗时计算后调用） */
DM_API void dm_loop_update_time(dm_io dm_loop_st *pstLoop);

/* 绑定/读取用户私有上下文指针（须在所属线程调用） */
DM_API void dm_loop_set_data(dm_io dm_loop_st *pstLoop, dm_in void *pvData);
DM_API void *dm_loop_get_data(dm_in const dm_loop_st *pstLoop);

/* ============================ 跨线程异步唤醒（dm_async） ============================ */

typedef void (*dm_async_fn)(dm_io dm_async_st *pstAsync, dm_io void *pvUser);

/* 在指定独立循环上创建异步唤醒句柄（须在 pstLoop 所属线程调用） */
DM_API int dm_async_create(dm_io dm_loop_st *pstLoop, dm_in dm_async_fn pfCb, dm_in void *pvUser,
                           dm_out dm_async_st **ppstOut);

/* 唤醒目标循环并合并触发 pfCb（唯一允许从任意外部线程并发调用的循环句柄函数！）。
 * 生命周期前置契约（AUD-V21-P1-10）：
 *   - DM_SHUTTING_DOWN 仅在 pstAsync 已调用 dm_async_close 但 pfOnClose 尚未完成（内存尚未释放）的
 *     安全窗口内有效（例如同循环线程在 close 后至排空前的保护）；
 *   - 跨线程生产者必须在调用 dm_async_close 之前先停止发送并 join 退出，否则并发 send 与
 *     pfOnClose 前的 dm_free_tag(pstAsync) 会产生释放后使用（UAF）。 */
DM_API int dm_async_send(dm_io dm_async_st *pstAsync);

/* 异步关闭并释放 pstAsync（须在 pstLoop 所属线程调用）。
 * 幂等保护：重复 close 返回 DM_SUCCESS 且不重复触发 pfOnClose；句柄内存在 pfOnClose 前安全释放。 */
DM_API int dm_async_close(dm_io dm_async_st *pstAsync, dm_in dm_close_fn pfOnClose,
                          dm_in void *pvUser);

DM_API void dm_async_set_data(dm_io dm_async_st *pstAsync, dm_in void *pvData);
DM_API void *dm_async_get_data(dm_in const dm_async_st *pstAsync);

/* ============================ 循环绑定定时器（dm_loop_timer） ============================ */
/* 说明：与 <dm/timer.h>（绑定 DM 主队列 uiLinkID）互补；本组绑定用户显式 dm_loop_st*。 */

typedef void (*dm_loop_timer_fn)(dm_io dm_loop_timer_st *pstTimer, dm_io void *pvUser);

DM_API int dm_loop_timer_create(dm_io dm_loop_st *pstLoop, dm_out dm_loop_timer_st **ppstOut);

/* 启动或重设单次/周期定时器（u64RepeatMs == 0 表示单次；允许在自身回调内重入调用；已 close 回
 * DM_SHUTTING_DOWN） */
DM_API int dm_loop_timer_start(dm_io dm_loop_timer_st *pstTimer, dm_in uint64 u64TimeoutMs,
                               dm_in uint64 u64RepeatMs, dm_in dm_loop_timer_fn pfCb,
                               dm_in void *pvUser);

DM_API int dm_loop_timer_stop(dm_io dm_loop_timer_st *pstTimer);
DM_API int dm_loop_timer_again(dm_io dm_loop_timer_st *pstTimer);
DM_API void dm_loop_timer_set_repeat(dm_io dm_loop_timer_st *pstTimer, dm_in uint64 u64RepeatMs);
DM_API uint64 dm_loop_timer_get_repeat(dm_in const dm_loop_timer_st *pstTimer);
DM_API uint64 dm_loop_timer_get_due_in(dm_in const dm_loop_timer_st *pstTimer);

/* 异步关闭并释放定时器（须在 pstLoop 所属线程调用；允许在 pfCb 回调内重入关闭自身） */
DM_API int dm_loop_timer_close(dm_io dm_loop_timer_st *pstTimer, dm_in dm_close_fn pfOnClose,
                               dm_in void *pvUser);

DM_API void dm_loop_timer_set_data(dm_io dm_loop_timer_st *pstTimer, dm_in void *pvData);
DM_API void *dm_loop_timer_get_data(dm_in const dm_loop_timer_st *pstTimer);

#ifdef __cplusplus
}
#endif

#endif /* DM_LOOP_H */
