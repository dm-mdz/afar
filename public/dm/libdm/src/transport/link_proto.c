/**
 ******************************************************************************
 * @file    link_proto.c
 * @author  MDM
 * @version 0.0.0.2
 * @brief   二进制传输口（DD-01）：监听 127.0.0.1、端口文件、连接表、16 字节帧头、握手、
 *          连接建立 / 断开事件、慢连接。
 *
 * 连接收在这里，业务不碰：每条连接一个 32 位编号 connID（从 1 递增、进程内不重用），
 * 编号到连接对象查表；connID 只在 DM 里面用、不上线。业务应答走 dm_reply_send（rpc.c），
 * 拿着凭据里的 connID 来这里查；连接已经断了就回 DM_CONN_GONE。
 * 不管：接口表和谁能调（rpc.c）、总队列（queue.c）。
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 MDM.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
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
#include <dm/module.h>
#include <dm/event.h>
#include <dm/json.h>
#include <dm/hash.h>
#include <dm/list.h>
#include <dm/log.h>
#include <dm/limits.h>

#include <stdio.h>  /* snprintf */
#include <string.h> /* memset / memmove / memcpy / strcmp */

#include "../dm_internal.h"
#include "conn.h"

#ifndef DM__TOKEN_STEP
#define DM__TOKEN_STEP() /* 白盒测试数口令比较走了几步（T-LINK-13）；平时是空的 */
#endif

static dm_tcp_st *g_pstServer =
    NULL; /* 二进制口的监听句柄：每次绑定新分配一个（没绑上的那个关完才释放） */
static int g_iBoundPort = 0;
static dm_tcp_st *g_pstHttpServer = NULL; /* HTTP 监听句柄 */
static int g_iHttpPort = 0;

/* 由 dm_main_ex 交过来的配置（dm__link_config） */
static const char *g_pcDataDir = NULL;
static const dm_role_st *g_pastRole = NULL;
static uint g_uiRoleCount = 0;
static uint32 g_u32FrameMax = DM_FRAME_MAX_DEFAULT;

/* 测试用固定端口：通过 dm_set_listen_port 显式指定（端口选择第 1 优先级） */
static bool_t g_bPortForced = DM_FALSE;
static int g_iPortForced = 0;
static bool_t g_bDisableLocalHttp = DM_FALSE;

void dm__link_config(in const dm_cfg_st *pstCfg)
{
  uint uiIdx = 0;

  g_pcDataDir = (NULL != pstCfg) ? pstCfg->pcDataDir : NULL;
  g_pastRole = (NULL != pstCfg) ? pstCfg->pastRole : NULL;
  g_uiRoleCount = (NULL != pstCfg) ? pstCfg->uiRoleCount : 0u;
  g_u32FrameMax = ((NULL != pstCfg) && (0u != pstCfg->u32FrameMax)) ? pstCfg->u32FrameMax
                                                                    : DM_FRAME_MAX_DEFAULT;
  g_bDisableLocalHttp = (NULL != pstCfg) ? pstCfg->bDisableLocalHttp : DM_FALSE;
  g_iHttpPort = 0;
  dm__http_config(pstCfg);
  for (uiIdx = 0; (NULL != g_pastRole) && (uiIdx < g_uiRoleCount); uiIdx++)
  {
    if ((NULL == g_pastRole[uiIdx].pcToken) || ('\0' == g_pastRole[uiIdx].pcToken[0]))
    {
      DM_LOG_ERROR("role '%s' has no token: every handshake as it will be refused",
                   (NULL != g_pastRole[uiIdx].pcName) ? g_pastRole[uiIdx].pcName : "?");
    }
  }
  return;
}

void dm_set_listen_port(in uint16 u16Port)
{
  g_bPortForced = DM_TRUE;
  g_iPortForced = (int)u16Port;
  return;
}

int dm_get_listen_port(void)
{
  return g_iBoundPort;
}

int dm__link_port(void)
{
  return g_iBoundPort;
}

/* ============================ 握手 ============================ */

/* 口令按字节比较，不提前退出（防按时间猜） */
static bool_t dm__token_equal(in const char *pcA, in const char *pcB)
{
  size_t uLenA = (NULL != pcA) ? strlen(pcA) : 0u;
  size_t uLenB = (NULL != pcB) ? strlen(pcB) : 0u;
  size_t uIdx = 0;
  uint8 u8Diff = (uLenA == uLenB) ? 0u : 1u;
  for (uIdx = 0; uIdx < uLenB; uIdx++)
  {
    DM__TOKEN_STEP();
    u8Diff |= (uint8)(((uIdx < uLenA) ? (uint8)pcA[uIdx] : 0u) ^ (uint8)pcB[uIdx]);
  }
  return (0u == u8Diff) ? DM_TRUE : DM_FALSE;
}

