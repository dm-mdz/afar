/**
 ******************************************************************************
 * @file    af_stream_tx.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   独立视频推流线程 T_video（UDP ChaCha20-Poly1305 + 交错 XOR FEC + 平滑 Pacing）。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "stream/af_stream_int.h"

#include <string.h>

typedef struct af__tx_boot
{
  dm_mutex_st *pstMtx;
  dm_cond_st *pstCond;
  bool_t bDone;
  int iErr;
  uint16 u16Port;
} af__tx_boot_st;

af__stream_tx_st g_stAfTx;

static void af__tx_pump(void);

static void af__tx_post_core(dm_in af__stream_cb_kind_et eKind, dm_in int iReason)
{
  af__stream_cb_msg_st stMsg = {eKind, g_stAfTx.u64Gen, iReason};
  (void)dm_queue_post_async(AF_LINK_STREAM_STATE_CB, &stMsg, (uint32)sizeof(stMsg));
}

static void af__tx_free_cur(void)
{
  dm_free_tag(g_stAfTx.stCur.pu8Payload);
  memset(&g_stAfTx.stCur, 0, sizeof(g_stAfTx.stCur));
  atomic_store(&g_stAfTx.bCurActive, DM_FALSE);
}

static void af__tx_free_all_pending(void)
{
  af__tx_frame_st astDump[AF_STREAM_PENDING_SLOTS];
  uint32 u32Cnt = 0u;

  dm_mutex_lock(g_stAfTx.pstMtx);
  u32Cnt = g_stAfTx.u32PendingCnt;
  memcpy(astDump, g_stAfTx.astPending, sizeof(astDump));
  memset(g_stAfTx.astPending, 0, sizeof(g_stAfTx.astPending));
  g_stAfTx.u32PendingCnt = 0u;
  dm_mutex_unlock(g_stAfTx.pstMtx);
  for (uint32 i = 0u; i < u32Cnt; i++)
  {
    dm_free_tag(astDump[i].pu8Data);
  }
}

static bool_t af__tx_peer_eq(dm_in const dm_sockaddr_st *pstA, dm_in const dm_sockaddr_st *pstB)
{
  if ((pstA->eFamily != pstB->eFamily) || (pstA->u16Port != pstB->u16Port))
  {
    return DM_FALSE;
  }
  if (DM_AF_IPV4 == pstA->eFamily)
  {
    return (0 == memcmp(pstA->au8Addr, pstB->au8Addr, 4u)) ? DM_TRUE : DM_FALSE;
  }
  return ((pstA->u32ScopeId == pstB->u32ScopeId) &&
          (0 == memcmp(pstA->au8Addr, pstB->au8Addr, 16u)))
             ? DM_TRUE
             : DM_FALSE;
}

static void af__tx_drop_peer(dm_in int iReason)
{
  if (NULL != g_stAfTx.pstPeerTimer)
  {
    (void)dm_loop_timer_stop(g_stAfTx.pstPeerTimer);
  }
  if (NULL != g_stAfTx.pstPaceTimer)
  {
    (void)dm_loop_timer_stop(g_stAfTx.pstPaceTimer);
    g_stAfTx.bPaceArmed = DM_FALSE;
  }
  g_stAfTx.bHasPeer = DM_FALSE;
  atomic_store(&g_stAfTx.bHasPeerAtomic, DM_FALSE);
  af__tx_free_cur();
  /* 严守 AUD-V21-P1-10：绝不自关 pstAsync，由 T_core 排空采集后统一停机 */
  if (!g_stAfTx.bDroppedNotified)
  {
    g_stAfTx.bDroppedNotified = DM_TRUE;
    af__tx_post_core(AF_ST_EV_TX_DROPPED, iReason);
  }
}

/* ============================ 动态码率调控 ============================ */

static void af__tx_br_reset_on_auth(void)
{
  g_stAfTx.u32CurBitrateBps = AF_STREAM_BR_DEFAULT_BPS;
  g_stAfTx.u64BrLastEvalNs = 0u;
  g_stAfTx.u64BrSteadyStartNs = 0u;
  g_stAfTx.u64BrLastDropSnap = atomic_load(&g_stAfTx.u64DroppedPCnt);
}

static uint32 af__tx_br_next_down(dm_in uint32 u32Cur)
{
  if (u32Cur > 8000000u) { return 8000000u; }
  if (u32Cur > 6000000u) { return 6000000u; }
  return AF_STREAM_BR_MIN_BPS;
}

