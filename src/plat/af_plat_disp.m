/**
 ******************************************************************************
 * @file    af_plat_disp.m
 * @author  Afar_C
 * @version 1.0.0
 * @brief   macOS 公开 CoreGraphics 显示器枚举、模式切换、镜像与变更监听实现。
 ******************************************************************************
 */

#include <dm.h>
#include <dm/link.h>
#include "plat/af_plat.h"
#include "plat/private/af_priv_internal.h"
#include "plat/af_objc_undef.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include <string.h>

static uint g_uiDispWatchLink = 0u;
static bool_t g_bDispWatching = DM_FALSE;

static void af__fill_mode(CGDisplayModeRef pMode, af_disp_mode_st *pstOut)
{
  memset(pstOut, 0, sizeof(*pstOut));
  if (NULL == pMode)
  {
    return;
  }
  pstOut->u32W = (uint32)CGDisplayModeGetWidth(pMode);
  pstOut->u32H = (uint32)CGDisplayModeGetHeight(pMode);
  pstOut->u32Pw = (uint32)CGDisplayModeGetPixelWidth(pMode);
  pstOut->u32Ph = (uint32)CGDisplayModeGetPixelHeight(pMode);
  pstOut->u32Hz100 = (uint32)(CGDisplayModeGetRefreshRate(pMode) * 100.0 + 0.5);
  pstOut->bUsable = CGDisplayModeIsUsableForDesktopGUI(pMode) ? DM_TRUE : DM_FALSE;
}

uint32 af_plat_disp_online(af_disp_info_st *pastOut, uint32 u32Cap)
{
  CGDirectDisplayID aIds[AF_DISP_MAX];
  uint32_t u32Cnt = 0u;
  uint32 i = 0u;

  if ((NULL == pastOut) || (0u == u32Cap))
  {
    return 0u;
  }
  if (kCGErrorSuccess != CGGetOnlineDisplayList(AF_DISP_MAX, aIds, &u32Cnt))
  {
    return 0u;
  }
  if (u32Cnt > u32Cap)
  {
    u32Cnt = u32Cap;
  }
  for (i = 0u; i < u32Cnt; i++)
  {
    CGDirectDisplayID uId = aIds[i];
    CGDirectDisplayID uMirror = CGDisplayMirrorsDisplay(uId);
    CGDisplayModeRef pCur = CGDisplayCopyDisplayMode(uId);
    memset(&pastOut[i], 0, sizeof(pastOut[i]));
    pastOut[i].u32Id = uId;
    pastOut[i].u32Vendor = CGDisplayVendorNumber(uId);
    pastOut[i].u32Model = CGDisplayModelNumber(uId);
    pastOut[i].u32Serial = CGDisplaySerialNumber(uId);
    pastOut[i].bBuiltin = CGDisplayIsBuiltin(uId) ? DM_TRUE : DM_FALSE;
    pastOut[i].u32MirrorOf = (kCGNullDirectDisplay == uMirror) ? 0u : uMirror;
    if (NULL != pCur)
    {
      af__fill_mode(pCur, &pastOut[i].stCur);
      pastOut[i].bHasCur = DM_TRUE;
      CGDisplayModeRelease(pCur);
    }
  }
  return u32Cnt;
}

uint32 af_plat_disp_modes(uint32 u32DispId, af_disp_mode_st *pastOut, uint32 u32Cap)
{
  CFDictionaryRef pOpts = NULL;
  CFArrayRef pArr = NULL;
  CFIndex iCnt = 0;
  CFIndex i = 0;
  uint32 u32Out = 0u;

  if ((NULL == pastOut) || (0u == u32Cap))
  {
    return 0u;
  }
  const void *apK[1] = {kCGDisplayShowDuplicateLowResolutionModes};
  const void *apV[1] = {kCFBooleanTrue};
  pOpts = CFDictionaryCreate(kCFAllocatorDefault, apK, apV, 1, &kCFTypeDictionaryKeyCallBacks,
                             &kCFTypeDictionaryValueCallBacks);
  pArr = CGDisplayCopyAllDisplayModes(u32DispId, pOpts);
  if (NULL != pOpts) { CFRelease(pOpts); }
  if (NULL == pArr)
  {
    return 0u;
  }
  iCnt = CFArrayGetCount(pArr);
  for (i = 0; (i < iCnt) && (u32Out < u32Cap); i++)
  {
    CGDisplayModeRef pM = (CGDisplayModeRef)CFArrayGetValueAtIndex(pArr, i);
    af__fill_mode(pM, &pastOut[u32Out++]);
  }
  CFRelease(pArr);
  return u32Out;
}