/* 校验 peer_id：非空、长度 < DM_PEER_ID_MAX、仅允许可见 ASCII（0x21..0x7E，无空白/控制/非 ASCII）
 */
static bool_t dm__peer_id_valid(in const yyjson_val *pstVal, out const char **ppcOut,
                                out size_t *puLenOut)
{
  const char *pcStr = NULL;
  size_t uLen = 0u;
  size_t uIdx = 0u;

  if (!yyjson_is_str(pstVal))
  {
    return DM_FALSE;
  }
  pcStr = yyjson_get_str(pstVal);
  uLen = yyjson_get_len(pstVal);
  if ((NULL == pcStr) || (0u == uLen) || (uLen >= DM_PEER_ID_MAX))
  {
    return DM_FALSE;
  }
  for (uIdx = 0u; uIdx < uLen; uIdx++)
  {
    uint8 u8Ch = (uint8)pcStr[uIdx];
    if ((u8Ch <= 0x20u) || (u8Ch >= 0x7Fu))
    {
      return DM_FALSE;
    }
  }
  *ppcOut = pcStr;
  *puLenOut = uLen;
  return DM_TRUE;
}

static void dm__conn_authed(io dm__conn_st *pstConn, in uint32 u32Role)
{
  dm_conn_ev_st stEv = {0};

  pstConn->bAuthed = DM_TRUE;
  pstConn->u32Role = u32Role;
  if ('\0' == pstConn->acPeerID[0])
  {
    memcpy(pstConn->acPeerID, "local", 6u);
  }
  (void)dm_timer_stop(&pstConn->stHsTimer);
  memset(&stEv, 0, sizeof(stEv));
  stEv.u32Role = u32Role;
  memcpy(stEv.acPeerID, pstConn->acPeerID, sizeof(stEv.acPeerID));
  stEv.acPeerID[sizeof(stEv.acPeerID) - 1u] = '\0';
  (void)dm_event_publish(DM_EV_CONN_OPEN, &stEv, sizeof(stEv));
  return;
}

