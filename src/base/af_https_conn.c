/**
 ******************************************************************************
 * @file    af_https_conn.c
 * @author  Afar_C
 * @version 2.0.0
 * @brief   单连接 TLS 1.3/1.2、mbedTLS 内存 BIO、llhttp 解析、写出与双级限流（仅 T_https 线程）。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "base/af_https_int.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ============================ 响应构造、缓冲与内存 BIO ============================ */

static const char *af__https_status_reason(dm_in uint uiStatus)
{
  static const uint auCodes[] = {200u, 400u, 401u, 403u, 404u, 405u,
                                 409u, 412u, 413u, 426u, 429u, 503u};
  static const char *const apcStr[] = {"OK",
                                       "Bad Request",
                                       "Unauthorized",
                                       "Forbidden",
                                       "Not Found",
                                       "Method Not Allowed",
                                       "Conflict",
                                       "Precondition Failed",
                                       "Payload Too Large",
                                       "Upgrade Required",
                                       "Too Many Requests",
                                       "Service Unavailable"};
  uint i = 0u;

  for (i = 0u; i < ARRAY_SIZE(auCodes); i++)
  {
    if (auCodes[i] == uiStatus)
    {
      return apcStr[i];
    }
  }
  return "Internal Server Error";
}

int af__https_build_resp(dm_in uint uiStatus, dm_in const void *pvBody, dm_in uint32 u32Len,
                         dm_out uint8 **ppu8Out, dm_out uint32 *pu32OutLen)
{
  char acHdr[256];
  int iHdrLen = 0;
  uint32 u32Total = 0u;
  uint8 *pu8Resp = NULL;

  *ppu8Out = NULL;
  *pu32OutLen = 0u;
  iHdrLen = snprintf(acHdr, sizeof(acHdr),
                     "HTTP/1.1 %u %s\r\nContent-Type: application/json\r\n"
                     "Content-Length: %u\r\nConnection: close\r\n\r\n",
                     uiStatus, af__https_status_reason(uiStatus), u32Len);
  if ((iHdrLen <= 0) || ((size_t)iHdrLen >= sizeof(acHdr)) ||
      (u32Len > (0xffffffffu - (uint32)iHdrLen)))
  {
    return DM_OVERFLOW;
  }
  u32Total = (uint32)iHdrLen + u32Len;
  pu8Resp = (uint8 *)dm_malloc_tag(u32Total, DM_MEM_TAG_HTTP);
  if (NULL == pu8Resp)
  {
    return DM_ALLOC;
  }
  memcpy(pu8Resp, acHdr, (size_t)iHdrLen);
  if ((u32Len > 0u) && (NULL != pvBody))
  {
    memcpy(pu8Resp + iHdrLen, pvBody, u32Len);
  }
  *ppu8Out = pu8Resp;
  *pu32OutLen = u32Total;
  return DM_SUCCESS;
}

static int af__https_secure_grow(dm_io void **ppvPtr, dm_in uint32 u32OldCap,
                                 dm_in uint32 u32NewCap)
{
  void *pvNew = dm_malloc_tag(u32NewCap, DM_MEM_TAG_HTTP);

  if (NULL == pvNew)
  {
    return DM_ALLOC;
  }
  if ((NULL != *ppvPtr) && (u32OldCap > 0u))
  {
    uint32 u32Copy = (u32OldCap < u32NewCap) ? u32OldCap : u32NewCap;
    memcpy(pvNew, *ppvPtr, u32Copy);
    dm_secure_zero(*ppvPtr, u32OldCap);
    dm_free_tag(*ppvPtr);
  }
  *ppvPtr = pvNew;
  return DM_SUCCESS;
}

static uint32 af__https_grow_cap(dm_in uint32 u32Old, dm_in uint32 u32Need, dm_in uint32 u32Init)
{
  uint32 u32Cap = (0u == u32Old) ? u32Init : u32Old;

  while (u32Cap < u32Need)
  {
    if (u32Cap > (0xffffffffu / 2u))
    {
      return u32Need;
    }
    u32Cap *= 2u;
  }
  return u32Cap;
}

