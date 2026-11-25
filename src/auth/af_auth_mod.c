/**
 ******************************************************************************
 * @file    af_auth_mod.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   af_auth DM 模块注册（ModID 16）、5 条 HTTPS 路由、CLI RPC、UI LinkID 与防爆破 OD 校验。
 ******************************************************************************
 */

#define _DARWIN_C_SOURCE
#define DM_LOG_MOD "auth"

#include "af_auth_internal.h"
#include <dm/event.h>
#include <dm/json.h>
#include <dm/log.h>
#include <psa/crypto.h>

#include <stdio.h>
#include <string.h>

af_auth_ctx_st g_stAfAuth = {.iDrainHold = -1};

static const dm_err_entry_st g_astAfAuthErrMap[] = {
    {AF_ERR_BAD_REQ, "bad_request"},        {AF_ERR_UNAUTH, "unauthorized"},
    {AF_ERR_WRONG_CODE, "wrong_code"},      {AF_ERR_LOCKED, "locked"},
    {AF_ERR_PAIR_CLOSED, "pairing_closed"}, {AF_ERR_UPGRADE_REQ, "upgrade_required"},
    {AF_ERR_WRONG_PW, "wrong_password"},    {AF_ERR_NOT_FOUND, "not_found"},
    {AF_ERR_INTERNAL, "internal"},          {0, NULL}};

uint64 af_auth_now_ms(void)
{
  if (NULL != g_stAfAuth.pfClock)
  {
    return g_stAfAuth.pfClock();
  }
  return dm_hrtime() / 1000000ULL;
}

void af_auth_set_clock_fn(dm_in af_auth_clock_fn pfClock)
{
  g_stAfAuth.pfClock = pfClock;
}

void af_auth_set_od_verify_fn(dm_in af_auth_od_verify_fn pfVerify)
{
  g_stAfAuth.pfOdVerify = pfVerify;
}

int af_auth_set_data_dir(dm_in const char *pcDataDir)
{
  if ((NULL == pcDataDir) || ('\0' == pcDataDir[0]))
  {
    return DM_INVALID;
  }
  snprintf(g_stAfAuth.acDataDir, sizeof(g_stAfAuth.acDataDir), "%s", pcDataDir);
  return DM_SUCCESS;
}

int af_auth_rng_init(void)
{
  if (g_stAfAuth.bRngReady)
  {
    return DM_SUCCESS;
  }
  if (PSA_SUCCESS != psa_crypto_init())
  {
    return DM_FAILD;
  }
  mbedtls_entropy_init(&g_stAfAuth.stEntropy);
  mbedtls_ctr_drbg_init(&g_stAfAuth.stDrbg);
  if (0 != mbedtls_ctr_drbg_seed(&g_stAfAuth.stDrbg, mbedtls_entropy_func, &g_stAfAuth.stEntropy,
                                 (const unsigned char *)"af_auth", 7u))
  {
    mbedtls_ctr_drbg_free(&g_stAfAuth.stDrbg);
    mbedtls_entropy_free(&g_stAfAuth.stEntropy);
    return DM_FAILD;
  }
  g_stAfAuth.bRngReady = DM_TRUE;
  return DM_SUCCESS;
}

void af_auth_rng_fini(void)
{
  if (g_stAfAuth.bRngReady)
  {
    mbedtls_ctr_drbg_free(&g_stAfAuth.stDrbg);
    mbedtls_entropy_free(&g_stAfAuth.stEntropy);
    g_stAfAuth.bRngReady = DM_FALSE;
  }
}

int af_auth_rng_cb(dm_io void *pvCtx, dm_out unsigned char *pucOut, dm_in size_t uLen)
{
  (void)pvCtx;
  if (!g_stAfAuth.bRngReady)
  {
    if (DM_SUCCESS != af_auth_rng_init())
    {
      return -1;
    }
  }
  return mbedtls_ctr_drbg_random(&g_stAfAuth.stDrbg, pucOut, uLen);
}

int af_auth_rng_bytes(dm_out uint8 *pu8Out, dm_in size_t uLen)
{
  return (0 == af_auth_rng_cb(NULL, pu8Out, uLen)) ? DM_SUCCESS : DM_FAILD;
}

