/**
 ******************************************************************************
 * @file    af_session_mod.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   af_session DM 模块（ModID 22）：CLI RPC、菜单 9 动作、事件联动、重启与 pfDrain（DD-05 §1）。
 ******************************************************************************
 */

#define _DARWIN_C_SOURCE
#define DM_LOG_MOD "session"

#include "af_session_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct af__cli_disc_ctx
{
  dm_reply_st stReply;
} af__cli_disc_ctx_st;

typedef struct af__menu_push
{
  af_menu_update_cb pfCb;
  af_menu_snap_st stSnap;
} af__menu_push_st;

typedef struct af__notify_job
{
  void (*pfNotify)(in const char *, in const char *, in const char *);
  char acTitle[64];
  char acBody[128];
  char acTag[32];
} af__notify_job_st;

typedef struct af__restart_job
{
  bool_t bReopen;
  int (*pfRunCmd)(in const char *const *, in const char *, in const _Atomic bool_t *, out char *,
                  in size_t, in uint32, out int *);
  bool_t bOk;
  char acErr[256];
} af__restart_job_st;

af_ses_ctx_st g_stAfSes;
static af_session_ops_st g_stSesOps;
static bool_t g_bSesOpsInited = DM_FALSE;
static af_menu_update_cb g_pfMenuCb = NULL;

static const dm_err_entry_st g_astAfSesErrMap[] = {
    {AF_ERR_CHOOSE_IPAD, "choose_ipad"},
    {AF_ERR_NO_IPAD, "no_ipad"},
    {0, NULL}
};

static uint64 af__ses_default_now_ms(void)
{
  return dm_hrtime() / 1000000ULL;
}

static void af__ses_init_default_ops(out af_session_ops_st *pstOps)
{
  memset(pstOps, 0, sizeof(*pstOps));
  pstOps->pfRunOnMain = af_plat_run_on_main;
  pstOps->pfBonjourStart = af_plat_bonjour_start;
  pstOps->pfBonjourStop = af_plat_bonjour_stop;
  pstOps->pfHostName = af_plat_host_name;
  pstOps->pfCurrentUser = af_plat_current_user;
  pstOps->pfNotify = af_plat_notify;
  pstOps->pfRunCmd = af_plat_run_cmd_cancellable;
  pstOps->pfNowMs = af__ses_default_now_ms;
  pstOps->u32FrontTickMs = 1000u;
}

const af_session_ops_st *af_ses_ops(void)
{
  if (!g_bSesOpsInited)
  {
    af__ses_init_default_ops(&g_stSesOps);
    g_bSesOpsInited = DM_TRUE;
  }
  return &g_stSesOps;
}

uint64 af_ses_now_ms(void)
{
  return af_ses_ops()->pfNowMs();
}

void af_session_set_menu_cb(in af_menu_update_cb pfCb)
{
  g_pfMenuCb = pfCb;
}

void af_session_set_ops(in const af_session_ops_st *pstOps)
{
  af__ses_init_default_ops(&g_stSesOps);
  g_bSesOpsInited = DM_TRUE;
  if (NULL == pstOps) { return; }
  if (NULL != pstOps->pfRunOnMain) { g_stSesOps.pfRunOnMain = pstOps->pfRunOnMain; }
  if (NULL != pstOps->pfBonjourStart) { g_stSesOps.pfBonjourStart = pstOps->pfBonjourStart; }
  if (NULL != pstOps->pfBonjourStop) { g_stSesOps.pfBonjourStop = pstOps->pfBonjourStop; }
  if (NULL != pstOps->pfHostName) { g_stSesOps.pfHostName = pstOps->pfHostName; }
  if (NULL != pstOps->pfCurrentUser) { g_stSesOps.pfCurrentUser = pstOps->pfCurrentUser; }
  if (NULL != pstOps->pfNotify) { g_stSesOps.pfNotify = pstOps->pfNotify; }
  if (NULL != pstOps->pfRunCmd) { g_stSesOps.pfRunCmd = pstOps->pfRunCmd; }
  if (NULL != pstOps->pfNowMs) { g_stSesOps.pfNowMs = pstOps->pfNowMs; }
  if (pstOps->u32FrontTickMs > 0u) { g_stSesOps.u32FrontTickMs = pstOps->u32FrontTickMs; }
}

