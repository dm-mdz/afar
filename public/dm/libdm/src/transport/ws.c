/**
 ******************************************************************************
 * @file    ws.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   WebSocket 传输口（RFC 6455，DD-01）：升级、收帧拼消息、发帧、保活；握手要的
 * SHA-1、Base64。
 *
 * 升级以后这条连接就按 link 的规矩走（握手、请求和应答、推送、反向调用），每条二进制消息
 * 恰好装一个 16 字节帧头的帧。规矩：只收二进制（文本回 1003）、客户端的帧必须带掩码（不带 1002）、
 * 分片拼起来再交（超过上限 1009）、ping 回 pong、close 回 close 再断；每 DM_WS_PING_MS 发 ping，
 * 两个周期没有 pong 就断。核心发的帧不带掩码。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "ws"
#include <dm.h>
#include <dm/frame.h>
#include <dm/log.h>
#include <dm/limits.h>

#include <stdio.h>  /* snprintf */
#include <string.h> /* memcpy / memmove / strlen */

#include "conn.h"

#define DM__WS_OP_CONT 0x0u
#define DM__WS_OP_TEXT 0x1u
#define DM__WS_OP_BIN 0x2u
#define DM__WS_OP_CLOSE 0x8u
#define DM__WS_OP_PING 0x9u
#define DM__WS_OP_PONG 0xAu
#define DM__WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

typedef struct dm__ws
{
  dm_uv_timer_st stPing; /* 首成员：保活 */
  dm__conn_st *pstConn;  /* 连接断了就是 NULL（等定时器关完再释放） */
  uint8 *pu8In;          /* 收进来还没解完的字节 */
  uint32 u32InLen;
  uint32 u32InCap;
  uint8 *pu8Msg; /* 分片拼到一半的消息 */
  uint32 u32MsgLen;
  uint32 u32MsgCap;
  bool_t bInMsg;
  bool_t bHadSensitive; /* AUD-06：曾收过 DM_FRAME_F_SENSITIVE 帧，释放/缩容前全量擦零 */
  uint64 u64LastPongMs;
} dm__ws_st;

/* ============================ SHA-1 和 Base64（公有领域的写法） ============================ */

#define DM__ROL(v, n) (((v) << (n)) | ((v) >> (32u - (n))))

static void dm__sha1_block(dm_io uint32 *pu32H, dm_in const uint8 *pu8Blk)
{
  uint32 au32W[80] = {0};
  uint32 u32A = pu32H[0];
  uint32 u32B = pu32H[1];
  uint32 u32C = pu32H[2];
  uint32 u32D = pu32H[3];
  uint32 u32E = pu32H[4];
  uint32 u32Idx = 0;

  for (u32Idx = 0; u32Idx < 16u; u32Idx++)
  {
    au32W[u32Idx] = ((uint32)pu8Blk[u32Idx * 4u] << 24) | ((uint32)pu8Blk[u32Idx * 4u + 1u] << 16) |
                    ((uint32)pu8Blk[u32Idx * 4u + 2u] << 8) | (uint32)pu8Blk[u32Idx * 4u + 3u];
  }
  for (u32Idx = 16; u32Idx < 80u; u32Idx++)
  {
    uint32 u32X =
        au32W[u32Idx - 3u] ^ au32W[u32Idx - 8u] ^ au32W[u32Idx - 14u] ^ au32W[u32Idx - 16u];
    au32W[u32Idx] = DM__ROL(u32X, 1u);
  }
  for (u32Idx = 0; u32Idx < 80u; u32Idx++)
  {
    uint32 u32F = 0;
    uint32 u32K = 0;
    uint32 u32T = 0;
    if (u32Idx < 20u)
    {
      u32F = (u32B & u32C) | ((~u32B) & u32D);
      u32K = 0x5A827999u;
    }
    else if (u32Idx < 40u)
    {
      u32F = u32B ^ u32C ^ u32D;
      u32K = 0x6ED9EBA1u;
    }
    else if (u32Idx < 60u)
    {
      u32F = (u32B & u32C) | (u32B & u32D) | (u32C & u32D);
      u32K = 0x8F1BBCDCu;
    }
    else
    {
      u32F = u32B ^ u32C ^ u32D;
      u32K = 0xCA62C1D6u;
    }
    u32T = DM__ROL(u32A, 5u) + u32F + u32E + u32K + au32W[u32Idx];
    u32E = u32D;
    u32D = u32C;
    u32C = DM__ROL(u32B, 30u);
    u32B = u32A;
    u32A = u32T;
  }
  pu32H[0] += u32A;
  pu32H[1] += u32B;
  pu32H[2] += u32C;
  pu32H[3] += u32D;
  pu32H[4] += u32E;
  return;
}

