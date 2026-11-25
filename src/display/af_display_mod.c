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

#define AF_DISP_MEMTAG            DM_MOD_MEMTAG(AF_MOD_DISPLAY, 0u)
#define AF_DISP_TIMER_TAG         0xD1590001ULL
#define AF_FOLLOW_ESC_COOLDOWN_MS 5000ULL

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
  int (*pfVdCreate)(dm_in uint32, dm_in uint32, dm_in uint32, dm_in const af_vd_mode_req_st *, dm_in uint32,
                    dm_out uint32 *, dm_out uint64 *);
  uint32 u32Serial;
  uint32 u32MaxPw;
  uint32 u32MaxPh;
  uint32 u32Count;
  af_vd_mode_req_st astModes[AF_VD_MODES_MAX];
} af__disp_create_job_st;

typedef struct af__disp_destroy_job
{
  void (*pfVdDestroy)(dm_in uint64);
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
  bool_t bDirectStreaming;
  char acDirectKey[65];
  af_vd_ready_cb pfDirectCb;
  void *pvDirectCtx;
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
  bool_t bHasScDisp;
  uint32 u32VdW;
  uint32 u32VdH;
  uint64 u64FollowEscMs;
} af__disp_ctx_st;

static af__disp_ctx_st g_stDisp;
static af_display_ops_st g_stDispOps;
static bool_t g_bDispOpsInited = DM_FALSE;
static uint32 g_u32DispTickMs = 1000u;

static uint64 af__disp_default_now_ms(void) { return dm_hrtime() / 1000000ULL; }

static void af__disp_init_default_ops(dm_out af_display_ops_st *pstOps)
{
  static const af_display_ops_st stDef = {
      af_plat_run_on_main,    af_plat_disp_online,       af_plat_disp_modes,
      af_plat_disp_set_mode,  af_plat_disp_set_hidpi_unmirrored, af_plat_disp_mirror,
      af_plat_disp_watch_start, af_plat_disp_watch_stop, af_plat_vd_create,
      af_plat_vd_apply,       af_plat_vd_destroy,        af_plat_vd_destroy_all,
      af__disp_default_now_ms
  };
  *pstOps = stDef;
}

static const af_display_ops_st *af__disp_ops(void)
{
  if (!g_bDispOpsInited) { af__disp_init_default_ops(&g_stDispOps); g_bDispOpsInited = DM_TRUE; }
  return &g_stDispOps;
}

#define AF_DISP_SET_OP(f) if (NULL != pstOps->f) { g_stDispOps.f = pstOps->f; }

void af_display_set_ops(dm_in const af_display_ops_st *pstOps)
{
  af__disp_init_default_ops(&g_stDispOps);
  g_bDispOpsInited = DM_TRUE;
  if (NULL == pstOps) { return; }
  AF_DISP_SET_OP(pfRunOnMain); AF_DISP_SET_OP(pfDispOnline); AF_DISP_SET_OP(pfDispModes);
  AF_DISP_SET_OP(pfDispSetMode); AF_DISP_SET_OP(pfDispSetHidpiUnmirrored); AF_DISP_SET_OP(pfDispMirror);
  AF_DISP_SET_OP(pfDispWatchStart); AF_DISP_SET_OP(pfDispWatchStop); AF_DISP_SET_OP(pfVdCreate);
  AF_DISP_SET_OP(pfVdApply); AF_DISP_SET_OP(pfVdDestroy); AF_DISP_SET_OP(pfVdDestroyAll);
  AF_DISP_SET_OP(pfNowMs);
}

void af_display_set_tick_ms(dm_in uint32 u32TickMs) { g_u32DispTickMs = (u32TickMs > 0u) ? u32TickMs : 1000u; }

static const char *af__disp_active_key(void)
{
  if (g_stDisp.bDirectStreaming && ('\0' != g_stDisp.acDirectKey[0])) { return g_stDisp.acDirectKey; }
  return g_stDisp.bSidecarShowing ? g_stDisp.acShowingName : "";
}

static void af__disp_release_drain(void)
{
  if (g_stDisp.iDrainHold > 0)
  {
    int iH = g_stDisp.iDrainHold;
    g_stDisp.iDrainHold = 0;
    dm_drain_release(iH);
  }
}

