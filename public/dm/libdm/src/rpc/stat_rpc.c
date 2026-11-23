/**
 ******************************************************************************
 * @file    stat_rpc.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   dm.stat 内置诊断接口（拆入 libdm_net.a，切断 libdm_core 对 json/net/child 的依赖）。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "dm"
#include <dm.h>
#include <dm/json.h>
#include <dm/rpc.h>
#include <dm/pool.h>
#include <dm/log.h>
#include <dm/mem.h>
#include "../dm_internal.h"

typedef struct dm__stat_rpc_ctx
{
  yyjson_mut_doc *pstDoc;
  yyjson_mut_val *pstArr;
  bool_t bOom;
} dm__stat_rpc_ctx_st;

/* ============================ dm.stat 辅助与实现 ============================ */

static void dm__stat_rpc_mem_one(in int iTag, in const char *pcName, in uint64 u64Blocks,
                                 in uint64 u64Bytes, in void *pvArg)
{
  dm__stat_rpc_ctx_st *pstCtx = (dm__stat_rpc_ctx_st *)pvArg;
  yyjson_mut_val *pstObj = NULL;
  bool bOk = true;

  if (pstCtx->bOom)
  {
    return;
  }
  pstObj = yyjson_mut_arr_add_obj(pstCtx->pstDoc, pstCtx->pstArr);
  if (NULL == pstObj)
  {
    pstCtx->bOom = DM_TRUE;
    return;
  }
  bOk = bOk && yyjson_mut_obj_add_int(pstCtx->pstDoc, pstObj, "tag", iTag);
  if (NULL != pcName)
  {
    bOk = bOk && yyjson_mut_obj_add_str(pstCtx->pstDoc, pstObj, "name", pcName);
  }
  bOk = bOk && yyjson_mut_obj_add_uint(pstCtx->pstDoc, pstObj, "blocks", u64Blocks);
  bOk = bOk && yyjson_mut_obj_add_uint(pstCtx->pstDoc, pstObj, "bytes", u64Bytes);
  if (!bOk)
  {
    pstCtx->bOom = DM_TRUE;
  }
  return;
}

static void dm__stat_rpc_row_one(in const dm__stat_row_st *pstRow, in void *pvArg)
{
  dm__stat_rpc_ctx_st *pstCtx = (dm__stat_rpc_ctx_st *)pvArg;
  yyjson_mut_val *pstObj = NULL;
  const char *pcName = NULL;
  bool bOk = true;

  if (pstCtx->bOom)
  {
    return;
  }
  pstObj = yyjson_mut_arr_add_obj(pstCtx->pstDoc, pstCtx->pstArr);
  if (NULL == pstObj)
  {
    pstCtx->bOom = DM_TRUE;
    return;
  }
  if (0u != (pstRow->u32Key & DM__STAT_EVENT_BIT))
  {
    bOk = bOk && yyjson_mut_obj_add_uint(pstCtx->pstDoc, pstObj, "event", pstRow->u32Key & 0xFFFFu);
  }
  else
  {
    bOk = bOk && yyjson_mut_obj_add_uint(pstCtx->pstDoc, pstObj, "id", pstRow->u32Key);
    pcName = dm__iface_name((uint)pstRow->u32Key);
    if (NULL != pcName)
    {
      bOk = bOk && yyjson_mut_obj_add_str(pstCtx->pstDoc, pstObj, "name", pcName);
    }
  }
  bOk = bOk && yyjson_mut_obj_add_uint(pstCtx->pstDoc, pstObj, "before", pstRow->u32Before);
  bOk = bOk && yyjson_mut_obj_add_uint(pstCtx->pstDoc, pstObj, "after", pstRow->u32After);
  bOk = bOk &&
        yyjson_mut_obj_add_uint(pstCtx->pstDoc, pstObj, "total_us", pstRow->u64TotalNs / 1000u);
  bOk = bOk && yyjson_mut_obj_add_uint(pstCtx->pstDoc, pstObj, "max_us", pstRow->u64MaxNs / 1000u);
  if (!bOk)
  {
    pstCtx->bOom = DM_TRUE;
  }
  return;
}

static void dm__diag_reply_err(in dm_link_msg_st *pstMsg, in int iStatus);

