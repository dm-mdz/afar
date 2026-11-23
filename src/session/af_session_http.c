/**
 ******************************************************************************
 * @file    af_session_http.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   af_session 6 条业务 HTTPS 路由实现（/status /connect /disconnect /front /settings /restart）。
 ******************************************************************************
 */

#define _DARWIN_C_SOURCE
#define DM_LOG_MOD "session"

#include "af_session_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct af__http_save_ctx
{
  uint64 u64ReplyId;
} af__http_save_ctx_st;

static bool_t af__json_depth_ok(in const char *pcText, in size_t uLen, in uint32 u32MaxDepth)
{
  uint32 u32Depth = 0u;
  bool_t bInStr = DM_FALSE;
  bool_t bEsc = DM_FALSE;
  size_t i = 0u;

  for (i = 0u; i < uLen; i++)
  {
    char c = pcText[i];
    if (bInStr)
    {
      if (bEsc) { bEsc = DM_FALSE; }
      else if ('\\' == c) { bEsc = DM_TRUE; }
      else if ('"' == c) { bInStr = DM_FALSE; }
      continue;
    }
    if ('"' == c) { bInStr = DM_TRUE; }
    else if (('{' == c) || ('[' == c))
    {
      u32Depth++;
      if (u32Depth > u32MaxDepth) { return DM_FALSE; }
    }
    else if ((('}' == c) || (']' == c)) && (u32Depth > 0u)) { u32Depth--; }
  }
  return DM_TRUE;
}

static bool_t af__is_blank(in const char *pcText, in size_t uLen)
{
  size_t i = 0u;
  for (i = 0u; i < uLen; i++)
  {
    if (!isspace((unsigned char)pcText[i])) { return DM_FALSE; }
  }
  return DM_TRUE;
}

static int af__read_json_obj(in const dm_https_msg_st *pstMsg, in bool_t bAllowEmpty,
                             out yyjson_doc **ppstDoc)
{
  const char *pcBody = (const char *)pstMsg->pu8Body;
  int iErr = DM_SUCCESS;
  yyjson_doc *pstDoc = NULL;

  *ppstDoc = NULL;
  if ((0u == pstMsg->u32BodyLen) || (NULL == pcBody) || af__is_blank(pcBody, pstMsg->u32BodyLen))
  {
    if (bAllowEmpty) { return DM_SUCCESS; }
    af_https_send_err(pstMsg->u64ReplyId, 400u, "bad_request", "请求体不能为空");
    return DM_INVALID;
  }
  if (!af__json_depth_ok(pcBody, pstMsg->u32BodyLen, AF_SES_JSON_MAX_DEPTH))
  {
    af_https_send_err(pstMsg->u64ReplyId, 400u, "bad_request", "JSON 嵌套层数超限");
    return DM_INVALID;
  }
  pstDoc = dm_json_read(pcBody, pstMsg->u32BodyLen, 65536u, &iErr);
  if ((NULL == pstDoc) || (!yyjson_is_obj(yyjson_doc_get_root(pstDoc))))
  {
    if (NULL != pstDoc) { yyjson_doc_free(pstDoc); }
    af_https_send_err(pstMsg->u64ReplyId, 400u, "bad_request", "请求 JSON 无效");
    return DM_INVALID;
  }
  *ppstDoc = pstDoc;
  return DM_SUCCESS;
}

