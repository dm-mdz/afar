/**
 ******************************************************************************
 * @file    af_sidecar_mod.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   服务层 af_sidecar（ModID 18）实现：Keep 补试、10s 宽限/冷却、错误节流与单飞轮询。
 ******************************************************************************
 */

#define DM_LOG_MOD "sidecar"

#include "af_sidecar.h"
#include "../af_modules.h"

#include <dm.h>
#include <dm/event.h>
#include <dm/link.h>
#include <dm/log.h>
#include <dm/timer.h>

#include <stdio.h>
#include <string.h>

#define AF_SC_MEMTAG DM_MOD_MEMTAG(AF_MOD_SIDECAR, 0u)
#define AF_SC_TIMER_TAG 0x51DEC401ULL

typedef enum af__sc_main_op
{
  AF_SC_OP_POLL = 1,
  AF_SC_OP_CONNECT = 2,
  AF_SC_OP_DISCONNECT = 3
} af__sc_main_op_et;

typedef struct af__sc_main_done
{
  af__sc_main_op_et eOp;
  int iStatus;
  uint32 u32Cnt;
  af_sc_device_st astDevs[AF_SC_DEV_MAX];
  char acErr[128];
} af__sc_main_done_st;

typedef struct af__sc_job
{
  af__sc_main_op_et eOp;
  uint32 (*pfScList)(dm_out af_sc_device_st *, dm_in uint32);
  int (*pfScConnect)(dm_in const char *, dm_out char *, dm_in size_t);
  int (*pfScDisconnect)(dm_in const char *, dm_out char *, dm_in size_t);
  char acName[65];
} af__sc_job_st;

typedef struct af__sc_ctx
{
  af_sc_state_et eState;
  char acShowing[65];
  uint32 u32NearbyCnt;
  char aacNearby[AF_SC_DEV_MAX][65];
  bool_t bPolledOnce;
  bool_t bPolling;
  bool_t bHasConnectDeadline;
  uint64 u64ConnectUntilMs;
  bool_t bHasGrace;
  uint64 u64GraceUntilMs;
  bool_t bHasCooldown;
  uint64 u64CooldownUntilMs;
  af_sc_keep_st stKeep;
  af_sc_err_throttle_st stErrThrottle;
  af_sc_diag_st stDiag;
  uint32 u32TimerId;
  bool_t bDraining;
} af__sc_ctx_st;

static af__sc_ctx_st g_stSc;
static af_sidecar_ops_st g_stScOps;
static bool_t g_bScOpsInited = DM_FALSE;
static uint32 g_u32ScPollMs = 1000u;

static uint64 af__sc_default_now_ms(void)
{
  return dm_hrtime() / 1000000ULL;
}

static void af__sc_init_default_ops(dm_out af_sidecar_ops_st *pstOps)
{
  memset(pstOps, 0, sizeof(*pstOps));
  pstOps->pfRunOnMain = af_plat_run_on_main;
  pstOps->pfScList = af_plat_sc_list;
  pstOps->pfScConnect = af_plat_sc_connect;
  pstOps->pfScDisconnect = af_plat_sc_disconnect;
  pstOps->pfNowMs = af__sc_default_now_ms;
}

static const af_sidecar_ops_st *af__sc_ops(void)
{
  if (!g_bScOpsInited)
  {
    af__sc_init_default_ops(&g_stScOps);
    g_bScOpsInited = DM_TRUE;
  }
  return &g_stScOps;
}

void af_sidecar_set_ops(dm_in const af_sidecar_ops_st *pstOps)
{
  af__sc_init_default_ops(&g_stScOps);
  g_bScOpsInited = DM_TRUE;
  if (NULL == pstOps)
  {
    return;
  }
  if (NULL != pstOps->pfRunOnMain)
  {
    g_stScOps.pfRunOnMain = pstOps->pfRunOnMain;
  }
  if (NULL != pstOps->pfScList)
  {
    g_stScOps.pfScList = pstOps->pfScList;
  }
  if (NULL != pstOps->pfScConnect)
  {
    g_stScOps.pfScConnect = pstOps->pfScConnect;
  }
  if (NULL != pstOps->pfScDisconnect)
  {
    g_stScOps.pfScDisconnect = pstOps->pfScDisconnect;
  }
  if (NULL != pstOps->pfNowMs)
  {
    g_stScOps.pfNowMs = pstOps->pfNowMs;
  }
}

