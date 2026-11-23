/**
 ******************************************************************************
 * @file    af_display_vd.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   af_display 核心状态机：want_vd 4 态机、8s 实体屏保护/恢复、desk 退让、切档回退与随航校准。
 ******************************************************************************
 */

#include "af_display.h"
#include "../af_store.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ============================ 1. want_vd 4 态机与 VdPlan ============================ */

af_vd_state_et af_vd_eval_state(in uint32 u32PhysCount, in bool_t bSidecarShowing,
                                in bool_t bSidecarStable2s, in bool_t bAntiUnplug,
                                in bool_t bMirror, in bool_t bHoldActive, in bool_t bHasVd,
                                out bool_t *pbWantVd)
{
  af_vd_state_et eState = AF_VD_PHYSICAL_ONLY;
  bool_t bWant = DM_FALSE;

  if (0u == u32PhysCount)
  {
    eState = bSidecarShowing ? AF_VD_HEADLESS_IPAD : AF_VD_HEADLESS_DESK;
    bWant = DM_TRUE;
  }
  else if (bAntiUnplug && bMirror && (bSidecarStable2s || (bHoldActive && bHasVd)))
  {
    eState = AF_VD_PHYSICAL_BACKUP;
    bWant = DM_TRUE;
  }
  else
  {
    eState = AF_VD_PHYSICAL_ONLY;
    bWant = DM_FALSE;
  }
  if (NULL != pbWantVd)
  {
    *pbWantVd = bWant;
  }
  return eState;
}

af_vd_plan_act_et af_vd_plan_step(io af_vd_plan_st *pstPlan, in bool_t bWantVd, in bool_t bHasVd,
                                  in bool_t bHoldActive, in uint64 u64NowMs)
{
  if (NULL == pstPlan)
  {
    return AF_VD_PLAN_NONE;
  }
  if (bWantVd)
  {
    pstPlan->bDropPending = DM_FALSE;
    pstPlan->u64DropSinceMs = 0u;
    return bHasVd ? AF_VD_PLAN_KEEP : AF_VD_PLAN_CREATE;
  }
  if (!bHasVd)
  {
    pstPlan->bDropPending = DM_FALSE;
    pstPlan->u64DropSinceMs = 0u;
    return AF_VD_PLAN_NONE;
  }
  if (bHoldActive)
  {
    pstPlan->bDropPending = DM_FALSE;
    pstPlan->u64DropSinceMs = 0u;
    return AF_VD_PLAN_KEEP;
  }
  if (!pstPlan->bDropPending)
  {
    pstPlan->bDropPending = DM_TRUE;
    pstPlan->u64DropSinceMs = u64NowMs;
    return AF_VD_PLAN_KEEP;
  }
  if ((u64NowMs >= pstPlan->u64DropSinceMs)
      && ((u64NowMs - pstPlan->u64DropSinceMs) >= AF_VD_DROP_MS))
  {
    pstPlan->bDropPending = DM_FALSE;
    pstPlan->u64DropSinceMs = 0u;
    return AF_VD_PLAN_DESTROY;
  }
  return AF_VD_PLAN_KEEP;
}

/* ============================ 2. 8s 实体屏保护窗与分辨率恢复 ============================ */

void af_phys_guard_init(out af_phys_guard_st *pstG)
{
  if (NULL != pstG)
  {
    memset(pstG, 0, sizeof(*pstG));
  }
}

bool_t af_phys_guard_is_active(in const af_phys_guard_st *pstG, in uint64 u64NowMs)
{
  if ((NULL == pstG) || (!pstG->bGuardActive))
  {
    return DM_FALSE;
  }
  return (u64NowMs < pstG->u64GuardUntilMs) ? DM_TRUE : DM_FALSE;
}

static void af__phys_make_key(in const af_disp_info_st *pstInfo, out char *pcBuf, in size_t uCap)
{
  snprintf(pcBuf, uCap, "%u-%u-%u", pstInfo->u32Vendor, pstInfo->u32Model, pstInfo->u32Serial);
}

