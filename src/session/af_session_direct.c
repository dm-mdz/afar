/**
 ******************************************************************************
 * @file    af_session_direct.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   af_session 日常模式直连路由（/direct/start, /direct/stop,
 * /direct/switch_prodraw）与互斥。
 ******************************************************************************
 */

#define _DARWIN_C_SOURCE
#define DM_LOG_MOD "session"

#include "af_session_internal.h"

#include <af_direct_proto.h>
#include <dm/crypto.h>
#include <mbedtls/base64.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

#include <stdio.h>
#include <string.h>

#define AF_DIRECT_HOLD_MS 15000u
#define AF_DIRECT_PERM_WIN_MS 120000u
#define AF_DIRECT_DEF_FPS 60u
#define AF_DIRECT_SWITCH_DELAY_MS 450u
#define AF_DIRECT_SC_WAIT_MS 3000u

typedef struct af__direct_keys
{
  uint8 au8SidBe[8];
  uint8 au8Master[32];
  uint8 au8VidAuth[32];
  uint8 au8KeyI2M[32];
  uint8 au8KeyM2I[32];
} af__direct_keys_st;

typedef struct af__direct_start_opt
{
  bool_t bChroma444;
  bool_t bTakeover;
  bool_t bHasScale;
  bool_t bHasScreen;
  uint16 u16Fps;
  uint32 u32ScrW;
  uint32 u32ScrH;
  char acScale[16];
} af__direct_start_opt_st;

typedef struct af__direct_pending
{
  uint64 u64ReplyId;
  uint64 u64ReqSeq;
  char acDevId[37];
  char acDevName[65];
  char acDirectKey[65];
  char acScale[16];
  uint32 u32ScrW;
  uint32 u32ScrH;
  uint16 u16Fps;
  bool_t bChroma444;
  bool_t bHasScale;
} af__direct_pending_st;

typedef struct af__direct_ctx
{
  bool_t bPendingStart;
  bool_t bWaitingStop;
  bool_t bWaitingScStop;
  bool_t bPendingProdraw;
  bool_t bProdrawKbd;
  uint32 u32ProdrawTimer;
  uint32 u32ScWaitTimer;
  uint64 u64NextSeq;
  char acDirectKey[65];
  char acProdrawTarget[65];
  af__direct_pending_st stPend;
} af__direct_ctx_st;

static af__direct_ctx_st g_stDirect;
static void af__direct_launch_vd(void);

static void af__direct_cancel_sc_wait(void)
{
  if (0u != g_stDirect.u32ScWaitTimer) { (void)dm_timer_cancel(g_stDirect.u32ScWaitTimer); g_stDirect.u32ScWaitTimer = 0u; }
  g_stDirect.bWaitingScStop = DM_FALSE;
}

static void af__direct_try_resume_sc(void)
{
  af_sc_snap_st stSc;
  if ((!g_stDirect.bPendingStart) || (!g_stDirect.bWaitingScStop)) { return; }
  af_sidecar_get_snap(&stSc);
  if ((AF_SC_IDLE == stSc.eState) && (!af_display_has_sidecar_disp()))
  {
    af__direct_cancel_sc_wait();
    if (!g_stDirect.bWaitingStop) { af__direct_launch_vd(); }
  }
}

static int af__direct_on_sc_wait_tmr(dm_io void *pvArg)
{
  (void)pvArg;
  g_stDirect.u32ScWaitTimer = 0u;
  if ((!g_stDirect.bPendingStart) || (!g_stDirect.bWaitingScStop) || g_stAfSes.bDraining) { af__direct_cancel_sc_wait(); return DM_SUCCESS; }
  DM_LOG_WARN("sidecar exit barrier timed out (%ums), proceeding with direct launch", AF_DIRECT_SC_WAIT_MS);
  g_stDirect.bWaitingScStop = DM_FALSE;
  if (!g_stDirect.bWaitingStop) { af__direct_launch_vd(); }
  return DM_SUCCESS;
}

static void af__direct_cancel_prodraw(void)
{
  if (0u != g_stDirect.u32ProdrawTimer) { (void)dm_timer_cancel(g_stDirect.u32ProdrawTimer); g_stDirect.u32ProdrawTimer = 0u; }
  g_stDirect.bPendingProdraw = DM_FALSE; g_stDirect.bProdrawKbd = DM_FALSE; g_stDirect.acProdrawTarget[0] = '\0';
}

static int af__direct_on_prodraw_tmr(dm_io void *pvArg)
{
  char acTarget[65];
  bool_t bKbd = g_stDirect.bProdrawKbd;

  (void)pvArg;
  g_stDirect.u32ProdrawTimer = 0u;
  if ((!g_stDirect.bPendingProdraw) || g_stAfSes.bDraining) { af__direct_cancel_prodraw(); return DM_SUCCESS; }
  snprintf(acTarget, sizeof(acTarget), "%s", g_stDirect.acProdrawTarget);
  af__direct_cancel_prodraw();
  if ('\0' != acTarget[0])
  {
    af_kbd_notify_connect(acTarget, bKbd);
    (void)af_sidecar_connect(acTarget, DM_TRUE);
    af_ses_push_menu();
  }
  return DM_SUCCESS;
}

