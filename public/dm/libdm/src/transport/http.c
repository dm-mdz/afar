/**
 ******************************************************************************
 * @file    http.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   HTTP 传输口（DD-01）：llhttp 解析、Host/Origin 校验、路由、应答、发文件、升级。
 *
 * 一个请求读完（头和正文）才交给路由的处理项，负载是正文；来源 DM_SRC_HTTP，凭据用 dm_reply_take
 * 取、 dm_http_reply / dm_http_reply_file
 * 答。同一条连接上按到达的顺序答：前面的没答，后面答好的先排着。 没答的到 DM_HTTP_PENDING
 * 就停读这条连接。出错由 DM 直接答、之后关连接。连接断了，每个没答的请求 交一条
 * DM_SRC_HTTP_GONE。/ws 上的升级交给 ws.c。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "http"
#include <dm.h>
#include <dm/http.h>
#include <dm/rpc.h>
#include <dm/module.h>
#include <dm/log.h>
#include <dm/limits.h>

#include <llhttp.h>

#include <stdarg.h>   /* va_list */
#include <stdio.h>    /* snprintf */
#include <stdlib.h>   /* strtoull */
#include <string.h>   /* memcpy / strcmp / strncmp */
#include <sys/stat.h> /* S_IFREG */
#include <ctype.h>    /* tolower */

#include "conn.h"

#define DM__ROUTE_MAX 64u
#define DM__FILE_CHUNK (64u * 1024u)
#define DM__FILE_HIGH (256u * 1024u) /* 连接上积压超过它就先不读下一块文件 */

typedef struct dm__route
{
  const char *pcMethod; /* "GET" 等；NULL = 什么方法都收 */
  const char *pcPrefix;
  uint uiLinkID;
} dm__route_st;

typedef struct dm__http_file
{
  dm_uv_fs_st stReq;     /* 读文件的请求（线程池里跑） */
  dm_uv_timer_st stWait; /* 连接上积压太多时等一下再读 */
  dm__conn_st *pstConn;  /* 连接没了就是 NULL */
  dm_uv_file iFd;
  uint64 u64Off;
  uint64 u64Left;
  bool_t bReading; /* 线程池里正读着：连接没了也要等它回来再收拾 */
  uint8 au8Buf[DM__FILE_CHUNK];
} dm__http_file_st;

typedef struct dm__http_req
{
  dm_list_node_st stNode;
  uint32 u32ReqID;
  uint uiLinkID;     /* 路由到的处理项；DM 自己答的错误是 0 */
  bool_t bDelivered; /* 交给了处理项 */
  bool_t bAnswered;
  bool_t bStreaming;      /* 已进入 chunked 流式应答态（AUD-03） */
  bool_t bStreamHeadSent; /* 流式响应头已写出到连接 */
  bool_t bCloseAfter;     /* 答完关连接（DM 答的 4xx） */
  char *pcPath;           /* 下面三样一块分配 */
  char *pcQuery;          /* "k\0v\0k\0v\0\0" */
  char *pcHeaders;        /* "name\0value\0…\0" */
  const char *pcOrigin; /* 白名单里的那个来源（应答带 Access-Control-Allow-Origin），没有是 NULL */
  uint8 *pu8Resp;
  uint32 u32RespLen;
  dm__http_file_st *pstFile; /* 发文件：头发完以后接着发它 */
} dm__http_req_st;

typedef struct dm__http
{
  dm_uv_timer_st stIdle; /* 首成员：空闲多久关 */
  llhttp_t stParser;
  dm__conn_st *pstConn;
  dm_list_st stReqs; /* 按到达的顺序 */
  uint32 u32Pending; /* 交了还没答的 */
  uint32 u32NextReq;
  /* 正在解析的这一个 */
  char *pcBuild; /* url\0 name\0value\0 …：边收边接 */
  uint32 u32BuildLen;
  uint32 u32BuildCap;
  uint32 u32UrlEnd; /* url 在 pcBuild 里占到哪 */
  bool_t bInValue;
  uint8 *pu8Body;
  uint32 u32BodyLen;
  uint32 u32BodyCap;
  bool_t bWantUpgrade;               /* /ws 上合规的升级请求 */
  bool_t bRejected;                  /* 这一个已经由 DM 答了错误，不再往下解 */
  bool_t bSending;                   /* 正在发文件：后面的应答等它 */
  struct dm__http_file *pstSendFile; /* 正在发的那个 */
  /* 停读时没解的 */
  uint8 *pu8Left;
  uint32 u32LeftLen;
  bool_t bPaused;
} dm__http_st;

static dm__route_st g_astRoute[DM__ROUTE_MAX];
static uint g_uiRouteCount = 0;
static const char *const *g_ppcOrigin = NULL;
static llhttp_settings_t g_stHttpSettings;
static bool_t g_bHttpSettingsUp = DM_FALSE;

void dm__http_config(dm_in const dm_cfg_st *pstCfg)
{
  g_ppcOrigin = (NULL != pstCfg) ? pstCfg->ppcOrigin : NULL;
  return;
}

/* ============================ 路由 ============================ */

int dm_http_route(dm_in const char *pcMethod, dm_in const char *pcPrefix, dm_in uint uiLinkID)
{
  uint uiIdx = 0;

  DM__CHECK_LOOP();
  if ((DM__PHASE_INIT != dm__phase()) && (DM__PHASE_NONE != dm__phase()))
  {
    DM_LOG_ERROR("dm_http_route outside init");
    return DM_INVALID;
  }
  if ((NULL == pcPrefix) || ('/' != pcPrefix[0]) || (0 == strcmp(pcPrefix, "/ws")))
  {
    return DM_INVALID; /* /ws 是 DM 自己的 */
  }
  if (NULL == dm__link_lookup(uiLinkID))
  {
    return DM_LINK_ERR_NOT_FOUND;
  }
  for (uiIdx = 0; uiIdx < g_uiRouteCount; uiIdx++)
  {
    const dm__route_st *pstOne = &g_astRoute[uiIdx];
    if ((0 == strcmp(pstOne->pcPrefix, pcPrefix)) &&
        ((NULL == pstOne->pcMethod) || (NULL == pcMethod) ||
         (0 == strcmp(pstOne->pcMethod, pcMethod))))
    {
      return DM_EXISTS;
    }
  }
  if (g_uiRouteCount >= DM__ROUTE_MAX)
  {
    return DM_RANGE;
  }
  g_astRoute[g_uiRouteCount].pcMethod = pcMethod;
  g_astRoute[g_uiRouteCount].pcPrefix = pcPrefix;
  g_astRoute[g_uiRouteCount].uiLinkID = uiLinkID;
  g_uiRouteCount++;
  return DM_SUCCESS;
}

void dm__http_routes_clear(void)
{
  g_uiRouteCount = 0;
  return;
}

/* 前缀对得上：整段一样，或者前缀后面紧跟 '/'，或者前缀本身以 '/' 结尾（/a 不匹配 /ab）。
 * 路径里已经去掉了 '?' 后面的部分 */
static bool_t dm__route_hit(dm_in const char *pcPrefix, dm_in const char *pcPath)
{
  size_t uLen = strlen(pcPrefix);
  if (0 != strncmp(pcPrefix, pcPath, uLen))
  {
    return DM_FALSE;
  }
  return (('\0' == pcPath[uLen]) || ('/' == pcPath[uLen]) || ('/' == pcPrefix[uLen - 1u]))
             ? DM_TRUE
             : DM_FALSE;
}

/* 最长的前缀优先；有前缀对得上、但方法不对，*piStatus = 405，否则 404 */
static uint dm__route_find(dm_in const char *pcMethod, dm_in const char *pcPath,
                           dm_out uint *puiStatus)
{
  size_t uBest = 0;
  uint uiBest = 0;
  bool_t bAnyPrefix = DM_FALSE;
  uint uiIdx = 0;

  for (uiIdx = 0; uiIdx < g_uiRouteCount; uiIdx++)
  {
    const dm__route_st *pstOne = &g_astRoute[uiIdx];
    size_t uLen = strlen(pstOne->pcPrefix);
    if (!dm__route_hit(pstOne->pcPrefix, pcPath))
    {
      continue;
    }
    if (uLen >= uBest)
    {
      bAnyPrefix = (uLen > uBest) ? DM_TRUE : bAnyPrefix;
    }
    if ((NULL != pstOne->pcMethod) && (0 != strcmp(pstOne->pcMethod, pcMethod)))
    {
      bAnyPrefix = DM_TRUE;
      continue;
    }
    if ((uLen > uBest) || (0u == uiBest))
    {
      uBest = uLen;
      uiBest = pstOne->uiLinkID;
    }
  }
  *puiStatus = (0u != uiBest) ? 200u : (bAnyPrefix ? 405u : 404u);
  return uiBest;
}

/* ============================ 请求的取值（只在 before 里有效） ============================ */

static dm__http_req_st *dm__http_req_of(dm_in const dm_link_msg_st *pstMsg)
{
  const dm__reply_ctx_st *pstCtx = NULL;
  dm__conn_st *pstConn = NULL;
  dm__http_st *pstHttp = NULL;
  dm_list_node_st *pstNode = NULL;

  if (!dm_in_loop()) /* dm_http_path / query / header 仅允许主循环线程调用 */
  {
    dm__wrong_thread("dm_http_path/query/header");
    return NULL;
  }
  if ((NULL == pstMsg) || ((DM_SRC_HTTP != pstMsg->u8Src) && (DM_SRC_HTTP_GONE != pstMsg->u8Src)))
  {
    return NULL;
  }
  pstCtx = (const dm__reply_ctx_st *)pstMsg->pvPriv;
  pstConn = (NULL != pstCtx) ? dm__conn_find(pstCtx->u32ConnID) : NULL;
  pstHttp = (NULL != pstConn) ? (dm__http_st *)pstConn->pvHttp : NULL;
  if (NULL == pstHttp)
  {
    return NULL;
  }
  DM_LIST_FOREACH(&pstHttp->stReqs, pstNode)
  {
    dm__http_req_st *pstReq = DM_LIST_ENTRY(pstNode, dm__http_req_st, stNode);
    if (pstReq->u32ReqID == pstCtx->u32ReqID)
    {
      return pstReq;
    }
  }
  return NULL;
}

