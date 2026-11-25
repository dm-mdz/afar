/**
 ******************************************************************************
 * @file    af_kbd_mod.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   af_kbd DM 模块实现：HTTPS 路由、HEAVY 串行接管/松开、配对回调与 pfDrain（DD-04）。
 ******************************************************************************
 */

#define _DARWIN_C_SOURCE
#define DM_LOG_MOD "kbd"

#include "af_kbd.h"
#include "../af_events.h"
#include "../af_modules.h"
#include "../af_store.h"
#include "../auth/af_auth.h"

#include <dm.h>
#include <dm/event.h>
#include "base/af_https_srv.h"
#include <dm/json.h>
#include <dm/link.h>
#include <dm/log.h>
#include <dm/module.h>
#include <dm/pool.h>
#include <dm/timer.h>
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define AF_KBD_LINK_TAKE_TMR DM_MOD_LINKID(AF_MOD_KBD, 0x02u)
#define AF_KBD_LINK_TICK_TMR DM_MOD_LINKID(AF_MOD_KBD, 0x03u)
#define AF_KBD_TAG_JOB       DM_MOD_MEMTAG(AF_MOD_KBD, 0u)
#define AF_KBD_TAG_MAIN      DM_MOD_MEMTAG(AF_MOD_KBD, 1u)
#define AF_KBD_TICK_MS       1000u
#define AF_KBD_BT_SETTINGS   "x-apple.systempreferences:com.apple.BluetoothSettings"
#define AF_KBD_DENIED_MSG    "请在系统设置 > 隐私与安全 > 蓝牙 中允许 Afar"

#define AF_KBD_OP_OF(ops, field, def) ((NULL != (ops).field) ? (ops).field : (def))
#define AF_KBD_OP(field, def)         AF_KBD_OP_OF(g_stOps, field, def)
#define AF_KBD_NOW_MS()               ((NULL != g_stOps.pfNowMs) ? g_stOps.pfNowMs() : (dm_hrtime() / 1000000ULL))
#define AF_KBD_COPY(dst, src)         snprintf((dst), sizeof(dst), "%s", (src))

typedef enum af_kbd_main_kind
{
  AF_KBD_MAIN_OPEN_URL = 1,
  AF_KBD_MAIN_PAIR_SEARCH,
  AF_KBD_MAIN_PAIR_START,
  AF_KBD_MAIN_PAIR_CANCEL
} af_kbd_main_kind_et;

typedef struct af_kbd_main_req
{
  af_kbd_main_kind_et eKind;
  char acStr[128];
  af_kbd_ops_st stOps;
} af_kbd_main_req_st;

typedef struct af_kbd_query_job
{
  uint64 u64ReplyId;
  bool_t bIsSet;
  char acAddr[32];
  af_kbd_ops_st stOps;
  af_bt_auth_et eAuth;
  uint32 u32Count;
  af_bt_device_st astDevs[AF_BT_DEV_MAX];
  bool_t bFound;
  af_bt_device_st stFound;
} af_kbd_query_job_st;

typedef struct af_kbd_bt_job
{
  bool_t bIsConnect;
  uint32 u32Gen;
  char acAddr[32];
  af_kbd_ops_st stOps;
  int iRet;
} af_kbd_bt_job_st;

typedef struct af_kbd_ctx
{
  af_kbd_flow_st stFlow;
  bool_t bSidecarShowing;
  bool_t bDirectActive;
  char acSidecarShowing[65];
  bool_t bTaking;
  uint32 u32TakeGen;
  char acTakeAddr[32];
  uint64 u64TakeDeadlineMs;
  uint32 u32TakeTimer;
  uint32 u32TickTimer;
  uint32 u32InflightBt;
  uint32 u32InflightQuery;
  bool_t bDrainSaveInflight;
  bool_t bDisconnectPending;
  char acPendingDiscAddr[32];
  bool_t bTakePending;
  bool_t bDraining;
  int iDrainHold;
  bool_t bDrainNeedDisc;
  char acDrainDiscAddr[32];
  bool_t bHasReleasedAt;
  uint64 u64ReleasedAtMs;
  char acError[128];
  af_kbd_pair_snap_st stPair;
} af_kbd_ctx_st;

static af_kbd_ops_st g_stOps;
static af_kbd_ctx_st g_stKbd;

static const dm_err_entry_st g_astKbdErrs[] = {
    {AF_ERR_BT_DENIED, "bluetooth permission denied"},
    {AF_ERR_BT_BUSY, "bluetooth pairing already in progress"},
    {0, NULL}
};

/* ============================ 平台钩子与通用辅助 ============================ */

void af_kbd_set_ops(dm_in const af_kbd_ops_st *pstOps)
{
  if (NULL == pstOps) { memset(&g_stOps, 0, sizeof(g_stOps)); }
  else { g_stOps = *pstOps; }
}

static void af__kbd_stop_timer(dm_io uint32 *pu32Tmr)
{
  if (0u != *pu32Tmr) { (void)dm_timer_cancel(*pu32Tmr); *pu32Tmr = 0u; }
}

static void af__kbd_cancel_take(void)
{
  g_stKbd.u32TakeGen++;
  g_stKbd.bTaking = DM_FALSE;
  g_stKbd.bTakePending = DM_FALSE;
  af__kbd_stop_timer(&g_stKbd.u32TakeTimer);
}

static void af__kbd_emit_changed(void)
{
  if (!g_stKbd.bDraining) { (void)dm_event_publish(AF_EV_KBD_CHANGED, NULL, 0u); }
}

static af_bt_device_st *af__kbd_find_pair_found(dm_in const char *pcAddr)
{
  uint32 i = 0u;
  if ((NULL == pcAddr) || ('\0' == pcAddr[0])) { return NULL; }
  for (i = 0u; i < g_stKbd.stPair.u32FoundCnt; i++)
  {
    if (0 == strcasecmp(g_stKbd.stPair.astFound[i].acAddr, pcAddr)) { return &g_stKbd.stPair.astFound[i]; }
  }
  return NULL;
}

