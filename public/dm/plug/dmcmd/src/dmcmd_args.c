/**
 ******************************************************************************
 * @file    dmcmd_args.c
 * @author  MDM
 * @version 0.0.1.0
 * @brief   dmcmd 参数解析（Schema + --body-*）、统一 JSON 信封与人类表格输出
 *          （DD-02 §3.1~3.2，CLI-01/04/08）。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "dmcmd_internal.h"
#include <dm/frame.h>

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void dmcmd__zero_mem(dm_out void *pvBuf, dm_in size_t uLen)
{
  volatile uint8 *pu8 = (volatile uint8 *)pvBuf;
  size_t uIdx = 0u;
  for (uIdx = 0u; uIdx < uLen; uIdx++)
  {
    pu8[uIdx] = 0u;
  }
  return;
}

/* ============================ 统一输出（CLI-01 / CLI-04） ============================ */

void dmcmd_emit_error(dm_in bool_t bJson, dm_in int iStatus, dm_in const char *pcErr,
                      dm_in const char *pcDetail)
{
  const char *pcMsg = (NULL != pcErr) ? pcErr : dmc_err_str(iStatus);
  if (bJson)
  {
    yyjson_mut_doc *pstDoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *pstRoot = (NULL != pstDoc) ? yyjson_mut_obj(pstDoc) : NULL;
    char *pcText = NULL;
    if (NULL != pstRoot)
    {
      yyjson_mut_doc_set_root(pstDoc, pstRoot);
      (void)yyjson_mut_obj_add_false(pstDoc, pstRoot, "ok");
      (void)yyjson_mut_obj_add_int(pstDoc, pstRoot, "status", iStatus);
      (void)yyjson_mut_obj_add_str(pstDoc, pstRoot, "error", pcMsg);
      if ((NULL != pcDetail) && ('\0' != pcDetail[0]))
      {
        (void)yyjson_mut_obj_add_str(pstDoc, pstRoot, "detail", pcDetail);
      }
      pcText = yyjson_mut_write(pstDoc, YYJSON_WRITE_NOFLAG, NULL);
    }
    yyjson_mut_doc_free(pstDoc);
    if (NULL != pcText)
    {
      fprintf(stdout, "%s\n", pcText);
      free(pcText);
    }
    else
    {
      fprintf(stdout, "{\"ok\":false,\"status\":%d,\"error\":\"error\"}\n", iStatus);
    }
    fflush(stdout);
    return;
  }
  if ((NULL != pcDetail) && ('\0' != pcDetail[0]))
  {
    fprintf(stderr, "dmcmd: %s (%d): %s\n", pcMsg, iStatus, pcDetail);
  }
  else
  {
    fprintf(stderr, "dmcmd: %s (%d)\n", pcMsg, iStatus);
  }
  fflush(stderr);
  return;
}

static void dmcmd__cell(dm_io FILE *pstOut, dm_in yyjson_val *pstVal)
{
  char *pcText = NULL;
  if (yyjson_is_str(pstVal))
  {
    fputs(yyjson_get_str(pstVal), pstOut);
    return;
  }
  pcText = yyjson_val_write(pstVal, YYJSON_WRITE_NOFLAG, NULL);
  if (NULL != pcText)
  {
    fputs(pcText, pstOut);
    free(pcText);
  }
  return;
}

static void dmcmd__obj(dm_io FILE *pstOut, dm_in yyjson_val *pstObj, dm_in const char *pcPrefix)
{
  yyjson_val *pstKey = NULL;
  yyjson_obj_iter stIter;
  char acKey[256];

  (void)yyjson_obj_iter_init(pstObj, &stIter);
  while (NULL != (pstKey = yyjson_obj_iter_next(&stIter)))
  {
    yyjson_val *pstVal = yyjson_obj_iter_get_val(pstKey);
    snprintf(acKey, sizeof(acKey), "%s%s%s", pcPrefix, ('\0' != pcPrefix[0]) ? "." : "",
             yyjson_get_str(pstKey));
    if (yyjson_is_obj(pstVal) && (0u != yyjson_obj_size(pstVal)))
    {
      dmcmd__obj(pstOut, pstVal, acKey);
      continue;
    }
    fprintf(pstOut, "%-24s ", acKey);
    dmcmd__cell(pstOut, pstVal);
    fputc('\n', pstOut);
  }
  return;
}

