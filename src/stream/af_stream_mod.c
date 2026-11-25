/**
 ******************************************************************************
 * @file    af_stream_mod.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   服务层 af_stream（ModID 24）4 态机、事件订阅、REQ_IDR 节流与安全停机编排。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "stream/af_stream_int.h"
#include "plat/af_plat.h"

#include <stdio.h>
#include <string.h>

typedef struct af__stream_ctx
{
  af_stream_state_et ePrev;
  af_stream_state_et eCur;
  uint64 u64Gen;
  uint64 u64StopGen;
  uint64 u64LastIdrNs;
  char acDevId[37];
  char acDevName[65];
  uint32 u32DispId;
  uint16 u16VideoPort;
  bool_t bChroma444;
  bool_t bCapStarted;
  bool_t bInited;
  int iDrainHold;
} af__stream_ctx_st;

static af__stream_ctx_st g_stStream;
static af_stream_ops_st g_stStreamOps;
static bool_t g_bStreamOpsInited = DM_FALSE;

/* ============================ 平台采集默认适配 ============================ */

static void af__stream_on_cap_frame(dm_in const af_stream_enc_frame_st *pstFrame,
                                    dm_io void *pvUser)
{
  (void)pvUser;
  af__stream_tx_submit(pstFrame);
}

static void af__stream_post_cb(dm_in af__stream_cb_kind_et eKind, dm_in uint64 u64Gen,
                               dm_in int iReason)
{
  af__stream_cb_msg_st stMsg = {eKind, u64Gen, iReason};
  (void)dm_queue_post_async(AF_LINK_STREAM_STATE_CB, &stMsg, (uint32)sizeof(stMsg));
}

static void af__stream_on_cap_stopped(dm_in uint64 u64Gen, dm_in int iReason, dm_io void *pvUser)
{
  (void)pvUser;
  af__stream_post_cb(AF_ST_EV_CAP_STOPPED, u64Gen, iReason);
}

static void af__stream_on_cap_stop_done(dm_in uint64 u64Gen, dm_in int iReason, dm_io void *pvUser)
{
  (void)pvUser;
  af__stream_post_cb(AF_ST_EV_CAP_STOP_DONE, u64Gen, iReason);
}

static void af__plat_frame_bridge(dm_in const af_plat_vid_frame_st *pstFrame, dm_io void *pvUser)
{
  af_stream_enc_frame_st stF;

  if (NULL == pstFrame)
  {
    return;
  }
  stF.u64Gen = pstFrame->u64Gen;
  stF.u32Flags = pstFrame->u32Flags;
  stF.u64CaptureUs = pstFrame->u64CaptureUs;
  stF.pu8Data = pstFrame->pu8Data;
  stF.u32Len = pstFrame->u32Len;
  af__stream_on_cap_frame(&stF, pvUser);
}

static int af__def_cap_start(dm_in const af_stream_cap_cfg_st *pstCfg, dm_out bool_t *pbEff444)
{
  af_plat_stream_cfg_st stPlat;

  if ((NULL == pstCfg) || (NULL == pbEff444))
  {
    return DM_INVALID;
  }
  memset(&stPlat, 0, sizeof(stPlat));
  stPlat.u64Gen = pstCfg->u64Gen;
  stPlat.u32DispId = pstCfg->u32DispId;
  stPlat.u16EncW = pstCfg->u16EncW;
  stPlat.u16EncH = pstCfg->u16EncH;
  stPlat.u16Fps = pstCfg->u16Fps;
  stPlat.bChroma444 = pstCfg->bChroma444;
  stPlat.pfOnFrame = af__plat_frame_bridge;
  stPlat.pfOnStopped = af__stream_on_cap_stopped;
  stPlat.pvUser = pstCfg->pvUser;
  return af_plat_stream_start(&stPlat, pbEff444);
}

static void af__stream_init_default_ops(dm_out af_stream_ops_st *pstOps)
{
  memset(pstOps, 0, sizeof(*pstOps));
  pstOps->pfCapStart = af__def_cap_start;
  pstOps->pfCapStopAsync = af_plat_stream_stop_async;
  pstOps->pfCapStopSync = af_plat_stream_stop_sync;
  pstOps->pfCapForceIdr = af_plat_stream_force_idr;
  pstOps->pfCapSwitchDisp = af_plat_stream_switch_disp;
  pstOps->pfCapSetBitrate = af_plat_stream_set_bitrate;
  pstOps->pfSupports444 = af_plat_hevc444_supported;
  pstOps->u32AuthTimeoutMs = 0u;
}

