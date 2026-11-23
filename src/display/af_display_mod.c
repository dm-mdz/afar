/**
 ******************************************************************************
 * @file    af_display_mod.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   服务层 af_display（ModID 17）DM 模块装配、1s 纠偏、主线程异步虚屏与双路径 pfDrain。
 ******************************************************************************
 */

#define DM_LOG_MOD "display"

#include "af_display.h"
#include "../af_modules.h"
#include "../af_store.h"

#include <dm.h>
#include <dm/event.h>
#include <dm/link.h>
#include <dm/log.h>
#include <dm/timer.h>

#include <stdio.h>
#include <string.h>

#define AF_DISP_MEMTAG    DM_MOD_MEMTAG(AF_MOD_DISPLAY, 0u)
#define AF_DISP_TIMER_TAG 0xD1590001ULL

typedef enum af__disp_main_op
{
  AF_DISP_OP_CREATE = 1,
  AF_DISP_OP_DESTROY = 2,
  AF_DISP_OP_SCALE = 3
} af__disp_main_op_et;

typedef struct af__disp_main_done
{
  af__disp_main_op_et eOp;
  int iStatus;
  uint32 u32Serial;
  uint32 u32DispId;
  uint64 u64Handle;
  char acKey[16];
  char acSidecarName[65];
  char acErr[128];
} af__disp_main_done_st;

typedef struct af__disp_scale_job
{
  af_display_ops_st stOps;
  af_scale_preset_st astP[AF_SCALE_COUNT];
  char acSidecarName[65];
  char acKey[16];
  uint32 aau32Extra[16][2];
  uint32 u32ExtraCnt;
  uint32 u32ScId;
  bool_t bMirror;
  uint32 u32VdDispId;
  uint64 u64VdHandle;
  uint32 u32VdSerial;
} af__disp_scale_job_st;

typedef struct af__disp_create_job
{
  int (*pfVdCreate)(in uint32, in uint32, in uint32, in const af_vd_mode_req_st *, in uint32,
                    out uint32 *, out uint64 *);
  uint32 u32Serial;
  uint32 u32MaxPw;
  uint32 u32MaxPh;
  uint32 u32Count;
  af_vd_mode_req_st astModes[AF_VD_MODES_MAX];
} af__disp_create_job_st;

typedef struct af__disp_destroy_job
{
  void (*pfVdDestroy)(in uint64);
  void (*pfVdDestroyAll)(void);
  uint64 u64Handle;
  bool_t bDestroyAll;
} af__disp_destroy_job_st;

typedef struct af__disp_ctx
{
  af_vd_state_et eVdState;
  uint32 u32PhysCount;
  uint32 u32VdDispId;
  uint64 u64VdHandle;
  uint32 u32VdSerial;
  bool_t bVdCreateInflight;
  bool_t bVdDestroyInflight;
  bool_t bSidecarShowing;
  bool_t bHasSidecarSince;
  uint64 u64SidecarSinceMs;
  char acShowingName[65];
  bool_t bCalibrated;
  char acCalibratedName[65];
  bool_t bHoldSet;
  uint64 u64HoldUntilMs;
  bool_t bScaleBusy;
  char acScaleError[128];
  char acActualScale[16];
  af_vd_plan_st stPlan;
  af_phys_guard_st stGuard;
  af_desk_st stDesk;
  uint32 u32TimerId;
  bool_t bDraining;
  int iDrainHold;
  bool_t bEvPublishedOnce;
} af__disp_ctx_st;

static af__disp_ctx_st g_stDisp;
static af_display_ops_st g_stDispOps;
static bool_t g_bDispOpsInited = DM_FALSE;
static uint32 g_u32DispTickMs = 1000u;

static uint64 af__disp_default_now_ms(void) { return dm_hrtime() / 1000000ULL; }

static void af__disp_init_default_ops(out af_display_ops_st *pstOps)
{
  memset(pstOps, 0, sizeof(*pstOps));
  pstOps->pfRunOnMain = af_plat_run_on_main;
  pstOps->pfDispOnline = af_plat_disp_online;
  pstOps->pfDispModes = af_plat_disp_modes;
  pstOps->pfDispSetMode = af_plat_disp_set_mode;
  pstOps->pfDispSetHidpiUnmirrored = af_plat_disp_set_hidpi_unmirrored;
  pstOps->pfDispMirror = af_plat_disp_mirror;
  pstOps->pfDispWatchStart = af_plat_disp_watch_start;
  pstOps->pfDispWatchStop = af_plat_disp_watch_stop;
  pstOps->pfVdCreate = af_plat_vd_create;
  pstOps->pfVdApply = af_plat_vd_apply;
  pstOps->pfVdDestroy = af_plat_vd_destroy;
  pstOps->pfVdDestroyAll = af_plat_vd_destroy_all;
  pstOps->pfNowMs = af__disp_default_now_ms;
}