static void dmcmd__arr(dm_io FILE *pstOut, dm_in yyjson_val *pstArr)
{
  yyjson_val *pstFirst = yyjson_arr_get_first(pstArr);
  yyjson_val *pstOne = NULL;
  yyjson_val *pstKey = NULL;
  yyjson_arr_iter stRows;
  yyjson_obj_iter stCols;

  if (!yyjson_is_obj(pstFirst))
  {
    (void)yyjson_arr_iter_init(pstArr, &stRows);
    while (NULL != (pstOne = yyjson_arr_iter_next(&stRows)))
    {
      dmcmd__cell(pstOut, pstOne);
      fputc('\n', pstOut);
    }
    return;
  }
  (void)yyjson_obj_iter_init(pstFirst, &stCols);
  while (NULL != (pstKey = yyjson_obj_iter_next(&stCols)))
  {
    const char *pcKey = yyjson_get_str(pstKey);
    fprintf(pstOut, "%-16s ", (NULL != pcKey) ? pcKey : "");
  }
  fputc('\n', pstOut);
  (void)yyjson_arr_iter_init(pstArr, &stRows);
  while (NULL != (pstOne = yyjson_arr_iter_next(&stRows)))
  {
    (void)yyjson_obj_iter_init(pstFirst, &stCols);
    while (NULL != (pstKey = yyjson_obj_iter_next(&stCols)))
    {
      yyjson_val *pstVal = yyjson_obj_get(pstOne, yyjson_get_str(pstKey));
      dmcmd__cell(pstOut, pstVal);
      fputs("  ", pstOut);
    }
    fputc('\n', pstOut);
  }
  return;
}

static void dmcmd__emit_json_envelope(dm_in bool_t bRaw, dm_in const char *pcBody,
                                      dm_in size_t uLen)
{
  yyjson_doc *pstDoc =
      ((NULL != pcBody) && (uLen > 0u)) ? yyjson_read(pcBody, uLen, YYJSON_READ_NOFLAG) : NULL;

  if (bRaw)
  {
    if (NULL != pstDoc)
    {
      char *pcText = yyjson_write(pstDoc, YYJSON_WRITE_NOFLAG, NULL);
      fprintf(stdout, "%s\n", (NULL != pcText) ? pcText : "null");
      free(pcText);
    }
    else if ((NULL != pcBody) && (uLen > 0u))
    {
      fwrite(pcBody, 1u, uLen, stdout);
      if ('\n' != pcBody[uLen - 1u])
      {
        fputc('\n', stdout);
      }
    }
    else
    {
      fputs("null\n", stdout);
    }
    yyjson_doc_free(pstDoc);
    fflush(stdout);
    return;
  }
  if (NULL != pstDoc)
  {
    char *pcCompact = yyjson_write(pstDoc, YYJSON_WRITE_NOFLAG, NULL);
    fprintf(stdout, "{\"ok\":true,\"status\":0,\"data\":%s}\n",
            (NULL != pcCompact) ? pcCompact : "null");
    free(pcCompact);
    yyjson_doc_free(pstDoc);
    fflush(stdout);
    return;
  }
  if ((NULL == pcBody) || (0u == uLen))
  {
    fputs("{\"ok\":true,\"status\":0,\"data\":null}\n", stdout);
    fflush(stdout);
    return;
  }
  {
    yyjson_mut_doc *pstOut = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *pstRoot = (NULL != pstOut) ? yyjson_mut_obj(pstOut) : NULL;
    char *pcText = NULL;
    if (NULL != pstRoot)
    {
      yyjson_mut_doc_set_root(pstOut, pstRoot);
      (void)yyjson_mut_obj_add_true(pstOut, pstRoot, "ok");
      (void)yyjson_mut_obj_add_int(pstOut, pstRoot, "status", 0);
      (void)yyjson_mut_obj_add_strn(pstOut, pstRoot, "data", pcBody, uLen);
      pcText = yyjson_mut_write(pstOut, YYJSON_WRITE_NOFLAG, NULL);
    }
    yyjson_mut_doc_free(pstOut);
    fprintf(stdout, "%s\n", (NULL != pcText) ? pcText : "{\"ok\":true,\"status\":0,\"data\":null}");
    free(pcText);
    fflush(stdout);
  }
  return;
}

