/**
 ******************************************************************************
 * @file    af_stream_int.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   服务层 af_stream（ModID 24）内部共享头（UDP AEAD + 交错 XOR FEC 推流）。
 ******************************************************************************
 */
#ifndef AF_STREAM_INT_H
#define AF_STREAM_INT_H

#ifndef DM_LOG_MOD
#define DM_LOG_MOD "stream"
#endif
#include <dm.h>
#include <dm/crypto.h>
#include <dm/event.h>
#include <dm/link.h>
#include <dm/log.h>
#include <dm/loop.h>
#include <dm/module.h>
#include <dm/sock.h>
#include <dm/thread.h>

#include "af_direct_proto.h"
#include "af_modules.h"
#include "stream/af_stream.h"

#include <mbedtls/chachapoly.h>

#include <stdatomic.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define AF_STREAM_AUTH_TIMEOUT_MS 10000u
#define AF_STREAM_PEER_TIMEOUT_MS 5000u
#define AF_STREAM_REQ_IDR_MIN_MS 200u
#define AF_STREAM_PENDING_SLOTS 2u
#define AF_STREAM_UDP_SNDBUF (1024u * 1024u)
#define AF_STREAM_UDP_RCVBUF (256u * 1024u)
#define AF_STREAM_CTL_WIRE_LEN (AF_DP_UDP_OUTER_LEN + AF_DP_VID_CTL_LEN + AF_DP_AEAD_TAG_LEN)

#define AF_STREAM_BR_DEFAULT_BPS 10000000u
#define AF_STREAM_BR_MIN_BPS 5000000u
#define AF_STREAM_BR_STEP_UP_BPS 1000000u
#define AF_STREAM_BR_LOSS_DOWN_PM 30u
#define AF_STREAM_BR_EVAL_NS (500ULL * 1000000ULL)
#define AF_STREAM_BR_STEADY_NS (3000ULL * 1000000ULL)

typedef enum af__stream_cb_kind
{
  AF_ST_EV_CLIENT_AUTHED = 1,
  AF_ST_EV_TX_DROPPED = 2,
  AF_ST_EV_CAP_STOPPED = 3,
  AF_ST_EV_CAP_STOP_DONE = 4,
  AF_ST_EV_SET_BITRATE = 5
} af__stream_cb_kind_et;

typedef struct af__stream_cb_msg
{
  af__stream_cb_kind_et eKind;
  uint64 u64Gen;
  int iReason;
} af__stream_cb_msg_st;

typedef struct af__stream_tx_cfg
{
  uint64 u64Gen;
  uint8 au8SidBe[8];
  uint8 au8VidAuthKey[32];
  uint32 u32AuthTimeoutMs;
} af__stream_tx_cfg_st;

typedef struct af__stream_tx_stats
{
  uint64 u64SendAgainCnt;
  uint64 u64DroppedPCnt;
  uint64 u64TxPktCnt;
  bool_t bLoopAlive;
  bool_t bCurActive;
  bool_t bHasPeer;
  bool_t bKeysZeroed;
} af__stream_tx_stats_st;

typedef struct af__tx_frame
{
  uint64 u64Gen;
  uint32 u32Flags;
  uint64 u64CaptureUs;
  uint8 *pu8Data;
  uint32 u32Len;
} af__tx_frame_st;

typedef struct af__tx_cur
{
  bool_t bActive;
  af_dp_vid_hdr_t stHdr;
  uint8 *pu8Payload;
  uint16 u16TotalPkts;
  uint16 u16NextPkt;
  uint16 u16BatchPkts;
} af__tx_cur_st;

typedef struct af__stream_tx
{
  dm_mutex_st *pstMtx;
  dm_thread_st *pstThread;
  dm_loop_st *pstLoop;
  dm_async_st *pstAsync;
  dm_udp_st *pstUdp;
  dm_loop_timer_st *pstSessTimer;
  dm_loop_timer_st *pstPeerTimer;
  dm_loop_timer_st *pstPaceTimer;

  bool_t bRunning;
  bool_t bStop;
  bool_t bGenValid;
  bool_t bDroppedNotified;
  bool_t bWaitIdr;
  bool_t bHasPeer;
  bool_t bAeadReady;
  bool_t bPaceArmed;
  uint64 u64Gen;
  uint32 u32AuthTimeoutMs;
  uint32 u32PeerTimeoutMs;
  uint32 u32FrameSeq;
  uint64 u64TxPktSeq;
  uint8 au8SidBe[8];
  uint8 au8VidAuthKey[32];
  dm_sockaddr_st stPeerAddr;
  mbedtls_chachapoly_context stAead;
  af_dp_replay_t stRxReplay;

  uint32 u32CurBitrateBps;
  uint64 u64BrLastEvalNs;
  uint64 u64BrSteadyStartNs;
  uint64 u64BrLastDropSnap;

  af__tx_cur_st stCur;
  af__tx_frame_st astPending[AF_STREAM_PENDING_SLOTS];
  uint32 u32PendingCnt;

  _Atomic uint64 u64SendAgainCnt;
  _Atomic uint64 u64DroppedPCnt;
  _Atomic uint64 u64TxPktCnt;
  _Atomic bool_t bLoopAlive;
  _Atomic bool_t bCurActive;
  _Atomic bool_t bHasPeerAtomic;
} af__stream_tx_st;

extern af__stream_tx_st g_stAfTx;

int af__stream_tx_start(dm_in const af__stream_tx_cfg_st *pstCfg, dm_out uint16 *pu16Port);
void af__stream_tx_invalidate_gen(void);
void af__stream_tx_stop_join(void);
void af__stream_tx_submit(dm_in const af_stream_enc_frame_st *pstFrame);
void af__stream_tx_get_stats(dm_out af__stream_tx_stats_st *pstOut);

#ifdef __cplusplus
}
#endif

#endif /* AF_STREAM_INT_H */