void af_auth_trunc_utf8_chars(dm_in const char *pcSrc, dm_out char *pcDst, dm_in size_t uDstCap,
                              dm_in size_t uMaxChars)
{
  size_t uPos = 0u;
  size_t uChars = 0u;
  size_t uSrcLen = (NULL != pcSrc) ? strlen(pcSrc) : 0u;

  if ((NULL == pcDst) || (0u == uDstCap))
  {
    return;
  }
  while ((uPos < uSrcLen) && (uChars < uMaxChars))
  {
    unsigned char uc = (unsigned char)pcSrc[uPos];
    size_t uStep = 1u;
    if (0xF0u == (uc & 0xF8u))
    {
      uStep = 4u;
    }
    else if (0xE0u == (uc & 0xF0u))
    {
      uStep = 3u;
    }
    else if (0xC0u == (uc & 0xE0u))
    {
      uStep = 2u;
    }
    if (((uPos + uStep) > uSrcLen) || ((uPos + uStep) >= uDstCap))
    {
      break;
    }
    uPos += uStep;
    uChars++;
  }
  if (uPos > 0u)
  {
    memcpy(pcDst, pcSrc, uPos);
  }
  pcDst[uPos] = '\0';
}

void af_auth_publish_ev(dm_in af_auth_ev_kind_et eKind, dm_in const char *pcDevId,
                        dm_in const char *pcDevName, dm_in const char *pcSidecarName)
{
  af_ev_auth_st stEv;

  memset(&stEv, 0, sizeof(stEv));
  stEv.eKind = eKind;
  if (NULL != pcDevId)
  {
    snprintf(stEv.acDevId, sizeof(stEv.acDevId), "%s", pcDevId);
  }
  if (NULL != pcDevName)
  {
    snprintf(stEv.acDevName, sizeof(stEv.acDevName), "%s", pcDevName);
  }
  if (NULL != pcSidecarName)
  {
    snprintf(stEv.acSidecarName, sizeof(stEv.acSidecarName), "%s", pcSidecarName);
  }
  (void)dm_event_publish(AF_EV_AUTH_CHANGED, &stEv, (uint32)sizeof(stEv));
}

void af_auth_inflight_inc(void)
{
  g_stAfAuth.u32InFlight++;
}

void af_auth_inflight_dec(void)
{
  if (g_stAfAuth.u32InFlight > 0u)
  {
    g_stAfAuth.u32InFlight--;
  }
  if ((0u == g_stAfAuth.u32InFlight) && (g_stAfAuth.iDrainHold >= 0))
  {
    dm_drain_release(g_stAfAuth.iDrainHold);
    g_stAfAuth.iDrainHold = -1;
  }
}

/* ============================ 公开 HTTPS 应答与查询 ============================ */

void af_https_send_ok(dm_in uint64 u64ReplyId, dm_in const char *pcJson)
{
  const char *pcBody = (NULL != pcJson) ? pcJson : "{}";
  (void)af_https_reply(u64ReplyId, 200u, pcBody, (uint32)strlen(pcBody));
}

void af_https_send_err(dm_in uint64 u64ReplyId, dm_in uint uiStatus, dm_in const char *pcCode,
                       dm_in const char *pcMsg)
{
  yyjson_mut_doc *pstDoc = yyjson_mut_doc_new(dm_json_alc());
  yyjson_mut_val *pstRoot = NULL;
  char *pcJson = NULL;
  size_t uLen = 0u;

  if (NULL == pstDoc)
  {
    static const char acFallback[] = "{\"code\":\"internal\",\"error\":\"internal\"}";
    (void)af_https_reply(u64ReplyId, uiStatus, acFallback, (uint32)(sizeof(acFallback) - 1u));
    return;
  }
  pstRoot = yyjson_mut_obj(pstDoc);
  yyjson_mut_doc_set_root(pstDoc, pstRoot);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "code", (NULL != pcCode) ? pcCode : "internal");
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "error", (NULL != pcMsg) ? pcMsg : "");
  pcJson = dm_json_write_mut(pstDoc, &uLen);
  yyjson_mut_doc_free(pstDoc);
  if (NULL != pcJson)
  {
    (void)af_https_reply(u64ReplyId, uiStatus, pcJson, (uint32)uLen);
    dm_json_free_str(pcJson);
  }
}