static af_phys_guard_item_st *af__phys_find_known(io af_phys_guard_st *pstG, in uint32 u32DispId)
{
  uint32 i = 0u;
  for (i = 0u; i < pstG->u32KnownCnt; i++)
  {
    if (pstG->astKnown[i].u32DispId == u32DispId)
    {
      return &pstG->astKnown[i];
    }
  }
  return NULL;
}

uint32 af_phys_guard_update(io af_phys_guard_st *pstG, in const af_disp_info_st *pastOnline,
                            in uint32 u32OnlineCnt, in uint32 u32OurVdId, in uint64 u64NowMs)
{
  af_phys_guard_item_st astNext[AF_DISP_MAX];
  uint32 u32NextCnt = 0u;
  bool_t bNewAppeared = DM_FALSE;
  uint32 i = 0u;

  if (NULL == pstG)
  {
    return 0u;
  }
  memset(astNext, 0, sizeof(astNext));
  for (i = 0u; (NULL != pastOnline) && (i < u32OnlineCnt); i++)
  {
    bool_t bIsOurs = (0u != u32OurVdId) && (pastOnline[i].u32Id == u32OurVdId);
    if (AF_DISP_PHYSICAL != af_disp_classify(bIsOurs, pastOnline[i].u32Vendor, pastOnline[i].u32Model))
    {
      continue;
    }
    if (u32NextCnt < AF_DISP_MAX)
    {
      af_phys_guard_item_st *pstOld = af__phys_find_known(pstG, pastOnline[i].u32Id);
      if (NULL != pstOld)
      {
        astNext[u32NextCnt] = *pstOld;
      }
      else
      {
        astNext[u32NextCnt].u32DispId = pastOnline[i].u32Id;
        af__phys_make_key(&pastOnline[i], astNext[u32NextCnt].acKey,
                          sizeof(astNext[u32NextCnt].acKey));
        bNewAppeared = DM_TRUE;
      }
      u32NextCnt++;
    }
  }
  memcpy(pstG->astKnown, astNext, sizeof(astNext));
  pstG->u32KnownCnt = u32NextCnt;

  if (0u == u32NextCnt)
  {
    /* 拔空全部实体屏立即清零保护窗，确保无头虚拟屏即刻创建 */
    pstG->bGuardActive = DM_FALSE;
    pstG->u64GuardUntilMs = 0u;
  }
  else if (bNewAppeared)
  {
    pstG->bGuardActive = DM_TRUE;
    pstG->u64GuardUntilMs = u64NowMs + AF_PHYS_GUARD_MS;
  }
  else if (pstG->bGuardActive && (u64NowMs >= pstG->u64GuardUntilMs))
  {
    pstG->bGuardActive = DM_FALSE;
  }
  return u32NextCnt;
}

static af_disp_mode_entry_st *af__store_find_disp_mode(in const char *pcKey)
{
  af_state_st *pstS = af_store_state_mut();
  uint32 i = 0u;

  for (i = 0u; i < pstS->u32DispModeCnt; i++)
  {
    if (0 == strcmp(pstS->astDispModes[i].acKey, pcKey))
    {
      return &pstS->astDispModes[i];
    }
  }
  return NULL;
}

static bool_t af__store_save_disp_mode(in const char *pcKey, in const af_disp_mode_st *pstMode)
{
  af_state_st *pstS = af_store_state_mut();
  af_disp_mode_entry_st *pstE = af__store_find_disp_mode(pcKey);

  if ((NULL == pstE) && (pstS->u32DispModeCnt < AF_DISP_MODES_SAVE))
  {
    pstE = &pstS->astDispModes[pstS->u32DispModeCnt++];
    snprintf(pstE->acKey, sizeof(pstE->acKey), "%s", pcKey);
  }
  if (NULL != pstE)
  {
    pstE->au32Mode[0] = pstMode->u32W;
    pstE->au32Mode[1] = pstMode->u32H;
    pstE->au32Mode[2] = pstMode->u32Pw;
    pstE->au32Mode[3] = pstMode->u32Ph;
    pstE->au32Mode[4] = pstMode->u32Hz100;
    return DM_TRUE;
  }
  return DM_FALSE;
}