static size_t af__trim_trunc_utf8(in const char *pcSrc, out char *pcDst, in size_t uDstCap,
                                  in size_t uMaxBytes)
{
  const char *pcS = pcSrc;
  const char *pcE = NULL;
  char acSlice[256];
  size_t uLen = 0u;

  if ((NULL == pcDst) || (0u == uDstCap)) { return 0u; }
  pcDst[0] = '\0';
  if (NULL == pcS) { return 0u; }
  while (('\0' != *pcS) && isspace((unsigned char)*pcS)) { pcS++; }
  pcE = pcS + strlen(pcS);
  while ((pcE > pcS) && isspace((unsigned char)pcE[-1])) { pcE--; }
  uLen = (size_t)(pcE - pcS);
  if (0u == uLen) { return 0u; }
  if (uLen >= sizeof(acSlice)) { uLen = sizeof(acSlice) - 1u; }
  memcpy(acSlice, pcS, uLen);
  acSlice[uLen] = '\0';
  return af_session_trunc_utf8_bytes(acSlice, pcDst, uDstCap, uMaxBytes);
}

static void af__https_save_reply_cb(in int iStatus, io void *pvCtx)
{
  af__http_save_ctx_st *pstCtx = (af__http_save_ctx_st *)pvCtx;
  uint64 u64Rid = pstCtx->u64ReplyId;

  dm_free_tag(pstCtx);
  af_ses_save_inflight_dec();
  if (DM_SUCCESS == iStatus) { af_https_send_ok(u64Rid, "{\"ok\":true}"); }
  else { af_https_send_err(u64Rid, 500u, "internal", "状态保存失败"); }
}

static const char *af__bt_stage_str(in af_bt_pair_stage_et eStage)
{
  switch (eStage)
  {
    case AF_BT_PAIR_SEARCHING: return "searching";
    case AF_BT_PAIR_SEARCHED: return "searched";
    case AF_BT_PAIR_PAIRING: return "pairing";
    case AF_BT_PAIR_DONE: return "done";
    case AF_BT_PAIR_FAILED: return "failed";
    default: return "idle";
  }
}

static void af__add_opt_str(io yyjson_mut_doc *pstDoc, io yyjson_mut_val *pstObj,
                            in const char *pcKey, in const char *pcVal)
{
  if ((NULL != pcVal) && ('\0' != pcVal[0]))
  {
    yyjson_mut_obj_add_strcpy(pstDoc, pstObj, pcKey, pcVal);
  }
  else
  {
    yyjson_mut_obj_add_null(pstDoc, pstObj, pcKey);
  }
}

static yyjson_mut_val *af__make_bt_dev_obj(io yyjson_mut_doc *pstDoc, in const af_bt_device_st *pstD)
{
  yyjson_mut_val *pstO = yyjson_mut_obj(pstDoc);
  yyjson_mut_obj_add_strcpy(pstDoc, pstO, "address", pstD->acAddr);
  yyjson_mut_obj_add_strcpy(pstDoc, pstO, "name", pstD->acName);
  yyjson_mut_obj_add_bool(pstDoc, pstO, "connected", pstD->bConnected ? true : false);
  yyjson_mut_obj_add_bool(pstDoc, pstO, "ble", pstD->bBle ? true : false);
  return pstO;
}

static void af__status_add_kbd(io yyjson_mut_doc *pstDoc, io yyjson_mut_val *pstRoot,
                               in const af_kbd_snap_st *pstKbd)
{
  if (pstKbd->bHasSelected)
  {
    yyjson_mut_obj_add_val(pstDoc, pstRoot, "keyboard",
                           af__make_bt_dev_obj(pstDoc, &pstKbd->stSelected));
  }
  else
  {
    yyjson_mut_obj_add_null(pstDoc, pstRoot, "keyboard");
  }
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "keyboard_auto", pstKbd->bAuto ? true : false);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "keyboard_taking", pstKbd->bTaking ? true : false);
  af__add_opt_str(pstDoc, pstRoot, "keyboard_error", pstKbd->acError);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "keyboard_released", pstKbd->bReleased ? true : false);
  if (pstKbd->stPair.bActive && (AF_BT_PAIR_IDLE != pstKbd->stPair.eStage))
  {
    yyjson_mut_val *pstP = yyjson_mut_obj(pstDoc);
    yyjson_mut_val *pstFound = yyjson_mut_arr(pstDoc);
    uint32 i = 0u;
    yyjson_mut_obj_add_str(pstDoc, pstP, "stage", af__bt_stage_str(pstKbd->stPair.eStage));
    for (i = 0u; (i < pstKbd->stPair.u32FoundCnt) && (i < AF_BT_DEV_MAX); i++)
    {
      yyjson_mut_arr_add_val(pstFound, af__make_bt_dev_obj(pstDoc, &pstKbd->stPair.astFound[i]));
    }
    yyjson_mut_obj_add_val(pstDoc, pstP, "found", pstFound);
    af__add_opt_str(pstDoc, pstP, "address", pstKbd->stPair.acAddr);
    af__add_opt_str(pstDoc, pstP, "code", pstKbd->stPair.acCode);
    af__add_opt_str(pstDoc, pstP, "message", pstKbd->stPair.acMsg);
    yyjson_mut_obj_add_val(pstDoc, pstRoot, "keyboard_pair", pstP);
  }
  else
  {
    yyjson_mut_obj_add_null(pstDoc, pstRoot, "keyboard_pair");
  }
}