void af_auth_get_pair_view(dm_out af_pair_view_st *pstOut)
{
  uint64 u64Now = af_auth_now_ms();

  if (NULL == pstOut)
  {
    return;
  }
  memset(pstOut, 0, sizeof(*pstOut));
  af_auth_pair_refresh();
  snprintf(pstOut->acLastPaired, sizeof(pstOut->acLastPaired), "%s", g_stAfAuth.acLastPaired);
  if (g_stAfAuth.bPairOpen)
  {
    snprintf(pstOut->acCode, sizeof(pstOut->acCode), "%s", g_stAfAuth.acCode);
    if (g_stAfAuth.u64WinUntilMs > u64Now)
    {
      pstOut->u64LeftSec = (g_stAfAuth.u64WinUntilMs - u64Now + 999ULL) / 1000ULL;
    }
    if ((0u != g_stAfAuth.u64LockedUntilMs) && (g_stAfAuth.u64LockedUntilMs > u64Now))
    {
      pstOut->u64LockSec = (g_stAfAuth.u64LockedUntilMs - u64Now + 999ULL) / 1000ULL;
    }
  }
}

uint32 af_auth_list_devices(dm_out af_device_st *pastOut, dm_in uint32 u32Cap)
{
  uint32 u32N = g_stAfAuth.u32DevCnt;

  if (NULL == pastOut)
  {
    return u32N;
  }
  if (u32N > u32Cap)
  {
    u32N = u32Cap;
  }
  if (u32N > 0u)
  {
    memcpy(pastOut, g_stAfAuth.astDevs, (size_t)u32N * sizeof(af_device_st));
  }
  return u32N;
}

bool_t af_auth_other_owns_sidecar(dm_in const char *pcSelfDevId, dm_in const char *pcSidecarName)
{
  uint32 i = 0u;

  if ((NULL == pcSidecarName) || ('\0' == pcSidecarName[0]))
  {
    return DM_FALSE;
  }
  for (i = 0u; i < g_stAfAuth.u32DevCnt; i++)
  {
    if ((0 == strcmp(g_stAfAuth.astDevs[i].acSidecarName, pcSidecarName)) &&
        ((NULL == pcSelfDevId) || (0 != strcmp(g_stAfAuth.astDevs[i].acId, pcSelfDevId))))
    {
      return DM_TRUE;
    }
  }
  return DM_FALSE;
}

int af_auth_bind_sidecar(dm_in const char *pcDevId, dm_in const char *pcSidecarName)
{
  int iIdx = af_auth_store_find_by_id(pcDevId);
  af_device_st *pstDev = NULL;

  if (iIdx < 0)
  {
    return AF_ERR_NOT_FOUND;
  }
  pstDev = &g_stAfAuth.astDevs[iIdx];
  snprintf(pstDev->acSidecarName, sizeof(pstDev->acSidecarName), "%s",
           (NULL != pcSidecarName) ? pcSidecarName : "");
  (void)af_auth_store_save_async(NULL, NULL);
  af_auth_publish_ev(AF_AUTH_EV_BINDING, pstDev->acId, pstDev->acName, pstDev->acSidecarName);
  return DM_SUCCESS;
}

/* ============================ HTTPS 路由：/info, /password, /unpair ============================
 */

static void af__on_info(dm_in const af_https_msg_st *pstMsg)
{
  const af_state_st *pstSt = af_store_state();
  char acName[65];
  yyjson_mut_doc *pstDoc = yyjson_mut_doc_new(dm_json_alc());
  yyjson_mut_val *pstRoot = NULL;
  char *pcJson = NULL;
  size_t uLen = 0u;

  if ((NULL != pstSt) && ('\0' != pstSt->acName[0]))
  {
    snprintf(acName, sizeof(acName), "%s", pstSt->acName);
  }
  else
  {
    af_plat_host_name(acName, sizeof(acName));
  }
  if (NULL == pstDoc)
  {
    af_https_send_err(pstMsg->u64ReplyId, 500u, "internal", "内存不足");
    return;
  }
  pstRoot = yyjson_mut_obj(pstDoc);
  yyjson_mut_doc_set_root(pstDoc, pstRoot);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "name", acName);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "version", AF_VERSION);
  yyjson_mut_obj_add_uint(pstDoc, pstRoot, "proto", 2u);
  pcJson = dm_json_write_mut(pstDoc, &uLen);
  yyjson_mut_doc_free(pstDoc);
  if (NULL != pcJson)
  {
    (void)af_https_reply(pstMsg->u64ReplyId, 200u, pcJson, (uint32)uLen);
    dm_json_free_str(pcJson);
  }
}

