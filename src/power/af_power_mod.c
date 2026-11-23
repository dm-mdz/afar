/**
 ******************************************************************************
 * @file    af_power_mod.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   服务层 af_power（ModID 19）实现：Q8 保活、屏保原值先落盘再改 0、崩溃恢复与 pfDrain。
 ******************************************************************************
 */

#define DM_LOG_MOD "power"

#include "af_power.h"
#include "../af_modules.h"
#include "../plat/af_plat.h"

#include <dm.h>
#include <dm/event.h>
#include <dm/link.h>
#include <dm/log.h>

#include <stdint.h>
#include <string.h>

typedef struct af__power_ctx
{
  uint32 u32PhysCount;
  af_sc_state_et eScState;
  bool_t bHolding;
  bool_t bSsSaveInflight;
  bool_t bDraining;
  bool_t bSawDispEv;
} af__power_ctx_st;

static af__power_ctx_st g_stPwr;
static af_power_ops_st g_stPwrOps;
static bool_t g_bPwrOpsInited = DM_FALSE;

static void af__pwr_init_default_ops(out af_power_ops_st *pstOps)
{
  memset(pstOps, 0, sizeof(*pstOps));
  pstOps->pfPowerHold = af_plat_power_hold;
  pstOps->pfSsGet = af_plat_screensaver_get;
  pstOps->pfSsSet = af_plat_screensaver_set;
  pstOps->pfSsDelete = af_plat_screensaver_delete;
}

static const af_power_ops_st *af__pwr_ops(void)
{
  if (!g_bPwrOpsInited)
  {
    af__pwr_init_default_ops(&g_stPwrOps);
    g_bPwrOpsInited = DM_TRUE;
  }
  return &g_stPwrOps;
}

void af_power_set_ops(in const af_power_ops_st *pstOps)
{
  af__pwr_init_default_ops(&g_stPwrOps);
  g_bPwrOpsInited = DM_TRUE;
  if (NULL == pstOps)
  {
    return;
  }
  if (NULL != pstOps->pfPowerHold) { g_stPwrOps.pfPowerHold = pstOps->pfPowerHold; }
  if (NULL != pstOps->pfSsGet) { g_stPwrOps.pfSsGet = pstOps->pfSsGet; }
  if (NULL != pstOps->pfSsSet) { g_stPwrOps.pfSsSet = pstOps->pfSsSet; }
  if (NULL != pstOps->pfSsDelete) { g_stPwrOps.pfSsDelete = pstOps->pfSsDelete; }
}

bool_t af_power_should_hold(in uint32 u32PhysCount, in af_sc_state_et eScState)
{
  return ((0u == u32PhysCount) || (AF_SC_SHOWING == eScState)) ? DM_TRUE : DM_FALSE;
}

bool_t af_power_is_holding(void)
{
  return g_stPwr.bHolding;
}

static void af__pwr_apply_restore_hw(in const af_orig_ss_st *pstOrig)
{
  const af_power_ops_st *pstOps = af__pwr_ops();
  if (pstOrig->bKnown && (pstOrig->iSec > 0))
  {
    (void)pstOps->pfSsSet(pstOrig->iSec);
  }
  else
  {
    (void)pstOps->pfSsDelete();
  }
}

void af_power_restore_screensaver(io af_orig_ss_st *pstOrig, in bool_t bSyncSave,
                                  in const char *pcDataDir)
{
  if ((NULL == pstOrig) || (!pstOrig->bSaved))
  {
    return;
  }
  af__pwr_apply_restore_hw(pstOrig);
  pstOrig->bSaved = DM_FALSE;
  pstOrig->bKnown = DM_FALSE;
  pstOrig->iSec = 0;
  if (bSyncSave)
  {
    (void)af_store_save_sync(pcDataDir);
  }
  else
  {
    (void)af_store_save_async(NULL, NULL);
  }
}

static void af__pwr_on_ss_saved(in int iStatus, io void *pvCtx)
{
  const af_power_ops_st *pstOps = af__pwr_ops();
  (void)pvCtx;

  g_stPwr.bSsSaveInflight = DM_FALSE;
  if ((DM_SUCCESS == iStatus) && g_stPwr.bHolding && (!g_stPwr.bDraining))
  {
    (void)pstOps->pfSsSet(0);
  }
}

static void af__pwr_on_drain_saved(in int iStatus, io void *pvCtx)
{
  int iHold = (int)(uintptr_t)pvCtx;
  (void)iStatus;
  if (iHold > 0)
  {
    dm_drain_release(iHold);
  }
}

static void af__pwr_enter_hold(void)
{
  const af_power_ops_st *pstOps = af__pwr_ops();
  af_orig_ss_st *pstOrig = &af_store_state_mut()->stOrigSs;

  (void)pstOps->pfPowerHold(DM_TRUE);
  g_stPwr.bHolding = DM_TRUE;

  if (!pstOrig->bSaved)
  {
    int iSec = 0;
    bool_t bExists = DM_FALSE;
    int iRc = DM_SUCCESS;

    (void)pstOps->pfSsGet(&iSec, &bExists);
    pstOrig->bSaved = DM_TRUE;
    if (bExists && (iSec > 0))
    {
      pstOrig->bKnown = DM_TRUE;
      pstOrig->iSec = iSec;
    }
    else
    {
      pstOrig->bKnown = DM_FALSE;
      pstOrig->iSec = 0;
    }
    g_stPwr.bSsSaveInflight = DM_TRUE;
    iRc = af_store_save_async(af__pwr_on_ss_saved, NULL);
    if (DM_SUCCESS != iRc)
    {
      pstOrig->bSaved = DM_FALSE;
      pstOrig->bKnown = DM_FALSE;
      pstOrig->iSec = 0;
      g_stPwr.bSsSaveInflight = DM_FALSE;
      DM_LOG_WARN("screensaver orig save_async failed (%d), skipping set(0)", iRc);
    }
  }
  else if (!g_stPwr.bSsSaveInflight)
  {
    /* 原值已在盘上持久化（如崩溃重启场景），保留原值不覆盖，仅确保屏保为 0 */
    (void)pstOps->pfSsSet(0);
  }
}