void af_sidecar_set_poll_ms(dm_in uint32 u32PollMs)
{
  g_u32ScPollMs = (u32PollMs > 0u) ? u32PollMs : 1000u;
}

/* ============================ 1. 纯 C Keep 与错误节流 ============================ */

void af_sc_keep_want(dm_out af_sc_keep_st *pstK, dm_in const char *pcName, dm_in uint64 u64NowMs)
{
  if (NULL == pstK)
  {
    return;
  }
  memset(pstK, 0, sizeof(*pstK));
  if ((NULL == pcName) || ('\0' == pcName[0]))
  {
    return;
  }
  pstK->bPending = DM_TRUE;
  snprintf(pstK->acTarget, sizeof(pstK->acTarget), "%s", pcName);
  pstK->u64NextRetryMs = u64NowMs + AF_SC_RETRY_GAP_MS;
  pstK->u32RetriesLeft = AF_SC_RETRY_MAX;
}

void af_sc_keep_once(dm_out af_sc_keep_st *pstK, dm_in const char *pcName)
{
  (void)pcName;
  if (NULL != pstK)
  {
    memset(pstK, 0, sizeof(*pstK));
  }
}

void af_sc_keep_on_connected(dm_io af_sc_keep_st *pstK)
{
  if (NULL != pstK)
  {
    memset(pstK, 0, sizeof(*pstK));
  }
}

void af_sc_keep_clear(dm_io af_sc_keep_st *pstK)
{
  if (NULL != pstK)
  {
    memset(pstK, 0, sizeof(*pstK));
  }
}

bool_t af_sc_keep_due(dm_io af_sc_keep_st *pstK, dm_in uint64 u64NowMs, dm_out char *pcTargetOut,
                      dm_in size_t uCap)
{
  if ((NULL == pstK) || (!pstK->bPending) || (0u == pstK->u32RetriesLeft) ||
      (u64NowMs < pstK->u64NextRetryMs))
  {
    return DM_FALSE;
  }
  if ((NULL != pcTargetOut) && (uCap > 0u))
  {
    snprintf(pcTargetOut, uCap, "%s", pstK->acTarget);
  }
  pstK->u32RetriesLeft--;
  if (pstK->u32RetriesLeft > 0u)
  {
    pstK->u64NextRetryMs = u64NowMs + AF_SC_RETRY_GAP_MS;
  }
  else
  {
    pstK->bPending = DM_FALSE;
    pstK->u64NextRetryMs = 0u;
  }
  return DM_TRUE;
}

void af_sc_err_throttle_init(dm_out af_sc_err_throttle_st *pstT)
{
  if (NULL != pstT)
  {
    memset(pstT, 0, sizeof(*pstT));
  }
}

bool_t af_sc_err_throttle_check(dm_io af_sc_err_throttle_st *pstT, dm_in const char *pcErr,
                                dm_in uint64 u64NowMs, dm_out uint32 *pu32SummarySuppressed)
{
  const char *pcMsg = (NULL != pcErr) ? pcErr : "";

  if (NULL != pu32SummarySuppressed)
  {
    *pu32SummarySuppressed = 0u;
  }
  if ((NULL == pstT) || ('\0' == pcMsg[0]))
  {
    return DM_FALSE;
  }
  if (pstT->bActive && (0 == strcmp(pstT->acLastErr, pcMsg)) && (u64NowMs >= pstT->u64WinStartMs) &&
      ((u64NowMs - pstT->u64WinStartMs) < AF_SC_ERR_THROTTLE_MS))
  {
    pstT->u32Suppressed++;
    pstT->u32TotalSuppressed++;
    return DM_FALSE;
  }
  if (pstT->bActive && (NULL != pu32SummarySuppressed))
  {
    *pu32SummarySuppressed = pstT->u32Suppressed;
  }
  pstT->bActive = DM_TRUE;
  snprintf(pstT->acLastErr, sizeof(pstT->acLastErr), "%s", pcMsg);
  pstT->u64WinStartMs = u64NowMs;
  pstT->u32Suppressed = 0u;
  pstT->u32TotalEmitted++;
  return DM_TRUE;
}