static void af__direct_arm_prodraw(void)
{
  if ((!g_stDirect.bPendingProdraw) || (0u != g_stDirect.u32ProdrawTimer)) { return; }
  if (DM_SUCCESS != dm_timer_add(AF_DIRECT_SWITCH_DELAY_MS, 0u, AF_SES_LINK_PRODRAW_TMR, 0u,
                                 &g_stDirect.u32ProdrawTimer))
  {
    (void)af__direct_on_prodraw_tmr(NULL);
  }
}

static int af__direct_derive_keys(dm_out af__direct_keys_st *pstK)
{
  static const char *const apcInfo[3] = {AF_DP_HKDF_INFO_VID_AUTH, AF_DP_HKDF_INFO_IN_I2M,
                                         AF_DP_HKDF_INFO_CTL_M2I};
  uint8 *apu8Out[3] = {pstK->au8VidAuth, pstK->au8KeyI2M, pstK->au8KeyM2I};
  const mbedtls_md_info_t *pstMd = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  uint32 i = 0u;

  if ((NULL == pstMd) || (DM_SUCCESS != dm_crypto_random(pstK->au8SidBe, 8u)) ||
      (DM_SUCCESS != dm_crypto_random(pstK->au8Master, 32u)))
  {
    return DM_FAILD;
  }
  for (i = 0u; i < 3u; i++)
  {
    if (0 != mbedtls_hkdf(pstMd, pstK->au8SidBe, 8u, pstK->au8Master, 32u,
                          (const unsigned char *)apcInfo[i], strlen(apcInfo[i]), apu8Out[i], 32u))
    {
      return DM_FAILD;
    }
  }
  return DM_SUCCESS;
}

static void af__direct_reply_perm_missing(dm_in uint64 u64ReplyId, dm_in bool_t bPermSc,
                                          dm_in bool_t bPermAx)
{
  char acBuf[256];
  int iN = 0;

  af_env_start_fast_perm_poll(AF_DIRECT_PERM_WIN_MS);
  iN = snprintf(acBuf, sizeof(acBuf),
                "{\"code\":\"perm_missing\","
                "\"error\":\"请先在 Mac 系统设置中授予屏幕录制与辅助功能权限\","
                "\"perm_sc\":%s,\"perm_ax\":%s}",
                bPermSc ? "true" : "false", bPermAx ? "true" : "false");
  if ((iN > 0) && ((size_t)iN < sizeof(acBuf)))
  {
    (void)af_https_reply(u64ReplyId, 412u, acBuf, (uint32)iN);
  }
}

typedef struct af__direct_ok_resp
{
  char acSidHex[17];
  char acMasterB64[64];
  char acCertSha[65];
  uint16 u16VideoPort;
  uint16 u16InputPort;
  uint16 u16EncW;
  uint16 u16EncH;
  uint16 u16Fps;
  bool_t bChroma444;
} af__direct_ok_resp_st;

static void af__direct_send_start_ok(dm_in uint64 u64ReplyId,
                                     dm_in const af__direct_ok_resp_st *pstResp)
{
  yyjson_mut_doc *pstDoc = yyjson_mut_doc_new(dm_json_alc_sensitive());
  yyjson_mut_val *pstRoot = NULL;
  char *pcJson = NULL;
  size_t uLen = 0u;

  if (NULL == pstDoc)
  {
    af_https_send_err(u64ReplyId, 500u, "internal", "内存不足");
    return;
  }
  pstRoot = yyjson_mut_obj(pstDoc);
  yyjson_mut_doc_set_root(pstDoc, pstRoot);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "ok", true);
  yyjson_mut_obj_add_strcpy(pstDoc, pstRoot, "session_id", pstResp->acSidHex);
  yyjson_mut_obj_add_uint(pstDoc, pstRoot, "video_port", pstResp->u16VideoPort);
  yyjson_mut_obj_add_uint(pstDoc, pstRoot, "input_port", pstResp->u16InputPort);
  yyjson_mut_obj_add_strcpy(pstDoc, pstRoot, "master_key_b64", pstResp->acMasterB64);
  yyjson_mut_obj_add_uint(pstDoc, pstRoot, "width", pstResp->u16EncW);
  yyjson_mut_obj_add_uint(pstDoc, pstRoot, "height", pstResp->u16EncH);
  yyjson_mut_obj_add_uint(pstDoc, pstRoot, "fps", pstResp->u16Fps);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "chroma444", pstResp->bChroma444 ? true : false);
  yyjson_mut_obj_add_strcpy(pstDoc, pstRoot, "tls_cert_sha256", pstResp->acCertSha);

  pcJson = dm_json_write_mut_sensitive(pstDoc, &uLen);
  yyjson_mut_doc_free(pstDoc);
  if (NULL != pcJson)
  {
    (void)af_https_reply(u64ReplyId, 200u, pcJson, (uint32)uLen);
    dm_json_free_str_sensitive(pcJson);
  }
  else
  {
    af_https_send_err(u64ReplyId, 500u, "internal", "序列化应答失败");
  }
}

