/**
 ******************************************************************************
 * @file    http_srv.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   轻量本机 HTTP/1.1 流式服务端实现（DD-20 §1.2.1，入 libdm_net.a）：
 *          - 严格仅监听 127.0.0.1，基于主循环 libuv + llhttp，0 额外线程；
 *          - 支持 Slowloris 读超时、请求体上限 413 拒收、RFC 9112 §6.3 头过滤、
 *            RFC 9112 §7.1 stream_abort（严禁写 0\r\n\r\n）与写队列高低水位背压通知；
 *          - 释放前对请求头与请求体执行 dm_secure_zero 擦零。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "httpsrv"
#include <dm.h>
#include <dm/http_srv.h>
#include <dm/list.h>
#include <dm/log.h>

#include <llhttp.h>
#include <stdint.h> /* SIZE_MAX */
#include <stdio.h>  /* snprintf */
#include <string.h> /* memcpy / memset / strlen */

#include "../dm_internal.h"

#define DM__HSRV_DEF_BODY_MAX (16u * 1024u * 1024u)
#define DM__HSRV_DEF_READ_MS 30000u
#define DM__HSRV_DEF_HIGH_WATER (2u * 1024u * 1024u)
#define DM__HSRV_HDR_MAX 128u
#define DM__HSRV_URL_MAX 8192u
#define DM__HSRV_RX_BUF_CAP 16384u

typedef struct dm__hsrv_hdr
{
  char *pcName;
  char *pcVal;
} dm__hsrv_hdr_st;

struct dm_http_srv
{
  dm_uv_tcp_st stListen; /* 首成员：监听句柄回调指针 */
  uint16 u16Port;
  uint32 u32MaxReqBodyBytes;
  uint32 u32ReadTimeoutMs;
  uint32 u32WriteHighWater;
  dm_http_srv_req_cb pfOnReq;
  dm_http_srv_state_cb pfOnState;
  void *pvUser;
  dm_list_st stReqs;
  bool_t bClosing;
};

struct dm_http_srv_req
{
  dm_uv_tcp_st stTcp; /* 首成员 */
  dm_uv_timer_st stTimer;
  dm_uv_shutdown_st stShutdown;
  dm_list_node_st stNode;
  dm_http_srv_st *pstSrv;
  void *pvReqUser;

  llhttp_t stParser;
  llhttp_settings_t stParserSet;
  char acMethod[16];
  char *pcUrl;
  size_t uUrlLen;
  size_t uUrlCap;
  dm__hsrv_hdr_st *pastHdrs;
  uint32 u32HdrCount;
  uint32 u32HdrCap;
  char *pcCurName;
  size_t uCurNameLen;
  size_t uCurNameCap;
  char *pcCurVal;
  size_t uCurValLen;
  size_t uCurValCap;
  uint8 *pu8Body;
  uint32 u32BodyLen;
  size_t uBodyCap;

  uint8 *pu8RxBuf;
  bool_t bTimerOpen;
  bool_t bTimerClosing;
  bool_t bTcpOpen;
  bool_t bTcpClosing;
  bool_t bReading;
  bool_t bShutdownPending;
  bool_t bReqDispatched;
  bool_t bStreaming;
  bool_t bReplied;
  bool_t bAborted;
  bool_t bBackpressure;
  bool_t bBodyTooBig;
  bool_t bHdrTooMany;
  uint32 u32WritesInflight;
};

typedef struct dm__hsrv_write
{
  dm_uv_write_st stReq;
  dm_http_srv_req_st *pstOwner;
  uint32 u32Len;
  uint8 au8Data[];
} dm__hsrv_write_st;

/* ============================ 字符串与头过滤辅助 ============================ */

static unsigned char dm__hsrv_ascii_lower(dm_in unsigned char u8Ch)
{
  return ((u8Ch >= 'A') && (u8Ch <= 'Z')) ? (unsigned char)(u8Ch - 'A' + 'a') : u8Ch;
}

static bool_t dm__hsrv_eq_ci_n(dm_in const char *pcA, dm_in size_t uALen, dm_in const char *pcB)
{
  size_t uBLen = strlen(pcB);
  size_t uIdx = 0u;

  if (uALen != uBLen)
  {
    return DM_FALSE;
  }
  for (uIdx = 0u; uIdx < uALen; uIdx++)
  {
    if (dm__hsrv_ascii_lower((unsigned char)pcA[uIdx]) !=
        dm__hsrv_ascii_lower((unsigned char)pcB[uIdx]))
    {
      return DM_FALSE;
    }
  }
  return DM_TRUE;
}

static const char *dm__hsrv_reason(dm_in uint uiStatus)
{
  switch (uiStatus)
  {
  case 200u:
    return "OK";
  case 201u:
    return "Created";
  case 204u:
    return "No Content";
  case 400u:
    return "Bad Request";
  case 401u:
    return "Unauthorized";
  case 403u:
    return "Forbidden";
  case 404u:
    return "Not Found";
  case 408u:
    return "Request Timeout";
  case 413u:
    return "Payload Too Large";
  case 429u:
    return "Too Many Requests";
  case 431u:
    return "Request Header Fields Too Large";
  case 500u:
    return "Internal Server Error";
  case 502u:
    return "Bad Gateway";
  case 503u:
    return "Service Unavailable";
  case 504u:
    return "Gateway Timeout";
  case 508u:
    return "Loop Detected";
  default:
    return "Status";
  }
}

