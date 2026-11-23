/**
 ******************************************************************************
 * @file    dmcmd_cmd.c
 * @brief   dmcmd 子命令实现：help / version / stat / diag / log / watch / 通用接口调用。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "dmcmd_internal.h"

#include <dm/errno.h>
#include <dm/frame.h>
#include <dm/rpc.h>
#include <dm/version.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct dmcmd__builtin_meta
{
  uint16 u16ID;
  const char *pcName;
  uint32 u32Flags;
  const char *pcDesc;
  const char *pcArgs;
} dmcmd__builtin_meta_st;

static const dmcmd__builtin_meta_st g_astBuiltins[] = {
    {DM_LINKID_IFACE_LIST, "dm.iface.list", 0u, "list interfaces registered on the core", NULL},
    {DM_LINKID_LOG_RECENT, "dm.log.recent", DM_IFACE_DIAG, "recent log lines with filters",
     "{\"type\":\"object\",\"properties\":{\"limit\":{\"type\":\"integer\"},"
     "\"n\":{\"type\":\"integer\"},\"mod\":{\"type\":\"string\"},"
     "\"level\":{\"type\":\"string\",\"description\":\"debug|info|warn|error\"},"
     "\"grep\":{\"type\":\"string\"}}}"},
    {DM_LINKID_STAT, "dm.stat", DM_IFACE_DIAG,
     "counters: handlers, loop, thread pool, memory, connections, log", NULL},
    {DM_LINKID_PING, "dm.ping", 0u, "heartbeat", NULL},
    {DM_LINKID_LOG_LEVEL, "dm.log.level", DM_IFACE_MUTATE | DM_IFACE_ADVANCED,
     "change the log level or console output (CLI only)",
     "{\"type\":\"object\",\"properties\":{\"level\":{\"type\":\"string\","
     "\"description\":\"debug|info|warn|error|none\"},\"console\":{\"type\":\"boolean\"}}}"},
    {DM_LINKID_DIAG_SNAPSHOT, "dm.diag.snapshot", DM_IFACE_DIAG,
     "runtime diagnostic snapshot: conns, queue, lanes, timers, loop, push", NULL},
};

bool_t dmcmd_parse_u64(in const char *pcStr, out uint64 *pu64Out)
{
  char *pcEnd = NULL;
  unsigned long long ullVal = 0ull;
  if ((NULL == pcStr) || ('\0' == pcStr[0]) || ('-' == pcStr[0]))
  {
    return DM_FALSE;
  }
  errno = 0;
  ullVal = strtoull(pcStr, &pcEnd, 10);
  if ((pcEnd == pcStr) || ('\0' != *pcEnd) || (0 != errno))
  {
    return DM_FALSE;
  }
  *pu64Out = (uint64)ullVal;
  return DM_TRUE;
}

void dmcmd_usage(io FILE *pstOut)
{
  fprintf(pstOut, "usage: dmcmd [--target URI] [--data-dir DIR] [--token TOK] [--role ROLE]\n"
                  "             [--timeout MS] [--json] [--raw]\n"
                  "             [--body-json JSON | --body-file PATH | --body-stdin]\n"
                  "             <command> [args...]\n"
                  "commands:\n"
                  "  help [name] [--all]          list interfaces (or schema for one)\n"
                  "  version                      versions of dmcmd and core\n"
                  "  stat                         core runtime counters (dm.stat)\n"
                  "  diag                         diagnostic snapshot (dm.diag.snapshot)\n"
                  "  log [--limit N] [--mod M] [--level L] [--grep S]\n"
                  "  watch [t1,t2] [--count N] [--timeout MS] [--since SEQ]\n"
                  "  mcp                          concurrent async stdio<->HTTP/SSE MCP relay\n"
                  "  <name> [--arg v ...]         call any registered core interface\n");
  return;
}

static int dmcmd__handle_resp(in const dmcmd_opt_st *pstOpt, in const char *pcName,
                              in const dmc_resp_st *pstResp)
{
  if (DMC_OK == pstResp->iStatus)
  {
    dmcmd_emit_ok(pstOpt->bJson, pstOpt->bRaw, pstResp->pcBody, pstResp->u32Len);
    return DMCMD_EXIT_OK;
  }
  {
    const char *pcErr = dmc_err_str(pstResp->iStatus);
    const char *pcDetail = pstResp->pcBody;
    yyjson_doc *pstDoc = ((NULL != pstResp->pcBody) && (pstResp->u32Len > 0u))
                             ? yyjson_read(pstResp->pcBody, pstResp->u32Len, YYJSON_READ_NOFLAG)
                             : NULL;
    if (NULL != pstDoc)
    {
      const char *pcCoreErr = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(pstDoc), "error"));
      if ((NULL != pcCoreErr) && ('\0' != pcCoreErr[0]))
      {
        pcErr = pcCoreErr;
      }
    }
    dmcmd_emit_error(pstOpt->bJson, pstResp->iStatus, pcErr,
                     (NULL != pcDetail) ? pcDetail : pcName);
    yyjson_doc_free(pstDoc);
  }
  return DMCMD_EXIT_ERR;
}

static int dmcmd__call_id_print(io dmc_client_st *pstCli, in const dmcmd_opt_st *pstOpt,
                                in const char *pcName, in uint16 u16LinkID, in const char *pcBody)
{
  dmc_resp_st stResp;
  uint32 u32Len = (NULL != pcBody) ? (uint32)strlen(pcBody) : 0u;
  int iRc = DMCMD_EXIT_OK;

  (void)dmc_client_call_id(pstCli, u16LinkID, (u32Len > 0u) ? DM_FRAME_F_JSON : 0u, pcBody, u32Len,
                           pstOpt->u32TimeoutMs, &stResp);
  iRc = dmcmd__handle_resp(pstOpt, pcName, &stResp);
  dmc_resp_free(&stResp);
  return iRc;
}

static const dmcmd__builtin_meta_st *dmcmd__find_builtin(in const char *pcName)
{
  uint uiIdx = 0u;
  for (uiIdx = 0u; uiIdx < (uint)ARRAY_SIZE(g_astBuiltins); uiIdx++)
  {
    if (0 == strcmp(g_astBuiltins[uiIdx].pcName, pcName))
    {
      return &g_astBuiltins[uiIdx];
    }
  }
  return NULL;
}

static yyjson_mut_val *dmcmd__meta_to_json(io yyjson_mut_doc *pstDoc, in uint16 u16ID,
                                           in const char *pcName, in uint32 u32Flags,
                                           in const char *pcDesc, in const char *pcArgs,
                                           in bool_t bAllowed)
{
  yyjson_mut_val *pstObj = (NULL != pstDoc) ? yyjson_mut_obj(pstDoc) : NULL;
  if (NULL == pstObj)
  {
    return NULL;
  }
  (void)yyjson_mut_obj_add_uint(pstDoc, pstObj, "id", u16ID);
  (void)yyjson_mut_obj_add_strcpy(pstDoc, pstObj, "name", (NULL != pcName) ? pcName : "");
  (void)yyjson_mut_obj_add_strcpy(pstDoc, pstObj, "desc", (NULL != pcDesc) ? pcDesc : "");
  (void)yyjson_mut_obj_add_uint(pstDoc, pstObj, "flags", u32Flags);
  (void)yyjson_mut_obj_add_bool(pstDoc, pstObj, "mutate", 0u != (u32Flags & DM_IFACE_MUTATE));
  (void)yyjson_mut_obj_add_bool(pstDoc, pstObj, "advanced", 0u != (u32Flags & DM_IFACE_ADVANCED));
  (void)yyjson_mut_obj_add_bool(pstDoc, pstObj, "diag", 0u != (u32Flags & DM_IFACE_DIAG));
  (void)yyjson_mut_obj_add_bool(pstDoc, pstObj, "test_only", 0u != (u32Flags & DM_IFACE_TEST_ONLY));
  (void)yyjson_mut_obj_add_bool(pstDoc, pstObj, "allowed", bAllowed ? true : false);
  if ((NULL != pcArgs) && ('\0' != pcArgs[0]))
  {
    yyjson_doc *pstSch = yyjson_read(pcArgs, strlen(pcArgs), YYJSON_READ_NOFLAG);
    if (NULL != pstSch)
    {
      yyjson_mut_val *pstCopy = yyjson_val_mut_copy(pstDoc, yyjson_doc_get_root(pstSch));
      if (NULL != pstCopy)
      {
        (void)yyjson_mut_obj_add_val(pstDoc, pstObj, "schema", pstCopy);
      }
      yyjson_doc_free(pstSch);
    }
    else
    {
      (void)yyjson_mut_obj_add_strcpy(pstDoc, pstObj, "args", pcArgs);
    }
  }
  else
  {
    (void)yyjson_mut_obj_add_obj(pstDoc, pstObj, "schema");
  }
  return pstObj;
}

static int dmcmd__emit_meta_json(in const dmcmd_opt_st *pstOpt, in uint16 u16ID,
                                 in const char *pcName, in uint32 u32Flags, in const char *pcDesc,
                                 in const char *pcArgs, in bool_t bAllowed)
{
  yyjson_mut_doc *pstDoc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *pstObj =
      dmcmd__meta_to_json(pstDoc, u16ID, pcName, u32Flags, pcDesc, pcArgs, bAllowed);
  char *pcText = NULL;
  size_t uLen = 0u;

  if (NULL != pstObj)
  {
    yyjson_mut_doc_set_root(pstDoc, pstObj);
    pcText = yyjson_mut_write(pstDoc, YYJSON_WRITE_NOFLAG, &uLen);
  }
  yyjson_mut_doc_free(pstDoc);
  if (NULL == pcText)
  {
    dmcmd_emit_error(DM_TRUE, DMC_ERR_QUEUE, "out of memory", pcName);
    return DMCMD_EXIT_ERR;
  }
  dmcmd_emit_ok(DM_TRUE, pstOpt->bRaw, pcText, uLen);
  free(pcText);
  return DMCMD_EXIT_OK;
}

static int dmcmd__help_one(in const dmcmd_opt_st *pstOpt, in const char *pcName,
                           in yyjson_val *pstIfaces)
{
  const dmcmd__builtin_meta_st *pstBi = dmcmd__find_builtin(pcName);
  yyjson_val *pstOne = NULL;
  yyjson_arr_iter stIter;

  if (NULL != pstBi)
  {
    if (pstOpt->bJson)
    {
      return dmcmd__emit_meta_json(pstOpt, pstBi->u16ID, pstBi->pcName, pstBi->u32Flags,
                                   pstBi->pcDesc, pstBi->pcArgs, DM_TRUE);
    }
    dmcmd_args_help(stdout, pstBi->pcName, pstBi->pcDesc, pstBi->pcArgs);
    return DMCMD_EXIT_OK;
  }
  if (yyjson_is_arr(pstIfaces))
  {
    yyjson_arr_iter_init(pstIfaces, &stIter);
    while (NULL != (pstOne = yyjson_arr_iter_next(&stIter)))
    {
      if (yyjson_equals_str(yyjson_obj_get(pstOne, "name"), pcName))
      {
        const char *pcDesc = yyjson_get_str(yyjson_obj_get(pstOne, "desc"));
        const char *pcArgs = yyjson_get_str(yyjson_obj_get(pstOne, "args"));
        uint32 u32Flags = (uint32)yyjson_get_uint(yyjson_obj_get(pstOne, "flags"));
        bool_t bAllowed = !yyjson_is_false(yyjson_obj_get(pstOne, "allowed"));
        if ((0u == u32Flags) && yyjson_get_bool(yyjson_obj_get(pstOne, "mutate")))
        {
          u32Flags = DM_IFACE_MUTATE;
        }
        if (pstOpt->bJson)
        {
          return dmcmd__emit_meta_json(pstOpt,
                                       (uint16)yyjson_get_uint(yyjson_obj_get(pstOne, "id")),
                                       pcName, u32Flags, pcDesc, pcArgs, bAllowed);
        }
        dmcmd_args_help(stdout, pcName, pcDesc, pcArgs);
        if (0u != (u32Flags & DM_IFACE_MUTATE))
        {
          printf("(changes things)\n");
        }
        if (!bAllowed)
        {
          printf("(this role may not call it)\n");
        }
        return DMCMD_EXIT_OK;
      }
    }
  }
  dmcmd_emit_error(pstOpt->bJson, DMC_ERR_NOT_FOUND, "no such interface", pcName);
  return DMCMD_EXIT_USAGE;
}

static int dmcmd__help_list_json(in const dmcmd_opt_st *pstOpt, in yyjson_val *pstArr)
{
  yyjson_mut_doc *pstOut = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *pstRoot = (NULL != pstOut) ? yyjson_mut_obj(pstOut) : NULL;
  yyjson_mut_val *pstOutArr =
      (NULL != pstRoot) ? yyjson_mut_obj_add_arr(pstOut, pstRoot, "ifaces") : NULL;
  yyjson_val *pstOne = NULL;
  yyjson_arr_iter stIter;
  uint uiBi = 0u;
  char *pcText = NULL;
  size_t uLen = 0u;

  if (NULL == pstOutArr)
  {
    yyjson_mut_doc_free(pstOut);
    dmcmd_emit_error(DM_TRUE, DMC_ERR_QUEUE, "out of memory", "help");
    return DMCMD_EXIT_ERR;
  }
  yyjson_mut_doc_set_root(pstOut, pstRoot);
  for (uiBi = 0u; uiBi < (uint)ARRAY_SIZE(g_astBuiltins); uiBi++)
  {
    yyjson_mut_val *pstItem = dmcmd__meta_to_json(
        pstOut, g_astBuiltins[uiBi].u16ID, g_astBuiltins[uiBi].pcName, g_astBuiltins[uiBi].u32Flags,
        g_astBuiltins[uiBi].pcDesc, g_astBuiltins[uiBi].pcArgs, DM_TRUE);
    if (NULL != pstItem)
    {
      (void)yyjson_mut_arr_append(pstOutArr, pstItem);
    }
  }
  if (yyjson_is_arr(pstArr))
  {
    yyjson_arr_iter_init(pstArr, &stIter);
    while (NULL != (pstOne = yyjson_arr_iter_next(&stIter)))
    {
      uint32 u32Flags = (uint32)yyjson_get_uint(yyjson_obj_get(pstOne, "flags"));
      yyjson_mut_val *pstItem = NULL;
      if ((0u == u32Flags) && yyjson_get_bool(yyjson_obj_get(pstOne, "mutate")))
      {
        u32Flags = DM_IFACE_MUTATE;
      }
      pstItem = dmcmd__meta_to_json(pstOut, (uint16)yyjson_get_uint(yyjson_obj_get(pstOne, "id")),
                                    yyjson_get_str(yyjson_obj_get(pstOne, "name")), u32Flags,
                                    yyjson_get_str(yyjson_obj_get(pstOne, "desc")),
                                    yyjson_get_str(yyjson_obj_get(pstOne, "args")),
                                    !yyjson_is_false(yyjson_obj_get(pstOne, "allowed")));
      if (NULL != pstItem)
      {
        (void)yyjson_mut_arr_append(pstOutArr, pstItem);
      }
    }
  }
  pcText = yyjson_mut_write(pstOut, YYJSON_WRITE_NOFLAG, &uLen);
  yyjson_mut_doc_free(pstOut);
  if (NULL == pcText)
  {
    dmcmd_emit_error(DM_TRUE, DMC_ERR_QUEUE, "out of memory", "help");
    return DMCMD_EXIT_ERR;
  }
  dmcmd_emit_ok(DM_TRUE, pstOpt->bRaw, pcText, uLen);
  free(pcText);
  return DMCMD_EXIT_OK;
}

static int dmcmd__help(io dmc_client_st *pstCli, in const dmcmd_opt_st *pstOpt, in int iArgc,
                       in char **ppcArgv)
{
  const char *pcRaw = NULL;
  yyjson_doc *pstDoc = NULL;
  yyjson_val *pstArr = NULL;
  const char *pcName = NULL;
  bool_t bAll = DM_FALSE;
  int iIdx = 0;
  int iRc = DMCMD_EXIT_OK;

  for (iIdx = 0; iIdx < iArgc; iIdx++)
  {
    if (0 == strcmp(ppcArgv[iIdx], "--all"))
    {
      bAll = DM_TRUE;
    }
    else if (('-' != ppcArgv[iIdx][0]) && (NULL == pcName))
    {
      pcName = ppcArgv[iIdx];
    }
    else
    {
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "unexpected help argument", ppcArgv[iIdx]);
      return DMCMD_EXIT_USAGE;
    }
  }
  if ((NULL != pcName) && (NULL != dmcmd__find_builtin(pcName)))
  {
    return dmcmd__help_one(pstOpt, pcName, NULL);
  }
  pcRaw = dmc_client_ifaces_json(pstCli, DM_FALSE);
  if (NULL == pcRaw)
  {
    dmcmd_emit_error(pstOpt->bJson, DMC_ERR_NOT_CON, "failed to query interface list", "help");
    return DMCMD_EXIT_ERR;
  }
  pstDoc = yyjson_read(pcRaw, strlen(pcRaw), 0);
  pstArr = yyjson_obj_get(yyjson_doc_get_root(pstDoc), "ifaces");
  if (NULL != pcName)
  {
    iRc = dmcmd__help_one(pstOpt, pcName, pstArr);
    yyjson_doc_free(pstDoc);
    return iRc;
  }
  if (pstOpt->bJson)
  {
    iRc = dmcmd__help_list_json(pstOpt, pstArr);
    yyjson_doc_free(pstDoc);
    return iRc;
  }

  dmcmd_usage(stdout);
  printf("interfaces:\n");
  if (yyjson_is_arr(pstArr))
  {
    yyjson_val *pstOne = NULL;
    yyjson_arr_iter stIter;
    uint32 u32Folded = 0u;
    yyjson_arr_iter_init(pstArr, &stIter);
    while (NULL != (pstOne = yyjson_arr_iter_next(&stIter)))
    {
      uint32 u32Flags = (uint32)yyjson_get_uint(yyjson_obj_get(pstOne, "flags"));
      const char *pcIfaceName = yyjson_get_str(yyjson_obj_get(pstOne, "name"));
      const char *pcDesc = yyjson_get_str(yyjson_obj_get(pstOne, "desc"));
      if (yyjson_is_false(yyjson_obj_get(pstOne, "allowed")))
      {
        continue;
      }
      if (!bAll && (0u != (u32Flags & (DM_IFACE_ADVANCED | DM_IFACE_DIAG))))
      {
        u32Folded++;
        continue;
      }
      printf("  %-24s %s%s\n", (NULL != pcIfaceName) ? pcIfaceName : "",
             (NULL != pcDesc) ? pcDesc : "",
             yyjson_get_bool(yyjson_obj_get(pstOne, "mutate")) ? " (changes things)" : "");
    }
    if (u32Folded > 0u)
    {
      printf("  (%u advanced/diag interfaces hidden; pass --all to expand)\n", (unsigned)u32Folded);
    }
  }
  yyjson_doc_free(pstDoc);
  return DMCMD_EXIT_OK;
}

static int dmcmd__version(io dmc_client_st *pstCli, in const dmcmd_opt_st *pstOpt)
{
  dmc_resp_st stResp;
  yyjson_doc *pstDoc = NULL;
  const char *pcCore = NULL;
  char acSelf[64];

  (void)dmc_client_call_id(pstCli, DM_LINKID_STAT, 0u, NULL, 0u, pstOpt->u32TimeoutMs, &stResp);
  if (DMC_OK != stResp.iStatus)
  {
    int iRc = dmcmd__handle_resp(pstOpt, "version", &stResp);
    dmc_resp_free(&stResp);
    return iRc;
  }
  pstDoc = yyjson_read(stResp.pcBody, stResp.u32Len, 0);
  pcCore = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(pstDoc), "version"));
  snprintf(acSelf, sizeof(acSelf), "%d.%d.%d-%s", DM_VERSION_MAJOR, DM_VERSION_MINOR,
           DM_VERSION_PATCH, DM_VERSION_SUFFIX);
  if (pstOpt->bJson)
  {
    yyjson_mut_doc *pstOut = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *pstRoot = (NULL != pstOut) ? yyjson_mut_obj(pstOut) : NULL;
    char *pcText = NULL;
    size_t uLen = 0u;
    if (NULL != pstRoot)
    {
      yyjson_mut_doc_set_root(pstOut, pstRoot);
      (void)yyjson_mut_obj_add_str(pstOut, pstRoot, "dmcmd", acSelf);
      (void)yyjson_mut_obj_add_str(pstOut, pstRoot, "core", (NULL != pcCore) ? pcCore : "");
      pcText = yyjson_mut_write(pstOut, YYJSON_WRITE_NOFLAG, &uLen);
    }
    yyjson_mut_doc_free(pstOut);
    if (NULL == pcText)
    {
      yyjson_doc_free(pstDoc);
      dmc_resp_free(&stResp);
      dmcmd_emit_error(DM_TRUE, DMC_ERR_QUEUE, "out of memory", "version");
      return DMCMD_EXIT_ERR;
    }
    dmcmd_emit_ok(DM_TRUE, pstOpt->bRaw, pcText, uLen);
    free(pcText);
  }
  else
  {
    printf("dmcmd %s\ncore  %s\n", acSelf, (NULL != pcCore) ? pcCore : "?");
    fflush(stdout);
  }
  yyjson_doc_free(pstDoc);
  dmc_resp_free(&stResp);
  return DMCMD_EXIT_OK;
}

static int dmcmd__log(io dmc_client_st *pstCli, in const dmcmd_opt_st *pstOpt, in int iArgc,
                      in char **ppcArgv)
{
  char *pcBody = NULL;
  dmc_resp_st stResp;
  int iRc = DMCMD_EXIT_OK;

  if (DMCMD_BODY_NONE != pstOpt->eBodyMode)
  {
    if (iArgc > 0)
    {
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "unexpected arguments with --body-*",
                       ppcArgv[0]);
      return DMCMD_EXIT_USAGE;
    }
    iRc = dmcmd_load_body_opt(pstOpt, &pcBody);
  }
  else
  {
    iRc = dmcmd_args_build(pstOpt, "log", g_astBuiltins[1].pcArgs, iArgc, ppcArgv, &pcBody);
  }
  if (DMCMD_EXIT_OK != iRc)
  {
    return iRc;
  }
  (void)dmc_client_call_id(pstCli, DM_LINKID_LOG_RECENT, DM_FRAME_F_JSON, pcBody,
                           (uint32)strlen(pcBody), pstOpt->u32TimeoutMs, &stResp);
  free(pcBody);
  if ((DMC_OK == stResp.iStatus) && !pstOpt->bJson)
  {
    yyjson_doc *pstDoc = yyjson_read(stResp.pcBody, stResp.u32Len, 0);
    yyjson_val *pstLines = yyjson_obj_get(yyjson_doc_get_root(pstDoc), "lines");
    yyjson_val *pstLine = NULL;
    yyjson_arr_iter stIter;
    if (yyjson_is_arr(pstLines))
    {
      yyjson_arr_iter_init(pstLines, &stIter);
      while (NULL != (pstLine = yyjson_arr_iter_next(&stIter)))
      {
        const char *pcStr = yyjson_get_str(pstLine);
        if (NULL != pcStr)
        {
          printf("%s\n", pcStr);
        }
      }
      fflush(stdout);
    }
    yyjson_doc_free(pstDoc);
    dmc_resp_free(&stResp);
    return DMCMD_EXIT_OK;
  }
  iRc = dmcmd__handle_resp(pstOpt, "log", &stResp);
  dmc_resp_free(&stResp);
  return iRc;
}

static int dmcmd__iface(io dmc_client_st *pstCli, in const dmcmd_opt_st *pstOpt,
                        in const char *pcName, in int iArgc, in char **ppcArgv)
{
  const dmcmd__builtin_meta_st *pstBi = dmcmd__find_builtin(pcName);
  char *pcBody = NULL;
  dmc_resp_st stResp;
  int iRc = DMCMD_EXIT_OK;

  if (DMCMD_BODY_NONE != pstOpt->eBodyMode)
  {
    if (iArgc > 0)
    {
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "unexpected arguments with --body-*",
                       ppcArgv[0]);
      return DMCMD_EXIT_USAGE;
    }
    iRc = dmcmd_load_body_opt(pstOpt, &pcBody);
  }
  else if (NULL != pstBi)
  {
    iRc = dmcmd_args_build(pstOpt, pcName, pstBi->pcArgs, iArgc, ppcArgv, &pcBody);
  }
  else
  {
    const char *pcRaw = dmc_client_ifaces_json(pstCli, DM_FALSE);
    yyjson_doc *pstDoc = NULL;
    yyjson_val *pstArr = NULL;
    yyjson_val *pstOne = NULL;
    yyjson_arr_iter stIter;
    const char *pcSchema = NULL;
    bool_t bFound = DM_FALSE;

    if (NULL == pcRaw)
    {
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_NOT_CON, "failed to query interface list", pcName);
      return DMCMD_EXIT_ERR;
    }
    pstDoc = yyjson_read(pcRaw, strlen(pcRaw), 0);
    pstArr = yyjson_obj_get(yyjson_doc_get_root(pstDoc), "ifaces");
    if (yyjson_is_arr(pstArr))
    {
      yyjson_arr_iter_init(pstArr, &stIter);
      while (NULL != (pstOne = yyjson_arr_iter_next(&stIter)))
      {
        if (yyjson_equals_str(yyjson_obj_get(pstOne, "name"), pcName))
        {
          pcSchema = yyjson_get_str(yyjson_obj_get(pstOne, "args"));
          bFound = DM_TRUE;
          break;
        }
      }
    }
    if (!bFound)
    {
      yyjson_doc_free(pstDoc);
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_NOT_FOUND, "unknown command", pcName);
      return DMCMD_EXIT_USAGE;
    }
    iRc = dmcmd_args_build(pstOpt, pcName, pcSchema, iArgc, ppcArgv, &pcBody);
    yyjson_doc_free(pstDoc);
  }
  if (DMCMD_EXIT_OK != iRc)
  {
    return iRc;
  }
  (void)dmc_client_call_name(pstCli, pcName, pcBody, pstOpt->u32TimeoutMs, &stResp);
  free(pcBody);
  iRc = dmcmd__handle_resp(pstOpt, pcName, &stResp);
  dmc_resp_free(&stResp);
  return iRc;
}

int dmcmd_cmd_run(io dmc_client_st *pstCli, in const dmcmd_opt_st *pstOpt, in int iArgc,
                  in char **ppcArgv)
{
  const char *pcCmd = ppcArgv[0];
  if ((0 == strcmp(pcCmd, "help")) || (0 == strcmp(pcCmd, "--help")))
  {
    return dmcmd__help(pstCli, pstOpt, iArgc - 1, ppcArgv + 1);
  }
  if (0 == strcmp(pcCmd, "version"))
  {
    if (iArgc > 1)
    {
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "unexpected argument for version", ppcArgv[1]);
      return DMCMD_EXIT_USAGE;
    }
    return dmcmd__version(pstCli, pstOpt);
  }
  if (0 == strcmp(pcCmd, "stat"))
  {
    if (iArgc > 1)
    {
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "unexpected argument for stat", ppcArgv[1]);
      return DMCMD_EXIT_USAGE;
    }
    return dmcmd__call_id_print(pstCli, pstOpt, "stat", DM_LINKID_STAT, NULL);
  }
  if (0 == strcmp(pcCmd, "diag"))
  {
    if (iArgc > 1)
    {
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "unexpected argument for diag", ppcArgv[1]);
      return DMCMD_EXIT_USAGE;
    }
    return dmcmd__call_id_print(pstCli, pstOpt, "diag", DM_LINKID_DIAG_SNAPSHOT, NULL);
  }
  if (0 == strcmp(pcCmd, "log"))
  {
    return dmcmd__log(pstCli, pstOpt, iArgc - 1, ppcArgv + 1);
  }
  if ((0 == strcmp(pcCmd, "watch")) || (0 == strcmp(pcCmd, "mcp")))
  {
    dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "command not allowed in interactive mode", pcCmd);
    return DMCMD_EXIT_USAGE;
  }
  return dmcmd__iface(pstCli, pstOpt, pcCmd, iArgc - 1, ppcArgv + 1);
}

#ifdef __cplusplus
}
#endif