static const af_stream_ops_st *af__stream_ops(void)
{
  if (!g_bStreamOpsInited)
  {
    af__stream_init_default_ops(&g_stStreamOps);
    g_bStreamOpsInited = DM_TRUE;
  }
  return &g_stStreamOps;
}

/* ============================ 状态切换与事件发布 ============================ */

static void af__stream_fill_ev(dm_out af_ev_stream_st *pstEv)
{
  memset(pstEv, 0, sizeof(*pstEv));
  pstEv->ePrev = g_stStream.ePrev;
  pstEv->eCur = g_stStream.eCur;
  pstEv->u64Gen = g_stStream.u64Gen;
  snprintf(pstEv->acDevId, sizeof(pstEv->acDevId), "%s", g_stStream.acDevId);
  snprintf(pstEv->acDevName, sizeof(pstEv->acDevName), "%s", g_stStream.acDevName);
  pstEv->u32DispId = g_stStream.u32DispId;
  pstEv->bChroma444 = g_stStream.bChroma444;
}

static void af__stream_set_state(dm_in af_stream_state_et eNext)
{
  af_ev_stream_st stEv;

  g_stStream.ePrev = g_stStream.eCur;
  g_stStream.eCur = eNext;
  af__stream_fill_ev(&stEv);
  (void)dm_event_publish(AF_EV_STREAM_CHANGED, &stEv, (uint32)sizeof(stEv));
}

static void af__stream_clear_meta(void)
{
  g_stStream.acDevId[0] = '\0';
  g_stStream.acDevName[0] = '\0';
  g_stStream.u32DispId = 0u;
  g_stStream.u16VideoPort = 0u;
  g_stStream.bChroma444 = DM_FALSE;
  g_stStream.u64LastIdrNs = 0u;
}

static void af__stream_finish_stop(void)
{
  int iHold = 0;

  g_stStream.bCapStarted = DM_FALSE;
  af__stream_tx_stop_join();
  af__stream_clear_meta();
  af__stream_set_state(AF_STREAM_IDLE);
  if (g_stStream.iDrainHold > 0)
  {
    iHold = g_stStream.iDrainHold;
    g_stStream.iDrainHold = 0;
    dm_drain_release(iHold);
  }
}

/* ============================ T_core 回调处理 ============================ */

static int af__stream_on_state_cb(dm_io void *pvArg)
{
  const dm_link_msg_st *pstLink = (const dm_link_msg_st *)pvArg;
  const af_stream_ops_st *pstOps = af__stream_ops();
  af__stream_cb_msg_st stMsg;

  if ((NULL == pstLink) || (NULL == pstLink->pu8Data) || (pstLink->u32Len < sizeof(stMsg)))
  {
    return DM_SUCCESS;
  }
  memcpy(&stMsg, pstLink->pu8Data, sizeof(stMsg));
  if (AF_ST_EV_CLIENT_AUTHED == stMsg.eKind)
  {
    if ((AF_STREAM_STREAMING == g_stStream.eCur) && (stMsg.u64Gen == g_stStream.u64Gen))
    {
      pstOps->pfCapForceIdr(g_stStream.u64Gen);
    }
    return DM_SUCCESS;
  }
  if ((AF_ST_EV_TX_DROPPED == stMsg.eKind) || (AF_ST_EV_CAP_STOPPED == stMsg.eKind))
  {
    if ((AF_STREAM_STREAMING == g_stStream.eCur) && (stMsg.u64Gen == g_stStream.u64Gen))
    {
      DM_LOG_INFO("af_stream async stop trigger (kind=%d reason=%d)", (int)stMsg.eKind,
                  stMsg.iReason);
      af_stream_force_stop();
    }
    return DM_SUCCESS;
  }
  if (AF_ST_EV_SET_BITRATE == stMsg.eKind)
  {
    if ((AF_STREAM_STREAMING == g_stStream.eCur) && (stMsg.u64Gen == g_stStream.u64Gen) &&
        (stMsg.iReason > 0) && (NULL != pstOps->pfCapSetBitrate))
    {
      pstOps->pfCapSetBitrate(g_stStream.u64Gen, (uint32)stMsg.iReason);
    }
    return DM_SUCCESS;
  }
  if ((AF_ST_EV_CAP_STOP_DONE == stMsg.eKind) && (AF_STREAM_STOPPING == g_stStream.eCur) &&
      (stMsg.u64Gen == g_stStream.u64StopGen))
  {
    af__stream_finish_stop();
  }
  return DM_SUCCESS;
}

