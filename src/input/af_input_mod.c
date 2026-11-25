/**
 ******************************************************************************
 * @file    af_input_mod.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   服务层 af_input（ModID 25）：T_input 高优 UDP AEAD 线程、看门狗与安全输入监控。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "af_input"
#include "input/af_input_int.h"

#include <string.h>

typedef struct af__in_boot
{
  dm_mutex_st *pstMtx;
  dm_cond_st *pstCond;
  bool_t bDone;
  int iErr;
  uint16 u16Port;
} af__in_boot_st;

typedef struct af_input_sess
{
  uint8 au8SidBe[8];
  uint8 au8KeyI2M[32];
  uint8 au8KeyM2I[32];
  uint32 u32DispId;
  uint32 u32DispW;
  uint32 u32DispH;
  uint16 u16Port;
  af_input_ops_st stOps;

  /* 密码学与协议状态（仅 T_input 独占访问） */
  mbedtls_chachapoly_context stRxAead;
  mbedtls_chachapoly_context stTxAead;
  bool_t bAeadReady;
  af_dp_replay_t stReplay;
  af_dp_cum_state_t stCum;
  af_dp_rx_dedup_t stDedup;
  dm_sockaddr_st stPeerAddr;
  bool_t bHasPeer;
  uint64 u64M2iSeq;
  uint32 u32M2iEvSeq;
  uint64 u64LastIdrNs;

  /* T_input 独立循环与句柄（遵循 AUD-V21-P1-10） */
  dm_thread_st *pstInputThread;
  dm_loop_st *pstLoop;
  dm_async_st *pstAsync;
  dm_loop_timer_st *pstWdTmr;
  dm_udp_st *pstUdp;
  af__in_boot_st *pstBoot;

  /* T_core -> T_input 控制信箱（pstCtlMtx 串行化 dm_async_send 与停机 close，UDP 热路径零锁） */
  dm_mutex_st *pstCtlMtx;
  bool_t bStopInput;
  _Atomic bool_t bDispUpdPending;
  _Atomic uint32 u32PendingDispId;
  _Atomic bool_t bSecSendPending;
  _Atomic bool_t bSecState;

  /* T_core 独占状态 */
  bool_t bLastSecInput;

  /* T_inject 无锁 SPSC 环与消费者 */
  af_inj_ctx_st stInj;
} af_input_sess_st;

typedef struct af__in_mod
{
  af_input_ops_st stUserOps;
  af_input_sess_st *pstSess;
  uint32 u32SecTimer;
  uint64 u64SessGen;
  _Atomic uint32 u32WdEvCnt;
  bool_t bInited;
} af__in_mod_st;

static af__in_mod_st g_stAfInMod;

static const dm_err_entry_st g_astAfInputErrs[] = {
    {AF_ERR_PERM_AX, "accessibility permission required"},
    {0, NULL},
};

/* ============================ Ops 解析与工具 ============================ */

static void af__in_resolve_ops(dm_in const af_input_ops_st *pstU, dm_out af_input_ops_st *pstO)
{
  memset(pstO, 0, sizeof(*pstO));
  pstO->pfGetBounds = (NULL != pstU->pfGetBounds) ? pstU->pfGetBounds : af_plat_disp_bounds;
  pstO->pfInjectMouse = (NULL != pstU->pfInjectMouse) ? pstU->pfInjectMouse : af_plat_inject_mouse;
  pstO->pfInjectScroll =
      (NULL != pstU->pfInjectScroll) ? pstU->pfInjectScroll : af_plat_inject_scroll;
  pstO->pfInjectKey = (NULL != pstU->pfInjectKey) ? pstU->pfInjectKey : af_plat_inject_key;
  pstO->pfInjectPencil =
      (NULL != pstU->pfInjectPencil) ? pstU->pfInjectPencil : af_plat_inject_pencil;
  pstO->pfInjectUnicode =
      (NULL != pstU->pfInjectUnicode) ? pstU->pfInjectUnicode : af_plat_inject_unicode;
  pstO->pfInjectFlags = (NULL != pstU->pfInjectFlags) ? pstU->pfInjectFlags : af_plat_inject_flags;
  pstO->pfIsSecureInput =
      (NULL != pstU->pfIsSecureInput) ? pstU->pfIsSecureInput : af_plat_is_secure_input;
  pstO->u32WatchdogMs = (0u != pstU->u32WatchdogMs) ? pstU->u32WatchdogMs : AF_INPUT_WD_DEFAULT_MS;
  pstO->u32SecPollMs = (0u != pstU->u32SecPollMs) ? pstU->u32SecPollMs : AF_INPUT_SEC_DEFAULT_MS;
}

