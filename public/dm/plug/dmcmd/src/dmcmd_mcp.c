/**
 ******************************************************************************
 * @file    dmcmd_mcp.c
 * @brief   dmcmd mcp 并发异步中继调度器（DD-02 §3.4，CLI-07，AUD-03）：
 *          主线程仅读 stdin（16MB 硬上限），最多 32 个并发工作线程转发请求，
 *          所有 stdout 行输出经唯一互斥锁串行刷出。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "dmcmd_mcp_int.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
static void dmcmd__mtx_init(out dmcmd_mcp_mutex_t *p)
{
  InitializeCriticalSection(p);
}
static void dmcmd__mtx_fini(io dmcmd_mcp_mutex_t *p)
{
  DeleteCriticalSection(p);
}
static void dmcmd__mtx_lock(io dmcmd_mcp_mutex_t *p)
{
  EnterCriticalSection(p);
}
static void dmcmd__mtx_unlock(io dmcmd_mcp_mutex_t *p)
{
  LeaveCriticalSection(p);
}
static void dmcmd__cond_init(out dmcmd_mcp_cond_t *p)
{
  InitializeConditionVariable(p);
}
static void dmcmd__cond_fini(io dmcmd_mcp_cond_t *p)
{
  (void)p;
}
static void dmcmd__cond_wait(io dmcmd_mcp_cond_t *c, io dmcmd_mcp_mutex_t *m)
{
  (void)SleepConditionVariableCS(c, m, INFINITE);
}
static void dmcmd__cond_signal(io dmcmd_mcp_cond_t *c)
{
  WakeConditionVariable(c);
}
#else
static void dmcmd__mtx_init(out dmcmd_mcp_mutex_t *p)
{
  (void)pthread_mutex_init(p, NULL);
}
static void dmcmd__mtx_fini(io dmcmd_mcp_mutex_t *p)
{
  (void)pthread_mutex_destroy(p);
}
static void dmcmd__mtx_lock(io dmcmd_mcp_mutex_t *p)
{
  (void)pthread_mutex_lock(p);
}
static void dmcmd__mtx_unlock(io dmcmd_mcp_mutex_t *p)
{
  (void)pthread_mutex_unlock(p);
}
static void dmcmd__cond_init(out dmcmd_mcp_cond_t *p)
{
  (void)pthread_cond_init(p, NULL);
}
static void dmcmd__cond_fini(io dmcmd_mcp_cond_t *p)
{
  (void)pthread_cond_destroy(p);
}
static void dmcmd__cond_wait(io dmcmd_mcp_cond_t *c, io dmcmd_mcp_mutex_t *m)
{
  (void)pthread_cond_wait(c, m);
}
static void dmcmd__cond_signal(io dmcmd_mcp_cond_t *c)
{
  (void)pthread_cond_signal(c);
}
#endif

static void dmcmd__zero_secret(out void *pvBuf, in size_t uLen)
{
  volatile uint8 *pu8 = (volatile uint8 *)pvBuf;
  size_t uIdx = 0u;
  for (uIdx = 0u; uIdx < uLen; uIdx++)
  {
    pu8[uIdx] = 0u;
  }
  return;
}

static bool_t dmcmd__has_ctl_or_space(in const char *pcStr)
{
  const unsigned char *pu8 = (const unsigned char *)pcStr;
  if (NULL == pu8)
  {
    return DM_FALSE;
  }
  while ('\0' != *pu8)
  {
    if ((*pu8 <= 0x20u) || (0x7Fu == *pu8))
    {
      return DM_TRUE;
    }
    pu8++;
  }
  return DM_FALSE;
}

static bool_t dmcmd__has_ctl_char(in const char *pcStr)
{
  const unsigned char *pu8 = (const unsigned char *)pcStr;
  if (NULL == pu8)
  {
    return DM_FALSE;
  }
  while ('\0' != *pu8)
  {
    if ((*pu8 < 0x20u) || (0x7Fu == *pu8))
    {
      return DM_TRUE;
    }
    pu8++;
  }
  return DM_FALSE;
}

static bool_t dmcmd__is_blank_line(in const char *pcLine, in size_t uLen)
{
  size_t uIdx = 0u;
  for (uIdx = 0u; uIdx < uLen; uIdx++)
  {
    if ((' ' != pcLine[uIdx]) && ('\t' != pcLine[uIdx]))
    {
      return DM_FALSE;
    }
  }
  return DM_TRUE;
}

void dmcmd_mcp_write_json_line(io dmcmd_mcp_ctx_st *pstCtx, in const char *pcData, in size_t uLen)
{
  yyjson_doc *pstDoc = NULL;
  char *pcCompact = NULL;

  if ((NULL == pcData) || (0u == uLen))
  {
    return;
  }
  pstDoc = yyjson_read(pcData, uLen, YYJSON_READ_NOFLAG);
  if (NULL != pstDoc)
  {
    pcCompact = yyjson_write(pstDoc, YYJSON_WRITE_NOFLAG, NULL);
    yyjson_doc_free(pstDoc);
  }
  dmcmd__mtx_lock(&pstCtx->stOutMtx);
  if (NULL != pcCompact)
  {
    fprintf(stdout, "%s\n", pcCompact);
  }
  else
  {
    size_t uIdx = 0u;
    for (uIdx = 0u; uIdx < uLen; uIdx++)
    {
      char c = pcData[uIdx];
      fputc((('\n' == c) || ('\r' == c)) ? ' ' : c, stdout);
    }
    fputc('\n', stdout);
  }
  fflush(stdout);
  dmcmd__mtx_unlock(&pstCtx->stOutMtx);
  free(pcCompact);
  return;
}

void dmcmd_mcp_reply_rpc_err(io dmcmd_mcp_ctx_st *pstCtx, in const char *pcReqLine,
                             in size_t uReqLen, in int iCode, in const char *pcMsg)
{
  yyjson_doc *pstReq =
      (NULL != pcReqLine) ? yyjson_read(pcReqLine, uReqLen, YYJSON_READ_NOFLAG) : NULL;
  yyjson_val *pstReqRoot = yyjson_doc_get_root(pstReq);
  yyjson_val *pstId = yyjson_is_obj(pstReqRoot) ? yyjson_obj_get(pstReqRoot, "id") : NULL;
  yyjson_mut_doc *pstOut = NULL;
  yyjson_mut_val *pstRoot = NULL;
  yyjson_mut_val *pstErr = NULL;
  char *pcText = NULL;

  if (yyjson_is_obj(pstReqRoot) && (NULL == pstId))
  {
    yyjson_doc_free(pstReq);
    return; /* JSON-RPC 通知对象不回错误应答 */
  }
  pstOut = yyjson_mut_doc_new(NULL);
  pstRoot = (NULL != pstOut) ? yyjson_mut_obj(pstOut) : NULL;
  if (NULL != pstRoot)
  {
    yyjson_mut_doc_set_root(pstOut, pstRoot);
    (void)yyjson_mut_obj_add_str(pstOut, pstRoot, "jsonrpc", "2.0");
    if (NULL != pstId)
    {
      (void)yyjson_mut_obj_add_val(pstOut, pstRoot, "id", yyjson_val_mut_copy(pstOut, pstId));
    }
    else
    {
      (void)yyjson_mut_obj_add_null(pstOut, pstRoot, "id");
    }
    pstErr = yyjson_mut_obj_add_obj(pstOut, pstRoot, "error");
    if (NULL != pstErr)
    {
      (void)yyjson_mut_obj_add_int(pstOut, pstErr, "code", iCode);
      (void)yyjson_mut_obj_add_str(pstOut, pstErr, "message", pcMsg);
    }
    pcText = yyjson_mut_write(pstOut, YYJSON_WRITE_NOFLAG, NULL);
  }
  yyjson_mut_doc_free(pstOut);
  yyjson_doc_free(pstReq);
  if (NULL != pcText)
  {
    dmcmd_mcp_write_json_line(pstCtx, pcText, strlen(pcText));
    free(pcText);
  }
  return;
}

