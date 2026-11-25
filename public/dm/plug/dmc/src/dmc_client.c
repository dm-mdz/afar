/**
 ******************************************************************************
 * @file    dmc_client.c
 * @brief   dmc 上层连接会话 SDK 核心连接与握手实现（DD-02 §2.4，PLUG-01/03）：
 *          负责 dmc_client_open/close、传输连接、HELLO 握手与基础帧泵。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "dmc_client_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define dmc__pid() ((unsigned long)_getpid())
#else
#include <unistd.h>
#define dmc__pid() ((unsigned long)getpid())
#endif

typedef struct dmc__wait
{
  bool_t bDone;
  int iStatus;
  uint8 u8Flag;
  char *pcBody;
  uint32 u32Len;
} dmc__wait_st;

void dmc__zero_mem(dm_out void *pvBuf, dm_in size_t uLen)
{
  volatile uint8 *pu8 = (volatile uint8 *)pvBuf;
  size_t uIdx = 0u;
  for (uIdx = 0u; uIdx < uLen; uIdx++)
  {
    pu8[uIdx] = 0u;
  }
  return;
}

void dmc__copy_str(dm_out char *pcDst, dm_in size_t uCap, dm_in const char *pcSrc)
{
  size_t uLen = 0u;
  if ((NULL == pcDst) || (0u == uCap))
  {
    return;
  }
  if (NULL == pcSrc)
  {
    pcDst[0] = '\0';
    return;
  }
  uLen = strlen(pcSrc);
  if (uLen >= uCap)
  {
    uLen = uCap - 1u;
  }
  memcpy(pcDst, pcSrc, uLen);
  pcDst[uLen] = '\0';
  return;
}

static unsigned char dmc__ascii_lo(dm_in unsigned char u8Ch)
{
  return ((u8Ch >= 'A') && (u8Ch <= 'Z')) ? (unsigned char)(u8Ch - 'A' + 'a') : u8Ch;
}

static bool_t dmc__str_eq_ci(dm_in const char *pcA, dm_in const char *pcB)
{
  size_t uIdx = 0u;
  if ((NULL == pcA) || (NULL == pcB))
  {
    return DM_FALSE;
  }
  while (('\0' != pcA[uIdx]) && ('\0' != pcB[uIdx]))
  {
    if (dmc__ascii_lo((unsigned char)pcA[uIdx]) != dmc__ascii_lo((unsigned char)pcB[uIdx]))
    {
      return DM_FALSE;
    }
    uIdx++;
  }
  return (pcA[uIdx] == pcB[uIdx]) ? DM_TRUE : DM_FALSE;
}

static bool_t dmc__extract_scheme(dm_in const char *pcTarget, dm_out char *pcScheme,
                                  dm_in size_t uCap)
{
  const char *pcSep = NULL;
  size_t uLen = 0u;

  if ((NULL == pcTarget) || ('\0' == pcTarget[0]))
  {
    dmc__copy_str(pcScheme, uCap, "local");
    return DM_TRUE;
  }
  pcSep = strstr(pcTarget, "://");
  if (NULL == pcSep)
  {
    return DM_FALSE;
  }
  uLen = (size_t)(pcSep - pcTarget);
  if ((0u == uLen) || (uLen >= uCap))
  {
    return DM_FALSE;
  }
  memcpy(pcScheme, pcTarget, uLen);
  pcScheme[uLen] = '\0';
  return DM_TRUE;
}

/* ============================ dmc_proto 宿主回调 ============================ */

static int dmc__cli_send(dm_in void *pvUser, dm_in const uint8 *pu8Head, dm_in const void *pvBody,
                         dm_in uint32 u32Len)
{
  dmc_client_st *pstCli = (dmc_client_st *)pvUser;
  if ((NULL == pstCli) || !pstCli->bHasVtbl || (NULL == pstCli->pvHandle))
  {
    return -1;
  }
  if (0 != pstCli->stVtbl.pfWrite(pstCli->pvHandle, pu8Head, DM_FRAME_HEAD))
  {
    return -1;
  }
  return (0u == u32Len) ? 0 : pstCli->stVtbl.pfWrite(pstCli->pvHandle, pvBody, u32Len);
}