static void af__disp_calc_enc_size(dm_in const char *pcKey, dm_out uint16 *pu16W, dm_out uint16 *pu16H)
{
  af_scale_preset_st astP[AF_SCALE_COUNT];
  int iIdx = af_scale_key_index(af_disp_store_target_scale(pcKey));
  uint32 u32Idx = (iIdx >= 0) ? (uint32)iIdx : 1u;
  uint32 u32Pw = 0u;
  uint32 u32Ph = 0u;

  af_disp_store_resolve_presets(pcKey, astP);
  u32Pw = astP[u32Idx].u32W * 2u;
  u32Ph = astP[u32Idx].u32H * 2u;
  *pu16W = (u32Pw > 65535u) ? 65535u : (uint16)u32Pw;
  *pu16H = (u32Ph > 65535u) ? 65535u : (uint16)u32Ph;
}

static void af__disp_fire_direct_cb(dm_in int iErr)
{
  af_vd_ready_cb pfCb = g_stDisp.pfDirectCb;
  void *pvCtx = g_stDisp.pvDirectCtx;
  uint16 u16W = 0u;
  uint16 u16H = 0u;

  if (NULL == pfCb) { return; }
  g_stDisp.pfDirectCb = NULL;
  g_stDisp.pvDirectCtx = NULL;
  if (DM_SUCCESS == iErr) { af__disp_calc_enc_size(g_stDisp.acDirectKey, &u16W, &u16H); }
  else if (DM_CANCELED != iErr) { g_stDisp.bDirectStreaming = DM_FALSE; g_stDisp.acDirectKey[0] = '\0'; }
  pfCb(iErr, (DM_SUCCESS == iErr) ? g_stDisp.u32VdDispId : 0u, u16W, u16H, pvCtx);
}

/* ============================ 主线程异步虚屏建/拆任务 ============================ */

static void af__disp_main_create_job(dm_io void *pvCtx)
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

static void af__disp_main_destroy_job(dm_io void *pvCtx)
{
  af__disp_destroy_job_st *pstJob = (af__disp_destroy_job_st *)pvCtx;
  af__disp_main_done_st stDone;

  memset(&stDone, 0, sizeof(stDone));
  stDone.eOp = AF_DISP_OP_DESTROY; stDone.iStatus = DM_SUCCESS; stDone.u64Handle = pstJob->u64Handle;
  if ((0u != pstJob->u64Handle) && (NULL != pstJob->pfVdDestroy)) { pstJob->pfVdDestroy(pstJob->u64Handle); }
  if (pstJob->bDestroyAll && (NULL != pstJob->pfVdDestroyAll)) { pstJob->pfVdDestroyAll(); }
  dm_free_tag(pstJob);
  (void)dm_queue_post_async(AF_LINK_DISP_MAIN_DONE, &stDone, (uint32)sizeof(stDone));
}

static void af__disp_main_scale_job(dm_io void *pvCtx)
{
  af__disp_scale_job_st *pstJob = (af__disp_scale_job_st *)pvCtx;
  af__disp_main_done_st stDone;

  memset(&stDone, 0, sizeof(stDone));
  stDone.eOp = AF_DISP_OP_SCALE;
  snprintf(stDone.acKey, sizeof(stDone.acKey), "%s", pstJob->acKey);
  snprintf(stDone.acSidecarName, sizeof(stDone.acSidecarName), "%s", pstJob->acSidecarName);
  stDone.u32DispId = pstJob->u32VdDispId; stDone.u64Handle = pstJob->u64VdHandle; stDone.u32Serial = pstJob->u32VdSerial;
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
  const char *pcName = af__disp_active_key();
  const char *pcWant = af_disp_store_target_scale(pcName);

  if (g_stDisp.bVdCreateInflight || g_stDisp.bVdDestroyInflight || g_stDisp.bScaleBusy || g_stDisp.bDraining) { return; }
  pstJob = (af__disp_create_job_st *)dm_calloc_tag(1u, sizeof(*pstJob), AF_DISP_MEMTAG);
  if (NULL == pstJob) { af__disp_fire_direct_cb(DM_ALLOC); return; }
  af_disp_store_resolve_presets(pcName, astP);
  pstJob->pfVdCreate = pstOps->pfVdCreate;
  pstJob->u32Serial = (1u == g_stDisp.u32VdSerial) ? 2u : 1u;
  pstJob->u32Count = af_vd_build_modes(astP, pcWant, pstCfg->aau32Extra, pstCfg->u32ExtraCnt, 0u,
                                       pstJob->astModes, AF_VD_MODES_MAX, &pstJob->u32MaxPw, &pstJob->u32MaxPh);
  if (0u == pstJob->u32Count) { dm_free_tag(pstJob); af__disp_fire_direct_cb(DM_FAILD); return; }
  g_stDisp.bVdCreateInflight = DM_TRUE;
  pstOps->pfRunOnMain(af__disp_main_create_job, pstJob);
}

