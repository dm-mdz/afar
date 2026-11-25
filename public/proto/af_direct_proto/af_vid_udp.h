/**
 ******************************************************************************
 * @file    af_vid_udp.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   Afar UDP video chunking, interleaved XOR FEC & control wire framing.
 *          SPDX-License-Identifier: MIT
 *          Zero OS headers, zero DM headers, zero crypto library coupling.
 ******************************************************************************
 */
#ifndef AF_VID_UDP_H
#define AF_VID_UDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define AF_DP_DIR_VID_I2M           3u
#define AF_DP_DIR_VID_M2I           4u

#define AF_DP_VID_UDP_MTU           1400u
#define AF_DP_VID_CHUNK_HDR_LEN     28u
#define AF_DP_VID_CHUNK_PAYLOAD     1328u
#define AF_DP_VID_PLAIN_MAX         (AF_DP_VID_CHUNK_HDR_LEN + AF_DP_VID_CHUNK_PAYLOAD)
#define AF_DP_VID_MAX_DATA_CHUNKS   6320u
#define AF_DP_VID_FEC_MAX           32u
#define AF_DP_VID_BM_BYTES          ((AF_DP_VID_MAX_DATA_CHUNKS + 7u) / 8u)

#define AF_DP_VID_PKT_DATA          1u
#define AF_DP_VID_PKT_FEC           2u

#define AF_DP_VID_CTL_LEN           12u
#define AF_DP_VID_CTL_HELLO         1u
#define AF_DP_VID_CTL_KEEPALIVE     2u
#define AF_DP_VID_CTL_BYE           3u

#define AF_DP_VID_RX_PENDING        0
#define AF_DP_VID_RX_READY          1
#define AF_DP_VID_RX_STALE          2

typedef struct af_dp_vid_ctl
{
  uint8_t type;
  uint32_t loss_permille;
} af_dp_vid_ctl_t;

typedef struct af_dp_vid_chunk_hdr
{
  uint32_t frame_seq;
  uint32_t flags;
  uint64_t capture_us;
  uint32_t frame_len;
  uint16_t chunk_idx;
  uint16_t data_cnt;
  uint16_t fec_cnt;
  uint8_t pkt_kind;
} af_dp_vid_chunk_hdr_t;

typedef struct af_dp_vid_tx_plan
{
  uint16_t data_cnt;
  uint16_t fec_cnt;
  uint16_t total_cnt;
} af_dp_vid_tx_plan_t;

typedef struct af_dp_vid_rx_slot
{
  bool active;
  uint32_t frame_seq;
  uint32_t flags;
  uint64_t capture_us;
  uint32_t frame_len;
  uint16_t data_cnt;
  uint16_t fec_cnt;
  uint16_t got_data;
  uint8_t *data_buf;
  size_t data_cap;
  uint8_t data_bm[AF_DP_VID_BM_BYTES];
  uint32_t fec_bm;
  uint8_t fec_buf[AF_DP_VID_FEC_MAX][AF_DP_VID_CHUNK_PAYLOAD];
} af_dp_vid_rx_slot_t;

typedef struct af_dp_vid_rx
{
  af_dp_vid_rx_slot_t slots[2];
  uint32_t last_emitted_seq;
  uint32_t total_frames_ok;
  uint32_t total_frames_lost;
  uint32_t total_fec_recovered;
  bool evicted_incomplete;
} af_dp_vid_rx_t;

int af_dp_vid_ctl_encode(uint8_t out[12], const af_dp_vid_ctl_t *ctl);
int af_dp_vid_ctl_decode(af_dp_vid_ctl_t *ctl, const uint8_t *in, size_t len);

int af_dp_vid_chunk_hdr_encode(uint8_t out[28], const af_dp_vid_chunk_hdr_t *h);
int af_dp_vid_chunk_hdr_decode(af_dp_vid_chunk_hdr_t *h, const uint8_t *in, size_t len);

void af_dp_vid_tx_plan(uint32_t frame_len, uint32_t flags, af_dp_vid_tx_plan_t *out);
int af_dp_vid_tx_build_chunk(const af_dp_vid_hdr_t *frame_hdr, const uint8_t *frame_data,
                             uint16_t pkt_idx, uint8_t *out_plain, size_t *out_len);

void af_dp_vid_rx_init(af_dp_vid_rx_t *rx, uint8_t *buf0, uint8_t *buf1, size_t buf_cap);
void af_dp_vid_rx_reset(af_dp_vid_rx_t *rx);
int af_dp_vid_rx_push(af_dp_vid_rx_t *rx, const uint8_t *plain, size_t len,
                      af_dp_vid_rx_slot_t **out_ready);

#ifdef __cplusplus
}
#endif

#endif /* AF_VID_UDP_H */