static void dmc__cli_on_push(dm_in void *pvUser, dm_in const dmc_push_st *pstPush)
{
  dmc_client_st *pstCli = (dmc_client_st *)pvUser;
  if ((NULL == pstCli) || (NULL == pstPush))
  {
    return;
  }
  if ((0u != pstPush->u64Seq) && (pstPush->u64Seq > pstCli->u64LastSeq))
  {
    pstCli->u64LastSeq = pstPush->u64Seq;
    pstCli->bHasSinceSeq = DM_TRUE;
  }
  if (NULL != pstCli->pfOnPush)
  {
    pstCli->pfOnPush(pstCli->pvUser, pstPush);
  }
  return;
}

static void dmc__cli_on_call(dm_in void *pvUser, dm_in uint16 u16Func, dm_in uint32 u32ReqID,
                             dm_in const uint8 *pu8Body, dm_in uint32 u32Len)
{
  dmc_client_st *pstCli = (dmc_client_st *)pvUser;
  if ((NULL != pstCli) && (NULL != pstCli->pfOnCall))
  {
    pstCli->pfOnCall(pstCli->pvUser, u16Func, u32ReqID, pu8Body, u32Len);
    return;
  }
  if ((NULL != pstCli) && (0u != u32ReqID))
  {
    dmc_ans_opt_st stAns = {u16Func, u32ReqID, (int32)DM__NOT_FOUND, 0u, NULL, 0u};
    (void)dmc_answer(pstCli->pstDmc, &stAns);
  }
  return;
}

static void dmc__cli_on_wire_err(dm_in void *pvUser, dm_in int iErr)
{
  dmc_client_st *pstCli = (dmc_client_st *)pvUser;
  (void)iErr;
  if ((NULL != pstCli) && (NULL != pstCli->pstDmc))
  {
    dmc_on_disconnected(pstCli->pstDmc);
  }
  return;
}

static void dmc__cli_on_reply(dm_in void *pvArg, dm_in int iStatus, dm_in uint8 u8Flag,
                              dm_in const uint8 *pu8Body, dm_in uint32 u32Len)
{
  dmc__wait_st *pstWait = (dmc__wait_st *)pvArg;

  pstWait->bDone = DM_TRUE;
  pstWait->iStatus = iStatus;
  pstWait->u8Flag = u8Flag;
  pstWait->pcBody = (char *)malloc((size_t)u32Len + 1u);
  if (NULL != pstWait->pcBody)
  {
    if ((u32Len > 0u) && (NULL != pu8Body))
    {
      memcpy(pstWait->pcBody, pu8Body, u32Len);
    }
    pstWait->pcBody[u32Len] = '\0';
    pstWait->u32Len = u32Len;
  }
  else
  {
    pstWait->iStatus = DMC_ERR_QUEUE;
    pstWait->u32Len = 0u;
  }
  return;
}

/* ============================ 底层泵与连接管理 ============================ */

void dmc__cli_drop_link(dm_io dmc_client_st *pstCli)
{
  if (NULL != pstCli->pstDmc)
  {
    dmc_on_disconnected(pstCli->pstDmc);
  }
  if ((NULL != pstCli->pvHandle) && pstCli->bHasVtbl)
  {
    pstCli->stVtbl.pfClose(pstCli->pvHandle);
    pstCli->pvHandle = NULL;
  }
  return;
}

void dmc__cli_clear_ifaces(dm_io dmc_client_st *pstCli)
{
  yyjson_doc_free(pstCli->pstIfacesDoc);
  pstCli->pstIfacesDoc = NULL;
  free(pstCli->pcIfacesJson);
  pstCli->pcIfacesJson = NULL;
  return;
}

