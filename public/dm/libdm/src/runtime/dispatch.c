/**
 ******************************************************************************
 * @file    dispatch.c
 * @author  MDM
 * @version 0.0.0.3
 * @brief   处理项登记表、总队列转交、对称三形态 (NULL,work,after) 零拷贝直通 dm_pool
 *          与泳道慢活提交（DD-01 §3.3，AUD-02/08）。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "link"
#include <dm.h>
#include <dm/link.h>
#include <dm/hash.h>
#include <dm/log.h>
#include <dm/list.h>

#include <string.h>

#include "../dm_internal.h"

#define DM_LINK_HASH_BUCKETS 64u

static dm_hash_table_st g_stLinkHashTable;

/* ============================ 登记表 ============================ */

int dm__link_table_init(void)
{
  int iErrCode = DM_SUCCESS;

  if (NULL != g_stLinkHashTable.pstBuckets)
  {
    return DM_SUCCESS;
  }
  memset(&g_stLinkHashTable, 0, sizeof(g_stLinkHashTable));
  iErrCode = dm_hash_init(&g_stLinkHashTable, DM_LINK_HASH_BUCKETS, NULL);
  if (DM_SUCCESS != iErrCode)
  {
    return iErrCode;
  }
  return DM_SUCCESS;
}

static void dm__link_handler_free(in void *pvValue)
{
  dm_free_tag(pvValue);
  return;
}

void dm__link_table_destroy(void)
{
  dm_hash_destroy(&g_stLinkHashTable, dm__link_handler_free);
  memset(&g_stLinkHashTable, 0, sizeof(g_stLinkHashTable));
  return;
}

dm_link_handler_st *dm__link_lookup(in uint uiLinkID)
{
  if (NULL == g_stLinkHashTable.pstBuckets)
  {
    return NULL;
  }
  return (dm_link_handler_st *)dm_hash_find(&g_stLinkHashTable, uiLinkID);
}

bool_t dm__link_table_ready(void)
{
  return (NULL != g_stLinkHashTable.pstBuckets) ? DM_TRUE : DM_FALSE;
}

/* ============================ 处理项（对称三形态） ============================ */

int dm__link_register_ex(in uint uiLinkID, in dm_pool_lane_et eLane, in dm_link_before_cb pfBefore,
                         in dm_link_work_cb pfWork, in dm_link_after_cb pfAfter)
{
  dm_link_handler_st *pstHandler = NULL;

  if ((DM_LINKID_INVALID == uiLinkID) || ((uint)eLane >= (uint)DM_LANE_MAX))
  {
    return DM_INVALID;
  }
  /* 合法三形态：(before,NULL,NULL)、(NULL,work,after)、(before,work,after)；(NULL,NULL,*) 非法 */
  if ((NULL == pfBefore) && (NULL == pfWork))
  {
    return DM_INVALID;
  }
  if (NULL == g_stLinkHashTable.pstBuckets)
  {
    return DM_LINK_ERR_STATE;
  }
  pstHandler = dm__link_lookup(uiLinkID);
  if (NULL != pstHandler)
  {
    DM_LOG_WARN("linkID 0x%04x registered twice, previous handler overwritten", uiLinkID);
    DMASSERT(!"duplicate link handler registration");
    pstHandler->pfBefore = pfBefore;
    pstHandler->pfWork = pfWork;
    pstHandler->pfAfter = pfAfter;
    pstHandler->u8Lane = (uint8)eLane;
    return DM_LINK_ERR_REREGIST;
  }
  pstHandler =
      (dm_link_handler_st *)dm_malloc_tag(sizeof(dm_link_handler_st), DM_MEM_TAG_LINK_CALLBACK);
  if (NULL == pstHandler)
  {
    return DM_ALLOC;
  }
  pstHandler->uiLinkID = uiLinkID;
  pstHandler->pfBefore = pfBefore;
  pstHandler->pfWork = pfWork;
  pstHandler->pfAfter = pfAfter;
  pstHandler->u8Lane = (uint8)eLane;
  if (DM_SUCCESS != dm_hash_add(&g_stLinkHashTable, uiLinkID, pstHandler))
  {
    dm_free_tag(pstHandler);
    return DM_ALLOC;
  }
  return DM_SUCCESS;
}

