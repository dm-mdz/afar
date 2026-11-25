/**
 ******************************************************************************
 * @file    dmc_frame_sync.c
 * @brief   COBS + 2B Magic (0xD4 0x4D) + 16B Head + Body + 2B CRC16-CCITT 帧同步编解码器
 *          （DD-02 §2.3，PLUG-04）。纯 C11 零堆分配、零 OS/libuv 依赖。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include <dmc_frame_sync.h>

#include <stdint.h> /* SIZE_MAX */
#include <string.h> /* memset */

/* ============================ CRC16-CCITT ============================ */

uint16 dmc_crc16_ccitt(dm_in const uint8 *pu8Data, dm_in size_t uLen)
{
  uint16 u16Crc = 0xFFFFu;
  size_t uIdx = 0u;

  if (NULL == pu8Data)
  {
    return u16Crc;
  }
  for (uIdx = 0u; uIdx < uLen; uIdx++)
  {
    uint uiBit = 0u;
    u16Crc ^= (uint16)((uint16)pu8Data[uIdx] << 8);
    for (uiBit = 0u; uiBit < 8u; uiBit++)
    {
      if (0u != (u16Crc & 0x8000u))
      {
        u16Crc = (uint16)((u16Crc << 1) ^ 0x1021u);
      }
      else
      {
        u16Crc = (uint16)(u16Crc << 1);
      }
    }
  }
  return u16Crc;
}

size_t dmc_sync_max_encoded_len(dm_in size_t uBodyLen)
{
  size_t uPlain = 0u;
  size_t uCobsExtra = 0u;

  if (uBodyLen > (SIZE_MAX - DMC_SYNC_OVERHEAD))
  {
    return 0u;
  }
  uPlain = uBodyLen + DMC_SYNC_OVERHEAD;
  uCobsExtra = (uPlain / 254u) + 1u;
  if (uPlain > (SIZE_MAX - uCobsExtra - 2u))
  {
    return 0u;
  }
  return 2u + uPlain + uCobsExtra;
}

/* ============================ 流式 COBS 编码器 ============================ */

typedef struct dmc__cobs_enc
{
  uint8 *pu8Out;
  size_t uCap;
  size_t uCodeIdx;
  size_t uWriteIdx;
  uint8 u8Code;
  bool_t bOverflow;
} dmc__cobs_enc_st;

static void dmc__cobs_put_byte(dm_io dmc__cobs_enc_st *pstEnc, dm_in uint8 u8Byte)
{
  if (pstEnc->bOverflow)
  {
    return;
  }
  if (0xFFu == pstEnc->u8Code)
  {
    if (pstEnc->uWriteIdx >= pstEnc->uCap)
    {
      pstEnc->bOverflow = DM_TRUE;
      return;
    }
    pstEnc->pu8Out[pstEnc->uCodeIdx] = 0xFFu;
    pstEnc->uCodeIdx = pstEnc->uWriteIdx++;
    pstEnc->u8Code = 1u;
  }
  if (0x00u == u8Byte)
  {
    if (pstEnc->uWriteIdx >= pstEnc->uCap)
    {
      pstEnc->bOverflow = DM_TRUE;
      return;
    }
    pstEnc->pu8Out[pstEnc->uCodeIdx] = pstEnc->u8Code;
    pstEnc->uCodeIdx = pstEnc->uWriteIdx++;
    pstEnc->u8Code = 1u;
    return;
  }
  if (pstEnc->uWriteIdx >= pstEnc->uCap)
  {
    pstEnc->bOverflow = DM_TRUE;
    return;
  }
  pstEnc->pu8Out[pstEnc->uWriteIdx++] = u8Byte;
  pstEnc->u8Code++;
  return;
}

static void dmc__cobs_put_buf(dm_io dmc__cobs_enc_st *pstEnc, dm_in const uint8 *pu8Data,
                              dm_in size_t uLen)
{
  size_t uIdx = 0u;
  for (uIdx = 0u; !pstEnc->bOverflow && (uIdx < uLen); uIdx++)
  {
    dmc__cobs_put_byte(pstEnc, pu8Data[uIdx]);
  }
  return;
}