static void af__direct_reply_stream_err(dm_in uint64 u64ReplyId, dm_in int iRc)
{
  uint uiSt = (AF_ERR_STREAM_BUSY == iRc) ? 409u : ((AF_ERR_STREAM_STOPPING == iRc) ? 503u : 500u);
  const char *pcCode = (AF_ERR_STREAM_BUSY == iRc)
                           ? "stream_busy"
                           : ((AF_ERR_STREAM_STOPPING == iRc) ? "stream_stopping" : "internal");
  const char *pcMsg =
      (AF_ERR_STREAM_BUSY == iRc)
          ? "直连通道正忙"
          : ((AF_ERR_STREAM_STOPPING == iRc) ? "直连正在停止，请稍后重试" : "启动直连推流失败");
  af_https_send_err(u64ReplyId, uiSt, pcCode, pcMsg);
}

static int af__direct_start_engines(dm_in const af__direct_pending_st *pstPend,
                                    dm_in uint32 u32VdDispId, dm_in uint16 u16EncW,
                                    dm_in uint16 u16EncH, dm_io af__direct_ok_resp_st *pstResp)
{
  af__direct_keys_st stK;
  af_stream_start_req_st stStrmReq;
  af_stream_start_res_st stStrmRes;
  af_input_start_req_st stInReq;
  uint16 u16InPort = 0u;
  size_t uB64Len = 0u;
  int iRc = DM_SUCCESS;

  memset(&stK, 0, sizeof(stK));
  memset(&stStrmReq, 0, sizeof(stStrmReq));
  memset(&stStrmRes, 0, sizeof(stStrmRes));
  memset(&stInReq, 0, sizeof(stInReq));
  iRc = af__direct_derive_keys(&stK);

  if (DM_SUCCESS == iRc)
  {
    snprintf(stStrmReq.acDevId, sizeof(stStrmReq.acDevId), "%s", pstPend->acDevId);
    snprintf(stStrmReq.acDevName, sizeof(stStrmReq.acDevName), "%s", pstPend->acDevName);
    stStrmReq.u32DispId = u32VdDispId;
    stStrmReq.u16EncW = u16EncW;
    stStrmReq.u16EncH = u16EncH;
    stStrmReq.u16Fps = pstPend->u16Fps;
    stStrmReq.bChroma444 = pstPend->bChroma444;
    memcpy(stStrmReq.au8SidBe, stK.au8SidBe, sizeof(stK.au8SidBe));
    memcpy(stStrmReq.au8VidAuthKey, stK.au8VidAuth, sizeof(stK.au8VidAuth));
    iRc = af_stream_start(&stStrmReq, &stStrmRes);
    dm_secure_zero(&stStrmReq, sizeof(stStrmReq));
  }
  if (DM_SUCCESS == iRc)
  {
    memcpy(stInReq.au8SidBe, stK.au8SidBe, sizeof(stK.au8SidBe));
    memcpy(stInReq.au8KeyI2M, stK.au8KeyI2M, sizeof(stK.au8KeyI2M));
    memcpy(stInReq.au8KeyM2I, stK.au8KeyM2I, sizeof(stK.au8KeyM2I));
    stInReq.u32DispId = u32VdDispId;
    iRc = af_input_start(&stInReq, &u16InPort);
    dm_secure_zero(&stInReq, sizeof(stInReq));
    if (DM_SUCCESS != iRc)
    {
      af_stream_force_stop();
    }
  }
  if (DM_SUCCESS == iRc)
  {
    (void)af_dp_sid_to_hex(stK.au8SidBe, pstResp->acSidHex);
    if (0 != mbedtls_base64_encode((unsigned char *)pstResp->acMasterB64,
                                   sizeof(pstResp->acMasterB64), &uB64Len, stK.au8Master, 32u))
    {
      af_input_force_stop();
      af_stream_force_stop();
      iRc = DM_FAILD;
    }
    else
    {
      pstResp->u16VideoPort = stStrmRes.u16VideoPort;
      pstResp->u16InputPort = u16InPort;
      pstResp->u16EncW = u16EncW;
      pstResp->u16EncH = u16EncH;
      pstResp->u16Fps = pstPend->u16Fps;
      pstResp->bChroma444 = stStrmRes.bEffChroma444;
    }
  }
  dm_secure_zero(&stK, sizeof(stK));
  return iRc;
}

