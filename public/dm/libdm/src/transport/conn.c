/**
 ******************************************************************************
 * @file    conn.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   连接表管理、连接关闭、帧写入、应答与推送/反向调用连接查询（DD-01）。
 *          从 link_proto.c 分出来：二进制口、HTTP 口、WebSocket 的连接都在这一张表里。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "link"
#include <dm.h>
#include <dm/link.h>
#include <dm/rpc.h>
#include <dm/event.h>
#include <dm/log.h>
#include <dm/limits.h>

#include <stdint.h> /* SIZE_MAX */
#include <stdio.h>  /* snprintf */
#include <string.h> /* memset / memcpy / strcmp */

#include "conn.h"

dm_list_st g_stConnList;
dm_hash_table_st g_stConnTable;
uint32 g_u32ConnNext = 0;

/* ============================ 连接表与关闭 ============================ */

/* 这条连接已经在关，或者答完错误等着关 */
bool_t dm__conn_dead(dm_in const dm__conn_st *pstConn)
{
  return (pstConn->bClosing || pstConn->bDoomed) ? DM_TRUE : DM_FALSE;
}

dm__conn_st *dm__conn_find(dm_in uint32 u32Id)
{
  if (NULL == g_stConnTable.pstBuckets)
  {
    return NULL;
  }
  return (dm__conn_st *)dm_hash_find(&g_stConnTable, u32Id);
}

static void dm__conn_on_closed(dm_in dm_uv_handle_st *pstHandle)
{
  dm__conn_st *pstConn = (dm__conn_st *)pstHandle;
  dm_hash_destroy(&pstConn->stPending, NULL);
  dm__qitem_free(pstConn->pstKillItem); /* 没用上的 */
  /* AUD-06
   * 冷路径兜底：连接关闭时无论是否完成帧解析（含截断/超限/预鉴权流水线残帧），一律擦零接收缓冲 */
  if ((NULL != pstConn->pu8Rx) && (pstConn->u32RxCap > 0u))
  {
    dm_secure_zero(pstConn->pu8Rx, pstConn->u32RxCap);
  }
  dm_free_tag(pstConn->pu8Rx);
  dm_free_tag(pstConn);
  return;
}

void dm__hs_timer_closed(dm_in dm_uv_handle_st *pstHandle)
{
  dm__conn_st *pstConn = container_of(pstHandle, dm__conn_st, stHsTimer);
  /* 握手定时器关完了，才关连接本身（连接对象里含着定时器，不能先释放） */
  dm_uv_close((dm_uv_handle_st *)dm__conn_stream(pstConn), dm__conn_on_closed);
  return;
}

/* 关一条连接：摘表、摘停读、发断开事件（握过手的），再关句柄 */
void dm__conn_close(dm_io dm__conn_st *pstConn, dm_in const char *pcWhy)
{
  dm_conn_ev_st stEv = {0};

  if (pstConn->bClosing)
  {
    return;
  }
  pstConn->bClosing = DM_TRUE;
  DM_LOG_DEBUG("conn %u closed: %s", pstConn->u32Id, pcWhy);
  (void)dm_hash_del(&g_stConnTable, pstConn->u32Id, NULL);
  dm_list_del_node(&pstConn->stNode);
  dm__flow_remove(&pstConn->stFlow);
  dm__conn_notify_peer_hello(pstConn, DM_CONN_GONE, 0u, NULL, 0u);
  dm__call_conn_gone(pstConn->u32Id); /* 发给它、还在等应答的反向调用，各交一次 DM_CONN_GONE */
  if (NULL != pstConn->pvHttp)
  {
    dm__http_conn_gone(pstConn); /* 没答的 HTTP 请求各交一条 DM_SRC_HTTP_GONE */
  }
  if (NULL != pstConn->pvWs)
  {
    dm__ws_conn_gone(pstConn);
  }
  if (pstConn->bAuthed && (dm__phase() < DM__PHASE_TEARDOWN))
  {
    memset(&stEv, 0, sizeof(stEv));
    stEv.u32Role = pstConn->u32Role;
    memcpy(stEv.acPeerID, pstConn->acPeerID, sizeof(stEv.acPeerID));
    stEv.acPeerID[sizeof(stEv.acPeerID) - 1u] = '\0';
    (void)dm_event_publish(DM_EV_CONN_CLOSE, &stEv, sizeof(stEv));
  }
  (void)dm_uv_timer_stop(&pstConn->stHsTimer);
  dm_uv_close((dm_uv_handle_st *)&pstConn->stHsTimer, dm__hs_timer_closed);
  return;
}