/* 握手在总队列里做（排在就绪事件后面：「就绪先于连接」） */
static void dm__run_hello(io dm__qitem_st *pstItem)
{
  dm__conn_st *pstConn = dm__conn_find(pstItem->stReply.u32ConnID);
  yyjson_doc *pstDoc = NULL;
  yyjson_val *pstRoot = NULL;
  yyjson_val *pstPeerVal = NULL;
  const char *pcRole = NULL;
  const char *pcToken = NULL;
  const char *pcBoot = NULL;
  const char *pcPeer = NULL;
  const char *pcResume = NULL;
  size_t uPeerLen = 0u;
  uint64 u64Seen = 0;
  const dm_role_st *pstRole = NULL;
  char acReply[160] = {0};
  uint uiIdx = 0;
  int iLen = 0;

  if ((NULL == pstConn) || dm__conn_dead(pstConn))
  {
    return; /* 比如：同一条连接一次连发几个握手，前一个被拒了，后面排着的不再比口令 */
  }
  if (NULL != pstItem->pstBuf)
  {
    pstDoc = dm_json_read_sensitive((const char *)pstItem->pstBuf->au8Data, pstItem->pstBuf->u32Len,
                                    DM_PRE_AUTH_MAX, NULL);
  }
  pstRoot = (NULL != pstDoc) ? yyjson_doc_get_root(pstDoc) : NULL;
  pcRole = dm_json_str(pstRoot, "/role", "");
  pcToken = dm_json_str(pstRoot, "/token", "");
  pcBoot = dm_json_str(pstRoot, "/boot", NULL);
  pstPeerVal = (NULL != pstRoot) ? yyjson_obj_get(pstRoot, "peer_id") : NULL;

  if (!pstConn->bAuthed)
  {
    for (uiIdx = 0; uiIdx < g_uiRoleCount; uiIdx++)
    {
      if ((NULL != g_pastRole[uiIdx].pcName) && (0 == strcmp(g_pastRole[uiIdx].pcName, pcRole)))
      {
        pstRole = &g_pastRole[uiIdx];
        break;
      }
    }
    /* 应用漏填了口令的角色一律拒绝（起来时已经记过 ERROR），不能让「不带口令」也通过 */
    if ((NULL == pstRole) || (NULL == pstRole->pcToken) || ('\0' == pstRole->pcToken[0]) ||
        !dm__token_equal(pcToken, pstRole->pcToken))
    {
      DM_LOG_WARN("conn %u: handshake refused for role '%s'", pstConn->u32Id, pcRole);
      yyjson_doc_free(pstDoc);
      (void)dm__conn_reply(pstConn, DM_LINKID_HELLO, pstItem->stReply.u32ReqID, DM_PERM, NULL, 0);
      dm__conn_close_after_write(pstConn);
      return;
    }
    if (NULL != pstPeerVal)
    {
      if (!dm__peer_id_valid(pstPeerVal, &pcPeer, &uPeerLen))
      {
        DM_LOG_WARN("conn %u: handshake refused due to invalid peer_id", pstConn->u32Id);
        yyjson_doc_free(pstDoc);
        (void)dm__conn_reply(pstConn, DM_LINKID_HELLO, pstItem->stReply.u32ReqID, DM_INVALID, NULL,
                             0);
        dm__conn_close_after_write(pstConn);
        return;
      }
      /* 非默认 "local" 的具名 peer_id 在活跃连接中保持唯一，防止跨角色/同角色冒充窃听或抢占 */
      if ((0 != strcmp(pcPeer, "local")) && dm__conn_peer_exists(pcPeer))
      {
        DM_LOG_WARN("conn %u: handshake refused due to duplicate active peer_id '%s'",
                    pstConn->u32Id, pcPeer);
        yyjson_doc_free(pstDoc);
        (void)dm__conn_reply(pstConn, DM_LINKID_HELLO, pstItem->stReply.u32ReqID, DM_EXISTS, NULL,
                             0);
        dm__conn_close_after_write(pstConn);
        return;
      }
      memcpy(pstConn->acPeerID, pcPeer, uPeerLen);
      pstConn->acPeerID[uPeerLen] = '\0';
    }
    else
    {
      memcpy(pstConn->acPeerID, "local", 6u);
    }
  }
  else if (NULL != pstPeerVal)
  {
    if (!dm__peer_id_valid(pstPeerVal, &pcPeer, &uPeerLen) ||
        (0 != strcmp(pcPeer, pstConn->acPeerID)))
    {
      DM_LOG_WARN("conn %u: re-hello refused due to peer_id mismatch", pstConn->u32Id);
      yyjson_doc_free(pstDoc);
      (void)dm__conn_reply(pstConn, DM_LINKID_HELLO, pstItem->stReply.u32ReqID, DM_INVALID, NULL,
                           0);
      return;
    }
  }
  /* 推送续传（DD-01）：可续传时返回 ok，并在握手应答后立即补发 */
  pcResume =
      dm__push_resume(pcBoot, yyjson_is_uint(yyjson_obj_get(pstRoot, "seq")) ? DM_TRUE : DM_FALSE,
                      (uint64)dm_json_int(pstRoot, "/seq", 0));
  u64Seen = (uint64)dm_json_int(pstRoot, "/seq", 0);
  dm__conn_parse_push(pstConn, pstRoot);
  iLen = snprintf(acReply, sizeof(acReply), "{\"boot\":\"%s\",\"seq\":%llu,\"resume\":\"%s\"}",
                  dm__boot_hex(), (unsigned long long)dm_push_seq(), pcResume);
  yyjson_doc_free(pstDoc);
  if (!pstConn->bAuthed)
  {
    dm__conn_authed(pstConn, pstRole->u32Bit);
  }
  (void)dm__conn_reply(pstConn, DM_LINKID_HELLO, pstItem->stReply.u32ReqID, DM_SUCCESS, acReply,
                       (uint32)iLen);
  if (0 == strcmp(pcResume, "ok"))
  {
    dm__push_replay(pstConn->u32Id, u64Seen);
  }
  return;
}

static void dm__on_hs_timeout(in dm_timer_st *pstTimer)
{
  dm__conn_st *pstConn = container_of(pstTimer, dm__conn_st, stHsTimer);
  DM_LOG_WARN("conn %u: no handshake within %u ms", pstConn->u32Id, (unsigned)DM_HANDSHAKE_MS);
  dm__conn_close(pstConn, "handshake timeout");
  return;
}

/* ============================ 收 ============================ */

