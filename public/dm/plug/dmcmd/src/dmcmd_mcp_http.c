/**
 ******************************************************************************
 * @file    dmcmd_mcp_http.c
 * @brief   dmcmd mcp 单请求 HTTP/1.1 与 SSE 增量流客户端（DD-02 §3.4，FIX-04/08，AUD-03）：
 *          - 响应体硬上限 16MB；
 *          - 状态行 >= 12B 安全校验（FIX-08）与安全 ASCII 小写比较（FIX-04）；
 *          - 严格 RFC 9112 chunked 解码、SSE 增量 data: 刷出与 : keepalive 续期。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "dmcmd_mcp_int.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define DMCMD__MCP_HEAD_MAX (64u * 1024u)

static unsigned char dmcmd__mcp_lower(in unsigned char u8Ch)
{
  return ((u8Ch >= 'A') && (u8Ch <= 'Z')) ? (unsigned char)(u8Ch - 'A' + 'a') : u8Ch;
}

static bool_t dmcmd__mcp_strcasestr(in const char *pcHay, in const char *pcNeedle)
{
  size_t uN = 0u;
  const char *pc = pcHay;
  if ((NULL == pcHay) || (NULL == pcNeedle))
  {
    return DM_FALSE;
  }
  uN = strlen(pcNeedle);
  if (0u == uN)
  {
    return DM_TRUE;
  }
  for (; '\0' != *pc; pc++)
  {
    size_t uIdx = 0u;
    while ((uIdx < uN) && ('\0' != pc[uIdx]) &&
           (dmcmd__mcp_lower((unsigned char)pc[uIdx]) ==
            dmcmd__mcp_lower((unsigned char)pcNeedle[uIdx])))
    {
      uIdx++;
    }
    if (uIdx == uN)
    {
      return DM_TRUE;
    }
  }
  return DM_FALSE;
}

/* ============================ HTTP 头解析（FIX-04 / FIX-08） ============================ */

static int dmcmd__mcp_parse_status(in const char *pcHead, in size_t uHeadLen)
{
  const char *pcCrlf = NULL;
  size_t uLineLen = 0u;
  int iCode = 0;

  if ((NULL == pcHead) || (uHeadLen < 12u))
  {
    return 0;
  }
  pcCrlf = strstr(pcHead, "\r\n");
  uLineLen = (NULL != pcCrlf) ? (size_t)(pcCrlf - pcHead) : uHeadLen;
  if ((uLineLen < 12u) || (0 != memcmp(pcHead, "HTTP/1.", 7u)) ||
      (('0' != pcHead[7]) && ('1' != pcHead[7])) || (' ' != pcHead[8]) ||
      !isdigit((unsigned char)pcHead[9]) || !isdigit((unsigned char)pcHead[10]) ||
      !isdigit((unsigned char)pcHead[11]))
  {
    return 0;
  }
  if ((uLineLen > 12u) && (' ' != pcHead[12]) && ('\t' != pcHead[12]))
  {
    return 0;
  }
  iCode = (pcHead[9] - '0') * 100 + (pcHead[10] - '0') * 10 + (pcHead[11] - '0');
  return iCode;
}

static bool_t dmcmd__mcp_get_header(in const char *pcHead, in const char *pcName, out char *pcVal,
                                    in size_t uValCap)
{
  size_t uName = strlen(pcName);
  const char *pc = strstr(pcHead, "\r\n");

  pcVal[0] = '\0';
  while ((NULL != pc) && ('\0' != pc[2]))
  {
    const char *pcLine = pc + 2;
    const char *pcEnd = strstr(pcLine, "\r\n");
    size_t uIdx = 0u;

    while ((uIdx < uName) && ('\0' != pcLine[uIdx]) &&
           (dmcmd__mcp_lower((unsigned char)pcLine[uIdx]) ==
            dmcmd__mcp_lower((unsigned char)pcName[uIdx])))
    {
      uIdx++;
    }
    if ((uIdx == uName) && (':' == pcLine[uName]))
    {
      const char *pcV = pcLine + uName + 1u;
      size_t uLen = 0u;
      while ((' ' == *pcV) || ('\t' == *pcV))
      {
        pcV++;
      }
      uLen = (NULL != pcEnd) ? (size_t)(pcEnd - pcV) : strlen(pcV);
      if (uLen >= uValCap)
      {
        uLen = uValCap - 1u;
      }
      memcpy(pcVal, pcV, uLen);
      pcVal[uLen] = '\0';
      return DM_TRUE;
    }
    pc = pcEnd;
  }
  return DM_FALSE;
}

