/**
 ******************************************************************************
 * @file    af_priv_vd.m
 * @author  Afar_C
 * @version 1.0.0
 * @brief   macOS 私有 CGVirtualDisplay 与 CGS 显示模式回退隔离实现（DD-01/03）。
 ******************************************************************************
 */

#include <dm.h>
#include "plat/af_plat.h"
#include "plat/private/af_priv_internal.h"
#include "plat/af_objc_undef.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <dlfcn.h>

#define AF_VD_SLOTS_MAX 8u

@protocol AFPrivVDProto <NSObject>
@optional
- (instancetype)initWithWidth:(NSUInteger)uW height:(NSUInteger)uH refreshRate:(double)dRate;
- (instancetype)initWithDescriptor:(id)oDesc;
- (BOOL)applySettings:(id)oSet;
@end

typedef struct af__vd_slot
{
  uint64 u64Handle;
  CFTypeRef pObj; /* __bridge_retained CGVirtualDisplay */
} af__vd_slot_st;

static af__vd_slot_st g_astVdSlots[AF_VD_SLOTS_MAX];
static uint64 g_u64NextVdHandle = 1u;
static void *g_pvSkyLight = NULL;
static bool_t g_bSkyLightTried = DM_FALSE;

static NSArray *af__build_vd_modes(const af_vd_mode_req_st *pastModes, uint32 u32Count)
{
  Class oModeCls = NSClassFromString(@"CGVirtualDisplayMode");
  NSMutableArray *oArr = [NSMutableArray arrayWithCapacity:u32Count];
  uint32 i = 0u;

  if ((Nil == oModeCls)
      || ![oModeCls instancesRespondToSelector:@selector(initWithWidth:height:refreshRate:)])
  {
    return nil;
  }
  for (i = 0u; i < u32Count; i++)
  {
    id<AFPrivVDProto> oAlloc = (id<AFPrivVDProto>)[oModeCls alloc];
    id oMode = [oAlloc initWithWidth:(NSUInteger)pastModes[i].u32W
                              height:(NSUInteger)pastModes[i].u32H
                         refreshRate:60.0];
    if (nil != oMode)
    {
      [oArr addObject:oMode];
    }
  }
  return oArr;
}

static id af__build_vd_settings(const af_vd_mode_req_st *pastModes, uint32 u32Count)
{
  Class oSetCls = NSClassFromString(@"CGVirtualDisplaySettings");
  NSArray *oModes = af__build_vd_modes(pastModes, u32Count);

  if ((Nil == oSetCls) || (nil == oModes) || (0u == oModes.count))
  {
    return nil;
  }
  id oSet = [[oSetCls alloc] init];
  [oSet setValue:@(1) forKey:@"hiDPI"];
  [oSet setValue:oModes forKey:@"modes"];
  return oSet;
}

int af_plat_vd_create(uint32 u32Serial, uint32 u32MaxPw, uint32 u32MaxPh,
                      const af_vd_mode_req_st *pastModes, uint32 u32Count, uint32 *pu32DispId,
                      uint64 *pu64Handle)
{
  uint32 u32Idx = 0u;

  if ((NULL == pastModes) || (0u == u32Count) || (NULL == pu32DispId) || (NULL == pu64Handle))
  {
    return DM_INVALID;
  }
  for (u32Idx = 0u; u32Idx < AF_VD_SLOTS_MAX; u32Idx++)
  {
    if (0u == g_astVdSlots[u32Idx].u64Handle)
    {
      break;
    }
  }
  if (u32Idx >= AF_VD_SLOTS_MAX)
  {
    return DM_BUSY;
  }
  @autoreleasepool
  {
    Class oDescCls = NSClassFromString(@"CGVirtualDisplayDescriptor");
    Class oVdCls = NSClassFromString(@"CGVirtualDisplay");
    if ((Nil == oDescCls) || (Nil == oVdCls)
        || ![oVdCls instancesRespondToSelector:@selector(initWithDescriptor:)]
        || ![oVdCls instancesRespondToSelector:@selector(applySettings:)])
    {
      return DM_NOT_FOUND;
    }
    id oDesc = [[oDescCls alloc] init];
    [oDesc setValue:@"Afar Virtual Display" forKey:@"name"];
    [oDesc setValue:@(0x4d50u) forKey:@"vendorID"];
    [oDesc setValue:@(0x0001u) forKey:@"productID"];
    [oDesc setValue:@(u32Serial) forKey:@"serialNum"];
    [oDesc setValue:@(u32MaxPw) forKey:@"maxPixelsWide"];
    [oDesc setValue:@(u32MaxPh) forKey:@"maxPixelsHigh"];
    [oDesc setValue:[NSValue valueWithSize:NSMakeSize(240.0, 170.0)] forKey:@"sizeInMillimeters"];
    [oDesc setValue:dispatch_get_main_queue() forKey:@"queue"];

    id<AFPrivVDProto> oVd = [(id<AFPrivVDProto>)[oVdCls alloc] initWithDescriptor:oDesc];
    id oSet = af__build_vd_settings(pastModes, u32Count);
    if ((nil == oVd) || (nil == oSet) || ![oVd applySettings:oSet])
    {
      return DM_FAILD;
    }
    NSNumber *oId = [(id)oVd valueForKey:@"displayID"];
    uint64 u64H = g_u64NextVdHandle++;
    g_astVdSlots[u32Idx].u64Handle = u64H;
    g_astVdSlots[u32Idx].pObj = (__bridge_retained CFTypeRef)oVd;
    *pu32DispId = (nil != oId) ? [oId unsignedIntValue] : 0u;
    *pu64Handle = u64H;
    return DM_SUCCESS;
  }
}

