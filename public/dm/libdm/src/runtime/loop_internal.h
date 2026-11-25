/**
 ******************************************************************************
 * @file    loop_internal.h
 * @author  MDM
 * @version 2.1.0
 * @brief   独立事件循环内部桥接契约（供 fs_event 与 transport/sock_* 复用，零 dm_internal.h
 * 耦合）。
 ******************************************************************************
 */
#ifndef DM_LOOP_INTERNAL_H
#define DM_LOOP_INTERNAL_H

#include <dm/loop.h>
#include "../uv_adapter.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* 获取底层 libuv 循环指针（pstLoop == NULL 返回 NULL） */
dm_uv_loop_st *dm__loop_raw(dm_in const dm_loop_st *pstLoop);

/* 别名：供 fs_event / sock_* 调用 */
dm_uv_loop_st *dm__loop_uv(dm_in const dm_loop_st *pstLoop);

/* 线程亲和守卫：非所属线程在 Debug 触发 DMASSERT、Release 安全返回 DM_WRONG_THREAD */
int dm__loop_check_owner(dm_in const dm_loop_st *pstLoop);

/* 别名：与 dm__loop_check_owner 完全等价 */
int dm__loop_check_thread(dm_in const dm_loop_st *pstLoop);

/* 活跃句柄计数（仅限所属线程调用，防止未排空句柄提前销毁循环） */
void dm__loop_handle_inc(dm_io dm_loop_st *pstLoop);
void dm__loop_handle_dec(dm_io dm_loop_st *pstLoop);

/* 统一 libuv 错误码 -> dm_errno_et 映射（实现位于 runtime/dm.c） */
int dm__uv_err_map(dm_in int iUvErr);

#ifdef __cplusplus
}
#endif

#endif /* DM_LOOP_INTERNAL_H */