void dmcmd_emit_ok(dm_in bool_t bJson, dm_in bool_t bRaw, dm_in const char *pcBody,
                   dm_in size_t uLen)
{
  yyjson_doc *pstDoc = NULL;
  yyjson_val *pstRoot = NULL;
  yyjson_val *pstKey = NULL;
  yyjson_obj_iter stIter;

  if (bJson)
  {
    dmcmd__emit_json_envelope(bRaw, pcBody, uLen);
    return;
  }
  if ((NULL == pcBody) || (0u == uLen))
  {
    return;
  }
  pstDoc = yyjson_read(pcBody, uLen, YYJSON_READ_NOFLAG);
  pstRoot = yyjson_doc_get_root(pstDoc);
  if (NULL == pstRoot)
  {
    fwrite(pcBody, 1u, uLen, stdout);
    if ('\n' != pcBody[uLen - 1u])
    {
      fputc('\n', stdout);
    }
    fflush(stdout);
    return;
  }
  if (yyjson_is_arr(pstRoot))
  {
    dmcmd__arr(stdout, pstRoot);
  }
  else if (yyjson_is_obj(pstRoot))
  {
    (void)yyjson_obj_iter_init(pstRoot, &stIter);
    pstKey = yyjson_obj_iter_next(&stIter);
    if ((1u == yyjson_obj_size(pstRoot)) && yyjson_is_arr(yyjson_obj_iter_get_val(pstKey)))
    {
      dmcmd__arr(stdout, yyjson_obj_iter_get_val(pstKey));
    }
    else
    {
      dmcmd__obj(stdout, pstRoot, "");
    }
  }
  else
  {
    dmcmd__cell(stdout, pstRoot);
    fputc('\n', stdout);
  }
  yyjson_doc_free(pstDoc);
  fflush(stdout);
  return;
}

/* ============================ --body-* 载荷加载 ============================ */

static char *dmcmd__read_stdin_all(dm_out size_t *puLen)
{
  size_t uCap = 4096u;
  size_t uLen = 0u;
  char *pcBuf = (char *)malloc(uCap);
  size_t uRead = 0u;

  *puLen = 0u;
  if (NULL == pcBuf)
  {
    return NULL;
  }
  for (;;)
  {
    if ((uCap - uLen - 1u < 4096u) && (uCap < DM_FRAME_MAX_DEFAULT + 1u))
    {
      size_t uNext =
          (uCap > (DM_FRAME_MAX_DEFAULT + 1u) / 2u) ? (DM_FRAME_MAX_DEFAULT + 1u) : (uCap * 2u);
      char *pcNew = (char *)realloc(pcBuf, uNext);
      if (NULL == pcNew)
      {
        free(pcBuf);
        return NULL;
      }
      pcBuf = pcNew;
      uCap = uNext;
    }
    if (uLen >= DM_FRAME_MAX_DEFAULT)
    {
      int iExtra = fgetc(stdin);
      if (EOF != iExtra)
      {
        free(pcBuf);
        return NULL;
      }
      break;
    }
    uRead = fread(pcBuf + uLen, 1u, uCap - uLen - 1u, stdin);
    uLen += uRead;
    if (0u == uRead)
    {
      break;
    }
  }
  pcBuf[uLen] = '\0';
  *puLen = uLen;
  return pcBuf;
}