static const af_display_ops_st *af__disp_ops(void)
{
  if (!g_bDispOpsInited) { af__disp_init_default_ops(&g_stDispOps); g_bDispOpsInited = DM_TRUE; }
  return &g_stDispOps;
}

void af_display_set_ops(in const af_display_ops_st *pstOps)
{
  af__disp_init_default_ops(&g_stDispOps);
  g_bDispOpsInited = DM_TRUE;
  if (NULL == pstOps) { return; }
  if (NULL != pstOps->pfRunOnMain) { g_stDispOps.pfRunOnMain = pstOps->pfRunOnMain; }
  if (NULL != pstOps->pfDispOnline) { g_stDispOps.pfDispOnline = pstOps->pfDispOnline; }
  if (NULL != pstOps->pfDispModes) { g_stDispOps.pfDispModes = pstOps->pfDispModes; }
  if (NULL != pstOps->pfDispSetMode) { g_stDispOps.pfDispSetMode = pstOps->pfDispSetMode; }
  if (NULL != pstOps->pfDispSetHidpiUnmirrored) { g_stDispOps.pfDispSetHidpiUnmirrored = pstOps->pfDispSetHidpiUnmirrored; }
  if (NULL != pstOps->pfDispMirror) { g_stDispOps.pfDispMirror = pstOps->pfDispMirror; }
  if (NULL != pstOps->pfDispWatchStart) { g_stDispOps.pfDispWatchStart = pstOps->pfDispWatchStart; }
  if (NULL != pstOps->pfDispWatchStop) { g_stDispOps.pfDispWatchStop = pstOps->pfDispWatchStop; }
  if (NULL != pstOps->pfVdCreate) { g_stDispOps.pfVdCreate = pstOps->pfVdCreate; }
  if (NULL != pstOps->pfVdApply) { g_stDispOps.pfVdApply = pstOps->pfVdApply; }
  if (NULL != pstOps->pfVdDestroy) { g_stDispOps.pfVdDestroy = pstOps->pfVdDestroy; }
  if (NULL != pstOps->pfVdDestroyAll) { g_stDispOps.pfVdDestroyAll = pstOps->pfVdDestroyAll; }
  if (NULL != pstOps->pfNowMs) { g_stDispOps.pfNowMs = pstOps->pfNowMs; }
}

void af_display_set_tick_ms(in uint32 u32TickMs) { g_u32DispTickMs = (u32TickMs > 0u) ? u32TickMs : 1000u; }

/* ============================ 主线程异步虚屏建/拆任务 ============================ */

static void af__disp_main_create_job(io void *pvCtx)
{
  af__disp_create_job_st *pstJob = (af__disp_create_job_st *)pvCtx;
  af__disp_main_done_st stDone;

  memset(&stDone, 0, sizeof(stDone));
  stDone.eOp = AF_DISP_OP_CREATE;
  stDone.u32Serial = pstJob->u32Serial;
  stDone.iStatus = pstJob->pfVdCreate(pstJob->u32Serial, pstJob->u32MaxPw, pstJob->u32MaxPh,
                                      pstJob->astModes, pstJob->u32Count, &stDone.u32DispId,
                                      &stDone.u64Handle);
  dm_free_tag(pstJob);
  (void)dm_queue_post_async(AF_LINK_DISP_MAIN_DONE, &stDone, (uint32)sizeof(stDone));
}

static void af__disp_main_destroy_job(io void *pvCtx)
{
  af__disp_destroy_job_st *pstJob = (af__disp_destroy_job_st *)pvCtx;
  af__disp_main_done_st stDone;

  memset(&stDone, 0, sizeof(stDone));
  stDone.eOp = AF_DISP_OP_DESTROY;
  stDone.iStatus = DM_SUCCESS;
  stDone.u64Handle = pstJob->u64Handle;
  if ((0u != pstJob->u64Handle) && (NULL != pstJob->pfVdDestroy))
  {
    pstJob->pfVdDestroy(pstJob->u64Handle);
  }
  if (pstJob->bDestroyAll && (NULL != pstJob->pfVdDestroyAll))
  {
    pstJob->pfVdDestroyAll();
  }
  dm_free_tag(pstJob);
  (void)dm_queue_post_async(AF_LINK_DISP_MAIN_DONE, &stDone, (uint32)sizeof(stDone));
}