void af_ses_get_mac_name(out char *pcOut, in size_t uCap)
{
  const af_state_st *pstSt = af_store_state();
  if ((NULL == pcOut) || (0u == uCap)) { return; }
  if ((NULL != pstSt) && ('\0' != pstSt->acName[0]))
  {
    snprintf(pcOut, uCap, "%s", pstSt->acName);
  }
  else
  {
    af_ses_ops()->pfHostName(pcOut, uCap);
  }
}

void af_ses_save_inflight_inc(void)
{
  g_stAfSes.u32InflightSave++;
}

void af_ses_save_inflight_dec(void)
{
  if (g_stAfSes.u32InflightSave > 0u) { g_stAfSes.u32InflightSave--; }
  af_ses_drain_check();
}

void af_ses_drain_check(void)
{
  if ((!g_stAfSes.bDraining) || g_stAfSes.bRestartRunning || (g_stAfSes.u32InflightSave > 0u))
  {
    return;
  }
  if (g_stAfSes.iDrainHold > 0)
  {
    int iHold = g_stAfSes.iDrainHold;
    g_stAfSes.iDrainHold = 0;
    dm_drain_release(iHold);
  }
}

/* ============================ 菜单快照推送与通知 ============================ */

static void af__ses_menu_trampoline(io void *pvCtx)
{
  af__menu_push_st *pstP = (af__menu_push_st *)pvCtx;
  if (NULL == pstP) { return; }
  if (NULL != pstP->pfCb) { pstP->pfCb(&pstP->stSnap); }
  dm_free_tag(pstP);
}

static void af__ses_notify_trampoline(io void *pvCtx)
{
  af__notify_job_st *pstJ = (af__notify_job_st *)pvCtx;
  if (NULL == pstJ) { return; }
  if (NULL != pstJ->pfNotify) { pstJ->pfNotify(pstJ->acTitle, pstJ->acBody, pstJ->acTag); }
  dm_free_tag(pstJ);
}

static void af__ses_post_notify(in const char *pcTitle, in const char *pcBody, in const char *pcTag)
{
  af__notify_job_st *pstJ = NULL;
  if (g_stAfSes.bDraining) { return; }
  pstJ = (af__notify_job_st *)dm_calloc_tag(1u, sizeof(*pstJ), AF_SES_TAG_MENU);
  if (NULL == pstJ) { return; }
  pstJ->pfNotify = af_ses_ops()->pfNotify;
  snprintf(pstJ->acTitle, sizeof(pstJ->acTitle), "%s", (NULL != pcTitle) ? pcTitle : "Afar");
  snprintf(pstJ->acBody, sizeof(pstJ->acBody), "%s", (NULL != pcBody) ? pcBody : "");
  snprintf(pstJ->acTag, sizeof(pstJ->acTag), "%s", (NULL != pcTag) ? pcTag : "afar");
  af_ses_ops()->pfRunOnMain(af__ses_notify_trampoline, pstJ);
}

void af_ses_push_menu(void)
{
  af_sc_snap_st stSc;
  af__menu_push_st *pstP = NULL;
  af_menu_snap_st *pstS = NULL;
  bool_t bShowing = DM_FALSE;

  if ((NULL == g_pfMenuCb) || g_stAfSes.bDraining) { return; }
  pstP = (af__menu_push_st *)dm_calloc_tag(1u, sizeof(*pstP), AF_SES_TAG_MENU);
  if (NULL == pstP) { return; }
  af_sidecar_get_snap(&stSc);
  bShowing = ((AF_SC_SHOWING == stSc.eState) && (!g_stAfSes.stFront.bActive)) ? DM_TRUE : DM_FALSE;
  pstP->pfCb = g_pfMenuCb;
  pstS = &pstP->stSnap;
  pstS->bShowing = bShowing;
  pstS->bBootAuto = (af_store_cfg()->bAutoConnect && (!af_store_state()->bNoBootConnect)) ? DM_TRUE
                                                                                          : DM_FALSE;
  pstS->bAntiUnplug = af_store_state()->bAntiUnplug;
  pstS->u32TodoCount = g_stAfSes.u32TodoCount;
  snprintf(pstS->acVersion, sizeof(pstS->acVersion), "%s", AF_VERSION);
  if (bShowing)
  {
    pstS->u8IconState = 1u;
    snprintf(pstS->acStatusLine, sizeof(pstS->acStatusLine), "正在显示到 %s",
             ('\0' != stSc.acShowing[0]) ? stSc.acShowing : "iPad");
  }
  else if (g_stAfSes.stFront.bActive || (AF_SC_CONNECTING == stSc.eState))
  {
    pstS->u8IconState = 0u;
    snprintf(pstS->acStatusLine, sizeof(pstS->acStatusLine), "正在连接…");
  }
  else if ((g_stAfSes.u32TodoCount > 0u) || ('\0' != g_stAfSes.acRestartError[0]))
  {
    pstS->u8IconState = 2u;
    if (g_stAfSes.u32TodoCount > 0u)
    {
      snprintf(pstS->acStatusLine, sizeof(pstS->acStatusLine), "还有 %u 项待配置",
               (unsigned)g_stAfSes.u32TodoCount);
    }
    else
    {
      snprintf(pstS->acStatusLine, sizeof(pstS->acStatusLine), "需要处理");
    }
  }
  else
  {
    pstS->u8IconState = 0u;
    if (stSc.u32NearbyCnt > 0u)
    {
      snprintf(pstS->acStatusLine, sizeof(pstS->acStatusLine), "已就绪，附近有 %u 台 iPad",
               (unsigned)stSc.u32NearbyCnt);
    }
    else
    {
      snprintf(pstS->acStatusLine, sizeof(pstS->acStatusLine), "已就绪，未连接");
    }
  }
  af_ses_ops()->pfRunOnMain(af__ses_menu_trampoline, pstP);
}