int dmc__cli_pump_once(dm_io dmc_client_st *pstCli, dm_in int iWaitMs)
{
  uint8 au8Buf[16384];
  int iN = 0;

  if ((NULL == pstCli->pvHandle) || !pstCli->bHasVtbl || !dmc_connected(pstCli->pstDmc))
  {
    return DMC_ERR_NOT_CON;
  }
  iN = pstCli->stVtbl.pfRead(pstCli->pvHandle, au8Buf, sizeof(au8Buf), iWaitMs);
  if (-2 == iN)
  {
    return DMC_OK;
  }
  if (iN <= 0)
  {
    dmc__cli_drop_link(pstCli);
    return DMC_ERR_NOT_CON;
  }
  (void)dmc_feed(pstCli->pstDmc, au8Buf, (size_t)iN);
  if (!dmc_connected(pstCli->pstDmc))
  {
    dmc__cli_drop_link(pstCli);
    return DMC_ERR_NOT_CON;
  }
  return DMC_OK;
}

int dmc__cli_call_raw(dm_io dmc_client_st *pstCli, dm_in const dmc_call_id_req_st *pstReq,
                      dm_out dmc_resp_st *pstResp)
{
  dmc__wait_st stWait;
  dmc_req_opt_st stOpt;
  uint32 u32ReqID = 0u;
  uint32 u32Ms = (0u != pstReq->u32TimeoutMs) ? pstReq->u32TimeoutMs : pstCli->u32DefaultTimeoutMs;
  uint64 u64End = dmc_now_ms() + (uint64)u32Ms;
  int iErr = DMC_OK;

  memset(&stWait, 0, sizeof(stWait));
  memset(&stOpt, 0, sizeof(stOpt));
  memset(pstResp, 0, sizeof(*pstResp));
  stOpt.u16LinkID = pstReq->u16LinkID;
  stOpt.u8Flag = pstReq->u8Flag;
  stOpt.pvBody = pstReq->pvBody;
  stOpt.u32Len = pstReq->u32Len;
  stOpt.pfReply = dmc__cli_on_reply;
  stOpt.pvArg = &stWait;
  iErr = dmc_request(pstCli->pstDmc, &stOpt, &u32ReqID);
  if (DMC_OK != iErr)
  {
    pstResp->iStatus = iErr;
    return iErr;
  }
  while (!stWait.bDone)
  {
    uint64 u64Now = dmc_now_ms();
    int iWait = 0;
    if (u64Now >= u64End)
    {
      (void)dmc_cancel(pstCli->pstDmc, u32ReqID, DMC_ERR_TIMEOUT);
      break;
    }
    iWait = ((u64End - u64Now) > 1000u) ? 1000 : (int)(u64End - u64Now);
    if (DMC_OK != dmc__cli_pump_once(pstCli, iWait))
    {
      break;
    }
  }
  pstResp->iStatus = stWait.bDone ? stWait.iStatus : DMC_ERR_NOT_CON;
  pstResp->u8Flag = stWait.u8Flag;
  pstResp->pcBody = stWait.pcBody;
  pstResp->u32Len = stWait.u32Len;
  return pstResp->iStatus;
}