static void af__direct_on_vd_ready(dm_in int iErr, dm_in uint32 u32VdDispId, dm_in uint16 u16EncW,
                                   dm_in uint16 u16EncH, dm_io void *pvCtx)
{
  uint64 u64Seq = (uint64)(uintptr_t)pvCtx;
  af__direct_pending_st stPend;
  af__direct_ok_resp_st stResp;
  int iRc = DM_SUCCESS;

  if ((!g_stDirect.bPendingStart) || (u64Seq != g_stDirect.stPend.u64ReqSeq))
  {
    return;
  }
  stPend = g_stDirect.stPend;
  g_stDirect.bPendingStart = DM_FALSE;
  memset(&g_stDirect.stPend, 0, sizeof(g_stDirect.stPend));

  if (g_stAfSes.bDraining || (DM_SUCCESS != iErr) || (0u == u32VdDispId))
  {
    g_stDirect.acDirectKey[0] = '\0';
    af_https_send_err(stPend.u64ReplyId, g_stAfSes.bDraining ? 503u : 500u,
                      g_stAfSes.bDraining ? "draining" : "internal", "虚拟屏未就绪");
    return;
  }
  memset(&stResp, 0, sizeof(stResp));
  if (!af_auth_get_cert_sha256(stResp.acCertSha))
  {
    af_display_direct_stop();
    g_stDirect.acDirectKey[0] = '\0';
    af_https_send_err(stPend.u64ReplyId, 500u, "CERT_NOT_READY", "TLS 证书指纹未就绪");
    return;
  }
  iRc = af__direct_start_engines(&stPend, u32VdDispId, u16EncW, u16EncH, &stResp);
  if (DM_SUCCESS != iRc)
  {
    dm_secure_zero(&stResp, sizeof(stResp));
    af_display_direct_stop();
    g_stDirect.acDirectKey[0] = '\0';
    af__direct_reply_stream_err(stPend.u64ReplyId, iRc);
    return;
  }
  af__direct_send_start_ok(stPend.u64ReplyId, &stResp);
  dm_secure_zero(&stResp, sizeof(stResp));
  af_store_state_mut()->bUserOff = DM_FALSE;
  (void)af_store_save_async(NULL, NULL);
  af_ses_push_menu();
}

static int af__direct_parse_screen(dm_in yyjson_val *pstRoot, dm_io af__direct_start_opt_st *pstOpt)
{
  yyjson_val *pstV = yyjson_obj_get(pstRoot, "screen");
  yyjson_val *pstW = NULL;
  yyjson_val *pstH = NULL;

  if ((NULL == pstV) || yyjson_is_null(pstV))
  {
    return DM_SUCCESS;
  }
  if ((!yyjson_is_arr(pstV)) || (2u != yyjson_arr_size(pstV)))
  {
    return DM_INVALID;
  }
  pstW = yyjson_arr_get(pstV, 0u);
  pstH = yyjson_arr_get(pstV, 1u);
  if ((!yyjson_is_uint(pstW)) || (!yyjson_is_uint(pstH)) || (yyjson_get_uint(pstW) > UINT32_MAX) ||
      (yyjson_get_uint(pstH) > UINT32_MAX))
  {
    return DM_INVALID;
  }
  pstOpt->u32ScrW = (uint32)yyjson_get_uint(pstW);
  pstOpt->u32ScrH = (uint32)yyjson_get_uint(pstH);
  pstOpt->bHasScreen = DM_TRUE;
  return DM_SUCCESS;
}

static int af__direct_opt_bool(dm_in yyjson_val *pstRoot, dm_in const char *pcKey,
                               dm_out bool_t *pbOut)
{
  yyjson_val *pstV = yyjson_obj_get(pstRoot, pcKey);

  if ((NULL == pstV) || yyjson_is_null(pstV))
  {
    return DM_SUCCESS;
  }
  if (!yyjson_is_bool(pstV))
  {
    return DM_INVALID;
  }
  *pbOut = yyjson_get_bool(pstV) ? DM_TRUE : DM_FALSE;
  return DM_SUCCESS;
}