static int dm__hsrv_buf_append(dm_io uint8 **ppu8Buf, dm_io size_t *puLen, dm_io size_t *puCap,
                               dm_in const void *pvData, dm_in size_t uAdd, dm_in size_t uMax)
{
  size_t uNeed = 0u;

  if (0u == uAdd)
  {
    return DM_SUCCESS;
  }
  if ((uAdd > uMax) || (*puLen > (uMax - uAdd)))
  {
    return DM_TOO_BIG;
  }
  uNeed = *puLen + uAdd + 1u;
  if (uNeed > *puCap)
  {
    size_t uNewCap = (0u != *puCap) ? (*puCap * 2u) : 256u;
    uint8 *pu8New = NULL;
    while (uNewCap < uNeed)
    {
      if (uNewCap > (SIZE_MAX / 2u))
      {
        uNewCap = uNeed;
        break;
      }
      uNewCap *= 2u;
    }
    pu8New = (uint8 *)dm_malloc_tag(uNewCap, DM_MEM_TAG_HTTP);
    if (NULL == pu8New)
    {
      return DM_ALLOC;
    }
    if ((NULL != *ppu8Buf) && (*puLen > 0u))
    {
      memcpy(pu8New, *ppu8Buf, *puLen);
    }
    if (NULL != *ppu8Buf)
    {
      dm_secure_zero(*ppu8Buf, *puCap);
      dm_free_tag(*ppu8Buf);
    }
    *ppu8Buf = pu8New;
    *puCap = uNewCap;
  }
  memcpy(*ppu8Buf + *puLen, pvData, uAdd);
  *puLen += uAdd;
  (*ppu8Buf)[*puLen] = 0u;
  return DM_SUCCESS;
}

static bool_t dm__hsrv_is_tchar(dm_in unsigned char u8Ch)
{
  if (((u8Ch >= '0') && (u8Ch <= '9')) || ((u8Ch >= 'A') && (u8Ch <= 'Z')) ||
      ((u8Ch >= 'a') && (u8Ch <= 'z')))
  {
    return DM_TRUE;
  }
  switch (u8Ch)
  {
  case '!':
  case '#':
  case '$':
  case '%':
  case '&':
  case '\'':
  case '*':
  case '+':
  case '-':
  case '.':
  case '^':
  case '_':
  case '`':
  case '|':
  case '~':
    return DM_TRUE;
  default:
    return DM_FALSE;
  }
}

static bool_t dm__hsrv_hdr_line_valid(dm_in const char *pcName, dm_in size_t uNameLen,
                                      dm_in const char *pcVal, dm_in size_t uValLen)
{
  size_t uIdx = 0u;

  if (0u == uNameLen)
  {
    return DM_FALSE;
  }
  for (uIdx = 0u; uIdx < uNameLen; uIdx++)
  {
    if (!dm__hsrv_is_tchar((unsigned char)pcName[uIdx]))
    {
      return DM_FALSE;
    }
  }
  for (uIdx = 0u; uIdx < uValLen; uIdx++)
  {
    unsigned char u8Ch = (unsigned char)pcVal[uIdx];
    if (((u8Ch < 0x20u) && ('\t' != u8Ch)) || (0x7Fu == u8Ch))
    {
      return DM_FALSE;
    }
  }
  return DM_TRUE;
}

/* 过滤响应头：无论定长或 chunked 模式均强制剥离 Connection / Content-Length / Transfer-Encoding，
 * 并拒绝含非法名称/控制字符的头行（RFC 9110 / RFC 9112 §6.3） */
static int dm__hsrv_append_filtered_hdrs(dm_io uint8 **ppu8Out, dm_io size_t *puLen,
                                         dm_io size_t *puCap, dm_in const char *pcHeaders,
                                         dm_in bool_t bChunked)
{
  const char *pcP = pcHeaders;

  (void)bChunked;
  if ((NULL == pcHeaders) || ('\0' == pcHeaders[0]))
  {
    return DM_SUCCESS;
  }
  while ('\0' != *pcP)
  {
    const char *pcEnd = pcP;
    const char *pcColon = NULL;
    size_t uLineLen = 0u;
    size_t uNameLen = 0u;
    size_t uValLen = 0u;
    bool_t bSkip = DM_FALSE;

    while (('\0' != *pcEnd) && ('\r' != *pcEnd) && ('\n' != *pcEnd))
    {
      if ((NULL == pcColon) && (':' == *pcEnd))
      {
        pcColon = pcEnd;
      }
      pcEnd++;
    }
    uLineLen = (size_t)(pcEnd - pcP);
    if ((uLineLen > 0u) && (NULL != pcColon))
    {
      uNameLen = (size_t)(pcColon - pcP);
      uValLen = (size_t)(pcEnd - (pcColon + 1));
      if (!dm__hsrv_hdr_line_valid(pcP, uNameLen, pcColon + 1, uValLen) ||
          dm__hsrv_eq_ci_n(pcP, uNameLen, "Connection") ||
          dm__hsrv_eq_ci_n(pcP, uNameLen, "Content-Length") ||
          dm__hsrv_eq_ci_n(pcP, uNameLen, "Transfer-Encoding"))
      {
        bSkip = DM_TRUE;
      }
      if (!bSkip)
      {
        int iRc = dm__hsrv_buf_append(ppu8Out, puLen, puCap, pcP, uLineLen, 65536u);
        if (DM_SUCCESS == iRc)
        {
          iRc = dm__hsrv_buf_append(ppu8Out, puLen, puCap, "\r\n", 2u, 65536u);
        }
        if (DM_SUCCESS != iRc)
        {
          return iRc;
        }
      }
    }
    pcP = pcEnd;
    while (('\r' == *pcP) || ('\n' == *pcP))
    {
      pcP++;
    }
  }
  return DM_SUCCESS;
}

/* ============================ 请求对象清理与关闭 ============================ */