typedef struct af__pw_job
{
  uint64 u64ReplyId;
  char acDevId[37];
  char acUser[64];
  char acPass[257];
  int iOdRc;
} af__pw_job_st;

static void af__pw_on_saved(dm_in int iStatus, dm_io void *pvCtx)
{
  uint64 *pu64Rid = (uint64 *)pvCtx;

  if (DM_SUCCESS == iStatus)
  {
    af_https_send_ok(*pu64Rid, "{\"ok\":true}");
  }
  else
  {
    af_https_send_err(*pu64Rid, 500u, "internal", "保存失败");
  }
  dm_free_tag(pu64Rid);
}

static void af__pw_work(dm_io void *pvArg)
{
  af__pw_job_st *pstJob = (af__pw_job_st *)pvArg;
  af_auth_od_verify_fn pfVerify =
      (NULL != g_stAfAuth.pfOdVerify) ? g_stAfAuth.pfOdVerify : af_plat_od_verify;

  pstJob->iOdRc = pfVerify(pstJob->acUser, pstJob->acPass);
  dm_secure_zero(pstJob->acPass, sizeof(pstJob->acPass));
}

static void af__pw_after(dm_io void *pvArg, dm_in int iStatus)
{
  af__pw_job_st *pstJob = (af__pw_job_st *)pvArg;
  uint64 u64Rid = pstJob->u64ReplyId;
  int iOdRc = pstJob->iOdRc;
  char acDevId[37];

  snprintf(acDevId, sizeof(acDevId), "%s", pstJob->acDevId);
  dm_secure_zero(pstJob->acPass, sizeof(pstJob->acPass));
  dm_free_tag(pstJob);

  if (0 != iStatus)
  {
    af_https_send_err(u64Rid, 500u, "internal", "服务正在退出");
    af_auth_inflight_dec();
    return;
  }
  if (0 == iOdRc)
  {
    int iIdx = af_auth_store_find_by_id(acDevId);
    g_stAfAuth.u32PwFails = 0u;
    g_stAfAuth.u64PwLockedUntilMs = 0u;
    if (iIdx < 0)
    {
      af_https_send_err(u64Rid, 401u, "unauthorized", "设备已解绑");
      af_auth_inflight_dec();
      return;
    }
    g_stAfAuth.astDevs[iIdx].bPasswordSaved = DM_TRUE;
    af_auth_publish_ev(AF_AUTH_EV_BINDING, g_stAfAuth.astDevs[iIdx].acId,
                       g_stAfAuth.astDevs[iIdx].acName, g_stAfAuth.astDevs[iIdx].acSidecarName);
    uint64 *pu64Ctx = (uint64 *)dm_malloc_tag(sizeof(uint64), DM_MEM_TAG_JSON);
    if ((NULL != pu64Ctx) &&
        (*pu64Ctx = u64Rid, DM_SUCCESS == af_auth_store_save_async(af__pw_on_saved, pu64Ctx)))
    {
      af_auth_inflight_dec();
      return;
    }
    if (NULL != pu64Ctx)
    {
      dm_free_tag(pu64Ctx);
    }
    af_https_send_ok(u64Rid, "{\"ok\":true}");
    af_auth_inflight_dec();
    return;
  }
  if (-1 == iOdRc)
  {
    if ((g_stAfAuth.u32PwFails >= AF_PW_MAX_TRY) && (0u == g_stAfAuth.u64PwLockedUntilMs))
    {
      g_stAfAuth.u64PwLockedUntilMs = af_auth_now_ms() + AF_PW_LOCK_MS;
    }
    af_https_send_err(u64Rid, 400u, "wrong_password", "密码不对");
    af_auth_inflight_dec();
    return;
  }
  af_https_send_err(u64Rid, 500u, "internal", "系统密码校验异常");
  af_auth_inflight_dec();
}