int dm__stat_builtin(in void *pvArg)
{
  dm_link_msg_st *pstMsg = (dm_link_msg_st *)pvArg;
  yyjson_mut_doc *pstDoc = yyjson_mut_doc_new(dm_json_alc());
  yyjson_mut_val *pstRoot = NULL;
  yyjson_mut_val *pstObj = NULL;
  dm__stat_rpc_ctx_st stCtx = {0};
  dm__stat_summary_st stSum = {0};
  dm_log_stat_st stLog = {0};
  dm_reply_st stReply = {0};
  char *pcText = NULL;
  size_t uLen = 0;
  bool bOk = true;

  if (NULL == pstDoc)
  {
    dm__diag_reply_err(pstMsg, DM_ALLOC);
    return DM_SUCCESS;
  }
  dm__stat_summary(&stSum);
  pstRoot = yyjson_mut_obj(pstDoc);
  bOk = (NULL != pstRoot);
  if (bOk)
  {
    yyjson_mut_doc_set_root(pstDoc, pstRoot);
    bOk = bOk && yyjson_mut_obj_add_str(pstDoc, pstRoot, "version", dm_version_string());
  }

  stCtx.pstDoc = pstDoc;
  stCtx.pstArr = bOk ? yyjson_mut_arr(pstDoc) : NULL;
  stCtx.bOom = DM_FALSE;
  bOk = bOk && (NULL != stCtx.pstArr) &&
        yyjson_mut_obj_add_val(pstDoc, pstRoot, "handlers", stCtx.pstArr);
  if (bOk)
  {
    dm__stat_each(dm__stat_rpc_row_one, &stCtx);
    bOk = bOk && !stCtx.bOom;
  }

  pstObj = bOk ? yyjson_mut_obj(pstDoc) : NULL;
  bOk = bOk && (NULL != pstObj) && yyjson_mut_obj_add_val(pstDoc, pstRoot, "loop", pstObj);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "max_busy_us", stSum.u64LoopMaxNs / 1000u);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "slow", stSum.u32LoopSlow);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "queue", dm__queue_len());

  pstObj = bOk ? yyjson_mut_obj(pstDoc) : NULL;
  bOk = bOk && (NULL != pstObj) && yyjson_mut_obj_add_val(pstDoc, pstRoot, "pool", pstObj);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "threads", dm__threads_count());
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "done", stSum.u32PoolN);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "inflight", dm__work_inflight());
  bOk = bOk && yyjson_mut_obj_add_uint(
                   pstDoc, pstObj, "wait_avg_us",
                   (0u != stSum.u32PoolN) ? (stSum.u64PoolTotalNs / stSum.u32PoolN / 1000u) : 0u);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "wait_max_us", stSum.u64PoolMaxNs / 1000u);

  stCtx.pstArr = bOk ? yyjson_mut_arr(pstDoc) : NULL;
  stCtx.bOom = DM_FALSE;
  bOk =
      bOk && (NULL != stCtx.pstArr) && yyjson_mut_obj_add_val(pstDoc, pstRoot, "mem", stCtx.pstArr);
  if (bOk)
  {
    dm__mem_tag_each(dm__stat_rpc_mem_one, &stCtx);
    bOk = bOk && !stCtx.bOom;
  }

  pstObj = bOk ? yyjson_mut_obj(pstDoc) : NULL;
  bOk = bOk && (NULL != pstObj) && yyjson_mut_obj_add_val(pstDoc, pstRoot, "push", pstObj);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "seq", dm_push_seq());
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "ring_bytes", dm__push_ring_bytes());
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "calls", dm__call_pending());

  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstRoot, "conns", dm__conn_count());
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstRoot, "children", dm__ext_child_count());

  dm_log_stat(&stLog);
  pstObj = bOk ? yyjson_mut_obj(pstDoc) : NULL;
  bOk = bOk && (NULL != pstObj) && yyjson_mut_obj_add_val(pstDoc, pstRoot, "log", pstObj);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "lines", stLog.u64Lines);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "lock_us", stLog.u64LockNs / 1000u);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "lock_max_us", stLog.u64LockMaxNs / 1000u);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "write_us", stLog.u64WriteNs / 1000u);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "write_max_us", stLog.u64WriteMaxNs / 1000u);

  if (!bOk)
  {
    yyjson_mut_doc_free(pstDoc);
    dm__diag_reply_err(pstMsg, DM_ALLOC);
    return DM_SUCCESS;
  }
  pcText = dm_json_write_mut(pstDoc, &uLen);
  yyjson_mut_doc_free(pstDoc);
  if (DM_SUCCESS == dm_reply_take(pstMsg, &stReply))
  {
    int iSendErr = dm_reply_send(&stReply, (NULL != pcText) ? DM_SUCCESS : DM_ALLOC, pcText,
                                 (NULL != pcText) ? (uint32)uLen : 0u);
    if (DM_TOO_BIG == iSendErr)
    {
      (void)dm_reply_send(&stReply, DM_TOO_BIG, NULL, 0u);
    }
  }
  dm_json_free_str(pcText);
  return DM_SUCCESS;
}