static void dm__hsrv_free_parsed(dm_io dm_http_srv_req_st *pstReq)
{
  uint32 u32Idx = 0u;

  if (NULL != pstReq->pcUrl)
  {
    dm_secure_zero(pstReq->pcUrl, pstReq->uUrlCap);
    dm_free_tag(pstReq->pcUrl);
    pstReq->pcUrl = NULL;
  }
  if (NULL != pstReq->pcCurName)
  {
    dm_secure_zero(pstReq->pcCurName, pstReq->uCurNameCap);
    dm_free_tag(pstReq->pcCurName);
    pstReq->pcCurName = NULL;
  }
  if (NULL != pstReq->pcCurVal)
  {
    dm_secure_zero(pstReq->pcCurVal, pstReq->uCurValCap);
    dm_free_tag(pstReq->pcCurVal);
    pstReq->pcCurVal = NULL;
  }
  if (NULL != pstReq->pastHdrs)
  {
    for (u32Idx = 0u; u32Idx < pstReq->u32HdrCount; u32Idx++)
    {
      if (NULL != pstReq->pastHdrs[u32Idx].pcName)
      {
        dm_secure_zero(pstReq->pastHdrs[u32Idx].pcName, strlen(pstReq->pastHdrs[u32Idx].pcName));
        dm_free_tag(pstReq->pastHdrs[u32Idx].pcName);
      }
      if (NULL != pstReq->pastHdrs[u32Idx].pcVal)
      {
        dm_secure_zero(pstReq->pastHdrs[u32Idx].pcVal, strlen(pstReq->pastHdrs[u32Idx].pcVal));
        dm_free_tag(pstReq->pastHdrs[u32Idx].pcVal);
      }
    }
    dm_free_tag(pstReq->pastHdrs);
    pstReq->pastHdrs = NULL;
    pstReq->u32HdrCount = 0u;
  }
  if (NULL != pstReq->pu8Body)
  {
    dm_secure_zero(pstReq->pu8Body, pstReq->uBodyCap);
    dm_free_tag(pstReq->pu8Body);
    pstReq->pu8Body = NULL;
  }
  return;
}

static void dm__hsrv_req_maybe_free(dm_io dm_http_srv_req_st *pstReq)
{
  if ((NULL == pstReq) || (!pstReq->bAborted) || pstReq->bTimerOpen || pstReq->bTcpOpen ||
      (0u != pstReq->u32WritesInflight))
  {
    return;
  }
  dm__hsrv_free_parsed(pstReq);
  if (NULL != pstReq->pu8RxBuf)
  {
    dm_secure_zero(pstReq->pu8RxBuf, DM__HSRV_RX_BUF_CAP);
    dm_free_tag(pstReq->pu8RxBuf);
    pstReq->pu8RxBuf = NULL;
  }
  dm_free_tag(pstReq);
  return;
}

static void dm__hsrv_req_on_tcp_closed(dm_in dm_uv_handle_st *pstHandle)
{
  dm_http_srv_req_st *pstReq = (dm_http_srv_req_st *)pstHandle;
  pstReq->bTcpOpen = DM_FALSE;
  pstReq->bTcpClosing = DM_FALSE;
  dm__hsrv_req_maybe_free(pstReq);
  return;
}

static void dm__hsrv_req_on_timer_closed(dm_in dm_uv_handle_st *pstHandle)
{
  dm_http_srv_req_st *pstReq = container_of(pstHandle, dm_http_srv_req_st, stTimer);
  pstReq->bTimerOpen = DM_FALSE;
  pstReq->bTimerClosing = DM_FALSE;
  if (pstReq->bTcpOpen && (!pstReq->bTcpClosing))
  {
    pstReq->bTcpClosing = DM_TRUE;
    dm_uv_close((dm_uv_handle_st *)&pstReq->stTcp, dm__hsrv_req_on_tcp_closed);
  }
  else
  {
    dm__hsrv_req_maybe_free(pstReq);
  }
  return;
}

static void dm__hsrv_req_close_now(dm_io dm_http_srv_req_st *pstReq, dm_in bool_t bNotifyAbort)
{
  dm_http_srv_st *pstSrv = pstReq->pstSrv;

  if (pstReq->bAborted)
  {
    return;
  }
  pstReq->bAborted = DM_TRUE;
  if (pstReq->bReading)
  {
    (void)dm_uv_read_stop((dm_uv_stream_st *)&pstReq->stTcp);
    pstReq->bReading = DM_FALSE;
  }
  if (NULL != pstSrv)
  {
    dm_list_del_node(&pstReq->stNode);
    pstReq->pstSrv = NULL;
  }
  if (bNotifyAbort && pstReq->bReqDispatched && (!pstReq->bReplied) && (NULL != pstSrv) &&
      (NULL != pstSrv->pfOnState))
  {
    pstSrv->pfOnState(pstReq, DM_TRUE, DM_FALSE, pstSrv->pvUser);
  }
  if (pstReq->bTimerOpen && (!pstReq->bTimerClosing))
  {
    (void)dm_uv_timer_stop(&pstReq->stTimer);
    pstReq->bTimerClosing = DM_TRUE;
    dm_uv_close((dm_uv_handle_st *)&pstReq->stTimer, dm__hsrv_req_on_timer_closed);
  }
  else if ((!pstReq->bTimerOpen) && pstReq->bTcpOpen && (!pstReq->bTcpClosing))
  {
    pstReq->bTcpClosing = DM_TRUE;
    dm_uv_close((dm_uv_handle_st *)&pstReq->stTcp, dm__hsrv_req_on_tcp_closed);
  }
  else
  {
    dm__hsrv_req_maybe_free(pstReq);
  }
  return;
}

static void dm__hsrv_on_shutdown(dm_in dm_uv_shutdown_st *pstShut, dm_in int iStatus)
{
  dm_http_srv_req_st *pstReq = container_of(pstShut, dm_http_srv_req_st, stShutdown);
  (void)iStatus;
  pstReq->bShutdownPending = DM_FALSE;
  dm__hsrv_req_close_now(pstReq, DM_FALSE);
  return;
}

static void dm__hsrv_req_close_after_write(dm_io dm_http_srv_req_st *pstReq)
{
  if (pstReq->bAborted || pstReq->bShutdownPending)
  {
    return;
  }
  if (pstReq->bReading)
  {
    (void)dm_uv_read_stop((dm_uv_stream_st *)&pstReq->stTcp);
    pstReq->bReading = DM_FALSE;
  }
  pstReq->bShutdownPending = DM_TRUE;
  if (DM_SUCCESS !=
      dm_uv_shutdown(&pstReq->stShutdown, (dm_uv_stream_st *)&pstReq->stTcp, dm__hsrv_on_shutdown))
  {
    pstReq->bShutdownPending = DM_FALSE;
    dm__hsrv_req_close_now(pstReq, DM_FALSE);
  }
  return;
}