static void af__status_add_env_disp(io yyjson_mut_doc *pstDoc, io yyjson_mut_val *pstRoot,
                                    in const af_env_snap_st *pstEnv,
                                    in const af_disp_snap_st *pstDisp, in const char *pcScale)
{
  yyjson_mut_val *pstScales = yyjson_mut_arr(pstDoc);
  yyjson_mut_val *pstKeys = yyjson_mut_arr(pstDoc);
  char acUser[64] = {0};
  uint32 i = 0u;

  yyjson_mut_obj_add_strcpy(pstDoc, pstRoot, "scale", (NULL != pcScale) ? pcScale : "default");
  for (i = 0u; i < AF_SCALE_COUNT; i++)
  {
    yyjson_mut_val *pstItem = yyjson_mut_obj(pstDoc);
    yyjson_mut_obj_add_str(pstDoc, pstItem, "key", pstDisp->astScales[i].pcKey);
    yyjson_mut_obj_add_str(pstDoc, pstItem, "label", pstDisp->astScales[i].pcLabel);
    yyjson_mut_obj_add_uint(pstDoc, pstItem, "w", pstDisp->astScales[i].u32W);
    yyjson_mut_obj_add_uint(pstDoc, pstItem, "h", pstDisp->astScales[i].u32H);
    yyjson_mut_arr_add_val(pstScales, pstItem);
  }
  yyjson_mut_obj_add_val(pstDoc, pstRoot, "scales", pstScales);
  if ('\0' != pstEnv->acUser[0]) { snprintf(acUser, sizeof(acUser), "%s", pstEnv->acUser); }
  else { af_ses_ops()->pfCurrentUser(acUser, sizeof(acUser)); }
  yyjson_mut_obj_add_strcpy(pstDoc, pstRoot, "user", acUser);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "filevault", pstEnv->bFileVault ? true : false);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "remote_login", pstEnv->bRemoteLogin ? true : false);
  for (i = 0u; (i < pstEnv->u32SshKeyCnt) && (i < AF_SSH_KEYS_MAX); i++)
  {
    yyjson_mut_arr_add_strcpy(pstDoc, pstKeys, pstEnv->aacSshKeys[i]);
  }
  yyjson_mut_obj_add_val(pstDoc, pstRoot, "ssh_host_keys", pstKeys);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "auto_login", pstEnv->bAutoLogin ? true : false);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "auto_restart", pstEnv->bAutoRestart ? true : false);
  af__add_opt_str(pstDoc, pstRoot, "scale_error", pstDisp->acScaleError);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "scale_busy", pstDisp->bScaleBusy ? true : false);
  af__add_opt_str(pstDoc, pstRoot, "restart_error", g_stAfSes.acRestartError);
}

