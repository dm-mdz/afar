/**
 ******************************************************************************
 * @file    af_direct_proto.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   Afar v0.6 Direct Stream & Input binary wire protocol implementation.
 *          SPDX-License-Identifier: MIT
 *          Pure C11 standard library only, explicit alignment-safe byte shifts.
 ******************************************************************************
 */

#include "af_direct_proto.h"

#include <math.h>
#include <string.h>

#define AF_DP_TXQ_UNSENT UINT64_MAX

/* ---- Alignment-Safe Byte & Float Helpers ---- */

static void put_u16_le(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint16_t get_u16_le(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put_u32_le(uint8_t *p, uint32_t v)
{
  put_u16_le(p, (uint16_t)(v & 0xFFFFu));
  put_u16_le(p + 2u, (uint16_t)(v >> 16));
}

static uint32_t get_u32_le(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t get_u32_be(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_u64_le(uint8_t *p, uint64_t v)
{
  put_u32_le(p, (uint32_t)(v & 0xFFFFFFFFull));
  put_u32_le(p + 4u, (uint32_t)(v >> 32));
}

static uint64_t get_u64_le(const uint8_t *p)
{
  return (uint64_t)get_u32_le(p) | ((uint64_t)get_u32_le(p + 4u) << 32);
}

static int put_f32_le(uint8_t *p, float v)
{
  uint32_t bits = 0u;
  if (!isfinite(v))
  {
    return AF_DP_ERR_FORMAT;
  }
  memcpy(&bits, &v, sizeof(bits));
  put_u32_le(p, bits);
  return AF_DP_OK;
}

static int get_f32_clamped(const uint8_t *p, float lo, float hi, float *out)
{
  uint32_t bits = get_u32_le(p);
  float v = 0.0f;
  memcpy(&v, &bits, sizeof(v));
  if (!isfinite(v))
  {
    return AF_DP_ERR_FORMAT;
  }
  *out = (v < lo) ? lo : ((v > hi) ? hi : v);
  return AF_DP_OK;
}

static int32_t u32_diff_to_i32(uint32_t cur, uint32_t last)
{
  uint32_t diff = cur - last;
  int32_t res = 0;
  memcpy(&res, &diff, sizeof(res));
  return res;
}

static int hex_nibble(char c)
{
  return (c >= '0' && c <= '9')
             ? (int)(c - '0')
             : ((c >= 'a' && c <= 'f') ? (10 + (int)(c - 'a'))
                                       : ((c >= 'A' && c <= 'F') ? (10 + (int)(c - 'A')) : -1));
}

/* ---- Session ID Hex Conversion ---- */

int af_dp_sid_to_hex(const uint8_t sid_be8[8], char out_hex[17])
{
  static const char lut[17] = "0123456789abcdef";
  size_t i = 0u;
  if (NULL == sid_be8 || NULL == out_hex)
  {
    return AF_DP_ERR_ARG;
  }
  for (i = 0u; i < 8u; i++)
  {
    out_hex[i * 2u] = lut[(sid_be8[i] >> 4) & 0x0Fu];
    out_hex[i * 2u + 1u] = lut[sid_be8[i] & 0x0Fu];
  }
  out_hex[16] = '\0';
  return AF_DP_OK;
}

int af_dp_sid_from_hex(const char *hex16, uint8_t out_sid_be8[8])
{
  uint8_t tmp[8];
  size_t i = 0u;
  if (NULL == hex16 || NULL == out_sid_be8)
  {
    return AF_DP_ERR_ARG;
  }
  for (i = 0u; i < 8u; i++)
  {
    int hi = hex_nibble(hex16[i * 2u]);
    int lo = hex_nibble(hex16[i * 2u + 1u]);
    if (hi < 0 || lo < 0)
    {
      return AF_DP_ERR_FORMAT;
    }
    tmp[i] = (uint8_t)(((unsigned int)hi << 4) | (unsigned int)lo);
  }
  if ('\0' != hex16[16])
  {
    return AF_DP_ERR_FORMAT;
  }
  memcpy(out_sid_be8, tmp, sizeof(tmp));
  return AF_DP_OK;
}

/* ---- Video Frame Header, Auth, CONFIG & HVCC ---- */

int af_dp_vid_hdr_encode(uint8_t out[24], const af_dp_vid_hdr_t *h)
{
  if (NULL == out || NULL == h)
  {
    return AF_DP_ERR_ARG;
  }
  if ((0u != h->magic && AF_DP_VID_MAGIC != h->magic) || h->payload_len > AF_DP_VID_PAYLOAD_MAX)
  {
    return (0u != h->magic && AF_DP_VID_MAGIC != h->magic) ? AF_DP_ERR_MAGIC : AF_DP_ERR_LIMIT;
  }
  put_u32_le(out, AF_DP_VID_MAGIC);
  put_u32_le(out + 4u, h->frame_seq);
  put_u32_le(out + 8u, h->flags);
  put_u32_le(out + 12u, h->payload_len);
  put_u64_le(out + 16u, h->capture_us);
  return AF_DP_OK;
}

int af_dp_vid_hdr_decode(af_dp_vid_hdr_t *h, const uint8_t in[24])
{
  uint32_t magic = 0u;
  uint32_t plen = 0u;
  if (NULL == h || NULL == in)
  {
    return AF_DP_ERR_ARG;
  }
  magic = get_u32_le(in);
  plen = get_u32_le(in + 12u);
  if (AF_DP_VID_MAGIC != magic || plen > AF_DP_VID_PAYLOAD_MAX)
  {
    return (AF_DP_VID_MAGIC != magic) ? AF_DP_ERR_MAGIC : AF_DP_ERR_LIMIT;
  }
  h->magic = magic;
  h->frame_seq = get_u32_le(in + 4u);
  h->flags = get_u32_le(in + 8u);
  h->payload_len = plen;
  h->capture_us = get_u64_le(in + 16u);
  return AF_DP_OK;
}

int af_dp_vid_auth_encode(uint8_t out[44], const uint8_t sid_be8[8], const uint8_t auth[32])
{
  if (NULL == out || NULL == sid_be8 || NULL == auth)
  {
    return AF_DP_ERR_ARG;
  }
  put_u32_le(out, AF_DP_VID_MAGIC);
  memcpy(out + 4u, sid_be8, 8u);
  memcpy(out + 12u, auth, 32u);
  return AF_DP_OK;
}

int af_dp_vid_auth_decode(uint8_t sid_be8[8], uint8_t auth[32], const uint8_t in[44])
{
  if (NULL == sid_be8 || NULL == auth || NULL == in || AF_DP_VID_MAGIC != get_u32_le(in))
  {
    return (NULL == sid_be8 || NULL == auth || NULL == in) ? AF_DP_ERR_ARG : AF_DP_ERR_MAGIC;
  }
  memcpy(sid_be8, in + 4u, 8u);
  memcpy(auth, in + 12u, 32u);
  return AF_DP_OK;
}

static void write_ps(uint8_t *buf, size_t *pos, const uint8_t *src, uint16_t n)
{
  put_u16_le(buf + *pos, n);
  *pos += 2u;
  memcpy(buf + *pos, src, n);
  *pos += n;
}

static int read_ps(const uint8_t *buf, size_t len, size_t *pos, const uint8_t **ptr, uint16_t *n)
{
  if (len - *pos < 2u)
  {
    return AF_DP_ERR_SHORT;
  }
  *n = get_u16_le(buf + *pos);
  *pos += 2u;
  if (0u == *n || len - *pos < (size_t)*n)
  {
    return (0u == *n) ? AF_DP_ERR_FORMAT : AF_DP_ERR_SHORT;
  }
  *ptr = buf + *pos;
  *pos += (size_t)*n;
  return AF_DP_OK;
}

int af_dp_vid_config_encode(const af_dp_vid_config_t *cfg, uint8_t *buf, size_t cap,
                            size_t *out_len)
{
  size_t need = 0u;
  size_t pos = 4u;
  if (NULL == cfg || NULL == buf || NULL == out_len || NULL == cfg->vps || NULL == cfg->sps ||
      NULL == cfg->pps)
  {
    return AF_DP_ERR_ARG;
  }
  if (0u == cfg->width || 0u == cfg->height || 0u == cfg->vps_len || 0u == cfg->sps_len ||
      0u == cfg->pps_len)
  {
    return AF_DP_ERR_FORMAT;
  }
  need = 10u + (size_t)cfg->vps_len + (size_t)cfg->sps_len + (size_t)cfg->pps_len;
  if (cap < need)
  {
    return AF_DP_ERR_SHORT;
  }
  put_u16_le(buf, cfg->width);
  put_u16_le(buf + 2u, cfg->height);
  write_ps(buf, &pos, cfg->vps, cfg->vps_len);
  write_ps(buf, &pos, cfg->sps, cfg->sps_len);
  write_ps(buf, &pos, cfg->pps, cfg->pps_len);
  *out_len = pos;
  return AF_DP_OK;
}

int af_dp_vid_config_decode(af_dp_vid_config_t *cfg, const uint8_t *buf, size_t len)
{
  af_dp_vid_config_t tmp;
  size_t pos = 4u;
  int rc = AF_DP_OK;
  if (NULL == cfg || NULL == buf || len < 4u)
  {
    return (NULL == cfg || NULL == buf) ? AF_DP_ERR_ARG : AF_DP_ERR_SHORT;
  }
  memset(&tmp, 0, sizeof(tmp));
  tmp.width = get_u16_le(buf);
  tmp.height = get_u16_le(buf + 2u);
  if (0u == tmp.width || 0u == tmp.height)
  {
    return AF_DP_ERR_FORMAT;
  }
  rc = read_ps(buf, len, &pos, &tmp.vps, &tmp.vps_len);
  rc = (AF_DP_OK == rc) ? read_ps(buf, len, &pos, &tmp.sps, &tmp.sps_len) : rc;
  rc = (AF_DP_OK == rc) ? read_ps(buf, len, &pos, &tmp.pps, &tmp.pps_len) : rc;
  if (AF_DP_OK != rc || pos != len)
  {
    return (AF_DP_OK != rc) ? rc : AF_DP_ERR_FORMAT;
  }
  *cfg = tmp;
  return AF_DP_OK;
}

int af_dp_hvcc_validate(const uint8_t *buf, size_t len)
{
  size_t pos = 0u;
  if (NULL == buf || 0u == len)
  {
    return (NULL == buf) ? AF_DP_ERR_ARG : AF_DP_ERR_SHORT;
  }
  while (pos < len)
  {
    uint32_t nalu_len = 0u;
    if (len - pos < 4u)
    {
      return AF_DP_ERR_SHORT;
    }
    nalu_len = get_u32_be(buf + pos);
    pos += 4u;
    if (0u == nalu_len || (size_t)nalu_len > len - pos)
    {
      return (0u == nalu_len) ? AF_DP_ERR_FORMAT : AF_DP_ERR_SHORT;
    }
    pos += (size_t)nalu_len;
  }
  return AF_DP_OK;
}

/* ---- UDP Outer Header & AEAD Nonce ---- */

int af_dp_udp_outer_encode(uint8_t out[16], const uint8_t sid_be8[8], uint64_t pkt_seq)
{
  if (NULL == out || NULL == sid_be8 || 0ull == pkt_seq || pkt_seq >= AF_DP_SEQ_MAX_SAFE)
  {
    return (NULL == out || NULL == sid_be8) ? AF_DP_ERR_ARG : AF_DP_ERR_LIMIT;
  }
  memcpy(out, sid_be8, 8u);
  put_u64_le(out + 8u, pkt_seq);
  return AF_DP_OK;
}

int af_dp_udp_outer_decode(uint8_t sid_be8[8], uint64_t *pkt_seq, const uint8_t in[16])
{
  uint64_t seq = 0ull;
  if (NULL == sid_be8 || NULL == pkt_seq || NULL == in)
  {
    return AF_DP_ERR_ARG;
  }
  seq = get_u64_le(in + 8u);
  if (0ull == seq || seq >= AF_DP_SEQ_MAX_SAFE)
  {
    return AF_DP_ERR_LIMIT;
  }
  memcpy(sid_be8, in, 8u);
  *pkt_seq = seq;
  return AF_DP_OK;
}

void af_dp_aead_nonce(uint8_t out[12], uint32_t dir, uint64_t pkt_seq)
{
  if (NULL != out)
  {
    put_u32_le(out, dir);
    put_u64_le(out + 4u, pkt_seq);
  }
}

/* ---- UDP Input Plaintext Encode & Decode ---- */

static int ev_body_len(const af_dp_ev_t *ev, uint16_t *out_len)
{
  static const uint8_t kFix[7] = {0u, 8u, 11u, 3u, 21u, 0u, 1u};
  if (ev->kind < AF_EV_MOUSE_ABS || ev->kind > AF_EV_SEC_INPUT)
  {
    return AF_DP_ERR_FORMAT;
  }
  if (AF_EV_TEXT_COMMIT == ev->kind)
  {
    uint16_t n = ev->u.text.utf8_len;
    if (n > AF_DP_TEXT_MAX_BYTES)
    {
      return AF_DP_ERR_LIMIT;
    }
    if (0u == n || NULL != memchr(ev->u.text.utf8, '\0', n))
    {
      return AF_DP_ERR_FORMAT;
    }
    *out_len = (uint16_t)(2u + n);
    return AF_DP_OK;
  }
  if ((AF_EV_MOUSE_ABS == ev->kind && (!isfinite(ev->u.abs.nx) || !isfinite(ev->u.abs.ny))) ||
      (AF_EV_MOUSE_BTN == ev->kind &&
       (ev->u.btn.btn > 2u || ev->u.btn.down > 1u || ev->u.btn.has_abs > 1u ||
        !isfinite(ev->u.btn.nx) || !isfinite(ev->u.btn.ny))) ||
      (AF_EV_KEY == ev->kind && ev->u.key.down > 1u) ||
      (AF_EV_PENCIL == ev->kind &&
       (ev->u.pencil.phase > AF_PENCIL_CANCEL || !isfinite(ev->u.pencil.nx) ||
        !isfinite(ev->u.pencil.ny) || !isfinite(ev->u.pencil.pressure) ||
        !isfinite(ev->u.pencil.tilt_x) || !isfinite(ev->u.pencil.tilt_y))) ||
      (AF_EV_SEC_INPUT == ev->kind && ev->u.sec_input.active > 1u))
  {
    return AF_DP_ERR_FORMAT;
  }
  *out_len = kFix[ev->kind];
  return AF_DP_OK;
}

static int ev_body_encode(const af_dp_ev_t *ev, uint8_t *dst)
{
  int r = 0;
  switch (ev->kind)
  {
  case AF_EV_MOUSE_ABS:
    r = put_f32_le(dst, ev->u.abs.nx) | put_f32_le(dst + 4u, ev->u.abs.ny);
    return (0 == r) ? AF_DP_OK : AF_DP_ERR_FORMAT;
  case AF_EV_MOUSE_BTN:
    dst[0] = ev->u.btn.btn;
    dst[1] = ev->u.btn.down;
    dst[2] = ev->u.btn.has_abs;
    r = put_f32_le(dst + 3u, ev->u.btn.nx) | put_f32_le(dst + 7u, ev->u.btn.ny);
    return (0 == r) ? AF_DP_OK : AF_DP_ERR_FORMAT;
  case AF_EV_KEY:
    put_u16_le(dst, ev->u.key.hid_usage);
    dst[2] = ev->u.key.down;
    return AF_DP_OK;
  case AF_EV_PENCIL:
    dst[0] = ev->u.pencil.phase;
    r = put_f32_le(dst + 1u, ev->u.pencil.nx) | put_f32_le(dst + 5u, ev->u.pencil.ny) |
        put_f32_le(dst + 9u, ev->u.pencil.pressure) | put_f32_le(dst + 13u, ev->u.pencil.tilt_x) |
        put_f32_le(dst + 17u, ev->u.pencil.tilt_y);
    return (0 == r) ? AF_DP_OK : AF_DP_ERR_FORMAT;
  case AF_EV_TEXT_COMMIT:
    put_u16_le(dst, ev->u.text.utf8_len);
    memcpy(dst + 2u, ev->u.text.utf8, ev->u.text.utf8_len);
    return AF_DP_OK;
  case AF_EV_SEC_INPUT:
    dst[0] = ev->u.sec_input.active;
    return AF_DP_OK;
  default:
    return AF_DP_ERR_FORMAT;
  }
}

static int ev_body_decode(af_dp_ev_t *ev, const uint8_t *src, uint16_t blen)
{
  static const uint8_t kFix[7] = {0u, 8u, 11u, 3u, 21u, 0u, 1u};
  int r = 0;
  if (ev->kind < AF_EV_MOUSE_ABS || ev->kind > AF_EV_SEC_INPUT ||
      (0u != kFix[ev->kind] && blen != kFix[ev->kind]))
  {
    return AF_DP_ERR_FORMAT;
  }
  switch (ev->kind)
  {
  case AF_EV_MOUSE_ABS:
    r = get_f32_clamped(src, 0.0f, 1.0f, &ev->u.abs.nx) |
        get_f32_clamped(src + 4u, 0.0f, 1.0f, &ev->u.abs.ny);
    return (0 == r) ? AF_DP_OK : AF_DP_ERR_FORMAT;
  case AF_EV_MOUSE_BTN:
    ev->u.btn.btn = src[0];
    ev->u.btn.down = src[1];
    ev->u.btn.has_abs = src[2];
    r = get_f32_clamped(src + 3u, 0.0f, 1.0f, &ev->u.btn.nx) |
        get_f32_clamped(src + 7u, 0.0f, 1.0f, &ev->u.btn.ny);
    return (0 == r && src[0] <= 2u && src[1] <= 1u && src[2] <= 1u) ? AF_DP_OK : AF_DP_ERR_FORMAT;
  case AF_EV_KEY:
    ev->u.key.hid_usage = get_u16_le(src);
    ev->u.key.down = src[2];
    return (src[2] <= 1u) ? AF_DP_OK : AF_DP_ERR_FORMAT;
  case AF_EV_PENCIL:
    ev->u.pencil.phase = src[0];
    r = get_f32_clamped(src + 1u, 0.0f, 1.0f, &ev->u.pencil.nx) |
        get_f32_clamped(src + 5u, 0.0f, 1.0f, &ev->u.pencil.ny) |
        get_f32_clamped(src + 9u, 0.0f, 1.0f, &ev->u.pencil.pressure) |
        get_f32_clamped(src + 13u, -1.0f, 1.0f, &ev->u.pencil.tilt_x) |
        get_f32_clamped(src + 17u, -1.0f, 1.0f, &ev->u.pencil.tilt_y);
    return (0 == r && src[0] <= AF_PENCIL_CANCEL) ? AF_DP_OK : AF_DP_ERR_FORMAT;
  case AF_EV_TEXT_COMMIT:
    if (blen < 2u || 0u == (ev->u.text.utf8_len = get_u16_le(src)) ||
        ev->u.text.utf8_len > AF_DP_TEXT_MAX_BYTES ||
        blen != (uint16_t)(2u + ev->u.text.utf8_len) ||
        NULL != memchr(src + 2u, '\0', ev->u.text.utf8_len))
    {
      return AF_DP_ERR_FORMAT;
    }
    memcpy(ev->u.text.utf8, src + 2u, ev->u.text.utf8_len);
    return AF_DP_OK;
  case AF_EV_SEC_INPUT:
    ev->u.sec_input.active = src[0];
    return (src[0] <= 1u) ? AF_DP_OK : AF_DP_ERR_FORMAT;
  default:
    return AF_DP_ERR_FORMAT;
  }
}

int af_dp_in_encode(const af_dp_in_pkt_t *pkt, uint8_t *buf, size_t cap, size_t *out_len)
{
  size_t pos = AF_DP_IN_HDR_LEN;
  size_t i = 0u;
  if (NULL == pkt || NULL == buf || NULL == out_len)
  {
    return AF_DP_ERR_ARG;
  }
  if ((0u != pkt->hdr.ver && AF_DP_IN_VER != pkt->hdr.ver) || pkt->hdr.ev_cnt > AF_DP_IN_MAX_EVS ||
      cap < AF_DP_IN_HDR_LEN)
  {
    return (0u != pkt->hdr.ver && AF_DP_IN_VER != pkt->hdr.ver)
               ? AF_DP_ERR_FORMAT
               : ((pkt->hdr.ev_cnt > AF_DP_IN_MAX_EVS) ? AF_DP_ERR_LIMIT : AF_DP_ERR_SHORT);
  }
  buf[0] = AF_DP_IN_VER;
  buf[1] = pkt->hdr.flags;
  buf[2] = pkt->hdr.ev_cnt;
  buf[3] = pkt->hdr.reserved;
  put_u32_le(buf + 4u, pkt->hdr.ack_ev_seq);
  put_u32_le(buf + 8u, pkt->hdr.cum_dx);
  put_u32_le(buf + 12u, pkt->hdr.cum_dy);
  put_u32_le(buf + 16u, pkt->hdr.cum_wx);
  put_u32_le(buf + 20u, pkt->hdr.cum_wy);
  put_u64_le(buf + 24u, pkt->hdr.client_us);
  for (i = 0u; i < pkt->hdr.ev_cnt; i++)
  {
    uint16_t blen = 0u;
    size_t need = 0u;
    int rc = ev_body_len(&pkt->evs[i], &blen);
    if (AF_DP_OK != rc)
    {
      return rc;
    }
    need = AF_DP_IN_TLV_HDR_LEN + (size_t)blen;
    if (pos + need > AF_DP_IN_PLAIN_MAX || pos + need > cap)
    {
      return (pos + need > AF_DP_IN_PLAIN_MAX) ? AF_DP_ERR_LIMIT : AF_DP_ERR_SHORT;
    }
    put_u32_le(buf + pos, pkt->evs[i].ev_seq);
    buf[pos + 4u] = pkt->evs[i].kind;
    put_u16_le(buf + pos + 5u, blen);
    if (AF_DP_OK != (rc = ev_body_encode(&pkt->evs[i], buf + pos + AF_DP_IN_TLV_HDR_LEN)))
    {
      return rc;
    }
    pos += need;
  }
  *out_len = pos;
  return AF_DP_OK;
}

int af_dp_in_decode(af_dp_in_pkt_t *pkt, const uint8_t *buf, size_t len)
{
  af_dp_in_pkt_t tmp;
  size_t pos = AF_DP_IN_HDR_LEN;
  size_t i = 0u;
  if (NULL == pkt || NULL == buf)
  {
    return AF_DP_ERR_ARG;
  }
  if (len < AF_DP_IN_HDR_LEN || len > AF_DP_IN_PLAIN_MAX)
  {
    return (len < AF_DP_IN_HDR_LEN) ? AF_DP_ERR_SHORT : AF_DP_ERR_LIMIT;
  }
  memset(&tmp, 0, sizeof(tmp));
  tmp.hdr.ver = buf[0];
  tmp.hdr.flags = buf[1];
  tmp.hdr.ev_cnt = buf[2];
  tmp.hdr.reserved = buf[3];
  if (AF_DP_IN_VER != tmp.hdr.ver || tmp.hdr.ev_cnt > AF_DP_IN_MAX_EVS)
  {
    return (AF_DP_IN_VER != tmp.hdr.ver) ? AF_DP_ERR_FORMAT : AF_DP_ERR_LIMIT;
  }
  tmp.hdr.ack_ev_seq = get_u32_le(buf + 4u);
  tmp.hdr.cum_dx = get_u32_le(buf + 8u);
  tmp.hdr.cum_dy = get_u32_le(buf + 12u);
  tmp.hdr.cum_wx = get_u32_le(buf + 16u);
  tmp.hdr.cum_wy = get_u32_le(buf + 20u);
  tmp.hdr.client_us = get_u64_le(buf + 24u);
  for (i = 0u; i < tmp.hdr.ev_cnt; i++)
  {
    uint16_t blen = 0u;
    int rc = AF_DP_OK;
    if (len - pos < AF_DP_IN_TLV_HDR_LEN)
    {
      return AF_DP_ERR_SHORT;
    }
    tmp.evs[i].ev_seq = get_u32_le(buf + pos);
    tmp.evs[i].kind = buf[pos + 4u];
    blen = get_u16_le(buf + pos + 5u);
    pos += AF_DP_IN_TLV_HDR_LEN;
    if (len - pos < (size_t)blen || AF_DP_OK != (rc = ev_body_decode(&tmp.evs[i], buf + pos, blen)))
    {
      return (len - pos < (size_t)blen) ? AF_DP_ERR_SHORT : rc;
    }
    pos += (size_t)blen;
  }
  if (pos != len)
  {
    return AF_DP_ERR_FORMAT;
  }
  *pkt = tmp;
  return AF_DP_OK;
}

/* ---- 64-bit Sliding Replay Window & Cumulative Delta ---- */

void af_dp_replay_init(af_dp_replay_t *w)
{
  if (NULL != w)
  {
    w->max_seq = 0ull;
    w->bitmap = 0ull;
  }
}

int af_dp_replay_check(af_dp_replay_t *w, uint64_t seq, int *out_is_newest)
{
  uint64_t diff = 0ull;
  if (NULL == w || NULL == out_is_newest || 0ull == seq || seq >= AF_DP_SEQ_MAX_SAFE)
  {
    return (NULL == w || NULL == out_is_newest)
               ? AF_DP_ERR_ARG
               : ((0ull == seq) ? AF_DP_ERR_REPLAY : AF_DP_ERR_LIMIT);
  }
  if (seq > w->max_seq)
  {
    diff = seq - w->max_seq;
    w->bitmap = (0ull == w->max_seq || diff >= 64ull) ? 1ull : ((w->bitmap << diff) | 1ull);
    w->max_seq = seq;
    *out_is_newest = 1;
    return AF_DP_OK;
  }
  diff = w->max_seq - seq;
  if (diff >= 64ull || 0ull != (w->bitmap & (1ull << diff)))
  {
    return AF_DP_ERR_REPLAY;
  }
  w->bitmap |= (1ull << diff);
  *out_is_newest = 0;
  return AF_DP_OK;
}

void af_dp_cum_init(af_dp_cum_state_t *st)
{
  if (NULL != st)
  {
    memset(st, 0, sizeof(*st));
  }
}

void af_dp_cum_step(af_dp_cum_state_t *st, const af_dp_in_hdr_t *hdr, int is_newest,
                    af_dp_delta_t *out)
{
  if (NULL != out)
  {
    memset(out, 0, sizeof(*out));
  }
  if (NULL == st || NULL == hdr || NULL == out || (st->initialized && 0 == is_newest))
  {
    return;
  }
  if (st->initialized)
  {
    out->dx = u32_diff_to_i32(hdr->cum_dx, st->last_dx);
    out->dy = u32_diff_to_i32(hdr->cum_dy, st->last_dy);
    out->wx = u32_diff_to_i32(hdr->cum_wx, st->last_wx);
    out->wy = u32_diff_to_i32(hdr->cum_wy, st->last_wy);
  }
  st->last_dx = hdr->cum_dx;
  st->last_dy = hdr->cum_dy;
  st->last_wx = hdr->cum_wx;
  st->last_wy = hdr->cum_wy;
  st->initialized = true;
}

/* ---- Reliable Retransmit Queue & Receiver Deduplication ---- */

void af_dp_txq_init(af_dp_txq_t *q)
{
  if (NULL != q)
  {
    memset(q, 0, sizeof(*q));
    q->next_ev_seq = 1u;
  }
}

int af_dp_txq_push(af_dp_txq_t *q, const af_dp_ev_t *ev, uint32_t *out_seq)
{
  uint16_t blen = 0u;
  size_t i = 0u;
  int rc = AF_DP_OK;
  if (NULL == q || NULL == ev || AF_DP_OK != (rc = ev_body_len(ev, &blen)) ||
      AF_DP_IN_TLV_HDR_LEN + (size_t)blen > AF_DP_IN_PAYLOAD_BUDGET)
  {
    return (NULL == q || NULL == ev) ? AF_DP_ERR_ARG : ((AF_DP_OK != rc) ? rc : AF_DP_ERR_LIMIT);
  }
  for (i = 0u; i < AF_DP_TXQ_CAP && q->count < AF_DP_TXQ_CAP; i++)
  {
    if (!q->items[i].active)
    {
      uint32_t seq = (0u == q->next_ev_seq) ? 1u : q->next_ev_seq;
      q->next_ev_seq = (0xFFFFFFFFu == seq) ? 1u : (seq + 1u);
      q->items[i].ev = *ev;
      q->items[i].ev.ev_seq = seq;
      q->items[i].last_tx_ms = AF_DP_TXQ_UNSENT;
      q->items[i].active = true;
      q->count++;
      if (NULL != out_seq)
      {
        *out_seq = seq;
      }
      return AF_DP_OK;
    }
  }
  return AF_DP_ERR_FULL;
}

void af_dp_txq_ack(af_dp_txq_t *q, uint32_t ack_ev_seq)
{
  size_t i = 0u;
  if (NULL == q || 0u == ack_ev_seq)
  {
    return;
  }
  for (i = 0u; i < AF_DP_TXQ_CAP; i++)
  {
    if (q->items[i].active && u32_diff_to_i32(ack_ev_seq, q->items[i].ev.ev_seq) >= 0)
    {
      q->items[i].active = false;
      q->count = (q->count > 0u) ? (q->count - 1u) : 0u;
    }
  }
}

static size_t txq_collect_sorted(const af_dp_txq_t *q, uint64_t now_ms, uint32_t rtx_ms,
                                 uint8_t out_idx[AF_DP_TXQ_CAP])
{
  size_t n = 0u;
  size_t i = 0u;
  for (i = 0u; i < AF_DP_TXQ_CAP; i++)
  {
    const af_dp_txq_item_t *it = &q->items[i];
    bool due =
        it->active && (AF_DP_TXQ_UNSENT == it->last_tx_ms ||
                       (now_ms > it->last_tx_ms && (now_ms - it->last_tx_ms) >= (uint64_t)rtx_ms));
    if (due)
    {
      size_t j = n;
      while (j > 0u && u32_diff_to_i32(q->items[out_idx[j - 1u]].ev.ev_seq, it->ev.ev_seq) > 0)
      {
        out_idx[j] = out_idx[j - 1u];
        j--;
      }
      out_idx[j] = (uint8_t)i;
      n++;
    }
  }
  return n;
}

size_t af_dp_txq_pack(af_dp_txq_t *q, uint64_t now_ms, uint32_t rtx_ms, af_dp_in_pkt_t *pkt)
{
  uint8_t idx[AF_DP_TXQ_CAP];
  size_t n = 0u;
  size_t k = 0u;
  size_t packed = 0u;
  size_t used = 0u;
  if (NULL == q || NULL == pkt)
  {
    return 0u;
  }
  pkt->hdr.ver = (0u == pkt->hdr.ver) ? AF_DP_IN_VER : pkt->hdr.ver;
  n = txq_collect_sorted(q, now_ms, rtx_ms, idx);
  for (k = 0u; k < n && packed < AF_DP_IN_MAX_EVS; k++)
  {
    af_dp_txq_item_t *it = &q->items[idx[k]];
    uint16_t blen = 0u;
    size_t wlen = 0u;
    if (AF_DP_OK != ev_body_len(&it->ev, &blen) ||
        (wlen = AF_DP_IN_TLV_HDR_LEN + (size_t)blen) > AF_DP_IN_PAYLOAD_BUDGET)
    {
      it->last_tx_ms = now_ms;
      continue;
    }
    if (used + wlen > AF_DP_IN_PAYLOAD_BUDGET)
    {
      break;
    }
    pkt->evs[packed++] = it->ev;
    used += wlen;
    it->last_tx_ms = now_ms;
  }
  pkt->hdr.ev_cnt = (uint8_t)packed;
  return packed;
}

void af_dp_rx_dedup_init(af_dp_rx_dedup_t *d)
{
  if (NULL != d)
  {
    d->last_ev_seq = 0u;
  }
}

bool af_dp_rx_dedup_accept(af_dp_rx_dedup_t *d, uint32_t ev_seq)
{
  if (NULL == d || 0u == ev_seq ||
      ev_seq != ((0xFFFFFFFFu == d->last_ev_seq) ? 1u : (d->last_ev_seq + 1u)))
  {
    return false;
  }
  d->last_ev_seq = ev_seq;
  return true;
}