/* ============================ 异步写与背压 ============================ */

static void dm__hsrv_on_written(dm_in dm_uv_write_st *pstWReq, dm_in int iStatus)
{
  dm__hsrv_write_st *pstW = container_of(pstWReq, dm__hsrv_write_st, stReq);
  dm_http_srv_req_st *pstReq = pstW->pstOwner;

  (void)iStatus;
  dm_free_tag(pstW);
  if (pstReq->u32WritesInflight > 0u)
  {
    pstReq->u32WritesInflight--;
  }
  if ((!pstReq->bAborted) && pstReq->bBackpressure && (NULL != pstReq->pstSrv))
  {
    size_t uQ = dm_uv_stream_get_write_queue_size((dm_uv_stream_st *)&pstReq->stTcp);
    if (uQ <= (size_t)(pstReq->pstSrv->u32WriteHighWater / 2u))
    {
      pstReq->bBackpressure = DM_FALSE;
      if (NULL != pstReq->pstSrv->pfOnState)
      {
        pstReq->pstSrv->pfOnState(pstReq, DM_FALSE, DM_FALSE, pstReq->pstSrv->pvUser);
      }
    }
  }
  dm__hsrv_req_maybe_free(pstReq);
  return;
}

static int dm__hsrv_write_raw(dm_io dm_http_srv_req_st *pstReq, dm_in const void *pvData,
                              dm_in uint32 u32Len)
{
  dm__hsrv_write_st *pstW = NULL;
  dm_uv_buf_st stBuf = {0};
  int iErrCode = DM_SUCCESS;

  if (pstReq->bAborted)
  {
    return DM_CONN_GONE;
  }
  if (0u == u32Len)
  {
    return DM_SUCCESS;
  }
  if (u32Len > (64u * 1024u * 1024u))
  {
    return DM_TOO_BIG;
  }
  pstW = (dm__hsrv_write_st *)dm_malloc_tag(sizeof(dm__hsrv_write_st) + (size_t)u32Len,
                                            DM_MEM_TAG_HTTP);
  if (NULL == pstW)
  {
    return DM_ALLOC;
  }
  pstW->pstOwner = pstReq;
  pstW->u32Len = u32Len;
  memcpy(pstW->au8Data, pvData, u32Len);
  stBuf = dm_uv_buf_init((char *)pstW->au8Data, u32Len);
  pstReq->u32WritesInflight++;
  iErrCode =
      dm_uv_write(&pstW->stReq, (dm_uv_stream_st *)&pstReq->stTcp, &stBuf, 1u, dm__hsrv_on_written);
  if (DM_SUCCESS != iErrCode)
  {
    pstReq->u32WritesInflight--;
    dm_free_tag(pstW);
    return DM_CONN_GONE;
  }
  if ((NULL != pstReq->pstSrv) && (!pstReq->bBackpressure))
  {
    size_t uQ = dm_uv_stream_get_write_queue_size((dm_uv_stream_st *)&pstReq->stTcp);
    if (uQ > (size_t)pstReq->pstSrv->u32WriteHighWater)
    {
      pstReq->bBackpressure = DM_TRUE;
      if (NULL != pstReq->pstSrv->pfOnState)
      {
        pstReq->pstSrv->pfOnState(pstReq, DM_FALSE, DM_TRUE, pstReq->pstSrv->pvUser);
      }
    }
  }
  return DM_SUCCESS;
}

/* ============================ llhttp 入站解析 ============================ */

static int dm__hsrv_commit_header(dm_io dm_http_srv_req_st *pstReq)
{
  if (NULL == pstReq->pcCurName)
  {
    return 0;
  }
  if (pstReq->u32HdrCount >= DM__HSRV_HDR_MAX)
  {
    dm_secure_zero(pstReq->pcCurName, pstReq->uCurNameCap);
    dm_free_tag(pstReq->pcCurName);
    pstReq->pcCurName = NULL;
    pstReq->uCurNameLen = 0u;
    pstReq->uCurNameCap = 0u;
    if (NULL != pstReq->pcCurVal)
    {
      dm_secure_zero(pstReq->pcCurVal, pstReq->uCurValCap);
      dm_free_tag(pstReq->pcCurVal);
      pstReq->pcCurVal = NULL;
      pstReq->uCurValLen = 0u;
      pstReq->uCurValCap = 0u;
    }
    pstReq->bHdrTooMany = DM_TRUE;
    return HPE_USER;
  }
  if (pstReq->u32HdrCount >= pstReq->u32HdrCap)
  {
    uint32 u32NewCap = (0u != pstReq->u32HdrCap) ? (pstReq->u32HdrCap * 2u) : 16u;
    dm__hsrv_hdr_st *pastNew =
        (dm__hsrv_hdr_st *)dm_calloc_tag(u32NewCap, sizeof(dm__hsrv_hdr_st), DM_MEM_TAG_HTTP);
    if (NULL == pastNew)
    {
      return HPE_USER;
    }
    if ((NULL != pstReq->pastHdrs) && (pstReq->u32HdrCount > 0u))
    {
      memcpy(pastNew, pstReq->pastHdrs, pstReq->u32HdrCount * sizeof(dm__hsrv_hdr_st));
    }
    dm_free_tag(pstReq->pastHdrs);
    pstReq->pastHdrs = pastNew;
    pstReq->u32HdrCap = u32NewCap;
  }
  if (NULL == pstReq->pcCurVal)
  {
    pstReq->pcCurVal = (char *)dm_calloc_tag(1u, 1u, DM_MEM_TAG_HTTP);
    if (NULL == pstReq->pcCurVal)
    {
      return HPE_USER;
    }
  }
  pstReq->pastHdrs[pstReq->u32HdrCount].pcName = pstReq->pcCurName;
  pstReq->pastHdrs[pstReq->u32HdrCount].pcVal = pstReq->pcCurVal;
  pstReq->u32HdrCount++;
  pstReq->pcCurName = NULL;
  pstReq->uCurNameLen = 0u;
  pstReq->uCurNameCap = 0u;
  pstReq->pcCurVal = NULL;
  pstReq->uCurValLen = 0u;
  pstReq->uCurValCap = 0u;
  return 0;
}