static void af__disp_main_scale_job(io void *pvCtx)
{
  af__disp_scale_job_st *pstJob = (af__disp_scale_job_st *)pvCtx;
  af__disp_main_done_st stDone;

  memset(&stDone, 0, sizeof(stDone));
  stDone.eOp = AF_DISP_OP_SCALE;
  snprintf(stDone.acKey, sizeof(stDone.acKey), "%s", pstJob->acKey);
  snprintf(stDone.acSidecarName, sizeof(stDone.acSidecarName), "%s", pstJob->acSidecarName);
  stDone.u32DispId = pstJob->u32VdDispId;
  stDone.u64Handle = pstJob->u64VdHandle;
  stDone.u32Serial = pstJob->u32VdSerial;
  stDone.iStatus = af_disp_scale_fallback_exec(
      &pstJob->stOps, pstJob->astP, pstJob->acKey, pstJob->aau32Extra, pstJob->u32ExtraCnt,
      pstJob->u32ScId, pstJob->bMirror, &stDone.u32DispId, &stDone.u64Handle, &stDone.u32Serial,
      stDone.acErr, sizeof(stDone.acErr));
  dm_free_tag(pstJob);
  (void)dm_queue_post_async(AF_LINK_DISP_MAIN_DONE, &stDone, (uint32)sizeof(stDone));
}

static void af__disp_start_create(void)
{
  const af_display_ops_st *pstOps = af__disp_ops();
  const af_config_st *pstCfg = af_store_cfg();
  af_scale_preset_st astP[AF_SCALE_COUNT];
  af__disp_create_job_st *pstJob = NULL;
  const char *pcName = g_stDisp.bSidecarShowing ? g_stDisp.acShowingName : "";
  const char *pcWant = af_disp_store_target_scale(pcName);

  if (g_stDisp.bVdCreateInflight || g_stDisp.bVdDestroyInflight || g_stDisp.bScaleBusy
      || g_stDisp.bDraining)
  {
    return;
  }
  pstJob = (af__disp_create_job_st *)dm_calloc_tag(1u, sizeof(*pstJob), AF_DISP_MEMTAG);
  if (NULL == pstJob)
  {
    return;
  }
  af_disp_store_resolve_presets(pcName, astP);
  pstJob->pfVdCreate = pstOps->pfVdCreate;
  pstJob->u32Serial = (1u == g_stDisp.u32VdSerial) ? 2u : 1u;
  pstJob->u32Count = af_vd_build_modes(astP, pcWant, pstCfg->aau32Extra, pstCfg->u32ExtraCnt, 0u,
                                       pstJob->astModes, AF_VD_MODES_MAX, &pstJob->u32MaxPw,
                                       &pstJob->u32MaxPh);
  if (0u == pstJob->u32Count)
  {
    dm_free_tag(pstJob);
    return;
  }
  g_stDisp.bVdCreateInflight = DM_TRUE;
  pstOps->pfRunOnMain(af__disp_main_create_job, pstJob);
}

static void af__disp_start_destroy(in uint64 u64Handle, in bool_t bDestroyAll)
{
  const af_display_ops_st *pstOps = af__disp_ops();
  af__disp_destroy_job_st *pstJob = NULL;

  if (g_stDisp.bVdDestroyInflight || g_stDisp.bScaleBusy)
  {
    return;
  }
  pstJob = (af__disp_destroy_job_st *)dm_calloc_tag(1u, sizeof(*pstJob), AF_DISP_MEMTAG);
  if (NULL == pstJob)
  {
    if (g_stDisp.iDrainHold > 0)
    {
      int iH = g_stDisp.iDrainHold;
      g_stDisp.iDrainHold = 0;
      dm_drain_release(iH);
    }
    return;
  }
  pstJob->pfVdDestroy = pstOps->pfVdDestroy;
  pstJob->pfVdDestroyAll = pstOps->pfVdDestroyAll;
  pstJob->u64Handle = u64Handle;
  pstJob->bDestroyAll = bDestroyAll;
  g_stDisp.bVdDestroyInflight = DM_TRUE;
  pstOps->pfRunOnMain(af__disp_main_destroy_job, pstJob);
}

/* ============================ 核心纠偏与事件发布 ============================ */

static void af__disp_publish_changed(void)
{
  af_ev_display_st stEv;
  if (g_stDisp.bDraining)
  {
    return;
  }
  memset(&stEv, 0, sizeof(stEv));
  stEv.u32PhysCount = g_stDisp.u32PhysCount;
  stEv.eVdState = g_stDisp.eVdState;
  stEv.bHasVd = (0u != g_stDisp.u64VdHandle) ? DM_TRUE : DM_FALSE;
  (void)dm_event_publish(AF_EV_DISPLAY_CHANGED, &stEv, (uint32)sizeof(stEv));
  g_stDisp.bEvPublishedOnce = DM_TRUE;
}