int dmc_cobs_encode(dm_in const uint8 *pu8In, dm_in size_t uInLen, dm_out uint8 *pu8Out,
                    dm_in size_t uOutCap, dm_out size_t *puOutLen)
{
  dmc__cobs_enc_st stEnc;

  if (((NULL == pu8In) && (uInLen > 0u)) || (NULL == pu8Out) || (NULL == puOutLen))
  {
    return DMC_ERR_ARG;
  }
  *puOutLen = 0u;
  if (uOutCap < 1u)
  {
    return DMC_ERR_QUEUE;
  }
  memset(&stEnc, 0, sizeof(stEnc));
  stEnc.pu8Out = pu8Out;
  stEnc.uCap = uOutCap;
  stEnc.uCodeIdx = 0u;
  stEnc.uWriteIdx = 1u;
  stEnc.u8Code = 1u;
  if (uInLen > 0u)
  {
    dmc__cobs_put_buf(&stEnc, pu8In, uInLen);
  }
  if (stEnc.bOverflow)
  {
    return DMC_ERR_QUEUE;
  }
  pu8Out[stEnc.uCodeIdx] = stEnc.u8Code;
  *puOutLen = stEnc.uWriteIdx;
  return DMC_OK;
}

int dmc_cobs_decode(dm_in const uint8 *pu8In, dm_in size_t uInLen, dm_out uint8 *pu8Out,
                    dm_in size_t uOutCap, dm_out size_t *puOutLen)
{
  size_t uRead = 0u;
  size_t uWrite = 0u;

  if (((NULL == pu8In) && (uInLen > 0u)) || ((NULL == pu8Out) && (uOutCap > 0u)) ||
      (NULL == puOutLen))
  {
    return DMC_ERR_ARG;
  }
  *puOutLen = 0u;
  if (0u == uInLen)
  {
    return DMC_ERR_WIRE;
  }
  while (uRead < uInLen)
  {
    uint8 u8Code = pu8In[uRead++];
    size_t uRun = 0u;
    size_t uIdx = 0u;

    if (0u == u8Code)
    {
      return DMC_ERR_WIRE;
    }
    uRun = (size_t)(u8Code - 1u);
    if (uRun > (uInLen - uRead))
    {
      return DMC_ERR_WIRE;
    }
    for (uIdx = 0u; uIdx < uRun; uIdx++)
    {
      uint8 u8Byte = pu8In[uRead++];
      if (0u == u8Byte)
      {
        return DMC_ERR_WIRE;
      }
      if (uWrite >= uOutCap)
      {
        return DMC_ERR_QUEUE;
      }
      pu8Out[uWrite++] = u8Byte;
    }
    if ((u8Code < 0xFFu) && (uRead < uInLen))
    {
      if (uWrite >= uOutCap)
      {
        return DMC_ERR_QUEUE;
      }
      pu8Out[uWrite++] = 0x00u;
    }
  }
  *puOutLen = uWrite;
  return DMC_OK;
}

/* 增量更新 CRC16-CCITT（避免整帧二次缓冲分配） */
static uint16 dmc__crc16_update(dm_in uint16 u16Crc, dm_in const uint8 *pu8Data, dm_in size_t uLen)
{
  size_t uIdx = 0u;
  for (uIdx = 0u; uIdx < uLen; uIdx++)
  {
    uint uiBit = 0u;
    u16Crc ^= (uint16)((uint16)pu8Data[uIdx] << 8);
    for (uiBit = 0u; uiBit < 8u; uiBit++)
    {
      u16Crc =
          (0u != (u16Crc & 0x8000u)) ? (uint16)((u16Crc << 1) ^ 0x1021u) : (uint16)(u16Crc << 1);
    }
  }
  return u16Crc;
}