static void af__tx_br_eval(dm_in uint32 u32LossPermille)
{
  uint64 u64NowNs = dm_hrtime();
  uint64 u64DropNow = atomic_load(&g_stAfTx.u64DroppedPCnt);
  bool_t bHasDrop = (u64DropNow > g_stAfTx.u64BrLastDropSnap) ? DM_TRUE : DM_FALSE;
  uint32 u32Next = g_stAfTx.u32CurBitrateBps;

  if (bHasDrop || (u32LossPermille >= AF_STREAM_BR_LOSS_DOWN_PM))
  {
    g_stAfTx.u64BrSteadyStartNs = 0u;
    if ((0u == g_stAfTx.u64BrLastEvalNs) ||
        ((u64NowNs - g_stAfTx.u64BrLastEvalNs) >= AF_STREAM_BR_EVAL_NS))
    {
      g_stAfTx.u64BrLastEvalNs = u64NowNs;
      g_stAfTx.u64BrLastDropSnap = u64DropNow;
      u32Next = af__tx_br_next_down(g_stAfTx.u32CurBitrateBps);
      if (u32Next != g_stAfTx.u32CurBitrateBps)
      {
        g_stAfTx.u32CurBitrateBps = u32Next;
        af__tx_post_core(AF_ST_EV_SET_BITRATE, (int)u32Next);
      }
    }
    return;
  }
  if (0u != u32LossPermille)
  {
    g_stAfTx.u64BrSteadyStartNs = 0u;
    return;
  }
  if (0u == g_stAfTx.u64BrSteadyStartNs)
  {
    g_stAfTx.u64BrSteadyStartNs = u64NowNs;
    return;
  }
  if (((u64NowNs - g_stAfTx.u64BrSteadyStartNs) >= AF_STREAM_BR_STEADY_NS) &&
      (g_stAfTx.u32CurBitrateBps < AF_STREAM_BR_DEFAULT_BPS))
  {
    u32Next = g_stAfTx.u32CurBitrateBps + AF_STREAM_BR_STEP_UP_BPS;
    u32Next = (u32Next > AF_STREAM_BR_DEFAULT_BPS) ? AF_STREAM_BR_DEFAULT_BPS : u32Next;
    g_stAfTx.u32CurBitrateBps = u32Next;
    g_stAfTx.u64BrSteadyStartNs = u64NowNs;
    g_stAfTx.u64BrLastEvalNs = u64NowNs;
    af__tx_post_core(AF_ST_EV_SET_BITRATE, (int)u32Next);
  }
}

/* ============================ UDP 认证与控制收包 ============================ */

static void af__tx_on_sess_timeout(dm_io dm_loop_timer_st *pstTmr, dm_io void *pvUser)
{
  (void)pstTmr; (void)pvUser;
  if (!g_stAfTx.bHasPeer) { af__tx_drop_peer(DM_TIMEOUT); }
}

static void af__tx_on_peer_timeout(dm_io dm_loop_timer_st *pstTmr, dm_io void *pvUser)
{
  (void)pstTmr; (void)pvUser;
  if (g_stAfTx.bHasPeer) { af__tx_drop_peer(DM_TIMEOUT); }
}