static void af__disp_sync_actual_scale(in const af_disp_info_st *pstVdInfo, in const char *pcName)
{
  af_scale_preset_st astP[AF_SCALE_COUNT];
  uint32 i = 0u;

  g_stDisp.acActualScale[0] = '\0';
  if ((NULL == pstVdInfo) || (!pstVdInfo->bHasCur))
  {
    return;
  }
  af_disp_store_resolve_presets(pcName, astP);
  for (i = 0u; i < AF_SCALE_COUNT; i++)
  {
    if ((pstVdInfo->stCur.u32W == astP[i].u32W) && (pstVdInfo->stCur.u32H == astP[i].u32H))
    {
      snprintf(g_stDisp.acActualScale, sizeof(g_stDisp.acActualScale), "%s", astP[i].pcKey);
      return;
    }
  }
}

static void af__disp_handle_ipad_vd(in const af_disp_info_st *pstVdInfo,
                                    in const af_disp_info_st *pstScInfo)
{
  const af_display_ops_st *pstOps = af__disp_ops();
  const af_config_st *pstCfg = af_store_cfg();

  if (0u == g_stDisp.u32VdDispId)
  {
    return;
  }
  if (NULL != pstScInfo)
  {
    if (pstCfg->bMirror && (pstScInfo->u32MirrorOf != g_stDisp.u32VdDispId))
    {
      (void)pstOps->pfDispMirror(pstScInfo->u32Id, g_stDisp.u32VdDispId);
    }
    if (g_stDisp.bSidecarShowing && ('\0' != g_stDisp.acShowingName[0])
        && ((!g_stDisp.bCalibrated)
            || (0 != strcmp(g_stDisp.acCalibratedName, g_stDisp.acShowingName))))
    {
      af_disp_mode_st astScModes[AF_DISP_MODES_MAX];
      af_scale_preset_st astCal[AF_SCALE_COUNT];
      uint32 u32PxW = 0u;
      uint32 u32PxH = 0u;
      uint32 u32Cnt = pstOps->pfDispModes(pstScInfo->u32Id, astScModes, AF_DISP_MODES_MAX);
      af_disp_store_ipad_pixels(g_stDisp.acShowingName, &u32PxW, &u32PxH);
      if (af_disp_from_sidecar(u32PxW, u32PxH, astScModes, u32Cnt, astCal))
      {
        af_disp_store_save_sizes(g_stDisp.acShowingName, astCal);
      }
      g_stDisp.bCalibrated = DM_TRUE;
      snprintf(g_stDisp.acCalibratedName, sizeof(g_stDisp.acCalibratedName), "%s",
               g_stDisp.acShowingName);
    }
  }
  if ((NULL != pstVdInfo) && g_stDisp.bSidecarShowing)
  {
    af_disp_mode_st astVdModes[AF_DISP_MODES_MAX];
    af_scale_preset_st astP[AF_SCALE_COUNT];
    af_disp_mode_st stTarget;
    uint32 u32MCnt = pstOps->pfDispModes(g_stDisp.u32VdDispId, astVdModes, AF_DISP_MODES_MAX);
    const char *pcWant = af_disp_store_target_scale(g_stDisp.acShowingName);
    af_disp_store_resolve_presets(g_stDisp.acShowingName, astP);
    if (AF_FOLLOW_SWITCH
        == af_disp_follow_eval(&pstVdInfo->stCur, pstVdInfo->bHasCur, astVdModes, u32MCnt, astP,
                               pcWant, &stTarget))
    {
      (void)pstOps->pfDispSetMode(g_stDisp.u32VdDispId, &stTarget, DM_FALSE);
    }
  }
}

static void af__disp_handle_desk_and_ipad(in const af_disp_info_st *pstVdInfo,
                                          in const af_disp_info_st *pstScInfo, in uint64 u64Now)
{
  const af_display_ops_st *pstOps = af__disp_ops();

  if (AF_VD_HEADLESS_DESK == g_stDisp.eVdState)
  {
    af_disp_mode_st stRestore;
    if ((NULL != pstVdInfo) && pstVdInfo->bHasCur)
    {
      af_desk_act_et eAct = af_desk_step(&g_stDisp.stDesk, &pstVdInfo->stCur, u64Now, &stRestore);
      if (AF_DESK_REMEMBER == eAct)
      {
        af_state_st *pstS = af_store_state_mut();
        pstS->bHasDeskMode = DM_TRUE;
        pstS->au32DeskMode[0] = pstVdInfo->stCur.u32W;
        pstS->au32DeskMode[1] = pstVdInfo->stCur.u32H;
        pstS->au32DeskMode[2] = pstVdInfo->stCur.u32Pw;
        pstS->au32DeskMode[3] = pstVdInfo->stCur.u32Ph;
        (void)af_store_save_async(NULL, NULL);
      }
      else if (AF_DESK_RESTORE == eAct)
      {
        (void)pstOps->pfDispSetMode(g_stDisp.u32VdDispId, &stRestore, DM_FALSE);
      }
    }
    return;
  }
  af__disp_handle_ipad_vd(pstVdInfo, pstScInfo);
}