int dmc_sync_encode(dm_in const dm_frame_head_st *pstHead, dm_in const void *pvBody,
                    dm_out uint8 *pu8Out, dm_in size_t uOutCap, dm_out size_t *puOutLen)
{
  uint8 au8Magic[2] = {DMC_SYNC_MAGIC_0, DMC_SYNC_MAGIC_1};
  uint8 au8Head[DM_FRAME_HEAD];
  uint8 au8Crc[2];
  uint16 u16Crc = 0xFFFFu;
  dmc__cobs_enc_st stEnc;

  if ((NULL == pstHead) || ((NULL == pvBody) && (pstHead->u32Len > 0u)) || (NULL == pu8Out) ||
      (NULL == puOutLen))
  {
    return DMC_ERR_ARG;
  }
  *puOutLen = 0u;
  if (uOutCap < 4u)
  {
    return DMC_ERR_QUEUE;
  }
  dm_frame_put(au8Head, pstHead);
  u16Crc = dmc__crc16_update(u16Crc, au8Magic, 2u);
  u16Crc = dmc__crc16_update(u16Crc, au8Head, DM_FRAME_HEAD);
  if (pstHead->u32Len > 0u)
  {
    u16Crc = dmc__crc16_update(u16Crc, (const uint8 *)pvBody, pstHead->u32Len);
  }
  dm_frame_wr16(au8Crc, u16Crc);

  memset(&stEnc, 0, sizeof(stEnc));
  stEnc.pu8Out = pu8Out;
  stEnc.uCap = uOutCap;
  pu8Out[0] = DMC_SYNC_DELIM;
  stEnc.uCodeIdx = 1u;
  stEnc.uWriteIdx = 2u;
  stEnc.u8Code = 1u;

  dmc__cobs_put_buf(&stEnc, au8Magic, 2u);
  dmc__cobs_put_buf(&stEnc, au8Head, DM_FRAME_HEAD);
  if (pstHead->u32Len > 0u)
  {
    dmc__cobs_put_buf(&stEnc, (const uint8 *)pvBody, pstHead->u32Len);
  }
  dmc__cobs_put_buf(&stEnc, au8Crc, 2u);

  if (stEnc.bOverflow || (stEnc.uWriteIdx >= uOutCap))
  {
    return DMC_ERR_QUEUE;
  }
  pu8Out[stEnc.uCodeIdx] = stEnc.u8Code;
  pu8Out[stEnc.uWriteIdx++] = DMC_SYNC_DELIM;
  *puOutLen = stEnc.uWriteIdx;
  return DMC_OK;
}

/* ============================ 流式 COBS 解码器 ============================ */

/* 原地 COBS 解码（解码后长度严格 <= 编码长度）；失败返回 DM_FALSE */
static bool_t dmc__cobs_decode_inplace(dm_io uint8 *pu8Buf, dm_in size_t uEncLen,
                                       dm_out size_t *puPlainLen)
{
  size_t uRead = 0u;
  size_t uWrite = 0u;

  while (uRead < uEncLen)
  {
    uint8 u8Code = pu8Buf[uRead++];
    size_t uRun = 0u;
    size_t uIdx = 0u;

    if (0u == u8Code)
    {
      return DM_FALSE;
    }
    uRun = (size_t)(u8Code - 1u);
    if (uRun > (uEncLen - uRead))
    {
      return DM_FALSE;
    }
    for (uIdx = 0u; uIdx < uRun; uIdx++)
    {
      uint8 u8Byte = pu8Buf[uRead++];
      if (0u == u8Byte)
      {
        return DM_FALSE;
      }
      pu8Buf[uWrite++] = u8Byte;
    }
    if ((u8Code < 0xFFu) && (uRead < uEncLen))
    {
      pu8Buf[uWrite++] = 0x00u;
    }
  }
  *puPlainLen = uWrite;
  return DM_TRUE;
}

static void dmc__sync_emit_err(dm_io dmc_sync_dec_st *pstDec, dm_in dmc_sync_err_cb pfOnErr,
                               dm_in void *pvUser, dm_io dmc_sync_stats_st *pstStats,
                               dm_in int iErr)
{
  pstDec->bHadErr = DM_TRUE;
  if (NULL != pstStats)
  {
    if (DMC_ERR_CRC == iErr)
    {
      pstStats->u64CrcErrors++;
    }
    else
    {
      pstStats->u64WireErrors++;
    }
  }
  if (NULL != pfOnErr)
  {
    pfOnErr(pvUser, iErr);
  }
  return;
}

