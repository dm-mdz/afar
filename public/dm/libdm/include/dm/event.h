/**
 ******************************************************************************
 * @file    event.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   事件订阅（观察者，详设 2.8）。
 *
 * 事件号单独一张表，和 linkID 分开。段：0x0001~0x00FF 给 DM，别的按模块分（总表 src/eventids.h）。
 * 订阅只在 init、退订只在 fini，运行中表不变，不用加锁。没有单独的队列：发布时给每个订阅者
 * 各放一条进总队列（u8Src = DM_SRC_EVENT，u64Tag = 事件号），负载拷一份大家共用。
 * 订阅者之间不保证谁先。订阅的三段和登记处理项一样：before 在主线程，要慢活就提交 work。
 ******************************************************************************
 */
#ifndef DM_EVENT_H
#define DM_EVENT_H

#include <dm.h>
#include <dm/pool.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_EV_READY 0x0001u      /* 各模块都初始化完了（只发一次），负载为空 */
#define DM_EV_CONN_OPEN 0x0002u  /* 一条连接握手成功，负载 dm_conn_ev_st */
#define DM_EV_CONN_CLOSE 0x0003u /* 一条握过手的连接断了，负载 dm_conn_ev_st */

#define DM_PEER_ID_MAX 64u /* 连接 peer_id 最大长度（含末尾 '\0'） */

/* 连接建立 / 断开事件的负载：角色位与 peer_id（连接句柄仍收在 link 层里） */
typedef struct dm_conn_ev
{
  uint32 u32Role;
  char acPeerID[DM_PEER_ID_MAX];
} dm_conn_ev_st;

/* 只在 init 里调，别的时候回 DM_INVALID 并记 ERROR；支持对称三形态：(before,NULL,NULL)、
 * (before,work,after) 或 (NULL,work,after)，但 (NULL,NULL,*) 非法；
 * 同一个事件可以有好几个订阅者；同一 (pfBefore, pfWork) 二元组重复订阅回 DM_EXISTS */
DM_API int dm_event_subscribe(dm_in uint uiEventID, dm_in dm_link_before_cb pfBefore,
                              dm_in dm_link_work_cb pfWork, dm_in dm_link_after_cb pfAfter);

/* 扩展事件订阅选项（只在 init 里调） */
typedef struct dm_event_sub_opt
{
  uint uiEventID;             /* 事件号 */
  dm_pool_lane_et eLane;      /* 线程池泳道（0 = DM_LANE_DEFAULT） */
  dm_link_before_cb pfBefore; /* 主线程前段回调（可 NULL，此时 pfWork 必非 NULL） */
  dm_link_work_cb pfWork;     /* 线程池工作回调（可 NULL） */
  dm_link_after_cb pfAfter;   /* 主线程后段回调（可 NULL） */
} dm_event_sub_opt_st;

/* 扩展订阅（只在 init 里调）：支持泳道指定、(NULL, work, after) 零拷贝直通 dm_pool
 * 与独立退订句柄 u32SubID（pu32SubID 可 NULL） */
DM_API int dm_event_subscribe_ex(dm_in const dm_event_sub_opt_st *pstOpt, dm_out uint32 *pu32SubID);

/* 只在 fini 里调，别的时候回 DM_INVALID 并记 ERROR；没订过回 DM_NOT_FOUND */
DM_API int dm_event_unsubscribe(dm_in uint uiEventID, dm_in dm_link_before_cb pfBefore);
DM_API int dm_event_unsubscribe_id(dm_in uint uiEventID, dm_in uint32 u32SubID);

/* 主线程调。给每个订阅者各放一条进总队列，返回放了几条（没人订阅是 0，不算错）；
 * 拆除阶段回 DM_SHUTTING_DOWN；在别的线程调回 DM_WRONG_THREAD */
DM_API int dm_event_publish(dm_in uint uiEventID, dm_in const void *pv, dm_in uint32 u32Len);

#ifdef __cplusplus
}
#endif

#endif /* DM_EVENT_H */
