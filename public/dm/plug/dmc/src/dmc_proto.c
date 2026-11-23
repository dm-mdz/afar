/**
 ******************************************************************************
 * @file    dmc_proto.c
 * @author  MDM
 * @version 0.0.1.0
 * @brief   dmc 下层纯 Sans-IO 协议状态机（DD-02 §2.1~2.2，PLUG-01/02）：
 *          只用标准 C 与 <dm/frame.h>/<dm/errno.h> 纯宏，零 IO、零 libuv、零 libdm 链接；
 *          支持 pfAlloc/pfRealloc/pfFree 自定义内存钩子与 dmc_notify(reqID=0) 单向通知。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include <dmc_proto.h>
#include <dm/errno.h>

#include <stdint.h> /* SIZE_MAX */
#include <stdlib.h> /* malloc / realloc / free */
#include <string.h> /* memcpy / memmove / memset */

typedef struct dmc__pending
{
  uint32 u32ReqID; /* 0 = 空位 */
  bool_t bInSend;  /* 正在 pfSend 中：断线时不触发 pfReply（dmc_request 直接返错） */
  dmc_reply_cb pfReply;
  void *pvArg;
} dmc__pending_st;

struct dmc
{
  dmc_host_st stHost;
  bool_t bConnected;
  bool_t bBroken;    /* 收到坏帧：到下次 dmc_on_connected 前喂入的字节丢弃 */
  bool_t bInFeed;    /* dmc_feed 重入保护（防止回调重入 dmc_feed 导致 pu8Rx 悬垂） */
  uint32 u32ConnGen; /* 连接代数：connect/disconnect 递增，防止回调重连后 memmove 下溢 */
  uint32 u32NextID;  /* 下一个请求号（从 1 递增，绕回严跳 0） */
  uint32 u32Inflight;
  dmc__pending_st astPending[DMC_PENDING_MAX];
  uint8 *pu8Rx; /* 未凑齐整帧的缓冲 */
  size_t uRxLen;
  size_t uRxCap;
};

/* ============================ 错误码文本 ============================ */

const char *dmc_err_str(in int iErr)
{
  switch (iErr)
  {
  case DMC_OK:
    return "ok";
  case DMC_ERR_TIMEOUT:
    return "client request timed out";
  case DMC_ERR_NOT_CON:
    return "not connected";
  case DMC_ERR_ARG:
    return "invalid client argument";
  case DMC_ERR_QUEUE:
    return "inflight queue full or out of memory";
  case DMC_ERR_WIRE:
    return "malformed wire frame";
  case DMC_ERR_CRC:
    return "frame sync CRC16 mismatch";
  case DMC_ERR_AUTH:
    return "handshake authentication refused";
  case DMC_ERR_NOT_FOUND:
    return "interface name not found";
  case DMC_ERR_UNSUPPORTED:
    return "transport scheme not implemented";
  case DM__FAILD:
    return "failed";
  case DM__INVALID:
    return "invalid argument";
  case DM__ALLOC:
    return "memory allocation failed";
  case DM__NOT_FOUND:
    return "not found";
  case DM__EXISTS:
    return "already exists";
  case DM__BUSY:
    return "resource busy";
  case DM__IO:
    return "i/o error";
  case DM__TIMEOUT:
    return "operation timed out";
  case DM__PERM:
    return "permission denied";
  case DM__RANGE:
    return "value out of range";
  case DM__LINK_ERR_NOT_FOUND:
    return "no such interface";
  case DM__SHUTTING_DOWN:
    return "core is shutting down";
  case DM__NOT_READY:
    return "not handshaken";
  case DM__TOO_BIG:
    return "too big";
  default:
    return "unknown error";
  }
}

/* ============================ 可插拔内存钩子 ============================ */

static void *dmc__alloc(in const dmc_host_st *pstHost, in size_t uSize)
{
  if (NULL != pstHost->pfAlloc)
  {
    return pstHost->pfAlloc(pstHost->pvUser, uSize);
  }
  return malloc(uSize);
}

static void dmc__free_with(in const dmc_host_st *pstHost, io void *pvPtr)
{
  if (NULL == pvPtr)
  {
    return;
  }
  if (NULL != pstHost->pfFree)
  {
    pstHost->pfFree(pstHost->pvUser, pvPtr);
    return;
  }
  free(pvPtr);
  return;
}