static void dm__conn_run_close(dm_io dm__qitem_st *pstItem)
{
  dm__conn_st *pstConn = dm__conn_find((uint32)(uintptr_t)pstItem->pvCtx);
  if (NULL != pstConn)
  {
    dm__conn_close(pstConn, pstConn->pcKillWhy);
  }
  return;
}

void dm__conn_close_later(dm_io dm__conn_st *pstConn, dm_in const char *pcWhy)
{
  if (pstConn->bClosing || pstConn->bKill)
  {
    return;
  }
  pstConn->bKill = DM_TRUE;
  pstConn->bDoomed = DM_TRUE; /* 它发来的不再处理 */
  pstConn->pcKillWhy = pcWhy;
  dm__flow_stop(&pstConn->stFlow, DM__STOP_CLOSING);
  if (NULL != pstConn->pstKillItem)
  {
    dm__qitem_st *pstItem = pstConn->pstKillItem;
    pstConn->pstKillItem = NULL; /* 交给总队列了（排不进时 dm__queue_push 自己释放） */
    pstItem->pfRun = dm__conn_run_close;
    pstItem->pvCtx = (void *)(uintptr_t)pstConn->u32Id;
    (void)dm__queue_push(pstItem);
  }
  /* 排不进仅发生在拆除阶段：此处绝不当场关闭（避免破坏调用方栈上状态）。
   * 它已经停读、不再写，拆除时 dm__proto_fini 统一关 */
  return;
}

static void dm__conn_on_shutdown(dm_in dm_uv_shutdown_st *pstReq, dm_in int iStatus)
{
  dm__conn_st *pstConn = container_of(pstReq, dm__conn_st, stShutdown);
  (void)iStatus;
  dm__conn_close(pstConn, "shutdown after error");
  return;
}

/* 答完一个错误再关：先停读，等写完（shutdown）再关。停读用自己的一位，总队列降下来也不会重新开读 */
void dm__conn_close_after_write(dm_io dm__conn_st *pstConn)
{
  if (pstConn->bClosing || pstConn->bDoomed)
  {
    return;
  }
  pstConn->bDoomed = DM_TRUE;
  dm__flow_stop(&pstConn->stFlow, DM__STOP_CLOSING);
  if (DM_SUCCESS !=
      dm_uv_shutdown(&pstConn->stShutdown, dm__conn_stream(pstConn), dm__conn_on_shutdown))
  {
    dm__conn_close_later(pstConn, "shutdown failed");
  }
  return;
}

/* ============================ 发 ============================ */

typedef struct dm__conn_write
{
  dm_uv_write_st stReq;
  uint32 u32FrameLen;
  bool_t bSensitive;
  uint8 au8Frame[];
} dm__conn_write_st;

static void dm__conn_on_written(dm_in dm_uv_write_st *pstReq, dm_in int iStatus)
{
  dm__conn_write_st *pstWrite = container_of(pstReq, dm__conn_write_st, stReq);
  (void)iStatus;
  if (pstWrite->bSensitive && (pstWrite->u32FrameLen > 0u))
  {
    dm_secure_zero(pstWrite->au8Frame, pstWrite->u32FrameLen);
  }
  dm_free_tag(pstWrite);
  return;
}

/* 往一条连接写一帧（pu8Head 为 NULL 时 pvRest 就是整帧）；没写出去的超过 DM_SLOW_CONN 就断开
 * （不让一个卡住的客户端撑爆内存） */