int dm__link_register(in uint uiLinkID, in dm_link_before_cb pfBefore, in dm_link_work_cb pfWork,
                      in dm_link_after_cb pfAfter)
{
  return dm__link_register_ex(uiLinkID, DM_LANE_DEFAULT, pfBefore, pfWork, pfAfter);
}

int dm_link_register_handler_ex(in uint uiLinkID, in dm_pool_lane_et eLane,
                                in dm_link_before_cb pfBefore, in dm_link_work_cb pfWork,
                                in dm_link_after_cb pfAfter)
{
  DM__CHECK_LOOP();
  return dm__link_register_ex(uiLinkID, eLane, pfBefore, pfWork, pfAfter);
}

int dm_link_register_handler(in uint uiLinkID, in dm_link_before_cb pfBefore,
                             in dm_link_work_cb pfWork, in dm_link_after_cb pfAfter)
{
  if (DM_LINKID_INVALID == uiLinkID)
  {
    return DM_LINK_ERR_ARG;
  }
  return dm_link_register_handler_ex(uiLinkID, DM_LANE_DEFAULT, pfBefore, pfWork, pfAfter);
}

bool_t dm_link_has_handler(in uint uiLinkID)
{
  if (DM_LINKID_INVALID == uiLinkID)
  {
    return DM_FALSE;
  }
  return (NULL != dm__link_lookup(uiLinkID)) ? DM_TRUE : DM_FALSE;
}

/* ============================ 总队列入队 ============================ */

int dm__link_enqueue_ex(in uint uiLinkID, in const dm_link_msg_st *pstMsg, in bool_t bExternal,
                        in uint32 u32ConnID, in uint32 u32ReqID, in uint8 u8ReplySrc)
{
  dm_link_handler_st *pstHandler = dm__link_lookup(uiLinkID);
  dm__qitem_st *pstItem = NULL;
  uint16 u16Depth = 0;
  uint64 u64TraceID = 0;
  int iErr = DM_SUCCESS;

  if (NULL == pstHandler)
  {
    return DM_LINK_ERR_NOT_FOUND;
  }
  iErr = dm__dispatch_next_hop(bExternal, uiLinkID, &u16Depth, &u64TraceID);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  pstItem = dm__qitem_new();
  if (NULL == pstItem)
  {
    return DM_ALLOC;
  }
  if (pstMsg->u32Len > 0u)
  {
    pstItem->pstBuf = dm__qbuf_new_ex(pstMsg->pu8Data, pstMsg->u32Len, pstMsg->u8Flags);
    if (NULL == pstItem->pstBuf)
    {
      dm__qitem_free(pstItem);
      return DM_ALLOC;
    }
  }
  pstItem->uiLinkID = uiLinkID;
  pstItem->pfBefore = pstHandler->pfBefore;
  pstItem->pfWork = pstHandler->pfWork;
  pstItem->pfAfter = pstHandler->pfAfter;
  pstItem->eLane = (dm_pool_lane_et)pstHandler->u8Lane;
  pstItem->u8Src = pstMsg->u8Src;
  pstItem->u8Flags = pstMsg->u8Flags;
  pstItem->bExternal = bExternal;
  pstItem->u32Role = pstMsg->u32Role;
  pstItem->u64Tag = pstMsg->u64Tag;
  pstItem->u16Depth = u16Depth;
  pstItem->u64TraceID = u64TraceID;
  pstItem->iStatus = pstMsg->iStatus;
  pstItem->pvPriv = pstMsg->pvPriv;
  pstItem->stReply.u32ConnID = u32ConnID;
  pstItem->stReply.u32ReqID = u32ReqID;
  pstItem->stReply.u8Src = u8ReplySrc;
  return dm__queue_push(pstItem);
}