int dmcmd_load_body_opt(dm_in const dmcmd_opt_st *pstOpt, dm_out char **ppcBody)
{
  char *pcRaw = NULL;
  size_t uRaw = 0u;
  yyjson_doc *pstCheck = NULL;

  *ppcBody = NULL;
  if (DMCMD_BODY_JSON == pstOpt->eBodyMode)
  {
    if (NULL == pstOpt->pcBodyArg)
    {
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "invalid argument", "--body-json requires JSON");
      return DMCMD_EXIT_USAGE;
    }
    uRaw = strlen(pstOpt->pcBodyArg);
    pcRaw = (char *)malloc(uRaw + 1u);
    if (NULL != pcRaw)
    {
      memcpy(pcRaw, pstOpt->pcBodyArg, uRaw + 1u);
    }
  }
  else if (DMCMD_BODY_FILE == pstOpt->eBodyMode)
  {
    pcRaw = dmc_read_file_utf8(pstOpt->pcBodyArg, &uRaw);
    if (NULL == pcRaw)
    {
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "invalid argument",
                       "cannot read --body-file (missing or >16MB)");
      return DMCMD_EXIT_USAGE;
    }
  }
  else if (DMCMD_BODY_STDIN == pstOpt->eBodyMode)
  {
    pcRaw = dmcmd__read_stdin_all(&uRaw);
    if (NULL == pcRaw)
    {
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "invalid argument",
                       "failed to read --body-stdin (oom or >16MB)");
      return DMCMD_EXIT_USAGE;
    }
  }
  if (NULL == pcRaw)
  {
    dmcmd_emit_error(pstOpt->bJson, DMC_ERR_QUEUE, "out of memory", NULL);
    return DMCMD_EXIT_ERR;
  }
  pstCheck = yyjson_read(pcRaw, uRaw, YYJSON_READ_NOFLAG);
  if (NULL == pstCheck)
  {
    free(pcRaw);
    dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "invalid argument",
                     "request body is not valid JSON");
    return DMCMD_EXIT_USAGE;
  }
  *ppcBody = yyjson_write(pstCheck, YYJSON_WRITE_NOFLAG, NULL);
  yyjson_doc_free(pstCheck);
  free(pcRaw);
  if (NULL == *ppcBody)
  {
    dmcmd_emit_error(pstOpt->bJson, DMC_ERR_QUEUE, "out of memory", NULL);
    return DMCMD_EXIT_ERR;
  }
  return DMCMD_EXIT_OK;
}

/* ============================ Schema 参数解析 ============================ */

static yyjson_val *dmcmd__props(dm_in yyjson_val *pstSchema)
{
  yyjson_val *pstProps = yyjson_obj_get(pstSchema, "properties");
  return yyjson_is_obj(pstProps) ? pstProps : NULL;
}

static const char *dmcmd__type(dm_in yyjson_val *pstProp)
{
  const char *pcType = yyjson_get_str(yyjson_obj_get(pstProp, "type"));
  return (NULL != pcType) ? pcType : "string";
}

static bool_t dmcmd__required(dm_in yyjson_val *pstSchema, dm_in const char *pcName)
{
  yyjson_val *pstReq = yyjson_obj_get(pstSchema, "required");
  yyjson_val *pstOne = NULL;
  yyjson_arr_iter stIter;

  if (!yyjson_is_arr(pstReq))
  {
    return DM_FALSE;
  }
  (void)yyjson_arr_iter_init(pstReq, &stIter);
  while (NULL != (pstOne = yyjson_arr_iter_next(&stIter)))
  {
    if (yyjson_equals_str(pstOne, pcName))
    {
      return DM_TRUE;
    }
  }
  return DM_FALSE;
}

