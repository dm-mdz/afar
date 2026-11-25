/**
 ******************************************************************************
 * @file    timer.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   定时器（详设 2.7）。只在主线程调；到点时把 DM_SRC_TIMER 放进总队列交给 uiLinkID，
 *          u64Tag 原样带上。编号进程内不重用，取消一个已经到点的不会误伤别人。
 *          业务代码里不许用睡眠（AR-11），要等时间一律用它。
 ******************************************************************************
 */
#ifndef DM_TIMER_H
#define DM_TIMER_H

#include <dm.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* u32RepeatMs = 0 是一次性的。uiLinkID 没登记过当场回 DM_LINK_ERR_NOT_FOUND；
 * 拆除阶段回 DM_SHUTTING_DOWN；别的线程调回 DM_WRONG_THREAD */
DM_API int dm_timer_add(dm_in uint32 u32DelayMs, dm_in uint32 u32RepeatMs, dm_in uint uiLinkID,
                        dm_in uint64 u64Tag, dm_out uint32 *pu32Timer);

/* 改期（从现在起算）。改期之前已经到点、还排在队列里的那一次丢掉。
 * 一次性的在交给 before 之前就摘掉了，所以在它自己的回调里改期回 DM_NOT_FOUND */
DM_API int dm_timer_reset(dm_in uint32 u32Timer, dm_in uint32 u32DelayMs);

/* 取消。已经到点交出去了或者取消过的，返回 DM_NOT_FOUND，不算错；
 * 到点了、还排在队列里没交的，那一次丢掉 */
DM_API int dm_timer_cancel(dm_in uint32 u32Timer);

#ifdef __cplusplus
}
#endif

#endif /* DM_TIMER_H */