static int dmc__cli_handshake(dm_io dmc_client_st *pstCli, dm_in const char *pcRole,
                              dm_in const char *pcToken)
{
  yyjson_mut_doc *pstDoc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *pstRoot = NULL;
  char *pcHello = NULL;
  size_t uHello = 0u;
  dmc_resp_st stResp;
  int iErr = DMC_OK;
  bool bOk = true;

  if (NULL == pstDoc)
  {
    return DMC_ERR_QUEUE;
  }
  pstRoot = yyjson_mut_obj(pstDoc);
  bOk = (NULL != pstRoot);
  if (bOk)
  {
    yyjson_mut_doc_set_root(pstDoc, pstRoot);
    bOk = bOk && yyjson_mut_obj_add_int(pstDoc, pstRoot, "v", 1);
    bOk = bOk && yyjson_mut_obj_add_str(pstDoc, pstRoot, "role", pcRole);
    bOk = bOk && yyjson_mut_obj_add_str(pstDoc, pstRoot, "token", pcToken);
    bOk = bOk && yyjson_mut_obj_add_str(pstDoc, pstRoot, "peer_id", pstCli->acPeerID);
    if (bOk && pstCli->bWantPush)
    {
      yyjson_mut_val *pstArr = yyjson_mut_obj_add_arr(pstDoc, pstRoot, "push");
      bOk = (NULL != pstArr);
      if (bOk && (0u == pstCli->uiPushTopicCount))
      {
        bOk = bOk && yyjson_mut_arr_add_str(pstDoc, pstArr, "*");
      }
      else
      {
        uint uiIdx = 0u;
        for (uiIdx = 0u; bOk && (uiIdx < pstCli->uiPushTopicCount); uiIdx++)
        {
          bOk = bOk && yyjson_mut_arr_add_str(pstDoc, pstArr, pstCli->aacPushTopics[uiIdx]);
        }
      }
    }
    if (bOk && pstCli->bHasSinceSeq)
    {
      if ('\0' != pstCli->acBoot[0])
      {
        bOk = bOk && yyjson_mut_obj_add_str(pstDoc, pstRoot, "boot", pstCli->acBoot);
      }
      bOk = bOk && yyjson_mut_obj_add_uint(pstDoc, pstRoot, "seq", pstCli->u64LastSeq);
    }
  }
  if (bOk)
  {
    pcHello = yyjson_mut_write(pstDoc, YYJSON_WRITE_NOFLAG, &uHello);
  }
  yyjson_mut_doc_free(pstDoc);
  if (NULL == pcHello)
  {
    return DMC_ERR_QUEUE;
  }
  {
    dmc_call_id_req_st stReq = {DM_LINKID_HELLO, (uint8)(DM_FRAME_F_JSON | DM_FRAME_F_SENSITIVE),
                                pcHello, (uint32)uHello, pstCli->u32ConnectTimeoutMs};
    iErr = dmc__cli_call_raw(pstCli, &stReq, &stResp);
  }
  dmc__zero_mem(pcHello, uHello);
  free(pcHello);
  if (DMC_OK == iErr)
  {
    yyjson_doc *pstRep =
        (NULL != stResp.pcBody) ? yyjson_read(stResp.pcBody, stResp.u32Len, 0) : NULL;
    yyjson_val *pstRepRoot = yyjson_doc_get_root(pstRep);
    const char *pcBoot = yyjson_get_str(yyjson_obj_get(pstRepRoot, "boot"));
    yyjson_val *pstSeq = yyjson_obj_get(pstRepRoot, "seq");
    bool_t bBootChanged = DM_FALSE;
    if (NULL != pcBoot)
    {
      if (('\0' != pstCli->acBoot[0]) && (0 != strcmp(pstCli->acBoot, pcBoot)))
      {
        bBootChanged = DM_TRUE;
      }
      dmc__copy_str(pstCli->acBoot, sizeof(pstCli->acBoot), pcBoot);
    }
    if (bBootChanged || !pstCli->bHasSinceSeq)
    {
      pstCli->u64LastSeq = yyjson_is_uint(pstSeq) ? yyjson_get_uint(pstSeq) : 0u;
    }
    yyjson_doc_free(pstRep);
    dmc_resp_free(&stResp);
    return DMC_OK;
  }
  dmc_resp_free(&stResp);
  return (DM__PERM == iErr) ? DMC_ERR_AUTH : iErr;
}