static void af__kbd_main_trampoline(dm_io void *pvCtx)
{
  af_kbd_main_req_st *pstR = (af_kbd_main_req_st *)pvCtx;
  if (NULL == pstR) { return; }
  if (AF_KBD_MAIN_OPEN_URL == pstR->eKind) { (void)AF_KBD_OP_OF(pstR->stOps, pfOpenUrl, af_plat_open_url)(pstR->acStr); }
  else if (AF_KBD_MAIN_PAIR_SEARCH == pstR->eKind) { (void)AF_KBD_OP_OF(pstR->stOps, pfBtPairSearch, af_plat_bt_pair_search)(AF_LINK_KBD_PAIR_CB); }
  else if (AF_KBD_MAIN_PAIR_START == pstR->eKind) { (void)AF_KBD_OP_OF(pstR->stOps, pfBtPairStart, af_plat_bt_pair_start)(pstR->acStr, AF_LINK_KBD_PAIR_CB); }
  else if (AF_KBD_MAIN_PAIR_CANCEL == pstR->eKind) { AF_KBD_OP_OF(pstR->stOps, pfBtPairCancel, af_plat_bt_pair_cancel)(); }
  dm_free_tag(pstR);
}

static void af__kbd_post_main(dm_in af_kbd_main_kind_et eKind, dm_in const char *pcStr)
{
  af_kbd_main_req_st *pstR = (af_kbd_main_req_st *)dm_calloc_tag(1u, sizeof(*pstR), AF_KBD_TAG_MAIN);
  if (NULL == pstR) { return; }
  pstR->eKind = eKind;
  pstR->stOps = g_stOps;
  if (NULL != pcStr) { AF_KBD_COPY(pstR->acStr, pcStr); }
  AF_KBD_OP(pfRunOnMain, af_plat_run_on_main)(af__kbd_main_trampoline, pstR);
}

static void af__trim_copy(dm_in const char *pcSrc, dm_out char *pcDst, dm_in size_t uCap)
{
  size_t uLen = 0u;
  const char *pcEnd = NULL;
  if ((NULL == pcDst) || (0u == uCap)) { return; }
  pcDst[0] = '\0';
  if (NULL == pcSrc) { return; }
  while (('\0' != *pcSrc) && isspace((unsigned char)*pcSrc)) { pcSrc++; }
  pcEnd = pcSrc + strlen(pcSrc);
  while ((pcEnd > pcSrc) && isspace((unsigned char)pcEnd[-1])) { pcEnd--; }
  uLen = (size_t)(pcEnd - pcSrc);
  if (uLen >= uCap) { uLen = uCap - 1u; }
  if (uLen > 0u) { memcpy(pcDst, pcSrc, uLen); }
  pcDst[uLen] = '\0';
}

/* ============================ HEAVY 串行连断与 pfDrain ============================ */

static int af__kbd_submit_bt(dm_in bool_t bIsConnect, dm_in const char *pcAddr, dm_in uint32 u32Gen);

static void af__kbd_drain_step(void)
{
  int iHold = 0;
  if ((!g_stKbd.bDraining) || (g_stKbd.u32InflightBt > 0u) || (g_stKbd.u32InflightQuery > 0u) || g_stKbd.bDrainSaveInflight) { return; }
  if (g_stKbd.bDrainNeedDisc && ('\0' != g_stKbd.acDrainDiscAddr[0]))
  {
    char acAddr[32];
    AF_KBD_COPY(acAddr, g_stKbd.acDrainDiscAddr);
    g_stKbd.bDrainNeedDisc = DM_FALSE;
    g_stKbd.acDrainDiscAddr[0] = '\0';
    if (DM_SUCCESS == af__kbd_submit_bt(DM_FALSE, acAddr, 0u)) { return; }
  }
  iHold = g_stKbd.iDrainHold;
  g_stKbd.iDrainHold = 0;
  if (iHold > 0) { dm_drain_release(iHold); }
}

static void af__kbd_on_drain_saved(dm_in int iErr, dm_io void *pvCtx)
{
  (void)iErr;
  (void)pvCtx;
  g_stKbd.bDrainSaveInflight = DM_FALSE;
  af__kbd_drain_step();
}

static void af__kbd_take_timeout(void)
{
  g_stKbd.bTaking = DM_FALSE;
  g_stKbd.bTakePending = DM_FALSE;
  g_stKbd.stFlow.bTaken = DM_FALSE;
  af_store_state_mut()->bKeyboardTaken = DM_FALSE;
  (void)af_store_save_async(NULL, NULL);
  AF_KBD_COPY(g_stKbd.acError, "60 秒内未能接管键盘，请确认已在 iPad 断开蓝牙");
  af__kbd_emit_changed();
}

static void af__kbd_bt_work(dm_io void *pvArg)
{
  af_kbd_bt_job_st *pstJ = (af_kbd_bt_job_st *)pvArg;
  pstJ->iRet = pstJ->bIsConnect
                   ? AF_KBD_OP_OF(pstJ->stOps, pfBtConnect, af_plat_bt_connect)(pstJ->acAddr)
                   : AF_KBD_OP_OF(pstJ->stOps, pfBtDisconnect, af_plat_bt_disconnect)(pstJ->acAddr);
}

static void af__kbd_on_connect_done(dm_in uint32 u32Gen, dm_in int iStatus, dm_in int iRet)
{
  uint32 u32Step = (g_stOps.u32TakeStepMs > 0u) ? g_stOps.u32TakeStepMs : AF_KBD_TAKE_STEP;
  if ((!g_stKbd.bTaking) || (u32Gen != g_stKbd.u32TakeGen) || atomic_load(&g_bAfQuitting)) { return; }
  if ((DM_SUCCESS == iStatus) && (DM_SUCCESS == iRet))
  {
    g_stKbd.bTaking = DM_FALSE;
    g_stKbd.acError[0] = '\0';
    af__kbd_emit_changed();
    return;
  }
  if (AF_KBD_NOW_MS() >= g_stKbd.u64TakeDeadlineMs) { af__kbd_take_timeout(); return; }
  af__kbd_stop_timer(&g_stKbd.u32TakeTimer);
  if (DM_SUCCESS != dm_timer_add(u32Step, 0u, AF_KBD_LINK_TAKE_TMR, (uint64)u32Gen, &g_stKbd.u32TakeTimer)) { g_stKbd.bTaking = DM_FALSE; }
}

