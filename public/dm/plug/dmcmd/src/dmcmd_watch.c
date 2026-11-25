/**
 ******************************************************************************
 * @file    dmcmd_watch.c
 * @brief   dmcmd watch 子命令实现（DD-02 §3.3，CLI-06）：
 *          支持多 topic（逗号分隔）、--since <seq> 断点续传、--count N 与 --timeout MS。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "dmcmd_internal.h"

#include <dm/frame.h>

#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static volatile sig_atomic_t g_iWatchStop = 0;

#ifndef _WIN32
static void dmcmd__on_stop(int iSig)
{
  (void)iSig;
  g_iWatchStop = 1;
  return;
}
#else
static BOOL WINAPI dmcmd__on_ctrl(DWORD dwType)
{
  (void)dwType;
  g_iWatchStop = 1;
  return TRUE;
}
#endif

static void dmcmd__catch_stop(void)
{
  g_iWatchStop = 0;
#ifdef _WIN32
  (void)SetConsoleCtrlHandler(dmcmd__on_ctrl, TRUE);
#else
  struct sigaction stAct;
  memset(&stAct, 0, sizeof(stAct));
  stAct.sa_handler = dmcmd__on_stop;
  (void)sigemptyset(&stAct.sa_mask);
  (void)sigaction(SIGINT, &stAct, NULL);
  (void)sigaction(SIGTERM, &stAct, NULL);
#endif
  return;
}

typedef struct dmcmd__watch_ctx
{
  uint32 u32WantCount;
  uint32 u32GotCount;
  bool_t bJson;
} dmcmd__watch_ctx_st;

static void dmcmd__watch_on_push(dm_in void *pvUser, dm_in const dmc_push_st *pstPush)
{
  dmcmd__watch_ctx_st *pstCtx = (dmcmd__watch_ctx_st *)pvUser;
  if ((0u != pstCtx->u32WantCount) && (pstCtx->u32GotCount >= pstCtx->u32WantCount))
  {
    return;
  }
  if (pstCtx->bJson)
  {
    yyjson_mut_doc *pstOut = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *pstRoot = (NULL != pstOut) ? yyjson_mut_obj(pstOut) : NULL;
    yyjson_doc *pstVal = (pstPush->u32Len > 0u)
                             ? yyjson_read((const char *)pstPush->pu8Val, pstPush->u32Len, 0)
                             : NULL;
    char *pcText = NULL;
    if (NULL != pstRoot)
    {
      yyjson_mut_doc_set_root(pstOut, pstRoot);
      (void)yyjson_mut_obj_add_uint(pstOut, pstRoot, "seq", pstPush->u64Seq);
      (void)yyjson_mut_obj_add_uint(pstOut, pstRoot, "topic", pstPush->u16Topic);
      (void)yyjson_mut_obj_add_uint(pstOut, pstRoot, "key", pstPush->u64Key);
      (void)yyjson_mut_obj_add_bool(pstOut, pstRoot, "stream",
                                    0u != (pstPush->u8Flag & DM_FRAME_F_STREAM));
      if (NULL != pstVal)
      {
        (void)yyjson_mut_obj_add_val(pstOut, pstRoot, "val",
                                     yyjson_val_mut_copy(pstOut, yyjson_doc_get_root(pstVal)));
      }
      else if ((pstPush->u32Len > 0u) && (NULL != pstPush->pu8Val))
      {
        (void)yyjson_mut_obj_add_strn(pstOut, pstRoot, "val", (const char *)pstPush->pu8Val,
                                      pstPush->u32Len);
      }
      else
      {
        (void)yyjson_mut_obj_add_null(pstOut, pstRoot, "val");
      }
      pcText = yyjson_mut_write(pstOut, YYJSON_WRITE_NOFLAG, NULL);
    }
    if (NULL != pcText)
    {
      fprintf(stdout, "%s\n", pcText);
      free(pcText);
    }
    yyjson_doc_free(pstVal);
    yyjson_mut_doc_free(pstOut);
  }
  else
  {
    printf("seq=%llu topic=%u key=%llu ", (unsigned long long)pstPush->u64Seq,
           (unsigned)pstPush->u16Topic, (unsigned long long)pstPush->u64Key);
    if (pstPush->u32Len > 0u)
    {
      fwrite(pstPush->pu8Val, 1u, pstPush->u32Len, stdout);
    }
    fputc('\n', stdout);
  }
  fflush(stdout);
  pstCtx->u32GotCount++;
  return;
}

int dmcmd_cmd_watch(dm_io dmcmd_opt_st *pstOpt, dm_in int iArgc, dm_in char **ppcArgv)
{
  const char *apcTopics[33];
  char acTopicBuf[512];
  uint uiTopicN = 0u;
  uint64 u64Count = 0u;
  uint64 u64Timeout = pstOpt->bHasTimeout ? (uint64)pstOpt->u32TimeoutMs : 0u;
  uint64 u64Since = 0u;
  bool_t bHasSince = DM_FALSE;
  bool_t bHasTimeout = pstOpt->bHasTimeout;
  bool_t bHasTopics = DM_FALSE;
  int iIdx = 0;
  dmc_client_opt_st stCliOpt;
  dmcmd__watch_ctx_st stCtx;
  dmc_client_st *pstCli = NULL;
  int iErr = DMC_OK;
  uint64 u64End = 0u;

  memset(apcTopics, 0, sizeof(apcTopics));
  acTopicBuf[0] = '\0';
  for (iIdx = 0; iIdx < iArgc; iIdx++)
  {
    if (0 == strcmp(ppcArgv[iIdx], "--count"))
    {
      if ((iIdx + 1 >= iArgc) || !dmcmd_parse_u64(ppcArgv[++iIdx], &u64Count) ||
          (u64Count > (uint64)UINT32_MAX))
      {
        dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "invalid --count", NULL);
        return DMCMD_EXIT_USAGE;
      }
    }
    else if (0 == strcmp(ppcArgv[iIdx], "--timeout"))
    {
      if ((iIdx + 1 >= iArgc) || !dmcmd_parse_u64(ppcArgv[++iIdx], &u64Timeout) ||
          (0u == u64Timeout) || (u64Timeout > 3600000u))
      {
        dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "invalid --timeout", NULL);
        return DMCMD_EXIT_USAGE;
      }
      bHasTimeout = DM_TRUE;
    }
    else if (0 == strcmp(ppcArgv[iIdx], "--since"))
    {
      if ((iIdx + 1 >= iArgc) || !dmcmd_parse_u64(ppcArgv[++iIdx], &u64Since))
      {
        dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "invalid --since", NULL);
        return DMCMD_EXIT_USAGE;
      }
      bHasSince = DM_TRUE;
    }
    else if (0 == strcmp(ppcArgv[iIdx], "--json"))
    {
      pstOpt->bJson = DM_TRUE;
    }
    else if (('-' != ppcArgv[iIdx][0]) && !bHasTopics)
    {
      snprintf(acTopicBuf, sizeof(acTopicBuf), "%s", ppcArgv[iIdx]);
      bHasTopics = DM_TRUE;
    }
    else
    {
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "unknown watch option", ppcArgv[iIdx]);
      return DMCMD_EXIT_USAGE;
    }
  }
  if ('\0' != acTopicBuf[0])
  {
    char *pc = acTopicBuf;
    while (('\0' != *pc) && (uiTopicN < 32u))
    {
      char *pcComma = strchr(pc, ',');
      if (NULL != pcComma)
      {
        *pcComma = '\0';
      }
      if ('\0' != *pc)
      {
        apcTopics[uiTopicN++] = pc;
      }
      if (NULL == pcComma)
      {
        break;
      }
      pc = pcComma + 1;
    }
  }
  apcTopics[uiTopicN] = NULL;

  dmcmd__catch_stop();
  memset(&stCtx, 0, sizeof(stCtx));
  stCtx.u32WantCount = (uint32)u64Count;
  stCtx.bJson = pstOpt->bJson;

  memset(&stCliOpt, 0, sizeof(stCliOpt));
  stCliOpt.pcTarget = pstOpt->pcTarget;
  stCliOpt.pcDataDir = pstOpt->pcDataDir;
  stCliOpt.pcToken = pstOpt->pcToken;
  stCliOpt.pcRole = pstOpt->pcRole;
  stCliOpt.u32ConnectTimeoutMs = pstOpt->bHasTimeout ? pstOpt->u32TimeoutMs : 5000u;
  stCliOpt.u32DefaultTimeoutMs = pstOpt->u32TimeoutMs;
  stCliOpt.bAutoReconnect = DM_TRUE;
  stCliOpt.bWantPush = DM_TRUE;
  stCliOpt.bHasSinceSeq = bHasSince;
  stCliOpt.u64SinceSeq = u64Since;
  stCliOpt.ppcPushTopics = (uiTopicN > 0u) ? apcTopics : NULL;
  stCliOpt.pfOnPush = dmcmd__watch_on_push;
  stCliOpt.pvUser = &stCtx;

  pstCli = dmc_client_open(&stCliOpt, &iErr);
  if (NULL == pstCli)
  {
    dmcmd_emit_error(pstOpt->bJson, iErr, dmc_err_str(iErr), "watch failed to connect");
    return DMCMD_EXIT_ERR;
  }
  if (bHasTimeout)
  {
    u64End = dmc_now_ms() + u64Timeout;
  }
  while (0 == g_iWatchStop)
  {
    uint32 u32Wait = 100u;
    if ((0u != stCtx.u32WantCount) && (stCtx.u32GotCount >= stCtx.u32WantCount))
    {
      dmc_client_close(pstCli);
      return DMCMD_EXIT_OK;
    }
    if (bHasTimeout)
    {
      uint64 u64Now = dmc_now_ms();
      if (u64Now >= u64End)
      {
        dmc_client_close(pstCli);
        dmcmd_emit_error(pstOpt->bJson, DMC_ERR_TIMEOUT, "watch timed out", NULL);
        return DMCMD_EXIT_ERR;
      }
      if ((u64End - u64Now) < u32Wait)
      {
        u32Wait = (uint32)(u64End - u64Now);
      }
    }
    (void)dmc_client_poll(pstCli, u32Wait);
  }
  dmc_client_close(pstCli);
  return DMCMD_EXIT_OK;
}

#ifdef __cplusplus
}
#endif
