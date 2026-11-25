/**
 ******************************************************************************
 * @file    af_direct_proto.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   Afar v0.6 Direct Stream & Input binary wire protocol (MIT, pure C11).
 *          SPDX-License-Identifier: MIT
 *          Zero OS headers, zero DM headers, zero crypto library coupling.
 *          Explicit alignment-safe byte serialization (no packed pointer casts).
 ******************************************************************************
 */
#ifndef AF_DIRECT_PROTO_H
#define AF_DIRECT_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ---- Return Codes ---- */
#define AF_DP_OK            0
#define AF_DP_ERR_ARG       (-1)
#define AF_DP_ERR_SHORT     (-2)
#define AF_DP_ERR_MAGIC     (-3)
#define AF_DP_ERR_LIMIT     (-4)
#define AF_DP_ERR_REPLAY    (-5)
#define AF_DP_ERR_FORMAT    (-6)
#define AF_DP_ERR_FULL      (-7)

/* ---- Wire Constants (Frozen DD v2.1 §4.2) ---- */
#define AF_DP_VID_MAGIC         0x41465636u /* "AFV6" */
#define AF_DP_VID_HDR_LEN       24u
#define AF_DP_VID_AUTH_LEN      44u
#define AF_DP_VID_PAYLOAD_MAX   (8u * 1024u * 1024u)

#define AF_VFLAG_KEYFRAME       (1u << 0)
#define AF_VFLAG_CONFIG         (1u << 1)
#define AF_VFLAG_IDLE_SHARP     (1u << 2)
#define AF_VFLAG_CHROMA_444     (1u << 3)

#define AF_DP_UDP_MTU_MAX       1200u
#define AF_DP_UDP_OUTER_LEN     16u
#define AF_DP_AEAD_TAG_LEN      16u
#define AF_DP_AEAD_NONCE_LEN    12u
#define AF_DP_IN_HDR_LEN        32u
#define AF_DP_IN_TLV_HDR_LEN    7u
#define AF_DP_IN_PAYLOAD_BUDGET (AF_DP_UDP_MTU_MAX - AF_DP_UDP_OUTER_LEN - AF_DP_AEAD_TAG_LEN - AF_DP_IN_HDR_LEN) /* 1136B */
#define AF_DP_IN_PLAIN_MAX      (AF_DP_IN_HDR_LEN + AF_DP_IN_PAYLOAD_BUDGET) /* 1168B */
#define AF_DP_IN_VER            1u
#define AF_DP_IN_MAX_EVS        16u
#define AF_DP_TEXT_MAX_BYTES    256u
#define AF_DP_SEQ_MAX_SAFE      (UINT64_MAX - 1024ull)

#define AF_DP_DIR_I2M           1u
#define AF_DP_DIR_M2I           2u

#define AF_IN_FLAG_HAS_ABS      (1u << 0)
#define AF_IN_FLAG_REQ_IDR      (1u << 1)
#define AF_IN_FLAG_RELEASE_ALL  (1u << 2)
#define AF_IN_FLAG_PING_PONG    (1u << 3)

#define AF_DP_HKDF_INFO_VID_AUTH "afar-v06-vid-auth"
#define AF_DP_HKDF_INFO_IN_I2M   "afar-v06-in-i2m"
#define AF_DP_HKDF_INFO_CTL_M2I  "afar-v06-ctl-m2i"

/* ---- Video Frame Header & CONFIG Payload ---- */
typedef struct af_dp_vid_hdr
{
  uint32_t magic;
  uint32_t frame_seq;
  uint32_t flags;
  uint32_t payload_len;
  uint64_t capture_us;
} af_dp_vid_hdr_t;

typedef struct af_dp_vid_config
{
  uint16_t width;
  uint16_t height;
  const uint8_t *vps;
  uint16_t vps_len;
  const uint8_t *sps;
  uint16_t sps_len;
  const uint8_t *pps;
  uint16_t pps_len;
} af_dp_vid_config_t;