static void af__kbd_bt_after(dm_io void *pvArg, dm_in int iStatus)
{
  af_kbd_bt_job_st *pstJ = (af_kbd_bt_job_st *)pvArg;
  bool_t bIsConnect = pstJ->bIsConnect;
  uint32 u32Gen = pstJ->u32Gen;
  int iRet = pstJ->iRet;
  char acAddr[32];

  AF_KBD_COPY(acAddr, pstJ->acAddr);
  dm_free_tag(pstJ);
  if (g_stKbd.u32InflightBt > 0u) { g_stKbd.u32InflightBt--; }
  if (g_stKbd.bDraining) { af__kbd_drain_step(); return; }
  if (bIsConnect) { af__kbd_on_connect_done(u32Gen, iStatus, iRet); }
  else
  {
    if ((DM_SUCCESS == iStatus) && (DM_SUCCESS == iRet))
    {
      const af_state_st *pstSt = af_store_state();
      if (pstSt->bHasKeyboard && (0 == strcasecmp(acAddr, pstSt->stKeyboard.acAddr)))
      {
        g_stKbd.bHasReleasedAt = DM_TRUE;
        g_stKbd.u64ReleasedAtMs = AF_KBD_NOW_MS();
      }
      g_stKbd.acError[0] = '\0';
    }
    else { DM_LOG_WARN("bt disconnect failed status=%d ret=%d", iStatus, iRet); }
    af__kbd_emit_changed();
  }
  if (0u != g_stKbd.u32InflightBt) { return; }
  if (g_stKbd.bDisconnectPending)
  {
    AF_KBD_COPY(acAddr, g_stKbd.acPendingDiscAddr);
    g_stKbd.bDisconnectPending = DM_FALSE;
    g_stKbd.acPendingDiscAddr[0] = '\0';
    (void)af__kbd_submit_bt(DM_FALSE, acAddr, 0u);
  }
  else if (g_stKbd.bTakePending && g_stKbd.bTaking && (!atomic_load(&g_bAfQuitting)))
  {
    g_stKbd.bTakePending = DM_FALSE;
    (void)af__kbd_submit_bt(DM_TRUE, g_stKbd.acTakeAddr, g_stKbd.u32TakeGen);
  }
}

static int af__kbd_submit_bt(dm_in bool_t bIsConnect, dm_in const char *pcAddr, dm_in uint32 u32Gen)
{
  af_kbd_bt_job_st *pstJ = NULL;
  int iErr = DM_SUCCESS;
  if ((NULL == pcAddr) || ('\0' == pcAddr[0])) { return DM_INVALID; }
  pstJ = (af_kbd_bt_job_st *)dm_calloc_tag(1u, sizeof(*pstJ), AF_KBD_TAG_JOB);
  if (NULL == pstJ) { return DM_ALLOC; }
  pstJ->bIsConnect = bIsConnect;
  pstJ->u32Gen = u32Gen;
  pstJ->stOps = g_stOps;
  AF_KBD_COPY(pstJ->acAddr, pcAddr);
  g_stKbd.u32InflightBt++;
  iErr = dm_pool_submit(DM_LANE_HEAVY, af__kbd_bt_work, af__kbd_bt_after, pstJ);
  if (DM_SUCCESS != iErr) { g_stKbd.u32InflightBt--; dm_free_tag(pstJ); }
  return iErr;
}

static void af__kbd_request_disconnect(dm_in const char *pcAddr)
{
  if ((NULL == pcAddr) || ('\0' == pcAddr[0])) { return; }
  af__kbd_cancel_take();
  if (0u == g_stKbd.u32InflightBt) { (void)af__kbd_submit_bt(DM_FALSE, pcAddr, 0u); return; }
  g_stKbd.bDisconnectPending = DM_TRUE;
  AF_KBD_COPY(g_stKbd.acPendingDiscAddr, pcAddr);
}

static void af__kbd_release_old_taken(dm_in const char *pcOldAddr)
{
  af__kbd_cancel_take();
  if (!g_stKbd.stFlow.bTaken) { return; }
  g_stKbd.stFlow.bTaken = DM_FALSE;
  af_store_state_mut()->bKeyboardTaken = DM_FALSE;
  af__kbd_request_disconnect(pcOldAddr);
}

static void af__kbd_exec_act(dm_in af_kbd_act_et eAct)
{
  const af_state_st *pstSt = af_store_state();
  uint32 u32MaxMs = (g_stOps.u32TakeMaxMs > 0u) ? g_stOps.u32TakeMaxMs : AF_KBD_TAKE_MAX_MS;

  if (AF_KBD_ACT_TAKE == eAct)
  {
    if ((!pstSt->bHasKeyboard) || ('\0' == pstSt->stKeyboard.acAddr[0])) { return; }
    af_store_state_mut()->bKeyboardTaken = g_stKbd.stFlow.bTaken;
    (void)af_store_save_async(NULL, NULL);
    af__kbd_cancel_take();
    g_stKbd.acError[0] = '\0';
    g_stKbd.bHasReleasedAt = DM_FALSE;
    g_stKbd.u64ReleasedAtMs = 0u;
    g_stKbd.bTaking = DM_TRUE;
    AF_KBD_COPY(g_stKbd.acTakeAddr, pstSt->stKeyboard.acAddr);
    g_stKbd.u64TakeDeadlineMs = AF_KBD_NOW_MS() + (uint64)u32MaxMs;
    af__kbd_emit_changed();
    if (0u != g_stKbd.u32InflightBt) { g_stKbd.bTakePending = DM_TRUE; }
    else if (DM_SUCCESS != af__kbd_submit_bt(DM_TRUE, g_stKbd.acTakeAddr, g_stKbd.u32TakeGen)) { g_stKbd.bTaking = DM_FALSE; }
  }
  else if (AF_KBD_ACT_RELEASE == eAct)
  {
    af__kbd_cancel_take();
    if (pstSt->bKeyboardTaken != g_stKbd.stFlow.bTaken)
    {
      af_store_state_mut()->bKeyboardTaken = g_stKbd.stFlow.bTaken;
      (void)af_store_save_async(NULL, NULL);
    }
    if (pstSt->bHasKeyboard) { af__kbd_request_disconnect(pstSt->stKeyboard.acAddr); }
    af__kbd_emit_changed();
  }
  else if ((AF_KBD_ACT_CLEAR_RELEASED == eAct) && g_stKbd.bHasReleasedAt)
  {
    g_stKbd.bHasReleasedAt = DM_FALSE;
    g_stKbd.u64ReleasedAtMs = 0u;
    af__kbd_emit_changed();
  }
}