static int af__https_append_nul_buf(dm_io void **ppvBuf, dm_io uint32 *pu32Len,
                                    dm_io uint32 *pu32Cap, dm_in const char *pcAt,
                                    dm_in size_t uLen, dm_in uint32 u32InitCap)
{
  uint32 u32Need = 0u;
  uint8 *pu8Dst = NULL;

  if ((uLen > (size_t)(0xffffffffu - 1u)) || (*pu32Len > (0xffffffffu - 1u - (uint32)uLen)))
  {
    return DM_OVERFLOW;
  }
  u32Need = *pu32Len + (uint32)uLen + 1u;
  if (u32Need > *pu32Cap)
  {
    uint32 u32Cap = af__https_grow_cap(*pu32Cap, u32Need, u32InitCap);
    if (DM_SUCCESS != af__https_secure_grow(ppvBuf, *pu32Cap, u32Cap))
    {
      return DM_ALLOC;
    }
    *pu32Cap = u32Cap;
  }
  pu8Dst = (uint8 *)(*ppvBuf);
  memcpy(pu8Dst + *pu32Len, pcAt, uLen);
  *pu32Len += (uint32)uLen;
  pu8Dst[*pu32Len] = 0u;
  return DM_SUCCESS;
}

static int af__https_buf_append(dm_io af__https_buf_st *pstB, dm_in const uint8 *pu8Src,
                                dm_in uint32 u32Len, dm_in uint32 u32Limit)
{
  uint32 u32Avail = pstB->u32Len - pstB->u32Off;
  uint32 u32Need = 0u;

  if (0u == u32Len)
  {
    return DM_SUCCESS;
  }
  if (u32Len > (0xffffffffu - u32Avail))
  {
    return DM_OVERFLOW;
  }
  if ((0u != u32Limit) && ((u32Avail > u32Limit) || (u32Len > (u32Limit - u32Avail))))
  {
    return DM_TOO_BIG;
  }
  if ((pstB->u32Off > 0u) && (u32Len > (pstB->u32Cap - pstB->u32Len)))
  {
    if (u32Avail > 0u)
    {
      memmove(pstB->pu8Data, pstB->pu8Data + pstB->u32Off, u32Avail);
    }
    dm_secure_zero(pstB->pu8Data + u32Avail, pstB->u32Len - u32Avail);
    pstB->u32Off = 0u;
    pstB->u32Len = u32Avail;
  }
  u32Need = pstB->u32Len + u32Len;
  if (u32Need > pstB->u32Cap)
  {
    uint32 u32NewCap = af__https_grow_cap(pstB->u32Cap, u32Need, 4096u);
    if (DM_SUCCESS != af__https_secure_grow((void **)&pstB->pu8Data, pstB->u32Cap, u32NewCap))
    {
      return DM_ALLOC;
    }
    pstB->u32Cap = u32NewCap;
  }
  memcpy(pstB->pu8Data + pstB->u32Len, pu8Src, u32Len);
  pstB->u32Len += u32Len;
  return DM_SUCCESS;
}

static void af__https_buf_free_zero(dm_io af__https_buf_st *pstB)
{
  if (NULL != pstB->pu8Data)
  {
    dm_secure_zero(pstB->pu8Data, pstB->u32Cap);
    dm_free_tag(pstB->pu8Data);
    pstB->pu8Data = NULL;
  }
  pstB->u32Len = 0u;
  pstB->u32Cap = 0u;
  pstB->u32Off = 0u;
}