static void af__pwr_leave_hold(void)
{
  const af_power_ops_st *pstOps = af__pwr_ops();
  af_orig_ss_st *pstOrig = &af_store_state_mut()->stOrigSs;

  if (g_stPwr.bHolding)
  {
    (void)pstOps->pfPowerHold(DM_FALSE);
    g_stPwr.bHolding = DM_FALSE;
  }
  if (pstOrig->bSaved)
  {
    af_power_restore_screensaver(pstOrig, DM_FALSE, NULL);
  }
}

static void af__pwr_eval(void)
{
  bool_t bNeed = DM_FALSE;

  if (g_stPwr.bDraining)
  {
    return;
  }
  bNeed = af_power_should_hold(g_stPwr.u32PhysCount, g_stPwr.eScState);
  if (bNeed && (!g_stPwr.bHolding))
  {
    af__pwr_enter_hold();
  }
  else if (!bNeed)
  {
    af__pwr_leave_hold();
  }
}

static int af__pwr_on_display_ev(void *pvArg)
{
  const dm_link_msg_st *pstMsg = (const dm_link_msg_st *)pvArg;
  af_ev_display_st stEv;

  if ((NULL == pstMsg) || (pstMsg->u32Len < sizeof(stEv)) || (NULL == pstMsg->pu8Data))
  {
    return DM_SUCCESS;
  }
  memcpy(&stEv, pstMsg->pu8Data, sizeof(stEv));
  g_stPwr.u32PhysCount = stEv.u32PhysCount;
  g_stPwr.bSawDispEv = DM_TRUE;
  af__pwr_eval();
  return DM_SUCCESS;
}

static int af__pwr_on_sidecar_ev(void *pvArg)
{
  const dm_link_msg_st *pstMsg = (const dm_link_msg_st *)pvArg;
  af_ev_sidecar_st stEv;

  if ((NULL == pstMsg) || (pstMsg->u32Len < sizeof(stEv)) || (NULL == pstMsg->pu8Data))
  {
    return DM_SUCCESS;
  }
  memcpy(&stEv, pstMsg->pu8Data, sizeof(stEv));
  g_stPwr.eScState = stEv.eCur;
  af__pwr_eval();
  return DM_SUCCESS;
}

static int af__pwr_init(void)
{
  int iRc = DM_SUCCESS;

  memset(&g_stPwr, 0, sizeof(g_stPwr));
  g_stPwr.u32PhysCount = 1u; /* 默认假设有实体屏，等待 AF_EV_DISPLAY_CHANGED 刷新 */
  g_stPwr.eScState = AF_SC_IDLE;

  iRc = dm_event_subscribe(AF_EV_DISPLAY_CHANGED, af__pwr_on_display_ev, NULL, NULL);
  if (DM_SUCCESS != iRc) { return iRc; }
  return dm_event_subscribe(AF_EV_SIDECAR_CHANGED, af__pwr_on_sidecar_ev, NULL, NULL);
}

static void af__pwr_drain(io dm_drain_st *pstDrain)
{
  const af_power_ops_st *pstOps = af__pwr_ops();
  af_orig_ss_st *pstOrig = &af_store_state_mut()->stOrigSs;

  g_stPwr.bDraining = DM_TRUE;
  if (g_stPwr.bHolding)
  {
    (void)pstOps->pfPowerHold(DM_FALSE);
    g_stPwr.bHolding = DM_FALSE;
  }
  if (pstOrig->bSaved)
  {
    int iHold = dm_drain_hold(pstDrain);
    af__pwr_apply_restore_hw(pstOrig);
    pstOrig->bSaved = DM_FALSE;
    pstOrig->bKnown = DM_FALSE;
    pstOrig->iSec = 0;
    if (iHold > 0)
    {
      int iRc = af_store_save_async(af__pwr_on_drain_saved, (void *)(uintptr_t)iHold);
      if (DM_SUCCESS != iRc)
      {
        dm_drain_release(iHold);
      }
    }
    else
    {
      (void)af_store_save_async(NULL, NULL);
    }
  }
}

static void af__pwr_fini(void)
{
  (void)dm_event_unsubscribe(AF_EV_SIDECAR_CHANGED, af__pwr_on_sidecar_ev);
  (void)dm_event_unsubscribe(AF_EV_DISPLAY_CHANGED, af__pwr_on_display_ev);
  memset(&g_stPwr, 0, sizeof(g_stPwr));
}

const dm_module_st g_stAfPowerMod = {
    .u16ModID = AF_MOD_POWER,
    .pcName = "af_power",
    .pfInit = af__pwr_init,
    .pfDrain = af__pwr_drain,
    .pfFini = af__pwr_fini,
    .pastErrMap = NULL
};

const dm_module_st *af_power_module(void)
{
  return &g_stAfPowerMod;
}