/* ============================ Front 定时器与断开核心 ============================ */

void af_ses_front_stop(void)
{
  g_stAfSes.stFront.bActive = DM_FALSE;
  if (0u != g_stAfSes.u32FrontTimer)
  {
    (void)dm_timer_cancel(g_stAfSes.u32FrontTimer);
    g_stAfSes.u32FrontTimer = 0u;
  }
}

void af_ses_front_begin(in const char *pcName)
{
  uint64 u64Now = af_ses_now_ms();
  uint32 u32TickMs = (af_ses_ops()->u32FrontTickMs > 0u) ? af_ses_ops()->u32FrontTickMs : 1000u;

  af_ses_front_stop();
  af_front_start(&g_stAfSes.stFront, pcName, u64Now);
  af_kbd_hold(u64Now + (uint64)AF_FRONT_TIMEOUT_MS);
  af_display_hold(u64Now + (uint64)AF_FRONT_TIMEOUT_MS);
  (void)af_sidecar_disconnect(pcName);
  (void)dm_timer_add(u32TickMs, u32TickMs, AF_SES_LINK_FRONT_TMR, 0u, &g_stAfSes.u32FrontTimer);
  af_ses_push_menu();
}

static bool_t af__name_in_nearby(in const af_sc_snap_st *pstSc, in const char *pcName)
{
  uint32 i = 0u;
  if ((NULL == pcName) || ('\0' == pcName[0])) { return DM_FALSE; }
  for (i = 0u; (i < pstSc->u32NearbyCnt) && (i < AF_SC_DEV_MAX); i++)
  {
    if (0 == strcmp(pstSc->aacNearby[i], pcName)) { return DM_TRUE; }
  }
  return DM_FALSE;
}

static int af__ses_on_front_timer(io void *pvArg)
{
  af_sc_snap_st stSc;
  bool_t bConn = DM_FALSE;
  bool_t bAvail = DM_FALSE;
  af_front_act_et eAct = AF_FRONT_WAIT;

  (void)pvArg;
  if (g_stAfSes.bDraining || (!g_stAfSes.stFront.bActive))
  {
    af_ses_front_stop();
    return DM_SUCCESS;
  }
  af_sidecar_get_snap(&stSc);
  bConn = ((AF_SC_SHOWING == stSc.eState)
           && (0 == strcmp(stSc.acShowing, g_stAfSes.stFront.acName)))
              ? DM_TRUE
              : DM_FALSE;
  bAvail = af__name_in_nearby(&stSc, g_stAfSes.stFront.acName);
  eAct = af_front_step(&g_stAfSes.stFront, af_ses_now_ms(), bConn, bAvail);
  if (AF_FRONT_CONNECT == eAct)
  {
    (void)af_sidecar_connect(g_stAfSes.stFront.acName, DM_TRUE);
  }
  else if (AF_FRONT_DONE == eAct)
  {
    af_ses_front_stop();
    af_ses_push_menu();
  }
  else if (AF_FRONT_GIVEUP == eAct)
  {
    af_ses_front_stop();
    af_kbd_hold(0u);
    af_display_hold(0u);
    af_ses_push_menu();
  }
  return DM_SUCCESS;
}