static void af__kbd_drive_tick(dm_in uint64 u64NowMs)
{
  const af_state_st *pstSt = af_store_state();
  const char *apcConn[1] = {g_stKbd.acSidecarShowing};
  uint32 u32Cnt = (g_stKbd.bSidecarShowing && ('\0' != g_stKbd.acSidecarShowing[0])) ? 1u : 0u;
  bool_t bKbOnMac = DM_FALSE;
  af_kbd_act_et eAct = AF_KBD_ACT_NONE;
  if (g_stKbd.bDirectActive) { return; }
  bKbOnMac = (pstSt->bHasKeyboard && ('\0' != pstSt->stKeyboard.acAddr[0]))
                 ? AF_KBD_OP(pfBtIsConnected, af_plat_bt_is_connected)(pstSt->stKeyboard.acAddr)
                 : DM_FALSE;
  eAct = af_kbd_flow_tick(&g_stKbd.stFlow, apcConn, u32Cnt, bKbOnMac, u64NowMs);
  if (AF_KBD_ACT_NONE != eAct) { af__kbd_exec_act(eAct); }
}

/* ============================ 定时器、事件与配对回调 ============================ */

static int af__kbd_on_take_timer(dm_io void *pvArg)
{
  dm_link_msg_st *pstMsg = (dm_link_msg_st *)pvArg;
  g_stKbd.u32TakeTimer = 0u;
  if ((!g_stKbd.bTaking) || g_stKbd.bDraining || g_stKbd.bDirectActive ||
      atomic_load(&g_bAfQuitting) || (pstMsg->u64Tag != (uint64)g_stKbd.u32TakeGen)) { return DM_SUCCESS; }
  if (AF_KBD_NOW_MS() >= g_stKbd.u64TakeDeadlineMs) { af__kbd_take_timeout(); return DM_SUCCESS; }
  if (0u != g_stKbd.u32InflightBt) { g_stKbd.bTakePending = DM_TRUE; }
  else if (DM_SUCCESS != af__kbd_submit_bt(DM_TRUE, g_stKbd.acTakeAddr, g_stKbd.u32TakeGen)) { g_stKbd.bTaking = DM_FALSE; }
  return DM_SUCCESS;
}

static int af__kbd_on_tick_timer(dm_io void *pvArg) { (void)pvArg; if ((!g_stKbd.bDraining) && (!g_stKbd.bDirectActive)) { af__kbd_drive_tick(AF_KBD_NOW_MS()); } return DM_SUCCESS; }

static int af__kbd_on_stream_ev(dm_io void *pvArg)
{
  dm_link_msg_st *pstMsg = (dm_link_msg_st *)pvArg;
  af_ev_stream_st stEv;
  if ((NULL == pstMsg) || (NULL == pstMsg->pu8Data) || (pstMsg->u32Len < sizeof(stEv))) { return DM_SUCCESS; }
  memcpy(&stEv, pstMsg->pu8Data, sizeof(stEv));
  g_stKbd.bDirectActive = ((AF_STREAM_STARTING == stEv.eCur) || (AF_STREAM_STREAMING == stEv.eCur)) ? DM_TRUE : DM_FALSE;
  if (g_stKbd.bDirectActive) { af__kbd_cancel_take(); }
  return DM_SUCCESS;
}

static int af__kbd_on_sidecar_ev(dm_io void *pvArg)
{
  dm_link_msg_st *pstMsg = (dm_link_msg_st *)pvArg;
  af_ev_sidecar_st stEv;
  if (g_stKbd.bDraining || (NULL == pstMsg->pu8Data) || (pstMsg->u32Len < sizeof(stEv))) { return DM_SUCCESS; }
  memcpy(&stEv, pstMsg->pu8Data, sizeof(stEv));
  if ((AF_SC_SHOWING == stEv.ePrev) && (!g_stKbd.stFlow.bWasShowing)) { g_stKbd.stFlow.bWasShowing = DM_TRUE; }
  g_stKbd.bSidecarShowing = (AF_SC_SHOWING == stEv.eCur) ? DM_TRUE : DM_FALSE;
  AF_KBD_COPY(g_stKbd.acSidecarShowing, g_stKbd.bSidecarShowing ? stEv.acShowing : "");
  af__kbd_drive_tick(AF_KBD_NOW_MS());
  return DM_SUCCESS;
}

static void af__kbd_pair_finish_done(dm_in const af_bt_pair_cb_msg_st *pstCb)
{
  af_bt_device_st stPicked;
  af_bt_device_st *pstFound = NULL;

  g_stKbd.stPair.eStage = AF_BT_PAIR_DONE;
  if ('\0' != pstCb->acAddr[0]) { AF_KBD_COPY(g_stKbd.stPair.acAddr, pstCb->acAddr); }
  g_stKbd.stPair.acCode[0] = '\0';
  g_stKbd.stPair.acMsg[0] = '\0';
  memset(&stPicked, 0, sizeof(stPicked));
  pstFound = af__kbd_find_pair_found(g_stKbd.stPair.acAddr);
  if (NULL != pstFound) { stPicked = *pstFound; }
  else if (pstCb->bHasDev && ('\0' != pstCb->stDev.acAddr[0])) { stPicked = pstCb->stDev; }
  else if ('\0' != g_stKbd.stPair.acAddr[0])
  {
    AF_KBD_COPY(stPicked.acAddr, g_stKbd.stPair.acAddr);
    AF_KBD_COPY(stPicked.acName, g_stKbd.stPair.acAddr);
  }
  else { return; }
  stPicked.bBle = DM_FALSE;
  af_store_state_mut()->bHasKeyboard = DM_TRUE;
  af_store_state_mut()->stKeyboard = stPicked;
  g_stKbd.acError[0] = '\0';
  af__kbd_exec_act(af_kbd_flow_release_now(&g_stKbd.stFlow));
  (void)af_store_save_async(NULL, NULL);
}

