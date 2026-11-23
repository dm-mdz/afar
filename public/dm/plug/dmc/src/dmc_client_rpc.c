/**
 ******************************************************************************
 * @file    dmc_client_rpc.c
 * @brief   dmc 上层会话 SDK：dm.run 读取、IFACE_LIST 缓存与按名调用、轮询心跳（DD-02 §2.4）。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "dmc_client_int.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

uint64 dmc_now_ms(void)
{
#ifdef _WIN32
  return (uint64)GetTickCount64();
#else
  struct timespec stTs;
  (void)clock_gettime(CLOCK_MONOTONIC, &stTs);
  return (uint64)stTs.tv_sec * 1000u + (uint64)stTs.tv_nsec / 1000000u;
#endif
}

char *dmc_read_file_utf8(in const char *pcPath, out size_t *puLen)
{
  FILE *pstFp = NULL;
  char *pcText = NULL;
  long lSize = 0;

  if (NULL != puLen)
  {
    *puLen = 0u;
  }
  if ((NULL == pcPath) || ('\0' == pcPath[0]))
  {
    return NULL;
  }
#ifdef _WIN32
  {
    wchar_t awcPath[1024];
    if (0 == MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pcPath, -1, awcPath, 1024))
    {
      return NULL;
    }
    pstFp = _wfopen(awcPath, L"rb");
  }
#else
  pstFp = fopen(pcPath, "rb");
#endif
  if (NULL == pstFp)
  {
    return NULL;
  }
  if ((0 == fseek(pstFp, 0, SEEK_END)) && ((lSize = ftell(pstFp)) >= 0) &&
      (lSize <= 16L * 1024L * 1024L) && (0 == fseek(pstFp, 0, SEEK_SET)))
  {
    pcText = (char *)malloc((size_t)lSize + 1u);
    if (NULL != pcText)
    {
      size_t uRead = fread(pcText, 1u, (size_t)lSize, pstFp);
      pcText[uRead] = '\0';
      if (NULL != puLen)
      {
        *puLen = uRead;
      }
    }
  }
  (void)fclose(pstFp);
  return pcText;
}

static void dmc__sleep_ms(in uint32 u32Ms)
{
  if (0u == u32Ms)
  {
    return;
  }
#ifdef _WIN32
  Sleep(u32Ms);
#else
  {
    struct timespec stReq;
    stReq.tv_sec = (time_t)(u32Ms / 1000u);
    stReq.tv_nsec = (long)((u32Ms % 1000u) * 1000000u);
    (void)nanosleep(&stReq, NULL);
  }
#endif
  return;
}

int dmc__read_run_file(in const char *pcDataDir, in const char *pcWantRole, out int *piLink,
                       out int *piHttp, out char *pcBoot, in size_t uBootCap, out char *pcRole,
                       in size_t uRoleCap, out char *pcToken, in size_t uTokenCap)
{
  char acPath[768];
  char *pcText = NULL;
  size_t uText = 0u;
  yyjson_doc *pstDoc = NULL;
  yyjson_val *pstRoot = NULL;
  yyjson_val *pstTok = NULL;
  yyjson_val *pstKey = NULL;
  yyjson_obj_iter stIter;
  const char *pcBootVal = NULL;
  bool_t bFound = DM_FALSE;

  *piLink = 0;
  *piHttp = 0;
  pcBoot[0] = '\0';
  pcRole[0] = '\0';
  pcToken[0] = '\0';

  snprintf(acPath, sizeof(acPath), "%s/dm.run",
           ((NULL != pcDataDir) && ('\0' != pcDataDir[0])) ? pcDataDir : ".");
  pcText = dmc_read_file_utf8(acPath, &uText);
  if (NULL == pcText)
  {
    return DMC_ERR_NOT_CON;
  }
  pstDoc = yyjson_read(pcText, uText, YYJSON_READ_NOFLAG);
  dmc__zero_mem(pcText, uText);
  free(pcText);
  pstRoot = yyjson_doc_get_root(pstDoc);
  if (!yyjson_is_obj(pstRoot))
  {
    yyjson_doc_free(pstDoc);
    return DMC_ERR_NOT_CON;
  }
  *piLink = (int)yyjson_get_int(yyjson_obj_get(pstRoot, "link"));
  *piHttp = (int)yyjson_get_int(yyjson_obj_get(pstRoot, "http"));
  pcBootVal = yyjson_get_str(yyjson_obj_get(pstRoot, "boot"));
  if (NULL != pcBootVal)
  {
    dmc__copy_str(pcBoot, uBootCap, pcBootVal);
  }
  pstTok = yyjson_obj_get(pstRoot, "tokens");
  if (yyjson_is_obj(pstTok))
  {
    const char *pcTargetRole =
        ((NULL != pcWantRole) && ('\0' != pcWantRole[0])) ? pcWantRole : "cli";
    yyjson_obj_iter_init(pstTok, &stIter);
    while (NULL != (pstKey = yyjson_obj_iter_next(&stIter)))
    {
      const char *pcName = yyjson_get_str(pstKey);
      const char *pcVal = yyjson_get_str(yyjson_obj_iter_get_val(pstKey));
      if ((NULL == pcName) || (NULL == pcVal))
      {
        continue;
      }
      if (0 == strcmp(pcName, pcTargetRole))
      {
        dmc__copy_str(pcRole, uRoleCap, pcName);
        dmc__copy_str(pcToken, uTokenCap, pcVal);
        bFound = DM_TRUE;
        break;
      }
    }
    if (!bFound && ((NULL == pcWantRole) || ('\0' == pcWantRole[0])))
    {
      yyjson_obj_iter_init(pstTok, &stIter);
      while (NULL != (pstKey = yyjson_obj_iter_next(&stIter)))
      {
        const char *pcName = yyjson_get_str(pstKey);
        const char *pcVal = yyjson_get_str(yyjson_obj_iter_get_val(pstKey));
        if ((NULL != pcName) && (NULL != pcVal))
        {
          dmc__copy_str(pcRole, uRoleCap, pcName);
          dmc__copy_str(pcToken, uTokenCap, pcVal);
          break;
        }
      }
    }
  }
  yyjson_doc_free(pstDoc);
  return (*piLink > 0) ? DMC_OK : DMC_ERR_NOT_CON;
}

static bool_t dmc__builtin_id(in const char *pcName, out uint16 *pu16ID)
{
  static const struct
  {
    const char *pcName;
    uint16 u16ID;
  } astMap[] = {
      {"dm.iface.list", DM_LINKID_IFACE_LIST},
      {"dm.log.recent", DM_LINKID_LOG_RECENT},
      {"dm.stat", DM_LINKID_STAT},
      {"dm.ping", DM_LINKID_PING},
      {"dm.log.level", DM_LINKID_LOG_LEVEL},
      {"dm.diag.snapshot", DM_LINKID_DIAG_SNAPSHOT},
  };
  uint uiIdx = 0u;
  for (uiIdx = 0u; uiIdx < (uint)(sizeof(astMap) / sizeof(astMap[0])); uiIdx++)
  {
    if (0 == strcmp(astMap[uiIdx].pcName, pcName))
    {
      *pu16ID = astMap[uiIdx].u16ID;
      return DM_TRUE;
    }
  }
  return DM_FALSE;
}

const char *dmc_client_ifaces_json(io dmc_client_st *pstCli, in bool_t bRefresh)
{
  dmc_resp_st stResp;
  int iErr = DMC_OK;

  if (NULL == pstCli)
  {
    return NULL;
  }
  if (!bRefresh && (NULL != pstCli->pcIfacesJson))
  {
    return pstCli->pcIfacesJson;
  }
  iErr = dmc_client_call_id(pstCli, DM_LINKID_IFACE_LIST, 0u, NULL, 0u, pstCli->u32DefaultTimeoutMs,
                            &stResp);
  if ((DMC_OK != iErr) || (NULL == stResp.pcBody))
  {
    dmc_resp_free(&stResp);
    return NULL;
  }
  dmc__cli_clear_ifaces(pstCli);
  pstCli->pcIfacesJson = stResp.pcBody;
  pstCli->pstIfacesDoc = yyjson_read(pstCli->pcIfacesJson, stResp.u32Len, YYJSON_READ_NOFLAG);
  return pstCli->pcIfacesJson;
}

static bool_t dmc__lookup_iface_id(io dmc_client_st *pstCli, in const char *pcName,
                                   in bool_t bRefresh, out uint16 *pu16ID)
{
  yyjson_val *pstArr = NULL;
  yyjson_val *pstOne = NULL;
  yyjson_arr_iter stIter;

  if (NULL == dmc_client_ifaces_json(pstCli, bRefresh))
  {
    return DM_FALSE;
  }
  pstArr = yyjson_obj_get(yyjson_doc_get_root(pstCli->pstIfacesDoc), "ifaces");
  if (!yyjson_is_arr(pstArr))
  {
    return DM_FALSE;
  }
  yyjson_arr_iter_init(pstArr, &stIter);
  while (NULL != (pstOne = yyjson_arr_iter_next(&stIter)))
  {
    if (yyjson_equals_str(yyjson_obj_get(pstOne, "name"), pcName))
    {
      yyjson_val *pstId = yyjson_obj_get(pstOne, "id");
      if (yyjson_is_uint(pstId) && (yyjson_get_uint(pstId) <= 65535u))
      {
        *pu16ID = (uint16)yyjson_get_uint(pstId);
        return DM_TRUE;
      }
      return DM_FALSE;
    }
  }
  return DM_FALSE;
}

int dmc_client_call_id(io dmc_client_st *pstCli, in uint16 u16LinkID, in uint8 u8Flag,
                       in const void *pvBody, in uint32 u32Len, in uint32 u32TimeoutMs,
                       out dmc_resp_st *pstResp)
{
  int iErr = DMC_OK;

  if ((NULL == pstCli) || (NULL == pstResp))
  {
    return DMC_ERR_ARG;
  }
  memset(pstResp, 0, sizeof(*pstResp));
  if (!dmc_connected(pstCli->pstDmc))
  {
    if (!pstCli->bAutoReconnect || (DMC_OK != (iErr = dmc__cli_connect_once(pstCli))))
    {
      pstResp->iStatus = (DMC_OK != iErr) ? iErr : DMC_ERR_NOT_CON;
      return pstResp->iStatus;
    }
  }
  iErr = dmc__cli_call_raw(pstCli, u16LinkID, u8Flag, pvBody, u32Len, u32TimeoutMs, pstResp);
  if ((DMC_ERR_NOT_CON == iErr) && pstCli->bAutoReconnect)
  {
    dmc_resp_free(pstResp);
    iErr = dmc__cli_connect_once(pstCli);
    if (DMC_OK != iErr)
    {
      pstResp->iStatus = iErr;
      return iErr;
    }
    iErr = dmc__cli_call_raw(pstCli, u16LinkID, u8Flag, pvBody, u32Len, u32TimeoutMs, pstResp);
  }
  return iErr;
}

int dmc_client_call_name(io dmc_client_st *pstCli, in const char *pcName, in const char *pcJsonBody,
                         in uint32 u32TimeoutMs, out dmc_resp_st *pstResp)
{
  uint16 u16LinkID = 0u;
  uint32 u32Len = (NULL != pcJsonBody) ? (uint32)strlen(pcJsonBody) : 0u;
  uint8 u8Flag = (u32Len > 0u) ? DM_FRAME_F_JSON : 0u;
  int iErr = DMC_OK;

  if ((NULL == pstCli) || (NULL == pcName) || ('\0' == pcName[0]) || (NULL == pstResp))
  {
    return DMC_ERR_ARG;
  }
  memset(pstResp, 0, sizeof(*pstResp));
  if (dmc__builtin_id(pcName, &u16LinkID))
  {
    return dmc_client_call_id(pstCli, u16LinkID, u8Flag, pcJsonBody, u32Len, u32TimeoutMs, pstResp);
  }
  if (!dmc__lookup_iface_id(pstCli, pcName, DM_FALSE, &u16LinkID) &&
      !dmc__lookup_iface_id(pstCli, pcName, DM_TRUE, &u16LinkID))
  {
    pstResp->iStatus = dmc_connected(pstCli->pstDmc) ? DMC_ERR_NOT_FOUND : DMC_ERR_NOT_CON;
    return pstResp->iStatus;
  }
  iErr = dmc_client_call_id(pstCli, u16LinkID, u8Flag, pcJsonBody, u32Len, u32TimeoutMs, pstResp);
  if (DM__LINK_ERR_NOT_FOUND == iErr)
  {
    if (dmc__lookup_iface_id(pstCli, pcName, DM_TRUE, &u16LinkID))
    {
      dmc_resp_free(pstResp);
      iErr =
          dmc_client_call_id(pstCli, u16LinkID, u8Flag, pcJsonBody, u32Len, u32TimeoutMs, pstResp);
    }
  }
  return iErr;
}

int dmc_client_poll(io dmc_client_st *pstCli, in uint32 u32WaitMs)
{
  uint64 u64Now = 0u;
  int iErr = DMC_OK;

  if (NULL == pstCli)
  {
    return DMC_ERR_ARG;
  }
  if (!dmc_connected(pstCli->pstDmc))
  {
    if (!pstCli->bAutoReconnect)
    {
      return DMC_ERR_NOT_CON;
    }
    u64Now = dmc_now_ms();
    if ((0u != pstCli->u64LastReconnMs) && (u64Now - pstCli->u64LastReconnMs < 200u))
    {
      uint32 u32Rem = (uint32)(200u - (u64Now - pstCli->u64LastReconnMs));
      uint32 u32Nap = (u32WaitMs < u32Rem) ? u32WaitMs : u32Rem;
      dmc__sleep_ms(u32Nap);
      return DMC_ERR_NOT_CON;
    }
    pstCli->u64LastReconnMs = u64Now;
    iErr = dmc__cli_connect_once(pstCli);
    if (DMC_OK != iErr)
    {
      return iErr;
    }
  }
  iErr = dmc__cli_pump_once(pstCli, (u32WaitMs > (uint32)INT_MAX) ? INT_MAX : (int)u32WaitMs);
  if (DMC_OK != iErr)
  {
    return iErr;
  }
  u64Now = dmc_now_ms();
  if ((pstCli->u32PingIntervalMs > 0u) &&
      (u64Now - pstCli->u64LastPingMs >= pstCli->u32PingIntervalMs))
  {
    dmc_resp_st stPing;
    pstCli->u64LastPingMs = u64Now;
    iErr = dmc__cli_call_raw(pstCli, DM_LINKID_PING, 0u, NULL, 0u, pstCli->u32ConnectTimeoutMs,
                             &stPing);
    dmc_resp_free(&stPing);
    if (DMC_OK != iErr)
    {
      dmc__cli_drop_link(pstCli);
      return DMC_ERR_NOT_CON;
    }
  }
  return DMC_OK;
}

uint64 dmc_client_last_seq(in const dmc_client_st *pstCli)
{
  return (NULL != pstCli) ? pstCli->u64LastSeq : 0u;
}

const char *dmc_client_boot(in const dmc_client_st *pstCli)
{
  return (NULL != pstCli) ? pstCli->acBoot : "";
}

int dmc_client_http_port(in const dmc_client_st *pstCli)
{
  return (NULL != pstCli) ? pstCli->iHttpPort : 0;
}

void dmc_resp_free(io dmc_resp_st *pstResp)
{
  if (NULL == pstResp)
  {
    return;
  }
  free(pstResp->pcBody);
  pstResp->pcBody = NULL;
  pstResp->u32Len = 0u;
  return;
}

#ifdef __cplusplus
}
#endif
