/**
 ******************************************************************************
 * @file    call.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   反向调用 dm_call（DD-01）：核心向客户端发起请求并回收应答或超时。
 *
 * 按角色找对方：发给最早连上、带这个角色位的那条连接。应答、超时、连接断开，三样只交一次，
 * 都进总队列交给 uiReplyLinkID（u8Src = DM_SRC_REPLY，u64Tag 照给的，iStatus 是结果）。
 * 当场就发不出去的用返回值报，不再另交。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "call"
#include <dm.h>
#include <dm/rpc.h>
#include <dm/frame.h>
#include <dm/hash.h>
#include <dm/log.h>

#include <string.h> /* memset */

#include "../dm_internal.h"
#include "../transport/conn.h" /* dm__link_frame_max */

#define DM__CALL_BUCKETS 32u

typedef struct dm__call
{
  dm_uv_timer_st stTimer; /* 首成员：超时 */
  uint32 u32CallID;       /* 帧里的请求号（核心这一头从 1 往上编） */
  uint32 u32ConnID;
  uint uiReplyLinkID;
  uint64 u64Tag;
  bool_t bTimer;
} dm__call_st;

static dm_hash_table_st g_stCallTable; /* 请求号 -> dm__call_st */
static uint32 g_u32CallNext = 0;

/* ============================ 内部清理与完成 ============================ */

static void dm__call_on_closed(dm_in dm_uv_handle_st *pstHandle)
{
  dm_free_tag(pstHandle);
  return;
}

/* 摘表、关定时器、释放（定时器关完才释放） */
static void dm__call_free(dm_io dm__call_st *pstCall)
{
  (void)dm_hash_del(&g_stCallTable, pstCall->u32CallID, NULL);
  if (pstCall->bTimer)
  {
    (void)dm_uv_timer_stop(&pstCall->stTimer);
    dm_uv_close((dm_uv_handle_st *)&pstCall->stTimer, dm__call_on_closed);
    return;
  }
  dm_free_tag(pstCall);
  return;
}

/* 把结果交给应答处理项（进总队列），然后放掉这次调用 */
static void dm__call_finish(dm_io dm__call_st *pstCall, dm_in int iStatus,
                            dm_in const uint8 *pu8Body, dm_in uint32 u32Len, dm_in uint8 u8Flags,
                            dm_in bool_t bExternal)
{
  dm_link_msg_st stMsg = {0};

  stMsg.pu8Data = (uint8 *)pu8Body;
  stMsg.u32Len = u32Len;
  stMsg.u8Src = DM_SRC_REPLY;
  stMsg.u8Flags = u8Flags;
  stMsg.u64Tag = pstCall->u64Tag;
  stMsg.iStatus = iStatus;
  if (dm__phase() < DM__PHASE_TEARDOWN)
  {
    (void)dm__link_enqueue(pstCall->uiReplyLinkID, &stMsg, bExternal, 0u, 0u);
  }
  dm__call_free(pstCall);
  return;
}

static void dm__call_on_timeout(dm_in dm_uv_timer_st *pstTimer)
{
  dm__call_st *pstCall = (dm__call_st *)pstTimer;
  DM_LOG_DEBUG("call %u to conn %u timed out", pstCall->u32CallID, pstCall->u32ConnID);
  dm__call_finish(pstCall, DM_TIMEOUT, NULL, 0u, 0u, DM_FALSE);
  return;
}

/* ============================ 反向调用接口 ============================ */

int dm_call_peer_ex(dm_in const char *pcPeerID, dm_in uint8 u8ExtraFlags,
                    dm_in const dm_call_req_st *pstReq)
{
  dm__call_st *pstCall = NULL;
  dm_frame_head_st stHead = {0};
  uint32 u32ConnID = 0;
  int iErrCode = DM_SUCCESS;

  DM__CHECK_LOOP();
  if ((NULL == pstReq) || ((NULL != pcPeerID) && ('\0' == pcPeerID[0])) ||
      ((NULL == pstReq->pvBody) && (pstReq->u32Len > 0u)) || (pstReq->uiFunc > 0xFFFFu))
  {
    return DM_INVALID;
  }
  if (pstReq->u32Len > dm__link_frame_max())
  {
    return DM_TOO_BIG; /* 超出单帧上限 */
  }
  if (dm__phase() >= DM__PHASE_TEARDOWN)
  {
    return DM_SHUTTING_DOWN;
  }
  if (NULL == dm__link_lookup(pstReq->uiReplyLinkID))
  {
    return DM_LINK_ERR_NOT_FOUND;
  }
  u32ConnID = dm__conn_pick_peer(pcPeerID, pstReq->u32Role);
  if (0u == u32ConnID)
  {
    return DM_CONN_GONE;
  }
  if ((NULL == g_stCallTable.pstBuckets) &&
      (DM_SUCCESS != dm_hash_init(&g_stCallTable, DM__CALL_BUCKETS, NULL)))
  {
    return DM_ALLOC;
  }
  pstCall = (dm__call_st *)dm_calloc_tag(1u, sizeof(dm__call_st), DM_MEM_TAG_RPC);
  if (NULL == pstCall)
  {
    return DM_ALLOC;
  }
  g_u32CallNext++;
  if (0u == g_u32CallNext)
  {
    g_u32CallNext = 1u; /* 0 留给「不要应答」 */
  }
  pstCall->u32CallID = g_u32CallNext;
  pstCall->u32ConnID = u32ConnID;
  pstCall->uiReplyLinkID = pstReq->uiReplyLinkID;
  pstCall->u64Tag = pstReq->u64Tag;
  if (DM_SUCCESS != dm_hash_add(&g_stCallTable, pstCall->u32CallID, pstCall))
  {
    dm_free_tag(pstCall);
    return DM_ALLOC;
  }
  memset(&stHead, 0, sizeof(stHead));
  stHead.u16LinkID = (uint16)pstReq->uiFunc;
  stHead.u8Type = DM_FRAME_T_REQ;
  stHead.u8Flag = (uint8)(((pstReq->u32Len > 0u) ? DM_FRAME_F_JSON : 0u) |
                          (u8ExtraFlags & DM_FRAME_F_SENSITIVE));
  stHead.u32ReqID = pstCall->u32CallID;
  stHead.u32Len = pstReq->u32Len;
  iErrCode = dm__conn_send(u32ConnID, &stHead, pstReq->pvBody);
  if (DM_SUCCESS != iErrCode)
  {
    dm__call_free(pstCall);
    return iErrCode;
  }
  if (0u != pstReq->u32TimeoutMs)
  {
    (void)dm_uv_timer_init(dm_uv_default_loop(), &pstCall->stTimer);
    pstCall->bTimer = DM_TRUE;
    (void)dm_uv_timer_start(&pstCall->stTimer, dm__call_on_timeout, pstReq->u32TimeoutMs, 0);
  }
  return DM_SUCCESS;
}