static void af__status_eval_owner(in const af_device_st *pstDev, in const af_sc_snap_st *pstSc,
                                  in bool_t bShowingState, out bool_t *pbMe, out bool_t *pbOther)
{
  *pbMe = DM_FALSE;
  *pbOther = DM_FALSE;
  if ((!bShowingState) || ('\0' == pstSc->acShowing[0])) { return; }
  if ('\0' != pstDev->acSidecarName[0])
  {
    if (0 == strcmp(pstDev->acSidecarName, pstSc->acShowing)) { *pbMe = DM_TRUE; }
    else { *pbOther = DM_TRUE; }
    return;
  }
  if (af_auth_other_owns_sidecar(pstDev->acId, pstSc->acShowing)) { *pbOther = DM_TRUE; }
}

static const char *af__resolve_scale_target(in const af_device_st *pstDev,
                                            in const af_sc_snap_st *pstSc)
{
  const af_state_st *pstSt = af_store_state();
  if ((AF_SC_SHOWING == pstSc->eState) && ('\0' != pstSc->acShowing[0])) { return pstSc->acShowing; }
  if ('\0' != pstDev->acSidecarName[0]) { return pstDev->acSidecarName; }
  if ('\0' != pstSt->acLastIpad[0]) { return pstSt->acLastIpad; }
  return NULL;
}

static void af__on_status(in const dm_https_msg_st *pstMsg)
{
  af_device_st stDev;
  af_sc_snap_st stSc;
  af_disp_snap_st stDisp;
  af_env_snap_st stEnv;
  af_kbd_snap_st stKbd;
  char acName[65];
  const char *pcScaleTarget = NULL;
  const char *pcScale = NULL;
  const char *pcState = "online";
  bool_t bShowingState = DM_FALSE;
  bool_t bMe = DM_FALSE;
  bool_t bOther = DM_FALSE;
  yyjson_mut_doc *pstDoc = NULL;
  yyjson_mut_val *pstRoot = NULL;
  yyjson_mut_val *pstNearby = NULL;
  char *pcJson = NULL;
  size_t uLen = 0u;
  uint32 i = 0u;

  if (!af_auth_check_https(pstMsg, &stDev)) { return; }
  af_sidecar_get_snap(&stSc);
  pcScaleTarget = af__resolve_scale_target(&stDev, &stSc);
  af_display_get_snap(pcScaleTarget, &stDisp);
  af_env_get_snapshot(&stEnv);
  af_kbd_get_snap(&stKbd);
  af_ses_get_mac_name(acName, sizeof(acName));

  bShowingState = ((AF_SC_SHOWING == stSc.eState) && (!g_stAfSes.stFront.bActive)) ? DM_TRUE : DM_FALSE;
  if (g_stAfSes.stFront.bActive || (AF_SC_CONNECTING == stSc.eState)) { pcState = "connecting"; }
  else if (bShowingState) { pcState = "showing"; }
  af__status_eval_owner(&stDev, &stSc, bShowingState, &bMe, &bOther);
  pcScale = ('\0' != stDisp.acActualScale[0]) ? stDisp.acActualScale
                                              : af_disp_store_target_scale(pcScaleTarget);

  pstDoc = yyjson_mut_doc_new(dm_json_alc());
  if (NULL == pstDoc) { af_https_send_err(pstMsg->u64ReplyId, 500u, "internal", "内存不足"); return; }
  pstRoot = yyjson_mut_obj(pstDoc);
  yyjson_mut_doc_set_root(pstDoc, pstRoot);
  yyjson_mut_obj_add_strcpy(pstDoc, pstRoot, "name", acName);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "version", AF_VERSION);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "state", pcState);
  af__add_opt_str(pstDoc, pstRoot, "showing", bShowingState ? stSc.acShowing : NULL);
  pstNearby = yyjson_mut_arr(pstDoc);
  for (i = 0u; (i < stSc.u32NearbyCnt) && (i < AF_SC_DEV_MAX); i++)
  {
    yyjson_mut_arr_add_strcpy(pstDoc, pstNearby, stSc.aacNearby[i]);
  }
  yyjson_mut_obj_add_val(pstDoc, pstRoot, "nearby", pstNearby);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "password_saved", stDev.bPasswordSaved ? true : false);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "showing_me", bMe ? true : false);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "showing_other", bOther ? true : false);
  af__status_add_env_disp(pstDoc, pstRoot, &stEnv, &stDisp, pcScale);
  af__status_add_kbd(pstDoc, pstRoot, &stKbd);
  pcJson = dm_json_write_mut(pstDoc, &uLen);
  yyjson_mut_doc_free(pstDoc);
  if (NULL != pcJson)
  {
    (void)dm_https_reply(pstMsg->u64ReplyId, 200u, pcJson, (uint32)uLen);
    dm_json_free_str(pcJson);
  }
  else
  {
    af_https_send_err(pstMsg->u64ReplyId, 500u, "internal", "序列化状态失败");
  }
}