static int af__stream_on_req_idr(dm_io void *pvArg)
{
  const dm_link_msg_st *pstLink = (const dm_link_msg_st *)pvArg;
  const af_stream_ops_st *pstOps = af__stream_ops();
  uint64 u64MsgGen = 0u;
  uint64 u64NowNs = 0u;

  if (AF_STREAM_STREAMING != g_stStream.eCur)
  {
    return DM_SUCCESS;
  }
  if ((NULL != pstLink) && (NULL != pstLink->pu8Data) && (pstLink->u32Len >= sizeof(uint64)))
  {
    memcpy(&u64MsgGen, pstLink->pu8Data, sizeof(uint64));
    if ((0u != u64MsgGen) && (u64MsgGen != g_stStream.u64Gen))
    {
      return DM_SUCCESS;
    }
  }
  u64NowNs = dm_hrtime();
  if ((0u != g_stStream.u64LastIdrNs) &&
      ((u64NowNs - g_stStream.u64LastIdrNs) < ((uint64)AF_STREAM_REQ_IDR_MIN_MS * 1000000ULL)))
  {
    return DM_SUCCESS;
  }
  g_stStream.u64LastIdrNs = u64NowNs;
  pstOps->pfCapForceIdr(g_stStream.u64Gen);
  return DM_SUCCESS;
}

static int af__stream_on_display_ev(dm_io void *pvArg)
{
  const dm_link_msg_st *pstLink = (const dm_link_msg_st *)pvArg;
  const af_stream_ops_st *pstOps = af__stream_ops();
  af_ev_display_st stEv;
  af_ev_stream_st stOut;

  if ((NULL == pstLink) || (NULL == pstLink->pu8Data) || (pstLink->u32Len < sizeof(stEv)))
  {
    return DM_SUCCESS;
  }
  memcpy(&stEv, pstLink->pu8Data, sizeof(stEv));
  if ((AF_STREAM_STREAMING == g_stStream.eCur) && (0u != stEv.u32VdDispId) &&
      (stEv.u32VdDispId != g_stStream.u32DispId))
  {
    g_stStream.u32DispId = stEv.u32VdDispId;
    pstOps->pfCapSwitchDisp(g_stStream.u64Gen, stEv.u32VdDispId);
    g_stStream.ePrev = g_stStream.eCur;
    af__stream_fill_ev(&stOut);
    (void)dm_event_publish(AF_EV_STREAM_CHANGED, &stOut, (uint32)sizeof(stOut));
  }
  return DM_SUCCESS;
}

/* ============================ 公开 API ============================ */

void af_stream_set_ops(dm_in const af_stream_ops_st *pstOps)
{
  af__stream_init_default_ops(&g_stStreamOps);
  g_bStreamOpsInited = DM_TRUE;
  if (NULL == pstOps)
  {
    return;
  }
  if (NULL != pstOps->pfCapStart)
  {
    g_stStreamOps.pfCapStart = pstOps->pfCapStart;
  }
  if (NULL != pstOps->pfCapStopAsync)
  {
    g_stStreamOps.pfCapStopAsync = pstOps->pfCapStopAsync;
  }
  if (NULL != pstOps->pfCapStopSync)
  {
    g_stStreamOps.pfCapStopSync = pstOps->pfCapStopSync;
  }
  if (NULL != pstOps->pfCapForceIdr)
  {
    g_stStreamOps.pfCapForceIdr = pstOps->pfCapForceIdr;
  }
  if (NULL != pstOps->pfCapSwitchDisp)
  {
    g_stStreamOps.pfCapSwitchDisp = pstOps->pfCapSwitchDisp;
  }
  if (NULL != pstOps->pfCapSetBitrate)
  {
    g_stStreamOps.pfCapSetBitrate = pstOps->pfCapSetBitrate;
  }
  if (NULL != pstOps->pfSupports444)
  {
    g_stStreamOps.pfSupports444 = pstOps->pfSupports444;
  }
  g_stStreamOps.u32AuthTimeoutMs = pstOps->u32AuthTimeoutMs;
}