void af_ses_disconnect_core(in af_store_saved_cb pfOnSaved, io void *pvCtx)
{
  af_sc_snap_st stSc;
  bool_t bShowingNow = DM_FALSE;
  bool_t bWasFront = g_stAfSes.stFront.bActive;

  af_sidecar_get_snap(&stSc);
  bShowingNow = (AF_SC_SHOWING == stSc.eState) ? DM_TRUE : DM_FALSE;
  af_ses_front_stop();
  af_display_hold(0u);
  af_store_state_mut()->bUserOff = DM_TRUE;
  if (bWasFront && (!bShowingNow))
  {
    af_kbd_hold(0u);
    af_kbd_notify_connect(NULL, DM_FALSE);
  }
  af_kbd_notify_disconnect(bShowingNow);
  (void)af_sidecar_disconnect(NULL);
  af_ses_push_menu();
  if ((DM_SUCCESS != af_store_save_async(pfOnSaved, pvCtx)) && (NULL != pfOnSaved))
  {
    pfOnSaved(DM_FAILD, pvCtx);
  }
}

/* ============================ 原生重启 HEAVY 作业 ============================ */

static void af__trim_err(in const char *pcSrc, out char *pcDst, in size_t uCap)
{
  const char *pcS = pcSrc;
  const char *pcE = NULL;
  size_t uLen = 0u;
  if ((NULL == pcDst) || (0u == uCap)) { return; }
  pcDst[0] = '\0';
  if (NULL == pcS) { return; }
  while (('\0' != *pcS) && isspace((unsigned char)*pcS)) { pcS++; }
  pcE = pcS + strlen(pcS);
  while ((pcE > pcS) && isspace((unsigned char)pcE[-1])) { pcE--; }
  uLen = (size_t)(pcE - pcS);
  if (uLen >= uCap) { uLen = uCap - 1u; }
  if (uLen > 0u) { memcpy(pcDst, pcS, uLen); }
  pcDst[uLen] = '\0';
}

static void af__restart_work(io void *pvArg)
{
  af__restart_job_st *pstJ = (af__restart_job_st *)pvArg;
  const char *apcDef[] = {"/usr/bin/defaults", "write", "com.apple.loginwindow",
                          "TALLogoutSavesState", "-bool", pstJ->bReopen ? "true" : "false", NULL};
  const char *apcSysEv[] = {
      "/usr/bin/osascript", "-e",
      "tell application \"System Events\" to restart with state saving preference", NULL};
  const char *apcLw[] = {"/usr/bin/osascript", "-e",
                         "tell application \"loginwindow\" to \xC2\xAB""event aevtrest\xC2\xBB",
                         NULL};
  char acOut1[256] = {0};
  char acOut2[256] = {0};
  int iExit = -1;
  int iRc = DM_SUCCESS;

  iRc = pstJ->pfRunCmd(apcDef, NULL, &g_bAfQuitting, acOut1, sizeof(acOut1), AF_SES_RESTART_MS,
                       &iExit);
  if ((DM_SHUTTING_DOWN == iRc) || atomic_load(&g_bAfQuitting)) { return; }

  acOut1[0] = '\0';
  iExit = -1;
  iRc = pstJ->pfRunCmd(apcSysEv, NULL, &g_bAfQuitting, acOut1, sizeof(acOut1), AF_SES_RESTART_MS,
                       &iExit);
  if ((DM_SHUTTING_DOWN == iRc) || atomic_load(&g_bAfQuitting)) { return; }
  if ((DM_SUCCESS == iRc) && (0 == iExit))
  {
    pstJ->bOk = DM_TRUE;
    return;
  }

  iExit = -1;
  iRc = pstJ->pfRunCmd(apcLw, NULL, &g_bAfQuitting, acOut2, sizeof(acOut2), AF_SES_RESTART_MS,
                       &iExit);
  if ((DM_SHUTTING_DOWN == iRc) || atomic_load(&g_bAfQuitting)) { return; }
  if ((DM_SUCCESS == iRc) && (0 == iExit))
  {
    pstJ->bOk = DM_TRUE;
    return;
  }
  af__trim_err(('\0' != acOut1[0]) ? acOut1 : acOut2, pstJ->acErr, sizeof(pstJ->acErr));
  if ('\0' == pstJ->acErr[0]) { snprintf(pstJ->acErr, sizeof(pstJ->acErr), "重启 Mac 失败"); }
}