#ifdef _WIN32
static DWORD WINAPI dmcmd__mcp_worker(LPVOID pvArg)
#else
static void *dmcmd__mcp_worker(void *pvArg)
#endif
{
  dmcmd_mcp_slot_st *pstSlot = (dmcmd_mcp_slot_st *)pvArg;
  dmcmd_mcp_ctx_st *pstCtx = pstSlot->pstCtx;

  dmcmd_mcp_handle_one(pstCtx, pstSlot->pcReqLine, pstSlot->uReqLen);
  free(pstSlot->pcReqLine);
  pstSlot->pcReqLine = NULL;
  pstSlot->uReqLen = 0u;

  dmcmd__mtx_lock(&pstCtx->stPoolMtx);
  pstSlot->bDone = DM_TRUE;
  pstCtx->u32Inflight--;
  dmcmd__cond_signal(&pstCtx->stPoolCond);
  dmcmd__mtx_unlock(&pstCtx->stPoolMtx);
#ifdef _WIN32
  return 0;
#else
  return NULL;
#endif
}

static void dmcmd__mcp_reap_done(io dmcmd_mcp_ctx_st *pstCtx)
{
  uint32 u32Idx = 0u;
  for (u32Idx = 0u; u32Idx < DMCMD_MCP_INFLIGHT; u32Idx++)
  {
    dmcmd_mcp_slot_st *pstSlot = &pstCtx->astSlots[u32Idx];
    if (pstSlot->bActive && pstSlot->bDone)
    {
      dmcmd_mcp_thread_t stThr = pstSlot->stThr;
      pstSlot->bActive = DM_FALSE;
      pstSlot->bDone = DM_FALSE;
      dmcmd__mtx_unlock(&pstCtx->stPoolMtx);
#ifdef _WIN32
      (void)WaitForSingleObject(stThr, INFINITE);
      (void)CloseHandle(stThr);
#else
      (void)pthread_join(stThr, NULL);
#endif
      dmcmd__mtx_lock(&pstCtx->stPoolMtx);
    }
  }
  return;
}