/* ---- UDP Input Header & Discrete Events ---- */
typedef enum af_dp_ev_kind
{
  AF_EV_MOUSE_ABS = 1,
  AF_EV_MOUSE_BTN = 2,
  AF_EV_KEY = 3,
  AF_EV_PENCIL = 4,
  AF_EV_TEXT_COMMIT = 5,
  AF_EV_SEC_INPUT = 6
} af_dp_ev_kind_t;

typedef enum af_dp_pencil_phase
{
  AF_PENCIL_HOVER = 0,
  AF_PENCIL_DOWN = 1,
  AF_PENCIL_MOVE = 2,
  AF_PENCIL_UP = 3,
  AF_PENCIL_CANCEL = 4
} af_dp_pencil_phase_t;

typedef struct af_dp_in_hdr
{
  uint8_t ver;
  uint8_t flags;
  uint8_t ev_cnt;
  uint8_t reserved;
  uint32_t ack_ev_seq;
  uint32_t cum_dx;
  uint32_t cum_dy;
  uint32_t cum_wx;
  uint32_t cum_wy;
  uint64_t client_us;
} af_dp_in_hdr_t;

typedef struct af_dp_ev
{
  uint32_t ev_seq;
  uint8_t kind;
  union
  {
    struct
    {
      float nx;
      float ny;
    } abs;
    struct
    {
      uint8_t btn;
      uint8_t down;
      uint8_t has_abs;
      float nx;
      float ny;
    } btn;
    struct
    {
      uint16_t hid_usage;
      uint8_t down;
    } key;
    struct
    {
      uint8_t phase;
      float nx;
      float ny;
      float pressure;
      float tilt_x;
      float tilt_y;
    } pencil;
    struct
    {
      uint16_t utf8_len;
      uint8_t utf8[AF_DP_TEXT_MAX_BYTES];
    } text;
    struct
    {
      uint8_t active;
    } sec_input;
  } u;
} af_dp_ev_t;

typedef struct af_dp_in_pkt
{
  af_dp_in_hdr_t hdr;
  af_dp_ev_t evs[AF_DP_IN_MAX_EVS];
} af_dp_in_pkt_t;

/* ---- Replay Window, Cumulative Delta & Reliable Retransmit Queue ---- */
typedef struct af_dp_replay
{
  uint64_t max_seq;
  uint64_t bitmap;
} af_dp_replay_t;

typedef struct af_dp_cum_state
{
  bool initialized;
  uint32_t last_dx;
  uint32_t last_dy;
  uint32_t last_wx;
  uint32_t last_wy;
} af_dp_cum_state_t;

typedef struct af_dp_delta
{
  int32_t dx;
  int32_t dy;
  int32_t wx;
  int32_t wy;
} af_dp_delta_t;

#define AF_DP_TXQ_CAP 64u

typedef struct af_dp_txq_item
{
  af_dp_ev_t ev;
  uint64_t last_tx_ms;
  bool active;
} af_dp_txq_item_t;

typedef struct af_dp_txq
{
  af_dp_txq_item_t items[AF_DP_TXQ_CAP];
  uint32_t next_ev_seq;
  uint32_t count;
} af_dp_txq_t;

typedef struct af_dp_rx_dedup
{
  uint32_t last_ev_seq;
} af_dp_rx_dedup_t;