static int dm__hsrv_on_url(dm_in llhttp_t *pstP, dm_in const char *pcAt, dm_in size_t uLen)
{
  dm_http_srv_req_st *pstReq = (dm_http_srv_req_st *)pstP->data;
  int iRc = dm__hsrv_buf_append((uint8 **)&pstReq->pcUrl, &pstReq->uUrlLen, &pstReq->uUrlCap, pcAt,
                                uLen, DM__HSRV_URL_MAX);
  return (DM_SUCCESS == iRc) ? 0 : HPE_USER;
}

static int dm__hsrv_on_hdr_field(dm_in llhttp_t *pstP, dm_in const char *pcAt, dm_in size_t uLen)
{
  dm_http_srv_req_st *pstReq = (dm_http_srv_req_st *)pstP->data;
  int iRc = dm__hsrv_buf_append((uint8 **)&pstReq->pcCurName, &pstReq->uCurNameLen,
                                &pstReq->uCurNameCap, pcAt, uLen, 4096u);
  return (DM_SUCCESS == iRc) ? 0 : HPE_USER;
}

static int dm__hsrv_on_hdr_value(dm_in llhttp_t *pstP, dm_in const char *pcAt, dm_in size_t uLen)
{
  dm_http_srv_req_st *pstReq = (dm_http_srv_req_st *)pstP->data;
  int iRc = dm__hsrv_buf_append((uint8 **)&pstReq->pcCurVal, &pstReq->uCurValLen,
                                &pstReq->uCurValCap, pcAt, uLen, 16384u);
  return (DM_SUCCESS == iRc) ? 0 : HPE_USER;
}

static int dm__hsrv_on_hdr_val_complete(dm_in llhttp_t *pstP)
{
  dm_http_srv_req_st *pstReq = (dm_http_srv_req_st *)pstP->data;
  return dm__hsrv_commit_header(pstReq);
}

static int dm__hsrv_on_body(dm_in llhttp_t *pstP, dm_in const char *pcAt, dm_in size_t uLen)
{
  dm_http_srv_req_st *pstReq = (dm_http_srv_req_st *)pstP->data;
  size_t uMax =
      (NULL != pstReq->pstSrv) ? pstReq->pstSrv->u32MaxReqBodyBytes : DM__HSRV_DEF_BODY_MAX;
  size_t uLenNow = pstReq->u32BodyLen;
  int iRc = dm__hsrv_buf_append(&pstReq->pu8Body, &uLenNow, &pstReq->uBodyCap, pcAt, uLen, uMax);

  if (DM_TOO_BIG == iRc)
  {
    pstReq->bBodyTooBig = DM_TRUE;
    return HPE_USER;
  }
  if (DM_SUCCESS != iRc)
  {
    return HPE_USER;
  }
  pstReq->u32BodyLen = (uint32)uLenNow;
  return 0;
}

static int dm__hsrv_on_msg_complete(dm_in llhttp_t *pstP)
{
  dm_http_srv_req_st *pstReq = (dm_http_srv_req_st *)pstP->data;
  dm_http_srv_st *pstSrv = pstReq->pstSrv;
  dm_http_srv_req_info_st stInfo = {0};
  dm_httpc_hdr_st astPubHdrs[DM__HSRV_HDR_MAX];
  uint32 u32Idx = 0u;
  const char *pcMethod = llhttp_method_name((llhttp_method_t)pstP->method);

  if (pstReq->bReqDispatched || (NULL == pstSrv))
  {
    return HPE_PAUSED;
  }
  pstReq->bReqDispatched = DM_TRUE;
  if (pstReq->bTimerOpen && (!pstReq->bTimerClosing))
  {
    (void)dm_uv_timer_stop(&pstReq->stTimer);
  }
  (void)snprintf(pstReq->acMethod, sizeof(pstReq->acMethod), "%s",
                 (NULL != pcMethod) ? pcMethod : "GET");
  for (u32Idx = 0u; (u32Idx < pstReq->u32HdrCount) && (u32Idx < DM__HSRV_HDR_MAX); u32Idx++)
  {
    astPubHdrs[u32Idx].pcName = pstReq->pastHdrs[u32Idx].pcName;
    astPubHdrs[u32Idx].pcVal = pstReq->pastHdrs[u32Idx].pcVal;
  }
  stInfo.pcMethod = pstReq->acMethod;
  stInfo.pcPath = (NULL != pstReq->pcUrl) ? pstReq->pcUrl : "/";
  stInfo.pastHdrs = (u32Idx > 0u) ? astPubHdrs : NULL;
  stInfo.u32HdrCount = u32Idx;
  stInfo.pu8Body = pstReq->pu8Body;
  stInfo.u32BodyLen = pstReq->u32BodyLen;

  if (NULL != pstSrv->pfOnReq)
  {
    pstSrv->pfOnReq(pstReq, &stInfo, pstSrv->pvUser);
  }
  else
  {
    (void)dm_http_srv_reply(pstReq, 404u, NULL, NULL, 0u);
  }
  dm__hsrv_free_parsed(pstReq);
  return HPE_PAUSED;
}

static void dm__hsrv_on_timeout(dm_in dm_uv_timer_st *pstTimer)
{
  dm_http_srv_req_st *pstReq = container_of(pstTimer, dm_http_srv_req_st, stTimer);
  if (!pstReq->bReqDispatched && !pstReq->bAborted)
  {
    (void)dm_http_srv_reply(pstReq, 408u, NULL, "request timeout", 15u);
  }
  return;
}