static int dm__conn_write_io(dm_io dm__conn_st *pstConn, dm_in const uint8 *pu8Head,
                             dm_in const void *pvRest, dm_in uint32 u32RestLen,
                             dm_in bool_t bWsFrame, dm_in bool_t bFramePayload)
{
  dm__conn_write_st *pstWrite = NULL;
  uint32 u32Head = (NULL != pu8Head) ? DM_FRAME_HEAD : 0u;
  uint8 au8Ws[16] = {0};
  uint32 u32Ws = 0u;
  uint8 u8FrameFlag = 0u;
  uint64 u64Total = 0u;
  size_t uTotal = 0u;
  dm_uv_buf_st stBuf = {0};
  int iErrCode = DM_SUCCESS;

  if (pstConn->bClosing || pstConn->bKill)
  {
    return DM_CONN_GONE;
  }
  if (bWsFrame)
  {
    u32Ws = dm__ws_head(au8Ws, 0x2u, (uint64)u32Head + u32RestLen); /* 一帧装成一条二进制消息 */
  }
  u64Total = (uint64)u32Ws + (uint64)u32Head + (uint64)u32RestLen;
  uTotal = (size_t)u64Total;
  if ((u64Total > (uint64)0xFFFFFFFFu) || (uTotal > (SIZE_MAX - sizeof(dm__conn_write_st))))
  {
    return DM_TOO_BIG;
  }
  pstWrite =
      (dm__conn_write_st *)dm_malloc_tag(sizeof(dm__conn_write_st) + uTotal, DM_MEM_TAG_LINK_TX);
  if (NULL == pstWrite)
  {
    dm__conn_close_later(pstConn, "out of memory");
    return DM_ALLOC;
  }
  if (bFramePayload)
  {
    if (NULL != pu8Head)
    {
      u8FrameFlag = pu8Head[3];
    }
    else if ((NULL != pvRest) && (u32RestLen >= DM_FRAME_HEAD))
    {
      u8FrameFlag = ((const uint8 *)pvRest)[3];
    }
  }
  pstWrite->u32FrameLen = u32Ws + u32Head + u32RestLen;
  pstWrite->bSensitive = (0u != (u8FrameFlag & DM_FRAME_F_SENSITIVE)) ? DM_TRUE : DM_FALSE;
  if (0u != u32Ws)
  {
    memcpy(pstWrite->au8Frame, au8Ws, u32Ws);
  }
  if (NULL != pu8Head)
  {
    memcpy(pstWrite->au8Frame + u32Ws, pu8Head, DM_FRAME_HEAD);
  }
  if ((NULL != pvRest) && (u32RestLen > 0u))
  {
    memcpy(pstWrite->au8Frame + u32Ws + u32Head, pvRest, u32RestLen);
  }
  stBuf = dm_uv_buf_init((char *)pstWrite->au8Frame, pstWrite->u32FrameLen);
  iErrCode =
      dm_uv_write(&pstWrite->stReq, dm__conn_stream(pstConn), &stBuf, 1, dm__conn_on_written);
  if (DM_SUCCESS != iErrCode)
  {
    if (pstWrite->bSensitive && (pstWrite->u32FrameLen > 0u))
    {
      dm_secure_zero(pstWrite->au8Frame, pstWrite->u32FrameLen);
    }
    dm_free_tag(pstWrite);
    dm__conn_close_later(pstConn, "write failed"); /* 延迟关闭，避免破坏调用方栈状态 */
    return DM_CONN_GONE;
  }
  if (dm_uv_stream_get_write_queue_size(dm__conn_stream(pstConn)) > DM_SLOW_CONN)
  {
    DM_LOG_WARN("conn %u: over %u bytes not written, disconnecting slow peer", pstConn->u32Id,
                (unsigned)DM_SLOW_CONN);
    dm__conn_close_later(pstConn, "slow peer");
  }
  return DM_SUCCESS;
}

/* 往一条连接写一帧（pu8Head 为 NULL 时 pvRest 就是整帧）；WebSocket 连接自动套一层消息头。
 * 没写出去的超过 DM_SLOW_CONN 就断开（不让一个卡住的客户端撑爆内存） */
int dm__conn_write_parts(dm_io dm__conn_st *pstConn, dm_in const uint8 *pu8Head,
                         dm_in const void *pvRest, dm_in uint32 u32RestLen)
{
  return dm__conn_write_io(pstConn, pu8Head, pvRest, u32RestLen,
                           (DM__CONN_WS == pstConn->u8Kind) ? DM_TRUE : DM_FALSE, DM_TRUE);
}

/* 原样写字节（HTTP 应答、WebSocket 的控制帧） */
int dm__conn_write_raw(dm_io dm__conn_st *pstConn, dm_in const void *pv, dm_in uint32 u32Len)
{
  return dm__conn_write_io(pstConn, NULL, pv, u32Len, DM_FALSE, DM_FALSE);
}

int dm__conn_write(dm_io dm__conn_st *pstConn, dm_in const dm_frame_head_st *pstHead,
                   dm_in const void *pvBody)
{
  uint8 au8Head[DM_FRAME_HEAD] = {0};
  dm_frame_put(au8Head, pstHead);
  return dm__conn_write_parts(pstConn, au8Head, pvBody, pstHead->u32Len);
}

/* UTF-8 完整序列长度检测（返回 1..4，非法/截断字节序列返回 0）；
 * 逐字节校验续字节再读下一字节，数学上绝不越过末尾 '\0' 越界读 */
