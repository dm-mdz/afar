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

#include <dm/rpc.h>

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

char *dmc_read_file_utf8(dm_in const char *pcPath, dm_out size_t *puLen)
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
  /* 禁用 stdio 内部缓冲，防止 dm.run / 密钥配置文件明文残留于 CRT 堆缓冲 */
  (void)setvbuf(pstFp, NULL, _IONBF, 0u);
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

static void dmc__sleep_ms(dm_in uint32 u32Ms)
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

int dmc__read_run_file(dm_in const char *pcDataDir, dm_in const char *pcWantRole,
                       dm_out dmc__run_info_st *pstOut)
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

  if (NULL == pstOut)
  {
    return DMC_ERR_ARG;
  }
  memset(pstOut, 0, sizeof(*pstOut));

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
  pstOut->iLinkPort = (int)yyjson_get_int(yyjson_obj_get(pstRoot, "link"));
  pstOut->iHttpPort = (int)yyjson_get_int(yyjson_obj_get(pstRoot, "http"));
  pcBootVal = yyjson_get_str(yyjson_obj_get(pstRoot, "boot"));
  if (NULL != pcBootVal)
  {
    dmc__copy_str(pstOut->acBoot, sizeof(pstOut->acBoot), pcBootVal);
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
        dmc__copy_str(pstOut->acRole, sizeof(pstOut->acRole), pcName);
        dmc__copy_str(pstOut->acToken, sizeof(pstOut->acToken), pcVal);
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
          dmc__copy_str(pstOut->acRole, sizeof(pstOut->acRole), pcName);
          dmc__copy_str(pstOut->acToken, sizeof(pstOut->acToken), pcVal);
          break;
        }
      }
    }
  }
  yyjson_doc_free(pstDoc);
  return (pstOut->iLinkPort > 0) ? DMC_OK : DMC_ERR_NOT_CON;
}

static bool_t dmc__builtin_id(dm_in const char *pcName, dm_out uint16 *pu16ID)
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

const char *dmc_client_ifaces_json(dm_io dmc_client_st *pstCli, dm_in bool_t bRefresh)
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
  {
    dmc_call_id_req_st stReq = {DM_LINKID_IFACE_LIST, 0u, NULL, 0u, pstCli->u32DefaultTimeoutMs};
    iErr = dmc_client_call_id(pstCli, &stReq, &stResp);
  }
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

static bool_t dmc__lookup_iface_meta(dm_io dmc_client_st *pstCli, dm_in const char *pcName,
                                     dm_in bool_t bRefresh, dm_out uint16 *pu16ID,
                                     dm_out uint32 *pu32Flags)
{
  yyjson_val *pstArr = NULL;
  yyjson_val *pstOne = NULL;
  yyjson_arr_iter stIter;

  if (NULL != pu32Flags)
  {
    *pu32Flags = 0u;
  }
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
        if (NULL != pu32Flags)
        {
          *pu32Flags = (uint32)yyjson_get_uint(yyjson_obj_get(pstOne, "flags"));
        }
        return DM_TRUE;
      }
      return DM_FALSE;
    }
  }
  return DM_FALSE;
}

int dmc_client_call_id(dm_io dmc_client_st *pstCli, dm_in const dmc_call_id_req_st *pstReq,
                       dm_out dmc_resp_st *pstResp)
{
  int iErr = DMC_OK;

  if ((NULL == pstCli) || (NULL == pstReq) || (NULL == pstResp))
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
  iErr = dmc__cli_call_raw(pstCli, pstReq, pstResp);
  if ((DMC_ERR_NOT_CON == iErr) && pstCli->bAutoReconnect)
  {
    dmc_resp_free(pstResp);
    iErr = dmc__cli_connect_once(pstCli);
    if (DMC_OK != iErr)
    {
      pstResp->iStatus = iErr;
      return iErr;
    }
    iErr = dmc__cli_call_raw(pstCli, pstReq, pstResp);
  }
  return iErr;
}