void dm__sha1(dm_in const void *pv, dm_in size_t uLen, dm_out uint8 au8Out[20])
{
  uint32 au32H[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  const uint8 *pu8 = (const uint8 *)pv;
  uint8 au8Blk[64] = {0};
  size_t uLeft = uLen;
  uint64 u64Bits = (uint64)uLen * 8u;
  uint32 u32Idx = 0;

  while (uLeft >= 64u)
  {
    dm__sha1_block(au32H, pu8);
    pu8 += 64;
    uLeft -= 64u;
  }
  memset(au8Blk, 0, sizeof(au8Blk));
  memcpy(au8Blk, pu8, uLeft);
  au8Blk[uLeft] = 0x80u;
  if (uLeft >= 56u)
  {
    dm__sha1_block(au32H, au8Blk);
    memset(au8Blk, 0, sizeof(au8Blk));
  }
  for (u32Idx = 0; u32Idx < 8u; u32Idx++)
  {
    au8Blk[63u - u32Idx] = (uint8)(u64Bits >> (u32Idx * 8u));
  }
  dm__sha1_block(au32H, au8Blk);
  for (u32Idx = 0; u32Idx < 20u; u32Idx++)
  {
    au8Out[u32Idx] = (uint8)(au32H[u32Idx / 4u] >> (24u - (u32Idx % 4u) * 8u));
  }
  return;
}

/* Base64 编码；pcOut 至少 (uLen + 2) / 3 * 4 + 1 字节 */
void dm__base64(dm_in const uint8 *pu8, dm_in size_t uLen, dm_out char *pcOut)
{
  static const char acTab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t uIdx = 0;
  size_t uOut = 0;

  for (uIdx = 0; uIdx + 2u < uLen; uIdx += 3u)
  {
    uint32 u32V = ((uint32)pu8[uIdx] << 16) | ((uint32)pu8[uIdx + 1u] << 8) | pu8[uIdx + 2u];
    pcOut[uOut++] = acTab[(u32V >> 18) & 0x3Fu];
    pcOut[uOut++] = acTab[(u32V >> 12) & 0x3Fu];
    pcOut[uOut++] = acTab[(u32V >> 6) & 0x3Fu];
    pcOut[uOut++] = acTab[u32V & 0x3Fu];
  }
  if (uIdx < uLen)
  {
    uint32 u32V = (uint32)pu8[uIdx] << 16;
    if (uIdx + 1u < uLen)
    {
      u32V |= (uint32)pu8[uIdx + 1u] << 8;
    }
    pcOut[uOut++] = acTab[(u32V >> 18) & 0x3Fu];
    pcOut[uOut++] = acTab[(u32V >> 12) & 0x3Fu];
    pcOut[uOut++] = (uIdx + 1u < uLen) ? acTab[(u32V >> 6) & 0x3Fu] : '=';
    pcOut[uOut++] = '=';
  }
  pcOut[uOut] = '\0';
  return;
}

/* Sec-WebSocket-Accept = Base64(SHA-1(key + GUID)) */
void dm__ws_accept_key(dm_in const char *pcKey, dm_out char acOut[32])
{
  char acCat[128] = {0};
  uint8 au8Sum[20] = {0};
  int iLen = snprintf(acCat, sizeof(acCat), "%s%s", (NULL != pcKey) ? pcKey : "", DM__WS_GUID);
  size_t uLen = 0u;

  if (iLen > 0)
  {
    uLen = ((size_t)iLen >= sizeof(acCat)) ? (sizeof(acCat) - 1u) : (size_t)iLen;
  }
  dm__sha1(acCat, uLen, au8Sum);
  dm__base64(au8Sum, sizeof(au8Sum), acOut);
  return;
}

/* ============================ 发 ============================ */

uint32 dm__ws_head(dm_out uint8 *pu8Out, dm_in uint8 u8Op, dm_in uint64 u64Len)
{
  pu8Out[0] = (uint8)(0x80u | u8Op); /* FIN，不分片 */
  if (u64Len < 126u)
  {
    pu8Out[1] = (uint8)u64Len;
    return 2u;
  }
  if (u64Len <= 0xFFFFu)
  {
    pu8Out[1] = 126u;
    dm_frame_wr16(pu8Out + 2, (uint32)u64Len);
    return 4u;
  }
  pu8Out[1] = 127u;
  dm_frame_wr64(pu8Out + 2, u64Len);
  return 10u;
}

static void dm__ws_send_ctl(dm_io dm__conn_st *pstConn, dm_in uint8 u8Op, dm_in const uint8 *pu8,
                            dm_in uint32 u32Len)
{
  uint8 au8Frame[2 + 125] = {0};
  uint32 u32Head = dm__ws_head(au8Frame, u8Op, u32Len);
  if (u32Len > 125u)
  {
    return;
  }
  if (u32Len > 0u)
  {
    memcpy(au8Frame + u32Head, pu8, u32Len);
  }
  (void)dm__conn_write_raw(pstConn, au8Frame, u32Head + u32Len);
  return;
}

/* 回一个关闭码，写完再断 */
static void dm__ws_close(dm_io dm__conn_st *pstConn, dm_in uint32 u32Code, dm_in const char *pcWhy)
{
  uint8 au8Code[2] = {0};
  DM_LOG_DEBUG("conn %u: websocket closing with %u (%s)", pstConn->u32Id, (unsigned)u32Code, pcWhy);
  dm_frame_wr16(au8Code, u32Code);
  dm__ws_send_ctl(pstConn, DM__WS_OP_CLOSE, au8Code, 2u);
  dm__conn_close_after_write(pstConn);
  return;
}

/* ============================ 收 ============================ */

static uint64 dm__ws_now_ms(void)
{
  return dm_hrtime() / 1000000u;
}

/* 一条完整的二进制消息：恰好一帧 */
static int dm__ws_message(dm_io dm__conn_st *pstConn, dm_io uint8 *pu8Msg, dm_in uint32 u32Len)
{
  dm_frame_head_st stHead = {0};
  bool_t bSens = DM_FALSE;
  int iRet = 0;

  if (u32Len < DM_FRAME_HEAD)
  {
    dm__ws_close(pstConn, 1002u, "message shorter than a frame head");
    return 1;
  }
  dm_frame_get(pu8Msg, &stHead);
  bSens = ((0u != (stHead.u8Flag & DM_FRAME_F_SENSITIVE)) || (DM_LINKID_HELLO == stHead.u16LinkID))
              ? DM_TRUE
              : DM_FALSE;
  if (bSens)
  {
    dm__ws_st *pstWs = (dm__ws_st *)pstConn->pvWs;
    pstConn->bHadSensitive = DM_TRUE;
    if (NULL != pstWs)
    {
      pstWs->bHadSensitive = DM_TRUE;
    }
  }
  if (stHead.u32Len != u32Len - DM_FRAME_HEAD)
  {
    if (bSens)
    {
      dm_secure_zero(pu8Msg, u32Len);
    }
    dm__ws_close(pstConn, 1002u, "frame length does not match the message");
    return 1;
  }
  iRet = dm__conn_on_frame(pstConn, &stHead, pu8Msg + DM_FRAME_HEAD);
  if (bSens)
  {
    dm_secure_zero(pu8Msg, u32Len);
  }
  return iRet;
}

static int dm__ws_grow_ex(dm_io uint8 **ppu8, dm_io uint32 *pu32Cap, dm_in uint32 u32UsedLen,
                          dm_in uint32 u32Need, dm_in bool_t bSecure)
{
  uint8 *pu8New = NULL;
  uint32 u32OldCap = *pu32Cap;
  uint32 u32Cap = (0u != u32OldCap) ? u32OldCap : 4096u;
  if (u32Need <= u32OldCap)
  {
    return DM_SUCCESS;
  }
  while (u32Cap < u32Need)
  {
    if (u32Cap > (0xFFFFFFFFu / 2u))
    {
      u32Cap = u32Need;
      break;
    }
    u32Cap *= 2u;
  }
  if (bSecure && (NULL != *ppu8))
  {
    uint32 u32Copy = (u32UsedLen < u32OldCap) ? u32UsedLen : u32OldCap;
    pu8New = (uint8 *)dm_malloc_tag(u32Cap, DM_MEM_TAG_WS);
    if (NULL == pu8New)
    {
      return DM_ALLOC;
    }
    if (u32Copy > 0u)
    {
      memcpy(pu8New, *ppu8, u32Copy);
    }
    dm_secure_zero(*ppu8, u32OldCap);
    dm_free_tag(*ppu8);
  }
  else
  {
    pu8New = (NULL == *ppu8) ? (uint8 *)dm_malloc_tag(u32Cap, DM_MEM_TAG_WS)
                             : (uint8 *)dm_realloc_tag(*ppu8, u32Cap);
    if (NULL == pu8New)
    {
      return DM_ALLOC;
    }
  }
  *ppu8 = pu8New;
  *pu32Cap = u32Cap;
  return DM_SUCCESS;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static int dm__ws_grow(dm_io uint8 **ppu8, dm_io uint32 *pu32Cap, dm_in uint32 u32Need)
{
  return dm__ws_grow_ex(ppu8, pu32Cap, *pu32Cap, u32Need, DM_TRUE);
}

/* 解一帧；返回用掉的字节数，0 = 还不够一帧，负数 = 连接开始关了 */
static int64 dm__ws_one(dm_io dm__ws_st *pstWs, dm_in const uint8 *pu8, dm_in uint32 u32Have)
{
  dm__conn_st *pstConn = pstWs->pstConn;
  uint8 u8Op = 0;
  bool_t bFin = DM_FALSE;
  uint64 u64Len = 0;
  uint32 u32Off = 2;
  uint32 u32Max = (pstConn->bAuthed ? dm__link_frame_max() : DM_PRE_AUTH_MAX) + DM_FRAME_HEAD;
  const uint8 *pu8Mask = NULL;
  uint8 *pu8Dst = NULL;
  uint64 u64Idx = 0;

  if (u32Have < 2u)
  {
    return 0;
  }
  bFin = (0u != (pu8[0] & 0x80u)) ? DM_TRUE : DM_FALSE;
  u8Op = pu8[0] & 0x0Fu;
  u64Len = pu8[1] & 0x7Fu;
  if (0u == (pu8[1] & 0x80u))
  {
    dm__ws_close(pstConn, 1002u, "client frame without mask");
    return -1;
  }
  if (126u == u64Len)
  {
    if (u32Have < 4u)
    {
      return 0;
    }
    u64Len = dm_frame_rd16(pu8 + 2);
    u32Off = 4;
  }
  else if (127u == u64Len)
  {
    if (u32Have < 10u)
    {
      return 0;
    }
    u64Len = dm_frame_rd64(pu8 + 2);
    if (0u != (u64Len & ((uint64)1u << 63)))
    {
      dm__ws_close(pstConn, 1002u, "invalid 64-bit payload length");
      return -1;
    }
    u32Off = 10;
  }
  if ((u8Op >= DM__WS_OP_CLOSE) && (!bFin || (u64Len > 125u)))
  {
    dm__ws_close(pstConn, 1002u, "bad control frame");
    return -1;
  }
  if ((u8Op < DM__WS_OP_CLOSE) &&
      ((u64Len > (uint64)u32Max) || (u64Len > (uint64)(u32Max - pstWs->u32MsgLen))))
  {
    dm__ws_close(pstConn, 1009u, "message too big"); /* 不等它传完：先看长度 */
    return -1;
  }
  if ((u64Len > (uint64)u32Have) || (((uint64)u32Have - u64Len) < ((uint64)u32Off + 4u)))
  {
    return 0;
  }
  pu8Mask = pu8 + u32Off;
  u32Off += 4u;
  switch (u8Op)
  {
  case DM__WS_OP_TEXT:
    dm__ws_close(pstConn, 1003u, "text message");
    return -1;
  case DM__WS_OP_PING:
  {
    uint8 au8Data[125] = {0};
    for (u64Idx = 0; u64Idx < u64Len; u64Idx++)
    {
      au8Data[u64Idx] = pu8[u32Off + u64Idx] ^ pu8Mask[u64Idx % 4u];
    }
    dm__ws_send_ctl(pstConn, DM__WS_OP_PONG, au8Data, (uint32)u64Len);
    return (int64)(u32Off + u64Len);
  }
  case DM__WS_OP_PONG:
    pstWs->u64LastPongMs = dm__ws_now_ms();
    return (int64)(u32Off + u64Len);
  case DM__WS_OP_CLOSE:
    dm__ws_close(pstConn, 1000u, "peer said goodbye");
    return -1;
  case DM__WS_OP_BIN:
  case DM__WS_OP_CONT:
    if ((DM__WS_OP_CONT == u8Op) != pstWs->bInMsg)
    {
      dm__ws_close(pstConn, 1002u, "bad fragment sequence");
      return -1;
    }
    break;
  default:
    dm__ws_close(pstConn, 1002u, "unknown opcode");
    return -1;
  }
  if (DM_SUCCESS != dm__ws_grow_ex(&pstWs->pu8Msg, &pstWs->u32MsgCap, pstWs->u32MsgLen,
                                   pstWs->u32MsgLen + (uint32)u64Len, DM_TRUE))
  {
    dm__conn_close(pstConn, "out of memory");
    return -1;
  }
  if (u64Len > 0u)
  {
    pu8Dst = pstWs->pu8Msg + pstWs->u32MsgLen;
    for (u64Idx = 0; u64Idx < u64Len; u64Idx++)
    {
      pu8Dst[u64Idx] = pu8[u32Off + u64Idx] ^ pu8Mask[u64Idx % 4u];
    }
  }
  pstWs->u32MsgLen += (uint32)u64Len;
  if ((!pstWs->bHadSensitive) && (pstWs->u32MsgLen >= DM_FRAME_HEAD))
  {
    dm_frame_head_st stPeek = {0};
    dm_frame_get(pstWs->pu8Msg, &stPeek);
    if ((0u != (stPeek.u8Flag & DM_FRAME_F_SENSITIVE)) || (DM_LINKID_HELLO == stPeek.u16LinkID))
    {
      pstWs->bHadSensitive = DM_TRUE;
      pstConn->bHadSensitive = DM_TRUE;
    }
  }
  pstWs->bInMsg = bFin ? DM_FALSE : DM_TRUE;
  if (bFin)
  {
    uint32 u32Msg = pstWs->u32MsgLen;
    pstWs->u32MsgLen = 0;
    if (0 != dm__ws_message(pstConn, pstWs->pu8Msg, u32Msg))
    {
      return -1;
    }
    if (pstWs->u32MsgCap > DM__RX_KEEP)
    {
      if (pstWs->bHadSensitive)
      {
        dm_secure_zero(pstWs->pu8Msg, pstWs->u32MsgCap);
      }
      dm_free_tag(pstWs->pu8Msg); /* 大消息交完就把缓冲还回去 */
      pstWs->pu8Msg = NULL;
      pstWs->u32MsgCap = 0;
    }
  }
  return (int64)(u32Off + u64Len);
}

void dm__ws_conn_data(dm_io dm__conn_st *pstConn, dm_in const uint8 *pu8, dm_in uint32 u32Len)
{
  dm__ws_st *pstWs = (dm__ws_st *)pstConn->pvWs;
  uint32 u32Off = 0;

  if ((NULL == pstWs) || (0u == u32Len))
  {
    return;
  }
  if ((u32Len > (0xFFFFFFFFu - pstWs->u32InLen)) ||
      (DM_SUCCESS != dm__ws_grow_ex(&pstWs->pu8In, &pstWs->u32InCap, pstWs->u32InLen,
                                    pstWs->u32InLen + u32Len, DM_TRUE)))
  {
    dm__conn_close(pstConn, "out of memory");
    return;
  }
  memcpy(pstWs->pu8In + pstWs->u32InLen, pu8, u32Len);
  pstWs->u32InLen += u32Len;
  while ((u32Off < pstWs->u32InLen) && (NULL != pstWs->pstConn) && !dm__conn_dead(pstWs->pstConn))
  {
    int64 i64Used = dm__ws_one(pstWs, pstWs->pu8In + u32Off, pstWs->u32InLen - u32Off);
    if (i64Used < 0)
    {
      /* AUD-06 冷路径兜底：WS 协议异常断开时无条件擦零原始入站与分片重组缓冲 */
      if ((NULL != pstWs->pu8In) && (pstWs->u32InCap > 0u))
      {
        dm_secure_zero(pstWs->pu8In, pstWs->u32InCap);
      }
      if ((NULL != pstWs->pu8Msg) && (pstWs->u32MsgCap > 0u))
      {
        dm_secure_zero(pstWs->pu8Msg, pstWs->u32MsgCap);
      }
      return; /* 连接开始关了：剩下的不要了 */
    }
    if (0 == i64Used)
    {
      break;
    }
    u32Off += (uint32)i64Used;
  }
  pstWs->u32InLen -= u32Off;
  if ((pstWs->u32InLen > 0u) && (u32Off > 0u))
  {
    memmove(pstWs->pu8In, pstWs->pu8In + u32Off, pstWs->u32InLen);
  }
  if (pstWs->bHadSensitive && (u32Off > 0u) && (NULL != pstWs->pu8In) &&
      (pstWs->u32InCap > pstWs->u32InLen))
  {
    dm_secure_zero(pstWs->pu8In + pstWs->u32InLen, pstWs->u32InCap - pstWs->u32InLen);
  }
  if ((0u == pstWs->u32InLen) && (pstWs->u32InCap > DM__RX_KEEP))
  {
    if (pstWs->bHadSensitive)
    {
      dm_secure_zero(pstWs->pu8In, pstWs->u32InCap);
    }
    dm_free_tag(pstWs->pu8In); /* 大消息处理完毕后释放临时扩容缓冲，避免长期驻留 */
    pstWs->pu8In = NULL;
    pstWs->u32InCap = 0;
  }
  return;
}

/* ============================ 升级、保活、收尾 ============================ */

static void dm__ws_on_ping(dm_in dm_uv_timer_st *pstTimer)
{
  dm__ws_st *pstWs = (dm__ws_st *)pstTimer;
  if (NULL == pstWs->pstConn)
  {
    return;
  }
  if (dm__ws_now_ms() - pstWs->u64LastPongMs >= 2u * (uint64)DM_WS_PING_MS)
  {
    DM_LOG_WARN("conn %u: no websocket pong for two periods, disconnecting", pstWs->pstConn->u32Id);
    dm__conn_close(pstWs->pstConn, "no pong");
    return;
  }
  dm__ws_send_ctl(pstWs->pstConn, DM__WS_OP_PING, NULL, 0u);
  return;
}

/* http.c 用：回 101、这条连接改成 WebSocket、开始走 link 的规矩；跟着请求一起到的字节接着解 */
int dm__ws_upgrade(dm_io dm__conn_st *pstConn, dm_in const char *pcKey, dm_in const uint8 *pu8Rest,
                   dm_in uint32 u32Rest)
{
  dm__ws_st *pstWs = NULL;
  char acAccept[32] = {0};
  char acResp[256] = {0};
  int iLen = 0;

  pstWs = (dm__ws_st *)dm_calloc_tag(1u, sizeof(dm__ws_st), DM_MEM_TAG_WS);
  if (NULL == pstWs)
  {
    return DM_ALLOC;
  }
  dm__ws_accept_key(pcKey, acAccept);
  iLen =
      snprintf(acResp, sizeof(acResp),
               "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Accept: %s\r\n\r\n",
               acAccept);
  (void)dm__conn_write_raw(pstConn, acResp, (uint32)iLen);
  DM_LOG_DEBUG("conn %u: upgraded to websocket (%u bytes already here)", pstConn->u32Id,
               (unsigned)u32Rest);
  pstWs->pstConn = pstConn;
  pstWs->u64LastPongMs = dm__ws_now_ms();
  pstConn->pvWs = pstWs;
  pstConn->u8Kind = DM__CONN_WS;
  (void)dm_uv_timer_init(dm_uv_default_loop(), &pstWs->stPing);
  (void)dm_uv_timer_start(&pstWs->stPing, dm__ws_on_ping, DM_WS_PING_MS, DM_WS_PING_MS);
  dm__conn_start_link(pstConn);
  if (u32Rest > 0u)
  {
    dm__ws_conn_data(pstConn, pu8Rest, u32Rest);
  }
  return DM_SUCCESS;
}

static void dm__ws_on_closed(dm_in dm_uv_handle_st *pstHandle)
{
  dm__ws_st *pstWs = (dm__ws_st *)pstHandle;
  /* AUD-06 冷路径兜底：连接关闭时无条件擦零 WS 缓冲（覆盖半帧断连/超限拒帧/预鉴权残帧） */
  if ((NULL != pstWs->pu8In) && (pstWs->u32InCap > 0u))
  {
    dm_secure_zero(pstWs->pu8In, pstWs->u32InCap);
  }
  if ((NULL != pstWs->pu8Msg) && (pstWs->u32MsgCap > 0u))
  {
    dm_secure_zero(pstWs->pu8Msg, pstWs->u32MsgCap);
  }
  dm_free_tag(pstWs->pu8In);
  dm_free_tag(pstWs->pu8Msg);
  dm_free_tag(pstWs);
  return;
}

void dm__ws_conn_gone(dm_io dm__conn_st *pstConn)
{
  dm__ws_st *pstWs = (dm__ws_st *)pstConn->pvWs;
  pstConn->pvWs = NULL;
  pstWs->pstConn = NULL;
  (void)dm_uv_timer_stop(&pstWs->stPing);
  dm_uv_close((dm_uv_handle_st *)&pstWs->stPing, dm__ws_on_closed);
  return;
}

#ifdef __cplusplus
}
#endif