static void dm__hsrv_on_alloc(dm_in dm_uv_handle_st *pstHandle, dm_in size_t uSuggested,
                              dm_out dm_uv_buf_st *pstBuf)
{
  dm_http_srv_req_st *pstReq = (dm_http_srv_req_st *)pstHandle;
  (void)uSuggested;
  pstBuf->base = (char *)pstReq->pu8RxBuf;
  pstBuf->len = DM__HSRV_RX_BUF_CAP;
  return;
}

static void dm__hsrv_on_read(dm_in dm_uv_stream_st *pstStream, dm_in ssize_t iNRead,
                             dm_in const dm_uv_buf_st *pstBuf)
{
  dm_http_srv_req_st *pstReq = (dm_http_srv_req_st *)pstStream;
  llhttp_errno_t eErr = HPE_OK;
  (void)pstBuf;

  if (pstReq->bAborted)
  {
    return;
  }
  if (iNRead < 0)
  {
    dm__hsrv_req_close_now(pstReq, DM_TRUE);
    return;
  }
  if ((0 == iNRead) || pstReq->bReqDispatched)
  {
    return;
  }
  eErr = llhttp_execute(&pstReq->stParser, (const char *)pstReq->pu8RxBuf, (size_t)iNRead);
  if ((HPE_OK != eErr) && (HPE_PAUSED != eErr))
  {
    if (!pstReq->bAborted && !pstReq->bReplied && !pstReq->bReqDispatched)
    {
      if (pstReq->bHdrTooMany)
      {
        (void)dm_http_srv_reply(pstReq, 431u, NULL, "request header fields too large", 31u);
      }
      else if (pstReq->bBodyTooBig)
      {
        (void)dm_http_srv_reply(pstReq, 413u, NULL, "payload too large", 17u);
      }
      else
      {
        (void)dm_http_srv_reply(pstReq, 400u, NULL, "bad request", 11u);
      }
    }
  }
  return;
}

static void dm__hsrv_on_conn(dm_in dm_uv_stream_st *pstServer, dm_in int iStatus)
{
  dm_http_srv_st *pstSrv = (dm_http_srv_st *)pstServer;
  dm_http_srv_req_st *pstReq = NULL;

  if ((DM_SUCCESS != iStatus) || pstSrv->bClosing)
  {
    return;
  }
  pstReq = (dm_http_srv_req_st *)dm_calloc_tag(1u, sizeof(dm_http_srv_req_st), DM_MEM_TAG_HTTP);
  if (NULL == pstReq)
  {
    return;
  }
  pstReq->pu8RxBuf = (uint8 *)dm_malloc_tag(DM__HSRV_RX_BUF_CAP, DM_MEM_TAG_HTTP);
  if (NULL == pstReq->pu8RxBuf)
  {
    dm_free_tag(pstReq);
    return;
  }
  if (DM_SUCCESS != dm_uv_tcp_init(dm_uv_default_loop(), &pstReq->stTcp))
  {
    dm_free_tag(pstReq->pu8RxBuf);
    dm_free_tag(pstReq);
    return;
  }
  pstReq->bTcpOpen = DM_TRUE;
  (void)dm_uv_timer_init(dm_uv_default_loop(), &pstReq->stTimer);
  pstReq->bTimerOpen = DM_TRUE;
  if (DM_SUCCESS != dm_uv_accept(pstServer, (dm_uv_stream_st *)&pstReq->stTcp))
  {
    dm__hsrv_req_close_now(pstReq, DM_FALSE);
    return;
  }
  (void)dm_uv_tcp_nodelay(&pstReq->stTcp, 1);
  pstReq->pstSrv = pstSrv;
  dm_list_add_tail(&pstSrv->stReqs, &pstReq->stNode);

  llhttp_settings_init(&pstReq->stParserSet);
  pstReq->stParserSet.on_url = dm__hsrv_on_url;
  pstReq->stParserSet.on_header_field = dm__hsrv_on_hdr_field;
  pstReq->stParserSet.on_header_value = dm__hsrv_on_hdr_value;
  pstReq->stParserSet.on_header_value_complete = dm__hsrv_on_hdr_val_complete;
  pstReq->stParserSet.on_body = dm__hsrv_on_body;
  pstReq->stParserSet.on_message_complete = dm__hsrv_on_msg_complete;
  llhttp_init(&pstReq->stParser, HTTP_REQUEST, &pstReq->stParserSet);
  pstReq->stParser.data = pstReq;

  (void)dm_uv_timer_start(&pstReq->stTimer, dm__hsrv_on_timeout, pstSrv->u32ReadTimeoutMs, 0u);
  if (DM_SUCCESS !=
      dm_uv_read_start((dm_uv_stream_st *)&pstReq->stTcp, dm__hsrv_on_alloc, dm__hsrv_on_read))
  {
    dm__hsrv_req_close_now(pstReq, DM_FALSE);
    return;
  }
  pstReq->bReading = DM_TRUE;
  return;
}

static void dm__hsrv_on_srv_closed(dm_in dm_uv_handle_st *pstHandle)
{
  dm_free_tag(pstHandle);
  return;
}

/* ============================ 公开 API ============================ */