static void *dmc__realloc_rx(io dmc_st *pstDmc, in size_t uNewCap)
{
  if (NULL != pstDmc->stHost.pfAlloc)
  {
    void *pvNew = NULL;
    if (NULL != pstDmc->stHost.pfRealloc)
    {
      return pstDmc->stHost.pfRealloc(pstDmc->stHost.pvUser, pstDmc->pu8Rx, uNewCap);
    }
    pvNew = pstDmc->stHost.pfAlloc(pstDmc->stHost.pvUser, uNewCap);
    if (NULL == pvNew)
    {
      return NULL;
    }
    if ((NULL != pstDmc->pu8Rx) && (pstDmc->uRxLen > 0u))
    {
      memcpy(pvNew, pstDmc->pu8Rx, pstDmc->uRxLen);
    }
    dmc__free_with(&pstDmc->stHost, pstDmc->pu8Rx);
    return pvNew;
  }
  return realloc(pstDmc->pu8Rx, uNewCap);
}

/* ============================ 在途表 ============================ */

static dmc__pending_st *dmc__find(io dmc_st *pstDmc, in uint32 u32ReqID)
{
  uint32 u32Idx = 0u;
  for (u32Idx = 0u; u32Idx < DMC_PENDING_MAX; u32Idx++)
  {
    if (u32ReqID == pstDmc->astPending[u32Idx].u32ReqID)
    {
      return &pstDmc->astPending[u32Idx];
    }
  }
  return NULL;
}

static void dmc__finish(io dmc_st *pstDmc, io dmc__pending_st *pstOne, in int iStatus,
                        in uint8 u8Flag, in const uint8 *pu8Body, in uint32 u32Len)
{
  dmc_reply_cb pfReply = pstOne->pfReply;
  void *pvArg = pstOne->pvArg;

  memset(pstOne, 0, sizeof(*pstOne));
  pstDmc->u32Inflight--;
  if (NULL != pfReply)
  {
    pfReply(pvArg, iStatus, u8Flag, pu8Body, u32Len);
  }
  return;
}

/* ============================ 生命周期 ============================ */

dmc_st *dmc_new(in const dmc_host_st *pstHost)
{
  dmc_st *pstDmc = NULL;

  if ((NULL == pstHost) || (NULL == pstHost->pfSend))
  {
    return NULL;
  }
  if ((NULL != pstHost->pfAlloc) && (NULL == pstHost->pfFree))
  {
    return NULL; /* 提供了自定义 pfAlloc 必须成对提供 pfFree */
  }
  if ((NULL == pstHost->pfAlloc) && ((NULL != pstHost->pfRealloc) || (NULL != pstHost->pfFree)))
  {
    return NULL; /* 禁止混用默认 malloc 与自定义 realloc/free */
  }
  pstDmc = (dmc_st *)dmc__alloc(pstHost, sizeof(dmc_st));
  if (NULL == pstDmc)
  {
    return NULL;
  }
  memset(pstDmc, 0, sizeof(*pstDmc));
  pstDmc->stHost = *pstHost;
  if (0u == pstDmc->stHost.u32FrameMax)
  {
    pstDmc->stHost.u32FrameMax = DM_FRAME_MAX_DEFAULT;
  }
  pstDmc->u32NextID = 1u;
  return pstDmc;
}

void dmc_free(io dmc_st *pstDmc)
{
  dmc_host_st stHostCopy;

  if (NULL == pstDmc)
  {
    return;
  }
  stHostCopy = pstDmc->stHost;
  dmc__free_with(&stHostCopy, pstDmc->pu8Rx);
  dmc__free_with(&stHostCopy, pstDmc);
  return;
}

void dmc_on_connected(io dmc_st *pstDmc)
{
  if (NULL == pstDmc)
  {
    return;
  }
  pstDmc->u32ConnGen++;
  pstDmc->bConnected = DM_TRUE;
  pstDmc->bBroken = DM_FALSE;
  pstDmc->uRxLen = 0u;
  return;
}

void dmc_on_disconnected(io dmc_st *pstDmc)
{
  uint32 u32Idx = 0u;

  if (NULL == pstDmc)
  {
    return;
  }
  pstDmc->u32ConnGen++;
  pstDmc->bConnected = DM_FALSE;
  pstDmc->uRxLen = 0u;
  for (u32Idx = 0u; u32Idx < DMC_PENDING_MAX; u32Idx++)
  {
    if (0u != pstDmc->astPending[u32Idx].u32ReqID)
    {
      if (pstDmc->astPending[u32Idx].bInSend)
      {
        memset(&pstDmc->astPending[u32Idx], 0, sizeof(pstDmc->astPending[u32Idx]));
        pstDmc->u32Inflight--;
      }
      else
      {
        dmc__finish(pstDmc, &pstDmc->astPending[u32Idx], DMC_ERR_NOT_CON, 0u, NULL, 0u);
      }
    }
  }
  return;
}