static bool_t af__phys_recover_one(io af_phys_guard_item_st *pstItem, in const af_disp_info_st *pstD,
                                   in uint64 u64NowMs, in const af_display_ops_st *pstOps)
{
  af_disp_mode_entry_st *pstSaved = af__store_find_disp_mode(pstItem->acKey);
  af_disp_mode_st stWant;

  if (!pstD->bHasCur || (0u == pstD->stCur.u32W) || (0u == pstD->stCur.u32H))
  {
    return DM_FALSE;
  }
  if (NULL == pstSaved)
  {
    return af__store_save_disp_mode(pstItem->acKey, &pstD->stCur);
  }
  if ((pstD->stCur.u32W == pstSaved->au32Mode[0]) && (pstD->stCur.u32H == pstSaved->au32Mode[1])
      && (pstD->stCur.u32Pw == pstSaved->au32Mode[2])
      && (pstD->stCur.u32Ph == pstSaved->au32Mode[3]))
  {
    pstItem->u32Retries = 0u;
    pstItem->bHasRetry = DM_FALSE;
    return DM_FALSE;
  }
  if (pstItem->u32Retries >= AF_PHYS_RETRY_MAX)
  {
    return DM_FALSE;
  }
  if (pstItem->bHasRetry && (u64NowMs >= pstItem->u64LastRetryMs)
      && ((u64NowMs - pstItem->u64LastRetryMs) < AF_PHYS_RETRY_MS))
  {
    return DM_FALSE;
  }
  memset(&stWant, 0, sizeof(stWant));
  stWant.u32W = pstSaved->au32Mode[0];
  stWant.u32H = pstSaved->au32Mode[1];
  stWant.u32Pw = pstSaved->au32Mode[2];
  stWant.u32Ph = pstSaved->au32Mode[3];
  stWant.u32Hz100 = pstSaved->au32Mode[4];
  stWant.bUsable = DM_TRUE;

  pstItem->u32Retries++;
  pstItem->bHasRetry = DM_TRUE;
  pstItem->u64LastRetryMs = u64NowMs;
  if (DM_SUCCESS != pstOps->pfDispSetMode(pstD->u32Id, &stWant, DM_FALSE))
  {
    (void)pstOps->pfDispSetMode(pstD->u32Id, &stWant, DM_TRUE);
  }
  return DM_FALSE;
}

bool_t af_phys_guard_recover(io af_phys_guard_st *pstG, in const af_disp_info_st *pastOnline,
                             in uint32 u32OnlineCnt, in uint32 u32OurVdId, in uint64 u64NowMs,
                             in const af_display_ops_st *pstOps)
{
  bool_t bDirty = DM_FALSE;
  uint32 i = 0u;

  if ((NULL == pstG) || (NULL == pastOnline) || (NULL == pstOps)
      || (NULL == pstOps->pfDispSetMode))
  {
    return DM_FALSE;
  }
  if (af_phys_guard_is_active(pstG, u64NowMs))
  {
    return DM_FALSE;
  }
  for (i = 0u; i < u32OnlineCnt; i++)
  {
    bool_t bIsOurs = (0u != u32OurVdId) && (pastOnline[i].u32Id == u32OurVdId);
    af_phys_guard_item_st *pstItem = NULL;
    if (AF_DISP_PHYSICAL
        != af_disp_classify(bIsOurs, pastOnline[i].u32Vendor, pastOnline[i].u32Model))
    {
      continue;
    }
    pstItem = af__phys_find_known(pstG, pastOnline[i].u32Id);
    if ((NULL != pstItem) && af__phys_recover_one(pstItem, &pastOnline[i], u64NowMs, pstOps))
    {
      bDirty = DM_TRUE;
    }
  }
  return bDirty;
}

/* ============================ 3. desk 远程桌面清晰度保护 ============================ */

void af_desk_init(out af_desk_st *pstD)
{
  if (NULL != pstD)
  {
    memset(pstD, 0, sizeof(*pstD));
  }
}