static void af__tx_on_udp_recv(dm_io dm_udp_st *pstUdp, dm_in int iErr,
                               dm_in const dm_udp_rx_st *pstRx, dm_io void *pvUser)
{
  uint8 au8Sid[8];
  uint8 au8Nonce[AF_DP_AEAD_NONCE_LEN];
  uint8 au8Plain[AF_DP_VID_CTL_LEN];
  af_dp_vid_ctl_t stCtl;
  uint64 u64PktSeq = 0u;
  int iIsNewest = 0;

  (void)pstUdp;
  (void)pvUser;
  if ((DM_SUCCESS != iErr) || (NULL == pstRx) || (!g_stAfTx.bAeadReady) ||
      (AF_STREAM_CTL_WIRE_LEN != pstRx->uLen) ||
      (AF_DP_OK != af_dp_udp_outer_decode(au8Sid, &u64PktSeq, pstRx->pu8Data)) ||
      (!dm_crypto_const_eq(au8Sid, g_stAfTx.au8SidBe, sizeof(au8Sid))))
  {
    return;
  }
  if (g_stAfTx.bHasPeer && (!af__tx_peer_eq(&g_stAfTx.stPeerAddr, &pstRx->stPeer)))
  {
    return;
  }
  af_dp_aead_nonce(au8Nonce, AF_DP_DIR_VID_I2M, u64PktSeq);
  if (0 != mbedtls_chachapoly_auth_decrypt(
               &g_stAfTx.stAead, AF_DP_VID_CTL_LEN, au8Nonce, pstRx->pu8Data, AF_DP_UDP_OUTER_LEN,
               pstRx->pu8Data + AF_DP_UDP_OUTER_LEN + AF_DP_VID_CTL_LEN,
               pstRx->pu8Data + AF_DP_UDP_OUTER_LEN, au8Plain))
  {
    return;
  }
  if ((AF_DP_OK != af_dp_replay_check(&g_stAfTx.stRxReplay, u64PktSeq, &iIsNewest)) ||
      (AF_DP_OK != af_dp_vid_ctl_decode(&stCtl, au8Plain, sizeof(au8Plain))))
  {
    return;
  }
  if (!g_stAfTx.bHasPeer)
  {
    if (AF_DP_VID_CTL_HELLO != stCtl.type)
    {
      return;
    }
    g_stAfTx.stPeerAddr = pstRx->stPeer;
    g_stAfTx.bHasPeer = DM_TRUE;
    atomic_store(&g_stAfTx.bHasPeerAtomic, DM_TRUE);
    if (NULL != g_stAfTx.pstSessTimer)
    {
      (void)dm_loop_timer_stop(g_stAfTx.pstSessTimer);
    }
    if (NULL != g_stAfTx.pstPeerTimer)
    {
      (void)dm_loop_timer_start(g_stAfTx.pstPeerTimer, g_stAfTx.u32PeerTimeoutMs, 0u,
                                af__tx_on_peer_timeout, NULL);
    }
    af__tx_br_reset_on_auth();
    af__tx_post_core(AF_ST_EV_CLIENT_AUTHED, DM_SUCCESS);
    af__tx_pump();
    return;
  }
  if (AF_DP_VID_CTL_BYE == stCtl.type)
  {
    af__tx_drop_peer(DM_EOF);
    return;
  }
  if (NULL != g_stAfTx.pstPeerTimer)
  {
    (void)dm_loop_timer_start(g_stAfTx.pstPeerTimer, g_stAfTx.u32PeerTimeoutMs, 0u,
                              af__tx_on_peer_timeout, NULL);
  }
  af__tx_br_eval(stCtl.loss_permille);
}

/* ============================ 分片、XOR FEC 与 1ms Pacing 泵 ============================ */

static uint16 af__tx_calc_batch(dm_in uint16 u16Total)
{
  uint32 u32B = 0u;
  if (u16Total <= 6u) { return u16Total; }
  u32B = (u16Total <= 24u) ? (((uint32)u16Total + 2u) / 3u) : (((uint32)u16Total + 5u) / 6u);
  if (u32B > 32u) { u32B = 32u; }
  if (u32B < 4u) { u32B = 4u; }
  return (uint16)u32B;
}

static bool_t af__tx_pull_next(void)
{
  for (;;)
  {
    af__tx_frame_st stF;
    af_dp_vid_tx_plan_t stPlan;
    bool_t bGot = DM_FALSE;

    dm_mutex_lock(g_stAfTx.pstMtx);
    if (g_stAfTx.bStop || (!g_stAfTx.bGenValid) || (0u == g_stAfTx.u32PendingCnt))
    {
      dm_mutex_unlock(g_stAfTx.pstMtx);
      return DM_FALSE;
    }
    stF = g_stAfTx.astPending[0];
    g_stAfTx.astPending[0] = g_stAfTx.astPending[1];
    g_stAfTx.u32PendingCnt--;
    memset(&g_stAfTx.astPending[g_stAfTx.u32PendingCnt], 0, sizeof(af__tx_frame_st));
    bGot = (stF.u64Gen == g_stAfTx.u64Gen) ? DM_TRUE : DM_FALSE;
    dm_mutex_unlock(g_stAfTx.pstMtx);
    if (!bGot)
    {
      dm_free_tag(stF.pu8Data);
      continue;
    }
    g_stAfTx.u32FrameSeq = (0u == (++g_stAfTx.u32FrameSeq)) ? 1u : g_stAfTx.u32FrameSeq;
    af_dp_vid_tx_plan(stF.u32Len, stF.u32Flags, &stPlan);
    g_stAfTx.stCur.bActive = DM_TRUE;
    g_stAfTx.stCur.stHdr = (af_dp_vid_hdr_t){AF_DP_VID_MAGIC, g_stAfTx.u32FrameSeq, stF.u32Flags,
                                             stF.u32Len, stF.u64CaptureUs};
    g_stAfTx.stCur.pu8Payload = stF.pu8Data;
    g_stAfTx.stCur.u16TotalPkts = stPlan.total_cnt;
    g_stAfTx.stCur.u16NextPkt = 0u;
    g_stAfTx.stCur.u16BatchPkts = af__tx_calc_batch(stPlan.total_cnt);
    atomic_store(&g_stAfTx.bCurActive, DM_TRUE);
    return DM_TRUE;
  }
}