static int af__kbd_on_pair_cb(dm_io void *pvArg)
{
  dm_link_msg_st *pstMsg = (dm_link_msg_st *)pvArg;
  af_bt_pair_cb_msg_st stCb;
  if ((NULL == pstMsg->pu8Data) || (pstMsg->u32Len < sizeof(stCb)) || (!g_stKbd.stPair.bActive)) { return DM_SUCCESS; }
  memcpy(&stCb, pstMsg->pu8Data, sizeof(stCb));
  if ((AF_BT_PAIR_SEARCHING == stCb.eStage) && (AF_BT_PAIR_SEARCHING == g_stKbd.stPair.eStage) &&
      stCb.bHasDev && ('\0' != stCb.stDev.acAddr[0]) && (!stCb.stDev.bBle) && af_kbd_device_allowed(&stCb.stDev))
  {
    af_bt_device_st *pstSlot = af__kbd_find_pair_found(stCb.stDev.acAddr);
    if (NULL != pstSlot) { *pstSlot = stCb.stDev; }
    else if (g_stKbd.stPair.u32FoundCnt < AF_BT_DEV_MAX) { g_stKbd.stPair.astFound[g_stKbd.stPair.u32FoundCnt++] = stCb.stDev; }
  }
  else if ((AF_BT_PAIR_SEARCHED == stCb.eStage) && (AF_BT_PAIR_SEARCHING == g_stKbd.stPair.eStage)) { g_stKbd.stPair.eStage = AF_BT_PAIR_SEARCHED; }
  else if (AF_BT_PAIR_PAIRING == stCb.eStage)
  {
    g_stKbd.stPair.eStage = AF_BT_PAIR_PAIRING;
    if ('\0' != stCb.acAddr[0]) { AF_KBD_COPY(g_stKbd.stPair.acAddr, stCb.acAddr); }
    if ('\0' != stCb.acCode[0]) { AF_KBD_COPY(g_stKbd.stPair.acCode, stCb.acCode); }
  }
  else if (AF_BT_PAIR_DONE == stCb.eStage) { af__kbd_pair_finish_done(&stCb); }
  else if (AF_BT_PAIR_FAILED == stCb.eStage)
  {
    g_stKbd.stPair.eStage = AF_BT_PAIR_FAILED;
    AF_KBD_COPY(g_stKbd.stPair.acMsg, ('\0' != stCb.acMsg[0]) ? stCb.acMsg : "配对失败");
  }
  af__kbd_emit_changed();
  return DM_SUCCESS;
}

/* ============================ DEFAULT 泳道查询与 HTTPS 路由 ============================ */

static void af__kbd_query_work(dm_io void *pvArg)
{
  af_kbd_query_job_st *pstJ = (af_kbd_query_job_st *)pvArg;
  uint32 i = 0u;
  pstJ->eAuth = AF_KBD_OP_OF(pstJ->stOps, pfBtAuth, af_plat_bt_auth)();
  if ((AF_BT_AUTH_RESTRICTED == pstJ->eAuth) || (AF_BT_AUTH_DENIED == pstJ->eAuth)) { return; }
  pstJ->u32Count = AF_KBD_OP_OF(pstJ->stOps, pfBtPaired, af_plat_bt_paired)(pstJ->astDevs, AF_BT_DEV_MAX);
  if (!pstJ->bIsSet) { return; }
  for (i = 0u; (i < pstJ->u32Count) && (i < AF_BT_DEV_MAX); i++)
  {
    if (0 == strcasecmp(pstJ->astDevs[i].acAddr, pstJ->acAddr))
    {
      pstJ->bFound = DM_TRUE;
      pstJ->stFound = pstJ->astDevs[i];
      return;
    }
  }
}

static void af__kbd_reply_list(dm_in const af_kbd_query_job_st *pstJ)
{
  yyjson_mut_doc *pstDoc = yyjson_mut_doc_new(dm_json_alc());
  yyjson_mut_val *pstArr = NULL;
  char *pcJson = NULL;
  size_t uLen = 0u;
  uint32 i = 0u;

  if (NULL == pstDoc) { af_https_send_err(pstJ->u64ReplyId, 500u, "internal", "内存不足"); return; }
  pstArr = yyjson_mut_arr(pstDoc);
  yyjson_mut_doc_set_root(pstDoc, pstArr);
  for (i = 0u; (i < pstJ->u32Count) && (i < AF_BT_DEV_MAX); i++)
  {
    const af_bt_device_st *pstD = &pstJ->astDevs[i];
    yyjson_mut_val *pstObj = NULL;
    if (!af_kbd_device_allowed(pstD)) { continue; }
    pstObj = yyjson_mut_obj(pstDoc);
    yyjson_mut_obj_add_str(pstDoc, pstObj, "address", pstD->acAddr);
    yyjson_mut_obj_add_str(pstDoc, pstObj, "name", pstD->acName);
    yyjson_mut_obj_add_bool(pstDoc, pstObj, "connected", pstD->bConnected ? true : false);
    yyjson_mut_obj_add_bool(pstDoc, pstObj, "ble", pstD->bBle ? true : false);
    yyjson_mut_arr_add_val(pstArr, pstObj);
  }
  pcJson = dm_json_write_mut(pstDoc, &uLen);
  yyjson_mut_doc_free(pstDoc);
  if (NULL != pcJson) { af_https_send_ok(pstJ->u64ReplyId, pcJson); dm_json_free_str(pcJson); }
  else { af_https_send_err(pstJ->u64ReplyId, 500u, "internal", "序列化失败"); }
}