static bool_t af__in_flush_disp_upd(dm_io af_input_sess_st *pstS)
{
  af_inj_item_st stIt;

  if (!atomic_exchange_explicit(&pstS->bDispUpdPending, DM_FALSE, memory_order_acq_rel))
  {
    return DM_FALSE;
  }
  memset(&stIt, 0, sizeof(stIt));
  stIt.eKind = AF_INJ_BOUNDS_UPD;
  stIt.u.u32DispId = atomic_load_explicit(&pstS->u32PendingDispId, memory_order_acquire);
  (void)af__inj_push(&pstS->stInj, &stIt);
  return DM_TRUE;
}

/* ============================ T_input 下行加密回包 ============================ */

static void af__in_send_m2i_pkt(dm_io af_input_sess_st *pstS, dm_in const af_dp_in_pkt_t *pstPkt)
{
  uint8 au8Plain[AF_DP_IN_PLAIN_MAX];
  uint8 au8Wire[AF_DP_UDP_MTU_MAX];
  uint8 au8Nonce[AF_DP_AEAD_NONCE_LEN];
  size_t uPlainLen = 0u;
  uint64 u64Seq = 0u;

  if ((!pstS->bHasPeer) || (NULL == pstS->pstUdp) || (pstS->u64M2iSeq >= AF_DP_SEQ_MAX_SAFE))
  {
    return;
  }
  if (AF_DP_OK != af_dp_in_encode(pstPkt, au8Plain, sizeof(au8Plain), &uPlainLen))
  {
    return;
  }
  u64Seq = pstS->u64M2iSeq++;
  if (AF_DP_OK != af_dp_udp_outer_encode(au8Wire, pstS->au8SidBe, u64Seq))
  {
    dm_secure_zero(au8Plain, sizeof(au8Plain));
    return;
  }
  af_dp_aead_nonce(au8Nonce, AF_DP_DIR_M2I, u64Seq);
  if (0 == mbedtls_chachapoly_encrypt_and_tag(
               &pstS->stTxAead, uPlainLen, au8Nonce, au8Wire, AF_DP_UDP_OUTER_LEN, au8Plain,
               au8Wire + AF_DP_UDP_OUTER_LEN, au8Wire + AF_DP_UDP_OUTER_LEN + uPlainLen))
  {
    size_t uWireLen = AF_DP_UDP_OUTER_LEN + uPlainLen + AF_DP_AEAD_TAG_LEN;
    (void)dm_udp_try_send(pstS->pstUdp, au8Wire, uWireLen, &pstS->stPeerAddr);
  }
  dm_secure_zero(au8Plain, sizeof(au8Plain));
}

static void af__in_attach_sec_ev(dm_io af_input_sess_st *pstS, dm_io af_dp_in_pkt_t *pstPkt,
                                 dm_in bool_t bActive)
{
  pstS->u32M2iEvSeq = (0xFFFFFFFFu == pstS->u32M2iEvSeq) ? 1u : (pstS->u32M2iEvSeq + 1u);
  pstPkt->hdr.ev_cnt = 1u;
  pstPkt->evs[0].ev_seq = pstS->u32M2iEvSeq;
  pstPkt->evs[0].kind = AF_EV_SEC_INPUT;
  pstPkt->evs[0].u.sec_input.active = bActive ? 1u : 0u;
}

static void af__in_send_sec_ev(dm_io af_input_sess_st *pstS, dm_in bool_t bActive)
{
  af_dp_in_pkt_t stPkt;

  if (!pstS->bHasPeer)
  {
    return;
  }
  memset(&stPkt, 0, sizeof(stPkt));
  stPkt.hdr.ver = AF_DP_IN_VER;
  stPkt.hdr.ack_ev_seq = pstS->stDedup.last_ev_seq;
  af__in_attach_sec_ev(pstS, &stPkt, bActive);
  af__in_send_m2i_pkt(pstS, &stPkt);
}

/* ============================ T_input 看门狗与 UDP 接收 ============================ */