static void af__restart_after(io void *pvArg, in int iStatus)
{
  af__restart_job_st *pstJ = (af__restart_job_st *)pvArg;

  g_stAfSes.bRestartRunning = DM_FALSE;
  if ((!g_stAfSes.bDraining) && (DM_SUCCESS == iStatus) && (!pstJ->bOk))
  {
    snprintf(g_stAfSes.acRestartError, sizeof(g_stAfSes.acRestartError), "%s", pstJ->acErr);
    af_ses_push_menu();
  }
  dm_free_tag(pstJ);
  af_ses_drain_check();
}

int af_ses_start_restart_job(in bool_t bReopen)
{
  af__restart_job_st *pstJ = NULL;
  int iRc = DM_SUCCESS;

  if (g_stAfSes.bDraining) { return DM_SHUTTING_DOWN; }
  if (g_stAfSes.bRestartRunning) { return DM_SUCCESS; }
  pstJ = (af__restart_job_st *)dm_calloc_tag(1u, sizeof(*pstJ), AF_SES_TAG_JOB);
  if (NULL == pstJ) { return DM_ALLOC; }
  pstJ->bReopen = bReopen;
  pstJ->pfRunCmd = af_ses_ops()->pfRunCmd;
  g_stAfSes.bRestartRunning = DM_TRUE;
  g_stAfSes.acRestartError[0] = '\0';
  iRc = dm_pool_submit(DM_LANE_HEAVY, af__restart_work, af__restart_after, pstJ);
  if (DM_SUCCESS != iRc)
  {
    g_stAfSes.bRestartRunning = DM_FALSE;
    dm_free_tag(pstJ);
  }
  return iRc;
}

/* ============================ 开机自连、CLI RPC 与菜单 9 动作 ============================ */

static void af__ses_try_auto_connect(in const char *pcAppeared)
{
  const af_config_st *pstCfg = af_store_cfg();
  const af_state_st *pstSt = af_store_state();
  af_sc_snap_st stSc;
  const char *pcTarget = NULL;

  if ((!pstCfg->bAutoConnect) || pstSt->bNoBootConnect || pstSt->bUserOff
      || g_stAfSes.stFront.bActive || af_sidecar_in_cooldown() || g_stAfSes.bDraining)
  {
    return;
  }
  af_sidecar_get_snap(&stSc);
  if (AF_SC_IDLE != stSc.eState) { return; }
  if ((NULL != pcAppeared) && ('\0' != pcAppeared[0]))
  {
    if ('\0' != pstCfg->acIpadName[0])
    {
      if (0 == strcmp(pstCfg->acIpadName, pcAppeared)) { pcTarget = pcAppeared; }
    }
    else if ('\0' != pstSt->acLastIpad[0])
    {
      if (0 == strcmp(pstSt->acLastIpad, pcAppeared)) { pcTarget = pcAppeared; }
    }
    else { pcTarget = pcAppeared; }
  }
  else
  {
    if (af__name_in_nearby(&stSc, pstCfg->acIpadName)) { pcTarget = pstCfg->acIpadName; }
    else if (af__name_in_nearby(&stSc, pstSt->acLastIpad)) { pcTarget = pstSt->acLastIpad; }
    else if (1u == stSc.u32NearbyCnt) { pcTarget = stSc.aacNearby[0]; }
  }
  if (NULL != pcTarget) { (void)af_sidecar_connect(pcTarget, DM_FALSE); }
}

static void af__cli_disc_on_saved(in int iStatus, io void *pvCtx)
{
  af__cli_disc_ctx_st *pstCtx = (af__cli_disc_ctx_st *)pvCtx;
  static const char acOk[] = "{\"ok\":true}";

  af_ses_save_inflight_dec();
  if (DM_SUCCESS == iStatus)
  {
    (void)dm_reply_send(&pstCtx->stReply, DM_SUCCESS, acOk, (uint32)(sizeof(acOk) - 1u));
  }
  else
  {
    (void)dm_reply_send(&pstCtx->stReply, DM_IO, "{\"ok\":false}", 12u);
  }
  dm_free_tag(pstCtx);
}