int dm_http_srv_create(dm_in const dm_http_srv_opt_st *pstOpt, dm_out dm_http_srv_st **ppstOut)
{
  dm_http_srv_st *pstSrv = NULL;
  struct sockaddr_in stAddr = {0};
  struct sockaddr_storage stName = {0};
  int iNameLen = (int)sizeof(stName);
  int iErrCode = DM_SUCCESS;

  DM__CHECK_LOOP();
  if (NULL != ppstOut)
  {
    *ppstOut = NULL;
  }
  if ((NULL == pstOpt) || (NULL == ppstOut))
  {
    return DM_INVALID;
  }
  iErrCode = dm_uv_ip4_addr("127.0.0.1", (int)pstOpt->u16Port, &stAddr);
  if (DM_SUCCESS != iErrCode)
  {
    return iErrCode;
  }
  pstSrv = (dm_http_srv_st *)dm_calloc_tag(1u, sizeof(dm_http_srv_st), DM_MEM_TAG_HTTP);
  if (NULL == pstSrv)
  {
    return DM_ALLOC;
  }
  dm_list_init(&pstSrv->stReqs);
  pstSrv->u32MaxReqBodyBytes =
      (0u != pstOpt->u32MaxReqBodyBytes) ? pstOpt->u32MaxReqBodyBytes : DM__HSRV_DEF_BODY_MAX;
  pstSrv->u32ReadTimeoutMs =
      (0u != pstOpt->u32ReadTimeoutMs) ? pstOpt->u32ReadTimeoutMs : DM__HSRV_DEF_READ_MS;
  pstSrv->u32WriteHighWater =
      (0u != pstOpt->u32WriteHighWater) ? pstOpt->u32WriteHighWater : DM__HSRV_DEF_HIGH_WATER;
  pstSrv->pfOnReq = pstOpt->pfOnReq;
  pstSrv->pfOnState = pstOpt->pfOnState;
  pstSrv->pvUser = pstOpt->pvUser;

  iErrCode = dm_uv_tcp_init(dm_uv_default_loop(), &pstSrv->stListen);
  if (DM_SUCCESS != iErrCode)
  {
    dm_free_tag(pstSrv);
    return iErrCode;
  }
  iErrCode = dm_uv_tcp_bind(&pstSrv->stListen, (const struct sockaddr *)&stAddr, 0u);
  if (DM_SUCCESS == iErrCode)
  {
    iErrCode = dm_uv_listen((dm_uv_stream_st *)&pstSrv->stListen, 128, dm__hsrv_on_conn);
  }
  if (DM_SUCCESS == iErrCode)
  {
    iErrCode = dm_uv_tcp_getsockname(&pstSrv->stListen, (struct sockaddr *)&stName, &iNameLen);
  }
  if (DM_SUCCESS != iErrCode)
  {
    pstSrv->bClosing = DM_TRUE;
    dm_uv_close((dm_uv_handle_st *)&pstSrv->stListen, dm__hsrv_on_srv_closed);
    return iErrCode;
  }
  pstSrv->u16Port = (uint16)ntohs(((struct sockaddr_in *)&stName)->sin_port);
  *ppstOut = pstSrv;
  return DM_SUCCESS;
}

uint16 dm_http_srv_port(dm_in const dm_http_srv_st *pstSrv)
{
  return (NULL != pstSrv) ? pstSrv->u16Port : 0u;
}

void dm_http_srv_destroy(dm_io dm_http_srv_st *pstSrv)
{
  dm_list_node_st *pstNode = NULL;
  dm_list_node_st *pstNext = NULL;

  if ((NULL == pstSrv) || pstSrv->bClosing)
  {
    return;
  }
  pstSrv->bClosing = DM_TRUE;
  DM_LIST_FOREACH_SAFE(&pstSrv->stReqs, pstNode, pstNext)
  {
    dm_http_srv_req_st *pstReq = DM_LIST_ENTRY(pstNode, dm_http_srv_req_st, stNode);
    dm__hsrv_req_close_now(pstReq, DM_TRUE);
  }
  dm_uv_close((dm_uv_handle_st *)&pstSrv->stListen, dm__hsrv_on_srv_closed);
  return;
}

void dm_http_srv_req_set_user(dm_io dm_http_srv_req_st *pstReq, dm_in void *pvCtx)
{
  if (NULL != pstReq)
  {
    pstReq->pvReqUser = pvCtx;
  }
  return;
}

void *dm_http_srv_req_get_user(dm_in const dm_http_srv_req_st *pstReq)
{
  return (NULL != pstReq) ? pstReq->pvReqUser : NULL;
}

int dm_http_srv_reply(dm_io dm_http_srv_req_st *pstReq, dm_in uint uiStatus,
                      dm_in const char *pcHeaders, dm_in const void *pvBody, dm_in uint32 u32Len)
{
  uint8 *pu8Buf = NULL;
  size_t uLen = 0u;
  size_t uCap = 0u;
  char acHead[128] = {0};
  int iHead = 0;
  int iErrCode = DM_SUCCESS;

  DM__CHECK_LOOP();
  if ((NULL == pstReq) || ((NULL == pvBody) && (u32Len > 0u)))
  {
    return DM_INVALID;
  }
  if (pstReq->bAborted)
  {
    return DM_CONN_GONE;
  }
  if (pstReq->bReplied || pstReq->bStreaming)
  {
    return DM_EXISTS;
  }
  iHead = snprintf(acHead, sizeof(acHead),
                   "HTTP/1.1 %u %s\r\nContent-Length: %u\r\nConnection: close\r\n", uiStatus,
                   dm__hsrv_reason(uiStatus), (unsigned)u32Len);
  if ((iHead <= 0) || ((size_t)iHead >= sizeof(acHead)))
  {
    return DM_OVERFLOW;
  }
  iErrCode = dm__hsrv_buf_append(&pu8Buf, &uLen, &uCap, acHead, (size_t)iHead, 65536u);
  if (DM_SUCCESS == iErrCode)
  {
    iErrCode = dm__hsrv_append_filtered_hdrs(&pu8Buf, &uLen, &uCap, pcHeaders, DM_FALSE);
  }
  if (DM_SUCCESS == iErrCode)
  {
    iErrCode = dm__hsrv_buf_append(&pu8Buf, &uLen, &uCap, "\r\n", 2u, 65536u);
  }
  if ((DM_SUCCESS == iErrCode) && (u32Len > 0u))
  {
    iErrCode = dm__hsrv_buf_append(&pu8Buf, &uLen, &uCap, pvBody, u32Len, 64u * 1024u * 1024u);
  }
  if (DM_SUCCESS == iErrCode)
  {
    pstReq->bReplied = DM_TRUE;
    iErrCode = dm__hsrv_write_raw(pstReq, pu8Buf, (uint32)uLen);
    dm__hsrv_req_close_after_write(pstReq);
  }
  if (NULL != pu8Buf)
  {
    dm_secure_zero(pu8Buf, uCap);
    dm_free_tag(pu8Buf);
  }
  return iErrCode;
}