static int af__stream_start_tx_part(dm_in const af_stream_start_req_st *pstReq,
                                    dm_out uint16 *pu16Port)
{
  const af_stream_ops_st *pstOps = af__stream_ops();
  af__stream_tx_cfg_st stTxCfg;
  int iErr = DM_SUCCESS;

  memset(&stTxCfg, 0, sizeof(stTxCfg));
  stTxCfg.u64Gen = g_stStream.u64Gen;
  memcpy(stTxCfg.au8SidBe, pstReq->au8SidBe, sizeof(stTxCfg.au8SidBe));
  memcpy(stTxCfg.au8VidAuthKey, pstReq->au8VidAuthKey, sizeof(stTxCfg.au8VidAuthKey));
  stTxCfg.u32AuthTimeoutMs =
      (0u != pstOps->u32AuthTimeoutMs) ? pstOps->u32AuthTimeoutMs : AF_STREAM_AUTH_TIMEOUT_MS;
  iErr = af__stream_tx_start(&stTxCfg, pu16Port);
  dm_secure_zero(&stTxCfg, sizeof(stTxCfg));
  return iErr;
}

int af_stream_start(dm_in const af_stream_start_req_st *pstReq,
                    dm_out af_stream_start_res_st *pstRes)
{
  const af_stream_ops_st *pstOps = af__stream_ops();
  af_stream_cap_cfg_st stCapCfg;
  uint16 u16Port = 0u;
  bool_t bEff444 = DM_FALSE;
  int iErr = DM_SUCCESS;

  if ((NULL == pstReq) || (NULL == pstRes) || (0u == pstReq->u16EncW) || (0u == pstReq->u16EncH))
  {
    return DM_INVALID;
  }
  if (AF_STREAM_STOPPING == g_stStream.eCur)
  {
    return AF_ERR_STREAM_STOPPING;
  }
  if (AF_STREAM_IDLE != g_stStream.eCur)
  {
    return AF_ERR_STREAM_BUSY;
  }
  memset(pstRes, 0, sizeof(*pstRes));
  g_stStream.u64Gen++;
  g_stStream.u64Gen = (0u == g_stStream.u64Gen) ? 1u : g_stStream.u64Gen;
  snprintf(g_stStream.acDevId, sizeof(g_stStream.acDevId), "%s", pstReq->acDevId);
  snprintf(g_stStream.acDevName, sizeof(g_stStream.acDevName), "%s", pstReq->acDevName);
  g_stStream.u32DispId = pstReq->u32DispId;
  g_stStream.bChroma444 = pstReq->bChroma444;
  g_stStream.u64LastIdrNs = 0u;
  af__stream_set_state(AF_STREAM_STARTING);

  iErr = af__stream_start_tx_part(pstReq, &u16Port);
  if (DM_SUCCESS != iErr)
  {
    af__stream_clear_meta();
    af__stream_set_state(AF_STREAM_IDLE);
    return iErr;
  }
  memset(&stCapCfg, 0, sizeof(stCapCfg));
  stCapCfg.u64Gen = g_stStream.u64Gen;
  stCapCfg.u32DispId = pstReq->u32DispId;
  stCapCfg.u16EncW = pstReq->u16EncW;
  stCapCfg.u16EncH = pstReq->u16EncH;
  stCapCfg.u16Fps = (0u != pstReq->u16Fps) ? pstReq->u16Fps : 60u;
  stCapCfg.bChroma444 = pstReq->bChroma444;
  stCapCfg.pfOnFrame = af__stream_on_cap_frame;
  stCapCfg.pfOnStopped = af__stream_on_cap_stopped;
  stCapCfg.pvUser = &g_stStream;
  iErr = pstOps->pfCapStart(&stCapCfg, &bEff444);
  if (DM_SUCCESS != iErr)
  {
    af__stream_tx_stop_join();
    af__stream_clear_meta();
    af__stream_set_state(AF_STREAM_IDLE);
    return iErr;
  }
  g_stStream.bCapStarted = DM_TRUE;
  g_stStream.bChroma444 = bEff444;
  g_stStream.u16VideoPort = u16Port;
  af__stream_set_state(AF_STREAM_STREAMING);
  pstRes->u16VideoPort = u16Port;
  pstRes->bEffChroma444 = bEff444;
  return DM_SUCCESS;
}