static void af__on_password(dm_in const af_https_msg_st *pstMsg)
{
  af_device_st stDev;
  uint64 u64Now = af_auth_now_ms();
  int iErr = 0;
  yyjson_doc *pstDoc = NULL;
  yyjson_val *pstPw = NULL;
  const char *pcPw = NULL;
  size_t uPwLen = 0u;
  af__pw_job_st *pstJob = NULL;

  if (!af_auth_check_https(pstMsg, &stDev))
  {
    return;
  }
  if (0u != g_stAfAuth.u64PwLockedUntilMs)
  {
    if (u64Now < g_stAfAuth.u64PwLockedUntilMs)
    {
      af_https_send_err(pstMsg->u64ReplyId, 429u, "locked", "密码试错次数太多，请 1 分钟后再试");
      return;
    }
    g_stAfAuth.u32PwFails = 0u;
    g_stAfAuth.u64PwLockedUntilMs = 0u;
  }
  if (g_stAfAuth.u32PwFails >= AF_PW_MAX_TRY)
  {
    g_stAfAuth.u64PwLockedUntilMs = u64Now + AF_PW_LOCK_MS;
    af_https_send_err(pstMsg->u64ReplyId, 429u, "locked", "密码试错次数太多，请 1 分钟后再试");
    return;
  }
  pstDoc = dm_json_read_sensitive((const char *)pstMsg->pu8Body, pstMsg->u32BodyLen, 65536u, &iErr);
  pstPw = (NULL != pstDoc) ? yyjson_obj_get(yyjson_doc_get_root(pstDoc), "password") : NULL;
  pcPw = yyjson_get_str(pstPw);
  uPwLen = (NULL != pcPw) ? yyjson_get_len(pstPw) : 0u;
  if ((NULL == pcPw) || (0u == uPwLen) || (uPwLen > 256u))
  {
    if (NULL != pstDoc)
    {
      yyjson_doc_free(pstDoc);
    }
    af_https_send_err(pstMsg->u64ReplyId, 400u, "bad_request", "密码长度无效");
    return;
  }
  pstJob = (af__pw_job_st *)dm_calloc_tag(1u, sizeof(*pstJob), DM_MEM_TAG_SECRET);
  if (NULL == pstJob)
  {
    yyjson_doc_free(pstDoc);
    af_https_send_err(pstMsg->u64ReplyId, 500u, "internal", "内存不足");
    return;
  }
  pstJob->u64ReplyId = pstMsg->u64ReplyId;
  snprintf(pstJob->acDevId, sizeof(pstJob->acDevId), "%s", stDev.acId);
  af_plat_current_user(pstJob->acUser, sizeof(pstJob->acUser));
  memcpy(pstJob->acPass, pcPw, uPwLen);
  pstJob->acPass[uPwLen] = '\0';
  yyjson_doc_free(pstDoc);

  /* 校验前立刻计次，保证 16+ 并发最多只有 5 个进入 DM_LANE_DEFAULT */
  g_stAfAuth.u32PwFails++;
  af_auth_inflight_inc();
  if (DM_SUCCESS != dm_pool_submit(DM_LANE_DEFAULT, af__pw_work, af__pw_after, pstJob))
  {
    af_auth_inflight_dec();
    dm_secure_zero(pstJob->acPass, sizeof(pstJob->acPass));
    dm_free_tag(pstJob);
    af_https_send_err(pstMsg->u64ReplyId, 500u, "internal", "系统繁忙");
  }
}

static void af__unpair_on_saved(dm_in int iStatus, dm_io void *pvCtx)
{
  uint64 *pu64Rid = (uint64 *)pvCtx;

  if (DM_SUCCESS == iStatus)
  {
    af_https_send_ok(*pu64Rid, "{\"ok\":true}");
  }
  else
  {
    af_https_send_err(*pu64Rid, 500u, "internal", "保存失败");
  }
  dm_free_tag(pu64Rid);
}