static size_t dm__utf8_seq_len(dm_in const uint8 *pu8)
{
  uint8 u8B0 = pu8[0];

  if (u8B0 < 0x80u)
  {
    return 1u;
  }
  if ((u8B0 >= 0xC2u) && (u8B0 <= 0xDFu))
  {
    uint8 u8B1 = pu8[1];
    return ((u8B1 & 0xC0u) == 0x80u) ? 2u : 0u;
  }
  if ((u8B0 >= 0xE0u) && (u8B0 <= 0xEFu))
  {
    uint8 u8B1 = pu8[1];
    uint8 u8B2 = 0u;
    if ((u8B1 & 0xC0u) != 0x80u)
    {
      return 0u;
    }
    if ((0xE0u == u8B0) && (u8B1 < 0xA0u))
    {
      return 0u;
    }
    if ((0xEDu == u8B0) && (u8B1 >= 0xA0u))
    {
      return 0u; /* 排除 UTF-16 代理项 */
    }
    u8B2 = pu8[2];
    return ((u8B2 & 0xC0u) == 0x80u) ? 3u : 0u;
  }
  if ((u8B0 >= 0xF0u) && (u8B0 <= 0xF4u))
  {
    uint8 u8B1 = pu8[1];
    uint8 u8B2 = 0u;
    uint8 u8B3 = 0u;
    if ((u8B1 & 0xC0u) != 0x80u)
    {
      return 0u;
    }
    if ((0xF0u == u8B0) && (u8B1 < 0x90u))
    {
      return 0u;
    }
    if ((0xF4u == u8B0) && (u8B1 > 0x8Fu))
    {
      return 0u;
    }
    u8B2 = pu8[2];
    if ((u8B2 & 0xC0u) != 0x80u)
    {
      return 0u;
    }
    u8B3 = pu8[3];
    return ((u8B3 & 0xC0u) == 0x80u) ? 4u : 0u;
  }
  return 0u;
}

/* 追加单个字符或完整 UTF-8 码点到 JSON 字符串；空间不足返回 0，否则返回消耗的输入字节数 */
static size_t dm__err_json_append_one(dm_in const char *pcMsg, dm_out char *pcBuf,
                                      dm_in size_t uCap, dm_io size_t *puPos)
{
  uint8 u8Ch = (uint8)*pcMsg;
  size_t uPos = *puPos;
  size_t uSeq = 0u;

  if (('"' == u8Ch) || ('\\' == u8Ch))
  {
    if (uPos + 2u + 3u > uCap)
    {
      return 0u;
    }
    pcBuf[uPos++] = '\\';
    pcBuf[uPos++] = (char)u8Ch;
    *puPos = uPos;
    return 1u;
  }
  if ((u8Ch < 0x20u) || (0x7Fu == u8Ch))
  {
    if (uPos + 6u + 3u > uCap)
    {
      return 0u;
    }
    (void)snprintf(pcBuf + uPos, uCap - uPos, "\\u%04x", (unsigned)u8Ch);
    *puPos = uPos + 6u;
    return 1u;
  }
  if (u8Ch < 0x80u)
  {
    if (uPos + 1u + 3u > uCap)
    {
      return 0u;
    }
    pcBuf[uPos++] = (char)u8Ch;
    *puPos = uPos;
    return 1u;
  }
  uSeq = dm__utf8_seq_len((const uint8 *)pcMsg);
  if (0u == uSeq)
  {
    if (uPos + 1u + 3u > uCap)
    {
      return 0u;
    }
    pcBuf[uPos++] = '?';
    *puPos = uPos;
    return 1u;
  }
  if (uPos + uSeq + 3u > uCap)
  {
    return 0u;
  }
  memcpy(pcBuf + uPos, pcMsg, uSeq);
  *puPos = uPos + uSeq;
  return uSeq;
}

/* 零分配构造标准错误信封 {"code":<iStatus>,"error":"<escaped>"}（DD-01 标准错误信封）；
 * 完整 UTF-8 码点原子拷贝不截断半个字符，控制字符与 DEL(0x7F) 均转义 */
static size_t dm__err_json_fmt(dm_in int iStatus, dm_out char *pcBuf, dm_in size_t uCap)
{
  const char *pcMsg = dm_errno_str(iStatus);
  size_t uPos = 0u;
  int iHead = 0;

  if ((NULL == pcBuf) || (uCap < 32u))
  {
    return 0u;
  }
  if (NULL == pcMsg)
  {
    pcMsg = "unknown";
  }
  iHead = snprintf(pcBuf, uCap, "{\"code\":%d,\"error\":\"", iStatus);
  if ((iHead < 0) || ((size_t)iHead + 3u > uCap))
  {
    return 0u;
  }
  uPos = (size_t)iHead;
  while ('\0' != *pcMsg)
  {
    size_t uStep = dm__err_json_append_one(pcMsg, pcBuf, uCap, &uPos);
    if (0u == uStep)
    {
      break;
    }
    pcMsg += uStep;
  }
  pcBuf[uPos++] = '"';
  pcBuf[uPos++] = '}';
  pcBuf[uPos] = '\0';
  return uPos;
}