static void af__disp_start_destroy(dm_in uint64 u64Handle, dm_in bool_t bDestroyAll)
{
  const af_display_ops_st *pstOps = af__disp_ops();
  af__disp_destroy_job_st *pstJob = NULL;

  if (g_stDisp.bVdDestroyInflight || g_stDisp.bScaleBusy) { return; }
  pstJob = (af__disp_destroy_job_st *)dm_calloc_tag(1u, sizeof(*pstJob), AF_DISP_MEMTAG);
  if (NULL == pstJob) { af__disp_release_drain(); return; }
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
  if (g_stDisp.bDraining) { return; }
  memset(&stEv, 0, sizeof(stEv));
  stEv.u32PhysCount = g_stDisp.u32PhysCount;
  stEv.eVdState = g_stDisp.eVdState;
  stEv.bHasVd = (0u != g_stDisp.u64VdHandle) ? DM_TRUE : DM_FALSE;
  stEv.u32VdDispId = g_stDisp.u32VdDispId;
  stEv.bHasScDisp = g_stDisp.bHasScDisp;
  stEv.u32VdW = g_stDisp.u32VdW;
  stEv.u32VdH = g_stDisp.u32VdH;
  (void)dm_event_publish(AF_EV_DISPLAY_CHANGED, &stEv, (uint32)sizeof(stEv));
  g_stDisp.bEvPublishedOnce = DM_TRUE;
}

