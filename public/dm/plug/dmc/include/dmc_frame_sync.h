/**
 ******************************************************************************
 * @file    dmc_frame_sync.h
 * @brief   串口 / 不可靠字节流帧同步契约（DD-02 §2.3，PLUG-04）：
 *          [0x00 定界符] [COBS( 0xD4 0x4D | 16B dm_frame_head_st | Body | 2B CRC16-CCITT BE )]
 * [0x00] 零堆分配、零 OS 依赖；半途插线或单比特损坏后，扫到下一个 0x00 即自动重同步。
 ******************************************************************************
 */
#ifndef DMC_FRAME_SYNC_H
#define DMC_FRAME_SYNC_H

#include <dmc_proto.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DMC_SYNC_DELIM 0x00u
#define DMC_SYNC_MAGIC_0 0xD4u
#define DMC_SYNC_MAGIC_1 0x4Du
#define DMC_SYNC_OVERHEAD (2u + DM_FRAME_HEAD + 2u) /* 2B Magic + 16B Head + 2B CRC16 */

typedef void (*dmc_sync_frame_cb)(in void *pvUser, in const dm_frame_head_st *pstHead,
                                  in const uint8 *pu8Body);
typedef void (*dmc_sync_err_cb)(in void *pvUser, in int iErr);

/* 零堆分配流式解码器（工作缓冲由调用方提供，长度建议 >= dmc_sync_max_encoded_len(u32FrameMax)） */
typedef struct dmc_sync_dec
{
  uint8 *pu8Buf;
  size_t uCap;
  size_t uLen;
  uint32 u32FrameMax;
  bool_t bOverflow;
} dmc_sync_dec_st;

/* 计算 CRC16-CCITT (poly=0x1021, init=0xFFFF) */
DMC_API uint16 dmc_crc16_ccitt(in const uint8 *pu8Data, in size_t uLen);

/* 计算 uBodyLen 字节载荷经 Magic+Head+CRC16+COBS+双定界符后的最大编码字节数（溢出返 0） */
DMC_API size_t dmc_sync_max_encoded_len(in size_t uBodyLen);

/* 将一帧编码为 [0x00][COBS...][0x00] 写入 pu8Out；成功返 DMC_OK，缓冲不足返 DMC_ERR_QUEUE */
DMC_API int dmc_sync_encode(in const dm_frame_head_st *pstHead, in const void *pvBody,
                            out uint8 *pu8Out, in size_t uOutCap, out size_t *puOutLen);

/* 初始化/复位流式解码器 */
DMC_API void dmc_sync_dec_init(out dmc_sync_dec_st *pstDec, io uint8 *pu8WorkBuf,
                               in size_t uWorkCap, in uint32 u32FrameMax);
DMC_API void dmc_sync_dec_reset(io dmc_sync_dec_st *pstDec);

/* 喂入串口原始字节流：合法帧回调 pfOnFrame，损坏/校验错回调 pfOnErr(DMC_ERR_CRC / DMC_ERR_WIRE) */
DMC_API size_t dmc_sync_dec_feed(io dmc_sync_dec_st *pstDec, in const uint8 *pu8In, in size_t uLen,
                                 in dmc_sync_frame_cb pfOnFrame, in dmc_sync_err_cb pfOnErr,
                                 in void *pvUser);

#ifdef __cplusplus
}
#endif

#endif /* DMC_FRAME_SYNC_H */
