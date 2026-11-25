/**
 ******************************************************************************
 * @file    af_vid_udp.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   Afar UDP video chunking, interleaved XOR FEC & zero-heap reassembler.
 *          SPDX-License-Identifier: MIT
 *          Pure C11 standard library only, explicit alignment-safe byte shifts.
 ******************************************************************************
 */

#include "af_direct_proto.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint16_t get16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put32(uint8_t *p, uint32_t v)
{
  put16(p, (uint16_t)(v & 0xFFFFu));
  put16(p + 2u, (uint16_t)(v >> 16));
}

static uint32_t get32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put64(uint8_t *p, uint64_t v)
{
  put32(p, (uint32_t)(v & 0xFFFFFFFFull));
  put32(p + 4u, (uint32_t)(v >> 32));
}

static uint64_t get64(const uint8_t *p)
{
  return (uint64_t)get32(p) | ((uint64_t)get32(p + 4u) << 32);
}

static int32_t seq_cmp(uint32_t a, uint32_t b)
{
  uint32_t d = a - b;
  int32_t r = 0;
  memcpy(&r, &d, sizeof(r));
  return r;
}

static bool bm_get(const uint8_t bm[AF_DP_VID_BM_BYTES], uint16_t idx)
{
  return (0u != (bm[idx >> 3] & (uint8_t)(1u << (idx & 7u))));
}

static void bm_set(uint8_t bm[AF_DP_VID_BM_BYTES], uint16_t idx)
{
  bm[idx >> 3] = (uint8_t)(bm[idx >> 3] | (uint8_t)(1u << (idx & 7u)));
}

static uint32_t chunk_expected_len(uint32_t frame_len, uint16_t idx)
{
  uint32_t off = (uint32_t)idx * AF_DP_VID_CHUNK_PAYLOAD;
  if (off >= frame_len)
  {
    return 0u;
  }
  return ((frame_len - off) > AF_DP_VID_CHUNK_PAYLOAD) ? AF_DP_VID_CHUNK_PAYLOAD
                                                       : (frame_len - off);
}

static void xor_bytes(uint8_t *dst, const uint8_t *src, size_t n)
{
  size_t i = 0u;
  for (i = 0u; i < n; i++)
  {
    dst[i] ^= src[i];
  }
}

/* ---- Control Datagrams (HELLO / KEEPALIVE / BYE) ---- */

int af_dp_vid_ctl_encode(uint8_t out[12], const af_dp_vid_ctl_t *ctl)
{
  if (NULL == out || NULL == ctl || ctl->type < AF_DP_VID_CTL_HELLO ||
      ctl->type > AF_DP_VID_CTL_BYE)
  {
    return (NULL == out || NULL == ctl) ? AF_DP_ERR_ARG : AF_DP_ERR_FORMAT;
  }
  put32(out, AF_DP_VID_MAGIC);
  out[4] = ctl->type;
  out[5] = 0u;
  out[6] = 0u;
  out[7] = 0u;
  put32(out + 8u, (ctl->loss_permille > 1000u) ? 1000u : ctl->loss_permille);
  return AF_DP_OK;
}

int af_dp_vid_ctl_decode(af_dp_vid_ctl_t *ctl, const uint8_t *in, size_t len)
{
  uint32_t loss = 0u;
  if (NULL == ctl || NULL == in)
  {
    return AF_DP_ERR_ARG;
  }
  if (len != AF_DP_VID_CTL_LEN)
  {
    return (len < AF_DP_VID_CTL_LEN) ? AF_DP_ERR_SHORT : AF_DP_ERR_FORMAT;
  }
  if (AF_DP_VID_MAGIC != get32(in))
  {
    return AF_DP_ERR_MAGIC;
  }
  if (in[4] < AF_DP_VID_CTL_HELLO || in[4] > AF_DP_VID_CTL_BYE)
  {
    return AF_DP_ERR_FORMAT;
  }
  loss = get32(in + 8u);
  ctl->type = in[4];
  ctl->loss_permille = (loss > 1000u) ? 1000u : loss;
  return AF_DP_OK;
}