/* 处理一帧；返回非 0 表示这条连接已经开始关，别再往下读 */
int dm__conn_on_frame(io dm__conn_st *pstConn, in const dm_frame_head_st *pstHead,
                      in uint8 *pu8Body)
{
  dm_link_msg_st stMsg = {0};
  dm__qitem_st *pstItem = NULL;
  int iErrCode = DM_SUCCESS;

  /* AUD-06：无论帧类型（含 REP/握手前异常帧），一旦命中敏感标志或 HELLO 立刻标记连接 */
  if ((0u != (pstHead->u8Flag & DM_FRAME_F_SENSITIVE)) || (DM_LINKID_HELLO == pstHead->u16LinkID))
  {
    pstConn->bHadSensitive = DM_TRUE;
  }

  /* 握手前仅接受握手请求；其余任意帧一律回 DM_NOT_READY 并断开 */
  if (!pstConn->bAuthed &&
      ((DM_FRAME_T_REQ != pstHead->u8Type) || (DM_LINKID_HELLO != pstHead->u16LinkID)))
  {
    (void)dm__conn_reply(pstConn, pstHead->u16LinkID, pstHead->u32ReqID, DM_NOT_READY, NULL, 0);
    dm__conn_close_after_write(pstConn);
    return 1;
  }
  if (DM_FRAME_T_REQ != pstHead->u8Type)
  {
    if (DM_FRAME_T_REP == pstHead->u8Type)
    {
      if (!dm__call_reply(pstConn->u32Id, pstHead, pu8Body))
      {
        DM_LOG_DEBUG("conn %u: reply frame with no call waiting, dropped", pstConn->u32Id);
      }
      return 0;
    }
    DM_LOG_WARN("conn %u: frame type %u not allowed, disconnecting", pstConn->u32Id,
                (unsigned)pstHead->u8Type);
    dm__conn_close(pstConn, "bad frame type");
    return 1;
  }

  /* 握手：进总队列（排在就绪事件后面），强制按敏感载荷处理避免口令残留 */
  if (DM_LINKID_HELLO == pstHead->u16LinkID)
  {
    pstConn->bHadSensitive = DM_TRUE;
    pstItem = dm__qitem_new();
    if (NULL == pstItem)
    {
      return 0;
    }
    pstItem->pfRun = dm__run_hello;
    pstItem->bExternal = DM_TRUE;
    pstItem->u8Flags = (uint8)(pstHead->u8Flag | DM_FRAME_F_SENSITIVE);
    pstItem->stReply.u32ConnID = pstConn->u32Id;
    pstItem->stReply.u32ReqID = pstHead->u32ReqID;
    pstItem->pstBuf = dm__qbuf_new_ex(pu8Body, pstHead->u32Len, DM_FRAME_F_SENSITIVE);
    (void)dm__queue_push(pstItem);
    return 0;
  }
  if (0u != (pstHead->u8Flag & DM_FRAME_F_SENSITIVE))
  {
    pstConn->bHadSensitive = DM_TRUE;
  }
  if (dm__phase() >= DM__PHASE_DRAIN)
  {
    (void)dm__conn_reply(pstConn, pstHead->u16LinkID, pstHead->u32ReqID, DM_SHUTTING_DOWN, NULL, 0);
    return 0;
  }
  iErrCode = dm__iface_allow(pstHead->u16LinkID, pstConn->u32Role);
  if (DM_SUCCESS == iErrCode)
  {
    memset(&stMsg, 0, sizeof(stMsg));
    stMsg.pu8Data = pu8Body;
    stMsg.u32Len = pstHead->u32Len;
    stMsg.u8Src = (DM__CONN_WS == pstConn->u8Kind) ? DM_SRC_WS : DM_SRC_LINK;
    stMsg.u8Flags = pstHead->u8Flag;
    stMsg.u32Role = pstConn->u32Role;
    if (0u != pstHead->u32ReqID)
    {
      if (NULL != dm_hash_find(&pstConn->stPending, pstHead->u32ReqID))
      {
        iErrCode = DM_EXISTS; /* 同一个请求号还挂着：对方编号乱了 */
      }
      else if (DM_SUCCESS != dm_hash_add(&pstConn->stPending, pstHead->u32ReqID, pstConn))
      {
        iErrCode = DM_ALLOC;
      }
    }
    if (DM_SUCCESS == iErrCode)
    {
      iErrCode =
          dm__link_enqueue(pstHead->u16LinkID, &stMsg, DM_TRUE, pstConn->u32Id, pstHead->u32ReqID);
      if ((DM_SUCCESS != iErrCode) && (0u != pstHead->u32ReqID))
      {
        (void)dm_hash_del(&pstConn->stPending, pstHead->u32ReqID, NULL);
      }
    }
  }
  if ((DM_SUCCESS != iErrCode) && (0u != pstHead->u32ReqID))
  {
    (void)dm__conn_reply(pstConn, pstHead->u16LinkID, pstHead->u32ReqID, iErrCode, NULL, 0);
  }
  return 0;
}