static int af__direct_parse_start_opt(dm_in yyjson_val *pstRoot,
                                      dm_out af__direct_start_opt_st *pstOpt)
{
  yyjson_val *pstV = NULL;

  memset(pstOpt, 0, sizeof(*pstOpt));
  pstOpt->u16Fps = AF_DIRECT_DEF_FPS;
  if (NULL == pstRoot)
  {
    return DM_SUCCESS;
  }
  if ((DM_SUCCESS != af__direct_opt_bool(pstRoot, "chroma444", &pstOpt->bChroma444)) ||
      (DM_SUCCESS != af__direct_opt_bool(pstRoot, "takeover", &pstOpt->bTakeover)))
  {
    return DM_INVALID;
  }
  pstV = yyjson_obj_get(pstRoot, "fps");
  if ((NULL != pstV) && (!yyjson_is_null(pstV)))
  {
    uint64 u64Fps = 0u;
    if (!yyjson_is_uint(pstV))
    {
      return DM_INVALID;
    }
    u64Fps = yyjson_get_uint(pstV);
    if ((u64Fps >= 15u) && (u64Fps <= 120u))
    {
      pstOpt->u16Fps = (uint16)u64Fps;
    }
  }
  pstV = yyjson_obj_get(pstRoot, "scale");
  if ((NULL != pstV) && (!yyjson_is_null(pstV)))
  {
    const char *pcS = yyjson_get_str(pstV);
    if ((NULL == pcS) || (af_scale_key_index(pcS) < 0))
    {
      return DM_INVALID;
    }
    snprintf(pstOpt->acScale, sizeof(pstOpt->acScale), "%s", pcS);
    pstOpt->bHasScale = DM_TRUE;
  }
  return af__direct_parse_screen(pstRoot, pstOpt);
}

static bool_t af__direct_preempt_check(dm_in uint64 u64ReplyId, dm_in const af_device_st *pstDev,
                                       dm_in bool_t bTakeover, dm_out bool_t *pbWaitStop,
                                       dm_out bool_t *pbWaitSc)
{
  af_ev_stream_st stStrm;
  af_sc_snap_st stSc;
  bool_t bScActive = DM_FALSE;

  *pbWaitStop = DM_FALSE;
  *pbWaitSc = DM_FALSE;
  af_stream_get_snapshot(&stStrm);
  if ((AF_STREAM_STOPPING == stStrm.eCur) || g_stDirect.bWaitingStop)
  {
    af_https_send_err(u64ReplyId, 503u, "stream_stopping", "直连正在停止，请稍后重试");
    return DM_FALSE;
  }
  if ((AF_STREAM_IDLE != stStrm.eCur) || g_stDirect.bPendingStart)
  {
    const char *pcOwner = g_stDirect.bPendingStart ? g_stDirect.stPend.acDevId : stStrm.acDevId;
    if ((0 != strcmp(pcOwner, pstDev->acId)) && (!bTakeover))
    {
      af_https_send_err(u64ReplyId, 409u, "stream_busy", "另一台设备正在直连显示");
      return DM_FALSE;
    }
    af_display_hold(af_ses_now_ms() + AF_DIRECT_HOLD_MS);
    af_ses_direct_stop_all(DM_TRUE);
    af_stream_get_snapshot(&stStrm);
    if (AF_STREAM_STOPPING == stStrm.eCur) { *pbWaitStop = DM_TRUE; }
  }
  af_sidecar_get_snap(&stSc);
  bScActive = ((AF_SC_SHOWING == stSc.eState) || (AF_SC_CONNECTING == stSc.eState) ||
               g_stAfSes.stFront.bActive) ? DM_TRUE : DM_FALSE;
  if (bScActive)
  {
    const char *pcScName = g_stAfSes.stFront.bActive ? g_stAfSes.stFront.acName
                           : (('\0' != stSc.acShowing[0]) ? stSc.acShowing : af_store_state()->acLastIpad);
    bool_t bMine = (('\0' != pstDev->acSidecarName[0]) && ('\0' != pcScName[0]) &&
                    (0 == strcmp(pstDev->acSidecarName, pcScName))) ? DM_TRUE : DM_FALSE;
    if ((!bMine) && (!bTakeover))
    {
      af_https_send_err(u64ReplyId, 409u, "stream_busy", "另一台设备正在随航显示");
      return DM_FALSE;
    }
    af_display_hold(af_ses_now_ms() + AF_DIRECT_HOLD_MS);
    af_ses_front_stop();
    af_store_state_mut()->bUserOff = DM_TRUE;
    af_kbd_hold(0u);
    af_kbd_notify_disconnect((AF_SC_SHOWING == stSc.eState) ? DM_TRUE : DM_FALSE);
    (void)af_sidecar_disconnect(NULL);
    *pbWaitSc = DM_TRUE;
  }
  else if (af_display_has_sidecar_disp()) { *pbWaitSc = DM_TRUE; }
  return DM_TRUE;
}

static void af__direct_abort_pending(dm_in uint uiStatus, dm_in const char *pcCode,
                                     dm_in const char *pcMsg)
{
  uint64 u64Rid = 0u;
  if (!g_stDirect.bPendingStart) { return; }
  u64Rid = g_stDirect.stPend.u64ReplyId;
  af__direct_cancel_sc_wait();
  g_stDirect.bPendingStart = DM_FALSE;
  g_stDirect.bWaitingStop = DM_FALSE;
  memset(&g_stDirect.stPend, 0, sizeof(g_stDirect.stPend));
  af_https_send_err(u64Rid, uiStatus, pcCode, pcMsg);
}