static void af__on_unpair(dm_in const af_https_msg_st *pstMsg)
{
  af_device_st stDev;
  af_device_st stRemoved;
  uint64 *pu64Rid = NULL;

  if (!af_auth_check_https(pstMsg, &stDev))
  {
    return;
  }
  if (!af_auth_store_remove_by_id(stDev.acId, &stRemoved))
  {
    af_https_send_ok(pstMsg->u64ReplyId, "{\"ok\":true}");
    return;
  }
  af_auth_publish_ev(AF_AUTH_EV_REVOKED, stRemoved.acId, stRemoved.acName, stRemoved.acSidecarName);
  pu64Rid = (uint64 *)dm_malloc_tag(sizeof(uint64), DM_MEM_TAG_JSON);
  if ((NULL != pu64Rid) && (*pu64Rid = pstMsg->u64ReplyId,
                            DM_SUCCESS == af_auth_store_save_async(af__unpair_on_saved, pu64Rid)))
  {
    return;
  }
  if (NULL != pu64Rid)
  {
    dm_free_tag(pu64Rid);
  }
  af_https_send_ok(pstMsg->u64ReplyId, "{\"ok\":true}");
}

/* ============================ CLI RPC 与 UI LinkID ============================ */

static int af__on_cli_pair_code(dm_io void *pvArg)
{
  dm_link_msg_st *pstMsg = (dm_link_msg_st *)pvArg;
  dm_reply_st stReply;
  af_pair_view_st stView;
  char acResp[128];
  int iN = 0;

  if (DM_SUCCESS != dm_reply_take(pstMsg, &stReply))
  {
    return DM_SUCCESS;
  }
  af_auth_pair_refresh();
  af_auth_get_pair_view(&stView);
  if (stView.u64LockSec > 0u)
  {
    iN = snprintf(acResp, sizeof(acResp), "{\"code\":\"\",\"lock_sec\":%llu}",
                  (unsigned long long)stView.u64LockSec);
  }
  else
  {
    af_auth_pair_open();
    af_auth_publish_ev(AF_AUTH_EV_PAIR_WIN, NULL, NULL, NULL);
    af_auth_get_pair_view(&stView);
    iN = snprintf(acResp, sizeof(acResp), "{\"code\":\"%s\",\"left_sec\":%llu}", stView.acCode,
                  (unsigned long long)stView.u64LeftSec);
  }
  if (iN > 0)
  {
    (void)dm_reply_send(&stReply, DM_SUCCESS, acResp, (uint32)iN);
  }
  dm_secure_zero(acResp, sizeof(acResp));
  dm_secure_zero(&stView, sizeof(stView));
  return DM_SUCCESS;
}

static int af__on_ui_pair_open(dm_io void *pvArg)
{
  (void)pvArg;
  af_auth_pair_open();
  af_auth_publish_ev(AF_AUTH_EV_PAIR_WIN, NULL, NULL, NULL);
  return DM_SUCCESS;
}

static int af__on_ui_pair_close(dm_io void *pvArg)
{
  (void)pvArg;
  af_auth_pair_close(DM_FALSE);
  af_auth_publish_ev(AF_AUTH_EV_PAIR_WIN, NULL, NULL, NULL);
  return DM_SUCCESS;
}

static int af__on_ui_unpair(dm_io void *pvArg)
{
  dm_link_msg_st *pstMsg = (dm_link_msg_st *)pvArg;
  int iErr = 0;
  yyjson_doc *pstDoc = NULL;
  const char *pcId = NULL;
  af_device_st stRemoved;

  if ((NULL == pstMsg) || (NULL == pstMsg->pu8Data) || (0u == pstMsg->u32Len))
  {
    return DM_SUCCESS;
  }
  pstDoc = dm_json_read((const char *)pstMsg->pu8Data, pstMsg->u32Len, 4096u, &iErr);
  if (NULL == pstDoc)
  {
    return DM_SUCCESS;
  }
  pcId = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(pstDoc), "id"));
  if ((NULL != pcId) && af_auth_store_remove_by_id(pcId, &stRemoved))
  {
    (void)af_auth_store_save_async(NULL, NULL);
    af_auth_publish_ev(AF_AUTH_EV_REVOKED, stRemoved.acId, stRemoved.acName,
                       stRemoved.acSidecarName);
  }
  yyjson_doc_free(pstDoc);
  return DM_SUCCESS;
}