int dm__conn_reply_ex(dm_io dm__conn_st *pstConn, dm_in uint uiLinkID, dm_in uint32 u32ReqID,
                      dm_in int iStatus, dm_in uint8 u8ExtraFlag, dm_in const void *pvBody,
                      dm_in uint32 u32Len)
{
  dm_frame_head_st stHead = {0};
  char acErr[256] = {0};

  if ((DM_SUCCESS != iStatus) && ((NULL == pvBody) || (0u == u32Len)))
  {
    uint32 u32Limit = pstConn->bAuthed ? dm__link_frame_max() : DM_PRE_AUTH_MAX;
    size_t uCap =
        ((uint64)sizeof(acErr) <= (uint64)u32Limit + 1u) ? sizeof(acErr) : ((size_t)u32Limit + 1u);
    size_t uErrLen = dm__err_json_fmt(iStatus, acErr, uCap);
    if (uErrLen > 0u)
    {
      pvBody = acErr;
      u32Len = (uint32)uErrLen;
    }
  }
  memset(&stHead, 0, sizeof(stHead));
  stHead.u16LinkID = (uint16)uiLinkID;
  stHead.u8Type = DM_FRAME_T_REP;
  stHead.u8Flag =
      (uint8)(((u32Len > 0u) ? DM_FRAME_F_JSON : 0u) | (u8ExtraFlag & DM_FRAME_F_SENSITIVE));
  stHead.u32ReqID = u32ReqID;
  stHead.i32Status = (int32)iStatus;
  stHead.u32Len = u32Len;
  return dm__conn_write(pstConn, &stHead, pvBody);
}

int dm__conn_reply(dm_io dm__conn_st *pstConn, dm_in uint uiLinkID, dm_in uint32 u32ReqID,
                   dm_in int iStatus, dm_in const void *pvBody, dm_in uint32 u32Len)
{
  return dm__conn_reply_ex(pstConn, uiLinkID, u32ReqID, iStatus, 0u, pvBody, u32Len);
}

/* rpc.c 用：答一个请求。只能答一次（没挂着的回 DM_EXISTS）；敏感请求自动透传 DM_FRAME_F_SENSITIVE
 */
int dm__conn_answer_ex(dm_in uint32 u32ConnID, dm_in uint uiLinkID, dm_in uint32 u32ReqID,
                       dm_in int iStatus, dm_in uint8 u8ExtraFlag, dm_in const void *pvBody,
                       dm_in uint32 u32Len)
{
  dm__conn_st *pstConn = dm__conn_find(u32ConnID);

  if ((NULL == pstConn) || dm__conn_dead(pstConn))
  {
    DM_LOG_DEBUG("reply to conn %u req %u: connection is gone", u32ConnID, u32ReqID);
    return DM_CONN_GONE;
  }
  if (DM_SUCCESS != dm_hash_del(&pstConn->stPending, u32ReqID, NULL))
  {
    return DM_EXISTS;
  }
  return dm__conn_reply_ex(pstConn, uiLinkID, u32ReqID, iStatus, u8ExtraFlag, pvBody, u32Len);
}

int dm__conn_answer(dm_in uint32 u32ConnID, dm_in uint uiLinkID, dm_in uint32 u32ReqID,
                    dm_in int iStatus, dm_in const void *pvBody, dm_in uint32 u32Len)
{
  return dm__conn_answer_ex(u32ConnID, uiLinkID, u32ReqID, iStatus, 0u, pvBody, u32Len);
}

uint32 dm__conn_count(void)
{
  return (NULL != g_stConnTable.pstBuckets) ? g_stConnTable.count : 0u;
}

/* 连接的角色位（rpc.c 的 dm.iface.list 用） */
uint32 dm__conn_role(dm_in uint32 u32ConnID)
{
  dm__conn_st *pstConn = dm__conn_find(u32ConnID);
  return (NULL != pstConn) ? pstConn->u32Role : 0u;
}

/* ============================ 推送、反向调用用（push.c、call.c） ============================ */

static bool_t dm__conn_wants(dm_in const dm__conn_st *pstConn, dm_in uint uiTopic)
{
  uint uiIdx = 0;
  if (!pstConn->bAuthed || dm__conn_dead(pstConn))
  {
    return DM_FALSE;
  }
  if (pstConn->bPushAll)
  {
    return DM_TRUE;
  }
  for (uiIdx = 0; uiIdx < pstConn->u8SubCount; uiIdx++)
  {
    if (pstConn->au16Sub[uiIdx] == (uint16)uiTopic)
    {
      return DM_TRUE;
    }
  }
  return DM_FALSE;
}