static void af__disp_reconcile(void)
{
  const af_display_ops_st *pstOps = af__disp_ops();
  const af_config_st *pstCfg = af_store_cfg();
  const af_state_st *pstSt = af_store_state();
  af_disp_info_st astOnline[AF_DISP_MAX];
  uint32 u32OnlineCnt = 0u;
  uint64 u64Now = pstOps->pfNowMs();
  uint32 u32PrevPhys = g_stDisp.u32PhysCount;
  af_vd_state_et ePrevState = g_stDisp.eVdState;
  bool_t bHasVd = (0u != g_stDisp.u64VdHandle) ? DM_TRUE : DM_FALSE;
  bool_t bHold = g_stDisp.bHoldSet && (u64Now < g_stDisp.u64HoldUntilMs);
  bool_t bStable2s = g_stDisp.bSidecarShowing && g_stDisp.bHasSidecarSince
                     && (u64Now >= g_stDisp.u64SidecarSinceMs)
                     && ((u64Now - g_stDisp.u64SidecarSinceMs) >= AF_VD_STABLE_MS);
  bool_t bWantVd = DM_FALSE;
  const af_disp_info_st *pstVdInfo = NULL;
  const af_disp_info_st *pstScInfo = NULL;
  uint32 i = 0u;

  if (g_stDisp.bDraining)
  {
    return;
  }
  u32OnlineCnt = pstOps->pfDispOnline(astOnline, AF_DISP_MAX);
  g_stDisp.u32PhysCount = af_phys_guard_update(&g_stDisp.stGuard, astOnline, u32OnlineCnt,
                                               g_stDisp.u32VdDispId, u64Now);
  for (i = 0u; i < u32OnlineCnt; i++)
  {
    bool_t bOurs = (0u != g_stDisp.u32VdDispId) && (astOnline[i].u32Id == g_stDisp.u32VdDispId);
    af_disp_kind_et eK = af_disp_classify(bOurs, astOnline[i].u32Vendor, astOnline[i].u32Model);
    if (AF_DISP_OURS == eK) { pstVdInfo = &astOnline[i]; }
    else if (AF_DISP_SIDECAR == eK) { pstScInfo = &astOnline[i]; }
  }
  af__disp_sync_actual_scale(pstVdInfo, g_stDisp.acShowingName);
  g_stDisp.eVdState = af_vd_eval_state(g_stDisp.u32PhysCount, g_stDisp.bSidecarShowing, bStable2s,
                                       pstSt->bAntiUnplug, pstCfg->bMirror, bHold, bHasVd,
                                       &bWantVd);
  if ((!g_stDisp.bEvPublishedOnce) || (u32PrevPhys != g_stDisp.u32PhysCount)
      || (ePrevState != g_stDisp.eVdState))
  {
    if ((AF_VD_HEADLESS_DESK == g_stDisp.eVdState) && (AF_VD_HEADLESS_DESK != ePrevState))
    {
      af_desk_on_enter_desk(&g_stDisp.stDesk);
    }
    else if (AF_VD_HEADLESS_IPAD == g_stDisp.eVdState)
    {
      af_desk_on_ipad(&g_stDisp.stDesk);
    }
    af__disp_publish_changed();
  }
  if (af_phys_guard_is_active(&g_stDisp.stGuard, u64Now))
  {
    return;
  }
  if (af_phys_guard_recover(&g_stDisp.stGuard, astOnline, u32OnlineCnt, g_stDisp.u32VdDispId,
                            u64Now, pstOps))
  {
    (void)af_store_save_async(NULL, NULL);
  }
  switch (af_vd_plan_step(&g_stDisp.stPlan, bWantVd, bHasVd, bHold, u64Now))
  {
    case AF_VD_PLAN_CREATE: af__disp_start_create(); break;
    case AF_VD_PLAN_DESTROY: af__disp_start_destroy(g_stDisp.u64VdHandle, DM_FALSE); break;
    case AF_VD_PLAN_KEEP:
      if (!g_stDisp.bScaleBusy) { af__disp_handle_desk_and_ipad(pstVdInfo, pstScInfo, u64Now); }
      break;
    default: break;
  }
}