int dm__link_enqueue(in uint uiLinkID, in const dm_link_msg_st *pstMsg, in bool_t bExternal,
                     in uint32 u32ConnID, in uint32 u32ReqID)
{
  return dm__link_enqueue_ex(uiLinkID, pstMsg, bExternal, u32ConnID, u32ReqID, pstMsg->u8Src);
}

int dm_link_dispatch(in uint uiLinkID, in const dm_link_msg_st *pstMsg)
{
  dm_link_msg_st stLocal = {0};

  if ((NULL == pstMsg) || ((NULL == pstMsg->pu8Data) && (pstMsg->u32Len > 0u)))
  {
    return DM_LINK_ERR_ARG;
  }
  DM__CHECK_LOOP();
  if (dm__phase() >= DM__PHASE_TEARDOWN)
  {
    return DM_SHUTTING_DOWN;
  }
  stLocal = *pstMsg;
  stLocal.pvPriv =
      NULL; /* 防止转交携带旧请求栈/队列项的 pvPriv（避免悬垂指针与 T-RPC-12 越权应答） */
  return dm__link_enqueue(uiLinkID, &stLocal, DM_FALSE, 0u, 0u);
}

/* ============================ 无状态直通 (NULL, work, after) ============================ */

typedef struct dm__stateless_wrap
{
  dm_stateless_job_st stJob;
  dm__qbuf_st *pstBuf;
  dm_link_work_cb pfWork;
  dm_link_after_cb pfAfter;
  uint32 u32ExtBytes; /* AUD-08：保留计入总队列 64MB 背压水位，直至 after 完成后释放 */
} dm__stateless_wrap_st;

static void dm__stateless_work_wrap(io void *pvArg)
{
  dm__stateless_wrap_st *pstWrap = (dm__stateless_wrap_st *)pvArg;
  if ((NULL != pstWrap) && (NULL != pstWrap->pfWork))
  {
    pstWrap->pfWork(&pstWrap->stJob);
  }
  return;
}

static void dm__stateless_after_wrap(io void *pvArg, in int iStatus)
{
  dm__stateless_wrap_st *pstWrap = (dm__stateless_wrap_st *)pvArg;
  if (NULL == pstWrap)
  {
    return;
  }
  if (NULL != pstWrap->pfAfter)
  {
    pstWrap->pfAfter(&pstWrap->stJob, iStatus);
  }
  else
  {
    if (pstWrap->stJob.bHasReply)
    {
      int iRepErr = (DM_SUCCESS != iStatus) ? iStatus : pstWrap->stJob.iResultErr;
      (void)dm__ext_reply_send(pstWrap->stJob.au64Reply, iRepErr, pstWrap->stJob.pvResult,
                               pstWrap->stJob.u32ResultLen);
    }
    if ((NULL != pstWrap->stJob.pvResult) && (pstWrap->stJob.u32ResultLen > 0u) &&
        (0u != (pstWrap->stJob.u8Flags & DM_FRAME_F_SENSITIVE)))
    {
      dm_secure_zero(pstWrap->stJob.pvResult, pstWrap->stJob.u32ResultLen);
    }
    dm_free_tag(pstWrap->stJob.pvResult);
    pstWrap->stJob.pvResult = NULL;
  }
  if (pstWrap->u32ExtBytes > 0u)
  {
    dm__queue_ext_release(pstWrap->u32ExtBytes);
    pstWrap->u32ExtBytes = 0u;
  }
  dm__qbuf_put(pstWrap->pstBuf);
  dm_free_tag(pstWrap);
  return;
}

