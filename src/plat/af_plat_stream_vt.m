/**
 ******************************************************************************
 * @file    af_plat_stream_vt.m
 * @author  Afar_C
 * @version 1.0.0
 * @brief   macOS VideoToolbox HEVC 硬编会话管理、码率调控与 CONFIG/帧打包。
 ******************************************************************************
 */

#define DM_LOG_MOD "plat_stream"

#include <dm.h>
#include <dm/log.h>
#include <dm/module.h>
#include "af_direct_proto.h"
#include "af_modules.h"
#include "plat/af_plat_stream_int.h"
#include "plat/af_objc_undef.h"

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

#include <string.h>

bool_t af_plat_perm_screen_capture(void)
{
  return CGPreflightScreenCaptureAccess() ? DM_TRUE : DM_FALSE;
}

bool_t af_plat_hevc444_supported(void)
{
  static dispatch_once_t tOnce;
  static bool_t bSup = DM_FALSE;

  dispatch_once(&tOnce, ^{
    VTCompressionSessionRef pVt = NULL;
    OSStatus iSt = VTCompressionSessionCreate(kCFAllocatorDefault, 256, 256, kCMVideoCodecType_HEVC,
                                              NULL, NULL, NULL, NULL, NULL, &pVt);
    if ((noErr == iSt) && (NULL != pVt))
    {
      if ((noErr == VTSessionSetProperty(pVt, kVTCompressionPropertyKey_ProfileLevel,
                                         CFSTR("HEVC_Main444_AutoLevel"))) &&
          (noErr == VTCompressionSessionPrepareToEncodeFrames(pVt)))
      {
        bSup = DM_TRUE;
      }
      VTCompressionSessionInvalidate(pVt);
      CFRelease(pVt);
    }
  });
  return bSup;
}

static bool_t af__ps_is_keyframe(dm_in CMSampleBufferRef pSample)
{
  CFArrayRef pArr = CMSampleBufferGetSampleAttachmentsArray(pSample, false);
  CFDictionaryRef pDict = NULL;
  CFBooleanRef pNotSync = NULL;

  if ((NULL == pArr) || (0 == CFArrayGetCount(pArr)))
  {
    return DM_TRUE;
  }
  pDict = (CFDictionaryRef)CFArrayGetValueAtIndex(pArr, 0);
  if (NULL == pDict)
  {
    return DM_TRUE;
  }
  pNotSync = (CFBooleanRef)CFDictionaryGetValue(pDict, kCMSampleAttachmentKey_NotSync);
  return ((NULL == pNotSync) || (!CFBooleanGetValue(pNotSync))) ? DM_TRUE : DM_FALSE;
}

static void af__ps_emit_config(dm_in CMSampleBufferRef pSample, dm_in uint64 u64CapUs)
{
  CMVideoFormatDescriptionRef pFmt = CMSampleBufferGetFormatDescription(pSample);
  const uint8 *apu8Ps[3] = {NULL, NULL, NULL};
  size_t auLen[3] = {0u, 0u, 0u};
  size_t uNeed = 0u;
  size_t uOut = 0u;
  uint8 *pu8Buf = NULL;

  if (NULL == pFmt)
  {
    return;
  }
  for (size_t i = 0u; i < 3u; i++)
  {
    if ((noErr != CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(pFmt, i, &apu8Ps[i], &auLen[i],
                                                                     NULL, NULL)) ||
        (NULL == apu8Ps[i]) || (0u == auLen[i]) || (auLen[i] > 65535u))
    {
      return;
    }
  }
  uNeed = 10u + auLen[0] + auLen[1] + auLen[2];
  pu8Buf = (uint8 *)dm_malloc_tag(uNeed, AF_TAG_STREAM_FRAME);
  if (NULL == pu8Buf)
  {
    return;
  }
  af_dp_vid_config_t stCfg = {g_stAfPs.stCfg.u16EncW,
                              g_stAfPs.stCfg.u16EncH,
                              apu8Ps[0],
                              (uint16)auLen[0],
                              apu8Ps[1],
                              (uint16)auLen[1],
                              apu8Ps[2],
                              (uint16)auLen[2]};
  if (AF_DP_OK != af_dp_vid_config_encode(&stCfg, pu8Buf, uNeed, &uOut))
  {
    dm_free_tag(pu8Buf);
    return;
  }
  af_plat_vid_frame_st stF = {g_stAfPs.stCfg.u64Gen,
                              AF_VFLAG_CONFIG | (g_stAfPs.bEff444 ? AF_VFLAG_CHROMA_444 : 0u),
                              u64CapUs, pu8Buf, (uint32)uOut};
  g_stAfPs.stCfg.pfOnFrame(&stF, g_stAfPs.stCfg.pvUser);
}