static bool_t dmcmd__put(dm_io yyjson_mut_doc *pstDoc, dm_io yyjson_mut_val *pstObj,
                         dm_in const char *pcKey, dm_in const char *pcType, dm_in const char *pcVal)
{
  char *pcEnd = NULL;
  yyjson_doc *pstJson = NULL;
  yyjson_mut_val *pstVal = NULL;

  if (0 == strcmp(pcType, "integer"))
  {
    long long llVal = 0;
    errno = 0;
    llVal = strtoll(pcVal, &pcEnd, 10);
    if ((pcEnd == pcVal) || ('\0' != *pcEnd) || (0 != errno))
    {
      return DM_FALSE;
    }
    pstVal = yyjson_mut_sint(pstDoc, (int64_t)llVal);
  }
  else if (0 == strcmp(pcType, "number"))
  {
    double dVal = strtod(pcVal, &pcEnd);
    if ((pcEnd == pcVal) || ('\0' != *pcEnd) || !isfinite(dVal))
    {
      return DM_FALSE;
    }
    pstVal = yyjson_mut_real(pstDoc, dVal);
  }
  else if (0 == strcmp(pcType, "boolean"))
  {
    if ((0 != strcmp(pcVal, "true")) && (0 != strcmp(pcVal, "false")))
    {
      return DM_FALSE;
    }
    pstVal = yyjson_mut_bool(pstDoc, 0 == strcmp(pcVal, "true"));
  }
  else if ((0 == strcmp(pcType, "object")) || (0 == strcmp(pcType, "array")))
  {
    bool_t bWantObj = (0 == strcmp(pcType, "object")) ? DM_TRUE : DM_FALSE;
    pstJson = yyjson_read(pcVal, strlen(pcVal), YYJSON_READ_NOFLAG);
    if ((NULL == pstJson) || (bWantObj ? !yyjson_is_obj(yyjson_doc_get_root(pstJson))
                                       : !yyjson_is_arr(yyjson_doc_get_root(pstJson))))
    {
      yyjson_doc_free(pstJson);
      return DM_FALSE;
    }
    pstVal = yyjson_val_mut_copy(pstDoc, yyjson_doc_get_root(pstJson));
    yyjson_doc_free(pstJson);
  }
  else
  {
    pstVal = yyjson_mut_strcpy(pstDoc, pcVal);
  }
  return yyjson_mut_obj_add(pstObj, yyjson_mut_strcpy(pstDoc, pcKey), pstVal) ? DM_TRUE : DM_FALSE;
}

