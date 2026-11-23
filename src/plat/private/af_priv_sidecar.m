/**
 ******************************************************************************
 * @file    af_priv_sidecar.m
 * @author  Afar_C
 * @version 1.0.0
 * @brief   macOS 私有 SidecarCore 动态加载与随航控制隔离实现（DD-01/03）。
 ******************************************************************************
 */

#include <dm.h>
#include "plat/af_plat.h"
#include "plat/af_objc_undef.h"

#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

@interface AFScWaitBox : NSObject
@property (nonatomic, assign) BOOL done;
@property (nonatomic, copy) NSString *errText;
@end

@implementation AFScWaitBox
@end

static id af__sc_manager(void)
{
  static BOOL g_bLoaded = NO;
  if (!g_bLoaded)
  {
    (void)dlopen("/System/Library/PrivateFrameworks/SidecarCore.framework/SidecarCore", RTLD_LAZY);
    g_bLoaded = YES;
  }
  Class oCls = NSClassFromString(@"SidecarDisplayManager");
  if (Nil == oCls)
  {
    return nil;
  }
  SEL tSel = NSSelectorFromString(@"sharedManager");
  if (![oCls respondsToSelector:tSel])
  {
    return nil;
  }
  return ((id (*)(id, SEL))objc_msgSend)(oCls, tSel);
}

int af_plat_sc_check_signatures(void)
{
  @autoreleasepool
  {
    id oMgr = af__sc_manager();
    if (nil == oMgr)
    {
      return DM_NOT_FOUND;
    }
    SEL aSels[4] = {
        NSSelectorFromString(@"devices"),
        NSSelectorFromString(@"connectedDevices"),
        NSSelectorFromString(@"connectToDevice:completion:"),
        NSSelectorFromString(@"disconnectFromDevice:completion:")
    };
    int i = 0;
    for (i = 0; i < 4; i++)
    {
      if (![oMgr respondsToSelector:aSels[i]])
      {
        return DM_NOT_FOUND;
      }
    }
    return DM_SUCCESS;
  }
}

uint32 af_plat_sc_list(af_sc_device_st *pastOut, uint32 u32Cap)
{
  if ((NULL == pastOut) || (0u == u32Cap))
  {
    return 0u;
  }
  @autoreleasepool
  {
    id oMgr = af__sc_manager();
    SEL tDevs = NSSelectorFromString(@"devices");
    SEL tConn = NSSelectorFromString(@"connectedDevices");
    if ((nil == oMgr) || ![oMgr respondsToSelector:tDevs] || ![oMgr respondsToSelector:tConn])
    {
      return 0u;
    }
    NSArray *oAll = ((id (*)(id, SEL))objc_msgSend)(oMgr, tDevs);
    NSArray *oCon = ((id (*)(id, SEL))objc_msgSend)(oMgr, tConn);
    NSMutableSet *oConNames = [NSMutableSet set];
    for (id oDev in oCon)
    {
      NSString *oN = [oDev valueForKey:@"name"];
      if (nil != oN) { [oConNames addObject:oN]; }
    }
    uint32 u32Cnt = 0u;
    for (id oDev in oAll)
    {
      if (u32Cnt >= u32Cap) { break; }
      NSString *oN = [oDev valueForKey:@"name"];
      if (nil == oN) { continue; }
      memset(&pastOut[u32Cnt], 0, sizeof(pastOut[u32Cnt]));
      snprintf(pastOut[u32Cnt].acName, sizeof(pastOut[u32Cnt].acName), "%s", [oN UTF8String]);
      pastOut[u32Cnt].bConnected = [oConNames containsObject:oN] ? DM_TRUE : DM_FALSE;
      u32Cnt++;
    }
    return u32Cnt;
  }
}