/* ---- USB HID Usage Page 0x07 Constants (Shared Mac/iPad/Yiwei) ---- */
#define AF_HID_KEY_A            0x04u
#define AF_HID_KEY_Z            0x1Du
#define AF_HID_KEY_1            0x1Eu
#define AF_HID_KEY_0            0x27u
#define AF_HID_KEY_RETURN       0x28u
#define AF_HID_KEY_ESCAPE       0x29u
#define AF_HID_KEY_BACKSPACE    0x2Au
#define AF_HID_KEY_TAB          0x2Bu
#define AF_HID_KEY_SPACE        0x2Cu
#define AF_HID_KEY_CAPS_LOCK    0x39u
#define AF_HID_KEY_F1           0x3Au
#define AF_HID_KEY_F12          0x45u
#define AF_HID_KEY_DELETE       0x4Cu
#define AF_HID_KEY_RIGHT        0x4Fu
#define AF_HID_KEY_LEFT         0x50u
#define AF_HID_KEY_DOWN         0x51u
#define AF_HID_KEY_UP           0x52u
#define AF_HID_KEY_LCTRL        0xE0u
#define AF_HID_KEY_LSHIFT       0xE1u
#define AF_HID_KEY_LALT         0xE2u
#define AF_HID_KEY_LGUI         0xE3u
#define AF_HID_KEY_RCTRL        0xE4u
#define AF_HID_KEY_RSHIFT       0xE5u
#define AF_HID_KEY_RALT         0xE6u
#define AF_HID_KEY_RGUI         0xE7u

/* ---- API (All <= 5 params, pure C11) ---- */
int af_dp_sid_to_hex(const uint8_t sid_be8[8], char out_hex[17]);
int af_dp_sid_from_hex(const char *hex16, uint8_t out_sid_be8[8]);

int af_dp_vid_hdr_encode(uint8_t out[24], const af_dp_vid_hdr_t *h);
int af_dp_vid_hdr_decode(af_dp_vid_hdr_t *h, const uint8_t in[24]);
int af_dp_vid_auth_encode(uint8_t out[44], const uint8_t sid_be8[8], const uint8_t auth[32]);
int af_dp_vid_auth_decode(uint8_t sid_be8[8], uint8_t auth[32], const uint8_t in[44]);
int af_dp_vid_config_encode(const af_dp_vid_config_t *cfg, uint8_t *buf, size_t cap,
                            size_t *out_len);
int af_dp_vid_config_decode(af_dp_vid_config_t *cfg, const uint8_t *buf, size_t len);
int af_dp_hvcc_validate(const uint8_t *buf, size_t len);

int af_dp_udp_outer_encode(uint8_t out[16], const uint8_t sid_be8[8], uint64_t pkt_seq);
int af_dp_udp_outer_decode(uint8_t sid_be8[8], uint64_t *pkt_seq, const uint8_t in[16]);
void af_dp_aead_nonce(uint8_t out[12], uint32_t dir, uint64_t pkt_seq);

int af_dp_in_encode(const af_dp_in_pkt_t *pkt, uint8_t *buf, size_t cap, size_t *out_len);
int af_dp_in_decode(af_dp_in_pkt_t *pkt, const uint8_t *buf, size_t len);

void af_dp_replay_init(af_dp_replay_t *w);
int af_dp_replay_check(af_dp_replay_t *w, uint64_t seq, int *out_is_newest);

void af_dp_cum_init(af_dp_cum_state_t *st);
void af_dp_cum_step(af_dp_cum_state_t *st, const af_dp_in_hdr_t *hdr, int is_newest,
                    af_dp_delta_t *out);

void af_dp_txq_init(af_dp_txq_t *q);
int af_dp_txq_push(af_dp_txq_t *q, const af_dp_ev_t *ev, uint32_t *out_seq);
void af_dp_txq_ack(af_dp_txq_t *q, uint32_t ack_ev_seq);
size_t af_dp_txq_pack(af_dp_txq_t *q, uint64_t now_ms, uint32_t rtx_ms, af_dp_in_pkt_t *pkt);

void af_dp_rx_dedup_init(af_dp_rx_dedup_t *d);
bool af_dp_rx_dedup_accept(af_dp_rx_dedup_t *d, uint32_t ev_seq);

#include "af_vid_udp.h"

#ifdef __cplusplus
}
#endif

#endif /* AF_DIRECT_PROTO_H */