static int af__tx_send_one_chunk(dm_in uint16 u16PktIdx)
{
  uint8 au8Plain[AF_DP_VID_PLAIN_MAX];
  uint8 au8Wire[AF_DP_VID_UDP_MTU];
  uint8 au8Nonce[AF_DP_AEAD_NONCE_LEN];
  size_t uPlainLen = 0u;
  uint64 u64Seq = 0u;
  int iErr = DM_SUCCESS;

  if (g_stAfTx.u64TxPktSeq >= AF_DP_SEQ_MAX_SAFE)
  {
    return DM_TOO_BIG;
  }
  if (AF_DP_OK != af_dp_vid_tx_build_chunk(&g_stAfTx.stCur.stHdr, g_stAfTx.stCur.pu8Payload,
                                           u16PktIdx, au8Plain, &uPlainLen))
  {
    return DM_INVALID;
  }
  u64Seq = g_stAfTx.u64TxPktSeq;
  if (AF_DP_OK != af_dp_udp_outer_encode(au8Wire, g_stAfTx.au8SidBe, u64Seq))
  {
    return DM_INVALID;
  }
  af_dp_aead_nonce(au8Nonce, AF_DP_DIR_VID_M2I, u64Seq);
  if (0 != mbedtls_chachapoly_encrypt_and_tag(
               &g_stAfTx.stAead, uPlainLen, au8Nonce, au8Wire, AF_DP_UDP_OUTER_LEN, au8Plain,
               au8Wire + AF_DP_UDP_OUTER_LEN, au8Wire + AF_DP_UDP_OUTER_LEN + uPlainLen))
  {
    return DM_FAILD;
  }
  iErr = dm_udp_try_send(g_stAfTx.pstUdp, au8Wire,
                         AF_DP_UDP_OUTER_LEN + uPlainLen + AF_DP_AEAD_TAG_LEN,
                         &g_stAfTx.stPeerAddr);
  if (DM_SUCCESS == iErr)
  {
    g_stAfTx.u64TxPktSeq++;
    atomic_fetch_add(&g_stAfTx.u64TxPktCnt, 1u);
  }
  return iErr;
}

static void af__tx_on_pace(dm_io dm_loop_timer_st *pstTmr, dm_io void *pvUser)
{
  (void)pstTmr; (void)pvUser;
  g_stAfTx.bPaceArmed = DM_FALSE;
  af__tx_pump();
}

static void af__tx_arm_pace(void)
{
  if ((!g_stAfTx.bPaceArmed) && (NULL != g_stAfTx.pstPaceTimer))
  {
    g_stAfTx.bPaceArmed = DM_TRUE;
    (void)dm_loop_timer_start(g_stAfTx.pstPaceTimer, 1u, 0u, af__tx_on_pace, NULL);
  }
}

static void af__tx_pump(void)
{
  while (g_stAfTx.bHasPeer && (NULL != g_stAfTx.pstUdp))
  {
    uint16 u16Sent = 0u;

    if ((!g_stAfTx.stCur.bActive) && (!af__tx_pull_next()))
    {
      return;
    }
    while ((g_stAfTx.stCur.u16NextPkt < g_stAfTx.stCur.u16TotalPkts) &&
           (u16Sent < g_stAfTx.stCur.u16BatchPkts))
    {
      int iErr = af__tx_send_one_chunk(g_stAfTx.stCur.u16NextPkt);
      if (DM_AGAIN == iErr)
      {
        atomic_fetch_add(&g_stAfTx.u64SendAgainCnt, 1u);
        af__tx_arm_pace();
        return;
      }
      if (DM_TOO_BIG == iErr)
      {
        af__tx_drop_peer(DM_TOO_BIG);
        return;
      }
      g_stAfTx.stCur.u16NextPkt++;
      u16Sent++;
    }
    if (g_stAfTx.stCur.u16NextPkt < g_stAfTx.stCur.u16TotalPkts)
    {
      af__tx_arm_pace();
      return;
    }
    af__tx_free_cur();
  }
}

/* ============================ 循环句柄与线程生命周期 ============================ */

