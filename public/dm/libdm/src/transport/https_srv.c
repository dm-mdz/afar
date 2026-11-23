/**
 ******************************************************************************
 * @file    https_srv.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   独立局域网单连接单请求 TLS HTTPS 服务（零修改 conn.c/http.c/ws.c）。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "https"
#include <dm.h>
#include <dm/https.h>
#include <dm/log.h>
#include <dm/module.h>
#include "../dm_internal.h"
#include "../infra/fs/fs_internal.h"
#include "../uv_adapter.h"

#include <llhttp.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define DM_HTTPS_ROUTES_MAX    32u
#define DM_HTTPS_SLOTS_MAX     64u
#define DM_HTTPS_IN_BIO_MAX    65536u
#define DM_HTTPS_AUTH_MAX      4096u
#define DM_HTTPS_PATH_MAX      256u
#define DM_HTTPS_HDR_VAL_MAX   4096u
#define DM_HTTPS_TX_QUEUE_MAX  (4u * 1024u * 1024u)

typedef struct dm__https_route
{
  char acMethod[12];
  char acPath[DM_HTTPS_PATH_MAX];
  dm_https_handler_fn pfHandler;
} dm__https_route_st;

typedef struct dm__https_buf
{
  uint8 *pu8Data;
  uint32 u32Len;
  uint32 u32Cap;
  uint32 u32Off;
} dm__https_buf_st;

typedef struct dm__https_tx
{
  dm_write_st stReq;
  uint8 *pu8Buf;
  uint32 u32Len;
  struct dm__https_conn *pstConn;
} dm__https_tx_st;

typedef struct dm__https_conn
{
  dm_tcp_st stTcp; /* 首成员：绝不搬移 uv_tcp_t */
  dm_timer_st stTimer;
  dm_shutdown_st stShut;
  uint32 u32Slot;
  uint32 u32Gen;
  uint8 au8PeerIp[16];
  bool_t bCounted;
  bool_t bHandshook;
  bool_t bReqDone;
  bool_t bReplied;
  bool_t bClosing;
  bool_t bTooBig;
  bool_t bShutPending;
  uint32 u32TxBytes;

  mbedtls_ssl_context stSsl;
  dm__https_buf_st stIn;
  dm__https_buf_st stOut;

  llhttp_t stParser;
  llhttp_settings_t stParseSet;
  char acMethod[12];
  char acPath[DM_HTTPS_PATH_MAX];
  uint32 u32PathLen;
  bool_t bCurHdrAuth;
  bool_t bSeenAuth;
  char acCurHdrName[32];
  uint32 u32CurHdrNameLen;
  char *pcAuthBuf;
  uint32 u32AuthLen;
  uint32 u32AuthCap;
  uint8 *pu8Body;
  uint32 u32BodyLen;
  uint32 u32BodyCap;
  uint8 *pu8Resp;
  uint32 u32RespLen;
} dm__https_conn_st;

static bool_t g_bInited = DM_FALSE;
static bool_t g_bTlsReady = DM_FALSE;
static dm_https_cfg_st g_stCfg;
static dm_tcp_st *g_pstListen = NULL;
static uint16 g_u16BoundPort = 0;

static mbedtls_entropy_context g_stEntropy;
static mbedtls_ctr_drbg_context g_stDrbg;
static mbedtls_x509_crt g_stCert;
static mbedtls_pk_context g_stKey;
static mbedtls_ssl_config g_stSslCfg;

static dm__https_route_st g_astRoutes[DM_HTTPS_ROUTES_MAX];
static uint32 g_u32RouteCnt = 0;
static dm__https_conn_st *g_apstSlots[DM_HTTPS_SLOTS_MAX];
static uint32 g_u32ConnCnt = 0;
static uint32 g_u32NextGen = 1u;
static dm_https_on_free_fn g_pfTestOnFree = NULL;

static void dm__https_conn_close(io dm__https_conn_st *pstC);

/* ============================ 缓冲与辅助 ============================ */

static int dm__secure_grow(io void **ppvPtr, in uint32 u32OldCap, in uint32 u32NewCap)
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

static int dm__buf_append(io dm__https_buf_st *pstB, in const uint8 *pu8Src, in uint32 u32Len,
                          in uint32 u32Limit)
{
  uint32 u32Avail = pstB->u32Len - pstB->u32Off;
  uint32 u32Need = 0;
  uint32 u32NewCap = 0;

  if (0u == u32Len)
  {
    return DM_SUCCESS;
  }
  if ((0u != u32Limit) && ((u32Avail > u32Limit) || (u32Len > (u32Limit - u32Avail))))
  {
    return DM_TOO_BIG;
  }
  if ((pstB->u32Off > 0u) && ((pstB->u32Len + u32Len) > pstB->u32Cap))
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
    u32NewCap = (0u == pstB->u32Cap) ? 4096u : pstB->u32Cap;
    while (u32NewCap < u32Need)
    {
      u32NewCap *= 2u;
    }
    if (DM_SUCCESS != dm__secure_grow((void **)&pstB->pu8Data, pstB->u32Cap, u32NewCap))
    {
      return DM_ALLOC;
    }
    pstB->u32Cap = u32NewCap;
  }
  memcpy(pstB->pu8Data + pstB->u32Len, pu8Src, u32Len);
  pstB->u32Len += u32Len;
  return DM_SUCCESS;
}

static void dm__buf_free_zero(io dm__https_buf_st *pstB)
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
  return;
}

static const char *dm__status_reason(in uint uiStatus)
{
  switch (uiStatus)
  {
    case 200u: return "OK";
    case 400u: return "Bad Request";
    case 401u: return "Unauthorized";
    case 403u: return "Forbidden";
    case 404u: return "Not Found";
    case 405u: return "Method Not Allowed";
    case 409u: return "Conflict";
    case 413u: return "Payload Too Large";
    case 426u: return "Upgrade Required";
    case 429u: return "Too Many Requests";
    case 503u: return "Service Unavailable";
    default:   return "Internal Server Error";
  }
}