const char *dm_http_path(dm_in const dm_link_msg_st *pstMsg)
{
  dm__http_req_st *pstReq = dm__http_req_of(pstMsg);
  return (NULL != pstReq) ? pstReq->pcPath : NULL;
}

/* 在 "a\0b\0c\0d\0\0" 这样的对里找；bNoCase 为真时名字不分大小写 */
static const char *dm__pairs_get(dm_in const char *pcPairs, dm_in const char *pcKey,
                                 dm_in bool_t bNoCase)
{
  const char *pc = pcPairs;
  while ((NULL != pc) && ('\0' != *pc))
  {
    const char *pcVal = pc + strlen(pc) + 1;
    bool_t bHit = DM_FALSE;
    if (bNoCase)
    {
      size_t uIdx = 0;
      bHit = DM_TRUE;
      for (uIdx = 0; bHit && (('\0' != pc[uIdx]) || ('\0' != pcKey[uIdx])); uIdx++)
      {
        bHit = (tolower((unsigned char)pc[uIdx]) == tolower((unsigned char)pcKey[uIdx])) ? DM_TRUE
                                                                                         : DM_FALSE;
      }
    }
    else
    {
      bHit = (0 == strcmp(pc, pcKey)) ? DM_TRUE : DM_FALSE;
    }
    if (bHit)
    {
      return pcVal;
    }
    pc = pcVal + strlen(pcVal) + 1;
  }
  return NULL;
}

const char *dm_http_query(dm_in const dm_link_msg_st *pstMsg, dm_in const char *pcKey)
{
  dm__http_req_st *pstReq = dm__http_req_of(pstMsg);
  return ((NULL != pstReq) && (NULL != pcKey)) ? dm__pairs_get(pstReq->pcQuery, pcKey, DM_FALSE)
                                               : NULL;
}

const char *dm_http_header(dm_in const dm_link_msg_st *pstMsg, dm_in const char *pcName)
{
  dm__http_req_st *pstReq = dm__http_req_of(pstMsg);
  return ((NULL != pstReq) && (NULL != pcName)) ? dm__pairs_get(pstReq->pcHeaders, pcName, DM_TRUE)
                                                : NULL;
}

/* ============================ 应答：按到达的顺序发 ============================ */

static const char *dm__http_reason(dm_in uint uiStatus)
{
  switch (uiStatus)
  {
  case 200:
    return "OK";
  case 202:
    return "Accepted";
  case 204:
    return "No Content";
  case 206:
    return "Partial Content";
  case 400:
    return "Bad Request";
  case 401:
    return "Unauthorized";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 411:
    return "Length Required";
  case 413:
    return "Payload Too Large";
  case 416:
    return "Range Not Satisfiable";
  case 431:
    return "Request Header Fields Too Large";
  case 500:
    return "Internal Server Error";
  case 503:
    return "Service Unavailable";
  default:
    return "Status";
  }
}

static void dm__http_req_free(dm_io dm__http_req_st *pstReq)
{
  dm_list_del_node(&pstReq->stNode);
  dm_free_tag(pstReq->pcPath);
  dm_free_tag(pstReq->pu8Resp);
  dm_free_tag(pstReq);
  return;
}

static void dm__http_on_idle(dm_in dm_uv_timer_st *pstTimer);

static void dm__http_idle_kick(dm_io dm__http_st *pstHttp)
{
  (void)dm_uv_timer_start(&pstHttp->stIdle, dm__http_on_idle, DM_HTTP_IDLE_MS, 0);
  return;
}

static void dm__http_resume_parse(dm_io dm__http_st *pstHttp);
static void dm__http_file_next(dm_io dm__http_file_st *pstFile);

/* 把排在最前面、已经答好的发出去 */
static void dm__http_flush(dm_io dm__http_st *pstHttp)
{
  dm__conn_st *pstConn = pstHttp->pstConn;

  while (!pstHttp->bSending && (NULL != pstHttp->stReqs.stHead.pstNext) &&
         (pstHttp->stReqs.stHead.pstNext != &pstHttp->stReqs.stHead))
  {
    dm__http_req_st *pstReq =
        DM_LIST_ENTRY(pstHttp->stReqs.stHead.pstNext, dm__http_req_st, stNode);
    bool_t bClose = pstReq->bCloseAfter;
    if (pstReq->bStreaming && !pstReq->bAnswered)
    {
      if (!pstReq->bStreamHeadSent)
      {
        if (NULL != pstReq->pu8Resp)
        {
          (void)dm__conn_write_raw(pstConn, pstReq->pu8Resp, pstReq->u32RespLen);
          dm_free_tag(pstReq->pu8Resp);
          pstReq->pu8Resp = NULL;
          pstReq->u32RespLen = 0u;
        }
        pstReq->bStreamHeadSent = DM_TRUE;
        pstHttp->bSending = DM_TRUE;
      }
      break;
    }
    if (!pstReq->bAnswered)
    {
      break;
    }
    (void)dm__conn_write_raw(pstConn, pstReq->pu8Resp, pstReq->u32RespLen);
    if (pstReq->bDelivered && (pstHttp->u32Pending > 0u))
    {
      pstHttp->u32Pending--;
    }
    if (NULL != pstReq->pstFile)
    {
      dm__http_file_st *pstFile = pstReq->pstFile;
      pstReq->pstFile = NULL;
      dm__http_req_free(
          pstReq); /* 必须先摘链释放，防 dm__http_file_next 同步回调 dm__http_flush 重入双关 */
      pstHttp->bSending = DM_TRUE; /* 文件发完了再接着发后面的 */
      pstHttp->pstSendFile = pstFile;
      pstFile->pstConn = pstConn;
      dm__http_file_next(pstFile);
      break;
    }
    dm__http_req_free(pstReq);
    if (bClose)
    {
      dm__conn_close_after_write(pstConn);
      return;
    }
  }
  dm__http_idle_kick(pstHttp);
  if (pstHttp->bPaused && (pstHttp->u32Pending < DM_HTTP_PENDING))
  {
    dm__http_resume_parse(pstHttp);
  }
  return;
}

/* 往响应头缓冲追加格式化片段；容量不足返回 DM_FALSE，防止下标越界 */
static bool_t dm__http_cat(dm_io char *pcHead, dm_in size_t uCap, dm_io int *piHead,
                           dm_in const char *pcFmt, ...)
{
  va_list stArgs;
  int iN = 0;
  if ((*piHead < 0) || ((size_t)*piHead >= uCap))
  {
    return DM_FALSE;
  }
  va_start(stArgs, pcFmt);
  iN = vsnprintf(pcHead + *piHead, uCap - (size_t)*piHead, pcFmt, stArgs);
  va_end(stArgs);
  if ((iN < 0) || ((size_t)iN >= uCap - (size_t)*piHead))
  {
    *piHead = (int)uCap;
    return DM_FALSE;
  }
  *piHead += iN;
  return DM_TRUE;
}

/* 构造 HTTP 应答并挂入请求（暂不发送）；u64Len 支持超过 4GB 的文件正文长度 */
static int dm__http_set_resp(dm_io dm__http_req_st *pstReq, dm_in uint uiStatus,
                             dm_in const char *pcType, dm_in const char *pcHeaders,
                             dm_in const void *pvBody, dm_in uint64 u64Len,
                             dm_in const char *pcExtra)
{
  char acHead[1024] = {0};
  int iHead = 0;
  uint32 u32Len = (uint32)u64Len; /* 不是发文件时正文在内存里，不会超过 4GB */
  bool_t bOk = dm__http_cat(acHead, sizeof(acHead), &iHead, "HTTP/1.1 %u %s\r\n", uiStatus,
                            dm__http_reason(uiStatus));

  if (bOk && (NULL != pcType))
  {
    bOk = dm__http_cat(acHead, sizeof(acHead), &iHead, "Content-Type: %s\r\n", pcType);
  }
  if (bOk && (NULL != pstReq->pcOrigin))
  {
    bOk = dm__http_cat(acHead, sizeof(acHead), &iHead,
                       "Access-Control-Allow-Origin: %s\r\nVary: Origin\r\n", pstReq->pcOrigin);
  }
  if (bOk && (NULL != pcExtra))
  {
    bOk = dm__http_cat(acHead, sizeof(acHead), &iHead, "%s", pcExtra);
  }
  if (bOk)
  {
    bOk = dm__http_cat(acHead, sizeof(acHead), &iHead, "Content-Length: %llu\r\n%s",
                       (unsigned long long)u64Len,
                       pstReq->bCloseAfter ? "Connection: close\r\n" : "");
  }
  if (!bOk)
  {
    return DM_OVERFLOW;
  }
  pstReq->u32RespLen = (uint32)iHead + ((NULL != pcHeaders) ? (uint32)strlen(pcHeaders) : 0u) + 2u +
                       ((NULL != pstReq->pstFile) ? 0u : u32Len);
  pstReq->pu8Resp = (uint8 *)dm_malloc_tag(pstReq->u32RespLen, DM_MEM_TAG_HTTP);
  if (NULL == pstReq->pu8Resp)
  {
    return DM_ALLOC;
  }
  memcpy(pstReq->pu8Resp, acHead, (size_t)iHead);
  if (NULL != pcHeaders)
  {
    memcpy(pstReq->pu8Resp + iHead, pcHeaders, strlen(pcHeaders));
    iHead += (int)strlen(pcHeaders);
  }
  memcpy(pstReq->pu8Resp + iHead, "\r\n", 2u);
  if ((NULL == pstReq->pstFile) && (u32Len > 0u))
  {
    memcpy(pstReq->pu8Resp + iHead + 2, pvBody, u32Len);
  }
  pstReq->bAnswered = DM_TRUE;
  return DM_SUCCESS;
}