/* ============================ DM 回调与生命周期 ============================ */

static int af__disp_on_main_done(void *pvArg)
{
  const dm_link_msg_st *pstMsg = (const dm_link_msg_st *)pvArg;
  af__disp_main_done_st stDone;

  if ((NULL == pstMsg) || (pstMsg->u32Len < sizeof(stDone)) || (NULL == pstMsg->pu8Data))
  {
    return DM_SUCCESS;
  }
  memcpy(&stDone, pstMsg->pu8Data, sizeof(stDone));
  if (AF_DISP_OP_CREATE == stDone.eOp)
  {
    g_stDisp.bVdCreateInflight = DM_FALSE;
    if (DM_SUCCESS == stDone.iStatus)
    {
      g_stDisp.u32VdSerial = stDone.u32Serial;
      g_stDisp.u32VdDispId = stDone.u32DispId;
      g_stDisp.u64VdHandle = stDone.u64Handle;
      af_desk_on_vd_rebuilt(&g_stDisp.stDesk);
    }
    if (g_stDisp.bDraining)
    {
      af__disp_start_destroy(g_stDisp.u64VdHandle, DM_TRUE);
      return DM_SUCCESS;
    }
    af__disp_publish_changed();
  }
  else if (AF_DISP_OP_DESTROY == stDone.eOp)
  {
    g_stDisp.bVdDestroyInflight = DM_FALSE;
    g_stDisp.u32VdDispId = 0u;
    g_stDisp.u64VdHandle = 0u;
    if (g_stDisp.bDraining)
    {
      if (g_stDisp.iDrainHold > 0)
      {
        int iH = g_stDisp.iDrainHold;
        g_stDisp.iDrainHold = 0;
        dm_drain_release(iH);
      }
      return DM_SUCCESS;
    }
    af__disp_publish_changed();
  }
  else if (AF_DISP_OP_SCALE == stDone.eOp)
  {
    g_stDisp.bScaleBusy = DM_FALSE;
    g_stDisp.u32VdDispId = stDone.u32DispId;
    g_stDisp.u64VdHandle = stDone.u64Handle;
    g_stDisp.u32VdSerial = stDone.u32Serial;
    snprintf(g_stDisp.acScaleError, sizeof(g_stDisp.acScaleError), "%s", stDone.acErr);
    if (DM_SUCCESS == stDone.iStatus)
    {
      snprintf(g_stDisp.acActualScale, sizeof(g_stDisp.acActualScale), "%s", stDone.acKey);
    }
    else
    {
      af_disp_store_save_scale(stDone.acSidecarName, "default");
    }
    if (g_stDisp.bDraining)
    {
      if (0u != g_stDisp.u64VdHandle) { af__disp_start_destroy(g_stDisp.u64VdHandle, DM_TRUE); }
      else if (g_stDisp.iDrainHold > 0)
      {
        int iH = g_stDisp.iDrainHold;
        g_stDisp.iDrainHold = 0;
        dm_drain_release(iH);
      }
      return DM_SUCCESS;
    }
    af__disp_publish_changed();
  }
  return DM_SUCCESS;
}

static int af__disp_on_reconf(void *pvArg)
{
  (void)pvArg;
  af__disp_reconcile();
  return DM_SUCCESS;
}

static int af__disp_on_sidecar_ev(void *pvArg)
{
  const dm_link_msg_st *pstMsg = (const dm_link_msg_st *)pvArg;
  af_ev_sidecar_st stEv;
  uint64 u64Now = af__disp_ops()->pfNowMs();

  if ((NULL == pstMsg) || (pstMsg->u32Len < sizeof(stEv)) || (NULL == pstMsg->pu8Data))
  {
    return DM_SUCCESS;
  }
  memcpy(&stEv, pstMsg->pu8Data, sizeof(stEv));
  if (AF_SC_SHOWING == stEv.eCur)
  {
    if (!g_stDisp.bSidecarShowing)
    {
      g_stDisp.bHasSidecarSince = DM_TRUE;
      g_stDisp.u64SidecarSinceMs = u64Now;
    }
    g_stDisp.bSidecarShowing = DM_TRUE;
    snprintf(g_stDisp.acShowingName, sizeof(g_stDisp.acShowingName), "%s", stEv.acShowing);
  }
  else
  {
    g_stDisp.bSidecarShowing = DM_FALSE;
    g_stDisp.bHasSidecarSince = DM_FALSE;
    g_stDisp.u64SidecarSinceMs = 0u;
    g_stDisp.acShowingName[0] = '\0';
  }
  af__disp_reconcile();
  return DM_SUCCESS;
}