static void af__in_on_wd_timeout(dm_io dm_loop_timer_st *pstTimer, dm_io void *pvUser)
{
  af_input_sess_st *pstS = (af_input_sess_st *)pvUser;
  af_inj_item_st stRel;

  (void)pstTimer;
  if (NULL == pstS)
  {
    return;
  }
  memset(&stRel, 0, sizeof(stRel));
  stRel.eKind = AF_INJ_RELEASE_ALL;
  (void)af__inj_push(&pstS->stInj, &stRel);
  af__inj_wake(&pstS->stInj);
  (void)dm_queue_post_async(AF_LINK_INPUT_WD_EV, NULL, 0u);
}

static bool_t af__in_enqueue_pkt(dm_io af_input_sess_st *pstS, dm_in const af_dp_in_pkt_t *pstPkt,
                                 dm_in int iIsNewest, dm_out bool_t *pbHasNewEv)
{
  af_dp_delta_t stDelta;
  af_inj_item_st stIt;
  bool_t bPushed = DM_FALSE;
  uint8 i = 0u;

  *pbHasNewEv = DM_FALSE;
  af_dp_cum_step(&pstS->stCum, &pstPkt->hdr, iIsNewest, &stDelta);
  if ((0 != stDelta.dx) || (0 != stDelta.dy))
  {
    stIt = (af_inj_item_st){.eKind = AF_INJ_REL_MOVE, .u.stRel = {stDelta.dx, stDelta.dy}};
    bPushed = af__inj_push(&pstS->stInj, &stIt) ? DM_TRUE : bPushed;
  }
  if ((0 != stDelta.wx) || (0 != stDelta.wy))
  {
    stIt = (af_inj_item_st){.eKind = AF_INJ_SCROLL, .u.stScroll = {stDelta.wx, stDelta.wy}};
    bPushed = af__inj_push(&pstS->stInj, &stIt) ? DM_TRUE : bPushed;
  }
  for (i = 0u; i < pstPkt->hdr.ev_cnt; i++)
  {
    if (!af_dp_rx_dedup_accept(&pstS->stDedup, pstPkt->evs[i].ev_seq))
    {
      continue;
    }
    *pbHasNewEv = DM_TRUE;
    if (af__inj_ev_to_item(&pstPkt->evs[i], &stIt))
    {
      bPushed = af__inj_push(&pstS->stInj, &stIt) ? DM_TRUE : bPushed;
    }
  }
  if (0u != (pstPkt->hdr.flags & AF_IN_FLAG_RELEASE_ALL))
  {
    stIt = (af_inj_item_st){.eKind = AF_INJ_RELEASE_ALL};
    bPushed = af__inj_push(&pstS->stInj, &stIt) ? DM_TRUE : bPushed;
  }
  return bPushed;
}

static void af__in_maybe_req_idr(dm_io af_input_sess_st *pstS, dm_in uint8 u8Flags)
{
  uint64 u64Now = 0u;

  if (0u == (u8Flags & AF_IN_FLAG_REQ_IDR))
  {
    return;
  }
  u64Now = dm_hrtime();
  if ((0u == pstS->u64LastIdrNs) || ((u64Now - pstS->u64LastIdrNs) >= AF_INPUT_IDR_THROTTLE_NS))
  {
    pstS->u64LastIdrNs = u64Now;
    (void)dm_queue_post_async(AF_LINK_STREAM_REQ_IDR, NULL, 0u);
  }
}

static void af__in_maybe_reply(dm_io af_input_sess_st *pstS, dm_in const af_dp_in_pkt_t *pstPkt,
                               dm_in bool_t bHasNewEv)
{
  bool_t bPing = (0u != (pstPkt->hdr.flags & AF_IN_FLAG_PING_PONG)) ? DM_TRUE : DM_FALSE;
  bool_t bSecDirty = pstS->bHasPeer && atomic_exchange_explicit(&pstS->bSecSendPending, DM_FALSE,
                                                                memory_order_acq_rel);
  af_dp_in_pkt_t stReply;

  if ((!bPing) && (!bHasNewEv) && (0u == pstPkt->hdr.ev_cnt) && (!bSecDirty))
  {
    return;
  }
  memset(&stReply, 0, sizeof(stReply));
  stReply.hdr.ver = AF_DP_IN_VER;
  stReply.hdr.flags = bPing ? AF_IN_FLAG_PING_PONG : 0u;
  stReply.hdr.ack_ev_seq = pstS->stDedup.last_ev_seq;
  stReply.hdr.client_us = pstPkt->hdr.client_us;
  if (bSecDirty)
  {
    af__in_attach_sec_ev(pstS, &stReply,
                         atomic_load_explicit(&pstS->bSecState, memory_order_acquire));
  }
  af__in_send_m2i_pkt(pstS, &stReply);
}