static int dmcmd__mcp_spawn(io dmcmd_mcp_ctx_st *pstCtx, io char *pcLine, in size_t uLen)
{
  uint32 u32Idx = 0u;
  dmcmd_mcp_slot_st *pstSlot = NULL;

  dmcmd__mtx_lock(&pstCtx->stPoolMtx);
  for (;;)
  {
    dmcmd__mcp_reap_done(pstCtx);
    for (u32Idx = 0u; u32Idx < DMCMD_MCP_INFLIGHT; u32Idx++)
    {
      if (!pstCtx->astSlots[u32Idx].bActive)
      {
        pstSlot = &pstCtx->astSlots[u32Idx];
        break;
      }
    }
    if (NULL != pstSlot)
    {
      break;
    }
    dmcmd__cond_wait(&pstCtx->stPoolCond, &pstCtx->stPoolMtx);
  }
  pstSlot->pstCtx = pstCtx;
  pstSlot->pcReqLine = pcLine;
  pstSlot->uReqLen = uLen;
  pstSlot->bActive = DM_TRUE;
  pstSlot->bDone = DM_FALSE;
  pstCtx->u32Inflight++;
#ifdef _WIN32
  pstSlot->stThr = CreateThread(NULL, 0u, dmcmd__mcp_worker, pstSlot, 0u, NULL);
  if (NULL == pstSlot->stThr)
  {
    pstSlot->bActive = DM_FALSE;
    pstSlot->pcReqLine = NULL;
    pstCtx->u32Inflight--;
    dmcmd__mtx_unlock(&pstCtx->stPoolMtx);
    return -1;
  }
#else
  if (0 != pthread_create(&pstSlot->stThr, NULL, dmcmd__mcp_worker, pstSlot))
  {
    pstSlot->bActive = DM_FALSE;
    pstSlot->pcReqLine = NULL;
    pstCtx->u32Inflight--;
    dmcmd__mtx_unlock(&pstCtx->stPoolMtx);
    return -1;
  }
#endif
  dmcmd__mtx_unlock(&pstCtx->stPoolMtx);
  return 0;
}