/* ============================ dm.diag.snapshot 辅助与实现 ============================ */

static void dm__diag_conn_one(in const dm__conn_info_st *pstInfo, in void *pvArg)
{
  dm__stat_rpc_ctx_st *pstCtx = (dm__stat_rpc_ctx_st *)pvArg;
  yyjson_mut_val *pstObj = NULL;
  const char *pcPeer = NULL;
  bool bOk = true;

  if (pstCtx->bOom)
  {
    return;
  }
  pstObj = yyjson_mut_arr_add_obj(pstCtx->pstDoc, pstCtx->pstArr);
  if (NULL == pstObj)
  {
    pstCtx->bOom = DM_TRUE;
    return;
  }
  pcPeer = pstInfo->bAuthed ? (('\0' != pstInfo->acPeerID[0]) ? pstInfo->acPeerID : "local") : "";
  bOk = bOk && yyjson_mut_obj_add_uint(pstCtx->pstDoc, pstObj, "id", pstInfo->u32Id);
  bOk = bOk && yyjson_mut_obj_add_uint(pstCtx->pstDoc, pstObj, "role", pstInfo->u32Role);
  bOk = bOk && yyjson_mut_obj_add_strcpy(pstCtx->pstDoc, pstObj, "peer_id", pcPeer);
  bOk = bOk &&
        yyjson_mut_obj_add_str(pstCtx->pstDoc, pstObj, "kind", dm__conn_kind_str(pstInfo->u8Kind));
  bOk = bOk &&
        yyjson_mut_obj_add_bool(pstCtx->pstDoc, pstObj, "authed", pstInfo->bAuthed ? true : false);
  bOk = bOk && yyjson_mut_obj_add_bool(pstCtx->pstDoc, pstObj, "push_all",
                                       pstInfo->bPushAll ? true : false);
  if (!bOk)
  {
    pstCtx->bOom = DM_TRUE;
  }
  return;
}

static void dm__diag_reply_err(in dm_link_msg_st *pstMsg, in int iStatus)
{
  dm_reply_st stReply = {0};
  if (DM_SUCCESS == dm_reply_take(pstMsg, &stReply))
  {
    (void)dm_reply_send(&stReply, iStatus, NULL, 0u);
  }
  return;
}