static void dm__conn_on_alloc(in dm_handle_st *pstHandle, in size_t uSuggested,
                              out dm_buf_st *pstBuf)
{
  dm__conn_st *pstConn = (dm__conn_st *)pstHandle;
  (void)uSuggested;
  pstBuf->base = (char *)(pstConn->pu8Rx + pstConn->u32RxLen);
  pstBuf->len = pstConn->u32RxCap - pstConn->u32RxLen;
  return;
}

/* 接收缓冲长到能装下 u32Need 字节（不超过单条上限 + 帧头） */
static int dm__conn_rx_grow(io dm__conn_st *pstConn, in uint32 u32Need)
{
  uint8 *pu8New = NULL;
  if (u32Need <= pstConn->u32RxCap)
  {
    return DM_SUCCESS;
  }
  if (pstConn->bHadSensitive && (NULL != pstConn->pu8Rx))
  {
    pu8New = (uint8 *)dm_malloc_tag(u32Need, DM_MEM_TAG_LINK_RX);
    if (NULL == pu8New)
    {
      return DM_ALLOC;
    }
    if (pstConn->u32RxLen > 0u)
    {
      memcpy(pu8New, pstConn->pu8Rx, pstConn->u32RxLen);
    }
    dm_secure_zero(pstConn->pu8Rx, pstConn->u32RxCap);
    dm_free_tag(pstConn->pu8Rx);
  }
  else
  {
    pu8New = (uint8 *)dm_realloc_tag(pstConn->pu8Rx, u32Need);
    if (NULL == pu8New)
    {
      return DM_ALLOC;
    }
  }
  pstConn->pu8Rx = pu8New;
  pstConn->u32RxCap = u32Need;
  return DM_SUCCESS;
}

static void dm__conn_rx_shrink(io dm__conn_st *pstConn)
{
  uint8 *pu8Small = NULL;

  if (pstConn->bHadSensitive)
  {
    pu8Small = (uint8 *)dm_malloc_tag(DM__RX_KEEP, DM_MEM_TAG_LINK_RX);
    if (NULL == pu8Small)
    {
      return;
    }
    if (pstConn->u32RxLen > 0u)
    {
      memcpy(pu8Small, pstConn->pu8Rx, pstConn->u32RxLen);
    }
    dm_secure_zero(pstConn->pu8Rx, pstConn->u32RxCap);
    dm_free_tag(pstConn->pu8Rx);
  }
  else
  {
    pu8Small = (uint8 *)dm_realloc_tag(pstConn->pu8Rx, DM__RX_KEEP);
    if (NULL == pu8Small)
    {
      return;
    }
  }
  pstConn->pu8Rx = pu8Small;
  pstConn->u32RxCap = DM__RX_KEEP;
  return;
}