static bool_t dmcmd__mcp_read_line(out char **ppcLine, out size_t *puLen, out bool_t *pbTooBig)
{
  size_t uCap = 4096u;
  size_t uLen = 0u;
  char *pcBuf = (char *)malloc(uCap);
  int iCh = 0;

  *ppcLine = NULL;
  *puLen = 0u;
  *pbTooBig = DM_FALSE;
  if (NULL == pcBuf)
  {
    return DM_FALSE;
  }
  while (EOF != (iCh = fgetc(stdin)))
  {
    if ('\n' == iCh)
    {
      break;
    }
    if (uLen >= DMCMD_MCP_LINE_MAX)
    {
      /* 精确 16MB 后紧跟 \r\n 或 \r+EOF 允许通过 */
      if ('\r' == iCh)
      {
        int iNext = fgetc(stdin);
        if (('\n' == iNext) || (EOF == iNext))
        {
          break;
        }
        iCh = iNext;
      }
      *pbTooBig = DM_TRUE;
      while ((EOF != iCh) && ('\n' != iCh))
      {
        iCh = fgetc(stdin);
      }
      free(pcBuf);
      return DM_TRUE;
    }
    if (uLen + 2u > uCap)
    {
      size_t uNext = (uCap > DMCMD_MCP_LINE_MAX / 2u) ? (DMCMD_MCP_LINE_MAX + 2u) : (uCap * 2u);
      char *pcNew = (char *)realloc(pcBuf, uNext);
      if (NULL == pcNew)
      {
        free(pcBuf);
        return DM_FALSE;
      }
      pcBuf = pcNew;
      uCap = uNext;
    }
    pcBuf[uLen++] = (char)iCh;
  }
  if ((EOF == iCh) && (0u == uLen))
  {
    free(pcBuf);
    return DM_FALSE;
  }
  if ((uLen > 0u) && ('\r' == pcBuf[uLen - 1u]))
  {
    uLen--;
  }
  pcBuf[uLen] = '\0';
  *ppcLine = pcBuf;
  *puLen = uLen;
  return DM_TRUE;
}

static bool_t dmcmd__mcp_parse_url(in const char *pcUrl, out char *pcHost, in size_t uHostCap,
                                   out int *piPort, out char *pcPath, in size_t uPathCap)
{
  const char *pc = pcUrl;
  const char *pcSlash = NULL;
  const char *pcColon = NULL;
  size_t uAuthLen = 0u;
  size_t uHostLen = 0u;
  const char *pcPathSrc = NULL;
  size_t uPathLen = 0u;

  if ((NULL == pcUrl) || (0 != strncmp(pcUrl, "http://", 7u)) || dmcmd__has_ctl_or_space(pcUrl))
  {
    return DM_FALSE;
  }
  pc += 7u;
  pcSlash = strchr(pc, '/');
  uAuthLen = (NULL != pcSlash) ? (size_t)(pcSlash - pc) : strlen(pc);
  if ('[' == *pc)
  {
    const char *pcClose = (const char *)memchr(pc, ']', uAuthLen);
    if (NULL == pcClose)
    {
      return DM_FALSE;
    }
    uHostLen = (size_t)(pcClose - (pc + 1));
    if (pcClose + 1 < pc + uAuthLen)
    {
      if (':' != pcClose[1])
      {
        return DM_FALSE;
      }
      pcColon = pcClose + 1;
    }
    pc++;
    uAuthLen--;
  }
  else
  {
    pcColon = (const char *)memchr(pc, ':', uAuthLen);
    uHostLen = (NULL != pcColon) ? (size_t)(pcColon - pc) : uAuthLen;
  }
  if (NULL != pcColon)
  {
    const char *pcP = pcColon + 1;
    const char *pcPEnd = pc + uAuthLen;
    unsigned long ulPort = 0ul;
    if (pcP >= pcPEnd)
    {
      return DM_FALSE;
    }
    while (pcP < pcPEnd)
    {
      if (!isdigit((unsigned char)*pcP))
      {
        return DM_FALSE;
      }
      ulPort = ulPort * 10ul + (unsigned long)(*pcP - '0');
      if (ulPort > 65535ul)
      {
        return DM_FALSE;
      }
      pcP++;
    }
    *piPort = (int)ulPort;
  }
  else
  {
    *piPort = 80;
  }
  pcPathSrc = (NULL != pcSlash) ? pcSlash : "/mcp";
  uPathLen = strlen(pcPathSrc);
  if ((0u == uHostLen) || (uHostLen >= uHostCap) || (*piPort <= 0) || (uPathLen >= uPathCap))
  {
    return DM_FALSE;
  }
  memcpy(pcHost, pc, uHostLen);
  pcHost[uHostLen] = '\0';
  memcpy(pcPath, pcPathSrc, uPathLen + 1u);
  return DM_TRUE;
}