/* 发给所有收这一类的连接。流式的：积压超过 DM_STREAM_HIGH 的这一拍不给它发（丢掉、不补） */
void dm__conn_push_fanout(dm_in uint uiTopic, dm_in const uint8 *pu8Frame, dm_in uint32 u32Len,
                          dm_in bool_t bStream)
{
  dm_list_node_st *pstNode = NULL;
  dm_list_node_st *pstNext = NULL;

  if (NULL == g_stConnList.stHead.pstNext)
  {
    return;
  }
  DM_LIST_FOREACH_SAFE(&g_stConnList, pstNode, pstNext)
  {
    dm__conn_st *pstConn = DM_LIST_ENTRY(pstNode, dm__conn_st, stNode);
    if (!dm__conn_wants(pstConn, uiTopic))
    {
      continue;
    }
    if (bStream && (dm_uv_stream_get_write_queue_size(dm__conn_stream(pstConn)) > DM_STREAM_HIGH))
    {
      continue;
    }
    (void)dm__conn_write_parts(pstConn, NULL, pu8Frame, u32Len); /* 慢的会在这里被断开、摘表 */
  }
  return;
}

/* 按 peer_id 单播推送：不进广播重放环，仅发给 peer_id 匹配且订阅该类的已握手活跃连接 */
int dm__conn_push_peer(dm_in const char *pcPeerID, dm_in uint uiTopic, dm_in const uint8 *pu8Frame,
                       dm_in uint32 u32Len)
{
  dm_list_node_st *pstNode = NULL;
  dm_list_node_st *pstNext = NULL;
  bool_t bFoundPeer = DM_FALSE;
  bool_t bSentAny = DM_FALSE;
  int iLastErr = DM_SUCCESS;

  if ((NULL == pcPeerID) || ('\0' == pcPeerID[0]) || (NULL == g_stConnList.stHead.pstNext))
  {
    return DM_CONN_GONE;
  }
  DM_LIST_FOREACH_SAFE(&g_stConnList, pstNode, pstNext)
  {
    dm__conn_st *pstConn = DM_LIST_ENTRY(pstNode, dm__conn_st, stNode);
    if (!pstConn->bAuthed || dm__conn_dead(pstConn))
    {
      continue;
    }
    if (0 != strcmp(pstConn->acPeerID, pcPeerID))
    {
      continue;
    }
    bFoundPeer = DM_TRUE;
    if (!dm__conn_wants(pstConn, uiTopic))
    {
      continue;
    }
    iLastErr = dm__conn_write_parts(pstConn, NULL, pu8Frame, u32Len);
    if (DM_SUCCESS == iLastErr)
    {
      bSentAny = DM_TRUE;
    }
  }
  if (!bFoundPeer)
  {
    return DM_CONN_GONE;
  }
  return bSentAny ? DM_SUCCESS : iLastErr;
}

/* 补发一条给某条连接（它要收这一类才发） */
int dm__conn_push_one(dm_in uint32 u32ConnID, dm_in uint uiTopic, dm_in const uint8 *pu8Frame,
                      dm_in uint32 u32Len)
{
  dm__conn_st *pstConn = dm__conn_find(u32ConnID);
  if ((NULL == pstConn) || dm__conn_dead(pstConn))
  {
    return DM_CONN_GONE;
  }
  if (!dm__conn_wants(pstConn, uiTopic))
  {
    return DM_SUCCESS;
  }
  return dm__conn_write_parts(pstConn, NULL, pu8Frame, u32Len);
}

/* 反向调用：往一条连接发一帧 */
int dm__conn_send(dm_in uint32 u32ConnID, dm_in const dm_frame_head_st *pstHead,
                  dm_in const void *pvBody)
{
  dm__conn_st *pstConn = dm__conn_find(u32ConnID);
  if ((NULL == pstConn) || dm__conn_dead(pstConn))
  {
    return DM_CONN_GONE;
  }
  return dm__conn_write(pstConn, pstHead, pvBody);
}

/* 反向调用找对方：最早连上、握过手、带这个角色位（且 pcPeerID 非 NULL 时匹配 peer_id）；没有返回 0
 */
uint32 dm__conn_pick_peer(dm_in const char *pcPeerID, dm_in uint32 u32Role)
{
  dm_list_node_st *pstNode = NULL;

  if ((NULL == g_stConnList.stHead.pstNext) || ((NULL != pcPeerID) && ('\0' == pcPeerID[0])))
  {
    return 0u;
  }
  DM_LIST_FOREACH(&g_stConnList, pstNode)
  {
    dm__conn_st *pstConn = DM_LIST_ENTRY(pstNode, dm__conn_st, stNode);
    if (!pstConn->bAuthed || dm__conn_dead(pstConn) || (0u == (pstConn->u32Role & u32Role)))
    {
      continue;
    }
    if ((NULL == pcPeerID) || (0 == strcmp(pstConn->acPeerID, pcPeerID)))
    {
      return pstConn->u32Id;
    }
  }
  return 0u;
}