static bool_t dmcmd__mcp_parse_cl(in const char *pcVal, out size_t *puOut)
{
  unsigned long long ullVal = 0ull;
  char *pcEnd = NULL;

  if ((NULL == pcVal) || !isdigit((unsigned char)pcVal[0]))
  {
    return DM_FALSE;
  }
  errno = 0;
  ullVal = strtoull(pcVal, &pcEnd, 10);
  while ((' ' == *pcEnd) || ('\t' == *pcEnd))
  {
    pcEnd++;
  }
  if ((0 != errno) || ('\0' != *pcEnd) || (ullVal > (unsigned long long)DMCMD_MCP_RESP_MAX))
  {
    return DM_FALSE;
  }
  *puOut = (size_t)ullVal;
  return DM_TRUE;
}

static bool_t dmcmd__buf_append(io char **ppcBuf, io size_t *puLen, io size_t *puCap,
                                in const char *pcData, in size_t uAdd, in size_t uMax)
{
  if ((uAdd > uMax) || (*puLen > uMax - uAdd))
  {
    return DM_FALSE;
  }
  if (*puLen + uAdd + 1u > *puCap)
  {
    size_t uCap = (0u != *puCap) ? *puCap : 1024u;
    char *pcNew = NULL;
    while (uCap < *puLen + uAdd + 1u)
    {
      uCap = (uCap > uMax / 2u) ? (uMax + 1u) : (uCap * 2u);
    }
    pcNew = (char *)realloc(*ppcBuf, uCap);
    if (NULL == pcNew)
    {
      return DM_FALSE;
    }
    *ppcBuf = pcNew;
    *puCap = uCap;
  }
  if (uAdd > 0u)
  {
    memcpy(*ppcBuf + *puLen, pcData, uAdd);
    *puLen += uAdd;
  }
  (*ppcBuf)[*puLen] = '\0';
  return DM_TRUE;
}

/* ============================ SSE 与严格 Chunked 解码器 ============================ */

typedef struct dmcmd__sse_state
{
  dmcmd_mcp_ctx_st *pstCtx;
  char *pcLine;
  size_t uLineLen;
  size_t uLineCap;
  char *pcEvent;
  size_t uEventLen;
  size_t uEventCap;
  uint32 u32Emitted;
  bool_t bError;
} dmcmd__sse_state_st;

static void dmcmd__sse_flush_event(io dmcmd__sse_state_st *pstSse)
{
  if (pstSse->uEventLen > 0u)
  {
    dmcmd_mcp_write_json_line(pstSse->pstCtx, pstSse->pcEvent, pstSse->uEventLen);
    pstSse->uEventLen = 0u;
    pstSse->u32Emitted++;
  }
  return;
}

static void dmcmd__sse_on_line(io dmcmd__sse_state_st *pstSse)
{
  char *pcLine = pstSse->pcLine;
  size_t uLen = pstSse->uLineLen;

  pstSse->uLineLen = 0u;
  if ((NULL == pcLine) || (0u == uLen))
  {
    dmcmd__sse_flush_event(pstSse);
    return;
  }
  if ('\r' == pcLine[uLen - 1u])
  {
    uLen--;
  }
  pcLine[uLen] = '\0';
  if (0u == uLen)
  {
    dmcmd__sse_flush_event(pstSse);
    return;
  }
  if (':' == pcLine[0])
  {
    return; /* SSE 注释 / : keepalive，已在读取时续期空闲超时 */
  }
  if ((uLen >= 5u) && (0 == memcmp(pcLine, "data:", 5u)))
  {
    const char *pcVal = pcLine + 5u;
    size_t uValLen = uLen - 5u;
    if ((uValLen > 0u) && (' ' == *pcVal))
    {
      pcVal++;
      uValLen--;
    }
    if (pstSse->uEventLen > 0u)
    {
      if (!dmcmd__buf_append(&pstSse->pcEvent, &pstSse->uEventLen, &pstSse->uEventCap, "\n", 1u,
                             DMCMD_MCP_RESP_MAX))
      {
        pstSse->bError = DM_TRUE;
        return;
      }
    }
    if (!dmcmd__buf_append(&pstSse->pcEvent, &pstSse->uEventLen, &pstSse->uEventCap, pcVal, uValLen,
                           DMCMD_MCP_RESP_MAX))
    {
      pstSse->bError = DM_TRUE;
    }
  }
  return;
}