static CGDisplayModeRef af__find_cg_mode(uint32 u32DispId, const af_disp_mode_st *pstWant)
{
  const void *apK[1] = {kCGDisplayShowDuplicateLowResolutionModes};
  const void *apV[1] = {kCFBooleanTrue};
  CFDictionaryRef pOpts = CFDictionaryCreate(kCFAllocatorDefault, apK, apV, 1,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
  CFArrayRef pArr = CGDisplayCopyAllDisplayModes(u32DispId, pOpts);
  CGDisplayModeRef pFound = NULL;

  if (NULL != pOpts) { CFRelease(pOpts); }
  if (NULL == pArr) { return NULL; }
  for (CFIndex i = 0; i < CFArrayGetCount(pArr); i++)
  {
    CGDisplayModeRef pM = (CGDisplayModeRef)CFArrayGetValueAtIndex(pArr, i);
    if ((CGDisplayModeGetWidth(pM) == pstWant->u32W)
        && (CGDisplayModeGetHeight(pM) == pstWant->u32H)
        && ((0u == pstWant->u32Pw) || (CGDisplayModeGetPixelWidth(pM) == pstWant->u32Pw)))
    {
      pFound = CGDisplayModeRetain(pM);
      break;
    }
  }
  CFRelease(pArr);
  return pFound;
}

int af_plat_disp_set_mode(uint32 u32DispId, const af_disp_mode_st *pstWant, bool_t bAllowCgsFallback)
{
  CGDisplayModeRef pMode = NULL;
  CGError eErr = kCGErrorFailure;

  if (NULL == pstWant)
  {
    return DM_INVALID;
  }
  pMode = af__find_cg_mode(u32DispId, pstWant);
  if (NULL != pMode)
  {
    eErr = CGDisplaySetDisplayMode(u32DispId, pMode, NULL);
    CGDisplayModeRelease(pMode);
    if (kCGErrorSuccess == eErr)
    {
      return DM_SUCCESS;
    }
  }
  if (bAllowCgsFallback)
  {
    return af_priv_cgs_set_mode(u32DispId, pstWant);
  }
  return DM_FAILD;
}

int af_plat_disp_set_hidpi_unmirrored(uint32 u32DispId, uint32 u32W, uint32 u32H)
{
  af_disp_mode_st stWant;
  CGDisplayModeRef pMode = NULL;
  CGDisplayConfigRef pCfg = NULL;

  memset(&stWant, 0, sizeof(stWant));
  stWant.u32W = u32W;
  stWant.u32H = u32H;
  stWant.u32Pw = u32W * 2u;
  stWant.u32Ph = u32H * 2u;
  pMode = af__find_cg_mode(u32DispId, &stWant);
  if (NULL == pMode)
  {
    return DM_NOT_FOUND;
  }
  if (kCGErrorSuccess != CGBeginDisplayConfiguration(&pCfg))
  {
    CGDisplayModeRelease(pMode);
    return DM_FAILD;
  }
  if ((kCGErrorSuccess != CGConfigureDisplayMirrorOfDisplay(pCfg, u32DispId, kCGNullDirectDisplay))
      || (kCGErrorSuccess != CGConfigureDisplayWithDisplayMode(pCfg, u32DispId, pMode, NULL)))
  {
    CGDisplayModeRelease(pMode);
    (void)CGCancelDisplayConfiguration(pCfg);
    return DM_FAILD;
  }
  CGDisplayModeRelease(pMode);
  if (kCGErrorSuccess != CGCompleteDisplayConfiguration(pCfg, kCGConfigurePermanently))
  {
    return DM_FAILD;
  }
  return DM_SUCCESS;
}

int af_plat_disp_mirror(uint32 u32DispId, uint32 u32MasterId)
{
  CGDisplayConfigRef pCfg = NULL;
  CGDirectDisplayID uMaster = (0u == u32MasterId) ? kCGNullDirectDisplay : u32MasterId;

  if (kCGErrorSuccess != CGBeginDisplayConfiguration(&pCfg))
  {
    return DM_FAILD;
  }
  if (kCGErrorSuccess != CGConfigureDisplayMirrorOfDisplay(pCfg, u32DispId, uMaster))
  {
    (void)CGCancelDisplayConfiguration(pCfg);
    return DM_FAILD;
  }
  return (kCGErrorSuccess == CGCompleteDisplayConfiguration(pCfg, kCGConfigurePermanently))
             ? DM_SUCCESS
             : DM_FAILD;
}

static void af__disp_reconf_cb(CGDirectDisplayID uDisp, CGDisplayChangeSummaryFlags uFlags,
                               void *pvUser)
{
  (void)uDisp;
  (void)uFlags;
  (void)pvUser;
  if (0u != g_uiDispWatchLink)
  {
    (void)dm_queue_post_async(g_uiDispWatchLink, NULL, 0u);
  }
}

void af_plat_disp_watch_start(uint uiLinkID)
{
  g_uiDispWatchLink = uiLinkID;
  if (!g_bDispWatching)
  {
    (void)CGDisplayRegisterReconfigurationCallback(af__disp_reconf_cb, NULL);
    g_bDispWatching = DM_TRUE;
  }
}

void af_plat_disp_watch_stop(void)
{
  if (g_bDispWatching)
  {
    (void)CGDisplayRemoveReconfigurationCallback(af__disp_reconf_cb, NULL);
    g_bDispWatching = DM_FALSE;
  }
  g_uiDispWatchLink = 0u;
}
