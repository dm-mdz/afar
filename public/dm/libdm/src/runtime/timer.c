/**
 ******************************************************************************
 * @file    timer.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   定时器（DD-01）。每个定时器一个 libuv 定时器，编号到对象查表，编号不重用。
 *
 * 到点时放一条进总队列；交给 before 之前再查一次（pfStale）：取消了、或者这一条是改期之前到的点
 * （代数对不上），就丢掉。一次性的在交出之前从表里摘掉，所以在它自己的回调里改期回 DM_NOT_FOUND。
 * libuv 句柄的释放是异步的（uv_close），这里自己管，调用方不用等。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "timer"
#include <dm.h>
#include <dm/timer.h>
#include <dm/hash.h>
#include <dm/log.h>

#include <string.h> /* memset */

#include "../dm_internal.h"

#define DM__TIMER_BUCKETS 64u

typedef struct dm__timer
{
  dm_timer_st stUv; /* 首成员：libuv 回调给的指针就是它 */
  uint32 u32Id;
  uint uiLinkID;
  uint64 u64Tag;
  uint32 u32Repeat;
  uint32 u32Gen; /* 改期一次加一；排在队列里的旧那一次靠它认出来 */
} dm__timer_st;

static dm_hash_table_st g_stTimerTable;
static uint32 g_u32TimerNext = 0;

static dm__timer_st *dm__timer_find(in uint32 u32Id)
{
  if (NULL == g_stTimerTable.pstBuckets)
  {
    return NULL;
  }
  return (dm__timer_st *)dm_hash_find(&g_stTimerTable, u32Id);
}

static void dm__timer_on_closed(in dm_handle_st *pstHandle)
{
  dm_free_tag(pstHandle);
  return;
}

/* 从表里摘掉、停掉、关掉（关完释放） */
static void dm__timer_drop(io dm__timer_st *pstTimer)
{
  (void)dm_hash_del(&g_stTimerTable, pstTimer->u32Id, NULL);
  (void)dm_timer_stop(&pstTimer->stUv);
  dm_close((dm_handle_st *)&pstTimer->stUv, dm__timer_on_closed);
  return;
}

/* 交出之前再查一次：返回真就丢掉 */
static bool_t dm__timer_stale(io dm__qitem_st *pstItem)
{
  dm__timer_st *pstTimer = dm__timer_find((uint32)(uintptr_t)pstItem->pvCtx);

  if ((NULL == pstTimer) || (pstTimer->u32Gen != pstItem->u32Aux))
  {
    return DM_TRUE; /* 取消了，或者是改期之前到的那一次 */
  }
  if (0u == pstTimer->u32Repeat)
  {
    dm__timer_drop(pstTimer); /* 一次性的：交出之前摘掉 */
  }
  return DM_FALSE;
}

static void dm__timer_on_fire(in dm_timer_st *pstUv)
{
  dm__timer_st *pstTimer = (dm__timer_st *)pstUv;
  dm_link_handler_st *pstHandler = dm__link_lookup(pstTimer->uiLinkID);
  dm__qitem_st *pstItem = NULL;

  if (NULL == pstHandler)
  {
    DM_LOG_WARN("timer %u: linkID 0x%04x is gone", pstTimer->u32Id, pstTimer->uiLinkID);
    return;
  }
  pstItem = dm__qitem_new();
  if (NULL == pstItem)
  {
    DM_LOG_ERROR("timer %u: out of memory, this tick is lost", pstTimer->u32Id);
    return;
  }
  pstItem->uiLinkID = pstTimer->uiLinkID;
  pstItem->pfBefore = pstHandler->pfBefore;
  pstItem->pfWork = pstHandler->pfWork;
  pstItem->pfAfter = pstHandler->pfAfter;
  pstItem->u8Src = DM_SRC_TIMER;
  pstItem->u64Tag = pstTimer->u64Tag;
  pstItem->pfStale = dm__timer_stale;
  pstItem->pvCtx = (void *)(uintptr_t)pstTimer->u32Id;
  pstItem->u32Aux = pstTimer->u32Gen;
  (void)dm__queue_push(pstItem); /* 拆除阶段会被拒，没关系：那时所有定时器也正在被取消 */
  return;
}