static bool_t af__direct_check_req(dm_in const af_https_msg_st *pstMsg, dm_out af_device_st *pstDev)
{
  if (g_stAfSes.bDraining) { af_https_send_err(pstMsg->u64ReplyId, 503u, "draining", "服务正在关闭"); return DM_FALSE; }
  return af_auth_check_https(pstMsg, pstDev);
}

static bool_t af__direct_check_owner(dm_in uint64 u64ReplyId, dm_in const char *pcDevId,
                                     dm_out bool_t *pbActive)
{
  af_ev_stream_st stStrm;
  af_ses_direct_get_active(&stStrm, NULL, 0u);
  if (NULL != pbActive) { *pbActive = (AF_STREAM_IDLE != stStrm.eCur) ? DM_TRUE : DM_FALSE; }
  if ((AF_STREAM_IDLE != stStrm.eCur) && ('\0' != stStrm.acDevId[0]) && (0 != strcmp(pcDevId, stStrm.acDevId)))
  {
    af_https_send_err(u64ReplyId, 403u, "forbidden", "无权操作其他设备的直连会话");
    return DM_FALSE;
  }
  return DM_TRUE;
}

static void af__direct_launch_vd(void)
{
  af_direct_vd_req_st stVdReq = {g_stDirect.stPend.acDirectKey, g_stDirect.stPend.u32ScrW,
                                 g_stDirect.stPend.u32ScrH,
                                 g_stDirect.stPend.bHasScale ? g_stDirect.stPend.acScale : NULL};
  if (DM_SUCCESS != af_display_ensure_direct_vd(&stVdReq, af__direct_on_vd_ready,
                                                (void *)(uintptr_t)g_stDirect.stPend.u64ReqSeq))
  {
    g_stDirect.acDirectKey[0] = '\0';
    af__direct_abort_pending(500u, "internal", "准备虚拟屏失败");
  }
}

static void af__on_direct_start(dm_in const af_https_msg_st *pstMsg)
{
  af_device_st stDev;
  af_env_snap_st stEnv;
  af__direct_start_opt_st stOpt;
  yyjson_doc *pstDoc = NULL;
  const char *pcKey = NULL;
  bool_t bWaitStop = DM_FALSE, bWaitSc = DM_FALSE;
  int iRc = DM_SUCCESS;

  if ((!af__direct_check_req(pstMsg, &stDev)) || (DM_SUCCESS != af_ses_read_json_obj(pstMsg, DM_TRUE, &pstDoc))) { return; }
  iRc = af__direct_parse_start_opt((NULL != pstDoc) ? yyjson_doc_get_root(pstDoc) : NULL, &stOpt);
  if (NULL != pstDoc) { yyjson_doc_free(pstDoc); }
  if (DM_SUCCESS != iRc) { af_https_send_err(pstMsg->u64ReplyId, 400u, "bad_request", "直连启动参数无效"); return; }
  af_env_get_snapshot(&stEnv);
  if ((!stEnv.bPermSc) || (!stEnv.bPermAx)) { af__direct_reply_perm_missing(pstMsg->u64ReplyId, stEnv.bPermSc, stEnv.bPermAx); return; }
  if (!af__direct_preempt_check(pstMsg->u64ReplyId, &stDev, stOpt.bTakeover, &bWaitStop, &bWaitSc)) { return; }
  af__direct_cancel_prodraw();
  pcKey = ('\0' != stDev.acSidecarName[0]) ? stDev.acSidecarName : stDev.acId;
  g_stDirect.u64NextSeq++;
  g_stDirect.bPendingStart = DM_TRUE;
  g_stDirect.bWaitingStop = bWaitStop;
  g_stDirect.bWaitingScStop = bWaitSc;
  memset(&g_stDirect.stPend, 0, sizeof(g_stDirect.stPend));
  g_stDirect.stPend.u64ReplyId = pstMsg->u64ReplyId;
  g_stDirect.stPend.u64ReqSeq = g_stDirect.u64NextSeq;
  snprintf(g_stDirect.stPend.acDevId, sizeof(g_stDirect.stPend.acDevId), "%s", stDev.acId);
  snprintf(g_stDirect.stPend.acDevName, sizeof(g_stDirect.stPend.acDevName), "%s", stDev.acName);
  snprintf(g_stDirect.stPend.acDirectKey, sizeof(g_stDirect.stPend.acDirectKey), "%s", pcKey);
  snprintf(g_stDirect.stPend.acScale, sizeof(g_stDirect.stPend.acScale), "%s", stOpt.acScale);
  g_stDirect.stPend.bHasScale = stOpt.bHasScale;
  g_stDirect.stPend.u32ScrW = stOpt.bHasScreen ? stOpt.u32ScrW : 0u;
  g_stDirect.stPend.u32ScrH = stOpt.bHasScreen ? stOpt.u32ScrH : 0u;
  g_stDirect.stPend.u16Fps = stOpt.u16Fps;
  g_stDirect.stPend.bChroma444 = stOpt.bChroma444;
  snprintf(g_stDirect.acDirectKey, sizeof(g_stDirect.acDirectKey), "%s", pcKey);
  if ((!bWaitStop) && (!bWaitSc)) { af__direct_launch_vd(); }
  else if (bWaitSc)
  {
    af__direct_try_resume_sc();
    if (g_stDirect.bWaitingScStop && (0u == g_stDirect.u32ScWaitTimer))
    {
      if (DM_SUCCESS != dm_timer_add(AF_DIRECT_SC_WAIT_MS, 0u, AF_SES_LINK_SC_WAIT_TMR, 0u, &g_stDirect.u32ScWaitTimer))
      {
        (void)af__direct_on_sc_wait_tmr(NULL);
      }
    }
  }
}