/* ---- Video Chunk Header ---- */

int af_dp_vid_chunk_hdr_encode(uint8_t out[28], const af_dp_vid_chunk_hdr_t *h)
{
  if (NULL == out || NULL == h)
  {
    return AF_DP_ERR_ARG;
  }
  if (0u == h->frame_seq || h->frame_len > AF_DP_VID_PAYLOAD_MAX || 0u == h->data_cnt ||
      h->data_cnt > AF_DP_VID_MAX_DATA_CHUNKS || h->fec_cnt > AF_DP_VID_FEC_MAX)
  {
    return AF_DP_ERR_LIMIT;
  }
  if ((AF_DP_VID_PKT_DATA == h->pkt_kind && h->chunk_idx >= h->data_cnt) ||
      (AF_DP_VID_PKT_FEC == h->pkt_kind && h->chunk_idx >= h->fec_cnt) ||
      (AF_DP_VID_PKT_DATA != h->pkt_kind && AF_DP_VID_PKT_FEC != h->pkt_kind))
  {
    return AF_DP_ERR_FORMAT;
  }
  put32(out, h->frame_seq);
  put32(out + 4u, h->flags);
  put64(out + 8u, h->capture_us);
  put32(out + 16u, h->frame_len);
  put16(out + 20u, h->chunk_idx);
  put16(out + 22u, h->data_cnt);
  put16(out + 24u, h->fec_cnt);
  out[26] = h->pkt_kind;
  out[27] = 0u;
  return AF_DP_OK;
}

int af_dp_vid_chunk_hdr_decode(af_dp_vid_chunk_hdr_t *h, const uint8_t *in, size_t len)
{
  af_dp_vid_chunk_hdr_t t;
  if (NULL == h || NULL == in)
  {
    return AF_DP_ERR_ARG;
  }
  if (len < AF_DP_VID_CHUNK_HDR_LEN)
  {
    return AF_DP_ERR_SHORT;
  }
  t.frame_seq = get32(in);
  t.flags = get32(in + 4u);
  t.capture_us = get64(in + 8u);
  t.frame_len = get32(in + 16u);
  t.chunk_idx = get16(in + 20u);
  t.data_cnt = get16(in + 22u);
  t.fec_cnt = get16(in + 24u);
  t.pkt_kind = in[26];
  if (0u == t.frame_seq || t.frame_len > AF_DP_VID_PAYLOAD_MAX || 0u == t.data_cnt ||
      t.data_cnt > AF_DP_VID_MAX_DATA_CHUNKS || t.fec_cnt > AF_DP_VID_FEC_MAX)
  {
    return AF_DP_ERR_LIMIT;
  }
  if ((AF_DP_VID_PKT_DATA == t.pkt_kind && t.chunk_idx >= t.data_cnt) ||
      (AF_DP_VID_PKT_FEC == t.pkt_kind && t.chunk_idx >= t.fec_cnt) ||
      (AF_DP_VID_PKT_DATA != t.pkt_kind && AF_DP_VID_PKT_FEC != t.pkt_kind))
  {
    return AF_DP_ERR_FORMAT;
  }
  *h = t;
  return AF_DP_OK;
}

/* ---- TX Interleaved XOR FEC Planner & Chunk Builder ---- */

void af_dp_vid_tx_plan(uint32_t frame_len, uint32_t flags, af_dp_vid_tx_plan_t *out)
{
  uint32_t d = 1u;
  uint32_t f = 0u;
  bool is_key = false;

  if (NULL == out)
  {
    return;
  }
  if (frame_len > 0u)
  {
    d = (frame_len + AF_DP_VID_CHUNK_PAYLOAD - 1u) / AF_DP_VID_CHUNK_PAYLOAD;
  }
  if (d > AF_DP_VID_MAX_DATA_CHUNKS)
  {
    d = AF_DP_VID_MAX_DATA_CHUNKS;
  }
  is_key = (0u != (flags & (AF_VFLAG_KEYFRAME | AF_VFLAG_CONFIG)));
  if (1u == d)
  {
    f = 1u;
  }
  else
  {
    f = is_key ? ((d + 3u) / 4u) : ((d + 4u) / 5u);
    if (f < 2u)
    {
      f = 2u;
    }
    if (f > AF_DP_VID_FEC_MAX)
    {
      f = AF_DP_VID_FEC_MAX;
    }
    if (f > d)
    {
      f = d;
    }
  }
  out->data_cnt = (uint16_t)d;
  out->fec_cnt = (uint16_t)f;
  out->total_cnt = (uint16_t)(d + f);
}