static void dmcmd__sse_feed(io dmcmd__sse_state_st *pstSse, in const char *pcData, in size_t uLen)
{
  size_t uIdx = 0u;
  for (uIdx = 0u; !pstSse->bError && (uIdx < uLen); uIdx++)
  {
    if ('\n' == pcData[uIdx])
    {
      dmcmd__sse_on_line(pstSse);
    }
    else if (!dmcmd__buf_append(&pstSse->pcLine, &pstSse->uLineLen, &pstSse->uLineCap,
                                &pcData[uIdx], 1u, DMCMD_MCP_RESP_MAX))
    {
      pstSse->bError = DM_TRUE;
    }
  }
  return;
}

typedef struct dmcmd__chunk_dec
{
  int iState; /* 0=size, 1=data, 2=expect_cr_or_lf, 4=expect_lf, 3=done, -1=err */
  size_t uRemain;
  char acSize[32];
  size_t uSizeLen;
  size_t uExtLen;
  bool_t bInExt;
} dmcmd__chunk_dec_st;

static bool_t dmcmd__parse_chunk_hex(in const char *pcLine, out size_t *puOut)
{
  const unsigned char *pu8 = (const unsigned char *)pcLine;
  unsigned long long ullVal = 0ull;
  bool_t bDigit = DM_FALSE;

  while (isxdigit(*pu8))
  {
    unsigned int uiD = (*pu8 <= '9') ? (unsigned int)(*pu8 - '0')
                                     : (unsigned int)(dmcmd__mcp_lower(*pu8) - 'a' + 10);
    bDigit = DM_TRUE;
    if (ullVal > ((unsigned long long)DMCMD_MCP_RESP_MAX - (unsigned long long)uiD) / 16ull)
    {
      return DM_FALSE;
    }
    ullVal = ullVal * 16ull + (unsigned long long)uiD;
    pu8++;
  }
  if (!bDigit)
  {
    return DM_FALSE;
  }
  while ((' ' == *pu8) || ('\t' == *pu8))
  {
    pu8++;
  }
  if (('\0' != *pu8) && (';' != *pu8))
  {
    return DM_FALSE;
  }
  *puOut = (size_t)ullVal;
  return DM_TRUE;
}