static int af__on_cli_disconnect(io void *pvArg)
{
  dm_link_msg_st *pstMsg = (dm_link_msg_st *)pvArg;
  af__cli_disc_ctx_st *pstCtx = NULL;
  dm_reply_st stReply;

  if (DM_SUCCESS != dm_reply_take(pstMsg, &stReply))
  {
    af_ses_disconnect_core(NULL, NULL);
    return DM_SUCCESS;
  }
  pstCtx = (af__cli_disc_ctx_st *)dm_calloc_tag(1u, sizeof(*pstCtx), AF_SES_TAG_SAVE);
  if (NULL == pstCtx)
  {
    af_ses_disconnect_core(NULL, NULL);
    (void)dm_reply_send(&stReply, DM_SUCCESS, "{\"ok\":true}", 11u);
    return DM_SUCCESS;
  }
  pstCtx->stReply = stReply;
  af_ses_save_inflight_inc();
  af_ses_disconnect_core(af__cli_disc_on_saved, pstCtx);
  return DM_SUCCESS;
}

static void af__menu_connect(void)
{
  af_sc_snap_st stSc;
  const af_config_st *pstCfg = af_store_cfg();
  const af_state_st *pstSt = af_store_state();
  const char *pcTarget = NULL;

  af_sidecar_get_snap(&stSc);
  if ('\0' != pstCfg->acIpadName[0]) { pcTarget = pstCfg->acIpadName; }
  else if ('\0' != pstSt->acLastIpad[0]) { pcTarget = pstSt->acLastIpad; }
  else if (stSc.u32NearbyCnt > 0u) { pcTarget = stSc.aacNearby[0]; }
  if (NULL == pcTarget) { return; }
  af_ses_front_stop();
  af_store_state_mut()->bUserOff = DM_FALSE;
  snprintf(af_store_state_mut()->acLastIpad, sizeof(af_store_state_mut()->acLastIpad), "%s",
           pcTarget);
  (void)af_store_save_async(NULL, NULL);
  af_kbd_notify_connect(pcTarget, pstSt->bKeyboardAuto);
  (void)af_sidecar_connect(pcTarget, DM_TRUE);
}

static int af__on_ui_action(io void *pvArg)
{
  const dm_link_msg_st *pstMsg = (const dm_link_msg_st *)pvArg;
  af_ui_action_msg_st stAct;

  if ((NULL == pstMsg) || (NULL == pstMsg->pu8Data) || (pstMsg->u32Len < sizeof(stAct)))
  {
    return DM_SUCCESS;
  }
  memcpy(&stAct, pstMsg->pu8Data, sizeof(stAct));
  switch (stAct.eAct)
  {
    case AF_MENU_CONNECT: af__menu_connect(); break;
    case AF_MENU_DISCONNECT: af_ses_disconnect_core(NULL, NULL); return DM_SUCCESS;
    case AF_MENU_TOGGLE_BOOT_AUTO:
      if (stAct.bVal)
      {
        if (!af_store_cfg()->bAutoConnect)
        {
          af_store_cfg_mut()->bAutoConnect = DM_TRUE;
          (void)af_store_save_cfg_async(NULL, NULL);
        }
        af_store_state_mut()->bNoBootConnect = DM_FALSE;
      }
      else { af_store_state_mut()->bNoBootConnect = DM_TRUE; }
      (void)af_store_save_async(NULL, NULL);
      break;
    case AF_MENU_TOGGLE_ANTI_UNPLUG: af_display_set_anti_unplug(stAct.bVal); break;
    case AF_MENU_PAIR_NEW: (void)dm_queue_post_async(AF_LINK_UI_SETUP_GO, "pair", 5u); break;
    case AF_MENU_OPEN_SETUP: (void)dm_queue_post_async(AF_LINK_UI_SETUP_GO, "status", 7u); break;
    case AF_MENU_RESET_SETUP:
      af_store_state_mut()->bSetupDone = DM_FALSE;
      (void)af_store_save_async(NULL, NULL);
      (void)dm_queue_post_async(AF_LINK_UI_SETUP_GO, "w1", 3u);
      break;
    case AF_MENU_REOPEN: (void)dm_queue_post_async(AF_LINK_UI_SETUP_GO, "", 1u); break;
    case AF_MENU_NOTIFY_DEVICES: (void)dm_queue_post_async(AF_LINK_UI_SETUP_GO, "devices", 8u); break;
    default: break;
  }
  af_ses_push_menu();
  return DM_SUCCESS;
}