int dmcmd_args_build(dm_in const dmcmd_args_req_st *pstReq, dm_out char **ppcBody)
{
  const dmcmd_opt_st *pstOpt = (NULL != pstReq) ? pstReq->pstOpt : NULL;
  const char *pcName = (NULL != pstReq) ? pstReq->pcName : NULL;
  const char *pcSchema = (NULL != pstReq) ? pstReq->pcSchema : NULL;
  int iArgc = (NULL != pstReq) ? pstReq->iArgc : 0;
  char **ppcArgv = (NULL != pstReq) ? pstReq->ppcArgv : NULL;
  yyjson_doc *pstSchemaDoc = NULL;
  yyjson_val *pstSchema = NULL;
  yyjson_val *pstProps = NULL;
  yyjson_val *pstKey = NULL;
  yyjson_obj_iter stIter;
  yyjson_mut_doc *pstDoc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *pstObj = (NULL != pstDoc) ? yyjson_mut_obj(pstDoc) : NULL;
  char acKey[128];
  char acDetail[256];
  int iIdx = 0;
  int iErr = DMCMD_EXIT_OK;

  *ppcBody = NULL;
  if (NULL == pstObj)
  {
    yyjson_mut_doc_free(pstDoc);
    dmcmd_emit_error(pstOpt->bJson, DMC_ERR_QUEUE, "out of memory", NULL);
    return DMCMD_EXIT_ERR;
  }
  yyjson_mut_doc_set_root(pstDoc, pstObj);
  if ((NULL != pcSchema) && ('\0' != pcSchema[0]))
  {
    pstSchemaDoc = yyjson_read(pcSchema, strlen(pcSchema), YYJSON_READ_NOFLAG);
    pstSchema = yyjson_doc_get_root(pstSchemaDoc);
  }
  pstProps = dmcmd__props(pstSchema);

  for (iIdx = 0; (iIdx < iArgc) && (DMCMD_EXIT_OK == iErr); iIdx++)
  {
    const char *pcArg = ppcArgv[iIdx];
    const char *pcEq = NULL;
    const char *pcVal = NULL;
    yyjson_val *pstProp = NULL;
    const char *pcType = NULL;
    size_t uKey = 0u;

    if (0 != strncmp(pcArg, "--", 2))
    {
      snprintf(acDetail, sizeof(acDetail), "%s: unexpected '%s' (arguments are --name value)",
               pcName, pcArg);
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "invalid argument", acDetail);
      iErr = DMCMD_EXIT_USAGE;
      break;
    }
    pcArg += 2;
    pcEq = strchr(pcArg, '=');
    uKey = (NULL != pcEq) ? (size_t)(pcEq - pcArg) : strlen(pcArg);
    if ((0u == uKey) || (uKey >= sizeof(acKey)))
    {
      snprintf(acDetail, sizeof(acDetail), "%s: bad argument '%s'", pcName, ppcArgv[iIdx]);
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "invalid argument", acDetail);
      iErr = DMCMD_EXIT_USAGE;
      break;
    }
    memcpy(acKey, pcArg, uKey);
    acKey[uKey] = '\0';
    pstProp = (NULL != pstProps) ? yyjson_obj_get(pstProps, acKey) : NULL;
    if (NULL == pstProp)
    {
      snprintf(acDetail, sizeof(acDetail), "%s has no argument --%s", pcName, acKey);
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "invalid argument", acDetail);
      iErr = DMCMD_EXIT_USAGE;
      break;
    }
    if (NULL != yyjson_mut_obj_get(pstObj, acKey))
    {
      snprintf(acDetail, sizeof(acDetail), "%s: --%s given twice", pcName, acKey);
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "invalid argument", acDetail);
      iErr = DMCMD_EXIT_USAGE;
      break;
    }
    pcType = dmcmd__type(pstProp);
    if (NULL != pcEq)
    {
      pcVal = pcEq + 1;
    }
    else if ((0 == strcmp(pcType, "boolean")) &&
             ((iIdx + 1 >= iArgc) || (0 == strncmp(ppcArgv[iIdx + 1], "--", 2))))
    {
      pcVal = "true";
    }
    else if (iIdx + 1 < iArgc)
    {
      iIdx++;
      pcVal = ppcArgv[iIdx];
    }
    else
    {
      snprintf(acDetail, sizeof(acDetail), "%s: --%s needs a value", pcName, acKey);
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "invalid argument", acDetail);
      iErr = DMCMD_EXIT_USAGE;
      break;
    }
    if (!dmcmd__put(pstDoc, pstObj, acKey, pcType, pcVal))
    {
      snprintf(acDetail, sizeof(acDetail), "%s: --%s expects %s, got '%s'", pcName, acKey, pcType,
               pcVal);
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "invalid argument", acDetail);
      iErr = DMCMD_EXIT_USAGE;
    }
  }

  if ((DMCMD_EXIT_OK == iErr) && (NULL != pstProps))
  {
    (void)yyjson_obj_iter_init(pstProps, &stIter);
    while (NULL != (pstKey = yyjson_obj_iter_next(&stIter)))
    {
      const char *pcKey = yyjson_get_str(pstKey);
      if ((NULL != pcKey) && dmcmd__required(pstSchema, pcKey) &&
          (NULL == yyjson_mut_obj_get(pstObj, pcKey)))
      {
        snprintf(acDetail, sizeof(acDetail), "%s: missing required argument --%s", pcName, pcKey);
        dmcmd_emit_error(pstOpt->bJson, DMC_ERR_ARG, "invalid argument", acDetail);
        iErr = DMCMD_EXIT_USAGE;
        break;
      }
    }
  }
  if (DMCMD_EXIT_OK == iErr)
  {
    *ppcBody = yyjson_mut_write(pstDoc, YYJSON_WRITE_NOFLAG, NULL);
    if (NULL == *ppcBody)
    {
      dmcmd_emit_error(pstOpt->bJson, DMC_ERR_QUEUE, "failed to serialize request body", pcName);
      iErr = DMCMD_EXIT_ERR;
    }
  }
  yyjson_mut_doc_free(pstDoc);
  yyjson_doc_free(pstSchemaDoc);
  return iErr;
}