static int af__parse_connect_req(in yyjson_val *pstRoot, out char *pcSidecar, in size_t uCap,
                                 out bool_t *pbKbd, out bool_t *pbAuto, out bool_t *pbHasScr,
                                 out uint32 *pu32W, out uint32 *pu32H)
{
  yyjson_val *pstV = NULL;

  pcSidecar[0] = '\0';
  *pbKbd = DM_FALSE;
  *pbAuto = DM_FALSE;
  *pbHasScr = DM_FALSE;
  *pu32W = 0u;
  *pu32H = 0u;

  pstV = yyjson_obj_get(pstRoot, "sidecar_name");
  if ((NULL != pstV) && (!yyjson_is_null(pstV)))
  {
    if (!yyjson_is_str(pstV)) { return DM_INVALID; }
    (void)af__trim_trunc_utf8(yyjson_get_str(pstV), pcSidecar, uCap, 64u);
  }
  pstV = yyjson_obj_get(pstRoot, "keyboard");
  if ((NULL != pstV) && (!yyjson_is_null(pstV)))
  {
    if (!yyjson_is_bool(pstV)) { return DM_INVALID; }
    *pbKbd = yyjson_get_bool(pstV) ? DM_TRUE : DM_FALSE;
  }
  pstV = yyjson_obj_get(pstRoot, "auto");
  if ((NULL != pstV) && (!yyjson_is_null(pstV)))
  {
    if (!yyjson_is_bool(pstV)) { return DM_INVALID; }
    *pbAuto = yyjson_get_bool(pstV) ? DM_TRUE : DM_FALSE;
  }
  pstV = yyjson_obj_get(pstRoot, "screen");
  if ((NULL != pstV) && (!yyjson_is_null(pstV)))
  {
    yyjson_val *pstW = NULL;
    yyjson_val *pstH = NULL;
    if ((!yyjson_is_arr(pstV)) || (2u != yyjson_arr_size(pstV))) { return DM_INVALID; }
    pstW = yyjson_arr_get(pstV, 0u);
    pstH = yyjson_arr_get(pstV, 1u);
    if ((!yyjson_is_uint(pstW)) || (!yyjson_is_uint(pstH))) { return DM_INVALID; }
    {
      uint64 u64W = yyjson_get_uint(pstW);
      uint64 u64H = yyjson_get_uint(pstH);
      if ((u64W > UINT32_MAX) || (u64H > UINT32_MAX)) { return DM_INVALID; }
      *pu32W = (uint32)u64W;
      *pu32H = (uint32)u64H;
    }
    *pbHasScr = DM_TRUE;
  }
  return DM_SUCCESS;
}