void af_desk_load_saved(io af_desk_st *pstD, in uint32 u32W, in uint32 u32H, in uint32 u32Pw,
                        in uint32 u32Ph)
{
  if ((NULL == pstD) || (0u == u32W) || (0u == u32H) || (u32Pw <= u32W))
  {
    return;
  }
  pstD->bHasSaved = DM_TRUE;
  pstD->u32SavedW = u32W;
  pstD->u32SavedH = u32H;
  pstD->u32SavedPw = u32Pw;
  pstD->u32SavedPh = u32Ph;
}

void af_desk_on_ipad(io af_desk_st *pstD)
{
  if (NULL == pstD)
  {
    return;
  }
  pstD->bNeedRestore = DM_FALSE;
  pstD->u32RestoreTries = 0u;
  pstD->bYieldLow = DM_FALSE;
  pstD->u32LowCnt = 0u;
}

void af_desk_on_enter_desk(io af_desk_st *pstD)
{
  if (NULL == pstD)
  {
    return;
  }
  pstD->bNeedRestore = pstD->bHasSaved;
  pstD->u32RestoreTries = 0u;
  pstD->bYieldLow = DM_FALSE;
  pstD->u32LowCnt = 0u;
}

void af_desk_on_vd_rebuilt(io af_desk_st *pstD)
{
  if (NULL == pstD)
  {
    return;
  }
  pstD->bNeedRestore = pstD->bHasSaved;
  pstD->u32RestoreTries = 0u;
}

void af_desk_reset(io af_desk_st *pstD)
{
  if (NULL == pstD)
  {
    return;
  }
  pstD->bYieldLow = DM_FALSE;
  pstD->u32LowCnt = 0u;
  pstD->u32RestoreTries = 0u;
}

static void af__desk_fill_saved(in const af_desk_st *pstD, out af_disp_mode_st *pstOut)
{
  if (NULL != pstOut)
  {
    memset(pstOut, 0, sizeof(*pstOut));
    pstOut->u32W = pstD->u32SavedW;
    pstOut->u32H = pstD->u32SavedH;
    pstOut->u32Pw = pstD->u32SavedPw;
    pstOut->u32Ph = pstD->u32SavedPh;
    pstOut->bUsable = DM_TRUE;
  }
}

static void af__desk_prune_low(io af_desk_st *pstD, in uint64 u64NowMs)
{
  uint32 u32Keep = 0u;
  uint32 i = 0u;

  for (i = 0u; i < pstD->u32LowCnt; i++)
  {
    if ((u64NowMs >= pstD->au64LowTimes[i])
        && ((u64NowMs - pstD->au64LowTimes[i]) < AF_DESK_LOW_WIN_MS))
    {
      pstD->au64LowTimes[u32Keep++] = pstD->au64LowTimes[i];
    }
  }
  pstD->u32LowCnt = u32Keep;
}

static af_desk_act_et af__desk_handle_low(io af_desk_st *pstD, in uint64 u64NowMs,
                                          out af_disp_mode_st *pstRestoreOut)
{
  if (!pstD->bHasSaved)
  {
    return AF_DESK_NONE;
  }
  if (pstD->bYieldLow)
  {
    return AF_DESK_NONE;
  }
  af__desk_prune_low(pstD, u64NowMs);
  if (pstD->u32LowCnt >= AF_DESK_LOW_MAX)
  {
    pstD->bYieldLow = DM_TRUE;
    pstD->bNeedRestore = DM_FALSE;
    return AF_DESK_YIELD_LOW;
  }
  pstD->au64LowTimes[pstD->u32LowCnt++] = u64NowMs;
  af__desk_fill_saved(pstD, pstRestoreOut);
  return AF_DESK_RESTORE;
}