int dmc_client_call_name_ex(dm_io dmc_client_st *pstCli, dm_in const dmc_call_name_req_st *pstReq,
                            dm_out dmc_resp_st *pstResp)
{
  uint16 u16LinkID = 0u;
  uint32 u32IfaceFlags = 0u;
  uint32 u32Len = 0u;
  uint8 u8Flag = 0u;
  dmc_call_id_req_st stIdReq;
  int iErr = DMC_OK;

  if ((NULL == pstCli) || (NULL == pstReq) || (NULL == pstReq->pcName) ||
      ('\0' == pstReq->pcName[0]) || (NULL == pstResp))
  {
    return DMC_ERR_ARG;
  }
  u32Len = (NULL != pstReq->pcJsonBody) ? (uint32)strlen(pstReq->pcJsonBody) : 0u;
  u8Flag = (uint8)(((u32Len > 0u) ? DM_FRAME_F_JSON : 0u) |
                   (pstReq->u8ExtraFlags & DM_FRAME_F_SENSITIVE));
  memset(pstResp, 0, sizeof(*pstResp));
  memset(&stIdReq, 0, sizeof(stIdReq));
  stIdReq.pvBody = pstReq->pcJsonBody;
  stIdReq.u32Len = u32Len;
  stIdReq.u32TimeoutMs = pstReq->u32TimeoutMs;
  if (dmc__builtin_id(pstReq->pcName, &u16LinkID))
  {
    stIdReq.u16LinkID = u16LinkID;
    stIdReq.u8Flag = u8Flag;
    return dmc_client_call_id(pstCli, &stIdReq, pstResp);
  }
  if (!dmc__lookup_iface_meta(pstCli, pstReq->pcName, DM_FALSE, &u16LinkID, &u32IfaceFlags) &&
      !dmc__lookup_iface_meta(pstCli, pstReq->pcName, DM_TRUE, &u16LinkID, &u32IfaceFlags))
  {
    pstResp->iStatus = dmc_connected(pstCli->pstDmc) ? DMC_ERR_NOT_FOUND : DMC_ERR_NOT_CON;
    return pstResp->iStatus;
  }
  if (0u != (u32IfaceFlags & DM_IFACE_SENSITIVE))
  {
    u8Flag |= DM_FRAME_F_SENSITIVE;
  }
  stIdReq.u16LinkID = u16LinkID;
  stIdReq.u8Flag = u8Flag;
  iErr = dmc_client_call_id(pstCli, &stIdReq, pstResp);
  if (DM__LINK_ERR_NOT_FOUND == iErr)
  {
    if (dmc__lookup_iface_meta(pstCli, pstReq->pcName, DM_TRUE, &u16LinkID, &u32IfaceFlags))
    {
      if (0u != (u32IfaceFlags & DM_IFACE_SENSITIVE))
      {
        u8Flag |= DM_FRAME_F_SENSITIVE;
      }
      dmc_resp_free(pstResp);
      stIdReq.u16LinkID = u16LinkID;
      stIdReq.u8Flag = u8Flag;
      iErr = dmc_client_call_id(pstCli, &stIdReq, pstResp);
    }
  }
  return iErr;
}

int dmc_client_call_name(dm_io dmc_client_st *pstCli, dm_in const char *pcName,
                         dm_in const char *pcJsonBody, dm_in uint32 u32TimeoutMs,
                         dm_out dmc_resp_st *pstResp)
{
  dmc_call_name_req_st stReq = {pcName, 0u, pcJsonBody, u32TimeoutMs};
  return dmc_client_call_name_ex(pstCli, &stReq, pstResp);
}

int dmc_client_poll(dm_io dmc_client_st *pstCli, dm_in uint32 u32WaitMs)
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
    dmc_call_id_req_st stPingReq = {DM_LINKID_PING, 0u, NULL, 0u, pstCli->u32ConnectTimeoutMs};
    pstCli->u64LastPingMs = u64Now;
    iErr = dmc__cli_call_raw(pstCli, &stPingReq, &stPing);
    dmc_resp_free(&stPing);
    if (DMC_OK != iErr)
    {
      dmc__cli_drop_link(pstCli);
      return DMC_ERR_NOT_CON;
    }
  }
  return DMC_OK;
}

uint64 dmc_client_last_seq(dm_in const dmc_client_st *pstCli)
{
  return (NULL != pstCli) ? pstCli->u64LastSeq : 0u;
}

const char *dmc_client_boot(dm_in const dmc_client_st *pstCli)
{
  return (NULL != pstCli) ? pstCli->acBoot : "";
}

int dmc_client_http_port(dm_in const dmc_client_st *pstCli)
{
  return (NULL != pstCli) ? pstCli->iHttpPort : 0;
}

void dmc_resp_free(dm_io dmc_resp_st *pstResp)
{
  if (NULL == pstResp)
  {
    return;
  }
  if ((NULL != pstResp->pcBody) && (0u != (pstResp->u8Flag & DM_FRAME_F_SENSITIVE)) &&
      (pstResp->u32Len > 0u))
  {
    dmc__zero_mem(pstResp->pcBody, pstResp->u32Len);
  }
  free(pstResp->pcBody);
  pstResp->pcBody = NULL;
  pstResp->u32Len = 0u;
  return;
}

#ifdef __cplusplus
}
#endif