static int af__disp_on_ready(void *pvArg)
{
  const af_state_st *pstS = af_store_state();
  const af_display_ops_st *pstOps = af__disp_ops();
  (void)pvArg;

  if (pstS->bHasDeskMode)
  {
    af_desk_load_saved(&g_stDisp.stDesk, pstS->au32DeskMode[0], pstS->au32DeskMode[1],
                       pstS->au32DeskMode[2], pstS->au32DeskMode[3]);
  }
  pstOps->pfDispWatchStart(AF_LINK_DISP_RECONF);
  (void)dm_timer_add(g_u32DispTickMs, g_u32DispTickMs, AF_LINK_DISP_RECONF, AF_DISP_TIMER_TAG,
                     &g_stDisp.u32TimerId);
  af__disp_reconcile();
  return DM_SUCCESS;
}

static int af__disp_init(void)
{
  int iRc = DM_SUCCESS;

  memset(&g_stDisp, 0, sizeof(g_stDisp));
  af_phys_guard_init(&g_stDisp.stGuard);
  af_desk_init(&g_stDisp.stDesk);
  dm_mem_tag_name(AF_DISP_MEMTAG, "af_display");

  iRc = dm_link_register_handler(AF_LINK_DISP_MAIN_DONE, af__disp_on_main_done, NULL, NULL);
  if (DM_SUCCESS != iRc) { return iRc; }
  iRc = dm_link_register_handler(AF_LINK_DISP_RECONF, af__disp_on_reconf, NULL, NULL);
  if (DM_SUCCESS != iRc) { return iRc; }
  iRc = dm_event_subscribe(DM_EV_READY, af__disp_on_ready, NULL, NULL);
  if (DM_SUCCESS != iRc) { return iRc; }
  return dm_event_subscribe(AF_EV_SIDECAR_CHANGED, af__disp_on_sidecar_ev, NULL, NULL);
}

static void af__disp_drain(io dm_drain_st *pstDrain)
{
  const af_display_ops_st *pstOps = af__disp_ops();

  g_stDisp.bDraining = DM_TRUE;
  if (0u != g_stDisp.u32TimerId)
  {
    (void)dm_timer_cancel(g_stDisp.u32TimerId);
    g_stDisp.u32TimerId = 0u;
  }
  pstOps->pfDispWatchStop();
  if ((0u != g_stDisp.u64VdHandle) || g_stDisp.bVdCreateInflight || g_stDisp.bVdDestroyInflight
      || g_stDisp.bScaleBusy)
  {
    g_stDisp.iDrainHold = dm_drain_hold(pstDrain);
    if ((!g_stDisp.bVdCreateInflight) && (!g_stDisp.bVdDestroyInflight) && (!g_stDisp.bScaleBusy))
    {
      af__disp_start_destroy(g_stDisp.u64VdHandle, DM_TRUE);
    }
  }
}

static void af__disp_fini(void)
{
  (void)dm_event_unsubscribe(AF_EV_SIDECAR_CHANGED, af__disp_on_sidecar_ev);
  (void)dm_event_unsubscribe(DM_EV_READY, af__disp_on_ready);
  memset(&g_stDisp, 0, sizeof(g_stDisp));
}

/* ============================ DM 线程公共 API ============================ */

void af_display_get_snap(in const char *pcSidecarName, out af_disp_snap_st *pstOut)
{
  if (NULL == pstOut)
  {
    return;
  }
  memset(pstOut, 0, sizeof(*pstOut));
  pstOut->eVdState = g_stDisp.eVdState;
  pstOut->u32PhysCount = g_stDisp.u32PhysCount;
  snprintf(pstOut->acActualScale, sizeof(pstOut->acActualScale), "%s", g_stDisp.acActualScale);
  af_disp_store_resolve_presets(pcSidecarName, pstOut->astScales);
  pstOut->bScaleBusy = g_stDisp.bScaleBusy;
  snprintf(pstOut->acScaleError, sizeof(pstOut->acScaleError), "%s", g_stDisp.acScaleError);
}