static void af__in_on_udp_recv(dm_io dm_udp_st *pstUdp, dm_in int iErr,
                               dm_in const dm_udp_rx_st *pstRx, dm_io void *pvUser)
{
  af_input_sess_st *pstS = (af_input_sess_st *)pvUser;
  uint8 au8Sid[8];
  uint8 au8Nonce[AF_DP_AEAD_NONCE_LEN];
  uint8 au8Plain[AF_DP_IN_PLAIN_MAX];
  af_dp_in_pkt_t stPkt;
  uint64 u64PktSeq = 0u;
  size_t uPlainLen = 0u;
  int iIsNewest = 0;
  bool_t bPushed = DM_FALSE;
  bool_t bHasNewEv = DM_FALSE;

  (void)pstUdp;
  if ((DM_SUCCESS != iErr) || (NULL == pstRx) || (NULL == pstS) ||
      (pstRx->uLen < AF_INPUT_UDP_MIN_LEN) || (pstRx->uLen > AF_DP_UDP_MTU_MAX) ||
      (AF_DP_OK != af_dp_udp_outer_decode(au8Sid, &u64PktSeq, pstRx->pu8Data)) ||
      (!dm_crypto_const_eq(au8Sid, pstS->au8SidBe, 8u)) || (0u == u64PktSeq) ||
      (u64PktSeq >= AF_DP_SEQ_MAX_SAFE))
  {
    return;
  }
  uPlainLen = pstRx->uLen - AF_DP_UDP_OUTER_LEN - AF_DP_AEAD_TAG_LEN;
  af_dp_aead_nonce(au8Nonce, AF_DP_DIR_I2M, u64PktSeq);
  if (0 != mbedtls_chachapoly_auth_decrypt(&pstS->stRxAead, uPlainLen, au8Nonce, pstRx->pu8Data,
                                           AF_DP_UDP_OUTER_LEN,
                                           pstRx->pu8Data + AF_DP_UDP_OUTER_LEN + uPlainLen,
                                           pstRx->pu8Data + AF_DP_UDP_OUTER_LEN, au8Plain))
  {
    return;
  }
  if ((AF_DP_OK != af_dp_replay_check(&pstS->stReplay, u64PktSeq, &iIsNewest)) ||
      (AF_DP_OK != af_dp_in_decode(&stPkt, au8Plain, uPlainLen)))
  {
    dm_secure_zero(au8Plain, uPlainLen);
    return;
  }

  pstS->stPeerAddr = pstRx->stPeer;
  pstS->bHasPeer = DM_TRUE;
  if (NULL != pstS->pstWdTmr)
  {
    (void)dm_loop_timer_start(pstS->pstWdTmr, pstS->stOps.u32WatchdogMs, 0u, af__in_on_wd_timeout,
                              pstS);
  }
  bPushed = af__in_flush_disp_upd(pstS);
  bPushed = af__in_enqueue_pkt(pstS, &stPkt, iIsNewest, &bHasNewEv) ? DM_TRUE : bPushed;
  if (bPushed)
  {
    af__inj_wake(&pstS->stInj);
  }
  af__in_maybe_req_idr(pstS, stPkt.hdr.flags);
  af__in_maybe_reply(pstS, &stPkt, bHasNewEv);
  dm_secure_zero(au8Plain, uPlainLen);
}

/* ============================ T_input 异步控制与线程主入口 ============================ */

static void af__in_close_loop_handles(dm_io af_input_sess_st *pstS)
{
  dm_async_st *pstLocalAsync = NULL;

  if (NULL != pstS->pstUdp)
  {
    (void)dm_udp_recv_stop(pstS->pstUdp);
    (void)dm_udp_close(pstS->pstUdp, NULL, NULL);
    pstS->pstUdp = NULL;
  }
  if (NULL != pstS->pstWdTmr)
  {
    (void)dm_loop_timer_stop(pstS->pstWdTmr);
    (void)dm_loop_timer_close(pstS->pstWdTmr, NULL, NULL);
    pstS->pstWdTmr = NULL;
  }
  dm_mutex_lock(pstS->pstCtlMtx);
  pstLocalAsync = pstS->pstAsync;
  pstS->pstAsync = NULL;
  dm_mutex_unlock(pstS->pstCtlMtx);
  if (NULL != pstLocalAsync)
  {
    (void)dm_async_close(pstLocalAsync, NULL, NULL);
  }
}

