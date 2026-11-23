/**
 ******************************************************************************
 * @file    dmcmd_main.c
 * @author  MDM
 * @version 0.0.1.0
 * @brief   dmcmd 主入口（DD-02 §3，CLI-01 ~ CLI-08，AUD-14）：
 *          全局参数解析、超 64 参数明确退出码 2、交互模式与子命令分发。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "dmcmd_internal.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define dmcmd__isatty(fd) _isatty(fd)
#else
#include <unistd.h>
#define dmcmd__isatty(fd) isatty(fd)
#endif

/* 返回参数数，超过 iMax 时返回 -1（不再静默截断，CLI-08） */
static int dmcmd__split(io char *pcLine, out char **ppcArgv, in int iMax)
{
  int iArgc = 0;
  char *pcIn = pcLine;

  for (;;)
  {
    char *pcOut = NULL;
    while ((' ' == *pcIn) || ('\t' == *pcIn))
    {
      pcIn++;
    }
    if ('\0' == *pcIn)
    {
      break;
    }
    if (iArgc >= iMax)
    {
      return -1;
    }
    ppcArgv[iArgc++] = pcIn;
    pcOut = pcIn;
    while (('\0' != *pcIn) && (' ' != *pcIn) && ('\t' != *pcIn))
    {
      if ('"' == *pcIn)
      {
        pcIn++;
        while (('\0' != *pcIn) && ('"' != *pcIn))
        {
          if (('\\' == *pcIn) && ('"' == pcIn[1]))
          {
            pcIn++;
          }
          *pcOut++ = *pcIn++;
        }
        if ('"' == *pcIn)
        {
          pcIn++;
        }
        continue;
      }
      *pcOut++ = *pcIn++;
    }
    if ('\0' != *pcIn)
    {
      pcIn++;
    }
    *pcOut = '\0';
  }
  return iArgc;
}

static int dmcmd__interactive(io dmc_client_st *pstCli, in const dmcmd_opt_st *pstOpt)
{
  char *apcArgv[DMCMD_MAX_ARGS];
  char acLine[4096];
  bool_t bTty = dmcmd__isatty(0) ? DM_TRUE : DM_FALSE;

  while (1)
  {
    int iArgc = 0;
    size_t uLen = 0u;
    if (bTty)
    {
      fputs("dm> ", stderr);
      fflush(stderr);
    }
    if (NULL == fgets(acLine, (int)sizeof(acLine), stdin))
    {
      break;
    }
    uLen = strlen(acLine);
    if ((uLen + 1u == sizeof(acLine)) && ('\n' != acLine[uLen - 1u]))
    {
      int iCh = 0;
      while ((EOF != (iCh = fgetc(stdin))) && ('\n' != iCh))
      {
      }
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "command line too long",
                       "maximum 4095 bytes in interactive mode");
      continue;
    }
    while ((uLen > 0u) && (('\n' == acLine[uLen - 1u]) || ('\r' == acLine[uLen - 1u])))
    {
      acLine[--uLen] = '\0';
    }
    iArgc = dmcmd__split(acLine, apcArgv, DMCMD_MAX_ARGS);
    if (iArgc < 0)
    {
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "too many arguments",
                       "maximum 64 arguments exceeded");
      continue;
    }
    if (0 == iArgc)
    {
      continue;
    }
    if ((0 == strcmp(apcArgv[0], "quit")) || (0 == strcmp(apcArgv[0], "exit")))
    {
      break;
    }
    (void)dmcmd_cmd_run(pstCli, pstOpt, iArgc, apcArgv);
  }
  return DMCMD_EXIT_OK;
}