/* ============================ 2. 主线程异步作业与事件发布 ============================ */

static void af__sc_main_job_run(dm_io void *pvCtx)
{
  af__sc_job_st *pstJob = (af__sc_job_st *)pvCtx;
  af__sc_main_done_st stDone;

  memset(&stDone, 0, sizeof(stDone));
  stDone.eOp = pstJob->eOp;
  if (AF_SC_OP_POLL == pstJob->eOp)
  {
    stDone.u32Cnt = pstJob->pfScList(stDone.astDevs, AF_SC_DEV_MAX);
    stDone.iStatus = DM_SUCCESS;
  }
  else if (AF_SC_OP_CONNECT == pstJob->eOp)
  {
    stDone.iStatus = pstJob->pfScConnect(pstJob->acName, stDone.acErr, sizeof(stDone.acErr));
  }
  else if (AF_SC_OP_DISCONNECT == pstJob->eOp)
  {
    const char *pcTarget = ('\0' != pstJob->acName[0]) ? pstJob->acName : NULL;
    stDone.iStatus = pstJob->pfScDisconnect(pcTarget, stDone.acErr, sizeof(stDone.acErr));
  }
  dm_free_tag(pstJob);
  (void)dm_queue_post_async(AF_LINK_SC_MAIN_DONE, &stDone, (uint32)sizeof(stDone));
}

static int af__sc_dispatch_job(dm_in af__sc_main_op_et eOp, dm_in const char *pcName)
{
  const af_sidecar_ops_st *pstOps = af__sc_ops();
  af__sc_job_st *pstJob = (af__sc_job_st *)dm_calloc_tag(1u, sizeof(*pstJob), AF_SC_MEMTAG);

  if (NULL == pstJob)
  {
    return DM_ALLOC;
  }
  pstJob->eOp = eOp;
  pstJob->pfScList = pstOps->pfScList;
  pstJob->pfScConnect = pstOps->pfScConnect;
  pstJob->pfScDisconnect = pstOps->pfScDisconnect;
  if (NULL != pcName)
  {
    snprintf(pstJob->acName, sizeof(pstJob->acName), "%s", pcName);
  }
  pstOps->pfRunOnMain(af__sc_main_job_run, pstJob);
  return DM_SUCCESS;
}

static void af__sc_publish(dm_in af_sc_state_et ePrev, dm_in bool_t bUserDisconnect,
                           dm_in bool_t bNewAppear, dm_in const char *pcAppeared)
{
  af_ev_sidecar_st stEv;
  if (g_stSc.bDraining)
  {
    return;
  }
  memset(&stEv, 0, sizeof(stEv));
  stEv.ePrev = ePrev;
  stEv.eCur = g_stSc.eState;
  snprintf(stEv.acShowing, sizeof(stEv.acShowing), "%s", g_stSc.acShowing);
  stEv.bUserDisconnect = bUserDisconnect;
  stEv.bNewAppear = bNewAppear;
  if (NULL != pcAppeared)
  {
    snprintf(stEv.acAppeared, sizeof(stEv.acAppeared), "%s", pcAppeared);
  }
  (void)dm_event_publish(AF_EV_SIDECAR_CHANGED, &stEv, (uint32)sizeof(stEv));
}

static void af__sc_log_err(dm_in const char *pcErr, dm_in uint64 u64Now)
{
  uint32 u32Sup = 0u;
  if (af_sc_err_throttle_check(&g_stSc.stErrThrottle, pcErr, u64Now, &u32Sup))
  {
    if (u32Sup > 0u)
    {
      DM_LOG_WARN("sidecar error (suppressed %u previous): %s", (unsigned)u32Sup, pcErr);
    }
    else
    {
      DM_LOG_WARN("sidecar error: %s", pcErr);
    }
  }
  g_stSc.stDiag.u32ErrEmitted = g_stSc.stErrThrottle.u32TotalEmitted;
  g_stSc.stDiag.u32ErrSuppressed = g_stSc.stErrThrottle.u32TotalSuppressed;
}