static void af__kbd_query_after(dm_io void *pvArg, dm_in int iStatus)
{
  af_kbd_query_job_st *pstJ = (af_kbd_query_job_st *)pvArg;
  af_state_st *pstSt = af_store_state_mut();
  if (g_stKbd.u32InflightQuery > 0u) { g_stKbd.u32InflightQuery--; }
  if (g_stKbd.bDraining) { af_https_send_err(pstJ->u64ReplyId, 500u, "internal", "服务正在退出"); dm_free_tag(pstJ); af__kbd_drain_step(); return; }
  if (DM_SUCCESS != iStatus) { af_https_send_err(pstJ->u64ReplyId, 500u, "internal", "蓝牙查询失败"); }
  else if ((AF_BT_AUTH_RESTRICTED == pstJ->eAuth) || (AF_BT_AUTH_DENIED == pstJ->eAuth)) { af_https_send_err(pstJ->u64ReplyId, 403u, "bt_denied", AF_KBD_DENIED_MSG); }
  else if (!pstJ->bIsSet) { af__kbd_reply_list(pstJ); }
  else if (!pstJ->bFound) { af_https_send_err(pstJ->u64ReplyId, 400u, "bad_request", "未找到已配对的蓝牙设备"); }
  else if (pstJ->stFound.bBle) { af_https_send_err(pstJ->u64ReplyId, 400u, "bad_request", "低功耗蓝牙设备无法交接"); }
  else
  {
    if (pstSt->bHasKeyboard && (0 != strcasecmp(pstSt->stKeyboard.acAddr, pstJ->stFound.acAddr)))
    {
      char acOld[32];
      AF_KBD_COPY(acOld, pstSt->stKeyboard.acAddr);
      af__kbd_release_old_taken(acOld);
    }
    pstSt->bHasKeyboard = DM_TRUE;
    pstSt->stKeyboard = pstJ->stFound;
    g_stKbd.acError[0] = '\0';
    (void)af_store_save_async(NULL, NULL);
    af__kbd_emit_changed();
    af_https_send_ok(pstJ->u64ReplyId, "{\"ok\":true}");
  }
  dm_free_tag(pstJ);
}

static void af__kbd_submit_query(dm_in uint64 u64ReplyId, dm_in bool_t bIsSet, dm_in const char *pcAddr)
{
  af_kbd_query_job_st *pstJ = NULL;
  if (g_stKbd.bDraining) { af_https_send_err(u64ReplyId, 500u, "internal", "服务正在退出"); return; }
  pstJ = (af_kbd_query_job_st *)dm_calloc_tag(1u, sizeof(*pstJ), AF_KBD_TAG_JOB);
  if (NULL == pstJ) { af_https_send_err(u64ReplyId, 500u, "internal", "内存不足"); return; }
  pstJ->u64ReplyId = u64ReplyId;
  pstJ->bIsSet = bIsSet;
  pstJ->stOps = g_stOps;
  if (NULL != pcAddr) { AF_KBD_COPY(pstJ->acAddr, pcAddr); }
  g_stKbd.u32InflightQuery++;
  if (DM_SUCCESS != dm_pool_submit(DM_LANE_DEFAULT, af__kbd_query_work, af__kbd_query_after, pstJ))
  {
    g_stKbd.u32InflightQuery--;
    dm_free_tag(pstJ);
    af_https_send_err(u64ReplyId, 500u, "internal", "蓝牙查询排队失败");
  }
}

static void af__kbd_http_get_keyboards(dm_in const af_https_msg_st *pstMsg)
{
  af_device_st stDev;
  if (af_auth_check_https(pstMsg, &stDev)) { af__kbd_submit_query(pstMsg->u64ReplyId, DM_FALSE, NULL); }
}

static void af__kbd_act_set(dm_in uint64 u64ReplyId, dm_in yyjson_val *pstRoot)
{
  yyjson_val *pstAddr = yyjson_obj_get(pstRoot, "address");
  char acAddr[32] = {0};
  af_state_st *pstSt = af_store_state_mut();
  if ((NULL != pstAddr) && (!yyjson_is_null(pstAddr)) && (!yyjson_is_str(pstAddr))) { af_https_send_err(u64ReplyId, 400u, "bad_request", "address 参数类型错误"); return; }
  if ((NULL != pstAddr) && yyjson_is_str(pstAddr)) { af__trim_copy(yyjson_get_str(pstAddr), acAddr, sizeof(acAddr)); }
  if ('\0' != acAddr[0]) { af__kbd_submit_query(u64ReplyId, DM_TRUE, acAddr); return; }
  if (pstSt->bHasKeyboard)
  {
    char acOld[32];
    AF_KBD_COPY(acOld, pstSt->stKeyboard.acAddr);
    af__kbd_release_old_taken(acOld);
  }
  pstSt->bHasKeyboard = DM_FALSE;
  memset(&pstSt->stKeyboard, 0, sizeof(pstSt->stKeyboard));
  g_stKbd.acError[0] = '\0';
  g_stKbd.bHasReleasedAt = DM_FALSE;
  g_stKbd.u64ReleasedAtMs = 0u;
  (void)af_store_save_async(NULL, NULL);
  af__kbd_emit_changed();
  af_https_send_ok(u64ReplyId, "{\"ok\":true}");
}

static void af__kbd_act_pair(dm_in uint64 u64ReplyId, dm_in yyjson_val *pstRoot)
{
  yyjson_val *pstAddr = yyjson_obj_get(pstRoot, "address");
  char acAddr[32] = {0};
  const af_bt_device_st *pstTarget = NULL;
  if (g_stKbd.stPair.bActive && (AF_BT_PAIR_PAIRING == g_stKbd.stPair.eStage)) { af_https_send_err(u64ReplyId, 409u, "bad_request", "正在配对中"); return; }
  if ((NULL == pstAddr) || (!yyjson_is_str(pstAddr))) { af_https_send_err(u64ReplyId, 400u, "bad_request", "缺少设备地址"); return; }
  af__trim_copy(yyjson_get_str(pstAddr), acAddr, sizeof(acAddr));
  pstTarget = af__kbd_find_pair_found(acAddr);
  if (NULL == pstTarget) { af_https_send_err(u64ReplyId, 400u, "bad_request", "设备不在搜索结果中"); return; }
  g_stKbd.stPair.bActive = DM_TRUE;
  g_stKbd.stPair.eStage = AF_BT_PAIR_PAIRING;
  AF_KBD_COPY(g_stKbd.stPair.acAddr, pstTarget->acAddr);
  g_stKbd.stPair.acCode[0] = '\0';
  g_stKbd.stPair.acMsg[0] = '\0';
  af__kbd_post_main(AF_KBD_MAIN_PAIR_START, g_stKbd.stPair.acAddr);
  af__kbd_emit_changed();
  af_https_send_ok(u64ReplyId, "{\"ok\":true}");
}