static void af__on_direct_stop(dm_in const af_https_msg_st *pstMsg)
{
  af_device_st stDev;
  bool_t bActive = DM_FALSE;

  if ((!af__direct_check_req(pstMsg, &stDev)) ||
      (!af__direct_check_owner(pstMsg->u64ReplyId, stDev.acId, &bActive)))
  {
    return;
  }
  if (bActive)
  {
    af_ses_direct_stop_all(DM_FALSE);
  }
  af_ses_push_menu();
  af_https_send_ok(pstMsg->u64ReplyId, "{\"ok\":true}");
}

static void af__on_direct_switch_prodraw(dm_in const af_https_msg_st *pstMsg)
{
  af_device_st stDev;
  af_sc_snap_st stSc;
  yyjson_doc *pstDoc = NULL;
  char acReqName[65] = {0};
  char acTarget[65] = {0};
  bool_t bKbd = DM_FALSE;
  int iRc = DM_SUCCESS;

  if ((!af__direct_check_req(pstMsg, &stDev)) ||
      (DM_SUCCESS != af_ses_read_json_obj(pstMsg, DM_TRUE, &pstDoc)))
  {
    return;
  }
  if (NULL != pstDoc)
  {
    yyjson_val *pstRoot = yyjson_doc_get_root(pstDoc);
    yyjson_val *pstN = yyjson_obj_get(pstRoot, "sidecar_name");
    yyjson_val *pstK = yyjson_obj_get(pstRoot, "keyboard");
    if ((NULL != pstN) && yyjson_is_str(pstN))
    {
      snprintf(acReqName, sizeof(acReqName), "%s", yyjson_get_str(pstN));
    }
    if ((NULL != pstK) && yyjson_is_bool(pstK))
    {
      bKbd = yyjson_get_bool(pstK) ? DM_TRUE : DM_FALSE;
    }
    yyjson_doc_free(pstDoc);
  }
  if (!af__direct_check_owner(pstMsg->u64ReplyId, stDev.acId, NULL))
  {
    return;
  }
  af_sidecar_get_snap(&stSc);
  iRc = af_ses_resolve_connect_target(acReqName, &stDev, &stSc, acTarget, sizeof(acTarget));
  if ((AF_ERR_NO_IPAD == iRc) || (AF_ERR_CHOOSE_IPAD == iRc))
  {
    af_https_send_err(pstMsg->u64ReplyId, 409u, (AF_ERR_NO_IPAD == iRc) ? "no_ipad" : "choose_ipad",
                      (AF_ERR_NO_IPAD == iRc) ? "附近没找到 iPad" : "附近有多台 iPad，请选择");
    return;
  }
  af_display_hold(af_ses_now_ms() + AF_DIRECT_HOLD_MS);
  af_ses_direct_stop_all(DM_TRUE);
  if (0 != strcmp(stDev.acSidecarName, acTarget))
  {
    (void)af_auth_bind_sidecar(stDev.acId, acTarget);
  }
  snprintf(af_store_state_mut()->acLastIpad, sizeof(af_store_state_mut()->acLastIpad), "%s",
           acTarget);
  af_store_state_mut()->bUserOff = DM_FALSE;
  (void)af_store_save_async(NULL, NULL);
  g_stDirect.bPendingProdraw = DM_TRUE;
  g_stDirect.bProdrawKbd = bKbd;
  snprintf(g_stDirect.acProdrawTarget, sizeof(g_stDirect.acProdrawTarget), "%s", acTarget);
  {
    af_ev_stream_st stCurStrm;
    af_stream_get_snapshot(&stCurStrm);
    if (AF_STREAM_IDLE == stCurStrm.eCur)
    {
      af__direct_arm_prodraw();
    }
  }
  af_ses_push_menu();
  af_https_send_ok(pstMsg->u64ReplyId, "{\"ok\":true,\"stage\":\"switching\"}");
}