static void af__in_wake_input(dm_io af_input_sess_st *pstS, dm_in bool_t bSetStop)
{
  dm_mutex_lock(pstS->pstCtlMtx);
  if (bSetStop)
  {
    pstS->bStopInput = DM_TRUE;
  }
  if ((NULL != pstS->pstAsync) && (bSetStop || (!pstS->bStopInput)))
  {
    (void)dm_async_send(pstS->pstAsync);
  }
  dm_mutex_unlock(pstS->pstCtlMtx);
}

static void af__in_on_async(dm_io dm_async_st *pstAsync, dm_io void *pvUser)
{
  af_input_sess_st *pstS = (af_input_sess_st *)pvUser;
  bool_t bStop = DM_FALSE;

  (void)pstAsync;
  if (NULL == pstS)
  {
    return;
  }
  dm_mutex_lock(pstS->pstCtlMtx);
  bStop = pstS->bStopInput;
  dm_mutex_unlock(pstS->pstCtlMtx);
  if (bStop)
  {
    af__in_close_loop_handles(pstS);
    return;
  }
  if (af__in_flush_disp_upd(pstS))
  {
    af__inj_wake(&pstS->stInj);
  }
  if (pstS->bHasPeer &&
      atomic_exchange_explicit(&pstS->bSecSendPending, DM_FALSE, memory_order_acq_rel))
  {
    af__in_send_sec_ev(pstS, atomic_load_explicit(&pstS->bSecState, memory_order_acquire));
  }
}