int af_display_set_scale(in const char *pcSidecarName, in const char *pcKey)
{
  const af_display_ops_st *pstOps = af__disp_ops();
  const af_config_st *pstCfg = af_store_cfg();
  af_disp_info_st astOnline[AF_DISP_MAX];
  af__disp_scale_job_st *pstJob = NULL;
  uint32 u32ScId = 0u;
  uint32 u32Cnt = 0u;
  uint32 i = 0u;

  if (af_scale_key_index(pcKey) < 0)
  {
    return DM_INVALID;
  }
  if (g_stDisp.bDraining || g_stDisp.bScaleBusy || g_stDisp.bVdCreateInflight
      || g_stDisp.bVdDestroyInflight)
  {
    return DM_BUSY;
  }
  af_disp_store_save_scale(pcSidecarName, pcKey);
  if (af_phys_guard_is_active(&g_stDisp.stGuard, pstOps->pfNowMs())
      || (0u == g_stDisp.u32VdDispId))
  {
    return DM_SUCCESS;
  }
  pstJob = (af__disp_scale_job_st *)dm_calloc_tag(1u, sizeof(*pstJob), AF_DISP_MEMTAG);
  if (NULL == pstJob)
  {
    return DM_ALLOC;
  }
  u32Cnt = pstOps->pfDispOnline(astOnline, AF_DISP_MAX);
  for (i = 0u; i < u32Cnt; i++)
  {
    if (AF_DISP_SIDECAR == af_disp_classify(DM_FALSE, astOnline[i].u32Vendor, astOnline[i].u32Model))
    {
      u32ScId = astOnline[i].u32Id;
      break;
    }
  }
  pstJob->stOps = *pstOps;
  af_disp_store_resolve_presets(pcSidecarName, pstJob->astP);
  snprintf(pstJob->acSidecarName, sizeof(pstJob->acSidecarName), "%s",
           (NULL != pcSidecarName) ? pcSidecarName : "");
  snprintf(pstJob->acKey, sizeof(pstJob->acKey), "%s", pcKey);
  pstJob->u32ExtraCnt = (pstCfg->u32ExtraCnt < 16u) ? pstCfg->u32ExtraCnt : 16u;
  if (pstJob->u32ExtraCnt > 0u)
  {
    memcpy(pstJob->aau32Extra, pstCfg->aau32Extra, pstJob->u32ExtraCnt * sizeof(pstJob->aau32Extra[0]));
  }
  pstJob->u32ScId = u32ScId;
  pstJob->bMirror = pstCfg->bMirror;
  pstJob->u32VdDispId = g_stDisp.u32VdDispId;
  pstJob->u64VdHandle = g_stDisp.u64VdHandle;
  pstJob->u32VdSerial = g_stDisp.u32VdSerial;
  g_stDisp.bScaleBusy = DM_TRUE;
  g_stDisp.acScaleError[0] = '\0';
  pstOps->pfRunOnMain(af__disp_main_scale_job, pstJob);
  return DM_SUCCESS;
}

void af_display_set_ipad_screen(in const char *pcSidecarName, in uint32 u32W, in uint32 u32H)
{
  uint32 u32Long = 0u;
  uint32 u32Short = 0u;
  af_state_st *pstS = af_store_state_mut();
  af_ipad_screen_entry_st *pstE = NULL;
  uint32 i = 0u;

  if ((NULL == pcSidecarName) || ('\0' == pcSidecarName[0])
      || (!af_scale_normalize_screen(u32W, u32H, &u32Long, &u32Short)))
  {
    return;
  }
  for (i = 0u; i < pstS->u32ScreenCnt; i++)
  {
    if (0 == strcmp(pstS->astScreens[i].acName, pcSidecarName))
    {
      pstE = &pstS->astScreens[i];
      break;
    }
  }
  if ((NULL == pstE) && (pstS->u32ScreenCnt < AF_IPAD_MAP_MAX))
  {
    pstE = &pstS->astScreens[pstS->u32ScreenCnt++];
    snprintf(pstE->acName, sizeof(pstE->acName), "%s", pcSidecarName);
  }
  if (NULL != pstE)
  {
    pstE->au32Px[0] = u32Long;
    pstE->au32Px[1] = u32Short;
    (void)af_store_save_async(NULL, NULL);
  }
}

void af_display_set_anti_unplug(in bool_t bEnable)
{
  af_store_state_mut()->bAntiUnplug = bEnable ? DM_TRUE : DM_FALSE;
  (void)af_store_save_async(NULL, NULL);
  af__disp_reconcile();
}

void af_display_hold(in uint64 u64UntilMs)
{
  g_stDisp.bHoldSet = DM_TRUE;
  g_stDisp.u64HoldUntilMs = u64UntilMs;
}

void af_display_reconcile_now(void)
{
  af__disp_reconcile();
}

const dm_module_st g_stAfDisplayMod = {
    .u16ModID = AF_MOD_DISPLAY,
    .pcName = "af_display",
    .pfInit = af__disp_init,
    .pfDrain = af__disp_drain,
    .pfFini = af__disp_fini,
    .pastErrMap = NULL
};

const dm_module_st *af_display_module(void)
{
  return &g_stAfDisplayMod;
}
