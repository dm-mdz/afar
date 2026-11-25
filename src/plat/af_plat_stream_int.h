/**
 ******************************************************************************
 * @file    af_plat_stream_int.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   macOS 屏幕采集与 VideoToolbox 硬编内部共享契约（仅 src/plat/ 内使用）。
 ******************************************************************************
 */
#ifndef AF_PLAT_STREAM_INT_H
#define AF_PLAT_STREAM_INT_H

#include <dm.h>
#include "plat/af_plat.h"
#include "plat/af_objc_undef.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#define AF_PLAT_AVG_BITRATE 10000000u
#define AF_PLAT_MIN_BITRATE 5000000u
#define AF_PLAT_PEAK_BYTES_PER_S 1875000u

typedef struct af__plat_stream_ctx
{
  af_plat_stream_cfg_st stCfg;
  VTCompressionSessionRef pVt;
  CVPixelBufferRef pLastBuf;
  bool_t bActive;
  bool_t bStopping;
  bool_t bEff444;
  bool_t bForceNextIdr;
  uint64 u64PtsIdx;
} af__plat_stream_ctx_st;

extern af__plat_stream_ctx_st g_stAfPs;

void af__ps_vt_apply_bitrate(dm_in VTCompressionSessionRef pVt, dm_in uint32 u32AvgBps,
                             dm_in uint32 u32PeakBytes);
int af__ps_vt_create(dm_in const af_plat_stream_cfg_st *pstCfg, dm_out bool_t *pbEff444);
void af__ps_vt_encode_buf(dm_in CVPixelBufferRef pBuf, dm_in bool_t bForceKey,
                          dm_in uint32 u32ExtraFlags);
void af__ps_vt_teardown_locked(void);

#endif /* AF_PLAT_STREAM_INT_H */