af_desk_act_et af_desk_step(io af_desk_st *pstD, in const af_disp_mode_st *pstCur,
                            in uint64 u64NowMs, out af_disp_mode_st *pstRestoreOut)
{
  bool_t bSharp = DM_FALSE;
  bool_t bSame = DM_FALSE;

  if ((NULL == pstD) || (NULL == pstCur) || (0u == pstCur->u32W) || (0u == pstCur->u32H))
  {
    return AF_DESK_NONE;
  }
  bSharp = (pstCur->u32Pw > pstCur->u32W) && (pstCur->u32Ph > pstCur->u32H);
  bSame = pstD->bHasSaved && (pstCur->u32W == pstD->u32SavedW)
          && (pstCur->u32H == pstD->u32SavedH) && (pstCur->u32Pw == pstD->u32SavedPw)
          && (pstCur->u32Ph == pstD->u32SavedPh);
  if (bSame)
  {
    pstD->bNeedRestore = DM_FALSE;
    pstD->u32RestoreTries = 0u;
    pstD->bYieldLow = DM_FALSE;
    return AF_DESK_NONE;
  }
  if (!bSharp)
  {
    return af__desk_handle_low(pstD, u64NowMs, pstRestoreOut);
  }
  if (pstD->bNeedRestore && pstD->bHasSaved)
  {
    if (pstD->u32RestoreTries < AF_DESK_TRIES)
    {
      pstD->u32RestoreTries++;
      af__desk_fill_saved(pstD, pstRestoreOut);
      return AF_DESK_RESTORE;
    }
    pstD->bNeedRestore = DM_FALSE;
    pstD->u32RestoreTries = 0u;
  }
  pstD->bHasSaved = DM_TRUE;
  pstD->u32SavedW = pstCur->u32W;
  pstD->u32SavedH = pstCur->u32H;
  pstD->u32SavedPw = pstCur->u32Pw;
  pstD->u32SavedPh = pstCur->u32Ph;
  pstD->bYieldLow = DM_FALSE;
  pstD->u32LowCnt = 0u;
  return AF_DESK_REMEMBER;
}

/* ============================ 4. from_sidecar 校准与 follow ============================ */

static bool_t af__sc_mode_valid(in const af_disp_mode_st *pstM, in double dBaseRatio,
                                in uint32 u32DefW)
{
  double dRatio = 0.0;
  double dMinW = (double)u32DefW * 0.65;
  double dMaxW = (double)u32DefW * 1.35;

  if ((!pstM->bUsable) || (0u == pstM->u32W) || (0u == pstM->u32H) || (pstM->u32Pw <= pstM->u32W))
  {
    return DM_FALSE;
  }
  dRatio = (double)pstM->u32H / (double)pstM->u32W;
  if (fabs(dRatio - dBaseRatio) > 0.015)
  {
    return DM_FALSE;
  }
  if (((double)pstM->u32W < dMinW) || ((double)pstM->u32W > dMaxW))
  {
    return DM_FALSE;
  }
  return DM_TRUE;
}

bool_t af_disp_from_sidecar(in uint32 u32PxW, in uint32 u32PxH, in const af_disp_mode_st *pastModes,
                            in uint32 u32ModeCnt, out af_scale_preset_st astOut[AF_SCALE_COUNT])
{
  af_scale_preset_st astBase[AF_SCALE_COUNT];
  uint32 u32A = 2360u;
  uint32 u32B = 1640u;
  double dRatio = 0.0;
  uint32 u32Matched = 0u;
  uint32 k = 0u;

  if (NULL == astOut)
  {
    return DM_FALSE;
  }
  if (!af_scale_normalize_screen(u32PxW, u32PxH, &u32A, &u32B))
  {
    u32A = 2360u;
    u32B = 1640u;
  }
  af_scale_presets(u32A, u32B, astBase);
  memcpy(astOut, astBase, sizeof(astBase));
  if ((NULL == pastModes) || (0u == u32ModeCnt))
  {
    return DM_FALSE;
  }
  dRatio = (double)u32B / (double)u32A;
  for (k = 0u; k < AF_SCALE_COUNT; k++)
  {
    double dBestDiff = 1e9;
    double dMaxTierDiff = (double)astBase[k].u32W * 0.15;
    uint32 u32BestIdx = u32ModeCnt;
    uint32 i = 0u;
    for (i = 0u; i < u32ModeCnt; i++)
    {
      double dDiff = 0.0;
      if (!af__sc_mode_valid(&pastModes[i], dRatio, astBase[1].u32W))
      {
        continue;
      }
      dDiff = fabs((double)pastModes[i].u32W - (double)astBase[k].u32W);
      if ((dDiff <= dMaxTierDiff) && (dDiff < dBestDiff))
      {
        dBestDiff = dDiff;
        u32BestIdx = i;
      }
    }
    if (u32BestIdx < u32ModeCnt)
    {
      astOut[k].u32W = pastModes[u32BestIdx].u32W;
      astOut[k].u32H = pastModes[u32BestIdx].u32H;
      u32Matched++;
    }
  }
  if ((0u == u32Matched) || (astOut[0].u32W >= astOut[1].u32W)
      || (astOut[1].u32W >= astOut[2].u32W))
  {
    memcpy(astOut, astBase, sizeof(astBase));
    return DM_FALSE;
  }
  return DM_TRUE;
}