static int af__sc_invoke_one(id oMgr, SEL tAct, id oDev, char *pcErr, size_t uErrCap)
{
  AFScWaitBox *oBox = [[AFScWaitBox alloc] init];
  void (^oBlk)(NSError *) = ^(NSError *oErr) {
    @synchronized(oBox)
    {
      if (nil != oErr)
      {
        oBox.errText = [oErr localizedDescription] ?: @"Sidecar error";
      }
      oBox.done = YES;
    }
  };
  ((void (*)(id, SEL, id, id))objc_msgSend)(oMgr, tAct, oDev, oBlk);
  uint64 u64Deadline = dm_hrtime() + (15000ULL * 1000000ULL);
  for (;;)
  {
    BOOL bDone = NO;
    @synchronized(oBox) { bDone = oBox.done; }
    if (bDone || atomic_load(&g_bAfQuitting) || (dm_hrtime() >= u64Deadline))
    {
      break;
    }
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
  }
  BOOL bFin = NO;
  NSString *oErrTxt = nil;
  @synchronized(oBox)
  {
    bFin = oBox.done;
    oErrTxt = oBox.errText;
  }
  if (!bFin)
  {
    if ((NULL != pcErr) && (uErrCap > 0u))
    {
      snprintf(pcErr, uErrCap, "%s", atomic_load(&g_bAfQuitting) ? "quitting" : "Sidecar timeout");
    }
    return atomic_load(&g_bAfQuitting) ? DM_SHUTTING_DOWN : DM_TIMEOUT;
  }
  if (nil != oErrTxt)
  {
    if ((NULL != pcErr) && (uErrCap > 0u))
    {
      snprintf(pcErr, uErrCap, "%s", [oErrTxt UTF8String] ?: "Sidecar error");
    }
    return DM_FAILD;
  }
  return DM_SUCCESS;
}

static int af__sc_call_device(const char *pcName, BOOL bConnect, char *pcErr, size_t uErrCap)
{
  if ((NULL != pcErr) && (uErrCap > 0u)) { pcErr[0] = '\0'; }
  @autoreleasepool
  {
    id oMgr = af__sc_manager();
    SEL tList = NSSelectorFromString(bConnect ? @"devices" : @"connectedDevices");
    SEL tAct = NSSelectorFromString(bConnect ? @"connectToDevice:completion:"
                                             : @"disconnectFromDevice:completion:");
    if ((nil == oMgr) || ![oMgr respondsToSelector:tList] || ![oMgr respondsToSelector:tAct])
    {
      if ((NULL != pcErr) && (uErrCap > 0u)) { snprintf(pcErr, uErrCap, "SidecarCore unavailable"); }
      return DM_NOT_FOUND;
    }
    NSArray *oList = ((id (*)(id, SEL))objc_msgSend)(oMgr, tList);
    BOOL bMatched = NO;
    int iFirstErr = DM_SUCCESS;
    for (id oDev in oList)
    {
      NSString *oN = [oDev valueForKey:@"name"];
      if ((NULL != pcName) && ('\0' != pcName[0]))
      {
        if ((nil == oN) || (0 != strcmp([oN UTF8String], pcName))) { continue; }
      }
      bMatched = YES;
      int iRc = af__sc_invoke_one(oMgr, tAct, oDev, (DM_SUCCESS == iFirstErr) ? pcErr : NULL,
                                  (DM_SUCCESS == iFirstErr) ? uErrCap : 0u);
      if ((DM_SUCCESS != iRc) && (DM_SUCCESS == iFirstErr))
      {
        iFirstErr = iRc;
      }
      if (bConnect) { break; }
    }
    return bMatched ? iFirstErr : DM_NOT_FOUND;
  }
}

int af_plat_sc_connect(const char *pcName, char *pcErr, size_t uErrCap)
{
  return af__sc_call_device(pcName, YES, pcErr, uErrCap);
}

int af_plat_sc_disconnect(const char *pcName, char *pcErr, size_t uErrCap)
{
  return af__sc_call_device(pcName, NO, pcErr, uErrCap);
}