static bool_t dmcmd__chunk_feed(io dmcmd__chunk_dec_st *pstDec, in const char *pcIn, in size_t uLen,
                                in void (*pfEmit)(io void *pvUser, in const char *pc, in size_t u),
                                io void *pvUser)
{
  size_t uIdx = 0u;
  while ((uIdx < uLen) && (pstDec->iState >= 0) && (3 != pstDec->iState))
  {
    if (0 == pstDec->iState)
    {
      char c = pcIn[uIdx++];
      if ('\n' == c)
      {
        size_t uSz = 0u;
        if ((pstDec->uSizeLen > 0u) && ('\r' == pstDec->acSize[pstDec->uSizeLen - 1u]))
        {
          pstDec->uSizeLen--;
        }
        pstDec->acSize[pstDec->uSizeLen] = '\0';
        if (!dmcmd__parse_chunk_hex(pstDec->acSize, &uSz))
        {
          pstDec->iState = -1;
          return DM_FALSE;
        }
        pstDec->uSizeLen = 0u;
        pstDec->uExtLen = 0u;
        pstDec->bInExt = DM_FALSE;
        pstDec->uRemain = uSz;
        pstDec->iState = (0u == uSz) ? 3 : 1;
      }
      else if (';' == c)
      {
        pstDec->bInExt = DM_TRUE;
      }
      else if (pstDec->bInExt)
      {
        if (++pstDec->uExtLen > 4096u)
        {
          pstDec->iState = -1;
          return DM_FALSE;
        }
      }
      else
      {
        if (pstDec->uSizeLen + 1u >= sizeof(pstDec->acSize))
        {
          pstDec->iState = -1;
          return DM_FALSE;
        }
        pstDec->acSize[pstDec->uSizeLen++] = c;
      }
    }
    else if (1 == pstDec->iState)
    {
      size_t uTake = (uLen - uIdx < pstDec->uRemain) ? (uLen - uIdx) : pstDec->uRemain;
      pfEmit(pvUser, pcIn + uIdx, uTake);
      uIdx += uTake;
      pstDec->uRemain -= uTake;
      if (0u == pstDec->uRemain)
      {
        pstDec->iState = 2;
      }
    }
    else if (2 == pstDec->iState)
    {
      char c = pcIn[uIdx++];
      if ('\r' == c)
      {
        pstDec->iState = 4;
      }
      else if ('\n' == c)
      {
        pstDec->iState = 0;
      }
      else
      {
        pstDec->iState = -1;
        return DM_FALSE;
      }
    }
    else if (4 == pstDec->iState)
    {
      if ('\n' == pcIn[uIdx++])
      {
        pstDec->iState = 0;
      }
      else
      {
        pstDec->iState = -1;
        return DM_FALSE;
      }
    }
  }
  return (pstDec->iState >= 0) ? DM_TRUE : DM_FALSE;
}

typedef struct dmcmd__body_acc
{
  char *pcBuf;
  size_t uLen;
  size_t uCap;
  bool_t bTooBig;
} dmcmd__body_acc_st;

static void dmcmd__acc_emit(io void *pvUser, in const char *pcData, in size_t uLen)
{
  dmcmd__body_acc_st *pstAcc = (dmcmd__body_acc_st *)pvUser;
  if (!pstAcc->bTooBig && !dmcmd__buf_append(&pstAcc->pcBuf, &pstAcc->uLen, &pstAcc->uCap, pcData,
                                             uLen, DMCMD_MCP_RESP_MAX))
  {
    pstAcc->bTooBig = DM_TRUE;
  }
  return;
}

static void dmcmd__sse_emit(io void *pvUser, in const char *pcData, in size_t uLen)
{
  dmcmd__sse_feed((dmcmd__sse_state_st *)pvUser, pcData, uLen);
  return;
}

/* ============================ 单请求 HTTP/SSE 处理 ============================ */

