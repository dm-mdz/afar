/**
 ******************************************************************************
 * @file    event.c
 * @author  MDM
 * @version 0.0.0.2
 * @brief   事件订阅与发布（DD-01 §3.3，AUD-08）：
 *          - 支持对称三形态，(NULL, work, after) 观察者零拷贝共享原子 qbuf 直投 dm_pool；
 *          - 采用 (pfBefore, pfWork) 二元组判重并返回独立 u32SubID 供退订；
 *          - 保留旧版 dm_event_subscribe / dm_event_unsubscribe 100% 兼容。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "event"
#include <dm.h>
#include <dm/event.h>
#include <dm/hash.h>
#include <dm/log.h>

#include <string.h>

#include "../dm_internal.h"

#define DM__EV_BUCKETS 32u

typedef struct dm__ev_sub
{
  uint32 u32SubID;
  dm_pool_lane_et eLane;
  dm_link_before_cb pfBefore;
  dm_link_work_cb pfWork;
  dm_link_after_cb pfAfter;
} dm__ev_sub_st;

typedef struct dm__ev_list
{
  uint uiEventID;
  dm__ev_sub_st *pastSub;
  uint uiCount;
  uint uiCap;
} dm__ev_list_st;

static dm_hash_table_st g_stEvTable;
static uint32 g_u32EvSubSeq = 0;

static bool_t dm__ev_can_subscribe(void)
{
  dm__phase_et ePhase = dm__phase();
  return ((DM__PHASE_INIT == ePhase) || (DM__PHASE_NONE == ePhase)) ? DM_TRUE : DM_FALSE;
}

static bool_t dm__ev_can_unsubscribe(void)
{
  dm__phase_et ePhase = dm__phase();
  return ((DM__PHASE_END == ePhase) || (DM__PHASE_NONE == ePhase)) ? DM_TRUE : DM_FALSE;
}

static dm__ev_list_st *dm__ev_find(dm_in uint uiEventID)
{
  if (NULL == g_stEvTable.pstBuckets)
  {
    return NULL;
  }
  return (dm__ev_list_st *)dm_hash_find(&g_stEvTable, uiEventID);
}

int dm_event_subscribe_ex(dm_in const dm_event_sub_opt_st *pstOpt, dm_out uint32 *pu32SubID)
{
  dm__ev_list_st *pstList = NULL;
  dm__ev_sub_st *pastNew = NULL;
  uint uiIdx = 0;
  uint32 u32ID = 0;

  DM__CHECK_LOOP();
  if ((NULL == pstOpt) || ((NULL == pstOpt->pfBefore) && (NULL == pstOpt->pfWork)) ||
      ((uint)pstOpt->eLane >= (uint)DM_LANE_MAX))
  {
    return DM_INVALID;
  }
  if (!dm__ev_can_subscribe())
  {
    DM_LOG_ERROR("dm_event_subscribe(0x%04x) outside init", pstOpt->uiEventID);
    return DM_INVALID;
  }
  if ((NULL == g_stEvTable.pstBuckets) &&
      (DM_SUCCESS != dm_hash_init(&g_stEvTable, DM__EV_BUCKETS, NULL)))
  {
    return DM_ALLOC;
  }
  pstList = dm__ev_find(pstOpt->uiEventID);
  if (NULL == pstList)
  {
    pstList = (dm__ev_list_st *)dm_calloc_tag(1u, sizeof(dm__ev_list_st), DM_MEM_TAG_EVENT);
    if (NULL == pstList)
    {
      return DM_ALLOC;
    }
    pstList->uiEventID = pstOpt->uiEventID;
    if (DM_SUCCESS != dm_hash_add(&g_stEvTable, pstOpt->uiEventID, pstList))
    {
      dm_free_tag(pstList);
      return DM_ALLOC;
    }
  }
  /* AUD-08：按 (pfBefore, pfWork) 二元组判重，允许多个不同的 (NULL, work_i, after_i) 共存 */
  for (uiIdx = 0; uiIdx < pstList->uiCount; uiIdx++)
  {
    if ((pstOpt->pfBefore == pstList->pastSub[uiIdx].pfBefore) &&
        (pstOpt->pfWork == pstList->pastSub[uiIdx].pfWork))
    {
      return DM_EXISTS;
    }
  }
  if (pstList->uiCount == pstList->uiCap)
  {
    pastNew = (dm__ev_sub_st *)dm_malloc_tag((pstList->uiCap + 4u) * sizeof(dm__ev_sub_st),
                                             DM_MEM_TAG_EVENT);
    if (NULL == pastNew)
    {
      return DM_ALLOC;
    }
    if (pstList->uiCount > 0u)
    {
      memcpy(pastNew, pstList->pastSub, pstList->uiCount * sizeof(dm__ev_sub_st));
    }
    dm_free_tag(pstList->pastSub);
    pstList->pastSub = pastNew;
    pstList->uiCap += 4u;
  }
  u32ID = ++g_u32EvSubSeq;
  if (0u == u32ID)
  {
    u32ID = ++g_u32EvSubSeq;
  }
  pstList->pastSub[pstList->uiCount].u32SubID = u32ID;
  pstList->pastSub[pstList->uiCount].eLane = pstOpt->eLane;
  pstList->pastSub[pstList->uiCount].pfBefore = pstOpt->pfBefore;
  pstList->pastSub[pstList->uiCount].pfWork = pstOpt->pfWork;
  pstList->pastSub[pstList->uiCount].pfAfter = pstOpt->pfAfter;
  pstList->uiCount++;
  if (NULL != pu32SubID)
  {
    *pu32SubID = u32ID;
  }
  return DM_SUCCESS;
}

