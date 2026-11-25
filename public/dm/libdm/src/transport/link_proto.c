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

static dm_uv_tcp_st *g_pstServer =
    NULL; /* 二进制口的监听句柄：每次绑定新分配一个（没绑上的那个关完才释放） */
static int g_iBoundPort = 0;
static dm_uv_tcp_st *g_pstHttpServer = NULL; /* HTTP 监听句柄 */
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

/* 跨节点 Peer 与移动端验签/入站推送回调（由业务层在 init 期注册，libdm 零业务词） */
static dm_peer_hello_verify_cb g_pfPeerHelloVerify = NULL;
static void *g_pvPeerHelloCtx = NULL;
static dm_peer_push_cb g_pfPeerPushCb = NULL;
static void *g_pvPeerPushCtx = NULL;
static dm_mobile_hello_verify_cb g_pfMobileHelloVerify = NULL;
static void *g_pvMobileHelloCtx = NULL;
static uint32 g_u32PeerHelloSeq = 0u;

void dm__link_config(dm_in const dm_cfg_st *pstCfg)
{
  uint uiIdx = 0;

  g_pcDataDir = (NULL != pstCfg) ? pstCfg->pcDataDir : NULL;
  g_pastRole = (NULL != pstCfg) ? pstCfg->pastRole : NULL;
  g_uiRoleCount = (NULL != pstCfg) ? pstCfg->uiRoleCount : 0u;
  g_u32FrameMax = ((NULL != pstCfg) && (0u != pstCfg->u32FrameMax)) ? pstCfg->u32FrameMax
                                                                    : DM_FRAME_MAX_DEFAULT;
  g_bDisableLocalHttp = (NULL != pstCfg) ? pstCfg->bDisableLocalHttp : DM_FALSE;
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

void dm_set_listen_port(dm_in uint16 u16Port)
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
static bool_t dm__token_equal(dm_in const char *pcA, dm_in const char *pcB)
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

/* 校验 C 字符串形式的 peer_id：非空、长度 < DM_PEER_ID_MAX、仅允许可见 ASCII（0x21..0x7E） */
static bool_t dm__peer_id_str_valid(dm_in const char *pcStr, dm_out size_t *puLenOut)
{
  size_t uLen = 0u;
  size_t uIdx = 0u;

  if (NULL == pcStr)
  {
    return DM_FALSE;
  }
  uLen = strlen(pcStr);
  if ((0u == uLen) || (uLen >= DM_PEER_ID_MAX))
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
  if (NULL != puLenOut)
  {
    *puLenOut = uLen;
  }
  return DM_TRUE;
}

/* 校验 peer_id：非空、长度 < DM_PEER_ID_MAX、仅允许可见 ASCII（0x21..0x7E，无空白/控制/非 ASCII）
 */
static bool_t dm__peer_id_valid(dm_in const yyjson_val *pstVal, dm_out const char **ppcOut,
                                dm_out size_t *puLenOut)
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

/* 检查并关闭同 peer_id 的旧代数连接；若存在同代或更新代数的活跃连接返回 DM_EXISTS */
static int dm__conn_check_gen_conflict(dm_in const char *pcPeerID, dm_in uint64 u64ConnGen,
                                       dm_in bool_t bMobile, dm_in const dm__conn_st *pstSkip)
{
  dm_list_node_st *pstNode = NULL;

  if ((NULL == pcPeerID) || ('\0' == pcPeerID[0]) || (NULL == g_stConnList.stHead.pstNext))
  {
    return DM_SUCCESS;
  }
  DM_LIST_FOREACH(&g_stConnList, pstNode)
  {
    const dm__conn_st *pstOther = DM_LIST_ENTRY(pstNode, dm__conn_st, stNode);
    bool_t bSameKind = bMobile ? pstOther->bMobileAdopted : pstOther->bPeerAdopted;
    if ((pstOther == pstSkip) || dm__conn_dead(pstOther) ||
        (pstOther->bMobileAdopted && !pstOther->bAuthed))
    {
      continue;
    }
    if (0 != strcmp(pstOther->acPeerID, pcPeerID))
    {
      continue;
    }
    if (!bSameKind || (u64ConnGen <= pstOther->u64ConnGen))
    {
      return DM_EXISTS;
    }
  }
  return DM_SUCCESS;
}

static void dm__conn_close_superseded(dm_in const char *pcPeerID, dm_in uint64 u64ConnGen,
                                      dm_in bool_t bMobile, dm_in const dm__conn_st *pstKeep)
{
  dm_list_node_st *pstNode = NULL;
  dm_list_node_st *pstNext = NULL;

  if ((NULL == pcPeerID) || ('\0' == pcPeerID[0]) || (NULL == g_stConnList.stHead.pstNext))
  {
    return;
  }
  DM_LIST_FOREACH_SAFE(&g_stConnList, pstNode, pstNext)
  {
    dm__conn_st *pstOther = DM_LIST_ENTRY(pstNode, dm__conn_st, stNode);
    bool_t bSameKind = bMobile ? pstOther->bMobileAdopted : pstOther->bPeerAdopted;
    if ((pstOther == pstKeep) || dm__conn_dead(pstOther) || !bSameKind ||
        (pstOther->bMobileAdopted && !pstOther->bAuthed))
    {
      continue;
    }
    if ((0 == strcmp(pstOther->acPeerID, pcPeerID)) && (pstOther->u64ConnGen < u64ConnGen))
    {
      dm__conn_close(pstOther, "superseded by newer conn_gen");
    }
  }
  return;
}

static void dm__conn_authed(dm_io dm__conn_st *pstConn, dm_in uint32 u32Role)
{
  dm_conn_ev_st stEv = {0};

  pstConn->bAuthed = DM_TRUE;
  pstConn->u32Role = u32Role;
  if ('\0' == pstConn->acPeerID[0])
  {
    memcpy(pstConn->acPeerID, "local", 6u);
  }
  (void)dm_uv_timer_stop(&pstConn->stHsTimer);
  memset(&stEv, 0, sizeof(stEv));
  stEv.u32Role = u32Role;
  memcpy(stEv.acPeerID, pstConn->acPeerID, sizeof(stEv.acPeerID));
  stEv.acPeerID[sizeof(stEv.acPeerID) - 1u] = '\0';
  (void)dm_event_publish(DM_EV_CONN_OPEN, &stEv, sizeof(stEv));
  return;
}

static void dm__run_hello_peer(dm_io dm__conn_st *pstConn, dm_in const dm__qitem_st *pstItem,
                               dm_in yyjson_val *pstRoot)
{
  const uint8 *pu8Raw = (NULL != pstItem->pstBuf) ? pstItem->pstBuf->au8Data : NULL;
  uint32 u32RawLen = (NULL != pstItem->pstBuf) ? pstItem->pstBuf->u32Len : 0u;
  char *pcAckJson = NULL;
  const char *pcSend = NULL;
  uint32 u32AckLen = 0u;
  int iVerifyErr = DM_PERM;

  if (NULL == g_pfPeerHelloVerify)
  {
    DM_LOG_WARN("conn %u: peer hello verifier not registered", pstConn->u32Id);
    (void)dm__conn_reply(pstConn, DM_LINKID_HELLO, pstItem->stReply.u32ReqID, DM_PERM, NULL, 0);
    dm__conn_close_after_write(pstConn);
    return;
  }
  iVerifyErr =
      g_pfPeerHelloVerify(pstConn->acPeerID, pu8Raw, u32RawLen, g_pvPeerHelloCtx, &pcAckJson);
  if (DM_SUCCESS != iVerifyErr)
  {
    if (NULL != pcAckJson)
    {
      dm_secure_zero(pcAckJson, strlen(pcAckJson));
      dm_free_tag(pcAckJson);
    }
    DM_LOG_WARN("conn %u: peer hello rejected (%d)", pstConn->u32Id, iVerifyErr);
    (void)dm__conn_reply(pstConn, DM_LINKID_HELLO, pstItem->stReply.u32ReqID, iVerifyErr, NULL, 0);
    dm__conn_close_after_write(pstConn);
    return;
  }
  dm__conn_parse_push(pstConn, pstRoot);
  dm__conn_authed(pstConn, DM_ROLE_PEER);
  pcSend = (NULL != pcAckJson) ? pcAckJson : "{}";
  u32AckLen = (uint32)strlen(pcSend);
  (void)dm__conn_reply_ex(pstConn, DM_LINKID_HELLO, pstItem->stReply.u32ReqID, DM_SUCCESS,
                          DM_FRAME_F_SENSITIVE, pcSend, u32AckLen);
  if (NULL != pcAckJson)
  {
    dm_secure_zero(pcAckJson, u32AckLen);
    dm_free_tag(pcAckJson);
  }
  return;
}

static void dm__run_hello_mobile(dm_io dm__conn_st *pstConn, dm_in const dm__qitem_st *pstItem,
                                 dm_in yyjson_val *pstRoot, dm_in const char *pcBoot)
{
  dm_mobile_hello_in_st stIn = {0};
  dm_mobile_hello_out_st stOut = {0};
  const char *pcResume = NULL;
  uint64 u64Seen = 0u;
  size_t uPeerLen = 0u;
  const char *pcSend = NULL;
  uint32 u32AckLen = 0u;
  int iErrCode = DM_PERM;

  if (NULL == g_pfMobileHelloVerify)
  {
    DM_LOG_WARN("conn %u: mobile hello verifier not registered", pstConn->u32Id);
    (void)dm__conn_reply(pstConn, DM_LINKID_HELLO, pstItem->stReply.u32ReqID, DM_PERM, NULL, 0);
    dm__conn_close_after_write(pstConn);
    return;
  }
  pcResume =
      dm__push_resume(pcBoot, yyjson_is_uint(yyjson_obj_get(pstRoot, "seq")) ? DM_TRUE : DM_FALSE,
                      (uint64)dm_json_int(pstRoot, "/seq", 0));
  u64Seen = (uint64)dm_json_int(pstRoot, "/seq", 0);
  stIn.pcHintPeerID = ('\0' != pstConn->acHintPeerID[0]) ? pstConn->acHintPeerID : NULL;
  stIn.pu8Body = (NULL != pstItem->pstBuf) ? pstItem->pstBuf->au8Data : NULL;
  stIn.u32Len = (NULL != pstItem->pstBuf) ? pstItem->pstBuf->u32Len : 0u;
  stIn.pcResume = pcResume;
  stIn.u64CurSeq = dm_push_seq();

  iErrCode = g_pfMobileHelloVerify(&stIn, g_pvMobileHelloCtx, &stOut);
  if (DM_SUCCESS != iErrCode)
  {
    if (NULL != stOut.pcAckJson)
    {
      dm_secure_zero(stOut.pcAckJson, strlen(stOut.pcAckJson));
      dm_free_tag(stOut.pcAckJson);
    }
    DM_LOG_WARN("conn %u: mobile hello rejected (%d)", pstConn->u32Id, iErrCode);
    (void)dm__conn_reply(pstConn, DM_LINKID_HELLO, pstItem->stReply.u32ReqID, iErrCode, NULL, 0);
    dm__conn_close_after_write(pstConn);
    return;
  }
  if (!dm__peer_id_str_valid(stOut.acVerifiedPeerID, &uPeerLen))
  {
    iErrCode = DM_INVALID;
  }
  else
  {
    iErrCode = dm__conn_check_gen_conflict(stOut.acVerifiedPeerID, stOut.u64VerifiedConnGen,
                                           DM_TRUE, pstConn);
  }
  if (DM_SUCCESS != iErrCode)
  {
    if (NULL != stOut.pcAckJson)
    {
      dm_secure_zero(stOut.pcAckJson, strlen(stOut.pcAckJson));
      dm_free_tag(stOut.pcAckJson);
    }
    (void)dm__conn_reply(pstConn, DM_LINKID_HELLO, pstItem->stReply.u32ReqID, iErrCode, NULL, 0);
    dm__conn_close_after_write(pstConn);
    return;
  }
  memcpy(pstConn->acPeerID, stOut.acVerifiedPeerID, uPeerLen + 1u);
  pstConn->u64ConnGen = stOut.u64VerifiedConnGen;
  dm__conn_close_superseded(pstConn->acPeerID, pstConn->u64ConnGen, DM_TRUE, pstConn);
  dm__conn_parse_push(pstConn, pstRoot);
  dm__conn_authed(pstConn, DM_ROLE_MOBILE);

  pcSend = (NULL != stOut.pcAckJson) ? stOut.pcAckJson : "{}";
  u32AckLen = (uint32)strlen(pcSend);
  (void)dm__conn_reply_ex(pstConn, DM_LINKID_HELLO, pstItem->stReply.u32ReqID, DM_SUCCESS,
                          DM_FRAME_F_SENSITIVE, pcSend, u32AckLen);
  if (NULL != stOut.pcAckJson)
  {
    dm_secure_zero(stOut.pcAckJson, u32AckLen);
    dm_free_tag(stOut.pcAckJson);
  }
  if (0 == strcmp(pcResume, "ok"))
  {
    dm__push_replay(pstConn->u32Id, u64Seen);
  }
  return;
}

/* 握手在总队列里做（排在就绪事件后面：「就绪先于连接」） */
static void dm__run_hello(dm_io dm__qitem_st *pstItem)
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
  char acReply[512] = {0};
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
  pcBoot = dm_json_str(pstRoot, "/boot", NULL);
  if (!pstConn->bAuthed && pstConn->bPeerAdopted)
  {
    dm__run_hello_peer(pstConn, pstItem, pstRoot);
    yyjson_doc_free(pstDoc);
    return;
  }
  if (!pstConn->bAuthed && pstConn->bMobileAdopted)
  {
    dm__run_hello_mobile(pstConn, pstItem, pstRoot, pcBoot);
    yyjson_doc_free(pstDoc);
    return;
  }
  pcRole = dm_json_str(pstRoot, "/role", "");
  pcToken = dm_json_str(pstRoot, "/token", "");
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

static void dm__on_hs_timeout(dm_in dm_uv_timer_st *pstTimer)
{
  dm__conn_st *pstConn = container_of(pstTimer, dm__conn_st, stHsTimer);
  DM_LOG_WARN("conn %u: no handshake within %u ms", pstConn->u32Id, (unsigned)DM_HANDSHAKE_MS);
  dm__conn_notify_peer_hello(pstConn, DM_TIMEOUT, 0u, NULL, 0u);
  dm__conn_close(pstConn, "handshake timeout");
  return;
}

/* ============================ 收 ============================ */

/* 处理一帧；返回非 0 表示这条连接已经开始关，别再往下读 */
int dm__conn_on_frame(dm_io dm__conn_st *pstConn, dm_in const dm_frame_head_st *pstHead,
                      dm_in uint8 *pu8Body)
{
  dm_link_msg_st stMsg = {0};
  dm__qitem_st *pstItem = NULL;
  int iErrCode = DM_SUCCESS;

  /* AUD-06：无论帧类型（含 REP/握手前异常帧），一旦命中敏感标志或 HELLO 立刻标记连接 */
  if ((0u != (pstHead->u8Flag & DM_FRAME_F_SENSITIVE)) || (DM_LINKID_HELLO == pstHead->u16LinkID))
  {
    pstConn->bHadSensitive = DM_TRUE;
  }

  /* 握手前门禁：主动出站 Peer 仅允许 HELLO REP，其余连接仅允许 HELLO REQ */
  if (!pstConn->bAuthed)
  {
    if (pstConn->bPeerAdopted && pstConn->bPeerInitiator)
    {
      if ((DM_FRAME_T_REP == pstHead->u8Type) && (DM_LINKID_HELLO == pstHead->u16LinkID) &&
          pstConn->bPeerHelloSent && (pstHead->u32ReqID == pstConn->u32PeerHelloReq))
      {
        dm__conn_notify_peer_hello(pstConn, (int)pstHead->i32Status, pstHead->u8Flag, pu8Body,
                                   pstHead->u32Len);
        return 0;
      }
      (void)dm__conn_reply(pstConn, pstHead->u16LinkID, pstHead->u32ReqID, DM_NOT_READY, NULL, 0);
      dm__conn_close_after_write(pstConn);
      return 1;
    }
    if ((DM_FRAME_T_REQ != pstHead->u8Type) || (DM_LINKID_HELLO != pstHead->u16LinkID))
    {
      (void)dm__conn_reply(pstConn, pstHead->u16LinkID, pstHead->u32ReqID, DM_NOT_READY, NULL, 0);
      dm__conn_close_after_write(pstConn);
      return 1;
    }
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
    if ((DM_FRAME_T_PUSH == pstHead->u8Type) && pstConn->bPeerAdopted &&
        (0u != (pstConn->u32Role & DM_ROLE_PEER)))
    {
      if ((pstHead->u32Len >= DM_FRAME_PUSH_HEAD) && (NULL != g_pfPeerPushCb))
      {
        dm_peer_push_msg_st stPush = {0};
        stPush.pcPeerID = pstConn->acPeerID;
        stPush.u16Topic = (uint16)dm_frame_rd16(pu8Body + 8u);
        stPush.u64Key = dm_frame_rd64(pu8Body + 10u);
        stPush.pu8Val = pu8Body + DM_FRAME_PUSH_HEAD;
        stPush.u32Len = pstHead->u32Len - DM_FRAME_PUSH_HEAD;
        g_pfPeerPushCb(&stPush, g_pvPeerPushCtx);
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

static void dm__conn_on_alloc(dm_in dm_uv_handle_st *pstHandle, dm_in size_t uSuggested,
                              dm_out dm_uv_buf_st *pstBuf)
{
  dm__conn_st *pstConn = (dm__conn_st *)pstHandle;
  (void)uSuggested;
  pstBuf->base = (char *)(pstConn->pu8Rx + pstConn->u32RxLen);
  pstBuf->len = pstConn->u32RxCap - pstConn->u32RxLen;
  return;
}

/* 接收缓冲长到能装下 u32Need 字节（不超过单条上限 + 帧头） */
static int dm__conn_rx_grow(dm_io dm__conn_st *pstConn, dm_in uint32 u32Need)
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

static void dm__conn_rx_shrink(dm_io dm__conn_st *pstConn)
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

static void dm__conn_on_read(dm_in dm_uv_stream_st *pstStream, dm_in ssize_t iNRead,
                             dm_in const dm_uv_buf_st *pstBuf)
{
  dm__conn_st *pstConn = (dm__conn_st *)pstStream;
  dm_frame_head_st stHead = {0};
  uint32 u32Off = 0;
  uint32 u32Max = 0;
  (void)pstBuf;

  if (iNRead < 0)
  {
    dm__conn_close(pstConn, (DM_UV_EOF == iNRead) ? "closed by peer" : "read error");
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
static dm__conn_st *dm__conn_accept(dm_in dm_uv_stream_st *pstServer, dm_in uint8 u8Kind)
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
  pstConn->u8StreamKind = DM__STREAM_TCP;
  (void)dm_uv_tcp_init(dm_uv_default_loop(), &pstConn->uStream.stTcp);
  (void)dm_uv_timer_init(dm_uv_default_loop(), &pstConn->stHsTimer);
  g_u32ConnNext++;
  pstConn->u32Id = g_u32ConnNext;
  dm_list_add_tail(&g_stConnList, &pstConn->stNode);
  if ((DM_SUCCESS != dm_uv_accept(pstServer, dm__conn_stream(pstConn))) ||
      (DM_SUCCESS != dm_hash_add(&g_stConnTable, pstConn->u32Id, pstConn)))
  {
    dm_list_del_node(&pstConn->stNode);
    pstConn->bClosing = DM_TRUE;
    dm_uv_close((dm_uv_handle_st *)&pstConn->stHsTimer, dm__hs_timer_closed);
    return NULL;
  }
  (void)dm_uv_tcp_nodelay(&pstConn->uStream.stTcp, 1);
  if (DM_SUCCESS != dm__flow_start(&pstConn->stFlow, dm__conn_stream(pstConn), dm__conn_on_alloc,
                                   dm__conn_on_read))
  {
    dm__conn_close(pstConn, "read start failed");
    return NULL;
  }
  return pstConn;
}

/* 开始走 link 的规矩（二进制口一连上；WebSocket 升级以后）：要握手的起握手定时器 */
void dm__conn_start_link(dm_io dm__conn_st *pstConn)
{
  if (NULL == g_pastRole)
  {
    dm__conn_authed(pstConn, DM_ROLE_ALL); /* 不要求握手（DM 自己的测试用）：连接带全部的位 */
    return;
  }
  (void)dm_uv_timer_start(&pstConn->stHsTimer, dm__on_hs_timeout, DM_HANDSHAKE_MS, 0);
  return;
}

uint32 dm__link_frame_max(void)
{
  return g_u32FrameMax;
}

static void dm__on_new_connection(dm_in dm_uv_stream_st *pstServer, dm_in int iStatus)
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

static void dm__on_new_http(dm_in dm_uv_stream_st *pstServer, dm_in int iStatus)
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

static void dm__server_on_closed(dm_in dm_uv_handle_st *pstHandle)
{
  dm_free_tag(pstHandle);
  return;
}

static void dm__server_close(dm_io dm_uv_tcp_st **ppstServer)
{
  if (NULL != *ppstServer)
  {
    dm_uv_close((dm_uv_handle_st *)*ppstServer, dm__server_on_closed);
    *ppstServer = NULL;
  }
  return;
}

/* 绑定并监听 127.0.0.1:iPort；返回 DM_SUCCESS 或错误码（端口被占要到 listen 才报） */
static int dm__listen_on(dm_out dm_uv_tcp_st **ppstServer, dm_in int iPort,
                         dm_in dm_uv_connection_cb pfOnNew, dm_out int *piBound)
{
  struct sockaddr_in stAddr = {0};
  struct sockaddr_storage stName = {0};
  int iNameLen = (int)sizeof(stName);
  int iErrCode = DM_SUCCESS;

  iErrCode = dm_uv_ip4_addr("127.0.0.1", iPort, &stAddr);
  if (DM_SUCCESS != iErrCode)
  {
    return iErrCode;
  }
  *ppstServer = (dm_uv_tcp_st *)dm_malloc_tag(sizeof(dm_uv_tcp_st), DM_MEM_TAG_LINK_CONN);
  if (NULL == *ppstServer)
  {
    return DM_ALLOC;
  }
  iErrCode = dm_uv_tcp_init(dm_uv_default_loop(), *ppstServer);
  if (DM_SUCCESS != iErrCode)
  {
    dm_free_tag(*ppstServer);
    *ppstServer = NULL;
    return iErrCode;
  }
  iErrCode = dm_uv_tcp_bind(*ppstServer, (const struct sockaddr *)&stAddr, 0);
  if (DM_SUCCESS == iErrCode)
  {
    iErrCode = dm_uv_listen((dm_uv_stream_st *)*ppstServer, 128, pfOnNew);
  }
  if (DM_SUCCESS == iErrCode)
  {
    iErrCode = dm_uv_tcp_getsockname(*ppstServer, (struct sockaddr *)&stName, &iNameLen);
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
static int dm__listen_pick(dm_out dm_uv_tcp_st **ppstServer, dm_in int iLast,
                           dm_in dm_uv_connection_cb pfOnNew, dm_out int *piBound,
                           dm_in const char *pcWhat)
{
  int iErrCode =
      (0 != iLast) ? dm__listen_on(ppstServer, iLast, pfOnNew, piBound) : DM_UV_EADDRINUSE;
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
  else if (g_bDisableLocalHttp)
  {
    g_iHttpPort = 0;
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

/* ============================ 跨节点 Peer 与移动端 fd 纳管 ============================ */

int dm_link_set_peer_hello_verifier(dm_in dm_peer_hello_verify_cb pfVerify, dm_in void *pvCtx)
{
  DM__CHECK_LOOP();
  g_pfPeerHelloVerify = pfVerify;
  g_pvPeerHelloCtx = (NULL != pfVerify) ? pvCtx : NULL;
  return DM_SUCCESS;
}

int dm_link_set_peer_push_cb(dm_in dm_peer_push_cb pfPush, dm_in void *pvCtx)
{
  DM__CHECK_LOOP();
  g_pfPeerPushCb = pfPush;
  g_pvPeerPushCtx = (NULL != pfPush) ? pvCtx : NULL;
  return DM_SUCCESS;
}

int dm_link_set_mobile_hello_verifier(dm_in dm_mobile_hello_verify_cb pfVerify, dm_in void *pvCtx)
{
  DM__CHECK_LOOP();
  g_pfMobileHelloVerify = pfVerify;
  g_pvMobileHelloCtx = (NULL != pfVerify) ? pvCtx : NULL;
  return DM_SUCCESS;
}

typedef struct dm__adopt_req
{
  dm_peer_fd_t tFd;
  const char *pcPeerID;
  uint64 u64ConnGen;
  bool_t bIsPipe;
  bool_t bIsInitiator;
  bool_t bMobile;
} dm__adopt_req_st;

static int dm__conn_adopt_fd(dm_in const dm__adopt_req_st *pstReq)
{
  dm__conn_st *pstConn = NULL;
  size_t uPeerLen = 0u;
  int iErrCode = DM_SUCCESS;

  DM__CHECK_LOOP();
  if (DM_PEER_FD_INVALID == pstReq->tFd)
  {
    return DM_INVALID;
  }
  if (!pstReq->bMobile || ((NULL != pstReq->pcPeerID) && ('\0' != pstReq->pcPeerID[0])))
  {
    if (!dm__peer_id_str_valid(pstReq->pcPeerID, &uPeerLen))
    {
      return DM_INVALID;
    }
  }
  if (dm__phase() >= DM__PHASE_TEARDOWN)
  {
    return DM_SHUTTING_DOWN;
  }
  if (NULL == g_stConnTable.pstBuckets)
  {
    return DM_LINK_ERR_STATE;
  }
  if (!pstReq->bMobile && (uPeerLen > 0u))
  {
    iErrCode = dm__conn_check_gen_conflict(pstReq->pcPeerID, pstReq->u64ConnGen, DM_FALSE, NULL);
    if (DM_SUCCESS != iErrCode)
    {
      return iErrCode;
    }
  }

  pstConn = (dm__conn_st *)dm_calloc_tag(1u, sizeof(dm__conn_st), DM_MEM_TAG_LINK_CONN);
  if (NULL == pstConn)
  {
    return DM_ALLOC;
  }
  pstConn->u8Kind = DM__CONN_BIN;
  pstConn->u8StreamKind = pstReq->bIsPipe ? DM__STREAM_PIPE : DM__STREAM_TCP;
  pstConn->bPeerAdopted = pstReq->bMobile ? DM_FALSE : DM_TRUE;
  pstConn->bPeerInitiator = (!pstReq->bMobile && pstReq->bIsInitiator) ? DM_TRUE : DM_FALSE;
  pstConn->bMobileAdopted = pstReq->bMobile ? DM_TRUE : DM_FALSE;
  pstConn->u64ConnGen = pstReq->bMobile ? 0u : pstReq->u64ConnGen;
  if (uPeerLen > 0u)
  {
    if (pstReq->bMobile)
    {
      memcpy(pstConn->acHintPeerID, pstReq->pcPeerID, uPeerLen + 1u);
    }
    else
    {
      memcpy(pstConn->acPeerID, pstReq->pcPeerID, uPeerLen + 1u);
    }
  }
  pstConn->pu8Rx = (uint8 *)dm_malloc_tag(DM__RX_KEEP, DM_MEM_TAG_LINK_RX);
  pstConn->pstKillItem = dm__qitem_new();
  if ((NULL == pstConn->pu8Rx) || (NULL == pstConn->pstKillItem) ||
      (DM_SUCCESS != dm_hash_init(&pstConn->stPending, DM__PEND_BUCKETS, NULL)))
  {
    dm__qitem_free(pstConn->pstKillItem);
    dm_free_tag(pstConn->pu8Rx);
    dm_free_tag(pstConn);
    return DM_ALLOC;
  }
  pstConn->u32RxCap = DM__RX_KEEP;

  if (pstReq->bIsPipe)
  {
    iErrCode = dm_uv_pipe_init(dm_uv_default_loop(), &pstConn->uStream.stPipe, 0);
  }
  else
  {
    iErrCode = dm_uv_tcp_init(dm_uv_default_loop(), &pstConn->uStream.stTcp);
  }
  if (DM_SUCCESS != iErrCode)
  {
    dm_hash_destroy(&pstConn->stPending, NULL);
    dm__qitem_free(pstConn->pstKillItem);
    dm_free_tag(pstConn->pu8Rx);
    dm_free_tag(pstConn);
    return iErrCode;
  }
  (void)dm_uv_timer_init(dm_uv_default_loop(), &pstConn->stHsTimer);

  g_u32ConnNext++;
  pstConn->u32Id = g_u32ConnNext;
  iErrCode = dm_hash_add(&g_stConnTable, pstConn->u32Id, pstConn);
  if (DM_SUCCESS != iErrCode)
  {
    pstConn->bClosing = DM_TRUE;
    dm_uv_close((dm_uv_handle_st *)&pstConn->stHsTimer, dm__hs_timer_closed);
    return iErrCode;
  }
  if (pstReq->bIsPipe)
  {
    iErrCode = dm_uv_pipe_open(&pstConn->uStream.stPipe, (dm_uv_file)pstReq->tFd);
  }
  else
  {
    iErrCode = dm_uv_tcp_open(&pstConn->uStream.stTcp, (dm_uv_os_sock_t)pstReq->tFd);
  }
  if (DM_SUCCESS != iErrCode)
  {
    /* open 失败时未接管 tFd，异步清理空句柄，调用方保留 tFd 所有权 */
    (void)dm_hash_del(&g_stConnTable, pstConn->u32Id, NULL);
    pstConn->bClosing = DM_TRUE;
    dm_uv_close((dm_uv_handle_st *)&pstConn->stHsTimer, dm__hs_timer_closed);
    return iErrCode;
  }
  dm_list_add_tail(&g_stConnList, &pstConn->stNode);
  if (!pstReq->bIsPipe)
  {
    (void)dm_uv_tcp_nodelay(&pstConn->uStream.stTcp, 1);
  }
  if (DM_SUCCESS != dm__flow_start(&pstConn->stFlow, dm__conn_stream(pstConn), dm__conn_on_alloc,
                                   dm__conn_on_read))
  {
    DM_LOG_WARN("conn %u: adopted fd read_start failed", pstConn->u32Id);
  }
  if (!pstReq->bMobile && (uPeerLen > 0u))
  {
    dm__conn_close_superseded(pstConn->acPeerID, pstConn->u64ConnGen, DM_FALSE, pstConn);
  }
  (void)dm_uv_timer_start(&pstConn->stHsTimer, dm__on_hs_timeout, DM_HANDSHAKE_MS, 0);
  return DM_SUCCESS;
}

int dm_link_adopt_peer_fd(dm_in dm_peer_fd_t tFd, dm_in const char *pcPeerID,
                          dm_in uint64 u64ConnGen, dm_in bool_t bIsPipe, dm_in bool_t bIsInitiator)
{
  dm__adopt_req_st stReq = {0};

  stReq.tFd = tFd;
  stReq.pcPeerID = pcPeerID;
  stReq.u64ConnGen = u64ConnGen;
  stReq.bIsPipe = bIsPipe;
  stReq.bIsInitiator = bIsInitiator;
  stReq.bMobile = DM_FALSE;
  return dm__conn_adopt_fd(&stReq);
}

int dm_link_adopt_mobile_fd(dm_in dm_peer_fd_t tFd, dm_in const char *pcHintPeerID,
                            dm_in uint64 u64ConnGen, dm_in bool_t bIsPipe)
{
  dm__adopt_req_st stReq = {0};

  stReq.tFd = tFd;
  stReq.pcPeerID = pcHintPeerID;
  stReq.u64ConnGen = u64ConnGen;
  stReq.bIsPipe = bIsPipe;
  stReq.bIsInitiator = DM_FALSE;
  stReq.bMobile = DM_TRUE;
  return dm__conn_adopt_fd(&stReq);
}

int dm_link_peer_send_hello(dm_in const dm_peer_hello_req_st *pstReq)
{
  dm__conn_st *pstConn = NULL;
  dm_frame_head_st stHead = {0};
  uint32 u32TimeoutMs = 0u;
  int iErrCode = DM_SUCCESS;

  DM__CHECK_LOOP();
  if ((NULL == pstReq) || (NULL == pstReq->pcPeerID) || ('\0' == pstReq->pcPeerID[0]) ||
      (NULL == pstReq->pvBody) || (0u == pstReq->u32Len))
  {
    return DM_INVALID;
  }
  if (pstReq->u32Len > DM_PRE_AUTH_MAX)
  {
    return DM_TOO_BIG;
  }
  if (dm__phase() >= DM__PHASE_TEARDOWN)
  {
    return DM_SHUTTING_DOWN;
  }
  if (NULL == dm__link_lookup(pstReq->uiReplyLinkID))
  {
    return DM_LINK_ERR_NOT_FOUND;
  }
  pstConn = dm__conn_find_peer_preauth(pstReq->pcPeerID, pstReq->u64ConnGen);
  if (NULL == pstConn)
  {
    return DM_CONN_GONE;
  }
  if (pstConn->bPeerHelloSent)
  {
    return DM_EXISTS;
  }
  g_u32PeerHelloSeq++;
  if (0u == g_u32PeerHelloSeq)
  {
    g_u32PeerHelloSeq = 1u;
  }
  pstConn->bPeerHelloSent = DM_TRUE;
  pstConn->u32PeerHelloReq = g_u32PeerHelloSeq;
  pstConn->uiPeerHelloReply = pstReq->uiReplyLinkID;
  pstConn->u64PeerHelloTag = pstReq->u64Tag;

  stHead.u16LinkID = DM_LINKID_HELLO;
  stHead.u8Type = DM_FRAME_T_REQ;
  stHead.u8Flag = (uint8)(DM_FRAME_F_JSON | DM_FRAME_F_SENSITIVE);
  stHead.u32ReqID = pstConn->u32PeerHelloReq;
  stHead.u32Len = pstReq->u32Len;
  iErrCode = dm__conn_write(pstConn, &stHead, pstReq->pvBody);
  if (DM_SUCCESS != iErrCode)
  {
    pstConn->bPeerHelloSent = DM_FALSE;
    return iErrCode;
  }
  u32TimeoutMs = (0u != pstReq->u32TimeoutMs) ? pstReq->u32TimeoutMs : DM_HANDSHAKE_MS;
  (void)dm_uv_timer_stop(&pstConn->stHsTimer);
  (void)dm_uv_timer_start(&pstConn->stHsTimer, dm__on_hs_timeout, u32TimeoutMs, 0);
  return DM_SUCCESS;
}

int dm_link_peer_mark_ready(dm_in const char *pcPeerID, dm_in uint64 u64ConnGen,
                            dm_in const char *pcRemotePushJson)
{
  dm__conn_st *pstConn = NULL;

  DM__CHECK_LOOP();
  if ((NULL == pcPeerID) || ('\0' == pcPeerID[0]))
  {
    return DM_INVALID;
  }
  if (dm__phase() >= DM__PHASE_TEARDOWN)
  {
    return DM_SHUTTING_DOWN;
  }
  pstConn = dm__conn_find_peer_preauth(pcPeerID, u64ConnGen);
  if (NULL == pstConn)
  {
    return DM_CONN_GONE;
  }
  if ((NULL != pcRemotePushJson) && ('\0' != pcRemotePushJson[0]))
  {
    yyjson_doc *pstDoc =
        dm_json_read(pcRemotePushJson, strlen(pcRemotePushJson), DM_PRE_AUTH_MAX, NULL);
    yyjson_val *pstRoot = (NULL != pstDoc) ? yyjson_doc_get_root(pstDoc) : NULL;
    if (yyjson_is_arr(pstRoot))
    {
      dm__conn_parse_push_arr(pstConn, pstRoot);
    }
    else if (yyjson_is_obj(pstRoot))
    {
      dm__conn_parse_push(pstConn, pstRoot);
    }
    yyjson_doc_free(pstDoc);
  }
  dm__conn_authed(pstConn, DM_ROLE_PEER);
  return DM_SUCCESS;
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
  g_pfPeerHelloVerify = NULL;
  g_pvPeerHelloCtx = NULL;
  g_pfPeerPushCb = NULL;
  g_pvPeerPushCtx = NULL;
  g_pfMobileHelloVerify = NULL;
  g_pvMobileHelloCtx = NULL;
  return;
}

#ifdef __cplusplus
}
#endif