static void af__kbd_dispatch_action(dm_in uint64 u64ReplyId, dm_in const char *pcAct, dm_in yyjson_val *pstRoot)
{
  const af_state_st *pstSt = af_store_state();
  if (0 == strcmp(pcAct, "set")) { af__kbd_act_set(u64ReplyId, pstRoot); }
  else if (0 == strcmp(pcAct, "auto"))
  {
    yyjson_val *pstAuto = yyjson_obj_get(pstRoot, "auto");
    if ((NULL == pstAuto) || (!yyjson_is_bool(pstAuto))) { af_https_send_err(u64ReplyId, 400u, "bad_request", "缺少 auto 布尔值"); return; }
    af_store_state_mut()->bKeyboardAuto = yyjson_get_bool(pstAuto) ? DM_TRUE : DM_FALSE;
    (void)af_store_save_async(NULL, NULL);
    af__kbd_emit_changed();
    af_https_send_ok(u64ReplyId, "{\"ok\":true}");
  }
  else if ((0 == strcmp(pcAct, "take")) || (0 == strcmp(pcAct, "release")) || (0 == strcmp(pcAct, "release_now")))
  {
    af_kbd_act_et eAct = AF_KBD_ACT_NONE;
    if ((!pstSt->bHasKeyboard) || ('\0' == pstSt->stKeyboard.acAddr[0])) { af_https_send_err(u64ReplyId, 400u, "bad_request", "未选择蓝牙键盘"); return; }
    if (0 == strcmp(pcAct, "take")) { eAct = af_kbd_flow_take(&g_stKbd.stFlow, AF_KBD_OP(pfBtIsConnected, af_plat_bt_is_connected)(pstSt->stKeyboard.acAddr)); }
    else if (0 == strcmp(pcAct, "release")) { eAct = af_kbd_flow_release(&g_stKbd.stFlow); }
    else { eAct = af_kbd_flow_release_now(&g_stKbd.stFlow); }
    af__kbd_exec_act(eAct);
    af_https_send_ok(u64ReplyId, "{\"ok\":true}");
  }
  else if (0 == strcmp(pcAct, "open_settings")) { af__kbd_post_main(AF_KBD_MAIN_OPEN_URL, AF_KBD_BT_SETTINGS); af_https_send_ok(u64ReplyId, "{\"ok\":true}"); }
  else if (0 == strcmp(pcAct, "pair_search"))
  {
    memset(&g_stKbd.stPair, 0, sizeof(g_stKbd.stPair));
    g_stKbd.stPair.bActive = DM_TRUE;
    g_stKbd.stPair.eStage = AF_BT_PAIR_SEARCHING;
    af__kbd_post_main(AF_KBD_MAIN_PAIR_SEARCH, NULL);
    af__kbd_emit_changed();
    af_https_send_ok(u64ReplyId, "{\"ok\":true}");
  }
  else if (0 == strcmp(pcAct, "pair_cancel"))
  {
    memset(&g_stKbd.stPair, 0, sizeof(g_stKbd.stPair));
    af__kbd_post_main(AF_KBD_MAIN_PAIR_CANCEL, NULL);
    af__kbd_emit_changed();
    af_https_send_ok(u64ReplyId, "{\"ok\":true}");
  }
  else if (0 == strcmp(pcAct, "pair")) { af__kbd_act_pair(u64ReplyId, pstRoot); }
  else { af_https_send_err(u64ReplyId, 400u, "bad_request", "未知操作"); }
}

static void af__kbd_http_post_keyboard(dm_in const af_https_msg_st *pstMsg)
{
  af_device_st stDev;
  yyjson_doc *pstDoc = NULL;
  yyjson_val *pstRoot = NULL;
  yyjson_val *pstAct = NULL;
  int iErr = DM_SUCCESS;
  if (g_stKbd.bDraining) { af_https_send_err(pstMsg->u64ReplyId, 503u, "draining", "服务正在关闭"); return; }
  if (!af_auth_check_https(pstMsg, &stDev)) { return; }
  pstDoc = dm_json_read((const char *)pstMsg->pu8Body, pstMsg->u32BodyLen, 65536u, &iErr);
  if (NULL == pstDoc) { af_https_send_err(pstMsg->u64ReplyId, 400u, "bad_request", "请求 JSON 无效"); return; }
  pstRoot = yyjson_doc_get_root(pstDoc);
  pstAct = yyjson_is_obj(pstRoot) ? yyjson_obj_get(pstRoot, "action") : NULL;
  if ((NULL == pstAct) || (!yyjson_is_str(pstAct))) { yyjson_doc_free(pstDoc); af_https_send_err(pstMsg->u64ReplyId, 400u, "bad_request", "缺少 action 字段"); return; }
  af__kbd_dispatch_action(pstMsg->u64ReplyId, yyjson_get_str(pstAct), pstRoot);
  yyjson_doc_free(pstDoc);
}

void af_kbd_get_snap(dm_out af_kbd_snap_st *pstOut)
{
  const af_state_st *pstSt = af_store_state();
  uint64 u64Now = AF_KBD_NOW_MS();
  if (NULL == pstOut) { return; }
  memset(pstOut, 0, sizeof(*pstOut));
  pstOut->bHasSelected = pstSt->bHasKeyboard;
  if (pstSt->bHasKeyboard) { pstOut->stSelected = pstSt->stKeyboard; pstOut->stSelected.bConnected = AF_KBD_OP(pfBtIsConnected, af_plat_bt_is_connected)(pstSt->stKeyboard.acAddr); }
  pstOut->bAuto = pstSt->bKeyboardAuto;
  pstOut->bTaking = g_stKbd.bTaking;
  AF_KBD_COPY(pstOut->acError, g_stKbd.acError);
  pstOut->bReleased = (g_stKbd.bHasReleasedAt && (u64Now >= g_stKbd.u64ReleasedAtMs) && ((u64Now - g_stKbd.u64ReleasedAtMs) < (uint64)AF_KBD_RELEASED_MS)) ? DM_TRUE : DM_FALSE;
  pstOut->stPair = g_stKbd.stPair;
}