static int af__resolve_connect_target(in const char *pcReqName, in const af_device_st *pstDev,
                                      in const af_sc_snap_st *pstSc, out char *pcOut,
                                      in size_t uCap)
{
  if ((NULL != pcReqName) && ('\0' != pcReqName[0]))
  {
    snprintf(pcOut, uCap, "%s", pcReqName);
    return DM_SUCCESS;
  }
  if ('\0' != pstDev->acSidecarName[0])
  {
    snprintf(pcOut, uCap, "%s", pstDev->acSidecarName);
    return DM_SUCCESS;
  }
  if (1u == pstSc->u32NearbyCnt)
  {
    snprintf(pcOut, uCap, "%s", pstSc->aacNearby[0]);
    return DM_SUCCESS;
  }
  if (0u == pstSc->u32NearbyCnt) { return AF_ERR_NO_IPAD; }
  return AF_ERR_CHOOSE_IPAD;
}

static void af__on_connect(in const dm_https_msg_st *pstMsg)
{
  af_device_st stDev;
  af_sc_snap_st stSc;
  yyjson_doc *pstDoc = NULL;
  char acReqName[65];
  char acTarget[65];
  bool_t bKbd = DM_FALSE;
  bool_t bAuto = DM_FALSE;
  bool_t bHasScr = DM_FALSE;
  uint32 u32W = 0u;
  uint32 u32H = 0u;
  int iRc = DM_SUCCESS;

  if (g_stAfSes.bDraining)
  {
    af_https_send_err(pstMsg->u64ReplyId, 503u, "draining", "服务正在关闭");
    return;
  }
  if (!af_auth_check_https(pstMsg, &stDev)) { return; }
  if (DM_SUCCESS != af__read_json_obj(pstMsg, DM_FALSE, &pstDoc)) { return; }
  iRc = af__parse_connect_req(yyjson_doc_get_root(pstDoc), acReqName, sizeof(acReqName), &bKbd,
                              &bAuto, &bHasScr, &u32W, &u32H);
  yyjson_doc_free(pstDoc);
  if (DM_SUCCESS != iRc)
  {
    af_https_send_err(pstMsg->u64ReplyId, 400u, "bad_request", "连接参数无效");
    return;
  }
  af_sidecar_get_snap(&stSc);
  iRc = af__resolve_connect_target(acReqName, &stDev, &stSc, acTarget, sizeof(acTarget));
  if (AF_ERR_NO_IPAD == iRc)
  {
    af_https_send_err(pstMsg->u64ReplyId, 409u, "no_ipad", "附近没找到 iPad");
    return;
  }
  if (AF_ERR_CHOOSE_IPAD == iRc)
  {
    af_https_send_err(pstMsg->u64ReplyId, 409u, "choose_ipad", "附近有多台 iPad，请选择");
    return;
  }
  if (bAuto && (af_store_state()->bUserOff || af_sidecar_in_cooldown()))
  {
    af_https_send_ok(pstMsg->u64ReplyId, "{\"ok\":true}");
    return;
  }
  af_ses_front_stop();
  if (0 != strcmp(stDev.acSidecarName, acTarget)) { (void)af_auth_bind_sidecar(stDev.acId, acTarget); }
  snprintf(af_store_state_mut()->acLastIpad, sizeof(af_store_state_mut()->acLastIpad), "%s", acTarget);
  af_store_state_mut()->bUserOff = DM_FALSE;
  (void)af_store_save_async(NULL, NULL);
  if (bHasScr) { af_display_set_ipad_screen(acTarget, u32W, u32H); }
  af_kbd_notify_connect(acTarget, bKbd);
  (void)af_sidecar_connect(acTarget, DM_TRUE);
  af_ses_push_menu();
  af_https_send_ok(pstMsg->u64ReplyId, "{\"ok\":true}");
}