static void dm__conn_on_read(in dm_stream_st *pstStream, in ssize_t iNRead,
                             in const dm_buf_st *pstBuf)
{
  dm__conn_st *pstConn = (dm__conn_st *)pstStream;
  dm_frame_head_st stHead = {0};
  uint32 u32Off = 0;
  uint32 u32Max = 0;
  (void)pstBuf;

  if (iNRead < 0)
  {
    dm__conn_close(pstConn, (DM_EOF == iNRead) ? "closed by peer" : "read error");
    return;
  }
  if (dm__conn_dead(pstConn))
  {
    pstConn->u32RxLen = 0; /* 等着关的连接，再来的都扔掉 */
    return;
  }
  if (DM__CONN_BIN != pstConn->u8Kind)
  {
    /* HTTP、WebSocket：读进来的交给它们自己的解析（各自攒自己的），接收缓冲每次从头用 */
    pstConn->u32RxLen = 0;
    if ((iNRead > 0) && ((uint32)iNRead > pstConn->u32RxHigh))
    {
      pstConn->u32RxHigh = (uint32)iNRead;
    }
    if (DM__CONN_HTTP == pstConn->u8Kind)
    {
      dm__http_conn_data(pstConn, pstConn->pu8Rx, (uint32)iNRead);
    }
    else
    {
      dm__ws_conn_data(pstConn, pstConn->pu8Rx, (uint32)iNRead);
    }
    if (pstConn->bHadSensitive && (pstConn->u32RxHigh > 0u))
    {
      uint32 u32Wipe =
          (pstConn->u32RxHigh <= pstConn->u32RxCap) ? pstConn->u32RxHigh : pstConn->u32RxCap;
      dm_secure_zero(pstConn->pu8Rx, u32Wipe);
      pstConn->u32RxHigh = 0u;
    }
    return;
  }
  pstConn->u32RxLen += (uint32)iNRead;
  while ((pstConn->u32RxLen - u32Off >= DM_FRAME_HEAD) && !dm__conn_dead(pstConn))
  {
    bool_t bSens = DM_FALSE;
    int iClose = 0;
    dm_frame_get(pstConn->pu8Rx + u32Off, &stHead);
    bSens =
        ((0u != (stHead.u8Flag & DM_FRAME_F_SENSITIVE)) || (DM_LINKID_HELLO == stHead.u16LinkID))
            ? DM_TRUE
            : DM_FALSE;
    if (bSens)
    {
      pstConn->bHadSensitive = DM_TRUE;
    }
    u32Max = pstConn->bAuthed ? g_u32FrameMax : DM_PRE_AUTH_MAX;
    if (stHead.u32Len > u32Max)
    {
      DM_LOG_WARN("conn %u: frame of %u bytes is over the %u limit, disconnecting", pstConn->u32Id,
                  (unsigned)stHead.u32Len, (unsigned)u32Max);
      dm__conn_close(pstConn, "frame too big");
      return;
    }
    if (pstConn->u32RxLen - u32Off < DM_FRAME_HEAD + stHead.u32Len)
    {
      break; /* 半帧：等下一次读 */
    }
    iClose = dm__conn_on_frame(pstConn, &stHead, pstConn->pu8Rx + u32Off + DM_FRAME_HEAD);
    if (bSens)
    {
      dm_secure_zero(pstConn->pu8Rx + u32Off, DM_FRAME_HEAD + stHead.u32Len);
    }
    if (0 != iClose)
    {
      return;
    }
    u32Off += DM_FRAME_HEAD + stHead.u32Len;
  }
  if (dm__conn_dead(pstConn))
  {
    return;
  }
  /* 没收全的挪回缓冲头；一帧比缓冲大就长缓冲，处理完了缩回 64KB */
  pstConn->u32RxLen -= u32Off;
  memmove(pstConn->pu8Rx, pstConn->pu8Rx + u32Off, pstConn->u32RxLen);
  if (pstConn->bHadSensitive && (u32Off > 0u))
  {
    dm_secure_zero(pstConn->pu8Rx + pstConn->u32RxLen, u32Off);
  }
  if (pstConn->u32RxLen >= DM_FRAME_HEAD)
  {
    dm_frame_get(pstConn->pu8Rx, &stHead);
    if (DM_SUCCESS != dm__conn_rx_grow(pstConn, DM_FRAME_HEAD + stHead.u32Len))
    {
      dm__conn_close(pstConn, "out of memory");
    }
  }
  else if (pstConn->u32RxCap > DM__RX_KEEP)
  {
    dm__conn_rx_shrink(pstConn);
  }
  return;
}

/* ============================ 接入 ============================ */