uint32 dm__conn_pick(dm_in uint32 u32Role)
{
  return dm__conn_pick_peer(NULL, u32Role);
}

const char *dm__conn_kind_str(dm_in uint8 u8Kind)
{
  switch ((dm__conn_kind_et)u8Kind)
  {
  case DM__CONN_BIN:
    return "bin";
  case DM__CONN_HTTP:
    return "http";
  case DM__CONN_WS:
    return "ws";
  default:
    return "unknown";
  }
}

bool_t dm__conn_peer_exists(dm_in const char *pcPeerID)
{
  dm_list_node_st *pstNode = NULL;

  if ((NULL == pcPeerID) || ('\0' == pcPeerID[0]) || (NULL == g_stConnList.stHead.pstNext))
  {
    return DM_FALSE;
  }
  DM_LIST_FOREACH(&g_stConnList, pstNode)
  {
    const dm__conn_st *pstConn = DM_LIST_ENTRY(pstNode, dm__conn_st, stNode);
    if (pstConn->bAuthed && !dm__conn_dead(pstConn) && (0 == strcmp(pstConn->acPeerID, pcPeerID)))
    {
      return DM_TRUE;
    }
  }
  return DM_FALSE;
}

/* 遍历活跃连接表（dm.diag.snapshot 诊断快照用） */
void dm__conn_each(dm_in dm__conn_info_cb pfEach, dm_in void *pvArg)
{
  dm_list_node_st *pstNode = NULL;

  _Static_assert(sizeof(((dm__conn_info_st *)0)->acPeerID) == DM_PEER_ID_MAX,
                 "dm__conn_info_st.acPeerID must match DM_PEER_ID_MAX");
  if ((NULL == pfEach) || (NULL == g_stConnList.stHead.pstNext))
  {
    return;
  }
  DM_LIST_FOREACH(&g_stConnList, pstNode)
  {
    const dm__conn_st *pstConn = DM_LIST_ENTRY(pstNode, dm__conn_st, stNode);
    dm__conn_info_st stInfo = {0};
    if (pstConn->bClosing)
    {
      continue;
    }
    stInfo.u32Id = pstConn->u32Id;
    stInfo.u32Role = pstConn->u32Role;
    stInfo.u8Kind = pstConn->u8Kind;
    stInfo.bAuthed = pstConn->bAuthed;
    stInfo.bPushAll = pstConn->bPushAll;
    memcpy(stInfo.acPeerID, pstConn->acPeerID, sizeof(stInfo.acPeerID));
    stInfo.acPeerID[sizeof(stInfo.acPeerID) - 1u] = '\0';
    pfEach(&stInfo, pvArg);
  }
  return;
}

/* 握手里的 push 数组解析：["*"] = 全收；["类名", …] = 只收这几类（不认识的名字忽略） */
void dm__conn_parse_push_arr(dm_io dm__conn_st *pstConn, dm_in yyjson_val *pstArr)
{
  yyjson_val *pstOne = NULL;
  yyjson_arr_iter stIter = {0};

  pstConn->bPushAll = DM_FALSE;
  pstConn->u8SubCount = 0;
  if (!yyjson_is_arr(pstArr))
  {
    return;
  }
  yyjson_arr_iter_init(pstArr, &stIter);
  while (NULL != (pstOne = yyjson_arr_iter_next(&stIter)))
  {
    const char *pcName = yyjson_get_str(pstOne);
    int iTopic = 0;
    if (NULL == pcName)
    {
      continue;
    }
    if (0 == strcmp(pcName, "*"))
    {
      pstConn->bPushAll = DM_TRUE;
      continue;
    }
    iTopic = dm__push_topic_id(pcName);
    if ((iTopic >= 0) && (pstConn->u8SubCount < DM__SUB_MAX))
    {
      pstConn->au16Sub[pstConn->u8SubCount++] = (uint16)iTopic;
    }
  }
  return;
}

/* 握手里的 push：不写 = 不收；["*"] = 全收；["类名", …] = 只收这几类（不认识的名字忽略） */
void dm__conn_parse_push(dm_io dm__conn_st *pstConn, dm_in yyjson_val *pstRoot)
{
  yyjson_val *pstArr = (NULL != pstRoot) ? yyjson_obj_get(pstRoot, "push") : NULL;
  dm__conn_parse_push_arr(pstConn, pstArr);
  return;
}