int af_dp_vid_tx_build_chunk(const af_dp_vid_hdr_t *frame_hdr, const uint8_t *frame_data,
                             uint16_t pkt_idx, uint8_t *out_plain, size_t *out_len)
{
  af_dp_vid_tx_plan_t plan;
  af_dp_vid_chunk_hdr_t ch;
  int rc = AF_DP_OK;

  if (NULL == frame_hdr || NULL == out_plain || NULL == out_len ||
      (frame_hdr->payload_len > 0u && NULL == frame_data) ||
      frame_hdr->payload_len > AF_DP_VID_PAYLOAD_MAX)
  {
    return (NULL == frame_hdr || NULL == out_plain || NULL == out_len) ? AF_DP_ERR_ARG
                                                                       : AF_DP_ERR_LIMIT;
  }
  af_dp_vid_tx_plan(frame_hdr->payload_len, frame_hdr->flags, &plan);
  if (pkt_idx >= plan.total_cnt)
  {
    return AF_DP_ERR_LIMIT;
  }
  ch.frame_seq = frame_hdr->frame_seq;
  ch.flags = frame_hdr->flags;
  ch.capture_us = frame_hdr->capture_us;
  ch.frame_len = frame_hdr->payload_len;
  ch.data_cnt = plan.data_cnt;
  ch.fec_cnt = plan.fec_cnt;
  if (pkt_idx < plan.data_cnt)
  {
    uint32_t off = (uint32_t)pkt_idx * AF_DP_VID_CHUNK_PAYLOAD;
    uint32_t n = chunk_expected_len(frame_hdr->payload_len, pkt_idx);
    ch.chunk_idx = pkt_idx;
    ch.pkt_kind = AF_DP_VID_PKT_DATA;
    rc = af_dp_vid_chunk_hdr_encode(out_plain, &ch);
    if (AF_DP_OK != rc)
    {
      return rc;
    }
    if (n > 0u)
    {
      memcpy(out_plain + AF_DP_VID_CHUNK_HDR_LEN, frame_data + off, n);
    }
    *out_len = AF_DP_VID_CHUNK_HDR_LEN + (size_t)n;
    return AF_DP_OK;
  }
  {
    uint16_t g = (uint16_t)(pkt_idx - plan.data_cnt);
    uint8_t *dst = out_plain + AF_DP_VID_CHUNK_HDR_LEN;
    uint16_t i = 0u;
    ch.chunk_idx = g;
    ch.pkt_kind = AF_DP_VID_PKT_FEC;
    rc = af_dp_vid_chunk_hdr_encode(out_plain, &ch);
    if (AF_DP_OK != rc)
    {
      return rc;
    }
    memset(dst, 0, AF_DP_VID_CHUNK_PAYLOAD);
    for (i = g; i < plan.data_cnt; i = (uint16_t)(i + plan.fec_cnt))
    {
      uint32_t off = (uint32_t)i * AF_DP_VID_CHUNK_PAYLOAD;
      uint32_t n = chunk_expected_len(frame_hdr->payload_len, i);
      if (n > 0u)
      {
        xor_bytes(dst, frame_data + off, n);
      }
    }
    *out_len = AF_DP_VID_CHUNK_HDR_LEN + AF_DP_VID_CHUNK_PAYLOAD;
    return AF_DP_OK;
  }
}

/* ---- RX 2-Slot Zero-Heap Interleaved XOR FEC Reassembler ---- */