static void dmc__sync_finish_pkt(dm_io dmc_sync_dec_st *pstDec, dm_in const dmc_sync_cbs_st *pstCbs,
                                 dm_io dmc_sync_stats_st *pstStats)
{
  size_t uPlain = 0u;
  dm_frame_head_st stHead;
  uint16 u16Calc = 0u;
  uint16 u16Got = 0u;
  dmc_sync_frame_cb pfOnFrame = (NULL != pstCbs) ? pstCbs->pfOnFrame : NULL;
  dmc_sync_err_cb pfOnErr = (NULL != pstCbs) ? pstCbs->pfOnErr : NULL;
  void *pvUser = (NULL != pstCbs) ? pstCbs->pvUser : NULL;

  if (pstDec->bOverflow)
  {
    pstDec->bOverflow = DM_FALSE;
    pstDec->uLen = 0u;
    dmc__sync_emit_err(pstDec, pfOnErr, pvUser, pstStats, DMC_ERR_WIRE);
    return;
  }
  if (0u == pstDec->uLen)
  {
    return; /* 连续 0x00 定界符：静默忽略 */
  }
  if (!dmc__cobs_decode_inplace(pstDec->pu8Buf, pstDec->uLen, &uPlain))
  {
    pstDec->uLen = 0u;
    dmc__sync_emit_err(pstDec, pfOnErr, pvUser, pstStats, DMC_ERR_WIRE);
    return;
  }
  pstDec->uLen = 0u;
  if ((uPlain < DMC_SYNC_OVERHEAD) || (DMC_SYNC_MAGIC_0 != pstDec->pu8Buf[0]) ||
      (DMC_SYNC_MAGIC_1 != pstDec->pu8Buf[1]))
  {
    dmc__sync_emit_err(pstDec, pfOnErr, pvUser, pstStats, DMC_ERR_WIRE);
    return;
  }
  u16Calc = dmc_crc16_ccitt(pstDec->pu8Buf, uPlain - 2u);
  u16Got = (uint16)dm_frame_rd16(pstDec->pu8Buf + uPlain - 2u);
  if (u16Calc != u16Got)
  {
    dmc__sync_emit_err(pstDec, pfOnErr, pvUser, pstStats, DMC_ERR_CRC);
    return;
  }
  dm_frame_get(pstDec->pu8Buf + 2u, &stHead);
  if ((stHead.u32Len > pstDec->u32FrameMax) ||
      ((uPlain - DMC_SYNC_OVERHEAD) != (size_t)stHead.u32Len))
  {
    dmc__sync_emit_err(pstDec, pfOnErr, pvUser, pstStats, DMC_ERR_WIRE);
    return;
  }
  if (NULL != pstStats)
  {
    pstStats->u64RxFrames++;
    if (pstDec->bHadErr)
    {
      pstStats->u64ResyncCount++;
    }
  }
  pstDec->bHadErr = DM_FALSE;
  if (NULL != pfOnFrame)
  {
    pfOnFrame(pvUser, &stHead, pstDec->pu8Buf + 2u + DM_FRAME_HEAD);
  }
  return;
}

void dmc_sync_dec_init(dm_out dmc_sync_dec_st *pstDec, dm_io uint8 *pu8WorkBuf,
                       dm_in size_t uWorkCap, dm_in uint32 u32FrameMax)
{
  if (NULL == pstDec)
  {
    return;
  }
  memset(pstDec, 0, sizeof(*pstDec));
  pstDec->pu8Buf = pu8WorkBuf;
  pstDec->uCap = uWorkCap;
  pstDec->u32FrameMax = (0u != u32FrameMax) ? u32FrameMax : DM_FRAME_MAX_DEFAULT;
  return;
}

void dmc_sync_dec_reset(dm_io dmc_sync_dec_st *pstDec)
{
  if (NULL == pstDec)
  {
    return;
  }
  pstDec->uLen = 0u;
  pstDec->bOverflow = DM_FALSE;
  pstDec->bHadErr = DM_FALSE;
  return;
}

size_t dmc_sync_dec_feed_ex(dm_io dmc_sync_dec_st *pstDec, dm_in const uint8 *pu8In,
                            dm_in size_t uLen, dm_in const dmc_sync_cbs_st *pstCbs,
                            dm_io dmc_sync_stats_st *pstStats)
{
  size_t uIdx = 0u;

  if ((NULL == pstDec) || (NULL == pstDec->pu8Buf) || (pstDec->uCap < DMC_SYNC_OVERHEAD) ||
      ((NULL == pu8In) && (uLen > 0u)))
  {
    return 0u;
  }
  for (uIdx = 0u; uIdx < uLen; uIdx++)
  {
    uint8 u8Byte = pu8In[uIdx];
    if (DMC_SYNC_DELIM == u8Byte)
    {
      dmc__sync_finish_pkt(pstDec, pstCbs, pstStats);
    }
    else if (!pstDec->bOverflow)
    {
      if (pstDec->uLen < pstDec->uCap)
      {
        pstDec->pu8Buf[pstDec->uLen++] = u8Byte;
      }
      else
      {
        pstDec->bOverflow = DM_TRUE;
      }
    }
  }
  return uLen;
}

size_t dmc_sync_dec_feed(dm_io dmc_sync_dec_st *pstDec, dm_in const uint8 *pu8In, dm_in size_t uLen,
                         dm_in const dmc_sync_cb_st *pstCb)
{
  return dmc_sync_dec_feed_ex(pstDec, pu8In, uLen, pstCb, NULL);
}

#ifdef __cplusplus
}
#endif