static void af__tx_send_bye(void)
{
  uint8 au8Plain[AF_DP_VID_CTL_LEN];
  uint8 au8Wire[AF_STREAM_CTL_WIRE_LEN];
  uint8 au8Nonce[AF_DP_AEAD_NONCE_LEN];
  af_dp_vid_ctl_t stBye = {AF_DP_VID_CTL_BYE, 0u};

  if ((!g_stAfTx.bHasPeer) || (!g_stAfTx.bAeadReady) || (NULL == g_stAfTx.pstUdp) ||
      (g_stAfTx.u64TxPktSeq >= AF_DP_SEQ_MAX_SAFE))
  {
    return;
  }
  if (AF_DP_OK != af_dp_vid_ctl_encode(au8Plain, &stBye))
  {
    return;
  }
  for (uint32 i = 0u; i < 2u; i++)
  {
    uint64 u64Seq = g_stAfTx.u64TxPktSeq++;
    if (AF_DP_OK != af_dp_udp_outer_encode(au8Wire, g_stAfTx.au8SidBe, u64Seq))
    {
      return;
    }
    af_dp_aead_nonce(au8Nonce, AF_DP_DIR_VID_M2I, u64Seq);
    if (0 == mbedtls_chachapoly_encrypt_and_tag(
                 &g_stAfTx.stAead, sizeof(au8Plain), au8Nonce, au8Wire, AF_DP_UDP_OUTER_LEN,
                 au8Plain, au8Wire + AF_DP_UDP_OUTER_LEN,
                 au8Wire + AF_DP_UDP_OUTER_LEN + sizeof(au8Plain)))
    {
      (void)dm_udp_try_send(g_stAfTx.pstUdp, au8Wire, sizeof(au8Wire), &g_stAfTx.stPeerAddr);
    }
  }
}

static void af__tx_close_loop_handles(void)
{
  dm_async_st *pstClose = NULL;

  af__tx_send_bye();
  if (NULL != g_stAfTx.pstUdp)
  {
    (void)dm_udp_recv_stop(g_stAfTx.pstUdp);
    (void)dm_udp_close(g_stAfTx.pstUdp, NULL, NULL);
    g_stAfTx.pstUdp = NULL;
  }
  if (NULL != g_stAfTx.pstSessTimer) { (void)dm_loop_timer_close(g_stAfTx.pstSessTimer, NULL, NULL); g_stAfTx.pstSessTimer = NULL; }
  if (NULL != g_stAfTx.pstPeerTimer) { (void)dm_loop_timer_close(g_stAfTx.pstPeerTimer, NULL, NULL); g_stAfTx.pstPeerTimer = NULL; }
  if (NULL != g_stAfTx.pstPaceTimer) { (void)dm_loop_timer_close(g_stAfTx.pstPaceTimer, NULL, NULL); g_stAfTx.pstPaceTimer = NULL; g_stAfTx.bPaceArmed = DM_FALSE; }
  af__tx_free_cur();
  af__tx_free_all_pending();
  dm_mutex_lock(g_stAfTx.pstMtx);
  pstClose = g_stAfTx.pstAsync;
  g_stAfTx.pstAsync = NULL;
  dm_mutex_unlock(g_stAfTx.pstMtx);
  if (NULL != pstClose) { (void)dm_async_close(pstClose, NULL, NULL); }
}

static void af__tx_on_async(dm_io dm_async_st *pstAsync, dm_io void *pvUser)
{
  bool_t bStop = DM_FALSE;

  (void)pstAsync;
  (void)pvUser;
  dm_mutex_lock(g_stAfTx.pstMtx);
  bStop = g_stAfTx.bStop;
  dm_mutex_unlock(g_stAfTx.pstMtx);
  if (bStop)
  {
    af__tx_close_loop_handles();
    return;
  }
  if (g_stAfTx.bHasPeer)
  {
    af__tx_br_eval(0u);
    if (!g_stAfTx.bPaceArmed)
    {
      af__tx_pump();
    }
  }
}