/* 查找尚未 bAuthed 的主动出站 Peer 连接（仅供 dm_link_peer_send_hello / mark_ready 使用） */
dm__conn_st *dm__conn_find_peer_preauth(dm_in const char *pcPeerID, dm_in uint64 u64ConnGen)
{
  dm_list_node_st *pstNode = NULL;

  if ((NULL == pcPeerID) || ('\0' == pcPeerID[0]) || (NULL == g_stConnList.stHead.pstNext))
  {
    return NULL;
  }
  DM_LIST_FOREACH(&g_stConnList, pstNode)
  {
    dm__conn_st *pstConn = DM_LIST_ENTRY(pstNode, dm__conn_st, stNode);
    if (pstConn->bPeerAdopted && pstConn->bPeerInitiator && !pstConn->bAuthed &&
        !dm__conn_dead(pstConn) && (pstConn->u64ConnGen == u64ConnGen) &&
        (0 == strcmp(pstConn->acPeerID, pcPeerID)))
    {
      return pstConn;
    }
  }
  return NULL;
}

/* 主动出站 Peer HELLO 应答/超时/断连唯一一次回投通知（严格标记敏感载荷） */
void dm__conn_notify_peer_hello(dm_io dm__conn_st *pstConn, dm_in int iStatus, dm_in uint8 u8Flags,
                                dm_in const uint8 *pu8Body, dm_in uint32 u32Len)
{
  dm_link_msg_st stMsg = {0};
  uint uiReply = 0u;

  if ((NULL == pstConn) || !pstConn->bPeerHelloSent)
  {
    return;
  }
  pstConn->bPeerHelloSent = DM_FALSE;
  uiReply = pstConn->uiPeerHelloReply;
  if (dm__phase() >= DM__PHASE_TEARDOWN)
  {
    return;
  }
  stMsg.pu8Data = (uint8 *)pu8Body;
  stMsg.u32Len = u32Len;
  stMsg.u8Src = DM_SRC_REPLY;
  stMsg.u8Flags = (uint8)(u8Flags | DM_FRAME_F_SENSITIVE);
  stMsg.u64Tag = pstConn->u64PeerHelloTag;
  stMsg.iStatus = iStatus;
  (void)dm__link_enqueue(uiReply, &stMsg, (DM_SUCCESS == iStatus) ? DM_TRUE : DM_FALSE, 0u, 0u);
  return;
}

/* 主动断开指定 peer_id 且角色匹配 u32RoleMask 的所有活跃连接（返回断开条数；仅主线程） */
uint32 dm_link_disconnect_peer(dm_in const char *pcPeerID, dm_in uint32 u32RoleMask)
{
  dm_list_node_st *pstNode = NULL;
  dm_list_node_st *pstNext = NULL;
  uint32 u32Closed = 0u;

  if ((NULL == pcPeerID) || ('\0' == pcPeerID[0]) || (0u == u32RoleMask) ||
      (NULL == g_stConnList.stHead.pstNext))
  {
    return 0u;
  }
  DM_LIST_FOREACH_SAFE(&g_stConnList, pstNode, pstNext)
  {
    dm__conn_st *pstConn = DM_LIST_ENTRY(pstNode, dm__conn_st, stNode);
    if (!pstConn->bAuthed || dm__conn_dead(pstConn))
    {
      continue;
    }
    if ((0u == (pstConn->u32Role & u32RoleMask)) || (0 != strcmp(pstConn->acPeerID, pcPeerID)))
    {
      continue;
    }
    dm__conn_close_later(pstConn, "peer revoked");
    u32Closed++;
  }
  return u32Closed;
}

/* 查询消息来源连接已认证的 peer_id（连接有效且已认证返回 DM_SUCCESS，否则返回 DM_NOT_FOUND） */
int dm_link_msg_peer_id(dm_in const dm_link_msg_st *pstMsg, dm_out char *pcBuf, dm_in size_t uCap)
{
  const dm__reply_ctx_st *pstCtx = NULL;
  const dm__conn_st *pstConn = NULL;
  size_t uLen = 0u;

  if ((NULL == pstMsg) || (NULL == pcBuf) || (0u == uCap))
  {
    return DM_INVALID;
  }
  pcBuf[0] = '\0';
  pstCtx = (const dm__reply_ctx_st *)pstMsg->pvPriv;
  if ((NULL == pstCtx) || (0u == pstCtx->u32ConnID))
  {
    return DM_NOT_FOUND;
  }
  pstConn = dm__conn_find(pstCtx->u32ConnID);
  if ((NULL == pstConn) || !pstConn->bAuthed || dm__conn_dead(pstConn) ||
      ('\0' == pstConn->acPeerID[0]))
  {
    return DM_NOT_FOUND;
  }
  uLen = strlen(pstConn->acPeerID);
  if (uLen >= uCap)
  {
    return DM_TOO_BIG;
  }
  memcpy(pcBuf, pstConn->acPeerID, uLen + 1u);
  return DM_SUCCESS;
}

#ifdef __cplusplus
}
#endif