int dmc__cli_connect_once(dm_io dmc_client_st *pstCli)
{
  char acOpenUri[600];
  char acRole[64];
  char acToken[256];
  char acRunBoot[32];
  int iLink = 0;
  int iHttp = 0;
  int iErr = DMC_OK;

  dmc__cli_drop_link(pstCli);
  acRole[0] = '\0';
  acToken[0] = '\0';
  acRunBoot[0] = '\0';

  if (dmc__str_eq_ci(pstCli->acScheme, "local"))
  {
    dmc__run_info_st stRun;
    memset(&stRun, 0, sizeof(stRun));
    iErr = dmc__read_run_file(pstCli->acDataDir, pstCli->bHasRole ? pstCli->acRole : NULL, &stRun);
    if (DMC_OK != iErr)
    {
      dmc__zero_mem(stRun.acToken, sizeof(stRun.acToken));
      return iErr;
    }
    iLink = stRun.iLinkPort;
    iHttp = stRun.iHttpPort;
    dmc__copy_str(acRunBoot, sizeof(acRunBoot), stRun.acBoot);
    dmc__copy_str(acRole, sizeof(acRole), stRun.acRole);
    dmc__copy_str(acToken, sizeof(acToken), stRun.acToken);
    dmc__zero_mem(stRun.acToken, sizeof(stRun.acToken));
    pstCli->iHttpPort = iHttp;
    if (('\0' == pstCli->acBoot[0]) && ('\0' != acRunBoot[0]))
    {
      dmc__copy_str(pstCli->acBoot, sizeof(pstCli->acBoot), acRunBoot);
    }
    if (pstCli->bHasRole && ('\0' == acRole[0]) && !pstCli->bHasToken)
    {
      dmc__zero_mem(acToken, sizeof(acToken));
      return DMC_ERR_AUTH;
    }
    if (pstCli->bHasRole)
    {
      dmc__copy_str(acRole, sizeof(acRole), pstCli->acRole);
    }
    if (pstCli->bHasToken)
    {
      dmc__copy_str(acToken, sizeof(acToken), pstCli->acToken);
    }
    snprintf(acOpenUri, sizeof(acOpenUri), "local://127.0.0.1:%d", iLink);
  }
  else
  {
    dmc__copy_str(acOpenUri, sizeof(acOpenUri), pstCli->acTarget);
    dmc__copy_str(acRole, sizeof(acRole), pstCli->bHasRole ? pstCli->acRole : "cli");
    if (pstCli->bHasToken)
    {
      dmc__copy_str(acToken, sizeof(acToken), pstCli->acToken);
    }
  }

  iErr = pstCli->stVtbl.pfOpen(acOpenUri, pstCli->u32ConnectTimeoutMs, &pstCli->pvHandle);
  if (DMC_OK != iErr)
  {
    dmc__zero_mem(acToken, sizeof(acToken));
    return iErr;
  }
  dmc_on_connected(pstCli->pstDmc);
  iErr = dmc__cli_handshake(pstCli, ('\0' != acRole[0]) ? acRole : "cli", acToken);
  dmc__zero_mem(acToken, sizeof(acToken));
  if (DMC_OK != iErr)
  {
    dmc__cli_drop_link(pstCli);
    return iErr;
  }
  pstCli->u64LastPingMs = dmc_now_ms();
  dmc__cli_clear_ifaces(pstCli);
  return DMC_OK;
}