static int af__tx_bind_udp(dm_out uint16 *pu16Port)
{
  dm_sockaddr_st stBind;
  dm_sockaddr_st stLocal;
  int iErr = dm_udp_create(g_stAfTx.pstLoop, &g_stAfTx.pstUdp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr = (DM_SUCCESS == dm_sockaddr_ip6("::", 0u, &stBind))
             ? dm_udp_bind(g_stAfTx.pstUdp, &stBind, DM_UDP_F_NONE)
             : DM_FAILD;
  if (DM_SUCCESS != iErr)
  {
    (void)dm_udp_close(g_stAfTx.pstUdp, NULL, NULL);
    g_stAfTx.pstUdp = NULL;
    iErr = dm_udp_create(g_stAfTx.pstLoop, &g_stAfTx.pstUdp);
    if ((DM_SUCCESS == iErr) && (DM_SUCCESS == dm_sockaddr_ip4("0.0.0.0", 0u, &stBind)))
    {
      iErr = dm_udp_bind(g_stAfTx.pstUdp, &stBind, DM_UDP_F_NONE);
    }
  }
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  memset(&stLocal, 0, sizeof(stLocal));
  iErr = dm_udp_getsockname(g_stAfTx.pstUdp, &stLocal);
  if (DM_SUCCESS == iErr)
  {
    *pu16Port = stLocal.u16Port;
  }
  return iErr;
}

static void af__tx_thread_entry(dm_io void *pvArg)
{
  af__tx_boot_st *pstBoot = (af__tx_boot_st *)pvArg;
  uint16 u16Port = 0u;
  int iErr = DM_SUCCESS;

  mbedtls_chachapoly_init(&g_stAfTx.stAead);
  g_stAfTx.bAeadReady = DM_TRUE;
  if (0 != mbedtls_chachapoly_setkey(&g_stAfTx.stAead, g_stAfTx.au8VidAuthKey))
  {
    iErr = DM_FAILD;
  }
  iErr = (DM_SUCCESS == iErr) ? dm_loop_create(&g_stAfTx.pstLoop) : iErr;
  iErr = (DM_SUCCESS == iErr)
             ? dm_async_create(g_stAfTx.pstLoop, af__tx_on_async, NULL, &g_stAfTx.pstAsync)
             : iErr;
  iErr = (DM_SUCCESS == iErr) ? dm_loop_timer_create(g_stAfTx.pstLoop, &g_stAfTx.pstSessTimer)
                              : iErr;
  iErr = (DM_SUCCESS == iErr) ? dm_loop_timer_create(g_stAfTx.pstLoop, &g_stAfTx.pstPeerTimer)
                              : iErr;
  iErr = (DM_SUCCESS == iErr) ? dm_loop_timer_create(g_stAfTx.pstLoop, &g_stAfTx.pstPaceTimer)
                              : iErr;
  iErr = (DM_SUCCESS == iErr) ? af__tx_bind_udp(&u16Port) : iErr;
  iErr = (DM_SUCCESS == iErr) ? dm_udp_recv_start(g_stAfTx.pstUdp, af__tx_on_udp_recv, NULL) : iErr;
  if (DM_SUCCESS == iErr)
  {
    (void)dm_loop_timer_start(g_stAfTx.pstSessTimer, g_stAfTx.u32AuthTimeoutMs, 0u,
                              af__tx_on_sess_timeout, NULL);
    atomic_store(&g_stAfTx.bLoopAlive, DM_TRUE);
  }
  else
  {
    af__tx_close_loop_handles();
    if (NULL != g_stAfTx.pstLoop)
    {
      (void)dm_loop_run(g_stAfTx.pstLoop, DM_LOOP_RUN_DEFAULT);
    }
  }
  dm_mutex_lock(pstBoot->pstMtx);
  pstBoot->iErr = iErr;
  pstBoot->u16Port = u16Port;
  pstBoot->bDone = DM_TRUE;
  dm_cond_signal(pstBoot->pstCond);
  dm_mutex_unlock(pstBoot->pstMtx);
  if (DM_SUCCESS == iErr)
  {
    (void)dm_loop_run(g_stAfTx.pstLoop, DM_LOOP_RUN_DEFAULT);
    atomic_store(&g_stAfTx.bLoopAlive, DM_FALSE);
    af__tx_free_cur();
    af__tx_free_all_pending();
  }
  if (g_stAfTx.bAeadReady)
  {
    mbedtls_chachapoly_free(&g_stAfTx.stAead);
    g_stAfTx.bAeadReady = DM_FALSE;
  }
  dm_secure_zero(g_stAfTx.au8SidBe, sizeof(g_stAfTx.au8SidBe));
  dm_secure_zero(g_stAfTx.au8VidAuthKey, sizeof(g_stAfTx.au8VidAuthKey));
  if (NULL != g_stAfTx.pstLoop)
  {
    (void)dm_loop_destroy(g_stAfTx.pstLoop);
    g_stAfTx.pstLoop = NULL;
  }
}

/* ============================ 2 槽邮箱投递与停机 API ============================ */

static bool_t af__tx_is_p(dm_in uint32 u32Flags)
{
  return (0u == (u32Flags & (AF_VFLAG_KEYFRAME | AF_VFLAG_CONFIG))) ? DM_TRUE : DM_FALSE;
}

static void af__tx_enqueue_locked(dm_in const af_stream_enc_frame_st *pstFrame,
                                  dm_out uint8 *apu8Drop[2], dm_out bool_t *pbTriggerIdr)
{
  af__tx_frame_st stIn = {pstFrame->u64Gen, pstFrame->u32Flags, pstFrame->u64CaptureUs,
                          pstFrame->pu8Data, pstFrame->u32Len};
  bool_t bIsP = af__tx_is_p(stIn.u32Flags);
  bool_t bP0 = DM_FALSE;
  bool_t bP1 = DM_FALSE;

  apu8Drop[0] = NULL;
  apu8Drop[1] = NULL;
  *pbTriggerIdr = DM_FALSE;
  if (bIsP && g_stAfTx.bWaitIdr)
  {
    apu8Drop[0] = stIn.pu8Data;
    *pbTriggerIdr = DM_TRUE;
    atomic_fetch_add(&g_stAfTx.u64DroppedPCnt, 1u);
    return;
  }
  if (0u != (stIn.u32Flags & AF_VFLAG_CONFIG))
  {
    g_stAfTx.bWaitIdr = DM_TRUE;
  }
  else if (0u != (stIn.u32Flags & AF_VFLAG_KEYFRAME))
  {
    g_stAfTx.bWaitIdr = DM_FALSE;
  }
  if (g_stAfTx.u32PendingCnt < AF_STREAM_PENDING_SLOTS)
  {
    g_stAfTx.astPending[g_stAfTx.u32PendingCnt++] = stIn;
    return;
  }
  if (bIsP)
  {
    apu8Drop[0] = stIn.pu8Data;
    g_stAfTx.bWaitIdr = DM_TRUE;
    *pbTriggerIdr = DM_TRUE;
    atomic_fetch_add(&g_stAfTx.u64DroppedPCnt, 1u);
    return;
  }
  bP0 = af__tx_is_p(g_stAfTx.astPending[0].u32Flags);
  bP1 = af__tx_is_p(g_stAfTx.astPending[1].u32Flags);
  if (0u != (stIn.u32Flags & AF_VFLAG_CONFIG))
  {
    apu8Drop[0] = g_stAfTx.astPending[0].pu8Data;
    apu8Drop[1] = g_stAfTx.astPending[1].pu8Data;
    g_stAfTx.astPending[0] = stIn;
    memset(&g_stAfTx.astPending[1], 0, sizeof(g_stAfTx.astPending[1]));
    g_stAfTx.u32PendingCnt = 1u;
    if (bP0 || bP1)
    {
      atomic_fetch_add(&g_stAfTx.u64DroppedPCnt, (uint64)(bP0 ? 1u : 0u) + (uint64)(bP1 ? 1u : 0u));
    }
    return;
  }
  if (bP0 && (!bP1))
  {
    apu8Drop[0] = g_stAfTx.astPending[0].pu8Data;
    g_stAfTx.astPending[0] = g_stAfTx.astPending[1];
    g_stAfTx.astPending[1] = stIn;
    atomic_fetch_add(&g_stAfTx.u64DroppedPCnt, 1u);
    return;
  }
  apu8Drop[0] = g_stAfTx.astPending[1].pu8Data;
  atomic_fetch_add(&g_stAfTx.u64DroppedPCnt, bP1 ? 1u : 0u);
  g_stAfTx.astPending[1] = stIn;
}

void af__stream_tx_submit(dm_in const af_stream_enc_frame_st *pstFrame)
{
  uint8 *apu8Drop[2] = {NULL, NULL};
  bool_t bTriggerIdr = DM_FALSE;
  uint64 u64Gen = 0u;

  if ((NULL == pstFrame) || (NULL == g_stAfTx.pstMtx))
  {
    if (NULL != pstFrame)
    {
      dm_free_tag(pstFrame->pu8Data);
    }
    return;
  }
  dm_mutex_lock(g_stAfTx.pstMtx);
  if ((!g_stAfTx.bRunning) || g_stAfTx.bStop || (!g_stAfTx.bGenValid) ||
      (pstFrame->u64Gen != g_stAfTx.u64Gen) || (NULL == g_stAfTx.pstAsync))
  {
    apu8Drop[0] = pstFrame->pu8Data;
  }
  else
  {
    u64Gen = g_stAfTx.u64Gen;
    af__tx_enqueue_locked(pstFrame, apu8Drop, &bTriggerIdr);
    (void)dm_async_send(g_stAfTx.pstAsync);
  }
  dm_mutex_unlock(g_stAfTx.pstMtx);
  dm_free_tag(apu8Drop[0]);
  dm_free_tag(apu8Drop[1]);
  if (bTriggerIdr)
  {
    (void)dm_queue_post_async(AF_LINK_STREAM_REQ_IDR, &u64Gen, (uint32)sizeof(u64Gen));
  }
}

int af__stream_tx_start(dm_in const af__stream_tx_cfg_st *pstCfg, dm_out uint16 *pu16Port)
{
  af__tx_boot_st stBoot = {0};
  dm_thread_opt_st stOpt = {.pcName = "af_video"};
  int iErr = DM_ALLOC;

  memset(&g_stAfTx, 0, sizeof(g_stAfTx));
  g_stAfTx.u64Gen = pstCfg->u64Gen;
  g_stAfTx.u32AuthTimeoutMs = pstCfg->u32AuthTimeoutMs;
  g_stAfTx.u32PeerTimeoutMs = (pstCfg->u32AuthTimeoutMs < AF_STREAM_AUTH_TIMEOUT_MS)
                                  ? ((pstCfg->u32AuthTimeoutMs * 3u > 600u)
                                         ? (pstCfg->u32AuthTimeoutMs * 3u)
                                         : 600u)
                                  : AF_STREAM_PEER_TIMEOUT_MS;
  g_stAfTx.u64TxPktSeq = 1ull;
  g_stAfTx.u32CurBitrateBps = AF_STREAM_BR_DEFAULT_BPS;
  g_stAfTx.bGenValid = DM_TRUE;
  g_stAfTx.bRunning = DM_TRUE;
  af_dp_replay_init(&g_stAfTx.stRxReplay);
  memcpy(g_stAfTx.au8SidBe, pstCfg->au8SidBe, sizeof(g_stAfTx.au8SidBe));
  memcpy(g_stAfTx.au8VidAuthKey, pstCfg->au8VidAuthKey, sizeof(g_stAfTx.au8VidAuthKey));

  if ((DM_SUCCESS == dm_mutex_create(&g_stAfTx.pstMtx)) &&
      (DM_SUCCESS == dm_mutex_create(&stBoot.pstMtx)) &&
      (DM_SUCCESS == dm_cond_create(&stBoot.pstCond)) &&
      (DM_SUCCESS ==
       (iErr = dm_thread_create_ex(&stOpt, af__tx_thread_entry, &stBoot, &g_stAfTx.pstThread))))
  {
    dm_mutex_lock(stBoot.pstMtx);
    while (!stBoot.bDone)
    {
      dm_cond_wait(stBoot.pstCond, stBoot.pstMtx);
    }
    iErr = stBoot.iErr;
    *pu16Port = stBoot.u16Port;
    dm_mutex_unlock(stBoot.pstMtx);
  }
  dm_cond_destroy(stBoot.pstCond);
  dm_mutex_destroy(stBoot.pstMtx);
  if (DM_SUCCESS != iErr)
  {
    af__stream_tx_stop_join();
  }
  return iErr;
}

void af__stream_tx_invalidate_gen(void)
{
  if (NULL != g_stAfTx.pstMtx)
  {
    dm_mutex_lock(g_stAfTx.pstMtx);
    g_stAfTx.bGenValid = DM_FALSE;
    dm_mutex_unlock(g_stAfTx.pstMtx);
  }
}

void af__stream_tx_stop_join(void)
{
  if (NULL != g_stAfTx.pstMtx)
  {
    dm_mutex_lock(g_stAfTx.pstMtx);
    g_stAfTx.bGenValid = DM_FALSE;
    g_stAfTx.bRunning = DM_FALSE;
    if (!g_stAfTx.bStop)
    {
      g_stAfTx.bStop = DM_TRUE;
      if (NULL != g_stAfTx.pstAsync) { (void)dm_async_send(g_stAfTx.pstAsync); }
    }
    dm_mutex_unlock(g_stAfTx.pstMtx);
  }
  if (NULL != g_stAfTx.pstThread) { (void)dm_thread_join(g_stAfTx.pstThread); g_stAfTx.pstThread = NULL; }
  if (NULL != g_stAfTx.pstMtx) { dm_mutex_destroy(g_stAfTx.pstMtx); g_stAfTx.pstMtx = NULL; }
  dm_secure_zero(g_stAfTx.au8SidBe, sizeof(g_stAfTx.au8SidBe));
  dm_secure_zero(g_stAfTx.au8VidAuthKey, sizeof(g_stAfTx.au8VidAuthKey));
}

void af__stream_tx_get_stats(dm_out af__stream_tx_stats_st *pstOut)
{
  static const uint8 kZero[32] = {0};

  if (NULL != pstOut)
  {
    pstOut->u64SendAgainCnt = atomic_load(&g_stAfTx.u64SendAgainCnt);
    pstOut->u64DroppedPCnt = atomic_load(&g_stAfTx.u64DroppedPCnt);
    pstOut->u64TxPktCnt = atomic_load(&g_stAfTx.u64TxPktCnt);
    pstOut->bLoopAlive = atomic_load(&g_stAfTx.bLoopAlive);
    pstOut->bCurActive = atomic_load(&g_stAfTx.bCurActive);
    pstOut->bHasPeer = atomic_load(&g_stAfTx.bHasPeerAtomic);
    pstOut->bKeysZeroed =
        (dm_crypto_const_eq(g_stAfTx.au8SidBe, kZero, sizeof(g_stAfTx.au8SidBe)) &&
         dm_crypto_const_eq(g_stAfTx.au8VidAuthKey, kZero, sizeof(g_stAfTx.au8VidAuthKey)))
            ? DM_TRUE
            : DM_FALSE;
  }
}

#ifdef __cplusplus
}
#endif