static bool_t af__sc_in_list(dm_in char aacList[AF_SC_DEV_MAX][65], dm_in uint32 u32Cnt,
                             dm_in const char *pcName)
{
  uint32 i = 0u;
  for (i = 0u; i < u32Cnt; i++)
  {
    if (0 == strcmp(aacList[i], pcName))
    {
      return DM_TRUE;
    }
  }
  return DM_FALSE;
}

static void af__sc_apply_poll(dm_in const af__sc_main_done_st *pstDone)
{
  uint64 u64Now = af__sc_ops()->pfNowMs();
  char aacPrev[AF_SC_DEV_MAX][65];
  uint32 u32PrevCnt = g_stSc.u32NearbyCnt;
  af_sc_state_et ePrevState = g_stSc.eState;
  char acPrevShowing[65];
  char acNewAppeared[65] = {0};
  const char *pcConnName = NULL;
  bool_t bInGrace = g_stSc.bHasGrace && (u64Now < g_stSc.u64GraceUntilMs);
  uint32 i = 0u;

  memcpy(aacPrev, g_stSc.aacNearby, sizeof(aacPrev));
  snprintf(acPrevShowing, sizeof(acPrevShowing), "%s", g_stSc.acShowing);
  g_stSc.u32NearbyCnt = 0u;
  for (i = 0u; (i < pstDone->u32Cnt) && (i < AF_SC_DEV_MAX); i++)
  {
    if ('\0' == pstDone->astDevs[i].acName[0])
    {
      continue;
    }
    snprintf(g_stSc.aacNearby[g_stSc.u32NearbyCnt], sizeof(g_stSc.aacNearby[0]), "%s",
             pstDone->astDevs[i].acName);
    if (g_stSc.bPolledOnce && ('\0' == acNewAppeared[0]) &&
        (!af__sc_in_list(aacPrev, u32PrevCnt, pstDone->astDevs[i].acName)))
    {
      snprintf(acNewAppeared, sizeof(acNewAppeared), "%s", pstDone->astDevs[i].acName);
    }
    g_stSc.u32NearbyCnt++;
    if (pstDone->astDevs[i].bConnected && (NULL == pcConnName))
    {
      pcConnName = pstDone->astDevs[i].acName;
    }
  }
  g_stSc.bPolledOnce = DM_TRUE;
  if ((NULL != pcConnName) && (!bInGrace))
  {
    g_stSc.eState = AF_SC_SHOWING;
    snprintf(g_stSc.acShowing, sizeof(g_stSc.acShowing), "%s", pcConnName);
    g_stSc.bHasConnectDeadline = DM_FALSE;
    af_sc_keep_on_connected(&g_stSc.stKeep);
  }
  else if (NULL == pcConnName)
  {
    g_stSc.bHasGrace = DM_FALSE;
    if (AF_SC_SHOWING == g_stSc.eState)
    {
      g_stSc.eState = AF_SC_IDLE;
      g_stSc.acShowing[0] = '\0';
      g_stSc.bHasCooldown = DM_TRUE;
      g_stSc.u64CooldownUntilMs = u64Now + AF_SC_COOLDOWN_MS;
    }
  }
  if ((ePrevState != g_stSc.eState) || (0 != strcmp(acPrevShowing, g_stSc.acShowing)) ||
      ('\0' != acNewAppeared[0]))
  {
    af__sc_publish(ePrevState, DM_FALSE, ('\0' != acNewAppeared[0]) ? DM_TRUE : DM_FALSE,
                   acNewAppeared);
  }
}