bool_t dmc_connected(in const dmc_st *pstDmc)
{
  return ((NULL != pstDmc) && pstDmc->bConnected) ? DM_TRUE : DM_FALSE;
}

uint32 dmc_inflight(in const dmc_st *pstDmc)
{
  return (NULL != pstDmc) ? pstDmc->u32Inflight : 0u;
}

/* ============================ 发 ============================ */

static int dmc__send(io dmc_st *pstDmc, in const dm_frame_head_st *pstHead, in const void *pvBody)
{
  uint8 au8Head[DM_FRAME_HEAD];

  dm_frame_put(au8Head, pstHead);
  return (0 == pstDmc->stHost.pfSend(pstDmc->stHost.pvUser, au8Head, pvBody, pstHead->u32Len))
             ? DMC_OK
             : DMC_ERR_NOT_CON;
}

int dmc_request(io dmc_st *pstDmc, in uint16 u16LinkID, in uint8 u8Flag, in const void *pvBody,
                in uint32 u32Len, in dmc_reply_cb pfReply, in void *pvArg, out uint32 *pu32ReqID)
{
  dm_frame_head_st stHead;
  dmc__pending_st *pstSlot = NULL;
  int iErr = DMC_OK;

  if ((NULL == pstDmc) || ((NULL == pvBody) && (0u != u32Len)))
  {
    return DMC_ERR_ARG;
  }
  if (u32Len > pstDmc->stHost.u32FrameMax)
  {
    return DMC_ERR_ARG;
  }
  if (!pstDmc->bConnected)
  {
    return DMC_ERR_NOT_CON;
  }
  pstSlot = dmc__find(pstDmc, 0u);
  if (NULL == pstSlot)
  {
    return DMC_ERR_QUEUE;
  }
  pstSlot->u32ReqID = pstDmc->u32NextID;
  pstSlot->bInSend = DM_TRUE;
  pstSlot->pfReply = pfReply;
  pstSlot->pvArg = pvArg;
  pstDmc->u32Inflight++;
  pstDmc->u32NextID++;
  if (0u == pstDmc->u32NextID)
  {
    pstDmc->u32NextID = 1u;
  }
  memset(&stHead, 0, sizeof(stHead));
  stHead.u16LinkID = u16LinkID;
  stHead.u8Type = DM_FRAME_T_REQ;
  stHead.u8Flag = u8Flag;
  stHead.u32ReqID = pstSlot->u32ReqID;
  stHead.u32Len = u32Len;
  if (NULL != pu32ReqID)
  {
    *pu32ReqID = pstSlot->u32ReqID;
  }
  iErr = dmc__send(pstDmc, &stHead, pvBody);
  if (DMC_OK != iErr)
  {
    if (pstSlot->u32ReqID == stHead.u32ReqID)
    {
      memset(pstSlot, 0, sizeof(*pstSlot));
      pstDmc->u32Inflight--;
    }
  }
  else if (pstSlot->u32ReqID == stHead.u32ReqID)
  {
    pstSlot->bInSend = DM_FALSE;
  }
  return iErr;
}

int dmc_notify(io dmc_st *pstDmc, in uint16 u16LinkID, in uint8 u8Flag, in const void *pvBody,
               in uint32 u32Len)
{
  dm_frame_head_st stHead;

  if ((NULL == pstDmc) || ((NULL == pvBody) && (0u != u32Len)))
  {
    return DMC_ERR_ARG;
  }
  if (u32Len > pstDmc->stHost.u32FrameMax)
  {
    return DMC_ERR_ARG;
  }
  if (!pstDmc->bConnected)
  {
    return DMC_ERR_NOT_CON;
  }
  memset(&stHead, 0, sizeof(stHead));
  stHead.u16LinkID = u16LinkID;
  stHead.u8Type = DM_FRAME_T_REQ;
  stHead.u8Flag = u8Flag;
  stHead.u32ReqID = 0u; /* 单向通知：不要求应答，不占 64 在途槽 */
  stHead.u32Len = u32Len;
  return dmc__send(pstDmc, &stHead, pvBody);
}