static void af__on_disconnect(in const dm_https_msg_st *pstMsg)
{
  af_device_st stDev;
  af__http_save_ctx_st *pstCtx = NULL;

  if (g_stAfSes.bDraining)
  {
    af_https_send_err(pstMsg->u64ReplyId, 503u, "draining", "服务正在关闭");
    return;
  }
  if (!af_auth_check_https(pstMsg, &stDev)) { return; }
  pstCtx = (af__http_save_ctx_st *)dm_calloc_tag(1u, sizeof(*pstCtx), AF_SES_TAG_SAVE);
  if (NULL == pstCtx)
  {
    af_ses_disconnect_core(NULL, NULL);
    af_https_send_ok(pstMsg->u64ReplyId, "{\"ok\":true}");
    return;
  }
  pstCtx->u64ReplyId = pstMsg->u64ReplyId;
  af_ses_save_inflight_inc();
  af_ses_disconnect_core(af__https_save_reply_cb, pstCtx);
}

static void af__on_front(in const dm_https_msg_st *pstMsg)
{
  af_device_st stDev;

  if (g_stAfSes.bDraining)
  {
    af_https_send_err(pstMsg->u64ReplyId, 503u, "draining", "服务正在关闭");
    return;
  }
  if (!af_auth_check_https(pstMsg, &stDev)) { return; }
  if ('\0' == stDev.acSidecarName[0])
  {
    af_https_send_err(pstMsg->u64ReplyId, 409u, "no_ipad", "这台 iPad 尚未绑定随航名称");
    return;
  }
  af_ses_front_begin(stDev.acSidecarName);
  af_https_send_ok(pstMsg->u64ReplyId, "{\"ok\":true}");
}

static int af__parse_settings_req(in yyjson_val *pstRoot, out bool_t *pbHasScale, out char *pcScale,
                                  in size_t uScaleCap, out bool_t *pbHasName, out char *pcName,
                                  in size_t uNameCap)
{
  yyjson_val *pstV = NULL;

  *pbHasScale = DM_FALSE;
  *pbHasName = DM_FALSE;
  pcScale[0] = '\0';
  pcName[0] = '\0';

  pstV = yyjson_obj_get(pstRoot, "scale");
  if ((NULL != pstV) && (!yyjson_is_null(pstV)))
  {
    const char *pcS = yyjson_get_str(pstV);
    if ((NULL == pcS) || (af_scale_key_index(pcS) < 0)) { return DM_INVALID; }
    snprintf(pcScale, uScaleCap, "%s", pcS);
    *pbHasScale = DM_TRUE;
  }
  pstV = yyjson_obj_get(pstRoot, "name");
  if ((NULL != pstV) && (!yyjson_is_null(pstV)))
  {
    if (!yyjson_is_str(pstV)) { return DM_INVALID; }
    if (0u == af__trim_trunc_utf8(yyjson_get_str(pstV), pcName, uNameCap, AF_NAME_MAX_BYTES))
    {
      return DM_INVALID;
    }
    *pbHasName = DM_TRUE;
  }
  return DM_SUCCESS;
}