static void af__sc_on_timer_tick(void)
{
  uint64 u64Now = af__sc_ops()->pfNowMs();
  char acRetryTarget[65];

  if (g_stSc.bDraining)
  {
    return;
  }
  if (af_sc_keep_due(&g_stSc.stKeep, u64Now, acRetryTarget, sizeof(acRetryTarget)))
  {
    g_stSc.stDiag.u32RetryTriggered++;
    g_stSc.eState = AF_SC_CONNECTING;
    g_stSc.bHasConnectDeadline = DM_TRUE;
    g_stSc.u64ConnectUntilMs = u64Now + AF_SC_CONNECT_TIMEOUT_MS;
    (void)af__sc_dispatch_job(AF_SC_OP_CONNECT, acRetryTarget);
  }
  else if ((AF_SC_CONNECTING == g_stSc.eState) && g_stSc.bHasConnectDeadline &&
           (u64Now >= g_stSc.u64ConnectUntilMs) && (!g_stSc.stKeep.bPending))
  {
    af_sc_state_et ePrev = g_stSc.eState;
    g_stSc.eState = AF_SC_IDLE;
    g_stSc.bHasConnectDeadline = DM_FALSE;
    af__sc_publish(ePrev, DM_FALSE, DM_FALSE, NULL);
  }
  if (g_stSc.bPolling)
  {
    g_stSc.stDiag.u32PollSkipped++;
    return;
  }
  g_stSc.bPolling = DM_TRUE;
  g_stSc.stDiag.u32PollDispatched++;
  if (DM_SUCCESS != af__sc_dispatch_job(AF_SC_OP_POLL, NULL))
  {
    g_stSc.bPolling = DM_FALSE;
  }
}

static int af__sc_on_link(void *pvArg)
{
  const dm_link_msg_st *pstMsg = (const dm_link_msg_st *)pvArg;
  af__sc_main_done_st stDone;

  if (NULL == pstMsg)
  {
    return DM_SUCCESS;
  }
  if (DM_SRC_TIMER == pstMsg->u8Src)
  {
    af__sc_on_timer_tick();
    return DM_SUCCESS;
  }
  if (g_stSc.bDraining || (pstMsg->u32Len < sizeof(stDone)) || (NULL == pstMsg->pu8Data))
  {
    return DM_SUCCESS;
  }
  memcpy(&stDone, pstMsg->pu8Data, sizeof(stDone));
  if (AF_SC_OP_POLL == stDone.eOp)
  {
    g_stSc.bPolling = DM_FALSE;
    g_stSc.stDiag.u32PollCompleted++;
    af__sc_apply_poll(&stDone);
  }
  else if ((DM_SUCCESS != stDone.iStatus) && ('\0' != stDone.acErr[0]))
  {
    af__sc_log_err(stDone.acErr, af__sc_ops()->pfNowMs());
  }
  if ((AF_SC_OP_CONNECT == stDone.eOp) && (DM_SUCCESS != stDone.iStatus) &&
      (AF_SC_CONNECTING == g_stSc.eState) && (!g_stSc.stKeep.bPending))
  {
    af_sc_state_et ePrev = g_stSc.eState;
    g_stSc.eState = AF_SC_IDLE;
    g_stSc.bHasConnectDeadline = DM_FALSE;
    af__sc_publish(ePrev, DM_FALSE, DM_FALSE, NULL);
  }
  return DM_SUCCESS;
}

static int af__sc_on_ready(void *pvArg)
{
  (void)pvArg;
  (void)dm_timer_add(g_u32ScPollMs, g_u32ScPollMs, AF_LINK_SC_MAIN_DONE, AF_SC_TIMER_TAG,
                     &g_stSc.u32TimerId);
  af__sc_on_timer_tick();
  return DM_SUCCESS;
}

static int af__sc_init(void)
{
  int iRc = DM_SUCCESS;

  memset(&g_stSc, 0, sizeof(g_stSc));
  af_sc_err_throttle_init(&g_stSc.stErrThrottle);
  dm_mem_tag_name(AF_SC_MEMTAG, "af_sidecar");

  iRc = dm_link_register_handler(AF_LINK_SC_MAIN_DONE, af__sc_on_link, NULL, NULL);
  if (DM_SUCCESS != iRc)
  {
    return iRc;
  }
  return dm_event_subscribe(DM_EV_READY, af__sc_on_ready, NULL, NULL);
}

static void af__sc_drain(dm_io dm_drain_st *pstDrain)
{
  (void)pstDrain;
  g_stSc.bDraining = DM_TRUE;
  af_sc_keep_clear(&g_stSc.stKeep);
  if (0u != g_stSc.u32TimerId)
  {
    (void)dm_timer_cancel(g_stSc.u32TimerId);
    g_stSc.u32TimerId = 0u;
  }
}