static void dmcmd__mcp_handle_sse(io dmcmd_mcp_ctx_st *pstCtx, in dmcmd_mcp_sock_t iSock,
                                  in int iStatus, in bool_t bChunked, in bool_t bHasCl,
                                  in size_t uWantCl, in const char *pcInit, in size_t uInitLen,
                                  in int iIdleMs, in const char *pcReq, in size_t uReqLen)
{
  dmcmd__sse_state_st stSse;
  dmcmd__chunk_dec_st stDec;
  char acTmp[4096];
  size_t uGotCl = 0u;

  memset(&stSse, 0, sizeof(stSse));
  memset(&stDec, 0, sizeof(stDec));
  stSse.pstCtx = pstCtx;
  if (uInitLen > 0u)
  {
    size_t uTake = (bHasCl && (uInitLen > uWantCl)) ? uWantCl : uInitLen;
    uGotCl += uTake;
    if (bChunked)
    {
      if (!dmcmd__chunk_feed(&stDec, pcInit, uTake, dmcmd__sse_emit, &stSse))
      {
        stSse.bError = DM_TRUE;
      }
    }
    else
    {
      dmcmd__sse_feed(&stSse, pcInit, uTake);
    }
  }
  while (!stSse.bError && (!bChunked || (3 != stDec.iState)) && (!bHasCl || (uGotCl < uWantCl)))
  {
    int iN = dmcmd_mcp_recv_idle(iSock, acTmp, sizeof(acTmp), iIdleMs);
    if (0 == iN)
    {
      if ((bChunked && (3 != stDec.iState)) || (bHasCl && (uGotCl < uWantCl)))
      {
        stSse.bError = DM_TRUE;
      }
      break;
    }
    if (iN < 0)
    {
      stSse.bError = DM_TRUE;
      break;
    }
    if (bChunked)
    {
      if (!dmcmd__chunk_feed(&stDec, acTmp, (size_t)iN, dmcmd__sse_emit, &stSse))
      {
        stSse.bError = DM_TRUE;
      }
    }
    else
    {
      size_t uTake = (size_t)iN;
      if (bHasCl && (uGotCl + uTake > uWantCl))
      {
        uTake = uWantCl - uGotCl;
      }
      uGotCl += uTake;
      dmcmd__sse_feed(&stSse, acTmp, uTake);
    }
  }
  if (!stSse.bError && (stSse.uLineLen > 0u))
  {
    dmcmd__sse_on_line(&stSse);
  }
  if (!stSse.bError)
  {
    dmcmd__sse_flush_event(&stSse);
    if ((iStatus >= 400) && (0u == stSse.u32Emitted))
    {
      char acMsg[64];
      snprintf(acMsg, sizeof(acMsg), "mcp sse http error %d", iStatus);
      dmcmd_mcp_reply_rpc_err(pstCtx, pcReq, uReqLen, -32000, acMsg);
    }
  }
  else
  {
    dmcmd_mcp_reply_rpc_err(pstCtx, pcReq, uReqLen, -32000, "sse stream error or timeout");
  }
  free(stSse.pcLine);
  free(stSse.pcEvent);
  return;
}

static void dmcmd__mcp_handle_body(io dmcmd_mcp_ctx_st *pstCtx, in dmcmd_mcp_sock_t iSock,
                                   in int iStatus, in bool_t bChunked, in bool_t bHasCl,
                                   in size_t uWantCl, in const char *pcInit, in size_t uInitLen,
                                   in int iIdleMs, in const char *pcReq, in size_t uReqLen)
{
  dmcmd__body_acc_st stAcc;
  dmcmd__chunk_dec_st stDec;
  char acTmp[8192];
  bool_t bIoErr = DM_FALSE;

  memset(&stAcc, 0, sizeof(stAcc));
  memset(&stDec, 0, sizeof(stDec));
  if (uInitLen > 0u)
  {
    size_t uInit = (bHasCl && (uInitLen > uWantCl)) ? uWantCl : uInitLen;
    if (bChunked)
    {
      if (!dmcmd__chunk_feed(&stDec, pcInit, uInit, dmcmd__acc_emit, &stAcc))
      {
        bIoErr = DM_TRUE;
      }
    }
    else
    {
      dmcmd__acc_emit(&stAcc, pcInit, uInit);
    }
  }
  while (!bIoErr && !stAcc.bTooBig)
  {
    int iN = 0;
    if ((bHasCl && (stAcc.uLen >= uWantCl)) || (bChunked && (3 == stDec.iState)))
    {
      break;
    }
    iN = dmcmd_mcp_recv_idle(iSock, acTmp, sizeof(acTmp), iIdleMs);
    if (0 == iN)
    {
      if ((bHasCl && (stAcc.uLen < uWantCl)) || (bChunked && (3 != stDec.iState)))
      {
        bIoErr = DM_TRUE;
      }
      break;
    }
    if (iN < 0)
    {
      bIoErr = DM_TRUE;
      break;
    }
    if (bChunked)
    {
      if (!dmcmd__chunk_feed(&stDec, acTmp, (size_t)iN, dmcmd__acc_emit, &stAcc))
      {
        bIoErr = DM_TRUE;
      }
    }
    else
    {
      size_t uTake = (size_t)iN;
      if (bHasCl && (stAcc.uLen + uTake > uWantCl))
      {
        uTake = uWantCl - stAcc.uLen;
      }
      dmcmd__acc_emit(&stAcc, acTmp, uTake);
    }
  }
  if (stAcc.bTooBig)
  {
    dmcmd_mcp_reply_rpc_err(pstCtx, pcReq, uReqLen, -32000, "mcp response body exceeds 16MB");
  }
  else if (bIoErr)
  {
    dmcmd_mcp_reply_rpc_err(pstCtx, pcReq, uReqLen, -32000, "mcp response io error or timeout");
  }
  else if (202 != iStatus)
  {
    yyjson_doc *pstChk =
        (stAcc.uLen > 0u) ? yyjson_read(stAcc.pcBuf, stAcc.uLen, YYJSON_READ_NOFLAG) : NULL;
    bool_t bValidJson = (NULL != pstChk) ? DM_TRUE : DM_FALSE;
    bool_t bJsonRpc =
        (NULL != yyjson_obj_get(yyjson_doc_get_root(pstChk), "jsonrpc")) ? DM_TRUE : DM_FALSE;
    yyjson_doc_free(pstChk);
    if ((iStatus >= 400) && !bJsonRpc)
    {
      char acMsg[64];
      snprintf(acMsg, sizeof(acMsg), "mcp http error %d", iStatus);
      dmcmd_mcp_reply_rpc_err(pstCtx, pcReq, uReqLen, -32000, acMsg);
    }
    else if ((stAcc.uLen > 0u) && !bValidJson)
    {
      dmcmd_mcp_reply_rpc_err(pstCtx, pcReq, uReqLen, -32000, "mcp response is not valid json");
    }
    else if (stAcc.uLen > 0u)
    {
      dmcmd_mcp_write_json_line(pstCtx, stAcc.pcBuf, stAcc.uLen);
    }
  }
  free(stAcc.pcBuf);
  return;
}