static void af__on_settings(in const dm_https_msg_st *pstMsg)
{
  af_device_st stDev;
  af_sc_snap_st stSc;
  yyjson_doc *pstDoc = NULL;
  bool_t bHasScale = DM_FALSE;
  bool_t bHasName = DM_FALSE;
  char acScale[16];
  char acName[65];
  int iRc = DM_SUCCESS;

  if (g_stAfSes.bDraining)
  {
    af_https_send_err(pstMsg->u64ReplyId, 503u, "draining", "服务正在关闭");
    return;
  }
  if (!af_auth_check_https(pstMsg, &stDev)) { return; }
  if (DM_SUCCESS != af__read_json_obj(pstMsg, DM_FALSE, &pstDoc)) { return; }
  iRc = af__parse_settings_req(yyjson_doc_get_root(pstDoc), &bHasScale, acScale, sizeof(acScale),
                               &bHasName, acName, sizeof(acName));
  yyjson_doc_free(pstDoc);
  if (DM_SUCCESS != iRc)
  {
    af_https_send_err(pstMsg->u64ReplyId, 400u, "bad_request", "设置参数无效");
    return;
  }
  if (bHasScale)
  {
    af_sidecar_get_snap(&stSc);
    iRc = af_display_set_scale(af__resolve_scale_target(&stDev, &stSc), acScale);
    if (DM_INVALID == iRc)
    {
      af_https_send_err(pstMsg->u64ReplyId, 400u, "bad_request", "无效的显示档位");
      return;
    }
    if (DM_BUSY == iRc)
    {
      af_https_send_err(pstMsg->u64ReplyId, 409u, "bad_request", "正在调整显示大小");
      return;
    }
  }
  if (bHasName)
  {
    uint16 u16Port = dm_https_get_port();
    if (0u == u16Port) { u16Port = af_store_cfg()->u16Port; }
    snprintf(af_store_state_mut()->acName, sizeof(af_store_state_mut()->acName), "%s", acName);
    (void)af_ses_ops()->pfBonjourStart(acName, u16Port);
  }
  if (bHasName || bHasScale)
  {
    af__http_save_ctx_st *pstCtx =
        (af__http_save_ctx_st *)dm_calloc_tag(1u, sizeof(*pstCtx), AF_SES_TAG_SAVE);
    if (NULL != pstCtx)
    {
      pstCtx->u64ReplyId = pstMsg->u64ReplyId;
      af_ses_save_inflight_inc();
      if (DM_SUCCESS == af_store_save_async(af__https_save_reply_cb, pstCtx)) { return; }
      af_ses_save_inflight_dec();
      dm_free_tag(pstCtx);
    }
  }
  af_https_send_ok(pstMsg->u64ReplyId, "{\"ok\":true}");
}

static void af__on_restart(in const dm_https_msg_st *pstMsg)
{
  af_device_st stDev;
  yyjson_doc *pstDoc = NULL;
  bool_t bReopen = DM_FALSE;

  if (g_stAfSes.bDraining)
  {
    af_https_send_err(pstMsg->u64ReplyId, 503u, "draining", "服务正在关闭");
    return;
  }
  if (!af_auth_check_https(pstMsg, &stDev)) { return; }
  if (DM_SUCCESS != af__read_json_obj(pstMsg, DM_TRUE, &pstDoc)) { return; }
  if (NULL != pstDoc)
  {
    yyjson_val *pstR = yyjson_obj_get(yyjson_doc_get_root(pstDoc), "reopen");
    if ((NULL != pstR) && (!yyjson_is_null(pstR)))
    {
      if (!yyjson_is_bool(pstR))
      {
        yyjson_doc_free(pstDoc);
        af_https_send_err(pstMsg->u64ReplyId, 400u, "bad_request", "reopen 参数无效");
        return;
      }
      bReopen = yyjson_get_bool(pstR) ? DM_TRUE : DM_FALSE;
    }
    yyjson_doc_free(pstDoc);
  }
  if (DM_SUCCESS != af_ses_start_restart_job(bReopen))
  {
    af_https_send_err(pstMsg->u64ReplyId, 500u, "internal", "重启任务调度失败");
    return;
  }
  af_https_send_ok(pstMsg->u64ReplyId, "{\"ok\":true}");
}

int af_ses_http_register(void)
{
  if ((DM_SUCCESS != dm_https_route_exact("GET", "/status", af__on_status))
      || (DM_SUCCESS != dm_https_route_exact("POST", "/connect", af__on_connect))
      || (DM_SUCCESS != dm_https_route_exact("POST", "/disconnect", af__on_disconnect))
      || (DM_SUCCESS != dm_https_route_exact("POST", "/front", af__on_front))
      || (DM_SUCCESS != dm_https_route_exact("POST", "/settings", af__on_settings))
      || (DM_SUCCESS != dm_https_route_exact("POST", "/restart", af__on_restart)))
  {
    return DM_FAILD;
  }
  return DM_SUCCESS;
}