static void af__ps_vt_cb(dm_io void *pvOutputRef, dm_io void *pvSrcRef, dm_in OSStatus iStatus,
                         dm_in VTEncodeInfoFlags u32InfoFlags, dm_in CMSampleBufferRef pSample)
{
  CMBlockBufferRef pBlock = NULL;
  size_t uLen = 0u;
  uint8 *pu8Data = NULL;
  bool_t bKey = DM_FALSE;
  uint64 u64CapUs = 0u;

  (void)pvOutputRef;
  (void)u32InfoFlags;
  if ((noErr != iStatus) || (NULL == pSample) || (!CMSampleBufferDataIsReady(pSample)) ||
      (NULL == g_stAfPs.stCfg.pfOnFrame) ||
      (NULL == (pBlock = CMSampleBufferGetDataBuffer(pSample))))
  {
    return;
  }
  uLen = CMBlockBufferGetDataLength(pBlock);
  if ((0u == uLen) || (uLen > AF_DP_VID_PAYLOAD_MAX))
  {
    return;
  }
  u64CapUs = dm_hrtime() / 1000ULL;
  bKey = af__ps_is_keyframe(pSample);
  if (bKey)
  {
    af__ps_emit_config(pSample, u64CapUs);
  }
  pu8Data = (uint8 *)dm_malloc_tag(uLen, AF_TAG_STREAM_FRAME);
  if (NULL == pu8Data)
  {
    return;
  }
  if (noErr != CMBlockBufferCopyDataBytes(pBlock, 0u, uLen, pu8Data))
  {
    dm_free_tag(pu8Data);
    return;
  }
  af_plat_vid_frame_st stF = {g_stAfPs.stCfg.u64Gen,
                              (bKey ? AF_VFLAG_KEYFRAME : 0u) | (uint32)(uintptr_t)pvSrcRef |
                                  (g_stAfPs.bEff444 ? AF_VFLAG_CHROMA_444 : 0u),
                              u64CapUs, pu8Data, (uint32)uLen};
  g_stAfPs.stCfg.pfOnFrame(&stF, g_stAfPs.stCfg.pvUser);
}

void af__ps_vt_apply_bitrate(dm_in VTCompressionSessionRef pVt, dm_in uint32 u32AvgBps,
                             dm_in uint32 u32PeakBytes)
{
  if (NULL == pVt)
  {
    return;
  }
  @autoreleasepool
  {
    NSArray *oLimits = @[ @(u32PeakBytes), @(1) ];
    (void)VTSessionSetProperty(pVt, kVTCompressionPropertyKey_AverageBitRate,
                               (__bridge CFNumberRef) @(u32AvgBps));
    (void)VTSessionSetProperty(pVt, kVTCompressionPropertyKey_DataRateLimits,
                               (__bridge CFArrayRef)oLimits);
  }
}