static void dm__norm_peer_ip(in const struct sockaddr_storage *pstSs, out uint8 au8Out[16])
{
  memset(au8Out, 0, 16u);
  if (AF_INET == pstSs->ss_family)
  {
    const struct sockaddr_in *pst4 = (const struct sockaddr_in *)pstSs;
    au8Out[10] = 0xffu;
    au8Out[11] = 0xffu;
    memcpy(au8Out + 12, &pst4->sin_addr, 4u);
  }
  else if (AF_INET6 == pstSs->ss_family)
  {
    const struct sockaddr_in6 *pst6 = (const struct sockaddr_in6 *)pstSs;
    memcpy(au8Out, &pst6->sin6_addr, 16u);
  }
  return;
}

/* ============================ 内存 BIO 与刷新 ============================ */

static int dm__bio_send(void *pvCtx, const unsigned char *pucBuf, size_t uLen)
{
  dm__https_conn_st *pstC = (dm__https_conn_st *)pvCtx;

  if (DM_SUCCESS != dm__buf_append(&pstC->stOut, pucBuf, (uint32)uLen, 0u))
  {
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  return (int)uLen;
}

static int dm__bio_recv(void *pvCtx, unsigned char *pucBuf, size_t uLen)
{
  dm__https_conn_st *pstC = (dm__https_conn_st *)pvCtx;
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

static void dm__https_on_write_done(dm_write_st *pstReq, int iStatus)
{
  dm__https_tx_st *pstTx = (dm__https_tx_st *)pstReq;
  dm__https_conn_st *pstC = pstTx->pstConn;

  if (NULL != pstC)
  {
    if (pstC->u32TxBytes >= pstTx->u32Len)
    {
      pstC->u32TxBytes -= pstTx->u32Len;
    }
    if ((0 != iStatus) && (!pstC->bClosing))
    {
      dm__https_conn_close(pstC);
    }
  }
  if (NULL != pstTx->pu8Buf)
  {
    dm_secure_zero(pstTx->pu8Buf, pstTx->u32Len);
    dm_free_tag(pstTx->pu8Buf);
  }
  dm_free_tag(pstTx);
  return;
}

static int dm__https_flush_out(io dm__https_conn_st *pstC)
{
  uint32 u32Avail = pstC->stOut.u32Len - pstC->stOut.u32Off;
  dm__https_tx_st *pstTx = NULL;
  dm_buf_st stBuf;
  int iErr = 0;

  if ((0u == u32Avail) || pstC->bClosing)
  {
    return DM_SUCCESS;
  }
  if ((pstC->u32TxBytes + u32Avail) > DM_HTTPS_TX_QUEUE_MAX)
  {
    dm__https_conn_close(pstC);
    return DM_OVERFLOW;
  }
  pstTx = (dm__https_tx_st *)dm_calloc_tag(1u, sizeof(*pstTx), DM_MEM_TAG_HTTP);
  if (NULL == pstTx)
  {
    dm__https_conn_close(pstC);
    return DM_ALLOC;
  }
  pstTx->pu8Buf = (uint8 *)dm_malloc_tag(u32Avail, DM_MEM_TAG_HTTP);
  if (NULL == pstTx->pu8Buf)
  {
    dm_free_tag(pstTx);
    dm__https_conn_close(pstC);
    return DM_ALLOC;
  }
  memcpy(pstTx->pu8Buf, pstC->stOut.pu8Data + pstC->stOut.u32Off, u32Avail);
  pstTx->u32Len = u32Avail;
  pstTx->pstConn = pstC;
  pstC->stOut.u32Off = 0u;
  pstC->stOut.u32Len = 0u;
  pstC->u32TxBytes += u32Avail;
  stBuf = dm_buf_init((char *)pstTx->pu8Buf, u32Avail);
  iErr = dm_write(&pstTx->stReq, (dm_stream_st *)&pstC->stTcp, &stBuf, 1u, dm__https_on_write_done);
  if (0 != iErr)
  {
    pstC->u32TxBytes -= u32Avail;
    dm_secure_zero(pstTx->pu8Buf, u32Avail);
    dm_free_tag(pstTx->pu8Buf);
    dm_free_tag(pstTx);
    dm__https_conn_close(pstC);
    return DM_IO;
  }
  return DM_SUCCESS;
}

/* ============================ 双句柄关闭与整块擦零 ============================ */

static void dm__https_on_closed(dm_handle_st *pstHandle)
{
  dm__https_conn_st *pstC = (dm__https_conn_st *)pstHandle;
  size_t uAuthCap = pstC->u32AuthCap;
  size_t uBodyCap = pstC->u32BodyCap;
  size_t uRespLen = pstC->u32RespLen;

  if (NULL != pstC->pcAuthBuf)
  {
    dm_secure_zero(pstC->pcAuthBuf, uAuthCap);
  }
  if (NULL != pstC->pu8Body)
  {
    dm_secure_zero(pstC->pu8Body, uBodyCap);
  }
  if (NULL != pstC->pu8Resp)
  {
    dm_secure_zero(pstC->pu8Resp, uRespLen);
  }
  if (NULL != g_pfTestOnFree)
  {
    g_pfTestOnFree((const uint8 *)pstC->pcAuthBuf, uAuthCap, pstC->pu8Body, uBodyCap,
                   pstC->pu8Resp, uRespLen);
  }
  if (NULL != pstC->pcAuthBuf)
  {
    dm_free_tag(pstC->pcAuthBuf);
  }
  if (NULL != pstC->pu8Body)
  {
    dm_free_tag(pstC->pu8Body);
  }
  if (NULL != pstC->pu8Resp)
  {
    dm_free_tag(pstC->pu8Resp);
  }
  dm__buf_free_zero(&pstC->stIn);
  dm__buf_free_zero(&pstC->stOut);
  if (pstC->bCounted)
  {
    if ((pstC->u32Slot < DM_HTTPS_SLOTS_MAX) && (g_apstSlots[pstC->u32Slot] == pstC))
    {
      g_apstSlots[pstC->u32Slot] = NULL;
    }
    mbedtls_ssl_free(&pstC->stSsl);
    if (g_u32ConnCnt > 0u)
    {
      g_u32ConnCnt--;
    }
  }
  dm_free_tag(pstC);
  return;
}

static void dm__https_on_timer_closed(dm_handle_st *pstHandle)
{
  dm__https_conn_st *pstC = (dm__https_conn_st *)pstHandle->data;

  dm_close((dm_handle_st *)&pstC->stTcp, dm__https_on_closed);
  return;
}

static void dm__https_conn_close(io dm__https_conn_st *pstC)
{
  if ((NULL == pstC) || pstC->bClosing)
  {
    return;
  }
  pstC->bClosing = DM_TRUE;
  (void)dm_read_stop((dm_stream_st *)&pstC->stTcp);
  if (pstC->bCounted)
  {
    (void)dm_timer_stop(&pstC->stTimer);
    pstC->stTimer.data = pstC;
    dm_close((dm_handle_st *)&pstC->stTimer, dm__https_on_timer_closed);
  }
  else
  {
    dm_close((dm_handle_st *)&pstC->stTcp, dm__https_on_closed);
  }
  return;
}

static void dm__https_on_timeout(dm_timer_st *pstTimer)
{
  dm__https_conn_st *pstC = (dm__https_conn_st *)pstTimer->data;

  dm__https_conn_close(pstC);
  return;
}

static void dm__https_on_shutdown(dm_shutdown_st *pstReq, int iStatus)
{
  dm__https_conn_st *pstC = (dm__https_conn_st *)pstReq->data;

  (void)iStatus;
  dm__https_conn_close(pstC);
  return;
}

/* ============================ llhttp 回调 ============================ */

static int dm__http_on_url(llhttp_t *pstP, const char *pcAt, size_t uLen)
{
  dm__https_conn_st *pstC = (dm__https_conn_st *)pstP->data;
  size_t uCopy = uLen;

  if ((pstC->u32PathLen + uCopy) >= DM_HTTPS_PATH_MAX)
  {
    return HPE_USER;
  }
  memcpy(pstC->acPath + pstC->u32PathLen, pcAt, uCopy);
  pstC->u32PathLen += (uint32)uCopy;
  pstC->acPath[pstC->u32PathLen] = '\0';
  return 0;
}

static int dm__http_on_hdr_field(llhttp_t *pstP, const char *pcAt, size_t uLen)
{
  dm__https_conn_st *pstC = (dm__https_conn_st *)pstP->data;
  size_t uIdx = 0u;

  for (uIdx = 0u; (uIdx < uLen) && (pstC->u32CurHdrNameLen < (sizeof(pstC->acCurHdrName) - 1u));
       uIdx++)
  {
    pstC->acCurHdrName[pstC->u32CurHdrNameLen++] = (char)tolower((unsigned char)pcAt[uIdx]);
  }
  pstC->acCurHdrName[pstC->u32CurHdrNameLen] = '\0';
  return 0;
}

static int dm__http_on_hdr_field_done(llhttp_t *pstP)
{
  dm__https_conn_st *pstC = (dm__https_conn_st *)pstP->data;

  pstC->bCurHdrAuth = (0 == strcmp(pstC->acCurHdrName, "authorization")) ? DM_TRUE : DM_FALSE;
  pstC->u32CurHdrNameLen = 0u;
  pstC->acCurHdrName[0] = '\0';
  if (pstC->bCurHdrAuth && pstC->bSeenAuth)
  {
    return HPE_USER;
  }
  return 0;
}

static int dm__http_on_hdr_value_done(llhttp_t *pstP)
{
  dm__https_conn_st *pstC = (dm__https_conn_st *)pstP->data;

  if (pstC->bCurHdrAuth)
  {
    pstC->bSeenAuth = DM_TRUE;
    pstC->bCurHdrAuth = DM_FALSE;
  }
  return 0;
}

static int dm__http_on_hdr_value(llhttp_t *pstP, const char *pcAt, size_t uLen)
{
  dm__https_conn_st *pstC = (dm__https_conn_st *)pstP->data;
  uint32 u32Need = 0u;

  if (!pstC->bCurHdrAuth)
  {
    return 0;
  }
  if ((pstC->u32AuthLen + uLen) >= DM_HTTPS_AUTH_MAX)
  {
    return HPE_USER;
  }
  u32Need = pstC->u32AuthLen + (uint32)uLen + 1u;
  if (u32Need > pstC->u32AuthCap)
  {
    uint32 u32Cap = (0u == pstC->u32AuthCap) ? 256u : pstC->u32AuthCap;
    while (u32Cap < u32Need)
    {
      u32Cap *= 2u;
    }
    if (DM_SUCCESS != dm__secure_grow((void **)&pstC->pcAuthBuf, pstC->u32AuthCap, u32Cap))
    {
      return HPE_USER;
    }
    pstC->u32AuthCap = u32Cap;
  }
  memcpy(pstC->pcAuthBuf + pstC->u32AuthLen, pcAt, uLen);
  pstC->u32AuthLen += (uint32)uLen;
  pstC->pcAuthBuf[pstC->u32AuthLen] = '\0';
  return 0;
}

static int dm__http_on_body(llhttp_t *pstP, const char *pcAt, size_t uLen)
{
  dm__https_conn_st *pstC = (dm__https_conn_st *)pstP->data;
  uint32 u32Need = 0u;

  if ((uLen > g_stCfg.u32BodyMax) || (pstC->u32BodyLen > (g_stCfg.u32BodyMax - (uint32)uLen)))
  {
    pstC->bTooBig = DM_TRUE;
    return HPE_USER;
  }
  u32Need = pstC->u32BodyLen + (uint32)uLen + 1u;
  if (u32Need > pstC->u32BodyCap)
  {
    uint32 u32Cap = (0u == pstC->u32BodyCap) ? 1024u : pstC->u32BodyCap;
    while (u32Cap < u32Need)
    {
      u32Cap *= 2u;
    }
    if (DM_SUCCESS != dm__secure_grow((void **)&pstC->pu8Body, pstC->u32BodyCap, u32Cap))
    {
      return HPE_USER;
    }
    pstC->u32BodyCap = u32Cap;
  }
  memcpy(pstC->pu8Body + pstC->u32BodyLen, pcAt, uLen);
  pstC->u32BodyLen += (uint32)uLen;
  pstC->pu8Body[pstC->u32BodyLen] = '\0';
  return 0;
}

static int dm__http_on_msg_done(llhttp_t *pstP)
{
  dm__https_conn_st *pstC = (dm__https_conn_st *)pstP->data;
  const char *pcMeth = llhttp_method_name((llhttp_method_t)pstP->method);

  snprintf(pstC->acMethod, sizeof(pstC->acMethod), "%s", (NULL != pcMeth) ? pcMeth : "");
  pstC->bReqDone = DM_TRUE;
  return HPE_PAUSED;
}

/* ============================ 路由分发与 TLS 读循环 ============================ */

static void dm__https_dispatch_req(io dm__https_conn_st *pstC)
{
  uint64 u64Rid = (((uint64)pstC->u32Slot) << 32) | (uint64)pstC->u32Gen;
  uint32 u32Idx = 0u;
  bool_t bPathHit = DM_FALSE;
  dm_https_handler_fn pfHit = NULL;
  dm_https_msg_st stMsg;
  static const uint8 au8Empty[1] = {0};

  (void)dm_timer_stop(&pstC->stTimer);
  pstC->stTimer.data = pstC;
  (void)dm_timer_start(&pstC->stTimer, dm__https_on_timeout, g_stCfg.u32ReplyMs, 0u);

  for (u32Idx = 0u; u32Idx < g_u32RouteCnt; u32Idx++)
  {
    if (0 == strcmp(g_astRoutes[u32Idx].acPath, pstC->acPath))
    {
      bPathHit = DM_TRUE;
      if (0 == strcmp(g_astRoutes[u32Idx].acMethod, pstC->acMethod))
      {
        pfHit = g_astRoutes[u32Idx].pfHandler;
        break;
      }
    }
  }
  if (NULL == pfHit)
  {
    const char *pcBody = bPathHit ? "{\"code\":\"method_not_allowed\"}" : "{\"code\":\"not_found\"}";
    (void)dm_https_reply(u64Rid, bPathHit ? 405u : 404u, pcBody, (uint32)strlen(pcBody));
    return;
  }
  memset(&stMsg, 0, sizeof(stMsg));
  stMsg.u64ReplyId = u64Rid;
  stMsg.pcMethod = pstC->acMethod;
  stMsg.pcPath = pstC->acPath;
  stMsg.pcAuthHeader = pstC->pcAuthBuf;
  stMsg.pu8Body = (NULL != pstC->pu8Body) ? pstC->pu8Body : au8Empty;
  stMsg.u32BodyLen = pstC->u32BodyLen;
  pfHit(&stMsg);
  return;
}

static void dm__https_step_tls(io dm__https_conn_st *pstC)
{
  uint8 au8Plain[4096];
  int iRet = 0;
  llhttp_errno_t eErr = HPE_OK;

  if (!pstC->bHandshook)
  {
    iRet = mbedtls_ssl_handshake(&pstC->stSsl);
    (void)dm__https_flush_out(pstC);
    if (pstC->bClosing)
    {
      return;
    }
    if ((MBEDTLS_ERR_SSL_WANT_READ == iRet) || (MBEDTLS_ERR_SSL_WANT_WRITE == iRet))
    {
      return;
    }
    if (0 != iRet)
    {
      dm__https_conn_close(pstC);
      return;
    }
    pstC->bHandshook = DM_TRUE;
    (void)dm_timer_stop(&pstC->stTimer);
    pstC->stTimer.data = pstC;
    (void)dm_timer_start(&pstC->stTimer, dm__https_on_timeout, g_stCfg.u32IdleMs, 0u);
  }

  /* 握手完成立即 fall-through 排空明文（兼容 TLS 1.3 Finished 与 HTTP 拼包） */
  while ((!pstC->bClosing) && (!pstC->bReqDone))
  {
    iRet = mbedtls_ssl_read(&pstC->stSsl, au8Plain, sizeof(au8Plain));
    (void)dm__https_flush_out(pstC);
    if (pstC->bClosing)
    {
      dm_secure_zero(au8Plain, sizeof(au8Plain));
      return;
    }
    if ((MBEDTLS_ERR_SSL_WANT_READ == iRet) || (MBEDTLS_ERR_SSL_WANT_WRITE == iRet))
    {
      break;
    }
    if (iRet <= 0)
    {
      dm__https_conn_close(pstC);
      break;
    }
    eErr = llhttp_execute(&pstC->stParser, (const char *)au8Plain, (size_t)iRet);
    dm_secure_zero(au8Plain, (size_t)iRet);
    if (pstC->bReqDone)
    {
      dm__https_dispatch_req(pstC);
      break;
    }
    if (HPE_OK != eErr)
    {
      uint64 u64Rid = (((uint64)pstC->u32Slot) << 32) | (uint64)pstC->u32Gen;
      if (pstC->bTooBig)
      {
        static const char ac413[] = "{\"code\":\"too_big\"}";
        (void)dm_https_reply(u64Rid, 413u, ac413, (uint32)(sizeof(ac413) - 1u));
      }
      else
      {
        static const char ac400[] = "{\"code\":\"bad_request\"}";
        (void)dm_https_reply(u64Rid, 400u, ac400, (uint32)(sizeof(ac400) - 1u));
      }
      break;
    }
  }
  return;
}

static void dm__https_on_alloc(dm_handle_st *pstH, size_t uSuggest, dm_buf_st *pstBuf)
{
  size_t uSz = (uSuggest > 16384u) ? 16384u : ((uSuggest < 2048u) ? 2048u : uSuggest);

  (void)pstH;
  pstBuf->base = (char *)dm_malloc_tag(uSz, DM_MEM_TAG_HTTP);
  pstBuf->len = (NULL != pstBuf->base) ? uSz : 0u;
  return;
}

static void dm__https_on_read(dm_stream_st *pstStream, ssize_t iNread, const dm_buf_st *pstBuf)
{
  dm__https_conn_st *pstC = (dm__https_conn_st *)pstStream;

  if (iNread > 0)
  {
    if (!pstC->bClosing)
    {
      if (!pstC->bReqDone)
      {
        if (DM_SUCCESS != dm__buf_append(&pstC->stIn, (const uint8 *)pstBuf->base,
                                         (uint32)iNread, DM_HTTPS_IN_BIO_MAX))
        {
          dm__https_conn_close(pstC);
        }
        else
        {
          dm__https_step_tls(pstC);
        }
      }
      else
      {
        /* 请求已收完、异步处理中：仅探测对端 TLS close_notify，忽略多余应用字节 */
        uint8 au8Discard[64];
        if (DM_SUCCESS != dm__buf_append(&pstC->stIn, (const uint8 *)pstBuf->base,
                                         (uint32)iNread, DM_HTTPS_IN_BIO_MAX))
        {
          dm__https_conn_close(pstC);
        }
        else
        {
          int iRd = mbedtls_ssl_read(&pstC->stSsl, au8Discard, sizeof(au8Discard));
          dm_secure_zero(au8Discard, sizeof(au8Discard));
          (void)dm__https_flush_out(pstC);
          if ((!pstC->bClosing) && (iRd <= 0) && (MBEDTLS_ERR_SSL_WANT_READ != iRd)
              && (MBEDTLS_ERR_SSL_WANT_WRITE != iRd))
          {
            dm__https_conn_close(pstC);
          }
        }
      }
    }
  }
  else if (iNread < 0)
  {
    if ((!pstC->bReqDone) || (DM_EOF != iNread))
    {
      dm__https_conn_close(pstC);
    }
  }
  if (NULL != pstBuf->base)
  {
    dm_secure_zero(pstBuf->base, pstBuf->len);
    dm_free_tag(pstBuf->base);
  }
  return;
}

/* ============================ 接入与双级限流 ============================ */

static void dm__https_on_drop_closed(dm_handle_st *pstHandle)
{
  dm_free_tag(pstHandle);
  return;
}

static uint32 dm__https_count_ip(in const uint8 au8Ip[16])
{
  uint32 u32Idx = 0u;
  uint32 u32Cnt = 0u;

  for (u32Idx = 0u; u32Idx < DM_HTTPS_SLOTS_MAX; u32Idx++)
  {
    if ((NULL != g_apstSlots[u32Idx]) && (!g_apstSlots[u32Idx]->bShutPending)
        && (0 == memcmp(g_apstSlots[u32Idx]->au8PeerIp, au8Ip, 16u)))
    {
      u32Cnt++;
    }
  }
  return u32Cnt;
}

static int dm__https_setup_conn(io dm__https_conn_st *pstC)
{
  uint32 u32Slot = 0u;

  for (u32Slot = 0u; u32Slot < DM_HTTPS_SLOTS_MAX; u32Slot++)
  {
    if (NULL == g_apstSlots[u32Slot])
    {
      break;
    }
  }
  if (u32Slot >= DM_HTTPS_SLOTS_MAX)
  {
    return DM_BUSY;
  }
  mbedtls_ssl_init(&pstC->stSsl);
  if (0 != mbedtls_ssl_setup(&pstC->stSsl, &g_stSslCfg))
  {
    mbedtls_ssl_free(&pstC->stSsl);
    return DM_FAILD;
  }
  mbedtls_ssl_set_bio(&pstC->stSsl, pstC, dm__bio_send, dm__bio_recv, NULL);
  if (0 != dm_timer_init(dm_default_loop(), &pstC->stTimer))
  {
    mbedtls_ssl_free(&pstC->stSsl);
    return DM_FAILD;
  }
  llhttp_settings_init(&pstC->stParseSet);
  pstC->stParseSet.on_url = dm__http_on_url;
  pstC->stParseSet.on_header_field = dm__http_on_hdr_field;
  pstC->stParseSet.on_header_field_complete = dm__http_on_hdr_field_done;
  pstC->stParseSet.on_header_value = dm__http_on_hdr_value;
  pstC->stParseSet.on_header_value_complete = dm__http_on_hdr_value_done;
  pstC->stParseSet.on_body = dm__http_on_body;
  pstC->stParseSet.on_message_complete = dm__http_on_msg_done;
  llhttp_init(&pstC->stParser, HTTP_REQUEST, &pstC->stParseSet);
  pstC->stParser.data = pstC;

  pstC->u32Slot = u32Slot;
  pstC->u32Gen = g_u32NextGen++;
  if (0u == g_u32NextGen)
  {
    g_u32NextGen = 1u;
  }
  pstC->bCounted = DM_TRUE;
  g_apstSlots[u32Slot] = pstC;
  g_u32ConnCnt++;
  pstC->stTimer.data = pstC;
  (void)dm_timer_start(&pstC->stTimer, dm__https_on_timeout, g_stCfg.u32HandshakeMs, 0u);
  if (0 != dm_read_start((dm_stream_st *)&pstC->stTcp, dm__https_on_alloc, dm__https_on_read))
  {
    dm__https_conn_close(pstC);
  }
  return DM_SUCCESS;
}

static void dm__https_on_new_conn(dm_stream_st *pstServer, int iStatus)
{
  dm__https_conn_st *pstC = NULL;
  struct sockaddr_storage stPeer;
  int iPeerLen = (int)sizeof(stPeer);

  if (iStatus < 0)
  {
    return;
  }
  if (g_u32ConnCnt >= g_stCfg.u32MaxConns)
  {
    dm_tcp_st *pstDrop = (dm_tcp_st *)dm_calloc_tag(1u, sizeof(*pstDrop), DM_MEM_TAG_HTTP);
    if (NULL != pstDrop)
    {
      if (0 != dm_tcp_init(dm_default_loop(), pstDrop))
      {
        dm_free_tag(pstDrop);
      }
      else
      {
        (void)dm_accept(pstServer, (dm_stream_st *)pstDrop);
        dm_close((dm_handle_st *)pstDrop, dm__https_on_drop_closed);
      }
    }
    return;
  }
  pstC = (dm__https_conn_st *)dm_calloc_tag(1u, sizeof(*pstC), DM_MEM_TAG_HTTP);
  if (NULL == pstC)
  {
    return;
  }
  if (0 != dm_tcp_init(dm_default_loop(), &pstC->stTcp))
  {
    dm_free_tag(pstC);
    return;
  }
  if (0 != dm_accept(pstServer, (dm_stream_st *)&pstC->stTcp))
  {
    dm_close((dm_handle_st *)&pstC->stTcp, dm__https_on_closed);
    return;
  }
  memset(&stPeer, 0, sizeof(stPeer));
  if (0 == dm_tcp_getpeername(&pstC->stTcp, (struct sockaddr *)&stPeer, &iPeerLen))
  {
    dm__norm_peer_ip(&stPeer, pstC->au8PeerIp);
  }
  if (dm__https_count_ip(pstC->au8PeerIp) >= g_stCfg.u32MaxPerIp)
  {
    dm_close((dm_handle_st *)&pstC->stTcp, dm__https_on_closed);
    return;
  }
  if (DM_SUCCESS != dm__https_setup_conn(pstC))
  {
    dm_close((dm_handle_st *)&pstC->stTcp, dm__https_on_closed);
  }
  return;
}

/* ============================ TLS 初始化与端口绑定 ============================ */

static int dm__https_load_tls(in const dm_https_cfg_st *pstCfg)
{
  char *pcCert = NULL;
  char *pcKey = NULL;
  size_t uCertLen = 0u;
  size_t uKeyLen = 0u;
  static const unsigned char aucPers[] = "afar_https_srv";
  int iRet = 0;

  if ((NULL == pstCfg->pcCertDerPath) || (NULL == pstCfg->pcKeyDerPath))
  {
    return DM_INVALID;
  }
  if (PSA_SUCCESS != psa_crypto_init())
  {
    return DM_FAILD;
  }
  mbedtls_entropy_init(&g_stEntropy);
  mbedtls_ctr_drbg_init(&g_stDrbg);
  mbedtls_x509_crt_init(&g_stCert);
  mbedtls_pk_init(&g_stKey);
  mbedtls_ssl_config_init(&g_stSslCfg);
  g_bTlsReady = DM_TRUE;

  iRet = mbedtls_ctr_drbg_seed(&g_stDrbg, mbedtls_entropy_func, &g_stEntropy, aucPers,
                               sizeof(aucPers) - 1u);
  if (0 != iRet)
  {
    return DM_FAILD;
  }
  pcCert = dm__file_read_all(pstCfg->pcCertDerPath, 65536u, &uCertLen);
  pcKey = dm__file_read_all(pstCfg->pcKeyDerPath, 65536u, &uKeyLen);
  if ((NULL == pcCert) || (NULL == pcKey))
  {
    if (NULL != pcCert) { dm_secure_zero(pcCert, uCertLen); dm_free_tag(pcCert); }
    if (NULL != pcKey) { dm_secure_zero(pcKey, uKeyLen); dm_free_tag(pcKey); }
    return DM_IO;
  }
  iRet = mbedtls_x509_crt_parse_der(&g_stCert, (const unsigned char *)pcCert, uCertLen);
  dm_secure_zero(pcCert, uCertLen);
  dm_free_tag(pcCert);
  if (0 == iRet)
  {
    iRet = mbedtls_pk_parse_key(&g_stKey, (const unsigned char *)pcKey, uKeyLen, NULL, 0u,
                                mbedtls_ctr_drbg_random, &g_stDrbg);
  }
  dm_secure_zero(pcKey, uKeyLen);
  dm_free_tag(pcKey);
  if (0 != iRet)
  {
    return DM_INVALID;
  }
  if ((0 != mbedtls_ssl_config_defaults(&g_stSslCfg, MBEDTLS_SSL_IS_SERVER,
                                        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)))
  {
    return DM_FAILD;
  }
  mbedtls_ssl_conf_min_tls_version(&g_stSslCfg, MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_max_tls_version(&g_stSslCfg, MBEDTLS_SSL_VERSION_TLS1_3);
  mbedtls_ssl_conf_rng(&g_stSslCfg, mbedtls_ctr_drbg_random, &g_stDrbg);
  if (0 != mbedtls_ssl_conf_own_cert(&g_stSslCfg, &g_stCert, &g_stKey))
  {
    return DM_FAILD;
  }
  return DM_SUCCESS;
}

static void dm__https_on_listen_closed(dm_handle_st *pstHandle)
{
  dm_free_tag(pstHandle);
  return;
}

static int dm__https_bind_listen(in const dm_https_cfg_st *pstCfg)
{
  const char *pcAddr = ((NULL != pstCfg->pcBindAddr) && ('\0' != pstCfg->pcBindAddr[0]))
                           ? pstCfg->pcBindAddr
                           : "::";
  struct sockaddr_in6 st6;
  struct sockaddr_in st4;
  struct sockaddr_storage stBound;
  int iBoundLen = (int)sizeof(stBound);
  int iErr = 0;

  g_pstListen = (dm_tcp_st *)dm_calloc_tag(1u, sizeof(*g_pstListen), DM_MEM_TAG_HTTP);
  if (NULL == g_pstListen)
  {
    return DM_ALLOC;
  }
  if (0 != dm_tcp_init(dm_default_loop(), g_pstListen))
  {
    dm_free_tag(g_pstListen);
    g_pstListen = NULL;
    return DM_IO;
  }
  if (0 == dm_ip6_addr(pcAddr, (int)pstCfg->u16Port, &st6))
  {
    iErr = dm_tcp_bind(g_pstListen, (const struct sockaddr *)&st6, 0u);
    if ((0 != iErr) && (0 == strcmp(pcAddr, "::"))
        && ((DM_EAFNOSUPPORT == iErr) || (DM_EADDRNOTAVAIL == iErr)))
    {
      (void)dm_ip4_addr("0.0.0.0", (int)pstCfg->u16Port, &st4);
      iErr = dm_tcp_bind(g_pstListen, (const struct sockaddr *)&st4, 0u);
    }
  }
  else if (0 == dm_ip4_addr(pcAddr, (int)pstCfg->u16Port, &st4))
  {
    iErr = dm_tcp_bind(g_pstListen, (const struct sockaddr *)&st4, 0u);
  }
  else
  {
    iErr = DM_EINVAL;
  }
  if (0 == iErr)
  {
    iErr = dm_listen((dm_stream_st *)g_pstListen, 128, dm__https_on_new_conn);
  }
  if (0 != iErr)
  {
    dm_close((dm_handle_st *)g_pstListen, dm__https_on_listen_closed);
    g_pstListen = NULL;
    return DM_IO;
  }
  memset(&stBound, 0, sizeof(stBound));
  if (0 == dm_tcp_getsockname(g_pstListen, (struct sockaddr *)&stBound, &iBoundLen))
  {
    g_u16BoundPort = (AF_INET6 == stBound.ss_family)
                         ? ntohs(((struct sockaddr_in6 *)&stBound)->sin6_port)
                         : ntohs(((struct sockaddr_in *)&stBound)->sin_port);
  }
  DM_LOG_INFO("https listening on [%s]:%u", pcAddr, (uint)g_u16BoundPort);
  return DM_SUCCESS;
}

/* ============================ 生命周期钩子与公开 API ============================ */

static void dm__https_free_tls(void)
{
  if (g_bTlsReady)
  {
    mbedtls_ssl_config_free(&g_stSslCfg);
    mbedtls_pk_free(&g_stKey);
    mbedtls_x509_crt_free(&g_stCert);
    mbedtls_ctr_drbg_free(&g_stDrbg);
    mbedtls_entropy_free(&g_stEntropy);
    g_bTlsReady = DM_FALSE;
  }
  return;
}

static int dm__https_before_init(in const dm_cfg_st *pstCfg)
{
  int iErr = DM_SUCCESS;

  if ((NULL == pstCfg) || (NULL == pstCfg->pstHttps))
  {
    return DM_SUCCESS;
  }
  g_stCfg = *pstCfg->pstHttps;
  if (0u == g_stCfg.u32MaxConns)    { g_stCfg.u32MaxConns = 32u; }
  if (g_stCfg.u32MaxConns > DM_HTTPS_SLOTS_MAX) { g_stCfg.u32MaxConns = DM_HTTPS_SLOTS_MAX; }
  if (0u == g_stCfg.u32MaxPerIp)    { g_stCfg.u32MaxPerIp = 8u; }
  if (0u == g_stCfg.u32BodyMax)     { g_stCfg.u32BodyMax = 65536u; }
  if (0u == g_stCfg.u32HandshakeMs) { g_stCfg.u32HandshakeMs = 3000u; }
  if (0u == g_stCfg.u32IdleMs)      { g_stCfg.u32IdleMs = 15000u; }
  if (0u == g_stCfg.u32ReplyMs)     { g_stCfg.u32ReplyMs = 30000u; }

  iErr = dm__https_load_tls(&g_stCfg);
  if (DM_SUCCESS != iErr)
  {
    dm__https_free_tls();
    return iErr;
  }
  iErr = dm__https_bind_listen(&g_stCfg);
  if (DM_SUCCESS != iErr)
  {
    dm__https_free_tls();
    return iErr;
  }
  g_bInited = DM_TRUE;
  return DM_SUCCESS;
}

static void dm__https_on_drain(void)
{
  if (g_bInited && (NULL != g_stCfg.pfOnDrain))
  {
    g_stCfg.pfOnDrain();
  }
  if (NULL != g_pstListen)
  {
    dm_close((dm_handle_st *)g_pstListen, dm__https_on_listen_closed);
    g_pstListen = NULL;
  }
  return;
}

static void dm__https_on_teardown(void)
{
  uint32 u32Idx = 0u;

  if (NULL != g_pstListen)
  {
    dm_close((dm_handle_st *)g_pstListen, dm__https_on_listen_closed);
    g_pstListen = NULL;
  }
  for (u32Idx = 0u; u32Idx < DM_HTTPS_SLOTS_MAX; u32Idx++)
  {
    if (NULL != g_apstSlots[u32Idx])
    {
      dm__https_conn_close(g_apstSlots[u32Idx]);
    }
  }
  return;
}

static void dm__https_on_end(void)
{
  dm__https_free_tls();
  memset(g_astRoutes, 0, sizeof(g_astRoutes));
  g_u32RouteCnt = 0u;
  memset(g_apstSlots, 0, sizeof(g_apstSlots));
  g_u32ConnCnt = 0u;
  g_u16BoundPort = 0u;
  memset(&g_stCfg, 0, sizeof(g_stCfg));
  g_bInited = DM_FALSE;
  return;
}

static const dm_ext_hooks_st g_stHttpsHooks = {
    .pfBeforeInit = dm__https_before_init,
    .pfOnDrain = dm__https_on_drain,
    .pfOnTeardown = dm__https_on_teardown,
    .pfOnEnd = dm__https_on_end,
};

const dm_ext_hooks_st *dm_https_hooks(void)
{
  return &g_stHttpsHooks;
}

int dm_https_route_exact(in const char *pcMethod, in const char *pcPath,
                         in dm_https_handler_fn pfHandler)
{
  dm__https_route_st *pstR = NULL;

  if ((NULL == pcMethod) || (NULL == pcPath) || (NULL == pfHandler))
  {
    return DM_INVALID;
  }
  if (g_u32RouteCnt >= DM_HTTPS_ROUTES_MAX)
  {
    return DM_RANGE;
  }
  pstR = &g_astRoutes[g_u32RouteCnt++];
  snprintf(pstR->acMethod, sizeof(pstR->acMethod), "%s", pcMethod);
  snprintf(pstR->acPath, sizeof(pstR->acPath), "%s", pcPath);
  pstR->pfHandler = pfHandler;
  return DM_SUCCESS;
}

int dm_https_reply(in uint64 u64ReplyId, in uint uiStatus, in const void *pvBody,
                   in uint32 u32Len)
{
  uint32 u32Slot = (uint32)(u64ReplyId >> 32);
  uint32 u32Gen = (uint32)(u64ReplyId & 0xffffffffu);
  dm__https_conn_st *pstC = NULL;
  char acHdr[256];
  int iHdrLen = 0;
  uint32 u32Total = 0u;
  uint32 u32Off = 0u;

  if ((u32Slot >= DM_HTTPS_SLOTS_MAX) || (NULL == g_apstSlots[u32Slot]))
  {
    return DM_CONN_GONE;
  }
  pstC = g_apstSlots[u32Slot];
  if ((pstC->u32Gen != u32Gen) || pstC->bClosing)
  {
    return DM_CONN_GONE;
  }
  if (pstC->bReplied)
  {
    return DM_EXISTS;
  }
  pstC->bReplied = DM_TRUE;
  pstC->bReqDone = DM_TRUE;
  (void)dm_read_stop((dm_stream_st *)&pstC->stTcp);
  (void)dm_timer_stop(&pstC->stTimer);

  iHdrLen = snprintf(acHdr, sizeof(acHdr),
                     "HTTP/1.1 %u %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n\r\n",
                     uiStatus, dm__status_reason(uiStatus), u32Len);
  if ((iHdrLen <= 0) || ((size_t)iHdrLen >= sizeof(acHdr))
      || (u32Len > (0xffffffffu - (uint32)iHdrLen)))
  {
    dm__https_conn_close(pstC);
    return DM_OVERFLOW;
  }
  u32Total = (uint32)iHdrLen + u32Len;
  pstC->pu8Resp = (uint8 *)dm_malloc_tag(u32Total, DM_MEM_TAG_HTTP);
  if (NULL == pstC->pu8Resp)
  {
    dm__https_conn_close(pstC);
    return DM_ALLOC;
  }
  pstC->u32RespLen = u32Total;
  memcpy(pstC->pu8Resp, acHdr, (size_t)iHdrLen);
  if ((u32Len > 0u) && (NULL != pvBody))
  {
    memcpy(pstC->pu8Resp + iHdrLen, pvBody, u32Len);
  }
  while ((u32Off < u32Total) && (!pstC->bClosing))
  {
    int iWr = mbedtls_ssl_write(&pstC->stSsl, pstC->pu8Resp + u32Off, u32Total - u32Off);
    (void)dm__https_flush_out(pstC);
    if ((iWr <= 0) || pstC->bClosing)
    {
      dm__https_conn_close(pstC);
      return DM_IO;
    }
    u32Off += (uint32)iWr;
  }
  if (!pstC->bClosing)
  {
    (void)mbedtls_ssl_close_notify(&pstC->stSsl);
    (void)dm__https_flush_out(pstC);
  }
  if (pstC->bClosing)
  {
    return DM_IO;
  }
  pstC->stTimer.data = pstC;
  (void)dm_timer_start(&pstC->stTimer, dm__https_on_timeout, g_stCfg.u32IdleMs, 0u);
  pstC->stShut.data = pstC;
  pstC->bShutPending = DM_TRUE;
  if (0 != dm_shutdown(&pstC->stShut, (dm_stream_st *)&pstC->stTcp, dm__https_on_shutdown))
  {
    dm__https_conn_close(pstC);
    return DM_IO;
  }
  return DM_SUCCESS;
}

uint16 dm_https_get_port(void)
{
  return g_u16BoundPort;
}

void dm_https_set_test_on_free(in dm_https_on_free_fn pfCb)
{
  g_pfTestOnFree = pfCb;
  return;
}

#ifdef __cplusplus
}
#endif