static void af__sc_fini(void)
{
  (void)dm_event_unsubscribe(DM_EV_READY, af__sc_on_ready);
  memset(&g_stSc, 0, sizeof(g_stSc));
}

/* ============================ 3. DM 线程业务 API ============================ */

bool_t af_sidecar_in_cooldown(void)
{
  uint64 u64Now = af__sc_ops()->pfNowMs();
  return (g_stSc.bHasCooldown && (u64Now < g_stSc.u64CooldownUntilMs)) ? DM_TRUE : DM_FALSE;
}

void af_sidecar_get_snap(dm_out af_sc_snap_st *pstOut)
{
  uint32 i = 0u;
  if (NULL == pstOut)
  {
    return;
  }
  memset(pstOut, 0, sizeof(*pstOut));
  pstOut->eState = g_stSc.eState;
  snprintf(pstOut->acShowing, sizeof(pstOut->acShowing), "%s", g_stSc.acShowing);
  pstOut->u32NearbyCnt = g_stSc.u32NearbyCnt;
  for (i = 0u; (i < g_stSc.u32NearbyCnt) && (i < AF_SC_DEV_MAX); i++)
  {
    snprintf(pstOut->aacNearby[i], sizeof(pstOut->aacNearby[i]), "%s", g_stSc.aacNearby[i]);
  }
  pstOut->bInCooldown = af_sidecar_in_cooldown();
}

int af_sidecar_connect(dm_in const char *pcName, dm_in bool_t bWithRetry)
{
  uint64 u64Now = af__sc_ops()->pfNowMs();
  af_sc_state_et ePrev = g_stSc.eState;

  if ((NULL == pcName) || ('\0' == pcName[0]))
  {
    return DM_INVALID;
  }
  g_stSc.bHasGrace = DM_FALSE;
  g_stSc.u64GraceUntilMs = 0u;
  g_stSc.eState = AF_SC_CONNECTING;
  g_stSc.bHasConnectDeadline = DM_TRUE;
  g_stSc.u64ConnectUntilMs = u64Now + AF_SC_CONNECT_TIMEOUT_MS;
  if (bWithRetry)
  {
    af_sc_keep_want(&g_stSc.stKeep, pcName, u64Now);
  }
  else
  {
    af_sc_keep_once(&g_stSc.stKeep, pcName);
  }
  if (ePrev != g_stSc.eState)
  {
    af__sc_publish(ePrev, DM_FALSE, DM_FALSE, NULL);
  }
  return af__sc_dispatch_job(AF_SC_OP_CONNECT, pcName);
}

int af_sidecar_disconnect(dm_in const char *pcName)
{
  uint64 u64Now = af__sc_ops()->pfNowMs();
  af_sc_state_et ePrev = g_stSc.eState;

  af_sc_keep_clear(&g_stSc.stKeep);
  g_stSc.bHasGrace = DM_TRUE;
  g_stSc.u64GraceUntilMs = u64Now + AF_SC_GRACE_MS;
  g_stSc.bHasConnectDeadline = DM_FALSE;
  g_stSc.eState = AF_SC_IDLE;
  g_stSc.acShowing[0] = '\0';
  if (AF_SC_IDLE != ePrev)
  {
    af__sc_publish(ePrev, DM_TRUE, DM_FALSE, NULL);
  }
  return af__sc_dispatch_job(AF_SC_OP_DISCONNECT, pcName);
}

void af_sidecar_get_diag(dm_out af_sc_diag_st *pstOut)
{
  if (NULL != pstOut)
  {
    *pstOut = g_stSc.stDiag;
  }
}

const dm_module_st g_stAfSidecarMod = {.u16ModID = AF_MOD_SIDECAR,
                                       .pcName = "af_sidecar",
                                       .pfInit = af__sc_init,
                                       .pfDrain = af__sc_drain,
                                       .pfFini = af__sc_fini,
                                       .pastErrMap = NULL};

const dm_module_st *af_sidecar_module(void)
{
  return &g_stAfSidecarMod;
}