af_follow_res_et af_disp_follow_eval(in const af_disp_mode_st *pstCur, in bool_t bHasCur,
                                     in const af_disp_mode_st *pastModes, in uint32 u32ModeCnt,
                                     in const af_scale_preset_st astPresets[AF_SCALE_COUNT],
                                     in const char *pcWantKey, out af_disp_mode_st *pstTargetOut)
{
  int iIdx = af_scale_key_index(pcWantKey);
  uint32 u32Want = (iIdx >= 0) ? (uint32)iIdx : 1u;
  uint32 u32W = 0u;
  uint32 u32H = 0u;
  uint32 i = 0u;

  if (NULL == astPresets)
  {
    return AF_FOLLOW_MISSING;
  }
  u32W = astPresets[u32Want].u32W;
  u32H = astPresets[u32Want].u32H;
  if (bHasCur && (NULL != pstCur) && (pstCur->u32W == u32W) && (pstCur->u32H == u32H)
      && (pstCur->u32Pw > pstCur->u32W))
  {
    return AF_FOLLOW_OK;
  }
  for (i = 0u; (NULL != pastModes) && (i < u32ModeCnt); i++)
  {
    if (pastModes[i].bUsable && (pastModes[i].u32W == u32W) && (pastModes[i].u32H == u32H)
        && (pastModes[i].u32Pw > pastModes[i].u32W))
    {
      if (NULL != pstTargetOut)
      {
        *pstTargetOut = pastModes[i];
      }
      return AF_FOLLOW_SWITCH;
    }
  }
  return AF_FOLLOW_MISSING;
}

/* ============================ 5. 切档 3 级回退链 ============================ */

static int af__scale_hot_rebuild(in const af_display_ops_st *pstOps,
                                 in const af_scale_preset_st astPresets[AF_SCALE_COUNT],
                                 in const char *pcWantKey, in const uint32 aau32Extra[][2],
                                 in uint32 u32ExtraCnt, in uint32 u32SidecarDispId,
                                 in bool_t bMirror, io uint32 *pu32VdDispId,
                                 io uint64 *pu64VdHandle, io uint32 *pu32VdSerial)
{
  uint32 u32NewSerial = (1u == *pu32VdSerial) ? 2u : 1u;
  uint32 u32Style = 0u;

  if ((NULL == pstOps->pfVdCreate) || (NULL == pstOps->pfVdDestroy))
  {
    return DM_NOT_FOUND;
  }
  for (u32Style = 0u; u32Style < 2u; u32Style++)
  {
    af_vd_mode_req_st astModes[AF_VD_MODES_MAX];
    uint32 u32MaxPw = 0u;
    uint32 u32MaxPh = 0u;
    uint32 u32NewDisp = 0u;
    uint64 u64NewHandle = 0u;
    uint32 u32Cnt = af_vd_build_modes(astPresets, pcWantKey, aau32Extra, u32ExtraCnt, u32Style,
                                      astModes, AF_VD_MODES_MAX, &u32MaxPw, &u32MaxPh);
    if ((u32Cnt > 0u)
        && (DM_SUCCESS
            == pstOps->pfVdCreate(u32NewSerial, u32MaxPw, u32MaxPh, astModes, u32Cnt, &u32NewDisp,
                                  &u64NewHandle)))
    {
      uint64 u64OldHandle = *pu64VdHandle;
      if (bMirror && (0u != u32SidecarDispId) && (NULL != pstOps->pfDispMirror))
      {
        (void)pstOps->pfDispMirror(u32SidecarDispId, u32NewDisp);
      }
      if (0u != u64OldHandle)
      {
        pstOps->pfVdDestroy(u64OldHandle);
      }
      *pu32VdDispId = u32NewDisp;
      *pu64VdHandle = u64NewHandle;
      *pu32VdSerial = u32NewSerial;
      return DM_SUCCESS;
    }
  }
  return DM_FAILD;
}