static void dm__stateless_job_fill(out dm_stateless_job_st *pstJob, in uint uiLinkID,
                                   in uint8 u8Src, in uint8 u8Flags, in uint32 u32Role,
                                   in uint64 u64Tag, in uint16 u16Depth, in uint64 u64TraceID,
                                   in dm__qbuf_st *pstBuf, in const dm_link_msg_st *pstReplyMsg)
{
  memset(pstJob, 0, sizeof(*pstJob));
  pstJob->uiLinkID = uiLinkID;
  pstJob->pu8Data = (NULL != pstBuf) ? pstBuf->au8Data : NULL;
  pstJob->u32Len = (NULL != pstBuf) ? pstBuf->u32Len : 0u;
  pstJob->u8Src = u8Src;
  pstJob->u8Flags = u8Flags;
  pstJob->u32Role = u32Role;
  pstJob->u64Tag = u64Tag;
  pstJob->u16Depth = u16Depth;
  pstJob->u64TraceID = u64TraceID;
  pstJob->iResultErr = DM_SUCCESS;
  if ((NULL != pstReplyMsg) && (DM_SUCCESS == dm__ext_reply_take(pstReplyMsg, pstJob->au64Reply)))
  {
    pstJob->bHasReply = DM_TRUE;
  }
  return;
}

int dm_stateless_reply_send(io dm_stateless_job_st *pstJob, in int iStatus, in const void *pvBody,
                            in uint32 u32Len)
{
  int iErr = DM_SUCCESS;

  DM__CHECK_LOOP();
  if (NULL == pstJob)
  {
    return DM_INVALID;
  }
  if (!pstJob->bHasReply)
  {
    return DM_NOT_FOUND;
  }
  iErr = dm__ext_reply_send(pstJob->au64Reply, iStatus, pvBody, u32Len);
  if (DM_SUCCESS == iErr)
  {
    pstJob->bHasReply = DM_FALSE;
  }
  return iErr;
}