int af_plat_vd_apply(uint64 u64Handle, const af_vd_mode_req_st *pastModes, uint32 u32Count)
{
  uint32 i = 0u;
  if ((0u == u64Handle) || (NULL == pastModes) || (0u == u32Count))
  {
    return DM_INVALID;
  }
  for (i = 0u; i < AF_VD_SLOTS_MAX; i++)
  {
    if (g_astVdSlots[i].u64Handle == u64Handle)
    {
      @autoreleasepool
      {
        id<AFPrivVDProto> oVd = (__bridge id<AFPrivVDProto>)g_astVdSlots[i].pObj;
        id oSet = af__build_vd_settings(pastModes, u32Count);
        if ((nil == oSet) || ![oVd respondsToSelector:@selector(applySettings:)])
        {
          return DM_FAILD;
        }
        return [oVd applySettings:oSet] ? DM_SUCCESS : DM_FAILD;
      }
    }
  }
  return DM_NOT_FOUND;
}

void af_plat_vd_destroy(uint64 u64Handle)
{
  uint32 i = 0u;
  for (i = 0u; i < AF_VD_SLOTS_MAX; i++)
  {
    if ((0u != u64Handle) && (g_astVdSlots[i].u64Handle == u64Handle))
    {
      if (NULL != g_astVdSlots[i].pObj)
      {
        CFRelease(g_astVdSlots[i].pObj);
        g_astVdSlots[i].pObj = NULL;
      }
      g_astVdSlots[i].u64Handle = 0u;
    }
  }
}

void af_plat_vd_destroy_all(void)
{
  uint32 i = 0u;
  for (i = 0u; i < AF_VD_SLOTS_MAX; i++)
  {
    if (0u != g_astVdSlots[i].u64Handle)
    {
      af_plat_vd_destroy(g_astVdSlots[i].u64Handle);
    }
  }
}

int af_priv_cgs_set_mode(uint32 u32DispId, const af_disp_mode_st *pstWant)
{
  typedef int (*cgs_cfg_fn)(CGDisplayConfigRef, CGDirectDisplayID, int);
  typedef void (*cgs_num_fn)(CGDirectDisplayID, int *);
  typedef struct cgs_mode_desc
  {
    uint32 u32ModeNum;
    uint32 u32Flags;
    uint32 u32Width;
    uint32 u32Height;
    uint32 u32Depth;
    uint8 au8Pad1[170];
    uint16 u16Freq;
    uint8 au8Pad2[16];
    float fDensity;
  } cgs_mode_desc_st;
  typedef void (*cgs_desc_fn)(CGDirectDisplayID, int, cgs_mode_desc_st *, int);

  if (!g_bSkyLightTried)
  {
    g_pvSkyLight = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY);
    g_bSkyLightTried = DM_TRUE;
  }
  if (NULL == g_pvSkyLight)
  {
    return DM_NOT_FOUND;
  }
  cgs_num_fn pfNum = (cgs_num_fn)dlsym(g_pvSkyLight, "CGSGetNumberOfDisplayModes");
  cgs_desc_fn pfDesc = (cgs_desc_fn)dlsym(g_pvSkyLight, "CGSGetDisplayModeDescriptionOfLength");
  cgs_cfg_fn pfCfg = (cgs_cfg_fn)dlsym(g_pvSkyLight, "CGSConfigureDisplayMode");
  if ((NULL == pfNum) || (NULL == pfDesc) || (NULL == pfCfg))
  {
    return DM_NOT_FOUND;
  }
  int iCnt = 0;
  int iIdx = 0;
  pfNum(u32DispId, &iCnt);
  if (iCnt <= 0) { return DM_NOT_FOUND; }
  if (iCnt > 4096) { iCnt = 4096; }
  for (iIdx = 0; iIdx < iCnt; iIdx++)
  {
    cgs_mode_desc_st stD;
    memset(&stD, 0, sizeof(stD));
    pfDesc(u32DispId, iIdx, &stD, (int)sizeof(stD));
    uint32 u32Scale = (stD.fDensity >= 1.5f) ? 2u : 1u;
    if ((stD.u32Width == pstWant->u32W) && (stD.u32Height == pstWant->u32H)
        && ((stD.u32Width * u32Scale) == pstWant->u32Pw))
    {
      CGDisplayConfigRef pCfg = NULL;
      if (kCGErrorSuccess == CGBeginDisplayConfiguration(&pCfg))
      {
        if (0 != pfCfg(pCfg, u32DispId, (int)stD.u32ModeNum))
        {
          (void)CGCancelDisplayConfiguration(pCfg);
        }
        else if (kCGErrorSuccess == CGCompleteDisplayConfiguration(pCfg, kCGConfigurePermanently))
        {
          return DM_SUCCESS;
        }
      }
    }
  }
  return DM_NOT_FOUND;
}