int dm__diag_snapshot_builtin(in void *pvArg)
{
  static const char *const apcLaneName[DM_LANE_MAX] = {"default", "db", "heavy"};
  dm_link_msg_st *pstMsg = (dm_link_msg_st *)pvArg;
  yyjson_mut_doc *pstDoc = yyjson_mut_doc_new(dm_json_alc());
  yyjson_mut_val *pstRoot = NULL;
  yyjson_mut_val *pstObj = NULL;
  yyjson_mut_val *pstLanes = NULL;
  dm__stat_rpc_ctx_st stCtx = {0};
  dm__stat_summary_st stSum = {0};
  dm_reply_st stReply = {0};
  char *pcText = NULL;
  size_t uLen = 0u;
  uint uiLane = 0u;
  bool bOk = true;

  _Static_assert(DM_LANE_MAX == 3, "update apcLaneName when DM_LANE_MAX changes");
  if (NULL == pstDoc)
  {
    dm__diag_reply_err(pstMsg, DM_ALLOC);
    return DM_SUCCESS;
  }
  dm__stat_summary(&stSum);
  pstRoot = yyjson_mut_obj(pstDoc);
  bOk = (NULL != pstRoot);
  if (bOk)
  {
    yyjson_mut_doc_set_root(pstDoc, pstRoot);
    bOk = bOk && yyjson_mut_obj_add_str(pstDoc, pstRoot, "version", dm_version_string());
    bOk = bOk && yyjson_mut_obj_add_str(pstDoc, pstRoot, "boot", dm__boot_hex());
  }

  stCtx.pstDoc = pstDoc;
  stCtx.pstArr = bOk ? yyjson_mut_arr(pstDoc) : NULL;
  stCtx.bOom = DM_FALSE;
  bOk = bOk && (NULL != stCtx.pstArr) &&
        yyjson_mut_obj_add_val(pstDoc, pstRoot, "conns", stCtx.pstArr);
  if (bOk)
  {
    dm__conn_each(dm__diag_conn_one, &stCtx);
    bOk = bOk && !stCtx.bOom;
  }

  pstObj = bOk ? yyjson_mut_obj(pstDoc) : NULL;
  bOk = bOk && (NULL != pstObj) && yyjson_mut_obj_add_val(pstDoc, pstRoot, "queue", pstObj);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "len", dm__queue_len());
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "ext_bytes", dm__queue_ext_bytes());
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "max_depth", dm__queue_max_depth());

  pstLanes = bOk ? yyjson_mut_arr(pstDoc) : NULL;
  bOk = bOk && (NULL != pstLanes) && yyjson_mut_obj_add_val(pstDoc, pstRoot, "lanes", pstLanes);
  for (uiLane = 0u; bOk && (uiLane < (uint)DM_LANE_MAX); uiLane++)
  {
    dm_pool_stat_st stLane = {0};
    yyjson_mut_val *pstLaneObj = yyjson_mut_arr_add_obj(pstDoc, pstLanes);
    if (NULL == pstLaneObj)
    {
      bOk = false;
      break;
    }
    if (DM_SUCCESS != dm_pool_stat((dm_pool_lane_et)uiLane, &stLane))
    {
      stLane.u16LiveThreads = 0u;
      stLane.u16BusyThreads = 0u;
      stLane.u32Queued = 0u;
      stLane.u64DoneCount = 0u;
      stLane.u64WaitMaxNs = 0u;
      stLane.u64RunMaxNs = 0u;
    }
    bOk = bOk && yyjson_mut_obj_add_str(pstDoc, pstLaneObj, "name", apcLaneName[uiLane]);
    bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstLaneObj, "live", stLane.u16LiveThreads);
    bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstLaneObj, "busy", stLane.u16BusyThreads);
    bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstLaneObj, "queued", stLane.u32Queued);
    bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstLaneObj, "done", stLane.u64DoneCount);
    bOk = bOk &&
          yyjson_mut_obj_add_uint(pstDoc, pstLaneObj, "wait_max_us", stLane.u64WaitMaxNs / 1000u);
    bOk = bOk &&
          yyjson_mut_obj_add_uint(pstDoc, pstLaneObj, "run_max_us", stLane.u64RunMaxNs / 1000u);
  }
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstRoot, "reaped_threads", dm_pool_reaped_threads());

  pstObj = bOk ? yyjson_mut_obj(pstDoc) : NULL;
  bOk = bOk && (NULL != pstObj) && yyjson_mut_obj_add_val(pstDoc, pstRoot, "timers", pstObj);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "active", dm__timer_count());

  pstObj = bOk ? yyjson_mut_obj(pstDoc) : NULL;
  bOk = bOk && (NULL != pstObj) && yyjson_mut_obj_add_val(pstDoc, pstRoot, "loop", pstObj);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "max_busy_us", stSum.u64LoopMaxNs / 1000u);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "slow", stSum.u32LoopSlow);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "turns", dm__stat_loop_turns());

  pstObj = bOk ? yyjson_mut_obj(pstDoc) : NULL;
  bOk = bOk && (NULL != pstObj) && yyjson_mut_obj_add_val(pstDoc, pstRoot, "push", pstObj);
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "seq", dm_push_seq());
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "ring_count", dm__push_ring_count());
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "ring_bytes", dm__push_ring_bytes());
  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstObj, "calls", dm__call_pending());

  bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstRoot, "children", dm__ext_child_count());

  if (!bOk)
  {
    yyjson_mut_doc_free(pstDoc);
    dm__diag_reply_err(pstMsg, DM_ALLOC);
    return DM_SUCCESS;
  }
  pcText = dm_json_write_mut(pstDoc, &uLen);
  yyjson_mut_doc_free(pstDoc);
  if (DM_SUCCESS == dm_reply_take(pstMsg, &stReply))
  {
    int iSendErr = dm_reply_send(&stReply, (NULL != pcText) ? DM_SUCCESS : DM_ALLOC, pcText,
                                 (NULL != pcText) ? (uint32)uLen : 0u);
    if (DM_TOO_BIG == iSendErr)
    {
      (void)dm_reply_send(&stReply, DM_TOO_BIG, NULL, 0u);
    }
  }
  dm_json_free_str(pcText);
  return DM_SUCCESS;
}

#ifdef __cplusplus
}
#endif