void af_stream_force_stop(void)
{
  const af_stream_ops_st *pstOps = af__stream_ops();
  uint64 u64OldGen = 0u;

  if ((AF_STREAM_IDLE == g_stStream.eCur) || (AF_STREAM_STOPPING == g_stStream.eCur))
  {
    return;
  }
  u64OldGen = g_stStream.u64Gen;
  g_stStream.u64Gen++;
  g_stStream.u64Gen = (0u == g_stStream.u64Gen) ? 1u : g_stStream.u64Gen;
  g_stStream.u64StopGen = u64OldGen;
  af__stream_tx_invalidate_gen();
  af__stream_set_state(AF_STREAM_STOPPING);
  if (g_stStream.bCapStarted)
  {
    pstOps->pfCapStopAsync(u64OldGen, af__stream_on_cap_stop_done, &g_stStream);
  }
  else
  {
    af__stream_finish_stop();
  }
}

void af_stream_get_snapshot(dm_out af_ev_stream_st *pstOut)
{
  if (NULL != pstOut)
  {
    af__stream_fill_ev(pstOut);
  }
}

bool_t af_stream_is_hevc444_supported(void)
{
  return af__stream_ops()->pfSupports444();
}

/* ============================ DM 模块生命周期 ============================ */

static int af__stream_init(void)
{
  int iErr = DM_SUCCESS;

  dm_mem_tag_name(AF_TAG_STREAM, "af_stream");
  dm_mem_tag_name(AF_TAG_STREAM_FRAME, "af_stream_frame");
  g_stStream.ePrev = AF_STREAM_IDLE;
  g_stStream.eCur = AF_STREAM_IDLE;
  g_stStream.bCapStarted = DM_FALSE;
  g_stStream.iDrainHold = 0;
  af__stream_clear_meta();

  iErr = dm_link_register_handler(AF_LINK_STREAM_STATE_CB, af__stream_on_state_cb, NULL, NULL);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr = dm_link_register_handler(AF_LINK_STREAM_REQ_IDR, af__stream_on_req_idr, NULL, NULL);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr = dm_event_subscribe(AF_EV_DISPLAY_CHANGED, af__stream_on_display_ev, NULL, NULL);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  g_stStream.bInited = DM_TRUE;
  return DM_SUCCESS;
}

static void af__stream_drain(dm_io dm_drain_st *pstDrain)
{
  if (AF_STREAM_IDLE == g_stStream.eCur)
  {
    return;
  }
  if (g_stStream.iDrainHold <= 0)
  {
    g_stStream.iDrainHold = dm_drain_hold(pstDrain);
  }
  if (AF_STREAM_STOPPING != g_stStream.eCur)
  {
    af_stream_force_stop();
  }
}

static void af__stream_fini(void)
{
  const af_stream_ops_st *pstOps = af__stream_ops();

  if (AF_STREAM_IDLE != g_stStream.eCur)
  {
    DM_LOG_WARN("af_stream fini sync fallback (state=%d)", (int)g_stStream.eCur);
    af__stream_tx_invalidate_gen();
    if (g_stStream.bCapStarted)
    {
      pstOps->pfCapStopSync();
      g_stStream.bCapStarted = DM_FALSE;
    }
    af__stream_tx_stop_join();
    af__stream_clear_meta();
    g_stStream.ePrev = g_stStream.eCur;
    g_stStream.eCur = AF_STREAM_IDLE;
    g_stStream.iDrainHold = 0;
  }
  if (g_stStream.bInited)
  {
    (void)dm_event_unsubscribe(AF_EV_DISPLAY_CHANGED, af__stream_on_display_ev);
    g_stStream.bInited = DM_FALSE;
  }
}

static const dm_err_entry_st g_astAfStreamErrs[] = {
    {AF_ERR_PERM_SC, "screen capture permission missing"},
    {AF_ERR_STREAM_BUSY, "stream already active"},
    {AF_ERR_STREAM_STOPPING, "stream is stopping"},
    {AF_ERR_STREAM_NOT_ACT, "stream not active"},
    {0, NULL}};

const dm_module_st g_stAfStreamMod = {.u16ModID = AF_MOD_STREAM,
                                      .pcName = "af_stream",
                                      .pfInit = af__stream_init,
                                      .pfDrain = af__stream_drain,
                                      .pfFini = af__stream_fini,
                                      .pastErrMap = g_astAfStreamErrs};

const dm_module_st *af_stream_module(void)
{
  return &g_stAfStreamMod;
}

#ifdef __cplusplus
}
#endif