int dm_timer_add(in uint32 u32DelayMs, in uint32 u32RepeatMs, in uint uiLinkID, in uint64 u64Tag,
                 out uint32 *pu32Timer)
{
  dm__timer_st *pstTimer = NULL;
  int iErrCode = DM_SUCCESS;

  DM__CHECK_LOOP();
  if (dm__phase() >= DM__PHASE_TEARDOWN)
  {
    return DM_SHUTTING_DOWN;
  }
  if (NULL == dm__link_lookup(uiLinkID))
  {
    return DM_LINK_ERR_NOT_FOUND;
  }
  if ((NULL == g_stTimerTable.pstBuckets) &&
      (DM_SUCCESS != dm_hash_init(&g_stTimerTable, DM__TIMER_BUCKETS, NULL)))
  {
    return DM_ALLOC;
  }
  pstTimer = (dm__timer_st *)dm_calloc_tag(1u, sizeof(dm__timer_st), DM_MEM_TAG_TIMER);
  if (NULL == pstTimer)
  {
    return DM_ALLOC;
  }
  g_u32TimerNext++;
  if (0u == g_u32TimerNext)
  {
    g_u32TimerNext = 1; /* 绕回来（要加 40 多亿次）也不给 0 */
  }
  pstTimer->u32Id = g_u32TimerNext;
  pstTimer->uiLinkID = uiLinkID;
  pstTimer->u64Tag = u64Tag;
  pstTimer->u32Repeat = u32RepeatMs;
  iErrCode = dm_timer_init(dm_default_loop(), &pstTimer->stUv);
  if (DM_SUCCESS != iErrCode)
  {
    dm_free_tag(pstTimer);
    return iErrCode;
  }
  if (DM_SUCCESS != dm_hash_add(&g_stTimerTable, pstTimer->u32Id, pstTimer))
  {
    dm_close((dm_handle_st *)&pstTimer->stUv, dm__timer_on_closed);
    return DM_ALLOC;
  }
  (void)dm_timer_start(&pstTimer->stUv, dm__timer_on_fire, u32DelayMs, u32RepeatMs);
  if (NULL != pu32Timer)
  {
    *pu32Timer = pstTimer->u32Id;
  }
  return DM_SUCCESS;
}

int dm_timer_reset(in uint32 u32Timer, in uint32 u32DelayMs)
{
  dm__timer_st *pstTimer = NULL;

  DM__CHECK_LOOP();
  pstTimer = dm__timer_find(u32Timer);
  if (NULL == pstTimer)
  {
    return DM_NOT_FOUND;
  }
  pstTimer->u32Gen++;
  return dm_timer_start(&pstTimer->stUv, dm__timer_on_fire, u32DelayMs, pstTimer->u32Repeat);
}

int dm_timer_cancel(in uint32 u32Timer)
{
  dm__timer_st *pstTimer = NULL;

  DM__CHECK_LOOP();
  pstTimer = dm__timer_find(u32Timer);
  if (NULL == pstTimer)
  {
    return DM_NOT_FOUND;
  }
  dm__timer_drop(pstTimer);
  return DM_SUCCESS;
}

/* 拆除：取消所有定时器 */
static void dm__timer_drop_one(in void *pvValue)
{
  dm__timer_st *pstTimer = (dm__timer_st *)pvValue;
  (void)dm_timer_stop(&pstTimer->stUv);
  dm_close((dm_handle_st *)&pstTimer->stUv, dm__timer_on_closed);
  return;
}

uint32 dm__timer_count(void)
{
  return (NULL != g_stTimerTable.pstBuckets) ? g_stTimerTable.count : 0u;
}

void dm__timer_cancel_all(void)
{
  dm_hash_destroy(&g_stTimerTable, dm__timer_drop_one);
  memset(&g_stTimerTable, 0, sizeof(g_stTimerTable));
  return;
}

#ifdef __cplusplus
}
#endif