static int af__in_bind_dual_stack(dm_io af_input_sess_st *pstS, dm_out uint16 *pu16Port)
{
  dm_sockaddr_st stBind;
  dm_sockaddr_st stLocal;
  int iErr = dm_udp_create(pstS->pstLoop, &pstS->pstUdp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr = (DM_SUCCESS == dm_sockaddr_ip6("::", 0u, &stBind))
             ? dm_udp_bind(pstS->pstUdp, &stBind, DM_UDP_F_NONE)
             : DM_FAILD;
  if (DM_SUCCESS != iErr)
  {
    (void)dm_udp_close(pstS->pstUdp, NULL, NULL);
    pstS->pstUdp = NULL;
    iErr = dm_udp_create(pstS->pstLoop, &pstS->pstUdp);
    if ((DM_SUCCESS == iErr) && (DM_SUCCESS == dm_sockaddr_ip4("0.0.0.0", 0u, &stBind)))
    {
      iErr = dm_udp_bind(pstS->pstUdp, &stBind, DM_UDP_F_NONE);
    }
  }
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  memset(&stLocal, 0, sizeof(stLocal));
  iErr = dm_udp_getsockname(pstS->pstUdp, &stLocal);
  if (DM_SUCCESS == iErr)
  {
    *pu16Port = stLocal.u16Port;
  }
  return iErr;
}

static void af__in_signal_boot(dm_io af__in_boot_st *pstB, dm_in int iErr, dm_in uint16 u16Port)
{
  dm_mutex_lock(pstB->pstMtx);
  pstB->iErr = iErr;
  pstB->u16Port = u16Port;
  pstB->bDone = DM_TRUE;
  dm_cond_signal(pstB->pstCond);
  dm_mutex_unlock(pstB->pstMtx);
}

static void af__in_thread_entry(dm_io void *pvArg)
{
  af_input_sess_st *pstS = (af_input_sess_st *)pvArg;
  af__in_boot_st *pstBoot = pstS->pstBoot;
  uint16 u16Port = 0u;
  int iErr = dm_loop_create(&pstS->pstLoop);

  iErr = (DM_SUCCESS == iErr)
             ? dm_async_create(pstS->pstLoop, af__in_on_async, pstS, &pstS->pstAsync)
             : iErr;
  iErr = (DM_SUCCESS == iErr) ? dm_loop_timer_create(pstS->pstLoop, &pstS->pstWdTmr) : iErr;
  iErr = (DM_SUCCESS == iErr) ? af__in_bind_dual_stack(pstS, &u16Port) : iErr;
  iErr = (DM_SUCCESS == iErr) ? dm_udp_recv_start(pstS->pstUdp, af__in_on_udp_recv, pstS) : iErr;
  if (DM_SUCCESS != iErr)
  {
    af__in_close_loop_handles(pstS);
    if (NULL != pstS->pstLoop)
    {
      (void)dm_loop_run(pstS->pstLoop, DM_LOOP_RUN_DEFAULT);
      (void)dm_loop_destroy(pstS->pstLoop);
      pstS->pstLoop = NULL;
    }
    af__in_signal_boot(pstBoot, iErr, 0u);
    return;
  }

  af__in_signal_boot(pstBoot, DM_SUCCESS, u16Port);
  (void)dm_loop_run(pstS->pstLoop, DM_LOOP_RUN_DEFAULT);
  (void)dm_loop_destroy(pstS->pstLoop);
  pstS->pstLoop = NULL;
}

/* ============================ T_core 回调与模块生命周期 ============================ */

static int af__input_on_sec_tmr(dm_io void *pvArg)
{
  dm_link_msg_st *pstMsg = (dm_link_msg_st *)pvArg;
  af_input_sess_st *pstS = g_stAfInMod.pstSess;
  bool_t bCur = DM_FALSE;

  if ((NULL == pstS) || ((NULL != pstMsg) && (pstMsg->u64Tag != g_stAfInMod.u64SessGen)))
  {
    return DM_SUCCESS;
  }
  bCur = pstS->stOps.pfIsSecureInput();
  if (bCur != pstS->bLastSecInput)
  {
    af_ev_input_sec_st stEv;
    pstS->bLastSecInput = bCur;
    memset(&stEv, 0, sizeof(stEv));
    stEv.bSecureInput = bCur;
    (void)dm_event_publish(AF_EV_INPUT_SEC_CHANGED, &stEv, (uint32)sizeof(stEv));
    atomic_store_explicit(&pstS->bSecState, bCur, memory_order_release);
    atomic_store_explicit(&pstS->bSecSendPending, DM_TRUE, memory_order_release);
    af__in_wake_input(pstS, DM_FALSE);
  }
  return DM_SUCCESS;
}

static int af__input_on_wd_ev(dm_io void *pvArg)
{
  (void)pvArg;
  atomic_fetch_add_explicit(&g_stAfInMod.u32WdEvCnt, 1u, memory_order_relaxed);
  if (NULL != g_stAfInMod.pstSess)
  {
    DM_LOG_DEBUG("af_input watchdog triggered RELEASE_ALL");
  }
  return DM_SUCCESS;
}

static int af__input_on_disp_changed(dm_io void *pvArg)
{
  dm_link_msg_st *pstMsg = (dm_link_msg_st *)pvArg;
  af_input_sess_st *pstS = g_stAfInMod.pstSess;
  af_ev_display_st stEv;

  if ((NULL == pstS) || (NULL == pstMsg) || (NULL == pstMsg->pu8Data) || (pstMsg->u32Len < sizeof(stEv))) { return DM_SUCCESS; }
  memcpy(&stEv, pstMsg->pu8Data, sizeof(stEv));
  if (0u == stEv.u32VdDispId) { return DM_SUCCESS; }
  if ((stEv.u32VdDispId == pstS->u32DispId) && (0u != stEv.u32VdW) && (0u != stEv.u32VdH) &&
      (stEv.u32VdW == pstS->u32DispW) && (stEv.u32VdH == pstS->u32DispH))
  {
    return DM_SUCCESS;
  }
  pstS->u32DispId = stEv.u32VdDispId;
  pstS->u32DispW = stEv.u32VdW;
  pstS->u32DispH = stEv.u32VdH;
  atomic_store_explicit(&pstS->u32PendingDispId, stEv.u32VdDispId, memory_order_release);
  atomic_store_explicit(&pstS->bDispUpdPending, DM_TRUE, memory_order_release);
  af__in_wake_input(pstS, DM_FALSE);
  return DM_SUCCESS;
}

static void af__in_destroy_sess(dm_io af_input_sess_st *pstS)
{
  if (NULL == pstS)
  {
    return;
  }
  if (NULL != pstS->pstInputThread)
  {
    af__in_wake_input(pstS, DM_TRUE);
    (void)dm_thread_join(pstS->pstInputThread);
    pstS->pstInputThread = NULL;
  }
  af__inj_stop_and_fini(&pstS->stInj);
  if (NULL != pstS->pstCtlMtx)
  {
    dm_mutex_destroy(pstS->pstCtlMtx);
    pstS->pstCtlMtx = NULL;
  }
  if (pstS->bAeadReady)
  {
    mbedtls_chachapoly_free(&pstS->stRxAead);
    mbedtls_chachapoly_free(&pstS->stTxAead);
    pstS->bAeadReady = DM_FALSE;
  }
  dm_secure_zero(pstS, sizeof(*pstS));
  dm_free_tag(pstS);
}

static int af__in_init_aead(dm_io af_input_sess_st *pstS)
{
  mbedtls_chachapoly_init(&pstS->stRxAead);
  mbedtls_chachapoly_init(&pstS->stTxAead);
  pstS->bAeadReady = DM_TRUE;
  if ((0 != mbedtls_chachapoly_setkey(&pstS->stRxAead, pstS->au8KeyI2M)) ||
      (0 != mbedtls_chachapoly_setkey(&pstS->stTxAead, pstS->au8KeyM2I)))
  {
    return DM_FAILD;
  }
  return DM_SUCCESS;
}

static int af__in_launch_input_thread(dm_io af_input_sess_st *pstS)
{
  af__in_boot_st stBoot;
  dm_thread_opt_st stOpt;
  int iErr = DM_SUCCESS;

  memset(&stBoot, 0, sizeof(stBoot));
  if (DM_SUCCESS != dm_mutex_create(&stBoot.pstMtx))
  {
    return DM_ALLOC;
  }
  if (DM_SUCCESS != dm_cond_create(&stBoot.pstCond))
  {
    dm_mutex_destroy(stBoot.pstMtx);
    return DM_ALLOC;
  }
  pstS->pstBoot = &stBoot;
  memset(&stOpt, 0, sizeof(stOpt));
  stOpt.ePriority = DM_THREAD_PRIO_HIGHEST;
  stOpt.bSetPriority = DM_TRUE;
  stOpt.pcName = "af_input";

  iErr = dm_thread_create_ex(&stOpt, af__in_thread_entry, pstS, &pstS->pstInputThread);
  if (DM_SUCCESS == iErr)
  {
    dm_mutex_lock(stBoot.pstMtx);
    while (!stBoot.bDone)
    {
      dm_cond_wait(stBoot.pstCond, stBoot.pstMtx);
    }
    iErr = stBoot.iErr;
    pstS->u16Port = stBoot.u16Port;
    dm_mutex_unlock(stBoot.pstMtx);
  }
  pstS->pstBoot = NULL;
  dm_cond_destroy(stBoot.pstCond);
  dm_mutex_destroy(stBoot.pstMtx);
  if ((DM_SUCCESS != iErr) && (NULL != pstS->pstInputThread))
  {
    (void)dm_thread_join(pstS->pstInputThread);
    pstS->pstInputThread = NULL;
  }
  return iErr;
}

/* ============================ 公共 API ============================ */

void af_input_set_ops(dm_in const af_input_ops_st *pstOps)
{
  if (NULL == pstOps)
  {
    memset(&g_stAfInMod.stUserOps, 0, sizeof(g_stAfInMod.stUserOps));
    return;
  }
  g_stAfInMod.stUserOps = *pstOps;
}

int af_input_start(dm_in const af_input_start_req_st *pstReq, dm_out uint16 *pu16InputPort)
{
  af_input_sess_st *pstS = NULL;
  int iErr = DM_SUCCESS;

  if ((NULL == pstReq) || (NULL == pu16InputPort))
  {
    return DM_INVALID;
  }
  *pu16InputPort = 0u;
  if (NULL != g_stAfInMod.pstSess)
  {
    return DM_BUSY;
  }
  pstS = (af_input_sess_st *)dm_calloc_tag(1u, sizeof(*pstS), AF_TAG_INPUT);
  if (NULL == pstS)
  {
    return DM_ALLOC;
  }
  memcpy(pstS->au8SidBe, pstReq->au8SidBe, sizeof(pstS->au8SidBe));
  memcpy(pstS->au8KeyI2M, pstReq->au8KeyI2M, sizeof(pstS->au8KeyI2M));
  memcpy(pstS->au8KeyM2I, pstReq->au8KeyM2I, sizeof(pstS->au8KeyM2I));
  pstS->u32DispId = pstReq->u32DispId;
  pstS->u64M2iSeq = 1ull;
  af__in_resolve_ops(&g_stAfInMod.stUserOps, &pstS->stOps);
  af_dp_replay_init(&pstS->stReplay);
  af_dp_cum_init(&pstS->stCum);
  af_dp_rx_dedup_init(&pstS->stDedup);
  atomic_init(&pstS->bDispUpdPending, DM_FALSE);
  atomic_init(&pstS->u32PendingDispId, 0u);
  atomic_init(&pstS->bSecSendPending, DM_FALSE);
  atomic_init(&pstS->bSecState, DM_FALSE);

  iErr = dm_mutex_create(&pstS->pstCtlMtx);
  iErr = (DM_SUCCESS == iErr) ? af__in_init_aead(pstS) : iErr;
  iErr = (DM_SUCCESS == iErr) ? af__inj_init(&pstS->stInj, &pstS->stOps, pstS->u32DispId) : iErr;
  iErr = (DM_SUCCESS == iErr) ? af__inj_start_thread(&pstS->stInj) : iErr;
  iErr = (DM_SUCCESS == iErr) ? af__in_launch_input_thread(pstS) : iErr;
  if (DM_SUCCESS != iErr)
  {
    af__in_destroy_sess(pstS);
    return iErr;
  }

  pstS->bLastSecInput = pstS->stOps.pfIsSecureInput();
  g_stAfInMod.u64SessGen++;
  g_stAfInMod.pstSess = pstS;
  if (g_stAfInMod.bInited)
  {
    (void)dm_timer_add(pstS->stOps.u32SecPollMs, pstS->stOps.u32SecPollMs, AF_LINK_INPUT_SEC_TMR,
                       g_stAfInMod.u64SessGen, &g_stAfInMod.u32SecTimer);
  }
  *pu16InputPort = pstS->u16Port;
  return DM_SUCCESS;
}

void af_input_force_stop(void)
{
  af_input_sess_st *pstS = g_stAfInMod.pstSess;

  if (0u != g_stAfInMod.u32SecTimer)
  {
    (void)dm_timer_cancel(g_stAfInMod.u32SecTimer);
    g_stAfInMod.u32SecTimer = 0u;
  }
  if (NULL == pstS)
  {
    return;
  }
  g_stAfInMod.pstSess = NULL;
  af__in_destroy_sess(pstS);
}

bool_t af_input_is_active(void)
{
  return (NULL != g_stAfInMod.pstSess) ? DM_TRUE : DM_FALSE;
}

uint32 af__input_wd_ev_count(void)
{
  return atomic_load_explicit(&g_stAfInMod.u32WdEvCnt, memory_order_acquire);
}

void af__input_reset_wd_ev_count(void)
{
  atomic_store_explicit(&g_stAfInMod.u32WdEvCnt, 0u, memory_order_release);
}

/* ============================ DM 模块注册 ============================ */

static int af__input_mod_init(void)
{
  int iErr = DM_SUCCESS;

  dm_mem_tag_name(AF_TAG_INPUT, "af_input");
  iErr = dm_link_register_handler(AF_LINK_INPUT_SEC_TMR, af__input_on_sec_tmr, NULL, NULL);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr = dm_link_register_handler(AF_LINK_INPUT_WD_EV, af__input_on_wd_ev, NULL, NULL);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr = dm_event_subscribe(AF_EV_DISPLAY_CHANGED, af__input_on_disp_changed, NULL, NULL);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  g_stAfInMod.bInited = DM_TRUE;
  return DM_SUCCESS;
}

static void af__input_mod_drain(dm_io dm_drain_st *pstDrain)
{
  (void)pstDrain;
  af_input_force_stop();
}

static void af__input_mod_fini(void)
{
  af_input_force_stop();
  if (g_stAfInMod.bInited)
  {
    (void)dm_event_unsubscribe(AF_EV_DISPLAY_CHANGED, af__input_on_disp_changed);
    g_stAfInMod.bInited = DM_FALSE;
  }
}

const dm_module_st g_stAfInputMod = {
    .u16ModID = AF_MOD_INPUT,
    .pcName = "af_input",
    .pfInit = af__input_mod_init,
    .pfDrain = af__input_mod_drain,
    .pfFini = af__input_mod_fini,
    .pastErrMap = g_astAfInputErrs,
};

const dm_module_st *af_input_module(void)
{
  return &g_stAfInputMod;
}

#ifdef __cplusplus
}
#endif