static void af__disp_sync_actual_scale(dm_in const af_disp_info_st *pstVdInfo, dm_in const char *pcName)
{
  af_scale_preset_st astP[AF_SCALE_COUNT];
  uint32 i = 0u;

  g_stDisp.acActualScale[0] = '\0';
  if ((NULL == pstVdInfo) || (!pstVdInfo->bHasCur)) { return; }
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

static void af__disp_follow_vd(dm_in const af_disp_info_st *pstVdInfo, dm_in const char *pcKey)
{
  const af_display_ops_st *pstOps = af__disp_ops();
  af_disp_mode_st astVdModes[AF_DISP_MODES_MAX];
  af_scale_preset_st astP[AF_SCALE_COUNT];
  af_disp_mode_st stTarget;
  af_follow_res_et eEval = AF_FOLLOW_OK;
  uint32 u32MCnt = 0u;
  const char *pcWant = NULL;
  int iSetRet = DM_SUCCESS;

  if ((0u == g_stDisp.u32VdDispId) || (NULL == pstVdInfo) || (NULL == pcKey) || ('\0' == pcKey[0])) { return; }
  u32MCnt = pstOps->pfDispModes(g_stDisp.u32VdDispId, astVdModes, AF_DISP_MODES_MAX);
  pcWant = af_disp_store_target_scale(pcKey);
  af_disp_store_resolve_presets(pcKey, astP);
  eEval = af_disp_follow_eval(&pstVdInfo->stCur, pstVdInfo->bHasCur, astVdModes, u32MCnt, astP,
                              pcWant, &stTarget);
  if (AF_FOLLOW_OK == eEval) { g_stDisp.u64FollowEscMs = 0u; return; }
  if (AF_FOLLOW_SWITCH == eEval)
  {
    iSetRet = pstOps->pfDispSetMode(g_stDisp.u32VdDispId, &stTarget, DM_TRUE);
    if (DM_SUCCESS == iSetRet)
    {
      g_stDisp.u32VdW = stTarget.u32W; g_stDisp.u32VdH = stTarget.u32H;
      g_stDisp.u64FollowEscMs = 0u;
      af__disp_publish_changed();
      return;
    }
  }
  if (pstOps->pfNowMs() >= g_stDisp.u64FollowEscMs)
  {
    g_stDisp.u64FollowEscMs = pstOps->pfNowMs() + AF_FOLLOW_ESC_COOLDOWN_MS;
    DM_LOG_WARN("follow escalate eval=%d rc=%d vd=%u want=%s", (int)eEval, iSetRet, g_stDisp.u32VdDispId, pcWant);
    (void)af_display_set_scale(pcKey, pcWant);
  }
}

static void af__disp_handle_ipad_vd(dm_in const af_disp_info_st *pstVdInfo,
                                    dm_in const af_disp_info_st *pstScInfo)
{
  const af_display_ops_st *pstOps = af__disp_ops();
  const af_config_st *pstCfg = af_store_cfg();

  if (0u == g_stDisp.u32VdDispId) { return; }
  if (NULL != pstScInfo)
  {
    if (pstCfg->bMirror && (pstScInfo->u32MirrorOf != g_stDisp.u32VdDispId))
    {
      int iMir = pstOps->pfDispMirror(pstScInfo->u32Id, g_stDisp.u32VdDispId);
      if (DM_SUCCESS != iMir) { DM_LOG_WARN("sidecar mirror(%u->%u) failed rc=%d", pstScInfo->u32Id, g_stDisp.u32VdDispId, iMir); }
    }
    if (g_stDisp.bSidecarShowing && ('\0' != g_stDisp.acShowingName[0])
        && ((!g_stDisp.bCalibrated)
            || (0 != strcmp(g_stDisp.acCalibratedName, g_stDisp.acShowingName))))
    {
      af_disp_mode_st astScModes[AF_DISP_MODES_MAX];
      af_scale_preset_st astCal[AF_SCALE_COUNT];
      uint32 u32PxW = 0u, u32PxH = 0u;
      uint32 u32Cnt = pstOps->pfDispModes(pstScInfo->u32Id, astScModes, AF_DISP_MODES_MAX);
      af_disp_store_ipad_pixels(g_stDisp.acShowingName, &u32PxW, &u32PxH);
      if (af_disp_from_sidecar(u32PxW, u32PxH, astScModes, u32Cnt, astCal)) { af_disp_store_save_sizes(g_stDisp.acShowingName, astCal); }
      g_stDisp.bCalibrated = DM_TRUE;
      snprintf(g_stDisp.acCalibratedName, sizeof(g_stDisp.acCalibratedName), "%s", g_stDisp.acShowingName);
    }
  }
  if ((NULL != pstScInfo) && g_stDisp.bSidecarShowing) { af__disp_follow_vd(pstVdInfo, g_stDisp.acShowingName); }
}

static void af__disp_handle_desk_and_ipad(dm_in const af_disp_info_st *pstVdInfo,
                                          dm_in const af_disp_info_st *pstScInfo,
                                          dm_in bool_t bHold, dm_in uint64 u64Now)
{
  const af_display_ops_st *pstOps = af__disp_ops();

  if (AF_VD_HEADLESS_DESK == g_stDisp.eVdState)
  {
    af_disp_mode_st stRestore;
    if (bHold || (NULL != pstScInfo) || ((NULL != pstVdInfo) && (0u != pstVdInfo->u32MirrorOf))) { return; }
    if ((NULL != pstVdInfo) && pstVdInfo->bHasCur)
    {
      af_desk_act_et eAct = af_desk_step(&g_stDisp.stDesk, &pstVdInfo->stCur, u64Now, &stRestore);
      if (AF_DESK_REMEMBER == eAct)
      {
        af_state_st *pstS = af_store_state_mut();
        pstS->bHasDeskMode = DM_TRUE;
        pstS->au32DeskMode[0] = pstVdInfo->stCur.u32W; pstS->au32DeskMode[1] = pstVdInfo->stCur.u32H;
        pstS->au32DeskMode[2] = pstVdInfo->stCur.u32Pw; pstS->au32DeskMode[3] = pstVdInfo->stCur.u32Ph;
        (void)af_store_save_async(NULL, NULL);
      }
      else if (AF_DESK_RESTORE == eAct) { (void)pstOps->pfDispSetMode(g_stDisp.u32VdDispId, &stRestore, DM_TRUE); }
    }
    return;
  }
  if (AF_VD_DIRECT_STREAM == g_stDisp.eVdState) { af__disp_follow_vd(pstVdInfo, g_stDisp.acDirectKey); return; }
  af__disp_handle_ipad_vd(pstVdInfo, pstScInfo);
}

static void af__disp_reconcile(void)
{
  const af_display_ops_st *pstOps = af__disp_ops();
  const af_config_st *pstCfg = af_store_cfg();
  const af_state_st *pstSt = af_store_state();
  af_disp_info_st astOnline[AF_DISP_MAX];
  af_vd_eval_in_st stIn;
  uint32 u32OnlineCnt = 0u, u32PrevPhys = g_stDisp.u32PhysCount, u32PrevVdId = g_stDisp.u32VdDispId;
  uint32 u32PrevW = g_stDisp.u32VdW, u32PrevH = g_stDisp.u32VdH, i = 0u;
  uint64 u64Now = pstOps->pfNowMs();
  af_vd_state_et ePrevState = g_stDisp.eVdState;
  bool_t bPrevScDisp = g_stDisp.bHasScDisp, bWantVd = DM_FALSE, bGuard = DM_FALSE;
  bool_t bHasVd = (0u != g_stDisp.u64VdHandle) ? DM_TRUE : DM_FALSE;
  bool_t bHold = g_stDisp.bHoldSet && (u64Now < g_stDisp.u64HoldUntilMs);
  bool_t bStable2s = g_stDisp.bSidecarShowing && g_stDisp.bHasSidecarSince
                     && (u64Now >= g_stDisp.u64SidecarSinceMs)
                     && ((u64Now - g_stDisp.u64SidecarSinceMs) >= AF_VD_STABLE_MS);
  const af_disp_info_st *pstVdInfo = NULL, *pstScInfo = NULL;

  if (g_stDisp.bDraining) { return; }
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
  g_stDisp.bHasScDisp = (NULL != pstScInfo) ? DM_TRUE : DM_FALSE;
  if ((NULL != pstVdInfo) && pstVdInfo->bHasCur) { g_stDisp.u32VdW = pstVdInfo->stCur.u32W; g_stDisp.u32VdH = pstVdInfo->stCur.u32H; }
  else if (0u == g_stDisp.u32VdDispId) { g_stDisp.u32VdW = 0u; g_stDisp.u32VdH = 0u; }
  af__disp_sync_actual_scale(pstVdInfo, af__disp_active_key());
  stIn = (af_vd_eval_in_st){g_stDisp.u32PhysCount, g_stDisp.bSidecarShowing, bStable2s,
                            pstSt->bAntiUnplug,    pstCfg->bMirror,          bHold,
                            bHasVd,                g_stDisp.bDirectStreaming};
  g_stDisp.eVdState = af_vd_eval_state_ex(&stIn, &bWantVd);
  if ((!g_stDisp.bEvPublishedOnce) || (u32PrevPhys != g_stDisp.u32PhysCount)
      || (ePrevState != g_stDisp.eVdState) || (u32PrevVdId != g_stDisp.u32VdDispId)
      || (bPrevScDisp != g_stDisp.bHasScDisp) || (u32PrevW != g_stDisp.u32VdW)
      || (u32PrevH != g_stDisp.u32VdH))
  {
    if ((AF_VD_HEADLESS_DESK == g_stDisp.eVdState) && (AF_VD_HEADLESS_DESK != ePrevState)) { af_desk_on_enter_desk(&g_stDisp.stDesk); }
    else if ((AF_VD_HEADLESS_IPAD == g_stDisp.eVdState) || (AF_VD_DIRECT_STREAM == g_stDisp.eVdState)) { af_desk_on_ipad(&g_stDisp.stDesk); }
    af__disp_publish_changed();
  }
  bGuard = af_phys_guard_is_active(&g_stDisp.stGuard, u64Now);
  if (bGuard && (!g_stDisp.bDirectStreaming)) { return; }
  if ((!bGuard) && af_phys_guard_recover(&g_stDisp.stGuard, astOnline, u32OnlineCnt,
                                         g_stDisp.u32VdDispId, u64Now, pstOps))
  {
    (void)af_store_save_async(NULL, NULL);
  }
  switch (af_vd_plan_step(&g_stDisp.stPlan, bWantVd, bHasVd, bHold, u64Now))
  {
    case AF_VD_PLAN_CREATE: af__disp_start_create(); break;
    case AF_VD_PLAN_DESTROY: af__disp_start_destroy(g_stDisp.u64VdHandle, DM_FALSE); break;
    case AF_VD_PLAN_KEEP:
      if (!g_stDisp.bScaleBusy) { af__disp_handle_desk_and_ipad(pstVdInfo, pstScInfo, bHold, u64Now); }
      break;
    default: break;
  }
}

/* ============================ DM 回调与生命周期 ============================ */

static void af__disp_done_create(dm_in const af__disp_main_done_st *pstDone)
{
  g_stDisp.bVdCreateInflight = DM_FALSE;
  if (DM_SUCCESS == pstDone->iStatus)
  {
    g_stDisp.u32VdSerial = pstDone->u32Serial; g_stDisp.u32VdDispId = pstDone->u32DispId; g_stDisp.u64VdHandle = pstDone->u64Handle;
    af_desk_on_vd_rebuilt(&g_stDisp.stDesk);
  }
  if (g_stDisp.bDraining) { af__disp_start_destroy(g_stDisp.u64VdHandle, DM_TRUE); return; }
  af__disp_publish_changed();
  if (NULL != g_stDisp.pfDirectCb)
  {
    if ((DM_SUCCESS == pstDone->iStatus) && (0u != g_stDisp.u32VdDispId)) { af__disp_fire_direct_cb(DM_SUCCESS); }
    else { g_stDisp.bDirectStreaming = DM_FALSE; g_stDisp.acDirectKey[0] = '\0'; af__disp_fire_direct_cb(DM_FAILD); }
  }
}

static void af__disp_done_destroy(void)
{
  g_stDisp.bVdDestroyInflight = DM_FALSE;
  g_stDisp.u32VdDispId = 0u; g_stDisp.u64VdHandle = 0u; g_stDisp.u32VdW = 0u; g_stDisp.u32VdH = 0u;
  if (g_stDisp.bDraining) { af__disp_release_drain(); return; }
  af__disp_publish_changed();
  if (g_stDisp.bDirectStreaming && (NULL != g_stDisp.pfDirectCb)) { af__disp_start_create(); }
}

static void af__disp_done_scale(dm_in const af__disp_main_done_st *pstDone)
{
  g_stDisp.bScaleBusy = DM_FALSE;
  g_stDisp.u32VdDispId = pstDone->u32DispId;
  g_stDisp.u64VdHandle = pstDone->u64Handle;
  g_stDisp.u32VdSerial = pstDone->u32Serial;
  snprintf(g_stDisp.acScaleError, sizeof(g_stDisp.acScaleError), "%s", pstDone->acErr);
  if (DM_SUCCESS == pstDone->iStatus)
  {
    af_scale_preset_st astP[AF_SCALE_COUNT];
    int iIdx = af_scale_key_index(pstDone->acKey);
    snprintf(g_stDisp.acActualScale, sizeof(g_stDisp.acActualScale), "%s", pstDone->acKey);
    af_disp_store_resolve_presets(pstDone->acSidecarName, astP);
    if ((iIdx >= 0) && (iIdx < (int)AF_SCALE_COUNT)) { g_stDisp.u32VdW = astP[iIdx].u32W; g_stDisp.u32VdH = astP[iIdx].u32H; }
    else { g_stDisp.u32VdW = 0u; g_stDisp.u32VdH = 0u; }
  }
  else { af_disp_store_save_scale(pstDone->acSidecarName, "default"); g_stDisp.u32VdW = 0u; g_stDisp.u32VdH = 0u; }
  if (g_stDisp.bDraining)
  {
    if (0u != g_stDisp.u64VdHandle) { af__disp_start_destroy(g_stDisp.u64VdHandle, DM_TRUE); }
    else { af__disp_release_drain(); }
    return;
  }
  af__disp_publish_changed();
  if (g_stDisp.bDirectStreaming && (NULL != g_stDisp.pfDirectCb))
  {
    if ((0u != g_stDisp.u32VdDispId) && (0u != g_stDisp.u64VdHandle)) { af__disp_fire_direct_cb((DM_SUCCESS == pstDone->iStatus) ? DM_SUCCESS : pstDone->iStatus); }
    else { af__disp_start_create(); }
  }
}

static int af__disp_on_main_done(void *pvArg)
{
  const dm_link_msg_st *pstMsg = (const dm_link_msg_st *)pvArg;
  af__disp_main_done_st stDone;

  if ((NULL == pstMsg) || (pstMsg->u32Len < sizeof(stDone)) || (NULL == pstMsg->pu8Data)) { return DM_SUCCESS; }
  memcpy(&stDone, pstMsg->pu8Data, sizeof(stDone));
  if (AF_DISP_OP_CREATE == stDone.eOp) { af__disp_done_create(&stDone); }
  else if (AF_DISP_OP_DESTROY == stDone.eOp) { af__disp_done_destroy(); }
  else if (AF_DISP_OP_SCALE == stDone.eOp) { af__disp_done_scale(&stDone); }
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

  if ((NULL == pstMsg) || (pstMsg->u32Len < sizeof(stEv)) || (NULL == pstMsg->pu8Data)) { return DM_SUCCESS; }
  memcpy(&stEv, pstMsg->pu8Data, sizeof(stEv));
  if (AF_SC_SHOWING == stEv.eCur)
  {
    if (!g_stDisp.bSidecarShowing) { g_stDisp.bHasSidecarSince = DM_TRUE; g_stDisp.u64SidecarSinceMs = u64Now; }
    g_stDisp.bSidecarShowing = DM_TRUE;
    snprintf(g_stDisp.acShowingName, sizeof(g_stDisp.acShowingName), "%s", stEv.acShowing);
  }
  else
  {
    g_stDisp.bSidecarShowing = DM_FALSE; g_stDisp.bHasSidecarSince = DM_FALSE;
    g_stDisp.u64SidecarSinceMs = 0u; g_stDisp.acShowingName[0] = '\0'; g_stDisp.u64FollowEscMs = 0u;
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

static void af__disp_drain(dm_io dm_drain_st *pstDrain)
{
  const af_display_ops_st *pstOps = af__disp_ops();

  g_stDisp.bDraining = DM_TRUE;
  af__disp_fire_direct_cb(DM_SHUTTING_DOWN);
  if (0u != g_stDisp.u32TimerId) { (void)dm_timer_cancel(g_stDisp.u32TimerId); g_stDisp.u32TimerId = 0u; }
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

void af_display_get_snap(dm_in const char *pcSidecarName, dm_out af_disp_snap_st *pstOut)
{
  if (NULL == pstOut) { return; }
  memset(pstOut, 0, sizeof(*pstOut));
  pstOut->eVdState = g_stDisp.eVdState;
  pstOut->u32PhysCount = g_stDisp.u32PhysCount;
  snprintf(pstOut->acActualScale, sizeof(pstOut->acActualScale), "%s", g_stDisp.acActualScale);
  af_disp_store_resolve_presets(pcSidecarName, pstOut->astScales);
  pstOut->bScaleBusy = g_stDisp.bScaleBusy;
  snprintf(pstOut->acScaleError, sizeof(pstOut->acScaleError), "%s", g_stDisp.acScaleError);
}

int af_display_set_scale(dm_in const char *pcSidecarName, dm_in const char *pcKey)
{
  const af_display_ops_st *pstOps = af__disp_ops();
  const af_config_st *pstCfg = af_store_cfg();
  af_disp_info_st astOnline[AF_DISP_MAX];
  af__disp_scale_job_st *pstJob = NULL;
  uint32 u32ScId = 0u;
  uint32 u32Cnt = 0u;
  uint32 i = 0u;

  if (af_scale_key_index(pcKey) < 0) { return DM_INVALID; }
  if (g_stDisp.bDraining || g_stDisp.bScaleBusy || g_stDisp.bVdCreateInflight
      || g_stDisp.bVdDestroyInflight) { return DM_BUSY; }
  af_disp_store_save_scale(pcSidecarName, pcKey);
  if (((!g_stDisp.bDirectStreaming) && (!g_stDisp.bSidecarShowing))
      || ((!g_stDisp.bDirectStreaming) && af_phys_guard_is_active(&g_stDisp.stGuard, pstOps->pfNowMs()))
      || (0u == g_stDisp.u32VdDispId)) { return DM_SUCCESS; }
  pstJob = (af__disp_scale_job_st *)dm_calloc_tag(1u, sizeof(*pstJob), AF_DISP_MEMTAG);
  if (NULL == pstJob) { return DM_ALLOC; }
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
  snprintf(pstJob->acSidecarName, sizeof(pstJob->acSidecarName), "%s", (NULL != pcSidecarName) ? pcSidecarName : "");
  snprintf(pstJob->acKey, sizeof(pstJob->acKey), "%s", pcKey);
  pstJob->u32ExtraCnt = (pstCfg->u32ExtraCnt < 16u) ? pstCfg->u32ExtraCnt : 16u;
  if (pstJob->u32ExtraCnt > 0u) { memcpy(pstJob->aau32Extra, pstCfg->aau32Extra, pstJob->u32ExtraCnt * sizeof(pstJob->aau32Extra[0])); }
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

void af_display_set_ipad_screen(dm_in const char *pcSidecarName, dm_in uint32 u32W, dm_in uint32 u32H)
{
  uint32 u32Long = 0u;
  uint32 u32Short = 0u;
  af_state_st *pstS = af_store_state_mut();
  af_ipad_screen_entry_st *pstE = NULL;
  uint32 i = 0u;

  if ((NULL == pcSidecarName) || ('\0' == pcSidecarName[0])
      || (!af_scale_normalize_screen(u32W, u32H, &u32Long, &u32Short))) { return; }
  for (i = 0u; i < pstS->u32ScreenCnt; i++)
  {
    if (0 == strcmp(pstS->astScreens[i].acName, pcSidecarName)) { pstE = &pstS->astScreens[i]; break; }
  }
  if ((NULL == pstE) && (pstS->u32ScreenCnt < AF_IPAD_MAP_MAX))
  {
    pstE = &pstS->astScreens[pstS->u32ScreenCnt++];
    snprintf(pstE->acName, sizeof(pstE->acName), "%s", pcSidecarName);
  }
  if (NULL != pstE) { pstE->au32Px[0] = u32Long; pstE->au32Px[1] = u32Short; (void)af_store_save_async(NULL, NULL); }
}

int af_display_ensure_direct_vd(dm_in const af_direct_vd_req_st *pstReq,
                                dm_in af_vd_ready_cb pfCb, dm_io void *pvCtx)
{
  if ((NULL == pstReq) || (NULL == pstReq->pcKeyId) || ('\0' == pstReq->pcKeyId[0]) || (NULL == pfCb)) { return DM_INVALID; }
  if (g_stDisp.bDraining) { return DM_SHUTTING_DOWN; }
  if (NULL != g_stDisp.pfDirectCb) { af__disp_fire_direct_cb(DM_CANCELED); }
  g_stDisp.bDirectStreaming = DM_TRUE;
  snprintf(g_stDisp.acDirectKey, sizeof(g_stDisp.acDirectKey), "%s", pstReq->pcKeyId);
  if ((pstReq->u32PxW > 0u) && (pstReq->u32PxH > 0u)) { af_display_set_ipad_screen(g_stDisp.acDirectKey, pstReq->u32PxW, pstReq->u32PxH); }
  if ((NULL != pstReq->pcScale) && ('\0' != pstReq->pcScale[0]) && (af_scale_key_index(pstReq->pcScale) >= 0))
  {
    af_disp_store_save_scale(g_stDisp.acDirectKey, pstReq->pcScale);
  }
  g_stDisp.pfDirectCb = pfCb;
  g_stDisp.pvDirectCtx = pvCtx;
  af__disp_reconcile();
  if ((0u != g_stDisp.u32VdDispId) && (0u != g_stDisp.u64VdHandle) && (!g_stDisp.bVdCreateInflight)
      && (!g_stDisp.bVdDestroyInflight) && (!g_stDisp.bScaleBusy))
  {
    const char *pcWant = af_disp_store_target_scale(g_stDisp.acDirectKey);
    if (('\0' != g_stDisp.acActualScale[0]) && (0 == strcmp(g_stDisp.acActualScale, pcWant)))
    {
      af__disp_fire_direct_cb(DM_SUCCESS);
    }
    else
    {
      int iScaleRc = af_display_set_scale(g_stDisp.acDirectKey, pcWant);
      if (DM_SUCCESS != iScaleRc) { af__disp_fire_direct_cb(iScaleRc); }
      else if (!g_stDisp.bScaleBusy) { af__disp_fire_direct_cb(DM_SUCCESS); }
    }
  }
  return DM_SUCCESS;
}

void af_display_direct_stop(void)
{
  g_stDisp.bDirectStreaming = DM_FALSE;
  g_stDisp.acDirectKey[0] = '\0';
  g_stDisp.u64FollowEscMs = 0u;
  if (NULL != g_stDisp.pfDirectCb) { af__disp_fire_direct_cb(DM_CANCELED); }
  af__disp_reconcile();
}

uint32 af_display_get_vd_id(void) { return g_stDisp.u32VdDispId; }
bool_t af_display_has_sidecar_disp(void) { return g_stDisp.bHasScDisp; }
void af_display_set_anti_unplug(dm_in bool_t bEnable) { af_store_state_mut()->bAntiUnplug = bEnable ? DM_TRUE : DM_FALSE; (void)af_store_save_async(NULL, NULL); af__disp_reconcile(); }
void af_display_hold(dm_in uint64 u64UntilMs) { g_stDisp.bHoldSet = (0u != u64UntilMs) ? DM_TRUE : DM_FALSE; g_stDisp.u64HoldUntilMs = u64UntilMs; }
void af_display_reconcile_now(void) { af__disp_reconcile(); }

const dm_module_st g_stAfDisplayMod = {.u16ModID = AF_MOD_DISPLAY, .pcName = "af_display",
                                       .pfInit = af__disp_init,    .pfDrain = af__disp_drain,
                                       .pfFini = af__disp_fini,    .pastErrMap = NULL};

const dm_module_st *af_display_module(void) { return &g_stAfDisplayMod; }