int dm_event_subscribe(dm_in uint uiEventID, dm_in dm_link_before_cb pfBefore,
                       dm_in dm_link_work_cb pfWork, dm_in dm_link_after_cb pfAfter)
{
  dm_event_sub_opt_st stOpt;

  memset(&stOpt, 0, sizeof(stOpt));
  stOpt.uiEventID = uiEventID;
  stOpt.eLane = DM_LANE_DEFAULT;
  stOpt.pfBefore = pfBefore;
  stOpt.pfWork = pfWork;
  stOpt.pfAfter = pfAfter;
  return dm_event_subscribe_ex(&stOpt, NULL);
}

int dm_event_unsubscribe(dm_in uint uiEventID, dm_in dm_link_before_cb pfBefore)
{
  dm__ev_list_st *pstList = NULL;
  uint uiIdx = 0;

  DM__CHECK_LOOP();
  if (NULL == pfBefore)
  {
    return DM_INVALID;
  }
  if (!dm__ev_can_unsubscribe())
  {
    DM_LOG_ERROR("dm_event_unsubscribe(0x%04x) outside fini", uiEventID);
    return DM_INVALID;
  }
  pstList = dm__ev_find(uiEventID);
  if (NULL == pstList)
  {
    return DM_NOT_FOUND;
  }
  for (uiIdx = 0; uiIdx < pstList->uiCount; uiIdx++)
  {
    if (pfBefore == pstList->pastSub[uiIdx].pfBefore)
    {
      memmove(&pstList->pastSub[uiIdx], &pstList->pastSub[uiIdx + 1u],
              (pstList->uiCount - uiIdx - 1u) * sizeof(dm__ev_sub_st));
      pstList->uiCount--;
      return DM_SUCCESS;
    }
  }
  return DM_NOT_FOUND;
}

int dm_event_unsubscribe_id(dm_in uint uiEventID, dm_in uint32 u32SubID)
{
  dm__ev_list_st *pstList = NULL;
  uint uiIdx = 0;

  DM__CHECK_LOOP();
  if (0u == u32SubID)
  {
    return DM_INVALID;
  }
  if (!dm__ev_can_unsubscribe())
  {
    DM_LOG_ERROR("dm_event_unsubscribe_id(0x%04x) outside fini", uiEventID);
    return DM_INVALID;
  }
  pstList = dm__ev_find(uiEventID);
  if (NULL == pstList)
  {
    return DM_NOT_FOUND;
  }
  for (uiIdx = 0; uiIdx < pstList->uiCount; uiIdx++)
  {
    if (u32SubID == pstList->pastSub[uiIdx].u32SubID)
    {
      memmove(&pstList->pastSub[uiIdx], &pstList->pastSub[uiIdx + 1u],
              (pstList->uiCount - uiIdx - 1u) * sizeof(dm__ev_sub_st));
      pstList->uiCount--;
      return DM_SUCCESS;
    }
  }
  return DM_NOT_FOUND;
}