/* ============================ DM 模块生命周期 ============================ */

static int af__auth_init(void)
{
  static const dm_iface_st stCliPairCode = {
      .uiLinkID = AF_LINK_CLI_PAIR_CODE,
      .pcName = "auth.pair_code",
      .u32Roles = AF_ROLE_CLI,
      .u32Flags = DM_IFACE_MUTATE,
      .pcDesc = "Open/extend pairing window and return 6-digit code",
      .pcArgs = NULL};
  int iRc = DM_SUCCESS;

  g_stAfAuth.bDraining = DM_FALSE;
  g_stAfAuth.iDrainHold = -1;
  g_stAfAuth.u32InFlight = 0u;
  g_stAfAuth.u32PwFails = 0u;
  g_stAfAuth.u64PwLockedUntilMs = 0u;
  af_auth_pair_close(DM_FALSE);

  iRc = af_auth_rng_init();
  if (DM_SUCCESS != iRc)
  {
    return iRc;
  }
  if ('\0' != g_stAfAuth.acDataDir[0])
  {
    (void)af_auth_store_load(g_stAfAuth.acDataDir);
  }
  if ((DM_SUCCESS != af_https_route_exact("GET", "/info", af__on_info)) ||
      (DM_SUCCESS != af_https_route_exact("POST", "/pair/start", af_auth_pair_on_start)) ||
      (DM_SUCCESS != af_https_route_exact("POST", "/pair/finish", af_auth_pair_on_finish)) ||
      (DM_SUCCESS != af_https_route_exact("POST", "/password", af__on_password)) ||
      (DM_SUCCESS != af_https_route_exact("POST", "/unpair", af__on_unpair)) ||
      (DM_SUCCESS !=
       dm_link_register_handler(AF_LINK_CLI_PAIR_CODE, af__on_cli_pair_code, NULL, NULL)) ||
      (DM_SUCCESS != dm_iface_register(&stCliPairCode)) ||
      (DM_SUCCESS !=
       dm_link_register_handler(AF_LINK_UI_PAIR_OPEN, af__on_ui_pair_open, NULL, NULL)) ||
      (DM_SUCCESS !=
       dm_link_register_handler(AF_LINK_UI_PAIR_CLOSE, af__on_ui_pair_close, NULL, NULL)) ||
      (DM_SUCCESS != dm_link_register_handler(AF_LINK_UI_UNPAIR, af__on_ui_unpair, NULL, NULL)))
  {
    return DM_FAILD;
  }
  return DM_SUCCESS;
}

static void af__auth_drain(dm_io dm_drain_st *pstDrain)
{
  g_stAfAuth.bDraining = DM_TRUE;
  if (g_stAfAuth.u32InFlight > 0u)
  {
    g_stAfAuth.iDrainHold = dm_drain_hold(pstDrain);
  }
}

static void af__auth_fini(void)
{
  af_auth_pair_close(DM_FALSE);
  af_auth_rng_fini();
  dm_secure_zero(g_stAfAuth.astDevs, sizeof(g_stAfAuth.astDevs));
  g_stAfAuth.u32DevCnt = 0u;
  g_stAfAuth.u32PwFails = 0u;
  g_stAfAuth.u64PwLockedUntilMs = 0u;
  g_stAfAuth.u32InFlight = 0u;
  g_stAfAuth.iDrainHold = -1;
  g_stAfAuth.bDraining = DM_FALSE;
}

const dm_module_st g_stAfAuthMod = {.u16ModID = AF_MOD_AUTH,
                                    .pcName = "af_auth",
                                    .pfInit = af__auth_init,
                                    .pfDrain = af__auth_drain,
                                    .pfFini = af__auth_fini,
                                    .pastErrMap = g_astAfAuthErrMap};

const dm_module_st *af_auth_module(void)
{
  return &g_stAfAuthMod;
}