int dm_http_srv_stream_begin(dm_io dm_http_srv_req_st *pstReq, dm_in uint uiStatus,
                             dm_in const char *pcHeaders)
{
  uint8 *pu8Buf = NULL;
  size_t uLen = 0u;
  size_t uCap = 0u;
  char acHead[128] = {0};
  int iHead = 0;
  int iErrCode = DM_SUCCESS;

  DM__CHECK_LOOP();
  if (NULL == pstReq)
  {
    return DM_INVALID;
  }
  if (pstReq->bAborted)
  {
    return DM_CONN_GONE;
  }
  if (pstReq->bReplied || pstReq->bStreaming)
  {
    return DM_EXISTS;
  }
  iHead = snprintf(acHead, sizeof(acHead),
                   "HTTP/1.1 %u %s\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n",
                   uiStatus, dm__hsrv_reason(uiStatus));
  if ((iHead <= 0) || ((size_t)iHead >= sizeof(acHead)))
  {
    return DM_OVERFLOW;
  }
  iErrCode = dm__hsrv_buf_append(&pu8Buf, &uLen, &uCap, acHead, (size_t)iHead, 65536u);
  if (DM_SUCCESS == iErrCode)
  {
    iErrCode = dm__hsrv_append_filtered_hdrs(&pu8Buf, &uLen, &uCap, pcHeaders, DM_TRUE);
  }
  if (DM_SUCCESS == iErrCode)
  {
    iErrCode = dm__hsrv_buf_append(&pu8Buf, &uLen, &uCap, "\r\n", 2u, 65536u);
  }
  if (DM_SUCCESS == iErrCode)
  {
    iErrCode = dm__hsrv_write_raw(pstReq, pu8Buf, (uint32)uLen);
    if (DM_SUCCESS == iErrCode)
    {
      pstReq->bStreaming = DM_TRUE;
    }
  }
  if (NULL != pu8Buf)
  {
    dm_secure_zero(pu8Buf, uCap);
    dm_free_tag(pu8Buf);
  }
  return iErrCode;
}

int dm_http_srv_stream_write(dm_io dm_http_srv_req_st *pstReq, dm_in const void *pvData,
                             dm_in uint32 u32Len)
{
  char acPrefix[32] = {0};
  int iPrefix = 0;
  uint32 u32FrameLen = 0u;
  uint8 *pu8Frame = NULL;
  int iErrCode = DM_SUCCESS;

  DM__CHECK_LOOP();
  if ((NULL == pstReq) || ((NULL == pvData) && (u32Len > 0u)))
  {
    return DM_INVALID;
  }
  if (pstReq->bAborted)
  {
    return DM_CONN_GONE;
  }
  if (!pstReq->bStreaming || pstReq->bReplied)
  {
    return DM_LINK_ERR_STATE;
  }
  if (0u == u32Len)
  {
    return DM_SUCCESS;
  }
  iPrefix = snprintf(acPrefix, sizeof(acPrefix), "%x\r\n", (unsigned)u32Len);
  if ((iPrefix <= 0) || ((size_t)iPrefix >= sizeof(acPrefix)))
  {
    return DM_OVERFLOW;
  }
  if ((uint64)u32Len + (uint64)iPrefix + 2u > (uint64)0xFFFFFFFFu)
  {
    return DM_TOO_BIG;
  }
  u32FrameLen = (uint32)iPrefix + u32Len + 2u;
  pu8Frame = (uint8 *)dm_malloc_tag(u32FrameLen, DM_MEM_TAG_HTTP);
  if (NULL == pu8Frame)
  {
    return DM_ALLOC;
  }
  memcpy(pu8Frame, acPrefix, (size_t)iPrefix);
  memcpy(pu8Frame + iPrefix, pvData, u32Len);
  memcpy(pu8Frame + iPrefix + u32Len, "\r\n", 2u);
  iErrCode = dm__hsrv_write_raw(pstReq, pu8Frame, u32FrameLen);
  dm_free_tag(pu8Frame);
  return iErrCode;
}

int dm_http_srv_stream_end(dm_io dm_http_srv_req_st *pstReq)
{
  static const char s_acTerm[] = "0\r\n\r\n";
  int iErrCode = DM_SUCCESS;

  DM__CHECK_LOOP();
  if (NULL == pstReq)
  {
    return DM_INVALID;
  }
  if (pstReq->bAborted)
  {
    return DM_CONN_GONE;
  }
  if (!pstReq->bStreaming || pstReq->bReplied)
  {
    return DM_LINK_ERR_STATE;
  }
  pstReq->bStreaming = DM_FALSE;
  pstReq->bReplied = DM_TRUE;
  iErrCode = dm__hsrv_write_raw(pstReq, s_acTerm, 5u);
  dm__hsrv_req_close_after_write(pstReq);
  return iErrCode;
}

void dm_http_srv_stream_abort(dm_io dm_http_srv_req_st *pstReq)
{
  if ((NULL == pstReq) || pstReq->bAborted)
  {
    return;
  }
  /* RFC 9112 §7.1：严禁写出 "0\r\n\r\n" 终止块！直接关闭连接让下游可靠感知流截断 */
  pstReq->bStreaming = DM_FALSE;
  pstReq->bReplied = DM_TRUE;
  if ((!pstReq->bBackpressure) && (pstReq->u32WritesInflight > 0u))
  {
    dm__hsrv_req_close_after_write(pstReq);
  }
  else
  {
    dm__hsrv_req_close_now(pstReq, DM_FALSE);
  }
  return;
}

#ifdef __cplusplus
}
#endif