static void slot_clear_meta(af_dp_vid_rx_slot_t *s)
{
  s->active = false;
  s->frame_seq = 0u;
  s->flags = 0u;
  s->capture_us = 0ull;
  s->frame_len = 0u;
  s->data_cnt = 0u;
  s->fec_cnt = 0u;
  s->got_data = 0u;
  s->fec_bm = 0u;
  memset(s->data_bm, 0, sizeof(s->data_bm));
}

void af_dp_vid_rx_init(af_dp_vid_rx_t *rx, uint8_t *buf0, uint8_t *buf1, size_t buf_cap)
{
  if (NULL == rx)
  {
    return;
  }
  memset(rx, 0, sizeof(*rx));
  rx->slots[0].data_buf = buf0;
  rx->slots[0].data_cap = buf_cap;
  rx->slots[1].data_buf = buf1;
  rx->slots[1].data_cap = buf_cap;
}

void af_dp_vid_rx_reset(af_dp_vid_rx_t *rx)
{
  if (NULL == rx)
  {
    return;
  }
  slot_clear_meta(&rx->slots[0]);
  slot_clear_meta(&rx->slots[1]);
  rx->last_emitted_seq = 0u;
  rx->evicted_incomplete = false;
}

static af_dp_vid_rx_slot_t *rx_find_or_alloc(af_dp_vid_rx_t *rx, const af_dp_vid_chunk_hdr_t *ch)
{
  int pick = -1;
  int i = 0;

  for (i = 0; i < 2; i++)
  {
    if (rx->slots[i].active && rx->slots[i].frame_seq == ch->frame_seq)
    {
      if (rx->slots[i].frame_len != ch->frame_len || rx->slots[i].data_cnt != ch->data_cnt ||
          rx->slots[i].fec_cnt != ch->fec_cnt)
      {
        return NULL;
      }
      return &rx->slots[i];
    }
  }
  for (i = 0; i < 2; i++)
  {
    if (!rx->slots[i].active)
    {
      pick = i;
      break;
    }
  }
  if (pick < 0)
  {
    pick = (seq_cmp(rx->slots[0].frame_seq, rx->slots[1].frame_seq) <= 0) ? 0 : 1;
    if (seq_cmp(ch->frame_seq, rx->slots[pick].frame_seq) <= 0)
    {
      return NULL;
    }
    rx->total_frames_lost++;
    rx->evicted_incomplete = true;
  }
  if (NULL == rx->slots[pick].data_buf || (size_t)ch->frame_len > rx->slots[pick].data_cap)
  {
    return NULL;
  }
  slot_clear_meta(&rx->slots[pick]);
  rx->slots[pick].active = true;
  rx->slots[pick].frame_seq = ch->frame_seq;
  rx->slots[pick].flags = ch->flags;
  rx->slots[pick].capture_us = ch->capture_us;
  rx->slots[pick].frame_len = ch->frame_len;
  rx->slots[pick].data_cnt = ch->data_cnt;
  rx->slots[pick].fec_cnt = ch->fec_cnt;
  return &rx->slots[pick];
}

static void rx_try_recover_stripe(af_dp_vid_rx_t *rx, af_dp_vid_rx_slot_t *s, uint16_t g)
{
  uint16_t miss_cnt = 0u;
  uint16_t miss_idx = 0u;
  uint16_t i = 0u;
  uint8_t tmp[AF_DP_VID_CHUNK_PAYLOAD];
  uint32_t off = 0u;
  uint32_t n = 0u;

  if (0u == s->fec_cnt || g >= s->fec_cnt || 0u == (s->fec_bm & (1u << g)))
  {
    return;
  }
  for (i = g; i < s->data_cnt; i = (uint16_t)(i + s->fec_cnt))
  {
    if (!bm_get(s->data_bm, i))
    {
      miss_cnt++;
      miss_idx = i;
      if (miss_cnt > 1u)
      {
        return;
      }
    }
  }
  if (1u != miss_cnt)
  {
    return;
  }
  memcpy(tmp, s->fec_buf[g], AF_DP_VID_CHUNK_PAYLOAD);
  for (i = g; i < s->data_cnt; i = (uint16_t)(i + s->fec_cnt))
  {
    if (i != miss_idx)
    {
      uint32_t ioff = (uint32_t)i * AF_DP_VID_CHUNK_PAYLOAD;
      uint32_t in = chunk_expected_len(s->frame_len, i);
      if (in > 0u)
      {
        xor_bytes(tmp, s->data_buf + ioff, in);
      }
    }
  }
  off = (uint32_t)miss_idx * AF_DP_VID_CHUNK_PAYLOAD;
  n = chunk_expected_len(s->frame_len, miss_idx);
  if (n > 0u)
  {
    memcpy(s->data_buf + off, tmp, n);
  }
  bm_set(s->data_bm, miss_idx);
  s->got_data++;
  rx->total_fec_recovered++;
}