int dmcmd_run_mcp(in const dmcmd_opt_st *pstOpt)
{
  dmcmd_mcp_ctx_st stCtx;
  const char *pcUrl = getenv("DMCMD_MCP_URL");
  const char *pcTok = getenv("DMCMD_MCP_TOKEN");
  char *pcLine = NULL;
  size_t uLineLen = 0u;
  bool_t bTooBig = DM_FALSE;

  memset(&stCtx, 0, sizeof(stCtx));
#ifdef _WIN32
  {
    WSADATA stWsa;
    if (0 != WSAStartup(MAKEWORD(2, 2), &stWsa))
    {
      fprintf(stderr, "dmcmd: winsock init failed\n");
      return DMCMD_EXIT_ERR;
    }
  }
#endif
  if ((NULL != pcUrl) && ('\0' != pcUrl[0]))
  {
    if (!dmcmd__mcp_parse_url(pcUrl, stCtx.acHost, sizeof(stCtx.acHost), &stCtx.iPort, stCtx.acPath,
                              sizeof(stCtx.acPath)))
    {
      fprintf(stderr, "dmcmd: invalid DMCMD_MCP_URL '%s'\n", pcUrl);
      return DMCMD_EXIT_USAGE;
    }
  }
  else if ((NULL != pstOpt->pcTarget) && (0 == strncmp(pstOpt->pcTarget, "http://", 7u)))
  {
    if (!dmcmd__mcp_parse_url(pstOpt->pcTarget, stCtx.acHost, sizeof(stCtx.acHost), &stCtx.iPort,
                              stCtx.acPath, sizeof(stCtx.acPath)))
    {
      fprintf(stderr, "dmcmd: invalid --target '%s'\n", pstOpt->pcTarget);
      return DMCMD_EXIT_USAGE;
    }
  }
  else
  {
    stCtx.iPort = dmcmd_read_http_port(pstOpt->pcDataDir);
    if (stCtx.iPort <= 0)
    {
      fprintf(stderr, "dmcmd: core is not running (no run file in %s)\n",
              (NULL != pstOpt->pcDataDir) ? pstOpt->pcDataDir : ".");
      return DMCMD_EXIT_ERR;
    }
    snprintf(stCtx.acHost, sizeof(stCtx.acHost), "127.0.0.1");
    snprintf(stCtx.acPath, sizeof(stCtx.acPath), "/mcp");
  }
  if ((NULL == pcTok) || ('\0' == pcTok[0]))
  {
    pcTok = pstOpt->pcToken;
  }
  if ((NULL != pcTok) && ('\0' != pcTok[0]))
  {
    if (dmcmd__has_ctl_char(pcTok) || (strlen(pcTok) + 8u > sizeof(stCtx.acAuth)))
    {
      fprintf(stderr, "dmcmd: invalid or oversized mcp token\n");
      return DMCMD_EXIT_USAGE;
    }
    snprintf(stCtx.acAuth, sizeof(stCtx.acAuth), "Bearer %s", pcTok);
    stCtx.bHasAuth = DM_TRUE;
  }
  stCtx.u32IdleMs = (0u != pstOpt->u32TimeoutMs) ? pstOpt->u32TimeoutMs : DMCMD_DEFAULT_TIMEOUT_MS;

  dmcmd__mtx_init(&stCtx.stOutMtx);
  dmcmd__mtx_init(&stCtx.stPoolMtx);
  dmcmd__cond_init(&stCtx.stPoolCond);

  while (dmcmd__mcp_read_line(&pcLine, &uLineLen, &bTooBig))
  {
    if (bTooBig)
    {
      dmcmd_mcp_reply_rpc_err(&stCtx, NULL, 0u, -32600, "request line exceeds 16MB limit");
      continue;
    }
    if (dmcmd__is_blank_line(pcLine, uLineLen))
    {
      free(pcLine);
      continue;
    }
    if (0 != dmcmd__mcp_spawn(&stCtx, pcLine, uLineLen))
    {
      dmcmd_mcp_reply_rpc_err(&stCtx, pcLine, uLineLen, -32000, "failed to spawn mcp worker");
      free(pcLine);
    }
  }

  dmcmd__mtx_lock(&stCtx.stPoolMtx);
  while (stCtx.u32Inflight > 0u)
  {
    dmcmd__mcp_reap_done(&stCtx);
    if (stCtx.u32Inflight > 0u)
    {
      dmcmd__cond_wait(&stCtx.stPoolCond, &stCtx.stPoolMtx);
    }
  }
  dmcmd__mcp_reap_done(&stCtx);
  dmcmd__mtx_unlock(&stCtx.stPoolMtx);

  dmcmd__cond_fini(&stCtx.stPoolCond);
  dmcmd__mtx_fini(&stCtx.stPoolMtx);
  dmcmd__mtx_fini(&stCtx.stOutMtx);
  dmcmd__zero_secret(stCtx.acAuth, sizeof(stCtx.acAuth));
  return DMCMD_EXIT_OK;
}

#ifdef __cplusplus
}
#endif