void dmcmd_args_help(dm_io FILE *pstOut, dm_in const char *pcName, dm_in const char *pcDesc,
                     dm_in const char *pcSchema)
{
  yyjson_doc *pstDoc = NULL;
  yyjson_val *pstSchema = NULL;
  yyjson_val *pstProps = NULL;
  yyjson_val *pstKey = NULL;
  yyjson_obj_iter stIter;

  fprintf(pstOut, "%s - %s\n", pcName, (NULL != pcDesc) ? pcDesc : "");
  if ((NULL != pcSchema) && ('\0' != pcSchema[0]))
  {
    pstDoc = yyjson_read(pcSchema, strlen(pcSchema), YYJSON_READ_NOFLAG);
    pstSchema = yyjson_doc_get_root(pstDoc);
  }
  pstProps = dmcmd__props(pstSchema);
  if ((NULL == pstProps) || (0u == yyjson_obj_size(pstProps)))
  {
    fprintf(pstOut, "  (no arguments)\n");
  }
  else
  {
    fprintf(pstOut, "arguments:\n");
    (void)yyjson_obj_iter_init(pstProps, &stIter);
    while (NULL != (pstKey = yyjson_obj_iter_next(&stIter)))
    {
      yyjson_val *pstProp = yyjson_obj_iter_get_val(pstKey);
      const char *pcKey = yyjson_get_str(pstKey);
      const char *pcHint = yyjson_get_str(yyjson_obj_get(pstProp, "description"));
      fprintf(pstOut, "  --%-16s %-8s %-9s %s\n", pcKey, dmcmd__type(pstProp),
              dmcmd__required(pstSchema, pcKey) ? "required" : "optional",
              (NULL != pcHint) ? pcHint : "");
    }
  }
  yyjson_doc_free(pstDoc);
  return;
}

int dmcmd_read_http_port(dm_in const char *pcDataDir)
{
  char acPath[768];
  char *pcText = NULL;
  size_t uLen = 0u;
  yyjson_doc *pstDoc = NULL;
  int iHttp = -1;

  snprintf(acPath, sizeof(acPath), "%s/dm.run",
           ((NULL != pcDataDir) && ('\0' != pcDataDir[0])) ? pcDataDir : ".");
  pcText = dmc_read_file_utf8(acPath, &uLen);
  if (NULL == pcText)
  {
    return -1;
  }
  pstDoc = yyjson_read(pcText, uLen, YYJSON_READ_NOFLAG);
  dmcmd__zero_mem(pcText, uLen);
  free(pcText);
  if (NULL != pstDoc)
  {
    int iVal = (int)yyjson_get_int(yyjson_obj_get(yyjson_doc_get_root(pstDoc), "http"));
    if ((iVal > 0) && (iVal <= 65535))
    {
      iHttp = iVal;
    }
    yyjson_doc_free(pstDoc);
  }
  return iHttp;
}

#ifdef __cplusplus
}
#endif