static void rx_store_chunk(af_dp_vid_rx_t *rx, af_dp_vid_rx_slot_t *s,
                           const af_dp_vid_chunk_hdr_t *ch, const uint8_t *pay, size_t pay_len)
{
  if (AF_DP_VID_PKT_DATA == ch->pkt_kind)
  {
    if (!bm_get(s->data_bm, ch->chunk_idx))
    {
      if (pay_len > 0u)
      {
        memcpy(s->data_buf + (size_t)ch->chunk_idx * AF_DP_VID_CHUNK_PAYLOAD, pay, pay_len);
      }
      bm_set(s->data_bm, ch->chunk_idx);
      s->got_data++;
      if (s->fec_cnt > 0u)
      {
        rx_try_recover_stripe(rx, s, (uint16_t)(ch->chunk_idx % s->fec_cnt));
      }
    }
    return;
  }
  if (0u == (s->fec_bm & (1u << ch->chunk_idx)))
  {
    memcpy(s->fec_buf[ch->chunk_idx], pay, AF_DP_VID_CHUNK_PAYLOAD);
    s->fec_bm |= (1u << ch->chunk_idx);
    rx_try_recover_stripe(rx, s, ch->chunk_idx);
  }
}

int af_dp_vid_rx_push(af_dp_vid_rx_t *rx, const uint8_t *plain, size_t len,
                      af_dp_vid_rx_slot_t **out_ready)
{
  af_dp_vid_chunk_hdr_t ch;
  af_dp_vid_rx_slot_t *s = NULL;
  const uint8_t *pay = NULL;
  size_t pay_len = 0u;
  int rc = AF_DP_OK;

  if (NULL != out_ready)
  {
    *out_ready = NULL;
  }
  if (NULL == rx || NULL == plain)
  {
    return AF_DP_ERR_ARG;
  }
  rx->evicted_incomplete = false;
  rc = af_dp_vid_chunk_hdr_decode(&ch, plain, len);
  if (AF_DP_OK != rc)
  {
    return rc;
  }
  if (0u != rx->last_emitted_seq && seq_cmp(ch.frame_seq, rx->last_emitted_seq) <= 0)
  {
    return AF_DP_VID_RX_STALE;
  }
  pay = plain + AF_DP_VID_CHUNK_HDR_LEN;
  pay_len = len - AF_DP_VID_CHUNK_HDR_LEN;
  if ((AF_DP_VID_PKT_DATA == ch.pkt_kind &&
       pay_len != (size_t)chunk_expected_len(ch.frame_len, ch.chunk_idx)) ||
      (AF_DP_VID_PKT_FEC == ch.pkt_kind && pay_len != AF_DP_VID_CHUNK_PAYLOAD))
  {
    return AF_DP_ERR_FORMAT;
  }
  s = rx_find_or_alloc(rx, &ch);
  if (NULL == s)
  {
    return AF_DP_VID_RX_STALE;
  }
  rx_store_chunk(rx, s, &ch, pay, pay_len);
  if (s->got_data == s->data_cnt)
  {
    s->active = false;
    rx->last_emitted_seq = s->frame_seq;
    rx->total_frames_ok++;
    if (NULL != out_ready)
    {
      *out_ready = s;
    }
    return AF_DP_VID_RX_READY;
  }
  return AF_DP_VID_RX_PENDING;
}