int af__ps_vt_create(dm_in const af_plat_stream_cfg_st *pstCfg, dm_out bool_t *pbEff444)
{
  VTCompressionSessionRef pVt = NULL;
  int32 i32Fps = (0u != pstCfg->u16Fps) ? (int32)pstCfg->u16Fps : 60;
  bool_t bEff444 = DM_FALSE;
  /* Low-latency rate control bounds HW encode delay (measured 8-14ms vs random ~50ms without). */
  NSDictionary *oSpec =
      @{(__bridge NSString *)kVTVideoEncoderSpecification_EnableLowLatencyRateControl : @YES};

  if (((noErr != VTCompressionSessionCreate(kCFAllocatorDefault, (int32)pstCfg->u16EncW,
                                            (int32)pstCfg->u16EncH, kCMVideoCodecType_HEVC,
                                            (__bridge CFDictionaryRef)oSpec, NULL, NULL,
                                            af__ps_vt_cb, NULL, &pVt)) ||
       (NULL == pVt)) &&
      ((noErr != VTCompressionSessionCreate(kCFAllocatorDefault, (int32)pstCfg->u16EncW,
                                            (int32)pstCfg->u16EncH, kCMVideoCodecType_HEVC, NULL,
                                            NULL, NULL, af__ps_vt_cb, NULL, &pVt)) ||
       (NULL == pVt)))
  {
    return DM_FAILD;
  }
  (void)VTSessionSetProperty(pVt, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
  (void)VTSessionSetProperty(pVt, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
  (void)VTSessionSetProperty(pVt, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality,
                             kCFBooleanTrue);
  @autoreleasepool
  {
    (void)VTSessionSetProperty(pVt, kVTCompressionPropertyKey_MaxFrameDelayCount,
                               (__bridge CFNumberRef) @(0));
    (void)VTSessionSetProperty(pVt, kVTCompressionPropertyKey_ExpectedFrameRate,
                               (__bridge CFNumberRef) @(i32Fps));
    (void)VTSessionSetProperty(pVt, kVTCompressionPropertyKey_MaxKeyFrameInterval,
                               (__bridge CFNumberRef) @(i32Fps * 30));
  }
  af__ps_vt_apply_bitrate(pVt, AF_PLAT_AVG_BITRATE, AF_PLAT_PEAK_BYTES_PER_S);
  if (pstCfg->bChroma444 &&
      (noErr == VTSessionSetProperty(pVt, kVTCompressionPropertyKey_ProfileLevel,
                                     CFSTR("HEVC_Main444_AutoLevel"))))
  {
    bEff444 = DM_TRUE;
  }
  else
  {
    (void)VTSessionSetProperty(pVt, kVTCompressionPropertyKey_ProfileLevel,
                               kVTProfileLevel_HEVC_Main_AutoLevel);
  }
  if (noErr != VTCompressionSessionPrepareToEncodeFrames(pVt))
  {
    VTCompressionSessionInvalidate(pVt);
    CFRelease(pVt);
    return DM_FAILD;
  }
  g_stAfPs.pVt = pVt;
  g_stAfPs.bEff444 = bEff444;
  *pbEff444 = bEff444;
  return DM_SUCCESS;
}

void af__ps_vt_encode_buf(dm_in CVPixelBufferRef pBuf, dm_in bool_t bForceKey,
                          dm_in uint32 u32ExtraFlags)
{
  int32 i32Fps = (0u != g_stAfPs.stCfg.u16Fps) ? (int32)g_stAfPs.stCfg.u16Fps : 60;
  CMTime stPts = CMTimeMake((int64_t)(++g_stAfPs.u64PtsIdx), i32Fps);
  CMTime stDur = CMTimeMake(1, i32Fps);
  CFDictionaryRef pProps = NULL;

  if ((NULL == pBuf) || (NULL == g_stAfPs.pVt))
  {
    return;
  }
  @autoreleasepool
  {
    NSDictionary *oProps =
        bForceKey
            ? @{(__bridge NSString *)kVTEncodeFrameOptionKey_ForceKeyFrame : @YES}
            : nil;
    pProps = (__bridge CFDictionaryRef)oProps;
    (void)VTCompressionSessionEncodeFrame(g_stAfPs.pVt, pBuf, stPts, stDur, pProps,
                                          (void *)(uintptr_t)u32ExtraFlags, NULL);
  }
}

void af__ps_vt_teardown_locked(void)
{
  if (NULL != g_stAfPs.pVt)
  {
    (void)VTCompressionSessionCompleteFrames(g_stAfPs.pVt, kCMTimeInvalid);
    VTCompressionSessionInvalidate(g_stAfPs.pVt);
    CFRelease(g_stAfPs.pVt);
    g_stAfPs.pVt = NULL;
  }
  if (NULL != g_stAfPs.pLastBuf)
  {
    CVPixelBufferRelease(g_stAfPs.pLastBuf);
    g_stAfPs.pLastBuf = NULL;
  }
  g_stAfPs.bActive = DM_FALSE;
  g_stAfPs.bStopping = DM_FALSE;
}