int dm_event_publish(dm_in uint uiEventID, dm_in const void *pv, dm_in uint32 u32Len)
{
  dm__ev_list_st *pstList = NULL;
  dm__qbuf_st *pstBuf = NULL;
  dm__qitem_st *pstItem = NULL;
  uint uiIdx = 0;
  int iPushed = 0;
  uint16 u16Depth = 0;
  uint64 u64TraceID = 0;
  int iHopErr = DM_SUCCESS;

  DM__CHECK_LOOP();
  if (dm__phase() >= DM__PHASE_TEARDOWN)
  {
    return DM_SHUTTING_DOWN;
  }
  if ((NULL == pv) && (u32Len > 0u))
  {
    return DM_INVALID;
  }
  pstList = dm__ev_find(uiEventID);
  if ((NULL == pstList) || (0u == pstList->uiCount))
  {
    return 0;
  }
  iHopErr = dm__dispatch_next_hop(DM_FALSE, uiEventID, &u16Depth, &u64TraceID);
  if (DM_SUCCESS != iHopErr)
  {
    return iHopErr;
  }
  if (u32Len > 0u)
  {
    pstBuf = dm__qbuf_new(pv, u32Len); /* 原子引用计数，多观察者与 dm_pool 线程零拷贝共享 */
    if (NULL == pstBuf)
    {
      return DM_ALLOC;
    }
  }
  for (uiIdx = 0; uiIdx < pstList->uiCount; uiIdx++)
  {
    const dm__ev_sub_st *pstSub = &pstList->pastSub[uiIdx];
    if ((NULL == pstSub->pfBefore) && (NULL != pstSub->pfWork))
    {
      uint32 u32SubID = pstSub->u32SubID;
      /* AUD-08：异步事件无状态观察者直接零拷贝入 dm_pool，不绕主线程总队列 */
      int iSubErr =
          dm__stateless_submit(0u, pstSub->eLane, pstSub->pfWork, pstSub->pfAfter, DM_SRC_EVENT, 0u,
                               0u, uiEventID, u16Depth, u64TraceID, pstBuf, NULL, DM_FALSE);
      if (DM_SUCCESS == iSubErr)
      {
        iPushed++;
      }
      else
      {
        DM_LOG_WARN("event 0x%04x: stateless subscriber #%u rejected: %s", uiEventID,
                    (unsigned)u32SubID, dm_errno_str(iSubErr));
      }
      continue;
    }
    pstItem = dm__qitem_new();
    if (NULL == pstItem)
    {
      DM_LOG_ERROR("event 0x%04x: out of memory, %u subscriber(s) skipped", uiEventID,
                   pstList->uiCount - uiIdx);
      break;
    }
    pstItem->uiLinkID = 0;
    pstItem->eLane = pstSub->eLane;
    pstItem->pfBefore = pstSub->pfBefore;
    pstItem->pfWork = pstSub->pfWork;
    pstItem->pfAfter = pstSub->pfAfter;
    pstItem->u8Src = DM_SRC_EVENT;
    pstItem->u64Tag = uiEventID;
    pstItem->u16Depth = u16Depth;
    pstItem->u64TraceID = u64TraceID;
    pstItem->pstBuf = pstBuf;
    dm__qbuf_get(pstBuf);
    if (DM_SUCCESS == dm__queue_push(pstItem))
    {
      iPushed++;
    }
  }
  dm__qbuf_put(pstBuf);
  return iPushed;
}

static void dm__ev_list_free(dm_in void *pvValue)
{
  dm__ev_list_st *pstList = (dm__ev_list_st *)pvValue;
  dm_free_tag(pstList->pastSub);
  dm_free_tag(pstList);
  return;
}

void dm__event_destroy(void)
{
  dm_hash_destroy(&g_stEvTable, dm__ev_list_free);
  memset(&g_stEvTable, 0, sizeof(g_stEvTable));
  return;
}

#ifdef __cplusplus
}
#endif