int dmc_cancel(io dmc_st *pstDmc, in uint32 u32ReqID, in int iStatus)
{
  dmc__pending_st *pstOne = NULL;

  if ((NULL == pstDmc) || (0u == u32ReqID))
  {
    return DMC_ERR_ARG;
  }
  pstOne = dmc__find(pstDmc, u32ReqID);
  if (NULL == pstOne)
  {
    return DMC_ERR_ARG;
  }
  dmc__finish(pstDmc, pstOne, iStatus, 0u, NULL, 0u);
  return DMC_OK;
}

int dmc_answer(io dmc_st *pstDmc, in uint16 u16Func, in uint32 u32ReqID, in int32 i32Status,
               in uint8 u8Flag, in const void *pvBody, in uint32 u32Len)
{
  dm_frame_head_st stHead;

  if ((NULL == pstDmc) || (0u == u32ReqID) || ((NULL == pvBody) && (0u != u32Len)) ||
      (u32Len > pstDmc->stHost.u32FrameMax))
  {
    return DMC_ERR_ARG;
  }
  if (!pstDmc->bConnected)
  {
    return DMC_ERR_NOT_CON;
  }
  memset(&stHead, 0, sizeof(stHead));
  stHead.u16LinkID = u16Func;
  stHead.u8Type = DM_FRAME_T_REP;
  stHead.u8Flag = u8Flag;
  stHead.u32ReqID = u32ReqID;
  stHead.i32Status = i32Status;
  stHead.u32Len = u32Len;
  return dmc__send(pstDmc, &stHead, pvBody);
}

/* ============================ 收 ============================ */

static void dmc__wire_error(io dmc_st *pstDmc)
{
  pstDmc->bBroken = DM_TRUE;
  pstDmc->uRxLen = 0u;
  if (NULL != pstDmc->stHost.pfOnWireError)
  {
    pstDmc->stHost.pfOnWireError(pstDmc->stHost.pvUser, DMC_ERR_WIRE);
  }
  return;
}

static bool_t dmc__on_frame(io dmc_st *pstDmc, in const dm_frame_head_st *pstHead,
                            in const uint8 *pu8Body)
{
  dmc__pending_st *pstOne = NULL;
  dmc_push_st stPush;

  switch (pstHead->u8Type)
  {
  case DM_FRAME_T_REP:
    pstOne = (0u != pstHead->u32ReqID) ? dmc__find(pstDmc, pstHead->u32ReqID) : NULL;
    if (NULL != pstOne)
    {
      dmc__finish(pstDmc, pstOne, (int)pstHead->i32Status, pstHead->u8Flag, pu8Body,
                  pstHead->u32Len);
    }
    return DM_TRUE;
  case DM_FRAME_T_PUSH:
    if (pstHead->u32Len < DM_FRAME_PUSH_HEAD)
    {
      return DM_FALSE;
    }
    if (NULL != pstDmc->stHost.pfOnPush)
    {
      stPush.u64Seq = dm_frame_rd64(pu8Body);
      stPush.u16Topic = (uint16)dm_frame_rd16(pu8Body + 8);
      stPush.u64Key = dm_frame_rd64(pu8Body + 10);
      stPush.u8Flag = pstHead->u8Flag;
      stPush.pu8Val = pu8Body + DM_FRAME_PUSH_HEAD;
      stPush.u32Len = pstHead->u32Len - DM_FRAME_PUSH_HEAD;
      pstDmc->stHost.pfOnPush(pstDmc->stHost.pvUser, &stPush);
    }
    return DM_TRUE;
  case DM_FRAME_T_REQ:
    if (NULL != pstDmc->stHost.pfOnCall)
    {
      pstDmc->stHost.pfOnCall(pstDmc->stHost.pvUser, pstHead->u16LinkID, pstHead->u32ReqID, pu8Body,
                              pstHead->u32Len);
    }
    else if (0u != pstHead->u32ReqID)
    {
      (void)dmc_answer(pstDmc, pstHead->u16LinkID, pstHead->u32ReqID, (int32)DM__NOT_FOUND, 0u,
                       NULL, 0u);
    }
    return DM_TRUE;
  default:
    return DM_FALSE;
  }
}