static int af__https_bio_send(dm_io void *pvCtx, dm_in const unsigned char *pucBuf,
                              dm_in size_t uLen)
{
  af__https_conn_st *pstC = (af__https_conn_st *)pvCtx;
  return (DM_SUCCESS == af__https_buf_append(&pstC->stOut, pucBuf, (uint32)uLen, 0u))
             ? (int)uLen
             : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int af__https_bio_recv(dm_io void *pvCtx, dm_out unsigned char *pucBuf, dm_in size_t uLen)
{
  af__https_conn_st *pstC = (af__https_conn_st *)pvCtx;
  uint32 u32Avail = pstC->stIn.u32Len - pstC->stIn.u32Off;
  uint32 u32Take = 0u;

  if (0u == u32Avail)
  {
    return MBEDTLS_ERR_SSL_WANT_READ;
  }
  u32Take = (uLen < (size_t)u32Avail) ? (uint32)uLen : u32Avail;
  memcpy(pucBuf, pstC->stIn.pu8Data + pstC->stIn.u32Off, u32Take);
  pstC->stIn.u32Off += u32Take;
  if (pstC->stIn.u32Off == pstC->stIn.u32Len)
  {
    pstC->stIn.u32Off = 0u;
    pstC->stIn.u32Len = 0u;
  }
  return (int)u32Take;
}

static void af__https_on_write_done(dm_io dm_tcp_st *pstTcp, dm_in int iErr, dm_io void *pvUser)
{
  af__https_conn_st *pstC = (af__https_conn_st *)pvUser;

  (void)pstTcp;
  if ((DM_SUCCESS != iErr) && (NULL != pstC) && (!pstC->bClosing))
  {
    af__https_conn_close(pstC);
  }
}

static int af__https_flush_out(dm_io af__https_conn_st *pstC)
{
  uint32 u32Avail = pstC->stOut.u32Len - pstC->stOut.u32Off;
  int iErr = DM_SUCCESS;

  if ((0u == u32Avail) || pstC->bClosing || (NULL == pstC->pstTcp))
  {
    return DM_SUCCESS;
  }
  if ((dm_tcp_write_queue_size(pstC->pstTcp) + (size_t)u32Avail) > AF_HTTPS_TX_QUEUE_MAX)
  {
    af__https_conn_close(pstC);
    return DM_OVERFLOW;
  }
  iErr = dm_tcp_write(pstC->pstTcp, pstC->stOut.pu8Data + pstC->stOut.u32Off, u32Avail,
                      af__https_on_write_done, pstC);
  dm_secure_zero(pstC->stOut.pu8Data + pstC->stOut.u32Off, u32Avail);
  pstC->stOut.u32Off = 0u;
  pstC->stOut.u32Len = 0u;
  if (DM_SUCCESS != iErr)
  {
    af__https_conn_close(pstC);
    return DM_IO;
  }
  return DM_SUCCESS;
}

/* ============================ 连接关闭与超时 ============================ */

static void af__https_conn_on_all_closed(dm_io af__https_conn_st *pstC)
{
  af__https_buf_free_zero(&pstC->stIn);
  af__https_buf_free_zero(&pstC->stOut);
  if (pstC->bSslInit)
  {
    mbedtls_ssl_free(&pstC->stSsl);
    pstC->bSslInit = DM_FALSE;
  }
  if (pstC->bHasSlot)
  {
    af__https_slot_on_conn_closed(pstC->u32Slot);
  }
  dm_free_tag(pstC);
}

static void af__https_conn_on_handle_closed(dm_io void *pvUser)
{
  af__https_conn_st *pstC = (af__https_conn_st *)pvUser;

  if ((NULL != pstC) && (pstC->u8ClosePending > 0u))
  {
    pstC->u8ClosePending--;
    if (0u == pstC->u8ClosePending)
    {
      af__https_conn_on_all_closed(pstC);
    }
  }
}

void af__https_conn_close(dm_io af__https_conn_st *pstC)
{
  dm_tcp_st *pstTcp = NULL;
  dm_loop_timer_st *pstTimer = NULL;

  if ((NULL == pstC) || pstC->bClosing)
  {
    return;
  }
  pstC->bClosing = DM_TRUE;
  if (pstC->bHasSlot && (pstC->u32Slot < AF_HTTPS_SLOTS_MAX))
  {
    dm_mutex_lock(g_stAfHttpsSrv.pstStateMtx);
    g_stAfHttpsSrv.astSlots[pstC->u32Slot].eState = AF_HTTPS_SLOT_CLOSING;
    g_stAfHttpsSrv.astSlots[pstC->u32Slot].bRespPending = DM_FALSE;
    dm_mutex_unlock(g_stAfHttpsSrv.pstStateMtx);
  }
  pstTcp = pstC->pstTcp;
  pstTimer = pstC->pstTimer;
  pstC->pstTcp = NULL;
  pstC->pstTimer = NULL;
  pstC->u8ClosePending = (uint8)(((NULL != pstTcp) ? 1u : 0u) + ((NULL != pstTimer) ? 1u : 0u));
  if (0u == pstC->u8ClosePending)
  {
    af__https_conn_on_all_closed(pstC);
    return;
  }
  (void)dm_loop_timer_close(pstTimer, af__https_conn_on_handle_closed, pstC);
  (void)dm_tcp_close(pstTcp, af__https_conn_on_handle_closed, pstC);
}

static void af__https_on_timeout(dm_io dm_loop_timer_st *pstTimer, dm_io void *pvUser)
{
  (void)pstTimer;
  af__https_conn_close((af__https_conn_st *)pvUser);
}

static void af__https_on_shutdown(dm_io dm_tcp_st *pstTcp, dm_in int iErr, dm_io void *pvUser)
{
  (void)pstTcp;
  (void)iErr;
  af__https_conn_close((af__https_conn_st *)pvUser);
}

/* ============================ llhttp 回调 ============================ */

static int af__http_on_url(dm_io llhttp_t *pstP, dm_in const char *pcAt, dm_in size_t uLen)
{
  af__https_conn_st *pstC = (af__https_conn_st *)pstP->data;
  af__https_req_st *pstReq = &g_stAfHttpsSrv.astSlots[pstC->u32Slot].stReq;

  if ((pstReq->u32PathLen + uLen) >= AF_HTTPS_PATH_MAX)
  {
    return HPE_USER;
  }
  memcpy(pstReq->acPath + pstReq->u32PathLen, pcAt, uLen);
  pstReq->u32PathLen += (uint32)uLen;
  pstReq->acPath[pstReq->u32PathLen] = '\0';
  return 0;
}

static int af__http_on_hdr_field(dm_io llhttp_t *pstP, dm_in const char *pcAt, dm_in size_t uLen)
{
  af__https_conn_st *pstC = (af__https_conn_st *)pstP->data;
  size_t uIdx = 0u;

  for (uIdx = 0u; (uIdx < uLen) && (pstC->u32CurHdrNameLen < (sizeof(pstC->acCurHdrName) - 1u));
       uIdx++)
  {
    pstC->acCurHdrName[pstC->u32CurHdrNameLen++] = (char)tolower((unsigned char)pcAt[uIdx]);
  }
  pstC->acCurHdrName[pstC->u32CurHdrNameLen] = '\0';
  return 0;
}

static int af__http_on_hdr_field_done(dm_io llhttp_t *pstP)
{
  af__https_conn_st *pstC = (af__https_conn_st *)pstP->data;

  pstC->bCurHdrAuth = (0 == strcmp(pstC->acCurHdrName, "authorization")) ? DM_TRUE : DM_FALSE;
  pstC->u32CurHdrNameLen = 0u;
  pstC->acCurHdrName[0] = '\0';
  return (pstC->bCurHdrAuth && pstC->bSeenAuth) ? HPE_USER : 0;
}

static int af__http_on_hdr_value_done(dm_io llhttp_t *pstP)
{
  af__https_conn_st *pstC = (af__https_conn_st *)pstP->data;

  pstC->bSeenAuth = (pstC->bSeenAuth || pstC->bCurHdrAuth) ? DM_TRUE : DM_FALSE;
  pstC->bCurHdrAuth = DM_FALSE;
  return 0;
}

static int af__http_on_hdr_value(dm_io llhttp_t *pstP, dm_in const char *pcAt, dm_in size_t uLen)
{
  af__https_conn_st *pstC = (af__https_conn_st *)pstP->data;
  af__https_req_st *pstReq = &g_stAfHttpsSrv.astSlots[pstC->u32Slot].stReq;

  if (!pstC->bCurHdrAuth)
  {
    return 0;
  }
  if ((pstReq->u32AuthLen + uLen) >= AF_HTTPS_AUTH_MAX)
  {
    return HPE_USER;
  }
  return (DM_SUCCESS == af__https_append_nul_buf((void **)&pstReq->pcAuthBuf, &pstReq->u32AuthLen,
                                                 &pstReq->u32AuthCap, pcAt, uLen, 256u))
             ? 0
             : HPE_USER;
}

static int af__http_on_body(dm_io llhttp_t *pstP, dm_in const char *pcAt, dm_in size_t uLen)
{
  af__https_conn_st *pstC = (af__https_conn_st *)pstP->data;
  af__https_req_st *pstReq = &g_stAfHttpsSrv.astSlots[pstC->u32Slot].stReq;
  uint32 u32Max = g_stAfHttpsSrv.stCfg.u32BodyMax;

  if ((uLen > u32Max) || (pstReq->u32BodyLen > (u32Max - (uint32)uLen)))
  {
    pstC->bTooBig = DM_TRUE;
    return HPE_USER;
  }
  return (DM_SUCCESS == af__https_append_nul_buf((void **)&pstReq->pu8Body, &pstReq->u32BodyLen,
                                                 &pstReq->u32BodyCap, pcAt, uLen, 1024u))
             ? 0
             : HPE_USER;
}

static int af__http_on_msg_done(dm_io llhttp_t *pstP)
{
  af__https_conn_st *pstC = (af__https_conn_st *)pstP->data;
  af__https_req_st *pstReq = &g_stAfHttpsSrv.astSlots[pstC->u32Slot].stReq;
  const char *pcMeth = llhttp_method_name((llhttp_method_t)pstP->method);

  snprintf(pstReq->acMethod, sizeof(pstReq->acMethod), "%s", (NULL != pcMeth) ? pcMeth : "");
  pstC->bReqDone = DM_TRUE;
  return HPE_PAUSED;
}

/* ============================ TLS 读写与响应写出 ============================ */

void af__https_conn_send_resp(dm_io af__https_conn_st *pstC, dm_in const uint8 *pu8Resp,
                              dm_in uint32 u32Len)
{
  uint32 u32Off = 0u;

  if ((NULL == pstC) || pstC->bClosing || (NULL == pstC->pstTcp))
  {
    return;
  }
  pstC->bReqDone = DM_TRUE;
  (void)dm_tcp_read_stop(pstC->pstTcp);
  if (NULL != pstC->pstTimer)
  {
    (void)dm_loop_timer_stop(pstC->pstTimer);
  }
  while ((u32Off < u32Len) && (!pstC->bClosing))
  {
    int iWr = mbedtls_ssl_write(&pstC->stSsl, pu8Resp + u32Off, u32Len - u32Off);
    (void)af__https_flush_out(pstC);
    if ((iWr <= 0) || pstC->bClosing)
    {
      af__https_conn_close(pstC);
      return;
    }
    u32Off += (uint32)iWr;
  }
  (void)mbedtls_ssl_close_notify(&pstC->stSsl);
  (void)af__https_flush_out(pstC);
  if (pstC->bClosing)
  {
    return;
  }
  if (NULL != pstC->pstTimer)
  {
    (void)dm_loop_timer_start(pstC->pstTimer, g_stAfHttpsSrv.stCfg.u32IdleMs, 0u,
                              af__https_on_timeout, pstC);
  }
  pstC->bShutPending = DM_TRUE;
  if (DM_SUCCESS != dm_tcp_shutdown(pstC->pstTcp, af__https_on_shutdown, pstC))
  {
    af__https_conn_close(pstC);
  }
}

static void af__https_conn_reply_local(dm_io af__https_conn_st *pstC, dm_in uint uiStatus,
                                       dm_in const char *pcBody, dm_in uint32 u32Len)
{
  uint8 *pu8Resp = NULL;
  uint32 u32RespLen = 0u;
  bool_t bOk = DM_FALSE;

  if (DM_SUCCESS != af__https_build_resp(uiStatus, pcBody, u32Len, &pu8Resp, &u32RespLen))
  {
    af__https_conn_close(pstC);
    return;
  }
  dm_mutex_lock(g_stAfHttpsSrv.pstStateMtx);
  if ((!g_stAfHttpsSrv.bStop) && (pstC->u32Slot < AF_HTTPS_SLOTS_MAX) &&
      (AF_HTTPS_SLOT_OPEN == g_stAfHttpsSrv.astSlots[pstC->u32Slot].eState))
  {
    af__https_slot_st *pstS = &g_stAfHttpsSrv.astSlots[pstC->u32Slot];
    pstS->stResp = (af__https_resp_st){pu8Resp, u32RespLen};
    pstS->bReplied = DM_TRUE;
    pstS->eState = AF_HTTPS_SLOT_REPLIED;
    bOk = DM_TRUE;
  }
  dm_mutex_unlock(g_stAfHttpsSrv.pstStateMtx);
  if (bOk)
  {
    af__https_conn_send_resp(pstC, pu8Resp, u32RespLen);
    return;
  }
  dm_secure_zero(pu8Resp, u32RespLen);
  dm_free_tag(pu8Resp);
  af__https_conn_close(pstC);
}

static void af__https_dispatch_req(dm_io af__https_conn_st *pstC)
{
  if (NULL != pstC->pstTimer)
  {
    (void)dm_loop_timer_stop(pstC->pstTimer);
    (void)dm_loop_timer_start(pstC->pstTimer, g_stAfHttpsSrv.stCfg.u32ReplyMs, 0u,
                              af__https_on_timeout, pstC);
  }
  if (DM_SUCCESS != af__https_slot_dispatch(pstC->u32Slot, pstC->u32Gen))
  {
    af__https_conn_close(pstC);
  }
}

static bool_t af__https_step_handshake(dm_io af__https_conn_st *pstC)
{
  int iRet = 0;

  if (pstC->bHandshook)
  {
    return DM_TRUE;
  }
  iRet = mbedtls_ssl_handshake(&pstC->stSsl);
  (void)af__https_flush_out(pstC);
  if (pstC->bClosing || (MBEDTLS_ERR_SSL_WANT_READ == iRet) || (MBEDTLS_ERR_SSL_WANT_WRITE == iRet))
  {
    return DM_FALSE;
  }
  if (0 != iRet)
  {
    af__https_conn_close(pstC);
    return DM_FALSE;
  }
  pstC->bHandshook = DM_TRUE;
  if (NULL != pstC->pstTimer)
  {
    (void)dm_loop_timer_stop(pstC->pstTimer);
    (void)dm_loop_timer_start(pstC->pstTimer, g_stAfHttpsSrv.stCfg.u32IdleMs, 0u,
                              af__https_on_timeout, pstC);
  }
  return DM_TRUE;
}

static void af__https_step_tls(dm_io af__https_conn_st *pstC)
{
  uint8 au8Plain[4096];

  if (!af__https_step_handshake(pstC))
  {
    return;
  }
  /* 握手完成立即 fall-through 排空明文（兼容 TLS 1.3 Finished 与 HTTP 拼包） */
  while ((!pstC->bClosing) && (!pstC->bReqDone))
  {
    int iRet = mbedtls_ssl_read(&pstC->stSsl, au8Plain, sizeof(au8Plain));
    llhttp_errno_t eErr = HPE_OK;
    (void)af__https_flush_out(pstC);
    if (pstC->bClosing || (MBEDTLS_ERR_SSL_WANT_READ == iRet) ||
        (MBEDTLS_ERR_SSL_WANT_WRITE == iRet))
    {
      dm_secure_zero(au8Plain, sizeof(au8Plain));
      return;
    }
    if (iRet <= 0)
    {
      af__https_conn_close(pstC);
      return;
    }
    eErr = llhttp_execute(&pstC->stParser, (const char *)au8Plain, (size_t)iRet);
    dm_secure_zero(au8Plain, (size_t)iRet);
    if (pstC->bReqDone)
    {
      af__https_dispatch_req(pstC);
      return;
    }
    if (HPE_OK != eErr)
    {
      const char *pcBody = pstC->bTooBig ? "{\"code\":\"too_big\"}" : "{\"code\":\"bad_request\"}";
      af__https_conn_reply_local(pstC, pstC->bTooBig ? 413u : 400u, pcBody, (uint32)strlen(pcBody));
      return;
    }
  }
}

static void af__https_drain_post_req(dm_io af__https_conn_st *pstC)
{
  uint8 au8Discard[64];
  int i = 0;

  for (i = 0; (i < 16) && (!pstC->bClosing) &&
              ((pstC->stIn.u32Len > pstC->stIn.u32Off) ||
               (mbedtls_ssl_get_bytes_avail(&pstC->stSsl) > 0u));
       i++)
  {
    int iRd = mbedtls_ssl_read(&pstC->stSsl, au8Discard, sizeof(au8Discard));
    dm_secure_zero(au8Discard, sizeof(au8Discard));
    (void)af__https_flush_out(pstC);
    if (iRd > 0)
    {
      pstC->u32PostJunk += (uint32)iRd;
      if (pstC->u32PostJunk > 1024u)
      {
        af__https_conn_close(pstC);
        return;
      }
      continue;
    }
    if ((MBEDTLS_ERR_SSL_WANT_READ != iRd) && (MBEDTLS_ERR_SSL_WANT_WRITE != iRd))
    {
      af__https_conn_close(pstC);
    }
    return;
  }
}

static void af__https_on_read(dm_io dm_tcp_st *pstTcp, dm_in int iErr, dm_in const uint8 *pu8Data,
                              dm_in size_t uLen, dm_io void *pvUser)
{
  af__https_conn_st *pstC = (af__https_conn_st *)pvUser;

  (void)pstTcp;
  if ((NULL == pstC) || pstC->bClosing ||
      ((DM_SUCCESS == iErr) && ((NULL == pu8Data) || (0u == uLen))))
  {
    return;
  }
  if (DM_SUCCESS != iErr)
  {
    if ((!pstC->bReqDone) || (DM_EOF != iErr))
    {
      af__https_conn_close(pstC);
    }
    return;
  }
  if (DM_SUCCESS != af__https_buf_append(&pstC->stIn, pu8Data, (uint32)uLen, AF_HTTPS_IN_BIO_MAX))
  {
    af__https_conn_close(pstC);
    return;
  }
  if (!pstC->bReqDone)
  {
    af__https_step_tls(pstC);
  }
  else
  {
    af__https_drain_post_req(pstC);
  }
}

/* ============================ 接入与双级限流 ============================ */

static void af__https_norm_peer_ip(dm_in const dm_sockaddr_st *pstAddr, dm_out uint8 au8Out[16])
{
  memset(au8Out, 0, 16u);
  if (DM_AF_IPV4 == pstAddr->eFamily)
  {
    au8Out[10] = 0xffu;
    au8Out[11] = 0xffu;
    memcpy(au8Out + 12, pstAddr->au8Addr, 4u);
  }
  else if (DM_AF_IPV6 == pstAddr->eFamily)
  {
    memcpy(au8Out, pstAddr->au8Addr, 16u);
  }
}

static bool_t af__https_can_accept(dm_in const uint8 au8Ip[16])
{
  uint32 u32Idx = 0u;
  uint32 u32All = 0u;
  uint32 u32IpAct = 0u;
  uint32 u32IpSlots = 0u;
  uint32 u32CapSlots = g_stAfHttpsSrv.stCfg.u32MaxPerIp * 2u;

  for (u32Idx = 0u; u32Idx < AF_HTTPS_SLOTS_MAX; u32Idx++)
  {
    af__https_conn_st *pstC = g_stAfHttpsSrv.astSlots[u32Idx].pstConn;
    if (NULL == pstC)
    {
      continue;
    }
    if (0 == memcmp(pstC->au8PeerIp, au8Ip, 16u))
    {
      u32IpSlots++;
      if ((!pstC->bShutPending) && (!pstC->bClosing))
      {
        u32IpAct++;
      }
    }
    if ((!pstC->bShutPending) && (!pstC->bClosing))
    {
      u32All++;
    }
  }
  return ((u32All < g_stAfHttpsSrv.stCfg.u32MaxConns) &&
          (u32IpAct < g_stAfHttpsSrv.stCfg.u32MaxPerIp) && (u32IpSlots < u32CapSlots))
             ? DM_TRUE
             : DM_FALSE;
}

static int af__https_setup_conn(dm_io af__https_conn_st *pstC)
{
  if (DM_SUCCESS != dm_loop_timer_create(g_stAfHttpsSrv.pstLoop, &pstC->pstTimer))
  {
    return DM_FAILD;
  }
  mbedtls_ssl_init(&pstC->stSsl);
  pstC->bSslInit = DM_TRUE;
  if (0 != mbedtls_ssl_setup(&pstC->stSsl, &g_stAfHttpsSrv.stSslCfg))
  {
    return DM_FAILD;
  }
  mbedtls_ssl_set_bio(&pstC->stSsl, pstC, af__https_bio_send, af__https_bio_recv, NULL);
  llhttp_settings_init(&pstC->stParseSet);
  pstC->stParseSet.on_url = af__http_on_url;
  pstC->stParseSet.on_header_field = af__http_on_hdr_field;
  pstC->stParseSet.on_header_field_complete = af__http_on_hdr_field_done;
  pstC->stParseSet.on_header_value = af__http_on_hdr_value;
  pstC->stParseSet.on_header_value_complete = af__http_on_hdr_value_done;
  pstC->stParseSet.on_body = af__http_on_body;
  pstC->stParseSet.on_message_complete = af__http_on_msg_done;
  llhttp_init(&pstC->stParser, HTTP_REQUEST, &pstC->stParseSet);
  pstC->stParser.data = pstC;
  if (DM_SUCCESS != af__https_slot_alloc(pstC, &pstC->u32Slot, &pstC->u32Gen))
  {
    return DM_BUSY;
  }
  pstC->bHasSlot = DM_TRUE;
  (void)dm_loop_timer_start(pstC->pstTimer, g_stAfHttpsSrv.stCfg.u32HandshakeMs, 0u,
                            af__https_on_timeout, pstC);
  if (DM_SUCCESS != dm_tcp_read_start(pstC->pstTcp, af__https_on_read, pstC))
  {
    af__https_conn_close(pstC);
  }
  return DM_SUCCESS;
}

void af__https_on_new_conn(dm_io dm_tcp_st *pstServer, dm_in int iErr, dm_io void *pvUser)
{
  dm_tcp_st *pstCli = NULL;
  dm_sockaddr_st stPeer = {0};
  af__https_conn_st *pstC = NULL;
  uint8 au8PeerIp[16] = {0};

  (void)pvUser;
  if ((DM_SUCCESS != iErr) || (NULL == g_stAfHttpsSrv.pstListen) ||
      (DM_SUCCESS != dm_tcp_accept(pstServer, &pstCli)))
  {
    return;
  }
  if (DM_SUCCESS == dm_tcp_getpeername(pstCli, &stPeer))
  {
    af__https_norm_peer_ip(&stPeer, au8PeerIp);
  }
  if (af__https_can_accept(au8PeerIp))
  {
    pstC = (af__https_conn_st *)dm_calloc_tag(1u, sizeof(*pstC), DM_MEM_TAG_HTTP);
  }
  if (NULL == pstC)
  {
    (void)dm_tcp_close(pstCli, NULL, NULL);
    return;
  }
  pstC->pstTcp = pstCli;
  memcpy(pstC->au8PeerIp, au8PeerIp, sizeof(au8PeerIp));
  if (DM_SUCCESS != af__https_setup_conn(pstC))
  {
    af__https_conn_close(pstC);
  }
}

#ifdef __cplusplus
}
#endif