void af_kbd_notify_connect(dm_in const char *pcTarget, dm_in bool_t bKeyboard)
{
  uint64 u64Now = AF_KBD_NOW_MS();
  af_kbd_flow_on_connect(&g_stKbd.stFlow, pcTarget, bKeyboard, u64Now);
  af__kbd_drive_tick(u64Now);
}

void af_kbd_notify_disconnect(dm_in bool_t bShowingNow)
{
  uint64 u64Now = AF_KBD_NOW_MS();
  af__kbd_cancel_take();
  af_kbd_flow_on_disconnect(&g_stKbd.stFlow, bShowingNow, u64Now);
  if (!bShowingNow) { g_stKbd.bSidecarShowing = DM_FALSE; g_stKbd.acSidecarShowing[0] = '\0'; af__kbd_drive_tick(u64Now); }
}

void af_kbd_hold(dm_in uint64 u64UntilMs) { af_kbd_flow_hold(&g_stKbd.stFlow, u64UntilMs); }

static int af__kbd_init(void)
{
  int iErr = DM_SUCCESS;
  memset(&g_stKbd, 0, sizeof(g_stKbd));
  dm_mem_tag_name(AF_KBD_TAG_JOB, "af_kbd_job");
  dm_mem_tag_name(AF_KBD_TAG_MAIN, "af_kbd_main");
  af_kbd_flow_init(&g_stKbd.stFlow, DM_FALSE, af_store_state()->bKeyboardTaken);
  if (DM_SUCCESS != (iErr = dm_link_register_handler(AF_LINK_KBD_PAIR_CB, af__kbd_on_pair_cb, NULL, NULL))) { return iErr; }
  if (DM_SUCCESS != (iErr = dm_link_register_handler(AF_KBD_LINK_TAKE_TMR, af__kbd_on_take_timer, NULL, NULL))) { return iErr; }
  if (DM_SUCCESS != (iErr = dm_link_register_handler(AF_KBD_LINK_TICK_TMR, af__kbd_on_tick_timer, NULL, NULL))) { return iErr; }
  if (DM_SUCCESS != (iErr = dm_event_subscribe(AF_EV_SIDECAR_CHANGED, af__kbd_on_sidecar_ev, NULL, NULL))) { return iErr; }
  if (DM_SUCCESS != (iErr = dm_event_subscribe(AF_EV_STREAM_CHANGED, af__kbd_on_stream_ev, NULL, NULL))) { return iErr; }
  if (DM_SUCCESS != (iErr = af_https_route_exact("GET", "/keyboards", af__kbd_http_get_keyboards))) { return iErr; }
  if (DM_SUCCESS != (iErr = af_https_route_exact("POST", "/keyboard", af__kbd_http_post_keyboard))) { return iErr; }
  return dm_timer_add(AF_KBD_TICK_MS, AF_KBD_TICK_MS, AF_KBD_LINK_TICK_TMR, 0u, &g_stKbd.u32TickTimer);
}

static void af__kbd_drain(dm_io dm_drain_st *pstDrain)
{
  bool_t bNeedDisc = DM_FALSE;
  char acDiscAddr[32] = {0};
  const af_state_st *pstSt = af_store_state();
  g_stKbd.bDraining = DM_TRUE;
  af__kbd_cancel_take();
  af__kbd_stop_timer(&g_stKbd.u32TickTimer);
  if (g_stKbd.stPair.bActive) { memset(&g_stKbd.stPair, 0, sizeof(g_stKbd.stPair)); af__kbd_post_main(AF_KBD_MAIN_PAIR_CANCEL, NULL); }
  if (g_stKbd.bDisconnectPending && ('\0' != g_stKbd.acPendingDiscAddr[0]))
  {
    bNeedDisc = DM_TRUE;
    AF_KBD_COPY(acDiscAddr, g_stKbd.acPendingDiscAddr);
    g_stKbd.bDisconnectPending = DM_FALSE;
    g_stKbd.acPendingDiscAddr[0] = '\0';
  }
  else if (g_stKbd.stFlow.bTaken && pstSt->bHasKeyboard && ('\0' != pstSt->stKeyboard.acAddr[0]))
  {
    bNeedDisc = DM_TRUE;
    AF_KBD_COPY(acDiscAddr, pstSt->stKeyboard.acAddr);
    g_stKbd.stFlow.bTaken = DM_FALSE;
    af_store_state_mut()->bKeyboardTaken = DM_FALSE;
    g_stKbd.bDrainSaveInflight = DM_TRUE;
    if (DM_SUCCESS != af_store_save_async(af__kbd_on_drain_saved, NULL)) { g_stKbd.bDrainSaveInflight = DM_FALSE; }
  }
  if ((g_stKbd.u32InflightBt > 0u) || (g_stKbd.u32InflightQuery > 0u) || bNeedDisc || g_stKbd.bDrainSaveInflight)
  {
    int iHold = dm_drain_hold(pstDrain);
    if (iHold > 0)
    {
      g_stKbd.iDrainHold = iHold;
      g_stKbd.bDrainNeedDisc = bNeedDisc;
      AF_KBD_COPY(g_stKbd.acDrainDiscAddr, acDiscAddr);
      af__kbd_drain_step();
    }
  }
}

static void af__kbd_fini(void)
{
  af__kbd_cancel_take(); af__kbd_stop_timer(&g_stKbd.u32TickTimer);
  (void)dm_event_unsubscribe(AF_EV_STREAM_CHANGED, af__kbd_on_stream_ev);
  (void)dm_event_unsubscribe(AF_EV_SIDECAR_CHANGED, af__kbd_on_sidecar_ev);
  memset(&g_stKbd, 0, sizeof(g_stKbd));
}
const dm_module_st g_stAfKbdMod = {.u16ModID = AF_MOD_KBD, .pcName = "af_kbd", .pfInit = af__kbd_init,
                                   .pfDrain = af__kbd_drain, .pfFini = af__kbd_fini, .pastErrMap = g_astKbdErrs};
const dm_module_st *af_kbd_module(void) { return &g_stAfKbdMod; }