int af_disp_scale_fallback_exec(in const af_display_ops_st *pstOps,
                                in const af_scale_preset_st astPresets[AF_SCALE_COUNT],
                                in const char *pcWantKey, in const uint32 aau32Extra[][2],
                                in uint32 u32ExtraCnt, in uint32 u32SidecarDispId,
                                in bool_t bMirror, io uint32 *pu32VdDispId,
                                io uint64 *pu64VdHandle, io uint32 *pu32VdSerial,
                                out char *pcErrOut, in size_t uErrCap)
{
  int iIdx = af_scale_key_index(pcWantKey);
  af_disp_mode_st stWant;

  if ((NULL == pstOps) || (NULL == astPresets) || (iIdx < 0) || (NULL == pu32VdDispId)
      || (NULL == pu64VdHandle) || (NULL == pu32VdSerial))
  {
    return DM_INVALID;
  }
  if ((NULL != pcErrOut) && (uErrCap > 0u))
  {
    pcErrOut[0] = '\0';
  }
  memset(&stWant, 0, sizeof(stWant));
  stWant.u32W = astPresets[iIdx].u32W;
  stWant.u32H = astPresets[iIdx].u32H;
  stWant.u32Pw = stWant.u32W * 2u;
  stWant.u32Ph = stWant.u32H * 2u;
  stWant.bUsable = DM_TRUE;

  /* ① 直接切模 */
  if ((NULL != pstOps->pfDispSetMode)
      && (DM_SUCCESS == pstOps->pfDispSetMode(*pu32VdDispId, &stWant, DM_FALSE)))
  {
    return DM_SUCCESS;
  }
  /* ② 原子解镜像 + HiDPI 切模后再重镜像 */
  if ((NULL != pstOps->pfDispSetHidpiUnmirrored)
      && (DM_SUCCESS == pstOps->pfDispSetHidpiUnmirrored(*pu32VdDispId, stWant.u32W, stWant.u32H)))
  {
    if (bMirror && (0u != u32SidecarDispId) && (NULL != pstOps->pfDispMirror))
    {
      (void)pstOps->pfDispMirror(u32SidecarDispId, *pu32VdDispId);
    }
    return DM_SUCCESS;
  }
  /* ③ 序列号 1 <-> 2 热重建（先写法 0 再写法 1） */
  if (DM_SUCCESS
      == af__scale_hot_rebuild(pstOps, astPresets, pcWantKey, aau32Extra, u32ExtraCnt,
                               u32SidecarDispId, bMirror, pu32VdDispId, pu64VdHandle,
                               pu32VdSerial))
  {
    return DM_SUCCESS;
  }
  /* ④ 全部失败：尽力回退 default 并记录错误 */
  if (NULL != pstOps->pfDispSetMode)
  {
    stWant.u32W = astPresets[1].u32W;
    stWant.u32H = astPresets[1].u32H;
    stWant.u32Pw = stWant.u32W * 2u;
    stWant.u32Ph = stWant.u32H * 2u;
    (void)pstOps->pfDispSetMode(*pu32VdDispId, &stWant, DM_FALSE);
  }
  if ((NULL != pcErrOut) && (uErrCap > 0u))
  {
    snprintf(pcErrOut, uErrCap, "切换显示大小失败，已回退默认");
  }
  return DM_FAILD;
}