static int af__on_quit(io void *pvArg)
{
  (void)pvArg;
  dm_quit(0);
  return DM_SUCCESS;
}

/* ============================ 事件订阅与模块生命周期 ============================ */

static int af__on_ready(io void *pvArg)
{
  char acName[65];
  uint16 u16Port = 0u;

  (void)pvArg;
  if (atomic_load(&g_bAfQuitRequested))
  {
    dm_quit(0);
    return DM_SUCCESS;
  }
  g_stAfSes.bReady = DM_TRUE;
  af_ses_get_mac_name(acName, sizeof(acName));
  u16Port = dm_https_get_port();
  if (0u == u16Port) { u16Port = af_store_cfg()->u16Port; }
  (void)af_ses_ops()->pfBonjourStart(acName, u16Port);
  af__ses_try_auto_connect(NULL);
  af_ses_push_menu();
  return DM_SUCCESS;
}

static int af__on_auth_ev(io void *pvArg)
{
  const dm_link_msg_st *pstMsg = (const dm_link_msg_st *)pvArg;
  af_ev_auth_st stEv;

  if ((NULL == pstMsg) || (NULL == pstMsg->pu8Data) || (pstMsg->u32Len < sizeof(stEv)))
  {
    return DM_SUCCESS;
  }
  memcpy(&stEv, pstMsg->pu8Data, sizeof(stEv));
  if (AF_AUTH_EV_REVOKED == stEv.eKind)
  {
    if ('\0' != stEv.acSidecarName[0])
    {
      af_sc_snap_st stSc;
      if (g_stAfSes.stFront.bActive
          && (0 == strcmp(g_stAfSes.stFront.acName, stEv.acSidecarName)))
      {
        af_ses_front_stop();
        af_kbd_hold(0u);
        af_display_hold(0u);
      }
      af_sidecar_get_snap(&stSc);
      if (((AF_SC_SHOWING == stSc.eState) && (0 == strcmp(stSc.acShowing, stEv.acSidecarName)))
          || ((AF_SC_CONNECTING == stSc.eState)
              && (0 == strcmp(af_store_state()->acLastIpad, stEv.acSidecarName))))
      {
        af_kbd_notify_disconnect((AF_SC_SHOWING == stSc.eState) ? DM_TRUE : DM_FALSE);
        (void)af_sidecar_disconnect(stEv.acSidecarName);
      }
      if (0 == strcmp(af_store_state()->acLastIpad, stEv.acSidecarName))
      {
        af_store_state_mut()->acLastIpad[0] = '\0';
        (void)af_store_save_async(NULL, NULL);
      }
    }
  }
  else if (AF_AUTH_EV_PAIRED == stEv.eKind)
  {
    af__ses_post_notify("新 iPad 已配对", stEv.acDevName, "paired");
  }
  af_ses_push_menu();
  return DM_SUCCESS;
}

static int af__on_setup_todo(io void *pvArg)
{
  const dm_link_msg_st *pstMsg = (const dm_link_msg_st *)pvArg;
  af_ev_setup_todo_st stEv;

  if ((NULL == pstMsg) || (NULL == pstMsg->pu8Data) || (pstMsg->u32Len < sizeof(stEv)))
  {
    return DM_SUCCESS;
  }
  memcpy(&stEv, pstMsg->pu8Data, sizeof(stEv));
  g_stAfSes.u32TodoCount = stEv.u32TodoCount;
  af_ses_push_menu();
  return DM_SUCCESS;
}

static int af__on_sidecar_ev(io void *pvArg)
{
  const dm_link_msg_st *pstMsg = (const dm_link_msg_st *)pvArg;
  af_ev_sidecar_st stEv;

  if ((NULL != pstMsg) && (NULL != pstMsg->pu8Data) && (pstMsg->u32Len >= sizeof(stEv)))
  {
    memcpy(&stEv, pstMsg->pu8Data, sizeof(stEv));
    if (stEv.bNewAppear && ('\0' != stEv.acAppeared[0]))
    {
      af__ses_try_auto_connect(stEv.acAppeared);
    }
  }
  af_ses_push_menu();
  return DM_SUCCESS;
}

static int af__on_simple_menu_ev(io void *pvArg)
{
  (void)pvArg;
  af_ses_push_menu();
  return DM_SUCCESS;
}