static int dmcmd__parse_cli(io dmcmd_opt_st *pstOpt, in int argc, in char **argv,
                            out char **ppcRest, out int *piRest)
{
  int iIdx = 1;
  int iRest = 0;

  /* 预扫 --json / --raw，确保任何早退错误（含参数超 64 或非法全局项）都吐合法 JSON 错误信封 */
  for (iIdx = 1; iIdx < argc; iIdx++)
  {
    if (0 == strcmp(argv[iIdx], "--json"))
    {
      pstOpt->bJson = DM_TRUE;
    }
    else if (0 == strcmp(argv[iIdx], "--raw"))
    {
      pstOpt->bRaw = DM_TRUE;
    }
  }
  iIdx = 1;

  while (iIdx < argc)
  {
    const char *pcArg = argv[iIdx];
    bool_t bGlobal = (0 == iRest) ? DM_TRUE : DM_FALSE;

    if (bGlobal && (0 == strcmp(pcArg, "--target")))
    {
      if (iIdx + 1 >= argc)
      {
        dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "--target needs a value", NULL);
        return DMCMD_EXIT_USAGE;
      }
      pstOpt->pcTarget = argv[++iIdx];
      iIdx++;
    }
    else if (bGlobal && ((0 == strcmp(pcArg, "--data-dir")) || (0 == strcmp(pcArg, "--dir"))))
    {
      if (iIdx + 1 >= argc)
      {
        dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "--data-dir needs a value", NULL);
        return DMCMD_EXIT_USAGE;
      }
      pstOpt->pcDataDir = argv[++iIdx];
      iIdx++;
    }
    else if (bGlobal && (0 == strcmp(pcArg, "--token")))
    {
      if (iIdx + 1 >= argc)
      {
        dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "--token needs a value", NULL);
        return DMCMD_EXIT_USAGE;
      }
      pstOpt->pcToken = argv[++iIdx];
      iIdx++;
    }
    else if (bGlobal && (0 == strcmp(pcArg, "--role")))
    {
      if (iIdx + 1 >= argc)
      {
        dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "--role needs a value", NULL);
        return DMCMD_EXIT_USAGE;
      }
      pstOpt->pcRole = argv[++iIdx];
      iIdx++;
    }
    else if (0 == strcmp(pcArg, "--json"))
    {
      pstOpt->bJson = DM_TRUE;
      iIdx++;
    }
    else if (0 == strcmp(pcArg, "--raw"))
    {
      pstOpt->bRaw = DM_TRUE;
      iIdx++;
    }
    else if (bGlobal && (0 == strcmp(pcArg, "--timeout")))
    {
      uint64 u64Ms = 0u;
      if ((iIdx + 1 >= argc) || !dmcmd_parse_u64(argv[iIdx + 1], &u64Ms) || (0u == u64Ms) ||
          (u64Ms > 3600000u))
      {
        dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "invalid --timeout", NULL);
        return DMCMD_EXIT_USAGE;
      }
      pstOpt->u32TimeoutMs = (uint32)u64Ms;
      pstOpt->bHasTimeout = DM_TRUE;
      iIdx += 2;
    }
    else if ((0 == strcmp(pcArg, "--body-json")) || (0 == strcmp(pcArg, "--body-file")) ||
             (0 == strcmp(pcArg, "--body-stdin")))
    {
      if (DMCMD_BODY_NONE != pstOpt->eBodyMode)
      {
        dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "only one --body-* option may be given", NULL);
        return DMCMD_EXIT_USAGE;
      }
      if (0 == strcmp(pcArg, "--body-stdin"))
      {
        pstOpt->eBodyMode = DMCMD_BODY_STDIN;
        iIdx++;
      }
      else
      {
        if (iIdx + 1 >= argc)
        {
          dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "missing value for --body-*", pcArg);
          return DMCMD_EXIT_USAGE;
        }
        pstOpt->eBodyMode = (0 == strcmp(pcArg, "--body-json")) ? DMCMD_BODY_JSON : DMCMD_BODY_FILE;
        pstOpt->pcBodyArg = argv[++iIdx];
        iIdx++;
      }
    }
    else
    {
      if (iRest >= DMCMD_MAX_ARGS)
      {
        /* 严格执行 64 参数硬边界，超限直接退出码 2（CLI-08 / T-BND-03 / T-PLG-07） */
        dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "too many arguments",
                         "maximum 64 command arguments exceeded");
        return DMCMD_EXIT_USAGE;
      }
      ppcRest[iRest++] = argv[iIdx++];
    }
  }
  *piRest = iRest;
  return DMCMD_EXIT_OK;
}

int main(int argc, char **argv)
{
  dmcmd_opt_st stOpt;
  char *apcRest[DMCMD_MAX_ARGS];
  int iRest = 0;
  int iRc = DMCMD_EXIT_OK;
  dmc_client_opt_st stCliOpt;
  dmc_client_st *pstCli = NULL;
  int iOpenErr = DMC_OK;

#ifndef _WIN32
  (void)signal(SIGPIPE, SIG_IGN); /* AUD-14：防断链写 socket 触发 SIGPIPE 杀进程 */
#endif
  memset(&stOpt, 0, sizeof(stOpt));
  stOpt.pcDataDir = getenv("DMCMD_DIR");
  stOpt.u32TimeoutMs = DMCMD_DEFAULT_TIMEOUT_MS;

  iRc = dmcmd__parse_cli(&stOpt, argc, argv, apcRest, &iRest);
  if (DMCMD_EXIT_OK != iRc)
  {
    return iRc;
  }
  if ((NULL == stOpt.pcDataDir) || ('\0' == stOpt.pcDataDir[0]))
  {
    stOpt.pcDataDir = ".";
  }
  if ((iRest > 0) && (0 == strcmp(apcRest[0], "mcp")))
  {
    if (iRest > 1)
    {
      dmcmd_emit_error(stOpt.bJson, DMC_ERR_ARG, "unexpected argument for mcp", apcRest[1]);
      return DMCMD_EXIT_USAGE;
    }
    return dmcmd_run_mcp(&stOpt);
  }
  if ((iRest > 0) && (0 == strcmp(apcRest[0], "watch")))
  {
    return dmcmd_cmd_watch(&stOpt, iRest - 1, apcRest + 1);
  }

  memset(&stCliOpt, 0, sizeof(stCliOpt));
  stCliOpt.pcTarget = stOpt.pcTarget;
  stCliOpt.pcDataDir = stOpt.pcDataDir;
  stCliOpt.pcToken = stOpt.pcToken;
  stCliOpt.pcRole = stOpt.pcRole;
  stCliOpt.u32ConnectTimeoutMs = stOpt.bHasTimeout ? stOpt.u32TimeoutMs : 5000u;
  stCliOpt.u32DefaultTimeoutMs = stOpt.u32TimeoutMs;
  pstCli = dmc_client_open(&stCliOpt, &iOpenErr);
  if (NULL == pstCli)
  {
    dmcmd_emit_error(stOpt.bJson, iOpenErr, dmc_err_str(iOpenErr),
                     "core is not running or handshake failed");
    return DMCMD_EXIT_ERR;
  }
  if (0 == iRest)
  {
    iRc = dmcmd__interactive(pstCli, &stOpt);
  }
  else
  {
    iRc = dmcmd_cmd_run(pstCli, &stOpt, iRest, apcRest);
  }
  dmc_client_close(pstCli);
  return iRc;
}

#ifdef __cplusplus
}
#endif