/* 接一条连接进连接表；失败返回 NULL（已经收拾干净） */
static dm__conn_st *dm__conn_accept(in dm_stream_st *pstServer, in uint8 u8Kind)
{
  dm__conn_st *pstConn =
      (dm__conn_st *)dm_calloc_tag(1u, sizeof(dm__conn_st), DM_MEM_TAG_LINK_CONN);

  if (NULL == pstConn)
  {
    return NULL;
  }
  pstConn->u8Kind = u8Kind;
  pstConn->pu8Rx = (uint8 *)dm_malloc_tag(DM__RX_KEEP, DM_MEM_TAG_LINK_RX);
  pstConn->pstKillItem = dm__qitem_new(); /* 关连接时一定排得进总队列（不再当场关） */
  if ((NULL == pstConn->pu8Rx) || (NULL == pstConn->pstKillItem) ||
      (DM_SUCCESS != dm_hash_init(&pstConn->stPending, DM__PEND_BUCKETS, NULL)))
  {
    dm__qitem_free(pstConn->pstKillItem);
    dm_free_tag(pstConn->pu8Rx);
    dm_free_tag(pstConn);
    return NULL;
  }
  pstConn->u32RxCap = DM__RX_KEEP;
  (void)dm_tcp_init(dm_default_loop(), &pstConn->stTcp);
  (void)dm_timer_init(dm_default_loop(), &pstConn->stHsTimer);
  g_u32ConnNext++;
  pstConn->u32Id = g_u32ConnNext;
  dm_list_add_tail(&g_stConnList, &pstConn->stNode);
  if ((DM_SUCCESS != dm_accept(pstServer, (dm_stream_st *)&pstConn->stTcp)) ||
      (DM_SUCCESS != dm_hash_add(&g_stConnTable, pstConn->u32Id, pstConn)))
  {
    dm_list_del_node(&pstConn->stNode);
    pstConn->bClosing = DM_TRUE;
    dm_close((dm_handle_st *)&pstConn->stHsTimer, dm__hs_timer_closed);
    return NULL;
  }
  (void)dm_tcp_nodelay(&pstConn->stTcp, 1);
  if (DM_SUCCESS != dm__flow_start(&pstConn->stFlow, (dm_stream_st *)&pstConn->stTcp,
                                   dm__conn_on_alloc, dm__conn_on_read))
  {
    dm__conn_close(pstConn, "read start failed");
    return NULL;
  }
  return pstConn;
}

/* 开始走 link 的规矩（二进制口一连上；WebSocket 升级以后）：要握手的起握手定时器 */
void dm__conn_start_link(io dm__conn_st *pstConn)
{
  if (NULL == g_pastRole)
  {
    dm__conn_authed(pstConn, DM_ROLE_ALL); /* 不要求握手（DM 自己的测试用）：连接带全部的位 */
    return;
  }
  (void)dm_timer_start(&pstConn->stHsTimer, dm__on_hs_timeout, DM_HANDSHAKE_MS, 0);
  return;
}

uint32 dm__link_frame_max(void)
{
  return g_u32FrameMax;
}

static void dm__on_new_connection(in dm_stream_st *pstServer, in int iStatus)
{
  dm__conn_st *pstConn = NULL;
  if (DM_SUCCESS != iStatus)
  {
    return;
  }
  pstConn = dm__conn_accept(pstServer, DM__CONN_BIN);
  if (NULL != pstConn)
  {
    dm__conn_start_link(pstConn);
  }
  return;
}

static void dm__on_new_http(in dm_stream_st *pstServer, in int iStatus)
{
  dm__conn_st *pstConn = NULL;
  if (DM_SUCCESS != iStatus)
  {
    return;
  }
  pstConn = dm__conn_accept(pstServer, DM__CONN_HTTP);
  if (NULL != pstConn)
  {
    dm__http_conn_new(pstConn);
  }
  return;
}

/* ============================ 端口 ============================ */

static void dm__server_on_closed(in dm_handle_st *pstHandle)
{
  dm_free_tag(pstHandle);
  return;
}

static void dm__server_close(io dm_tcp_st **ppstServer)
{
  if (NULL != *ppstServer)
  {
    dm_close((dm_handle_st *)*ppstServer, dm__server_on_closed);
    *ppstServer = NULL;
  }
  return;
}

/* 绑定并监听 127.0.0.1:iPort；返回 DM_SUCCESS 或错误码（端口被占要到 listen 才报） */
static int dm__listen_on(out dm_tcp_st **ppstServer, in int iPort, in dm_connection_cb pfOnNew,
                         out int *piBound)
{
  struct sockaddr_in stAddr = {0};
  struct sockaddr_storage stName = {0};
  int iNameLen = (int)sizeof(stName);
  int iErrCode = DM_SUCCESS;

  iErrCode = dm_ip4_addr("127.0.0.1", iPort, &stAddr);
  if (DM_SUCCESS != iErrCode)
  {
    return iErrCode;
  }
  *ppstServer = (dm_tcp_st *)dm_malloc_tag(sizeof(dm_tcp_st), DM_MEM_TAG_LINK_CONN);
  if (NULL == *ppstServer)
  {
    return DM_ALLOC;
  }
  iErrCode = dm_tcp_init(dm_default_loop(), *ppstServer);
  if (DM_SUCCESS != iErrCode)
  {
    dm_free_tag(*ppstServer);
    *ppstServer = NULL;
    return iErrCode;
  }
  iErrCode = dm_tcp_bind(*ppstServer, (const struct sockaddr *)&stAddr, 0);
  if (DM_SUCCESS == iErrCode)
  {
    iErrCode = dm_listen((dm_stream_st *)*ppstServer, 128, pfOnNew);
  }
  if (DM_SUCCESS == iErrCode)
  {
    iErrCode = dm_tcp_getsockname(*ppstServer, (struct sockaddr *)&stName, &iNameLen);
  }
  if (DM_SUCCESS != iErrCode)
  {
    dm__server_close(ppstServer);
    return iErrCode;
  }
  *piBound = (int)ntohs(((struct sockaddr_in *)&stName)->sin_port);
  return DM_SUCCESS;
}