/* 凭据 -> 连接和请求。没了回 DM_CONN_GONE，答过了回 DM_EXISTS，不是 HTTP 的回 DM_INVALID */
static int dm__http_req_by_reply(dm_in const dm_reply_st *pstReply, dm_out dm__http_st **ppstHttp,
                                 dm_out dm__http_req_st **ppstReq)
{
  dm__conn_st *pstConn = NULL;
  dm__http_st *pstHttp = NULL;
  dm_list_node_st *pstNode = NULL;

  if ((NULL == pstReply) || (DM__REPLY_MAGIC != pstReply->au32Priv[3]) ||
      (DM_SRC_HTTP != (uint8)(pstReply->au32Priv[2] >> 16)))
  {
    return DM_INVALID;
  }
  pstConn = dm__conn_find(pstReply->au32Priv[0]);
  pstHttp = ((NULL != pstConn) && !dm__conn_dead(pstConn)) ? (dm__http_st *)pstConn->pvHttp : NULL;
  if (NULL == pstHttp)
  {
    DM_LOG_DEBUG("http reply to conn %u: connection is gone", pstReply->au32Priv[0]);
    return DM_CONN_GONE;
  }
  DM_LIST_FOREACH(&pstHttp->stReqs, pstNode)
  {
    dm__http_req_st *pstReq = DM_LIST_ENTRY(pstNode, dm__http_req_st, stNode);
    if (pstReq->u32ReqID == pstReply->au32Priv[1])
    {
      if (pstReq->bAnswered || pstReq->bStreaming)
      {
        return DM_EXISTS;
      }
      *ppstHttp = pstHttp;
      *ppstReq = pstReq;
      return DM_SUCCESS;
    }
  }
  return DM_EXISTS; /* 答完、发出去了 */
}

int dm_http_reply(dm_in const dm_reply_st *pstReply, dm_in const dm_http_reply_opt_st *pstOpt)
{
  dm__http_st *pstHttp = NULL;
  dm__http_req_st *pstReq = NULL;
  int iErrCode = DM_SUCCESS;

  DM__CHECK_LOOP();
  if ((NULL == pstOpt) || ((NULL == pstOpt->pvBody) && (pstOpt->u32Len > 0u)))
  {
    return DM_INVALID;
  }
  iErrCode = dm__http_req_by_reply(pstReply, &pstHttp, &pstReq);
  if (DM_SUCCESS != iErrCode)
  {
    return iErrCode;
  }
  iErrCode = dm__http_set_resp(pstReq, pstOpt->uiStatus, pstOpt->pcType, pstOpt->pcHeaders,
                               pstOpt->pvBody, pstOpt->u32Len, NULL);
  if (DM_SUCCESS == iErrCode)
  {
    dm__http_flush(pstHttp);
  }
  return iErrCode;
}

/* ============================ 流式分块应答（AUD-03 SSE） ============================ */

static int dm__http_stream_find(dm_in const dm_reply_st *pstReply, dm_out dm__http_st **ppstHttp,
                                dm_out dm__http_req_st **ppstReq)
{
  dm__conn_st *pstConn = NULL;
  dm__http_st *pstHttp = NULL;
  dm_list_node_st *pstNode = NULL;

  if ((NULL == pstReply) || (DM__REPLY_MAGIC != pstReply->au32Priv[3]) ||
      (DM_SRC_HTTP != (uint8)(pstReply->au32Priv[2] >> 16)))
  {
    return DM_INVALID;
  }
  pstConn = dm__conn_find(pstReply->au32Priv[0]);
  pstHttp = ((NULL != pstConn) && !dm__conn_dead(pstConn)) ? (dm__http_st *)pstConn->pvHttp : NULL;
  if (NULL == pstHttp)
  {
    return DM_CONN_GONE;
  }
  DM_LIST_FOREACH(&pstHttp->stReqs, pstNode)
  {
    dm__http_req_st *pstReq = DM_LIST_ENTRY(pstNode, dm__http_req_st, stNode);
    if (pstReq->u32ReqID == pstReply->au32Priv[1])
    {
      if (!pstReq->bStreaming || pstReq->bAnswered)
      {
        return DM_INVALID;
      }
      *ppstHttp = pstHttp;
      *ppstReq = pstReq;
      return DM_SUCCESS;
    }
  }
  return DM_EXISTS;
}

static int dm__http_stream_append(dm_io dm__http_req_st *pstReq, dm_in const void *pvData,
                                  dm_in uint32 u32Len, dm_in bool_t bFinal)
{
  uint8 *pu8New = NULL;
  uint32 u32Total = 0u;
  uint32 u32Limit = bFinal ? DM_HTTP_BODY_MAX : (DM_HTTP_BODY_MAX - 5u);

  if ((u32Len > u32Limit) || (pstReq->u32RespLen > u32Limit - u32Len))
  {
    return DM_TOO_BIG;
  }
  u32Total = pstReq->u32RespLen + u32Len;
  pu8New = (NULL == pstReq->pu8Resp) ? (uint8 *)dm_malloc_tag(u32Total, DM_MEM_TAG_HTTP)
                                     : (uint8 *)dm_realloc_tag(pstReq->pu8Resp, u32Total);
  if (NULL == pu8New)
  {
    return DM_ALLOC;
  }
  memcpy(pu8New + pstReq->u32RespLen, pvData, u32Len);
  pstReq->pu8Resp = pu8New;
  pstReq->u32RespLen = u32Total;
  return DM_SUCCESS;
}

int dm_http_stream_begin(dm_in const dm_reply_st *pstReply, dm_in uint uiStatus,
                         dm_in const char *pcType, dm_in const char *pcHeaders)
{
  dm__http_st *pstHttp = NULL;
  dm__http_req_st *pstReq = NULL;
  char acHead[1024] = {0};
  int iHead = 0;
  size_t uExtra = (NULL != pcHeaders) ? strlen(pcHeaders) : 0u;
  bool_t bOk = DM_TRUE;
  int iErrCode = DM_SUCCESS;

  DM__CHECK_LOOP();
  iErrCode = dm__http_req_by_reply(pstReply, &pstHttp, &pstReq);
  if (DM_SUCCESS != iErrCode)
  {
    return iErrCode;
  }
  bOk = dm__http_cat(acHead, sizeof(acHead), &iHead, "HTTP/1.1 %u %s\r\n", uiStatus,
                     dm__http_reason(uiStatus));
  if (bOk && (NULL != pcType))
  {
    bOk = dm__http_cat(acHead, sizeof(acHead), &iHead, "Content-Type: %s\r\n", pcType);
  }
  if (bOk && (NULL != pstReq->pcOrigin))
  {
    bOk = dm__http_cat(acHead, sizeof(acHead), &iHead,
                       "Access-Control-Allow-Origin: %s\r\nVary: Origin\r\n", pstReq->pcOrigin);
  }
  if (bOk)
  {
    bOk = dm__http_cat(acHead, sizeof(acHead), &iHead,
                       "Cache-Control: no-cache\r\nTransfer-Encoding: chunked\r\n%s",
                       pstReq->bCloseAfter ? "Connection: close\r\n" : "");
  }
  if (!bOk || (uExtra > DM_HTTP_HEAD_MAX))
  {
    return DM_OVERFLOW;
  }
  pstReq->u32RespLen = (uint32)iHead + (uint32)uExtra + 2u;
  pstReq->pu8Resp = (uint8 *)dm_malloc_tag(pstReq->u32RespLen, DM_MEM_TAG_HTTP);
  if (NULL == pstReq->pu8Resp)
  {
    pstReq->u32RespLen = 0u;
    return DM_ALLOC;
  }
  memcpy(pstReq->pu8Resp, acHead, (size_t)iHead);
  if (uExtra > 0u)
  {
    memcpy(pstReq->pu8Resp + iHead, pcHeaders, uExtra);
  }
  memcpy(pstReq->pu8Resp + iHead + uExtra, "\r\n", 2u);
  pstReq->bStreaming = DM_TRUE;
  dm__http_flush(pstHttp);
  return DM_SUCCESS;
}