dmc_client_st *dmc_client_open(dm_in const dmc_client_opt_st *pstOpt, dm_out int *piErr)
{
  dmc_client_st *pstCli = NULL;
  dmc_host_st stHost;
  int iErr = DMC_OK;

  if (NULL != piErr)
  {
    *piErr = DMC_OK;
  }
  pstCli = (dmc_client_st *)calloc(1u, sizeof(dmc_client_st));
  if (NULL == pstCli)
  {
    if (NULL != piErr)
    {
      *piErr = DMC_ERR_QUEUE;
    }
    return NULL;
  }
  if (NULL != pstOpt)
  {
    if (NULL != pstOpt->pcTarget)
    {
      dmc__copy_str(pstCli->acTarget, sizeof(pstCli->acTarget), pstOpt->pcTarget);
    }
    if (NULL != pstOpt->pcDataDir)
    {
      dmc__copy_str(pstCli->acDataDir, sizeof(pstCli->acDataDir), pstOpt->pcDataDir);
    }
    if ((NULL != pstOpt->pcToken) && ('\0' != pstOpt->pcToken[0]))
    {
      dmc__copy_str(pstCli->acToken, sizeof(pstCli->acToken), pstOpt->pcToken);
      pstCli->bHasToken = DM_TRUE;
    }
    if ((NULL != pstOpt->pcRole) && ('\0' != pstOpt->pcRole[0]))
    {
      dmc__copy_str(pstCli->acRole, sizeof(pstCli->acRole), pstOpt->pcRole);
      pstCli->bHasRole = DM_TRUE;
    }
    if ((NULL != pstOpt->pcPeerID) && ('\0' != pstOpt->pcPeerID[0]))
    {
      dmc__copy_str(pstCli->acPeerID, sizeof(pstCli->acPeerID), pstOpt->pcPeerID);
    }
    pstCli->u32ConnectTimeoutMs = pstOpt->u32ConnectTimeoutMs;
    pstCli->u32DefaultTimeoutMs = pstOpt->u32DefaultTimeoutMs;
    pstCli->u32PingIntervalMs = pstOpt->u32PingIntervalMs;
    pstCli->bAutoReconnect = pstOpt->bAutoReconnect;
    pstCli->bWantPush = pstOpt->bWantPush;
    pstCli->bHasSinceSeq = pstOpt->bHasSinceSeq;
    pstCli->u64LastSeq = pstOpt->u64SinceSeq;
    pstCli->pfOnPush = pstOpt->pfOnPush;
    pstCli->pfOnCall = pstOpt->pfOnCall;
    pstCli->pvUser = pstOpt->pvUser;
    if (NULL != pstOpt->ppcPushTopics)
    {
      uint uiIdx = 0u;
      for (uiIdx = 0u; (NULL != pstOpt->ppcPushTopics[uiIdx]) && (uiIdx < DMC__PUSH_TOPIC_MAX);
           uiIdx++)
      {
        dmc__copy_str(pstCli->aacPushTopics[pstCli->uiPushTopicCount],
                      sizeof(pstCli->aacPushTopics[0]), pstOpt->ppcPushTopics[uiIdx]);
        pstCli->uiPushTopicCount++;
      }
    }
  }
  if ('\0' == pstCli->acPeerID[0])
  {
    snprintf(pstCli->acPeerID, sizeof(pstCli->acPeerID), "cli-%lu", dmc__pid());
  }
  if (0u == pstCli->u32ConnectTimeoutMs)
  {
    pstCli->u32ConnectTimeoutMs = 5000u;
  }
  if (0u == pstCli->u32DefaultTimeoutMs)
  {
    pstCli->u32DefaultTimeoutMs = 15000u;
  }
  if (0u == pstCli->u32PingIntervalMs)
  {
    pstCli->u32PingIntervalMs = 15000u;
  }
  if (!dmc__extract_scheme(pstCli->acTarget, pstCli->acScheme, sizeof(pstCli->acScheme)))
  {
    iErr = DMC_ERR_ARG;
    goto fail;
  }
  pstCli->bHasVtbl = dmc__transport_copy(pstCli->acScheme, &pstCli->stVtbl);
  if (!pstCli->bHasVtbl)
  {
    iErr = DMC_ERR_UNSUPPORTED;
    goto fail;
  }
  memset(&stHost, 0, sizeof(stHost));
  stHost.pfSend = dmc__cli_send;
  stHost.pfOnPush = dmc__cli_on_push;
  stHost.pfOnCall = dmc__cli_on_call;
  stHost.pfOnWireError = dmc__cli_on_wire_err;
  stHost.pvUser = pstCli;
  pstCli->pstDmc = dmc_new(&stHost);
  if (NULL == pstCli->pstDmc)
  {
    iErr = DMC_ERR_QUEUE;
    goto fail;
  }
  iErr = dmc__cli_connect_once(pstCli);
  if (DMC_OK != iErr)
  {
    goto fail;
  }
  return pstCli;

fail:
  dmc_client_close(pstCli);
  if (NULL != piErr)
  {
    *piErr = iErr;
  }
  return NULL;
}

void dmc_client_close(dm_io dmc_client_st *pstCli)
{
  if (NULL == pstCli)
  {
    return;
  }
  dmc__cli_drop_link(pstCli);
  dmc_free(pstCli->pstDmc);
  pstCli->pstDmc = NULL;
  dmc__cli_clear_ifaces(pstCli);
  dmc__zero_mem(pstCli->acToken, sizeof(pstCli->acToken));
  free(pstCli);
  return;
}

#ifdef __cplusplus
}
#endif