/* 先试 iLast（上次记下的，0 = 没有），被占了记 WARN 再让系统挑 */
static int dm__listen_pick(out dm_tcp_st **ppstServer, in int iLast, in dm_connection_cb pfOnNew,
                           out int *piBound, in const char *pcWhat)
{
  int iErrCode = (0 != iLast) ? dm__listen_on(ppstServer, iLast, pfOnNew, piBound) : DM_EADDRINUSE;
  if (DM_SUCCESS != iErrCode)
  {
    if (0 != iLast)
    {
      DM_LOG_WARN("%s port %d from dm.port is taken (%s), picking another", pcWhat, iLast,
                  dm_errno_str(iErrCode));
    }
    iErrCode = dm__listen_on(ppstServer, 0, pfOnNew, piBound);
  }
  return iErrCode;
}

int dm__proto_init(void)
{
  int iErrCode = DM_SUCCESS;
  int iLastHttp = 0;
  int iLast = 0;

  dm_list_init(&g_stConnList);
  if ((NULL == g_stConnTable.pstBuckets) &&
      (DM_SUCCESS != dm_hash_init(&g_stConnTable, DM__CONN_BUCKETS, NULL)))
  {
    return DM_ALLOC;
  }
  /* 二进制口：① 测试指定的；② 上次记下的（被占了才重挑）；③ 让系统挑。HTTP 口：② ③ */
  iLast = dm__port_file_read(g_pcDataDir, &iLastHttp);
  if (g_bPortForced)
  {
    iErrCode = dm__listen_on(&g_pstServer, g_iPortForced, dm__on_new_connection, &g_iBoundPort);
  }
  else
  {
    iErrCode = dm__listen_pick(&g_pstServer, iLast, dm__on_new_connection, &g_iBoundPort, "link");
  }
  if ((DM_SUCCESS == iErrCode) && (!g_bDisableLocalHttp))
  {
    iErrCode = dm__listen_pick(&g_pstHttpServer, iLastHttp, dm__on_new_http, &g_iHttpPort, "http");
  }
  if (DM_SUCCESS != iErrCode)
  {
    DM_LOG_ERROR("listen on 127.0.0.1 failed: %s", dm_errno_str(iErrCode));
    dm__server_close(&g_pstServer);
    dm__server_close(&g_pstHttpServer);
    dm_hash_destroy(&g_stConnTable, NULL); /* 口没起来，dm__proto_fini 不会被调 */
    memset(&g_stConnTable, 0, sizeof(g_stConnTable));
    return iErrCode;
  }
  if (!g_bPortForced)
  {
    dm__port_file_write(g_pcDataDir, g_iBoundPort, g_iHttpPort);
  }
  DM_LOG_INFO("listening on 127.0.0.1:%d (link) and 127.0.0.1:%d (http)", g_iBoundPort,
              g_iHttpPort);
  return DM_SUCCESS;
}

int dm__http_port(void)
{
  return g_iHttpPort;
}

/* 收尾：不再接新连接（关两个监听），已经连着的照常 */
void dm__link_stop_accept(void)
{
  dm__server_close(&g_pstServer);
  dm__server_close(&g_pstHttpServer);
  return;
}

/* 关监听和所有连接 */
void dm__proto_fini(void)
{
  dm_list_node_st *pstNode = NULL;
  dm_list_node_st *pstNext = NULL;

  dm__link_stop_accept();
  if (NULL != g_stConnList.stHead.pstNext)
  {
    DM_LIST_FOREACH_SAFE(&g_stConnList, pstNode, pstNext)
    {
      dm__conn_close(DM_LIST_ENTRY(pstNode, dm__conn_st, stNode), "link closing");
    }
  }
  dm_hash_destroy(&g_stConnTable, NULL);
  memset(&g_stConnTable, 0, sizeof(g_stConnTable));
  return;
}

#ifdef __cplusplus
}
#endif