void dmcmd_mcp_handle_one(io dmcmd_mcp_ctx_st *pstCtx, in const char *pcReq, in size_t uReqLen)
{
  dmcmd_mcp_sock_t iSock = DMCMD_MCP_SOCK_BAD;
  char acReqHead[4096];
  char *pcHead = NULL;
  size_t uHeadLen = 0u;
  size_t uHeadCap = 0u;
  size_t uHeadEnd = 0u;
  int iHeadN = 0;
  int iStatus = 0;
  char acVal[256];
  bool_t bSse = DM_FALSE;
  bool_t bChunked = DM_FALSE;
  bool_t bHasCl = DM_FALSE;
  size_t uWantCl = 0u;
  uint32 u32Idle = (0u != pstCtx->u32IdleMs) ? pstCtx->u32IdleMs : DMCMD_DEFAULT_TIMEOUT_MS;
  int iIdleMs = (u32Idle > (uint32)INT_MAX) ? INT_MAX : (int)u32Idle;
  bool_t bIpv6 = (NULL != strchr(pstCtx->acHost, ':')) ? DM_TRUE : DM_FALSE;

  iSock = dmcmd_mcp_connect(pstCtx->acHost, pstCtx->iPort, 3000u);
  if (DMCMD_MCP_SOCK_BAD == iSock)
  {
    dmcmd_mcp_reply_rpc_err(pstCtx, pcReq, uReqLen, -32000, "failed to connect to mcp endpoint");
    return;
  }
  iHeadN =
      snprintf(acReqHead, sizeof(acReqHead),
               "POST %s HTTP/1.1\r\nHost: %s%s%s:%d\r\n"
               "Content-Type: application/json\r\n"
               "Accept: application/json, text/event-stream\r\n"
               "Content-Length: %lu\r\n%s%s%sConnection: close\r\n\r\n",
               pstCtx->acPath, bIpv6 ? "[" : "", pstCtx->acHost, bIpv6 ? "]" : "", pstCtx->iPort,
               (unsigned long)uReqLen, pstCtx->bHasAuth ? "Authorization: " : "",
               pstCtx->bHasAuth ? pstCtx->acAuth : "", pstCtx->bHasAuth ? "\r\n" : "");
  if ((iHeadN <= 0) || ((size_t)iHeadN >= sizeof(acReqHead)) ||
      (0 != dmcmd_mcp_send_all(iSock, acReqHead, (size_t)iHeadN, iIdleMs)) ||
      (0 != dmcmd_mcp_send_all(iSock, pcReq, uReqLen, iIdleMs)))
  {
    dmcmd_mcp_sock_close(iSock);
    dmcmd_mcp_reply_rpc_err(pstCtx, pcReq, uReqLen, -32000, "failed to send mcp request");
    return;
  }
  while (0u == uHeadEnd)
  {
    char acTmp[4096];
    int iN = dmcmd_mcp_recv_idle(iSock, acTmp, sizeof(acTmp), iIdleMs);
    char *pcSep = NULL;
    if (iN <= 0)
    {
      free(pcHead);
      dmcmd_mcp_sock_close(iSock);
      dmcmd_mcp_reply_rpc_err(pstCtx, pcReq, uReqLen, -32000,
                              (-2 == iN) ? "mcp response timeout" : "malformed or truncated http");
      return;
    }
    if (!dmcmd__buf_append(&pcHead, &uHeadLen, &uHeadCap, acTmp, (size_t)iN,
                           DMCMD__MCP_HEAD_MAX + 4096u))
    {
      free(pcHead);
      dmcmd_mcp_sock_close(iSock);
      dmcmd_mcp_reply_rpc_err(pstCtx, pcReq, uReqLen, -32000, "http response header too large");
      return;
    }
    pcSep = strstr(pcHead, "\r\n\r\n");
    if (NULL != pcSep)
    {
      uHeadEnd = (size_t)(pcSep - pcHead) + 4u;
    }
    else if (uHeadLen > DMCMD__MCP_HEAD_MAX)
    {
      free(pcHead);
      dmcmd_mcp_sock_close(iSock);
      dmcmd_mcp_reply_rpc_err(pstCtx, pcReq, uReqLen, -32000, "http response header too large");
      return;
    }
  }
  pcHead[uHeadEnd - 2u] = '\0';
  iStatus = dmcmd__mcp_parse_status(pcHead, uHeadEnd - 2u);
  if (iStatus < 100)
  {
    free(pcHead);
    dmcmd_mcp_sock_close(iSock);
    dmcmd_mcp_reply_rpc_err(pstCtx, pcReq, uReqLen, -32000, "invalid http status line");
    return;
  }
  if (dmcmd__mcp_get_header(pcHead, "Content-Type", acVal, sizeof(acVal)) &&
      dmcmd__mcp_strcasestr(acVal, "text/event-stream"))
  {
    bSse = DM_TRUE;
  }
  if (dmcmd__mcp_get_header(pcHead, "Transfer-Encoding", acVal, sizeof(acVal)) &&
      dmcmd__mcp_strcasestr(acVal, "chunked"))
  {
    bChunked = DM_TRUE;
  }
  if (!bChunked && dmcmd__mcp_get_header(pcHead, "Content-Length", acVal, sizeof(acVal)))
  {
    if (!dmcmd__mcp_parse_cl(acVal, &uWantCl))
    {
      free(pcHead);
      dmcmd_mcp_sock_close(iSock);
      dmcmd_mcp_reply_rpc_err(pstCtx, pcReq, uReqLen, -32000,
                              "invalid or oversized content-length (>16MB)");
      return;
    }
    bHasCl = DM_TRUE;
  }
  if (bSse)
  {
    dmcmd__mcp_handle_sse(pstCtx, iSock, iStatus, bChunked, bHasCl, uWantCl, pcHead + uHeadEnd,
                          uHeadLen - uHeadEnd, iIdleMs, pcReq, uReqLen);
  }
  else
  {
    dmcmd__mcp_handle_body(pstCtx, iSock, iStatus, bChunked, bHasCl, uWantCl, pcHead + uHeadEnd,
                           uHeadLen - uHeadEnd, iIdleMs, pcReq, uReqLen);
  }
  free(pcHead);
  dmcmd_mcp_sock_close(iSock);
  return;
}

#ifdef __cplusplus
}
#endif