int dm__stateless_submit(in uint uiLinkID, in dm_pool_lane_et eLane, in dm_link_work_cb pfWork,
                         in dm_link_after_cb pfAfter, in uint8 u8Src, in uint8 u8Flags,
                         in uint32 u32Role, in uint64 u64Tag, in uint16 u16Depth,
                         in uint64 u64TraceID, in dm__qbuf_st *pstBuf,
                         in const dm_link_msg_st *pstReplyMsg, in bool_t bExtOwned)
{
  dm__stateless_wrap_st *pstWrap = NULL;
  uint32 u32BufLen = (NULL != pstBuf) ? pstBuf->u32Len : 0u;
  int iErr = DM_SUCCESS;

  if (NULL == pfWork)
  {
    if (bExtOwned && (u32BufLen > 0u))
    {
      dm__queue_ext_release(u32BufLen);
    }
    return DM_INVALID;
  }
  pstWrap =
      (dm__stateless_wrap_st *)dm_calloc_tag(1u, sizeof(dm__stateless_wrap_st), DM_MEM_TAG_POOL);
  if (NULL == pstWrap)
  {
    dm_stateless_job_st stFallback = {0};
    DM_LOG_ERROR("stateless submit oom for link 0x%04x", uiLinkID);
    dm__stateless_job_fill(&stFallback, uiLinkID, u8Src, u8Flags, u32Role, u64Tag, u16Depth,
                           u64TraceID, pstBuf, pstReplyMsg);
    if (NULL != pfAfter)
    {
      bool_t bPrevIn = DM_FALSE;
      uint16 u16PrevDepth = 0;
      uint64 u64PrevTrace = 0;
      dm__dispatch_ctx_get(&bPrevIn, &u16PrevDepth, &u64PrevTrace);
      dm__dispatch_ctx_set(DM_TRUE, u16Depth, (0u != u64TraceID) ? u64TraceID : dm__trace_next());
      pfAfter(&stFallback, DM_ALLOC);
      dm__dispatch_ctx_set(bPrevIn, u16PrevDepth, u64PrevTrace);
    }
    else if (stFallback.bHasReply)
    {
      (void)dm__ext_reply_send(stFallback.au64Reply, DM_ALLOC, NULL, 0u);
    }
    if (bExtOwned && (u32BufLen > 0u))
    {
      dm__queue_ext_release(u32BufLen);
    }
    return DM_ALLOC;
  }
  pstWrap->pfWork = pfWork;
  pstWrap->pfAfter = pfAfter;
  pstWrap->pstBuf = pstBuf;
  dm__qbuf_get(pstBuf);

  dm__stateless_job_fill(&pstWrap->stJob, uiLinkID, u8Src, u8Flags, u32Role, u64Tag, u16Depth,
                         u64TraceID, pstBuf, pstReplyMsg);

  /* AUD-08：外部帧直接零重复计数接管 g_u64QueueExt；非外部无状态直通按需计入并在 after 后释放 */
  if (u32BufLen > 0u)
  {
    pstWrap->u32ExtBytes = u32BufLen;
    if (!bExtOwned)
    {
      dm__queue_ext_add(pstWrap->u32ExtBytes);
    }
  }

  iErr = dm__pool_submit_internal(eLane, dm__stateless_work_wrap, dm__stateless_after_wrap, pstWrap,
                                  dm__stat_key(uiLinkID, u8Src, u64Tag), u16Depth, u64TraceID);
  if (DM_SUCCESS != iErr)
  {
    DM_LOG_WARN("stateless submit rejected for link 0x%04x lane %u: %s", uiLinkID, (unsigned)eLane,
                dm_errno_str(iErr));
    if (NULL != pstWrap->pfAfter)
    {
      bool_t bPrevIn = DM_FALSE;
      uint16 u16PrevDepth = 0;
      uint64 u64PrevTrace = 0;
      dm__dispatch_ctx_get(&bPrevIn, &u16PrevDepth, &u64PrevTrace);
      dm__dispatch_ctx_set(DM_TRUE, u16Depth, (0u != u64TraceID) ? u64TraceID : dm__trace_next());
      pstWrap->pfAfter(&pstWrap->stJob, iErr);
      dm__dispatch_ctx_set(bPrevIn, u16PrevDepth, u64PrevTrace);
    }
    else if (pstWrap->stJob.bHasReply)
    {
      (void)dm__ext_reply_send(pstWrap->stJob.au64Reply, iErr, NULL, 0u);
    }
    if (pstWrap->u32ExtBytes > 0u)
    {
      dm__queue_ext_release(pstWrap->u32ExtBytes);
      pstWrap->u32ExtBytes = 0u;
    }
    dm__qbuf_put(pstWrap->pstBuf);
    dm_free_tag(pstWrap);
  }
  return iErr;
}

/* ============================ 线程池提交（转发至 dm_pool） ============================ */

int dm_link_work_submit_lane(in dm_pool_lane_et eLane, in dm_link_msg_st *pstMsg, in void *pvArg)
{
  uint16 u16Depth = 0;
  uint64 u64TraceID = 0;

  if (NULL == pstMsg)
  {
    return DM_LINK_ERR_ARG;
  }
  dm__dispatch_ctx_get(NULL, &u16Depth, &u64TraceID);
  if (0u == u64TraceID)
  {
    u16Depth = pstMsg->u16Depth;
    u64TraceID = pstMsg->u64TraceID;
  }
  return dm__pool_submit_internal(eLane, pstMsg->pfWork, pstMsg->pfAfter, pvArg,
                                  dm__stat_key(pstMsg->uiLinkID, pstMsg->u8Src, pstMsg->u64Tag),
                                  u16Depth, u64TraceID);
}

int dm_link_work_submit(in dm_link_msg_st *pstMsg, in void *pvArg)
{
  if (NULL == pstMsg)
  {
    return DM_LINK_ERR_ARG;
  }
  return dm_link_work_submit_lane(pstMsg->eLane, pstMsg, pvArg);
}

#ifdef __cplusplus
}
#endif
