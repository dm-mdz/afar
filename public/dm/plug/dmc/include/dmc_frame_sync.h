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
#define DMC_SYNC_MCU_FRAME_MAX 4096u
#define DMC_SYNC_HOST_SERIAL_FRAME_MAX 65536u

typedef void (*dmc_sync_frame_cb)(dm_in void *pvUser, dm_in const dm_frame_head_st *pstHead,
                                  dm_in const uint8 *pu8Body);
typedef void (*dmc_sync_err_cb)(dm_in void *pvUser, dm_in int iErr);

/* 零堆分配流式解码器（工作缓冲由调用方提供，长度建议 >= dmc_sync_max_encoded_len(u32FrameMax)） */
typedef struct dmc_sync_dec
{
  uint8 *pu8Buf;
  size_t uCap;
  size_t uLen;
  uint32 u32FrameMax;
  bool_t bOverflow;
  bool_t bHadErr;
} dmc_sync_dec_st;

/* 独立纯 C11 COBS 编解码原语（热路径零堆分配） */
DMC_API int dmc_cobs_encode(dm_in const uint8 *pu8In, dm_in size_t uInLen, dm_out uint8 *pu8Out,
                            dm_in size_t uOutCap, dm_out size_t *puOutLen);
DMC_API int dmc_cobs_decode(dm_in const uint8 *pu8In, dm_in size_t uInLen, dm_out uint8 *pu8Out,
                            dm_in size_t uOutCap, dm_out size_t *puOutLen);

/* 计算 CRC16-CCITT (poly=0x1021, init=0xFFFF) */
DMC_API uint16 dmc_crc16_ccitt(dm_in const uint8 *pu8Data, dm_in size_t uLen);

/* 计算 uBodyLen 字节载荷经 Magic+Head+CRC16+COBS+双定界符后的最大编码字节数（溢出返 0） */
DMC_API size_t dmc_sync_max_encoded_len(dm_in size_t uBodyLen);

/* 将一帧编码为 [0x00][COBS...][0x00] 写入 pu8Out；成功返 DMC_OK，缓冲不足返 DMC_ERR_QUEUE */
DMC_API int dmc_sync_encode(dm_in const dm_frame_head_st *pstHead, dm_in const void *pvBody,
                            dm_out uint8 *pu8Out, dm_in size_t uOutCap, dm_out size_t *puOutLen);

/* 初始化/复位流式解码器 */
DMC_API void dmc_sync_dec_init(dm_out dmc_sync_dec_st *pstDec, dm_io uint8 *pu8WorkBuf,
                               dm_in size_t uWorkCap, dm_in uint32 u32FrameMax);
DMC_API void dmc_sync_dec_reset(dm_io dmc_sync_dec_st *pstDec);

/* 串口同步解码回调打包结构 */
typedef struct dmc_sync_cb
{
  dmc_sync_frame_cb pfOnFrame; /* 合法帧回调（可 NULL） */
  dmc_sync_err_cb pfOnErr;     /* 坏帧/校验错回调（可 NULL） */
  void *pvUser;                /* 回调透传上下文 */
} dmc_sync_cb_st;

typedef dmc_sync_cb_st dmc_sync_cbs_st;

/* 流式解码诊断统计 */
typedef struct dmc_sync_stats
{
  uint64 u64RxFrames;    /* 成功解出合法帧数 */
  uint64 u64CrcErrors;   /* CRC16 校验失败帧数 */
  uint64 u64WireErrors;  /* COBS 畸形 / Magic 错 / 超长溢出帧数 */
  uint64 u64ResyncCount; /* 错误后首次重新锁定合法帧次数 */
} dmc_sync_stats_st;

/* 喂入串口原始字节流：合法帧回调 pfOnFrame，损坏/校验错回调 pfOnErr(DMC_ERR_CRC / DMC_ERR_WIRE) */
DMC_API size_t dmc_sync_dec_feed(dm_io dmc_sync_dec_st *pstDec, dm_in const uint8 *pu8In,
                                 dm_in size_t uLen, dm_in const dmc_sync_cb_st *pstCb);

/* 扩展版流式喂入：严格 5 参数，累加诊断统计（pstStats 可 NULL） */
DMC_API size_t dmc_sync_dec_feed_ex(dm_io dmc_sync_dec_st *pstDec, dm_in const uint8 *pu8In,
                                    dm_in size_t uLen, dm_in const dmc_sync_cbs_st *pstCbs,
                                    dm_io dmc_sync_stats_st *pstStats);

#ifdef __cplusplus
}
#endif

#endif /* DMC_FRAME_SYNC_H */