/* ============================ 内部协作接口 ============================ */

int af_ses_direct_register(void)
{
  return ((DM_SUCCESS ==
           dm_link_register_handler(AF_SES_LINK_PRODRAW_TMR, af__direct_on_prodraw_tmr, NULL, NULL)) &&
          (DM_SUCCESS ==
           dm_link_register_handler(AF_SES_LINK_SC_WAIT_TMR, af__direct_on_sc_wait_tmr, NULL, NULL)) &&
          (DM_SUCCESS == af_https_route_exact("POST", "/direct/start", af__on_direct_start)) &&
          (DM_SUCCESS == af_https_route_exact("POST", "/direct/stop", af__on_direct_stop)) &&
          (DM_SUCCESS ==
           af_https_route_exact("POST", "/direct/switch_prodraw", af__on_direct_switch_prodraw)))
             ? DM_SUCCESS : DM_FAILD;
}

void af_ses_direct_init(void) { memset(&g_stDirect, 0, sizeof(g_stDirect)); }

void af_ses_direct_drain(void)
{
  af__direct_cancel_sc_wait();
  af__direct_cancel_prodraw();
  af__direct_abort_pending(503u, "draining", "服务正在关闭");
  g_stDirect.acDirectKey[0] = '\0';
}

void af_ses_direct_fini(void)
{
  af__direct_cancel_sc_wait();
  af__direct_cancel_prodraw();
  memset(&g_stDirect, 0, sizeof(g_stDirect));
}

bool_t af_ses_direct_is_active(void)
{
  af_ev_stream_st stStrm;

  if (g_stDirect.bPendingStart) { return DM_TRUE; }
  af_stream_get_snapshot(&stStrm);
  return (AF_STREAM_IDLE != stStrm.eCur) ? DM_TRUE : DM_FALSE;
}

bool_t af_ses_direct_is_switching_prodraw(void) { return g_stDirect.bPendingProdraw; }

void af_ses_direct_get_active(dm_out af_ev_stream_st *pstOut, dm_out char *pcKeyOut,
                              dm_in size_t uKeyCap)
{
  if (NULL != pstOut)
  {
    af_stream_get_snapshot(pstOut);
    if ((AF_STREAM_IDLE == pstOut->eCur) && g_stDirect.bPendingStart)
    {
      pstOut->eCur = AF_STREAM_STARTING;
      snprintf(pstOut->acDevId, sizeof(pstOut->acDevId), "%s", g_stDirect.stPend.acDevId);
      snprintf(pstOut->acDevName, sizeof(pstOut->acDevName), "%s", g_stDirect.stPend.acDevName);
    }
  }
  if ((NULL != pcKeyOut) && (uKeyCap > 0u)) { snprintf(pcKeyOut, uKeyCap, "%s", g_stDirect.acDirectKey); }
}

void af_ses_direct_stop_all(dm_in bool_t bHoldVd)
{
  if (!bHoldVd) { af_display_hold(0u); }
  af__direct_cancel_sc_wait();
  af__direct_cancel_prodraw();
  af__direct_abort_pending(409u, "stream_busy", "直连启动已被中断");
  af_input_force_stop();
  af_stream_force_stop();
  af_display_direct_stop();
  g_stDirect.acDirectKey[0] = '\0';
}

void af_ses_direct_on_sc_or_disp_ev(void) { af__direct_try_resume_sc(); }

void af_ses_direct_on_revoked(dm_in const char *pcDevId)
{
  af_ev_stream_st stStrm;
  if ((NULL == pcDevId) || ('\0' == pcDevId[0])) { return; }
  af_ses_direct_get_active(&stStrm, NULL, 0u);
  if ((AF_STREAM_IDLE != stStrm.eCur) && (0 == strcmp(stStrm.acDevId, pcDevId))) { af_ses_direct_stop_all(DM_FALSE); }
}

void af_ses_direct_on_stream_ev(dm_in const af_ev_stream_st *pstEv)
{
  if (NULL == pstEv) { return; }
  if (AF_STREAM_STOPPING == pstEv->eCur) { af_input_force_stop(); }
  else if (AF_STREAM_IDLE == pstEv->eCur)
  {
    if (g_stDirect.bPendingStart && g_stDirect.bWaitingStop)
    {
      g_stDirect.bWaitingStop = DM_FALSE;
      if (!g_stDirect.bWaitingScStop) { af__direct_launch_vd(); }
      return;
    }
    if (g_stDirect.bPendingProdraw) { af__direct_arm_prodraw(); af_ses_push_menu(); return; }
    if ((AF_STREAM_IDLE != pstEv->ePrev) && (!g_stDirect.bPendingStart))
    {
      af_input_force_stop();
      af_display_hold(0u); af_display_direct_stop();
      g_stDirect.acDirectKey[0] = '\0';
    }
  }
  af_ses_push_menu();
}