int dm_call_peer(dm_in const char *pcPeerID, dm_in const dm_call_req_st *pstReq)
{
  if (NULL == pstReq)
  {
    return DM_INVALID;
  }
  return dm_call_peer_ex(pcPeerID, pstReq->u8ExtraFlags, pstReq);
}

int dm_call(dm_in const dm_call_req_st *pstReq)
{
  if (NULL == pstReq)
  {
    return DM_INVALID;
  }
  return dm_call_peer_ex(pstReq->pcPeerID, pstReq->u8ExtraFlags, pstReq);
}

/* 连接上来一条应答帧：是不是在等的。是就交出去，返回 DM_TRUE */
bool_t dm__call_reply(dm_in uint32 u32ConnID, dm_in const dm_frame_head_st *pstHead,
                      dm_in const uint8 *pu8Body)
{
  dm__call_st *pstCall = NULL;

  if (NULL == g_stCallTable.pstBuckets)
  {
    return DM_FALSE;
  }
  pstCall = (dm__call_st *)dm_hash_find(&g_stCallTable, pstHead->u32ReqID);
  if ((NULL == pstCall) || (pstCall->u32ConnID != u32ConnID))
  {
    return DM_FALSE; /* 超时以后才到的、或者不是发给这条连接的：丢掉 */
  }
  dm__call_finish(pstCall, (int)pstHead->i32Status, pu8Body, pstHead->u32Len, pstHead->u8Flag,
                  DM_TRUE);
  return DM_TRUE;
}

/* 一条连接断了：发给它、还在等的，各交一次 DM_CONN_GONE */
void dm__call_conn_gone(dm_in uint32 u32ConnID)
{
  uint32 u32Idx = 0;
  dm_list_node_st *pstNode = NULL;
  dm_list_node_st *pstNext = NULL;

  if ((NULL == g_stCallTable.pstBuckets) || (0u == g_stCallTable.count))
  {
    return;
  }
  for (u32Idx = 0; u32Idx < g_stCallTable.size; u32Idx++)
  {
    DM_LIST_FOREACH_SAFE(&g_stCallTable.pstBuckets[u32Idx], pstNode, pstNext)
    {
      dm__call_st *pstCall = (dm__call_st *)DM_HASH_NODE_ENTRY(pstNode)->value;
      if (pstCall->u32ConnID == u32ConnID)
      {
        dm__call_finish(pstCall, DM_CONN_GONE, NULL, 0u, 0u, DM_FALSE);
      }
    }
  }
  return;
}

uint32 dm__call_pending(void)
{
  return (NULL != g_stCallTable.pstBuckets) ? g_stCallTable.count : 0u;
}

void dm__call_set_next_req_id(dm_in uint32 u32Next)
{
  g_u32CallNext = u32Next;
  return;
}

void dm__call_destroy(void)
{
  uint32 u32Idx = 0;
  dm_list_node_st *pstNode = NULL;
  dm_list_node_st *pstNext = NULL;

  if (NULL == g_stCallTable.pstBuckets)
  {
    return;
  }
  for (u32Idx = 0; u32Idx < g_stCallTable.size; u32Idx++)
  {
    DM_LIST_FOREACH_SAFE(&g_stCallTable.pstBuckets[u32Idx], pstNode, pstNext)
    {
      dm__call_free((dm__call_st *)DM_HASH_NODE_ENTRY(pstNode)->value);
    }
  }
  dm_hash_destroy(&g_stCallTable, NULL);
  memset(&g_stCallTable, 0, sizeof(g_stCallTable));
  g_u32CallNext = 0;
  return;
}

#ifdef __cplusplus
}
#endif