int dm_http_stream_write(dm_in const dm_reply_st *pstReply, dm_in const void *pvData,
                         dm_in uint32 u32Len)
{
  dm__http_st *pstHttp = NULL;
  dm__http_req_st *pstReq = NULL;
  char acPrefix[32] = {0};
  int iPrefix = 0;
  uint32 u32FrameLen = 0u;
  uint8 *pu8Frame = NULL;
  int iErrCode = DM_SUCCESS;

  DM__CHECK_LOOP();
  if ((NULL == pvData) && (u32Len > 0u))
  {
    return DM_INVALID;
  }
  iErrCode = dm__http_stream_find(pstReply, &pstHttp, &pstReq);
  if (DM_SUCCESS != iErrCode)
  {
    return iErrCode;
  }
  if (0u == u32Len)
  {
    return DM_SUCCESS;
  }
  if (u32Len > DM_HTTP_BODY_MAX)
  {
    return DM_TOO_BIG;
  }
  if (pstReq->bStreamHeadSent &&
      (dm_uv_stream_get_write_queue_size(dm__conn_stream(pstHttp->pstConn)) > DM_STREAM_HIGH))
  {
    return DM_BUSY;
  }
  iPrefix = snprintf(acPrefix, sizeof(acPrefix), "%x\r\n", (unsigned)u32Len);
  if ((iPrefix <= 0) || ((size_t)iPrefix >= sizeof(acPrefix)))
  {
    return DM_OVERFLOW;
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
  if (pstReq->bStreamHeadSent)
  {
    iErrCode = dm__conn_write_raw(pstHttp->pstConn, pu8Frame, u32FrameLen);
    if (DM_SUCCESS == iErrCode)
    {
      dm__http_idle_kick(pstHttp);
    }
  }
  else
  {
    iErrCode = dm__http_stream_append(pstReq, pu8Frame, u32FrameLen, DM_FALSE);
  }
  dm_free_tag(pu8Frame);
  return iErrCode;
}

int dm_http_stream_end(dm_in const dm_reply_st *pstReply)
{
  static const char s_acTerm[] = "0\r\n\r\n";
  dm__http_st *pstHttp = NULL;
  dm__http_req_st *pstReq = NULL;
  int iErrCode = DM_SUCCESS;

  DM__CHECK_LOOP();
  iErrCode = dm__http_stream_find(pstReply, &pstHttp, &pstReq);
  if (DM_SUCCESS != iErrCode)
  {
    return iErrCode;
  }
  if (pstReq->bStreamHeadSent)
  {
    dm__conn_st *pstConn = pstHttp->pstConn;
    bool_t bClose = pstReq->bCloseAfter;
    iErrCode = dm__conn_write_raw(pstConn, s_acTerm, 5u);
    pstReq->bStreaming = DM_FALSE;
    pstReq->bAnswered = DM_TRUE;
    if (pstReq->bDelivered && (pstHttp->u32Pending > 0u))
    {
      pstHttp->u32Pending--;
    }
    pstHttp->bSending = DM_FALSE;
    dm__http_req_free(pstReq);
    if (bClose)
    {
      dm__conn_close_after_write(pstConn);
    }
    else
    {
      dm__http_flush(pstHttp);
    }
    return iErrCode;
  }
  iErrCode = dm__http_stream_append(pstReq, s_acTerm, 5u, DM_TRUE);
  if (DM_SUCCESS != iErrCode)
  {
    dm__conn_close_later(pstHttp->pstConn, "stream end failed");
    return iErrCode;
  }
  pstReq->bStreaming = DM_FALSE;
  pstReq->bAnswered = DM_TRUE;
  dm__http_flush(pstHttp);
  return DM_SUCCESS;
}

/* ============================ 发文件（线程池里分块读） ============================ */

static void dm__http_file_done(dm_io dm__http_file_st *pstFile);

static void dm__http_file_on_wait(dm_in dm_uv_timer_st *pstTimer)
{
  dm__http_file_next(container_of(pstTimer, dm__http_file_st, stWait));
  return;
}

static void dm__http_file_on_read(dm_in dm_uv_fs_st *pstReq)
{
  dm__http_file_st *pstFile = container_of(pstReq, dm__http_file_st, stReq);
  ssize_t iGot = pstReq->result;

  pstFile->bReading = DM_FALSE;
  dm_uv_fs_req_cleanup(pstReq);
  if ((NULL == pstFile->pstConn) || (iGot <= 0))
  {
    if ((NULL != pstFile->pstConn) && !dm__conn_dead(pstFile->pstConn))
    {
      /* 读文件失败或文件在发送期间被截断：Content-Length 已发出，只能关闭连接 */
      DM_LOG_WARN("conn %u: file read failed or file got shorter, closing",
                  pstFile->pstConn->u32Id);
      dm__conn_close_later(pstFile->pstConn, "file read failed");
    }
    dm__http_file_done(pstFile);
    return;
  }
  (void)dm__conn_write_raw(pstFile->pstConn, pstFile->au8Buf, (uint32)iGot);
  pstFile->u64Off += (uint64)iGot;
  pstFile->u64Left -= (uint64)iGot;
  dm__http_file_next(pstFile);
  return;
}

static void dm__http_file_next(dm_io dm__http_file_st *pstFile)
{
  dm_uv_buf_st stBuf = {0};
  uint32 u32Want = (pstFile->u64Left < DM__FILE_CHUNK) ? (uint32)pstFile->u64Left : DM__FILE_CHUNK;

  if ((NULL == pstFile->pstConn) || dm__conn_dead(pstFile->pstConn) || (0u == pstFile->u64Left))
  {
    dm__http_file_done(pstFile);
    return;
  }
  if (dm_uv_stream_get_write_queue_size(dm__conn_stream(pstFile->pstConn)) > DM__FILE_HIGH)
  {
    (void)dm_uv_timer_start(&pstFile->stWait, dm__http_file_on_wait, 5u, 0); /* 等对方收一收 */
    return;
  }
  stBuf = dm_uv_buf_init((char *)pstFile->au8Buf, u32Want);
  pstFile->bReading = DM_TRUE;
  if (DM_SUCCESS != dm_uv_fs_read(dm_uv_default_loop(), &pstFile->stReq, pstFile->iFd, &stBuf, 1,
                                  (int64_t)pstFile->u64Off, dm__http_file_on_read))
  {
    pstFile->bReading = DM_FALSE;
    dm__http_file_done(pstFile);
  }
  return;
}

static void dm__http_file_on_closed(dm_in dm_uv_handle_st *pstHandle)
{
  dm_free_tag(container_of((dm_uv_timer_st *)pstHandle, dm__http_file_st, stWait));
  return;
}

/* 发完（或者连接没了）：关文件，接着发后面排着的应答 */
static void dm__http_file_done(dm_io dm__http_file_st *pstFile)
{
  dm_uv_fs_st stClose = {0};
  dm__conn_st *pstConn = pstFile->pstConn;

  (void)dm_uv_fs_close(NULL, &stClose, pstFile->iFd, NULL);
  dm_uv_fs_req_cleanup(&stClose);
  dm_uv_close((dm_uv_handle_st *)&pstFile->stWait, dm__http_file_on_closed);
  if ((NULL != pstConn) && (NULL != pstConn->pvHttp))
  {
    dm__http_st *pstHttp = (dm__http_st *)pstConn->pvHttp;
    pstHttp->bSending = DM_FALSE;
    pstHttp->pstSendFile = NULL;
    dm__http_flush(pstHttp);
  }
  return;
}

/* Range: bytes=a-b / a- / -n。范围不对返回 DM_RANGE；没有 Range 头返回 DM_NOT_FOUND */
static int dm__http_range(dm_in const char *pcRange, dm_in uint64 u64Size, dm_out uint64 *pu64From,
                          dm_out uint64 *pu64To)
{
  char *pcEnd = NULL;
  if (NULL == pcRange)
  {
    return DM_NOT_FOUND;
  }
  if ((0 != strncmp(pcRange, "bytes=", 6)) || (0u == u64Size))
  {
    return DM_RANGE;
  }
  pcRange += 6;
  if ('-' == pcRange[0])
  {
    uint64 u64N = strtoull(pcRange + 1, &pcEnd, 10);
    if ((0u == u64N) || (pcEnd == pcRange + 1))
    {
      return DM_RANGE;
    }
    *pu64From = (u64N >= u64Size) ? 0u : (u64Size - u64N);
    *pu64To = u64Size - 1u;
    return DM_SUCCESS;
  }
  *pu64From = strtoull(pcRange, &pcEnd, 10);
  if ((pcEnd == pcRange) || ('-' != *pcEnd) || (*pu64From >= u64Size))
  {
    return DM_RANGE;
  }
  if ('\0' == pcEnd[1])
  {
    *pu64To = u64Size - 1u;
    return DM_SUCCESS;
  }
  *pu64To = strtoull(pcEnd + 1, NULL, 10);
  if (*pu64To < *pu64From)
  {
    return DM_RANGE;
  }
  if (*pu64To >= u64Size)
  {
    *pu64To = u64Size - 1u;
  }
  return DM_SUCCESS;
}

int dm_http_reply_file(dm_in const dm_reply_st *pstReply, dm_in const char *pcPath,
                       dm_in const char *pcType)
{
  dm__http_st *pstHttp = NULL;
  dm__http_req_st *pstReq = NULL;
  dm__http_file_st *pstFile = NULL;
  dm_uv_fs_st stOpen = {0};
  dm_uv_fs_st stStat = {0};
  uint64 u64Size = 0;
  uint64 u64From = 0;
  uint64 u64To = 0;
  char acExtra[128] = {0};
  int iFd = -1;
  int iRange = DM_NOT_FOUND;
  int iErrCode = DM_SUCCESS;

  DM__CHECK_LOOP();
  iErrCode = dm__http_req_by_reply(pstReply, &pstHttp, &pstReq);
  if (DM_SUCCESS != iErrCode)
  {
    return iErrCode;
  }
  /* 仅允许普通文件：目录或 FIFO 一律返回 404（避免打开 FIFO 阻塞主线程） */
  memset(&stStat, 0, sizeof(stStat));
  if ((NULL != pcPath) && (0 == dm_uv_fs_stat(NULL, &stStat, pcPath, NULL)) &&
      (S_IFREG == (stStat.statbuf.st_mode & S_IFMT)))
  {
    memset(&stOpen, 0, sizeof(stOpen));
    iFd = dm_uv_fs_open(NULL, &stOpen, pcPath, DM_UV_FS_O_RDONLY, 0, NULL);
    dm_uv_fs_req_cleanup(&stOpen);
  }
  dm_uv_fs_req_cleanup(&stStat);
  if (iFd >= 0)
  {
    memset(&stStat, 0, sizeof(stStat));
    if (0 == dm_uv_fs_fstat(NULL, &stStat, iFd, NULL))
    {
      u64Size = stStat.statbuf.st_size;
    }
    dm_uv_fs_req_cleanup(&stStat);
  }
  if (iFd < 0)
  {
    if (DM_SUCCESS != dm__http_set_resp(pstReq, 404u, "text/plain", NULL, "not found", 9u, NULL))
    {
      dm__conn_close_later(pstHttp->pstConn, "out of memory");
    }
    dm__http_flush(pstHttp);
    return DM_NOT_FOUND;
  }
  iRange =
      dm__http_range(dm__pairs_get(pstReq->pcHeaders, "Range", DM_TRUE), u64Size, &u64From, &u64To);
  if (DM_RANGE == iRange)
  {
    dm_uv_fs_st stClose = {0};
    (void)dm_uv_fs_close(NULL, &stClose, iFd, NULL);
    dm_uv_fs_req_cleanup(&stClose);
    snprintf(acExtra, sizeof(acExtra), "Content-Range: bytes */%llu\r\n",
             (unsigned long long)u64Size);
    if (DM_SUCCESS != dm__http_set_resp(pstReq, 416u, NULL, NULL, NULL, 0u, acExtra))
    {
      dm__conn_close_later(pstHttp->pstConn, "out of memory");
    }
    dm__http_flush(pstHttp);
    return DM_SUCCESS;
  }
  if (DM_NOT_FOUND == iRange)
  {
    u64From = 0;
    u64To = (u64Size > 0u) ? (u64Size - 1u) : 0u;
  }
  pstFile = (dm__http_file_st *)dm_calloc_tag(1u, sizeof(dm__http_file_st), DM_MEM_TAG_HTTP);
  if (NULL == pstFile)
  {
    dm_uv_fs_st stClose = {0};
    (void)dm_uv_fs_close(NULL, &stClose, iFd, NULL);
    dm_uv_fs_req_cleanup(&stClose);
    return DM_ALLOC;
  }
  pstFile->iFd = (dm_uv_file)iFd;
  pstFile->u64Off = u64From;
  pstFile->u64Left = (u64Size > 0u) ? (u64To - u64From + 1u) : 0u;
  pstReq->pstFile = pstFile;
  if (DM_SUCCESS == iRange)
  {
    snprintf(acExtra, sizeof(acExtra),
             "Accept-Ranges: bytes\r\nContent-Range: bytes %llu-%llu/%llu\r\n",
             (unsigned long long)u64From, (unsigned long long)u64To, (unsigned long long)u64Size);
  }
  else
  {
    snprintf(acExtra, sizeof(acExtra), "Accept-Ranges: bytes\r\n");
  }
  iErrCode = dm__http_set_resp(pstReq, (DM_SUCCESS == iRange) ? 206u : 200u,
                               (NULL != pcType) ? pcType : "application/octet-stream", NULL, NULL,
                               pstFile->u64Left, acExtra);
  if (DM_SUCCESS != iErrCode)
  {
    dm_uv_fs_st stClose = {0};
    pstReq->pstFile = NULL;
    dm_free_tag(pstFile);
    (void)dm_uv_fs_close(NULL, &stClose, iFd, NULL);
    dm_uv_fs_req_cleanup(&stClose);
    return iErrCode;
  }
  (void)dm_uv_timer_init(dm_uv_default_loop(), &pstFile->stWait);
  dm__http_flush(pstHttp);
  return DM_SUCCESS;
}

/* ============================ 解析 ============================ */

static int dm__http_build_put(dm_io dm__http_st *pstHttp, dm_in const char *pc, dm_in size_t uLen)
{
  if (pstHttp->u32BuildLen + uLen + 2u > DM_HTTP_HEAD_MAX)
  {
    return DM_TOO_BIG;
  }
  if (pstHttp->u32BuildLen + uLen + 2u > pstHttp->u32BuildCap)
  {
    uint32 u32Cap = (0u != pstHttp->u32BuildCap) ? pstHttp->u32BuildCap : 1024u;
    char *pcNew = NULL;
    while (u32Cap < pstHttp->u32BuildLen + uLen + 2u)
    {
      u32Cap *= 2u;
    }
    pcNew = (NULL == pstHttp->pcBuild) ? (char *)dm_malloc_tag(u32Cap, DM_MEM_TAG_HTTP)
                                       : (char *)dm_realloc_tag(pstHttp->pcBuild, u32Cap);
    if (NULL == pcNew)
    {
      return DM_ALLOC;
    }
    pstHttp->pcBuild = pcNew;
    pstHttp->u32BuildCap = u32Cap;
  }
  memcpy(pstHttp->pcBuild + pstHttp->u32BuildLen, pc, uLen);
  pstHttp->u32BuildLen += (uint32)uLen;
  return DM_SUCCESS;
}

static int dm__http_build_end(dm_io dm__http_st *pstHttp)
{
  if (pstHttp->u32BuildLen + 1u > DM_HTTP_HEAD_MAX)
  {
    return DM_TOO_BIG;
  }
  if (pstHttp->u32BuildLen + 1u > pstHttp->u32BuildCap)
  {
    uint32 u32Cap = (0u != pstHttp->u32BuildCap) ? pstHttp->u32BuildCap : 1024u;
    char *pcNew = NULL;
    while (u32Cap < pstHttp->u32BuildLen + 1u)
    {
      u32Cap *= 2u;
    }
    pcNew = (NULL == pstHttp->pcBuild) ? (char *)dm_malloc_tag(u32Cap, DM_MEM_TAG_HTTP)
                                       : (char *)dm_realloc_tag(pstHttp->pcBuild, u32Cap);
    if (NULL == pcNew)
    {
      return DM_ALLOC;
    }
    pstHttp->pcBuild = pcNew;
    pstHttp->u32BuildCap = u32Cap;
  }
  pstHttp->pcBuild[pstHttp->u32BuildLen++] = '\0';
  return DM_SUCCESS;
}

static bool_t dm__http_eq_ci(dm_in const char *pcA, dm_in const char *pcB)
{
  if ((NULL == pcA) || (NULL == pcB))
  {
    return DM_FALSE;
  }
  while (('\0' != *pcA) && ('\0' != *pcB))
  {
    unsigned char u8A = (unsigned char)*pcA++;
    unsigned char u8B = (unsigned char)*pcB++;
    if ((u8A >= 'A') && (u8A <= 'Z'))
    {
      u8A = (unsigned char)(u8A - 'A' + 'a');
    }
    if ((u8B >= 'A') && (u8B <= 'Z'))
    {
      u8B = (unsigned char)(u8B - 'A' + 'a');
    }
    if (u8A != u8B)
    {
      return DM_FALSE;
    }
  }
  return (('\0' == *pcA) && ('\0' == *pcB)) ? DM_TRUE : DM_FALSE;
}

/* DM 自己答一个错误（排在这条连接前面的应答后面），答完关连接。
 * pcOrigin：命中白名单的来源（CORS 预检 204 需带 Access-Control-Allow-Origin），其余传 NULL */
static void dm__http_reject_ex(dm_io dm__http_st *pstHttp, dm_in uint uiStatus,
                               dm_in const char *pcWhy, dm_in const char *pcOrigin)
{
  dm__http_req_st *pstReq =
      (dm__http_req_st *)dm_calloc_tag(1u, sizeof(dm__http_req_st), DM_MEM_TAG_HTTP);

  DM_LOG_DEBUG("conn %u: http %u (%s)", pstHttp->pstConn->u32Id, uiStatus, pcWhy);
  pstHttp->bRejected = DM_TRUE;
  if (NULL == pstReq)
  {
    dm__conn_close_later(pstHttp->pstConn, "out of memory");
    return;
  }
  pstReq->u32ReqID = ++pstHttp->u32NextReq;
  pstReq->bCloseAfter =
      DM_TRUE; /* 答完关：解析器已经停在错误上了（预检的 204 也一样，浏览器会重连） */
  dm_list_add_tail(&pstHttp->stReqs, &pstReq->stNode);
  pstReq->pcOrigin = pcOrigin;
  if (DM_SUCCESS !=
      dm__http_set_resp(pstReq, uiStatus, (204u == uiStatus) ? NULL : "text/plain", NULL,
                        dm__http_reason(uiStatus),
                        (204u == uiStatus) ? 0u : (uint32)strlen(dm__http_reason(uiStatus)),
                        (204u == uiStatus) ? "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                                             "Access-Control-Allow-Headers: *\r\n"
                                             "Access-Control-Max-Age: 600\r\n"
                                           : NULL))
  {
    dm__conn_close_later(pstHttp->pstConn, "out of memory");
  }
  return;
}

static void dm__http_reject(dm_io dm__http_st *pstHttp, dm_in uint uiStatus,
                            dm_in const char *pcWhy)
{
  dm__http_reject_ex(pstHttp, uiStatus, pcWhy, NULL);
  return;
}

static int dm__on_url(dm_in llhttp_t *pstP, dm_in const char *pc, dm_in size_t uLen)
{
  dm__http_st *pstHttp = (dm__http_st *)pstP->data;
  if (DM_SUCCESS != dm__http_build_put(pstHttp, pc, uLen))
  {
    dm__http_reject(pstHttp, 431u, "request line too long");
    return -1;
  }
  return 0;
}

static int dm__on_url_done(dm_in llhttp_t *pstP)
{
  dm__http_st *pstHttp = (dm__http_st *)pstP->data;
  if (DM_SUCCESS != dm__http_build_end(pstHttp))
  {
    dm__http_reject(pstHttp, 431u, "request line too long");
    return -1;
  }
  pstHttp->u32UrlEnd = pstHttp->u32BuildLen;
  return 0;
}

static int dm__on_field(dm_in llhttp_t *pstP, dm_in const char *pc, dm_in size_t uLen)
{
  dm__http_st *pstHttp = (dm__http_st *)pstP->data;
  if (DM_SUCCESS != dm__http_build_put(pstHttp, pc, uLen))
  {
    dm__http_reject(pstHttp, 431u, "headers too long");
    return -1;
  }
  return 0;
}

static int dm__on_field_done(dm_in llhttp_t *pstP)
{
  dm__http_st *pstHttp = (dm__http_st *)pstP->data;
  if (DM_SUCCESS != dm__http_build_end(pstHttp))
  {
    dm__http_reject(pstHttp, 431u, "headers too long");
    return -1;
  }
  return 0;
}

static int dm__on_value(dm_in llhttp_t *pstP, dm_in const char *pc, dm_in size_t uLen)
{
  return dm__on_field(pstP, pc, uLen);
}

/* Host 仅接受 127.0.0.1 或 localhost 加本机端口（DD-01 防 DNS 重绑定） */
static bool_t dm__http_host_ok(dm_in const char *pcHost)
{
  char acA[64] = {0};
  char acB[64] = {0};
  snprintf(acA, sizeof(acA), "127.0.0.1:%d", dm__http_port());
  snprintf(acB, sizeof(acB), "localhost:%d", dm__http_port());
  return ((NULL != pcHost) && ((0 == strcmp(pcHost, acA)) || (0 == strcmp(pcHost, acB))))
             ? DM_TRUE
             : DM_FALSE;
}

static int dm__http_hex_val(dm_in char cCh)
{
  if ((cCh >= '0') && (cCh <= '9'))
  {
    return (int)(cCh - '0');
  }
  if ((cCh >= 'a') && (cCh <= 'f'))
  {
    return (int)(cCh - 'a' + 10);
  }
  if ((cCh >= 'A') && (cCh <= 'F'))
  {
    return (int)(cCh - 'A' + 10);
  }
  return -1;
}

static bool_t dm__http_has_dotdot(dm_in const char *pcPath)
{
  const char *pc = pcPath;
  while ('\0' != *pc)
  {
    const char *pcSeg = NULL;
    size_t uSegLen = 0u;
    while ('/' == *pc)
    {
      pc++;
    }
    pcSeg = pc;
    while (('\0' != *pc) && ('/' != *pc))
    {
      pc++;
    }
    uSegLen = (size_t)(pc - pcSeg);
    if ((2u == uSegLen) && ('.' == pcSeg[0]) && ('.' == pcSeg[1]))
    {
      return DM_TRUE;
    }
  }
  return DM_FALSE;
}

/* T-BND-06：HTTP 入口路径穿越与非法编码拦截（含多重编码
 * ../、..\、%2e%2e%2f、%252e、%5c、%00、控制字符） */
static bool_t dm__http_path_unsafe(dm_in const char *pcUrl)
{
  char acCur[2048] = {0};
  char acNext[2048] = {0};
  size_t uLen = 0u;
  uint uiPass = 0u;

  if (NULL == pcUrl)
  {
    return DM_TRUE;
  }
  while (('\0' != pcUrl[uLen]) && ('?' != pcUrl[uLen]))
  {
    unsigned char uc = (unsigned char)pcUrl[uLen];
    if ((uc < 0x20u) || (0x7Fu == uc) || ('\\' == uc) || ((uLen + 1u) >= sizeof(acCur)))
    {
      return DM_TRUE;
    }
    acCur[uLen] = (char)uc;
    uLen++;
  }
  acCur[uLen] = '\0';

  if (dm__http_has_dotdot(acCur))
  {
    return DM_TRUE;
  }

  for (uiPass = 0u; uiPass < 4u; uiPass++)
  {
    size_t uOut = 0u;
    const char *pc = acCur;
    bool_t bChanged = DM_FALSE;

    while ('\0' != *pc)
    {
      unsigned char uc = (unsigned char)*pc++;
      if ('%' == uc)
      {
        int iHi = dm__http_hex_val(pc[0]);
        int iLo = (iHi >= 0) ? dm__http_hex_val(pc[1]) : -1;
        if ((iHi < 0) || (iLo < 0))
        {
          return DM_TRUE;
        }
        uc = (unsigned char)((iHi << 4) | iLo);
        pc += 2;
        bChanged = DM_TRUE;
      }
      if ((uc < 0x20u) || (0x7Fu == uc) || ('\\' == uc) || ((uOut + 1u) >= sizeof(acNext)))
      {
        return DM_TRUE;
      }
      acNext[uOut++] = (char)uc;
    }
    acNext[uOut] = '\0';

    if (dm__http_has_dotdot(acNext))
    {
      return DM_TRUE;
    }
    if (!bChanged)
    {
      return DM_FALSE;
    }
    memcpy(acCur, acNext, uOut + 1u);
  }
  /* 超过 4 轮仍有百分号编码视为恶意多重编码 */
  return (NULL != strchr(acCur, '%')) ? DM_TRUE : DM_FALSE;
}

/* Origin：没带放行（不是浏览器）；带了就必须在白名单里。返回白名单里那一项，或 "" = 没带，NULL =
 * 不行 */
static const char *dm__http_origin(dm_in const char *pcOrigin)
{
  uint uiIdx = 0;
  if (NULL == pcOrigin)
  {
    return "";
  }
  for (uiIdx = 0; (NULL != g_ppcOrigin) && (NULL != g_ppcOrigin[uiIdx]); uiIdx++)
  {
    if (0 == strcmp(g_ppcOrigin[uiIdx], pcOrigin))
    {
      return g_ppcOrigin[uiIdx];
    }
  }
  return NULL;
}

/* 仅放行单一且修剪后精确等于 "chunked" 的 Transfer-Encoding；重头返 400，复合/非 chunked 返 411 */
static bool_t dm__http_te_chunked_only(dm_in const char *pcHeaders, dm_out uint *puiStatus)
{
  const char *pc = pcHeaders;
  const char *pcValFound = NULL;
  uint32 u32Count = 0u;

  *puiStatus = 411u;
  while ((NULL != pc) && ('\0' != *pc))
  {
    const char *pcVal = pc + strlen(pc) + 1u;
    if (dm__http_eq_ci(pc, "Transfer-Encoding"))
    {
      u32Count++;
      pcValFound = pcVal;
    }
    pc = pcVal + strlen(pcVal) + 1u;
  }
  if (0u == u32Count)
  {
    *puiStatus = 411u;
    return DM_FALSE;
  }
  if (u32Count > 1u)
  {
    *puiStatus = 400u;
    return DM_FALSE;
  }
  while ((' ' == *pcValFound) || ('\t' == *pcValFound))
  {
    pcValFound++;
  }
  {
    size_t uLen = strlen(pcValFound);
    char acTrim[16] = {0};
    while ((uLen > 0u) && ((' ' == pcValFound[uLen - 1u]) || ('\t' == pcValFound[uLen - 1u])))
    {
      uLen--;
    }
    if ((7u != uLen) || (uLen >= sizeof(acTrim)))
    {
      *puiStatus = 411u;
      return DM_FALSE;
    }
    memcpy(acTrim, pcValFound, uLen);
    acTrim[uLen] = '\0';
    if (!dm__http_eq_ci(acTrim, "chunked"))
    {
      *puiStatus = 411u;
      return DM_FALSE;
    }
  }
  return DM_TRUE;
}

static int dm__on_headers_done(dm_in llhttp_t *pstP)
{
  dm__http_st *pstHttp = (dm__http_st *)pstP->data;
  const char *pcHeaders = NULL;
  const char *pcUpgrade = NULL;
  const char *pcOrigin = NULL;

  if (DM_SUCCESS != dm__http_build_end(pstHttp)) /* 头的对以 "\0" 结尾 */
  {
    dm__http_reject(pstHttp, 431u, "headers too long");
    return -1;
  }
  pcHeaders = pstHttp->pcBuild + pstHttp->u32UrlEnd;
  if (!dm__http_host_ok(dm__pairs_get(pcHeaders, "Host", DM_TRUE)))
  {
    dm__http_reject(pstHttp, 403u, "bad host");
    return -1;
  }
  pcOrigin = dm__http_origin(dm__pairs_get(pcHeaders, "Origin", DM_TRUE));
  if (NULL == pcOrigin)
  {
    dm__http_reject(pstHttp, 403u, "origin not allowed");
    return -1;
  }
  if ((0u != (pstP->flags & F_TRANSFER_ENCODING)) && (0u != (pstP->flags & F_CONTENT_LENGTH)))
  {
    dm__http_reject(pstHttp, 400u, "both content-length and transfer-encoding");
    return -1;
  }
  if (0u != (pstP->flags & (F_TRANSFER_ENCODING | F_CHUNKED)))
  {
    uint uiTeStatus = 411u;
    if ((0u == (pstP->flags & F_CHUNKED)) || !dm__http_te_chunked_only(pcHeaders, &uiTeStatus))
    {
      dm__http_reject(pstHttp, uiTeStatus, "unsupported transfer-encoding");
      return -1;
    }
  }
  if ((0u == (pstP->flags & F_CHUNKED)) && (pstP->content_length > DM_HTTP_BODY_MAX))
  {
    dm__http_reject(pstHttp, 413u, "body too big");
    return -1;
  }
  if ((HTTP_OPTIONS == pstP->method) && ('\0' != pcOrigin[0]))
  {
    dm__http_reject_ex(pstHttp, 204u, "preflight", pcOrigin); /* 预检 DM 直接答，不交给处理项 */
    return -1;
  }
  if (dm__http_path_unsafe(pstHttp->pcBuild))
  {
    dm__http_reject(pstHttp, 403u, "unsafe path");
    return -1;
  }
  pcUpgrade = dm__pairs_get(pcHeaders, "Upgrade", DM_TRUE);
  pstHttp->bWantUpgrade = DM_FALSE;
  if ((0 == strncmp(pstHttp->pcBuild, "/ws", 3)) &&
      (('\0' == pstHttp->pcBuild[3]) || ('?' == pstHttp->pcBuild[3])))
  {
    if ((HTTP_GET != pstP->method) || (NULL == pcUpgrade) ||
        !dm__http_eq_ci(pcUpgrade, "websocket") ||
        (NULL == dm__pairs_get(pcHeaders, "Sec-WebSocket-Key", DM_TRUE)) ||
        (0u != pstHttp->u32Pending))
    {
      dm__http_reject(pstHttp, 400u, "bad websocket upgrade");
      return -1;
    }
    pstHttp->bWantUpgrade = DM_TRUE;
  }
  return 0;
}

static int dm__on_body(dm_in llhttp_t *pstP, dm_in const char *pc, dm_in size_t uLen)
{
  dm__http_st *pstHttp = (dm__http_st *)pstP->data;
  if ((uLen > DM_HTTP_BODY_MAX) || (pstHttp->u32BodyLen > (DM_HTTP_BODY_MAX - (uint32)uLen)))
  {
    dm__http_reject(pstHttp, 413u, "body too big");
    return -1;
  }
  if (pstHttp->u32BodyLen + uLen > pstHttp->u32BodyCap)
  {
    uint32 u32Need = pstHttp->u32BodyLen + (uint32)uLen;
    uint32 u32Cap = 0u;
    uint8 *pu8New = NULL;
    if ((0u != (pstP->flags & F_CONTENT_LENGTH)) && (0u == (pstP->flags & F_CHUNKED)) &&
        (pstP->content_length <= (uint64)DM_HTTP_BODY_MAX))
    {
      u32Cap = (uint32)pstP->content_length + pstHttp->u32BodyLen;
      if (u32Cap > DM_HTTP_BODY_MAX)
      {
        u32Cap = DM_HTTP_BODY_MAX;
      }
    }
    if (u32Cap < u32Need)
    {
      u32Cap = (0u != pstHttp->u32BodyCap) ? pstHttp->u32BodyCap : 1024u;
      while (u32Cap < u32Need)
      {
        u32Cap = (u32Cap > (DM_HTTP_BODY_MAX / 2u)) ? DM_HTTP_BODY_MAX : (u32Cap * 2u);
      }
    }
    pu8New = (NULL == pstHttp->pu8Body) ? (uint8 *)dm_malloc_tag(u32Cap, DM_MEM_TAG_HTTP)
                                        : (uint8 *)dm_realloc_tag(pstHttp->pu8Body, u32Cap);
    if (NULL == pu8New)
    {
      return -1;
    }
    pstHttp->pu8Body = pu8New;
    pstHttp->u32BodyCap = u32Cap;
  }
  memcpy(pstHttp->pu8Body + pstHttp->u32BodyLen, pc, uLen);
  pstHttp->u32BodyLen += (uint32)uLen;
  return 0;
}

/* url 拆成路径和查询，和头一起放进一块：路径\0 k\0v\0…\0 name\0value\0…\0。没有 = 的值是空串 */
static char *dm__http_split_url(dm_in const char *pcUrl, dm_out char **ppcQuery,
                                dm_in const char *pcHeaders, dm_in uint32 u32HeadLen,
                                dm_out char **ppcHeaders)
{
  size_t uUrl = strlen(pcUrl);
  char *pcAll = (char *)dm_malloc_tag(uUrl * 2u + 4u + u32HeadLen, DM_MEM_TAG_HTTP);
  const char *pcMark = strchr(pcUrl, '?');
  size_t uPath = (NULL != pcMark) ? (size_t)(pcMark - pcUrl) : uUrl;
  char *pcQ = NULL;
  size_t uOut = 0;
  const char *pc = NULL;
  bool_t bInValue = DM_FALSE;
  bool_t bInPair = DM_FALSE;

  if (NULL == pcAll)
  {
    return NULL;
  }
  memcpy(pcAll, pcUrl, uPath);
  pcAll[uPath] = '\0';
  pcQ = pcAll + uPath + 1;
  for (pc = (NULL != pcMark) ? pcMark + 1 : "";; pc++)
  {
    if (('&' == *pc) || ('\0' == *pc))
    {
      if (bInPair)
      {
        pcQ[uOut++] = '\0'; /* 结束键或值 */
        if (!bInValue)
        {
          pcQ[uOut++] = '\0'; /* 没有 = ：值是空串 */
        }
      }
      bInPair = DM_FALSE;
      bInValue = DM_FALSE;
      if ('\0' == *pc)
      {
        break;
      }
      continue;
    }
    bInPair = DM_TRUE;
    if (('=' == *pc) && !bInValue)
    {
      pcQ[uOut++] = '\0';
      bInValue = DM_TRUE;
      continue;
    }
    pcQ[uOut++] = *pc;
  }
  pcQ[uOut++] = '\0';
  *ppcQuery = pcQ;
  *ppcHeaders = pcQ + uOut;
  memcpy(*ppcHeaders, pcHeaders, u32HeadLen);
  return pcAll;
}

static int dm__on_message_done(dm_in llhttp_t *pstP)
{
  dm__http_st *pstHttp = (dm__http_st *)pstP->data;
  dm__http_req_st *pstReq = NULL;
  dm_link_msg_st stMsg = {0};
  uint uiStatus = 0;
  uint uiLinkID = 0;
  const char *pcMethod = llhttp_method_name((llhttp_method_t)pstP->method);
  const char *pcHeaders = pstHttp->pcBuild + pstHttp->u32UrlEnd;
  uint32 u32HeadLen = pstHttp->u32BuildLen - pstHttp->u32UrlEnd;
  int iRet = 0;

  if (pstHttp->bWantUpgrade)
  {
    return HPE_PAUSED; /* 升级：剩下的字节交给 ws.c（在 dm__http_parse 里） */
  }
  pstReq = (dm__http_req_st *)dm_calloc_tag(1u, sizeof(dm__http_req_st), DM_MEM_TAG_HTTP);
  if (NULL != pstReq)
  {
    pstReq->pcPath = dm__http_split_url(pstHttp->pcBuild, &pstReq->pcQuery, pcHeaders, u32HeadLen,
                                        &pstReq->pcHeaders);
  }
  if ((NULL == pstReq) || (NULL == pstReq->pcPath))
  {
    dm_free_tag(pstReq);
    dm__http_reject(pstHttp, 500u, "out of memory");
    return -1;
  }
  uiLinkID = dm__route_find(pcMethod, pstReq->pcPath, &uiStatus); /* 按路径（不带查询）找 */
  if (0u == uiLinkID)
  {
    dm_free_tag(pstReq->pcPath);
    dm_free_tag(pstReq);
    dm__http_reject(pstHttp, uiStatus, "no route");
    return -1;
  }
  pstReq->u32ReqID = ++pstHttp->u32NextReq;
  pstReq->uiLinkID = uiLinkID;
  pstReq->pcOrigin = dm__http_origin(dm__pairs_get(pstReq->pcHeaders, "Origin", DM_TRUE));
  if ((NULL != pstReq->pcOrigin) && ('\0' == pstReq->pcOrigin[0]))
  {
    pstReq->pcOrigin = NULL;
  }
  dm_list_add_tail(&pstHttp->stReqs, &pstReq->stNode);
  memset(&stMsg, 0, sizeof(stMsg));
  stMsg.pu8Data = pstHttp->pu8Body;
  stMsg.u32Len = pstHttp->u32BodyLen;
  stMsg.u8Src = DM_SRC_HTTP;
  if (DM_SUCCESS ==
      dm__link_enqueue(uiLinkID, &stMsg, DM_TRUE, pstHttp->pstConn->u32Id, pstReq->u32ReqID))
  {
    pstReq->bDelivered = DM_TRUE;
    pstHttp->u32Pending++;
  }
  else
  {
    if (DM_SUCCESS != dm__http_set_resp(pstReq, 503u, "text/plain", NULL, "busy", 4u, NULL))
    {
      dm__conn_close_later(pstHttp->pstConn, "out of memory");
    }
  }
  /* 这一个收完了：清掉解析用的，等下一个 */
  pstHttp->u32BuildLen = 0;
  pstHttp->u32BodyLen = 0;
  if (pstHttp->u32BodyCap > DM__RX_KEEP)
  {
    dm_free_tag(pstHttp->pu8Body);
    pstHttp->pu8Body = NULL;
    pstHttp->u32BodyCap = 0;
  }
  if (pstHttp->u32Pending >= DM_HTTP_PENDING)
  {
    iRet = HPE_PAUSED; /* 没答的到上限：停读，答掉一个再接着解 */
  }
  return iRet;
}

static int dm__on_message_begin(dm_in llhttp_t *pstP)
{
  dm__http_st *pstHttp = (dm__http_st *)pstP->data;
  pstHttp->u32BuildLen = 0;
  pstHttp->u32BodyLen = 0;
  pstHttp->u32UrlEnd = 0;
  return 0;
}

/* 解一段；llhttp 停下来（满了 / 升级）时把剩下的存起来 */
static void dm__http_parse(dm_io dm__http_st *pstHttp, dm_in const uint8 *pu8, dm_in uint32 u32Len)
{
  dm__conn_st *pstConn = pstHttp->pstConn;
  llhttp_errno_t eErr = llhttp_execute(&pstHttp->stParser, (const char *)pu8, u32Len);
  const uint8 *pu8Pos = NULL;
  uint32 u32Rest = 0;

  if (HPE_OK == eErr)
  {
    return;
  }
  pu8Pos = (const uint8 *)llhttp_get_error_pos(&pstHttp->stParser);
  u32Rest = (NULL != pu8Pos) ? (uint32)(pu8 + u32Len - pu8Pos) : 0u;
  if ((HPE_PAUSED == eErr) && pstHttp->bWantUpgrade)
  {
    char acKey[64] = {0};
    const char *pcKey =
        dm__pairs_get(pstHttp->pcBuild + pstHttp->u32UrlEnd, "Sec-WebSocket-Key", DM_TRUE);
    uint8 *pu8Copy = NULL;
    snprintf(acKey, sizeof(acKey), "%s", (NULL != pcKey) ? pcKey : "");
    if (u32Rest > 0u)
    {
      pu8Copy = (uint8 *)dm_malloc_tag(u32Rest, DM_MEM_TAG_HTTP);
      if (NULL == pu8Copy)
      {
        dm__conn_close(pstConn, "out of memory");
        return;
      }
      memcpy(pu8Copy, pu8Pos, u32Rest);
    }
    dm__http_conn_gone(pstConn); /* HTTP 的状态不要了（没有挂着的请求，前面查过） */
    if (DM_SUCCESS != dm__ws_upgrade(pstConn, acKey, pu8Copy, u32Rest))
    {
      dm__conn_close(pstConn, "out of memory");
    }
    dm_free_tag(pu8Copy);
    return;
  }
  if (HPE_PAUSED_UPGRADE == eErr)
  {
    /* 别的路径带了 Upgrade：不升级，照普通请求处理（已经交了），接着解后面的 */
    llhttp_resume_after_upgrade(&pstHttp->stParser);
    if (u32Rest > 0u)
    {
      dm__http_parse(pstHttp, pu8Pos, u32Rest);
    }
    return;
  }
  if (HPE_PAUSED == eErr)
  {
    /* 没答的到上限：剩下的存起来，停读 */
    pstHttp->bPaused = DM_TRUE;
    dm__flow_stop(&pstConn->stFlow, DM__STOP_HTTP);
    if (u32Rest > 0u)
    {
      uint8 *pu8New = (uint8 *)dm_malloc_tag(pstHttp->u32LeftLen + u32Rest, DM_MEM_TAG_HTTP);
      if (NULL != pu8New)
      {
        memcpy(pu8New, pu8Pos, u32Rest);
        memcpy(pu8New + u32Rest, (NULL != pstHttp->pu8Left) ? pstHttp->pu8Left : pu8New,
               pstHttp->u32LeftLen);
        dm_free_tag(pstHttp->pu8Left);
        pstHttp->pu8Left = pu8New;
        pstHttp->u32LeftLen += u32Rest;
      }
    }
    return;
  }
  if (!pstHttp->bRejected)
  {
    dm__http_reject(pstHttp, 400u, llhttp_errno_name(eErr));
  }
  dm__http_flush(pstHttp);
  return;
}

static void dm__http_resume_parse(dm_io dm__http_st *pstHttp)
{
  dm__conn_st *pstConn = pstHttp->pstConn; /* 解析当中可能升级、关掉 HTTP 状态：先记下连接 */
  uint8 *pu8Left = pstHttp->pu8Left;
  uint32 u32Left = pstHttp->u32LeftLen;

  if ((NULL == pstConn) || dm__conn_dead(pstConn))
  {
    return; /* 连接已进入关闭态：剩余请求不再解析，缓存字节随 HTTP 状态释放 */
  }
  pstHttp->bPaused = DM_FALSE;
  pstHttp->pu8Left = NULL;
  pstHttp->u32LeftLen = 0;
  llhttp_resume(&pstHttp->stParser);
  if (u32Left > 0u)
  {
    dm__http_parse(pstHttp, pu8Left, u32Left);
  }
  dm_free_tag(pu8Left);
  if ((NULL == pstConn) || ((void *)pstHttp != pstConn->pvHttp))
  {
    return; /* 解析过程中已升级为 WebSocket 或 HTTP 状态已销毁，不再访问 */
  }
  if (!pstHttp->bPaused)
  {
    dm__flow_resume(&pstConn->stFlow, DM__STOP_HTTP);
  }
  /* 刚才解析出来、DM 当场答了的（404、503 这类）发出去。这里不会再回到 resume：
   * 又停下来只会是因为没答的到了上限，flush 那时不恢复解析 */
  dm__http_flush(pstHttp);
  return;
}

/* ============================ 连接 ============================ */

static void dm__http_on_idle(dm_in dm_uv_timer_st *pstTimer)
{
  dm__http_st *pstHttp = (dm__http_st *)pstTimer;
  if ((NULL == pstHttp->pstConn) || (NULL == pstHttp->pstConn->pvHttp))
  {
    return;
  }
  if ((0u != pstHttp->u32Pending) || pstHttp->bSending ||
      (pstHttp->stReqs.stHead.pstNext != &pstHttp->stReqs.stHead))
  {
    (void)dm_uv_timer_start(&pstHttp->stIdle, dm__http_on_idle, DM_HTTP_IDLE_MS,
                            0); /* 有挂着的不算空闲 */
    return;
  }
  dm__conn_close(pstHttp->pstConn, "http idle");
  return;
}

void dm__http_conn_new(dm_io dm__conn_st *pstConn)
{
  dm__http_st *pstHttp = (dm__http_st *)dm_calloc_tag(1u, sizeof(dm__http_st), DM_MEM_TAG_HTTP);

  if (NULL == pstHttp)
  {
    dm__conn_close(pstConn, "out of memory");
    return;
  }
  if (!g_bHttpSettingsUp)
  {
    llhttp_settings_init(&g_stHttpSettings);
    g_stHttpSettings.on_message_begin = dm__on_message_begin;
    g_stHttpSettings.on_url = dm__on_url;
    g_stHttpSettings.on_url_complete = dm__on_url_done;
    g_stHttpSettings.on_header_field = dm__on_field;
    g_stHttpSettings.on_header_field_complete = dm__on_field_done;
    g_stHttpSettings.on_header_value = dm__on_value;
    g_stHttpSettings.on_header_value_complete = dm__on_field_done;
    g_stHttpSettings.on_headers_complete = dm__on_headers_done;
    g_stHttpSettings.on_body = dm__on_body;
    g_stHttpSettings.on_message_complete = dm__on_message_done;
    g_bHttpSettingsUp = DM_TRUE;
  }
  llhttp_init(&pstHttp->stParser, HTTP_REQUEST, &g_stHttpSettings);
  pstHttp->stParser.data = pstHttp;
  pstHttp->pstConn = pstConn;
  dm_list_init(&pstHttp->stReqs);
  pstConn->pvHttp = pstHttp;
  (void)dm_uv_timer_init(dm_uv_default_loop(), &pstHttp->stIdle);
  (void)dm_uv_timer_start(&pstHttp->stIdle, dm__http_on_idle, DM_HTTP_IDLE_MS, 0);
  return;
}

void dm__http_conn_data(dm_io dm__conn_st *pstConn, dm_in const uint8 *pu8, dm_in uint32 u32Len)
{
  dm__http_st *pstHttp = (dm__http_st *)pstConn->pvHttp;
  if ((NULL == pstHttp) || pstHttp->bRejected)
  {
    return;
  }
  (void)dm_uv_timer_start(&pstHttp->stIdle, dm__http_on_idle, DM_HTTP_IDLE_MS, 0);
  if (pstHttp->bPaused)
  {
    return; /* 停读期间到的（读回调里剩的）：不会有，流已经停了 */
  }
  dm__http_parse(pstHttp, pu8, u32Len);
  if ((void *)pstHttp == pstConn->pvHttp)
  {
    dm__http_flush(pstHttp); /* 刷出解析阶段直接生成的即时应答（如入队失败时的 503） */
  }
  return;
}

static void dm__http_on_closed(dm_in dm_uv_handle_st *pstHandle)
{
  dm__http_st *pstHttp = (dm__http_st *)pstHandle;
  dm_free_tag(pstHttp->pcBuild);
  dm_free_tag(pstHttp->pu8Body);
  dm_free_tag(pstHttp->pu8Left);
  dm_free_tag(pstHttp);
  return;
}

/* 连接断了（或者升级成 WebSocket 了）：没答的各交一条 DM_SRC_HTTP_GONE，然后放掉 */
void dm__http_conn_gone(dm_io dm__conn_st *pstConn)
{
  dm__http_st *pstHttp = (dm__http_st *)pstConn->pvHttp;
  dm_list_node_st *pstNode = NULL;
  dm_list_node_st *pstNext = NULL;
  dm_link_msg_st stMsg = {0};

  pstConn->pvHttp = NULL;
  DM_LIST_FOREACH_SAFE(&pstHttp->stReqs, pstNode, pstNext)
  {
    dm__http_req_st *pstReq = DM_LIST_ENTRY(pstNode, dm__http_req_st, stNode);
    if (pstReq->bDelivered && !pstReq->bAnswered && (dm__phase() < DM__PHASE_TEARDOWN))
    {
      memset(&stMsg, 0, sizeof(stMsg));
      stMsg.u8Src = DM_SRC_HTTP_GONE;
      (void)dm__link_enqueue_ex(pstReq->uiLinkID, &stMsg, DM_FALSE, pstConn->u32Id,
                                pstReq->u32ReqID, DM_SRC_HTTP);
    }
    if (NULL != pstReq->pstFile)
    {
      pstReq->pstFile->pstConn = NULL;
      dm__http_file_done(pstReq->pstFile);
    }
    dm__http_req_free(pstReq);
  }
  if (NULL != pstHttp->pstSendFile)
  {
    dm__http_file_st *pstFile = pstHttp->pstSendFile;
    pstHttp->pstSendFile = NULL;
    pstFile->pstConn = NULL;
    if (!pstFile->bReading)
    {
      (void)dm_uv_timer_stop(&pstFile->stWait);
      dm__http_file_done(pstFile); /* 在等积压降下来：现在就收拾；正读着的等它回来自己收拾 */
    }
  }
  pstHttp->pstConn = NULL;
  (void)dm_uv_timer_stop(&pstHttp->stIdle);
  dm_uv_close((dm_uv_handle_st *)&pstHttp->stIdle, dm__http_on_closed);
  return;
}

#ifdef __cplusplus
}
#endif