static size_t dmc__cut(io dmc_st *pstDmc, in const uint8 *pu8, in size_t uLen, in uint32 u32Gen)
{
  dm_frame_head_st stHead;
  size_t uUsed = 0u;

  while ((uLen - uUsed) >= DM_FRAME_HEAD)
  {
    dm_frame_get(pu8 + uUsed, &stHead);
    if (stHead.u32Len > pstDmc->stHost.u32FrameMax)
    {
      return (size_t)-1;
    }
    if ((uLen - uUsed - DM_FRAME_HEAD) < (size_t)stHead.u32Len)
    {
      break;
    }
    if (!dmc__on_frame(pstDmc, &stHead, pu8 + uUsed + DM_FRAME_HEAD))
    {
      return (size_t)-1;
    }
    uUsed += DM_FRAME_HEAD + (size_t)stHead.u32Len;
    if (pstDmc->bBroken || !pstDmc->bConnected || (pstDmc->u32ConnGen != u32Gen))
    {
      break;
    }
  }
  return uUsed;
}

static bool_t dmc__rx_append(io dmc_st *pstDmc, in const uint8 *pu8, in size_t uLen)
{
  if (uLen > (SIZE_MAX - pstDmc->uRxLen))
  {
    return DM_FALSE;
  }
  if (pstDmc->uRxLen + uLen > pstDmc->uRxCap)
  {
    size_t uNeed = pstDmc->uRxLen + uLen;
    size_t uCap = (0u != pstDmc->uRxCap) ? pstDmc->uRxCap : 4096u;
    uint8 *pu8New = NULL;
    while (uCap < uNeed)
    {
      if (uCap > (SIZE_MAX / 2u))
      {
        uCap = uNeed;
        break;
      }
      uCap *= 2u;
    }
    pu8New = (uint8 *)dmc__realloc_rx(pstDmc, uCap);
    if (NULL == pu8New)
    {
      return DM_FALSE;
    }
    pstDmc->pu8Rx = pu8New;
    pstDmc->uRxCap = uCap;
  }
  memcpy(pstDmc->pu8Rx + pstDmc->uRxLen, pu8, uLen);
  pstDmc->uRxLen += uLen;
  return DM_TRUE;
}

size_t dmc_feed(io dmc_st *pstDmc, in const void *pvData, in size_t uLen)
{
  const uint8 *pu8 = (const uint8 *)pvData;
  size_t uUsed = 0u;
  uint32 u32Gen = 0u;

  if ((NULL == pstDmc) || ((NULL == pvData) && (0u != uLen)) || pstDmc->bInFeed)
  {
    return 0u;
  }
  if (pstDmc->bBroken || !pstDmc->bConnected || (0u == uLen))
  {
    return uLen;
  }
  u32Gen = pstDmc->u32ConnGen;
  pstDmc->bInFeed = DM_TRUE;
  if (0u == pstDmc->uRxLen)
  {
    uUsed = dmc__cut(pstDmc, pu8, uLen, u32Gen);
    if ((size_t)-1 == uUsed)
    {
      dmc__wire_error(pstDmc);
      pstDmc->bInFeed = DM_FALSE;
      return uLen;
    }
    if (pstDmc->bBroken || !pstDmc->bConnected || (pstDmc->u32ConnGen != u32Gen))
    {
      pstDmc->bInFeed = DM_FALSE;
      return uLen;
    }
    pu8 += uUsed;
    uLen -= uUsed;
    if (0u == uLen)
    {
      pstDmc->bInFeed = DM_FALSE;
      return uUsed;
    }
    uUsed += uLen;
    if (!dmc__rx_append(pstDmc, pu8, uLen))
    {
      dmc__wire_error(pstDmc);
    }
    pstDmc->bInFeed = DM_FALSE;
    return uUsed;
  }
  if (!dmc__rx_append(pstDmc, pu8, uLen))
  {
    dmc__wire_error(pstDmc);
    pstDmc->bInFeed = DM_FALSE;
    return uLen;
  }
  uUsed = dmc__cut(pstDmc, pstDmc->pu8Rx, pstDmc->uRxLen, u32Gen);
  if ((size_t)-1 == uUsed)
  {
    dmc__wire_error(pstDmc);
    pstDmc->bInFeed = DM_FALSE;
    return uLen;
  }
  if (pstDmc->bBroken || !pstDmc->bConnected || (pstDmc->u32ConnGen != u32Gen))
  {
    pstDmc->bInFeed = DM_FALSE;
    return uLen;
  }
  if ((0u != uUsed) && (uUsed <= pstDmc->uRxLen))
  {
    memmove(pstDmc->pu8Rx, pstDmc->pu8Rx + uUsed, pstDmc->uRxLen - uUsed);
    pstDmc->uRxLen -= uUsed;
  }
  pstDmc->bInFeed = DM_FALSE;
  return uLen;
}

#ifdef __cplusplus
}
#endif