static int af__ses_init(void)
{
  static const dm_iface_st stCliDisc = {
      .uiLinkID = AF_LINK_CLI_DISCONNECT,
      .pcName = "session.disconnect",
      .u32Roles = AF_ROLE_CLI,
      .u32Flags = DM_IFACE_MUTATE,
      .pcDesc = "Disconnect Sidecar and persist user_off=true",
      .pcArgs = NULL
  };

  memset(&g_stAfSes, 0, sizeof(g_stAfSes));
  dm_mem_tag_name(AF_SES_TAG_JOB, "af_ses_job");
  dm_mem_tag_name(AF_SES_TAG_MENU, "af_ses_menu");
  dm_mem_tag_name(AF_SES_TAG_SAVE, "af_ses_save");

  if ((DM_SUCCESS != af_ses_http_register())
      || (DM_SUCCESS != dm_link_register_handler(AF_LINK_UI_ACTION, af__on_ui_action, NULL, NULL))
      || (DM_SUCCESS != dm_link_register_handler(AF_LINK_QUIT, af__on_quit, NULL, NULL))
      || (DM_SUCCESS != dm_link_register_handler(AF_LINK_CLI_DISCONNECT, af__on_cli_disconnect,
                                                 NULL, NULL))
      || (DM_SUCCESS != dm_iface_register(&stCliDisc))
      || (DM_SUCCESS != dm_link_register_handler(AF_SES_LINK_FRONT_TMR, af__ses_on_front_timer,
                                                 NULL, NULL))
      || (DM_SUCCESS != dm_event_subscribe(DM_EV_READY, af__on_ready, NULL, NULL))
      || (DM_SUCCESS != dm_event_subscribe(AF_EV_AUTH_CHANGED, af__on_auth_ev, NULL, NULL))
      || (DM_SUCCESS != dm_event_subscribe(AF_EV_SETUP_TODO, af__on_setup_todo, NULL, NULL))
      || (DM_SUCCESS != dm_event_subscribe(AF_EV_SIDECAR_CHANGED, af__on_sidecar_ev, NULL, NULL))
      || (DM_SUCCESS != dm_event_subscribe(AF_EV_DISPLAY_CHANGED, af__on_simple_menu_ev, NULL,
                                           NULL))
      || (DM_SUCCESS != dm_event_subscribe(AF_EV_KBD_CHANGED, af__on_simple_menu_ev, NULL, NULL))
      || (DM_SUCCESS != dm_event_subscribe(AF_EV_ENV_UPDATED, af__on_simple_menu_ev, NULL, NULL)))
  {
    return DM_FAILD;
  }
  return DM_SUCCESS;
}

static void af__ses_drain(io dm_drain_st *pstDrain)
{
  g_stAfSes.bDraining = DM_TRUE;
  af_ses_front_stop();
  af_ses_ops()->pfBonjourStop();
  if (g_stAfSes.bRestartRunning)
  {
    atomic_store(&g_bAfQuitting, DM_TRUE);
  }
  if (g_stAfSes.bRestartRunning || (g_stAfSes.u32InflightSave > 0u))
  {
    int iHold = dm_drain_hold(pstDrain);
    if (iHold > 0) { g_stAfSes.iDrainHold = iHold; }
  }
}

static void af__ses_fini(void)
{
  af_ses_front_stop();
  (void)dm_event_unsubscribe(AF_EV_ENV_UPDATED, af__on_simple_menu_ev);
  (void)dm_event_unsubscribe(AF_EV_KBD_CHANGED, af__on_simple_menu_ev);
  (void)dm_event_unsubscribe(AF_EV_DISPLAY_CHANGED, af__on_simple_menu_ev);
  (void)dm_event_unsubscribe(AF_EV_SIDECAR_CHANGED, af__on_sidecar_ev);
  (void)dm_event_unsubscribe(AF_EV_SETUP_TODO, af__on_setup_todo);
  (void)dm_event_unsubscribe(AF_EV_AUTH_CHANGED, af__on_auth_ev);
  (void)dm_event_unsubscribe(DM_EV_READY, af__on_ready);
  memset(&g_stAfSes, 0, sizeof(g_stAfSes));
}

const dm_module_st g_stAfSessionMod = {
    .u16ModID = AF_MOD_SESSION,
    .pcName = "af_session",
    .pfInit = af__ses_init,
    .pfDrain = af__ses_drain,
    .pfFini = af__ses_fini,
    .pastErrMap = g_astAfSesErrMap
};

const dm_module_st *af_session_module(void)
{
  return &g_stAfSessionMod;
}
