/**
 ******************************************************************************
 * @file    af_plat_stream.m
 * @author  Afar_C
 * @version 1.0.0
 * @brief   macOS ScreenCaptureKit 屏幕采集、150ms 静止补帧与生命周期编排。
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
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <stdatomic.h>
#include <string.h>

#define AF_PLAT_IDLE_SHARP_NS (150ULL * NSEC_PER_MSEC)
#define AF_PLAT_SC_WAIT_NS (5ULL * NSEC_PER_SEC)
#define AF_PLAT_STOP_WAIT_NS (2ULL * NSEC_PER_SEC)

@interface AFPlatStreamDel : NSObject <SCStreamDelegate, SCStreamOutput>
@property(nonatomic, assign) uint64 u64Gen;
@end

@interface AFPlatStartGate : NSObject
{
@public
  _Atomic int iState; /* 0=pending, 1=timed_out, 2=completed */
}
@property(nonatomic, assign) BOOL bOk;
@end

@implementation AFPlatStartGate
@end

af__plat_stream_ctx_st g_stAfPs;
static SCStream *g_oScStream = nil;
static AFPlatStreamDel *g_oScDel = nil;
static id g_oSleepObs = nil;
static dispatch_source_t g_oIdleTimer = nil;
static dispatch_queue_t g_oCapQueue = nil;
static dispatch_group_t g_oStopGrp = nil;
static uint32 g_u32StopInflight = 0u;
static _Atomic uint64 g_u64PsGen = 0u;

static void af__ps_on_sample(dm_in uint64 u64Gen, dm_in CMSampleBufferRef pSample);
static void af__ps_on_sc_error(dm_in uint64 u64Gen, dm_in int iReason);

@implementation AFPlatStreamDel
- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type
{
  (void)stream;
  if ((SCStreamOutputTypeScreen == type) && (NULL != sampleBuffer))
  {
    af__ps_on_sample(self.u64Gen, sampleBuffer);
  }
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
  (void)stream;
  (void)error;
  af__ps_on_sc_error(self.u64Gen, DM_IO);
}
@end

/* ============================ 串行队列与 150ms 静止补帧 ============================ */

static void af__ps_init_gcd(void)
{
  static dispatch_once_t tOnce;

  dispatch_once(&tOnce, ^{
    g_oCapQueue = dispatch_queue_create("com.afar.stream.capture", DISPATCH_QUEUE_SERIAL);
    g_oStopGrp = dispatch_group_create();
  });
}

static dispatch_queue_t af__ps_queue(void)
{
  af__ps_init_gcd();
  return g_oCapQueue;
}

static dispatch_group_t af__ps_stop_grp(void)
{
  af__ps_init_gcd();
  return g_oStopGrp;
}

static void af__ps_arm_idle_timer(void)
{
  if (nil == g_oIdleTimer)
  {
    g_oIdleTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0u, 0u, af__ps_queue());
    dispatch_source_set_event_handler(g_oIdleTimer, ^{
      if (g_stAfPs.bActive && (!g_stAfPs.bStopping) && (NULL != g_stAfPs.pLastBuf))
      {
        af__ps_vt_encode_buf(g_stAfPs.pLastBuf, DM_FALSE, AF_VFLAG_IDLE_SHARP);
      }
      if (nil != g_oIdleTimer)
      {
        dispatch_source_set_timer(g_oIdleTimer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0u);
      }
    });
    dispatch_resume(g_oIdleTimer);
  }
  dispatch_source_set_timer(g_oIdleTimer, dispatch_time(DISPATCH_TIME_NOW, AF_PLAT_IDLE_SHARP_NS),
                            DISPATCH_TIME_FOREVER, 10ULL * NSEC_PER_MSEC);
}

static void af__ps_cancel_idle_timer(void)
{
  if (nil != g_oIdleTimer)
  {
    dispatch_source_cancel(g_oIdleTimer);
    g_oIdleTimer = nil;
  }
}

static void af__ps_on_sample(dm_in uint64 u64Gen, dm_in CMSampleBufferRef pSample)
{
  CVPixelBufferRef pBuf = NULL;
  bool_t bForceKey = DM_FALSE;

  if ((!g_stAfPs.bActive) || g_stAfPs.bStopping || (g_stAfPs.stCfg.u64Gen != u64Gen))
  {
    return;
  }
  pBuf = CMSampleBufferGetImageBuffer(pSample);
  if (NULL == pBuf)
  {
    return;
  }
  if (NULL != g_stAfPs.pLastBuf)
  {
    CVPixelBufferRelease(g_stAfPs.pLastBuf);
  }
  g_stAfPs.pLastBuf = CVPixelBufferRetain(pBuf);
  bForceKey = g_stAfPs.bForceNextIdr;
  g_stAfPs.bForceNextIdr = DM_FALSE;
  af__ps_vt_encode_buf(pBuf, bForceKey, 0u);
  af__ps_arm_idle_timer();
}

static void af__ps_on_sc_error(dm_in uint64 u64Gen, dm_in int iReason)
{
  dispatch_async(af__ps_queue(), ^{
    if (g_stAfPs.bActive && (!g_stAfPs.bStopping) && (g_stAfPs.stCfg.u64Gen == u64Gen) &&
        (NULL != g_stAfPs.stCfg.pfOnStopped))
    {
      g_stAfPs.stCfg.pfOnStopped(u64Gen, iReason, g_stAfPs.stCfg.pvUser);
    }
  });
}

/* ============================ SCStream 创建与启停 ============================ */

static SCDisplay *af__ps_pick_display(dm_in SCShareableContent *oContent, dm_in uint32 u32DispId)
{
  if (nil == oContent)
  {
    return nil;
  }
  for (SCDisplay *oD in oContent.displays)
  {
    if ((0u == u32DispId) || (oD.displayID == u32DispId))
    {
      return oD;
    }
  }
  return nil;
}

static SCDisplay *af__ps_find_display(dm_in uint32 u32DispId)
{
  __block SCDisplay *oFound = nil;
  dispatch_semaphore_t oSem = dispatch_semaphore_create(0);

  [SCShareableContent
      getShareableContentExcludingDesktopWindows:NO
                             onScreenWindowsOnly:NO
                               completionHandler:^(SCShareableContent *oContent, NSError *oErr) {
                                 if (nil == oErr)
                                 {
                                   oFound = af__ps_pick_display(oContent, u32DispId);
                                 }
                                 dispatch_semaphore_signal(oSem);
                               }];
  (void)dispatch_semaphore_wait(oSem, dispatch_time(DISPATCH_TIME_NOW, AF_PLAT_SC_WAIT_NS));
  return oFound;
}

static void af__ps_leave_stop_on_q(void)
{
  if (g_u32StopInflight > 0u)
  {
    g_u32StopInflight--;
  }
  dispatch_group_leave(af__ps_stop_grp());
}

static void af__ps_stop_orphan_sc(dm_in SCStream *oSt)
{
  if (nil == oSt)
  {
    return;
  }
  dispatch_group_enter(af__ps_stop_grp());
  dispatch_async(af__ps_queue(), ^{
    g_u32StopInflight++;
    [oSt stopCaptureWithCompletionHandler:^(NSError *oErr) {
      (void)oErr;
      dispatch_async(af__ps_queue(), ^{
        af__ps_leave_stop_on_q();
      });
    }];
  });
}

static int af__ps_build_and_start_sc(dm_in SCDisplay *oDisp,
                                     dm_in const af_plat_stream_cfg_st *pstCfg,
                                     dm_in bool_t bEff444, dm_out SCStream *__strong *poOutSt,
                                     dm_out AFPlatStreamDel *__strong *poOutDel)
{
  @autoreleasepool
  {
    SCContentFilter *oFilter = [[SCContentFilter alloc] initWithDisplay:oDisp excludingWindows:@[]];
    SCStreamConfiguration *oCfg = [[SCStreamConfiguration alloc] init];
    AFPlatStreamDel *oDel = [[AFPlatStreamDel alloc] init];
    SCStream *oSt = nil;
    NSError *oErr = nil;
    AFPlatStartGate *oGate = [[AFPlatStartGate alloc] init];
    dispatch_semaphore_t oSem = dispatch_semaphore_create(0);
    int32 i32Fps = (0u != pstCfg->u16Fps) ? (int32)pstCfg->u16Fps : 60;

    *poOutSt = nil;
    *poOutDel = nil;
    oCfg.width = pstCfg->u16EncW;
    oCfg.height = pstCfg->u16EncH;
    oCfg.minimumFrameInterval = CMTimeMake(1, i32Fps);
    oCfg.queueDepth = 3;
    oCfg.showsCursor = YES;
    oCfg.pixelFormat = bEff444 ? kCVPixelFormatType_444YpCbCr8BiPlanarFullRange
                               : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    oDel.u64Gen = pstCfg->u64Gen;
    oSt = [[SCStream alloc] initWithFilter:oFilter configuration:oCfg delegate:oDel];
    if ((nil == oSt) || (![oSt addStreamOutput:oDel
                                          type:SCStreamOutputTypeScreen
                            sampleHandlerQueue:af__ps_queue()
                                         error:&oErr]))
    {
      return DM_FAILD;
    }
    atomic_store(&oGate->iState, 0);
    [oSt startCaptureWithCompletionHandler:^(NSError *oStartErr) {
      int iExp = 0;
      if (atomic_compare_exchange_strong(&oGate->iState, &iExp, 2))
      {
        oGate.bOk = (nil == oStartErr);
        dispatch_semaphore_signal(oSem);
      }
      else if (nil == oStartErr)
      {
        af__ps_stop_orphan_sc(oSt);
      }
    }];
    if (0 != dispatch_semaphore_wait(oSem, dispatch_time(DISPATCH_TIME_NOW, AF_PLAT_SC_WAIT_NS)))
    {
      int iExp = 0;
      if (!atomic_compare_exchange_strong(&oGate->iState, &iExp, 1) && oGate.bOk)
      {
        af__ps_stop_orphan_sc(oSt);
      }
      return DM_TIMEOUT;
    }
    if (!oGate.bOk)
    {
      return DM_FAILD;
    }
    *poOutSt = oSt;
    *poOutDel = oDel;
    return DM_SUCCESS;
  }
}

static void af__ps_teardown_all_locked(void)
{
  af__ps_vt_teardown_locked();
  g_oScDel = nil;
}

static void af__ps_remove_sleep_obs(void)
{
  if (nil != g_oSleepObs)
  {
    [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:g_oSleepObs];
    g_oSleepObs = nil;
  }
}

static void af__ps_attach_sleep_obs(dm_in uint64 u64Gen)
{
  af__ps_remove_sleep_obs();
  g_oSleepObs = [[[NSWorkspace sharedWorkspace] notificationCenter]
      addObserverForName:NSWorkspaceWillSleepNotification
                  object:nil
                   queue:nil
              usingBlock:^(NSNotification *oNote) {
                (void)oNote;
                af__ps_on_sc_error(u64Gen, DM_SHUTTING_DOWN);
              }];
}

int af_plat_stream_start(dm_in const af_plat_stream_cfg_st *pstCfg, dm_out bool_t *pbEff444)
{
  SCDisplay *oDisp = nil;
  SCStream *oNewSt = nil;
  AFPlatStreamDel *oNewDel = nil;
  __block int iErr = DM_FAILD;
  __block bool_t bEff = DM_FALSE;

  if ((NULL == pstCfg) || (NULL == pbEff444) || (0u == pstCfg->u16EncW) || (0u == pstCfg->u16EncH))
  {
    return DM_INVALID;
  }
  if (!af_plat_perm_screen_capture())
  {
    return AF_ERR_PERM_SC;
  }
  /* H3: SCShareableContent 枚举在 Q_capture 之外执行，严禁在串行队列内等 5s 信号量 */
  oDisp = af__ps_find_display(pstCfg->u32DispId);
  if (nil == oDisp)
  {
    return DM_NOT_FOUND;
  }
  dispatch_sync(af__ps_queue(), ^{
    if (g_stAfPs.bActive || (g_u32StopInflight > 0u))
    {
      iErr = AF_ERR_STREAM_BUSY;
      return;
    }
    memset(&g_stAfPs, 0, sizeof(g_stAfPs));
    g_stAfPs.stCfg = *pstCfg;
    g_stAfPs.bForceNextIdr = DM_TRUE;
    iErr = af__ps_vt_create(pstCfg, &bEff);
    if (DM_SUCCESS == iErr)
    {
      g_stAfPs.bActive = DM_TRUE;
      atomic_store(&g_u64PsGen, pstCfg->u64Gen);
    }
  });
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr = af__ps_build_and_start_sc(oDisp, pstCfg, bEff, &oNewSt, &oNewDel);
  dispatch_sync(af__ps_queue(), ^{
    bool_t bSame = g_stAfPs.bActive && (!g_stAfPs.bStopping) &&
                   (g_stAfPs.stCfg.u64Gen == pstCfg->u64Gen) &&
                   (atomic_load(&g_u64PsGen) == pstCfg->u64Gen);
    if ((DM_SUCCESS != iErr) || (!bSame))
    {
      if (nil != oNewSt)
      {
        af__ps_stop_orphan_sc(oNewSt);
      }
      if (atomic_load(&g_u64PsGen) == pstCfg->u64Gen)
      {
        af__ps_teardown_all_locked();
      }
      iErr = (DM_SUCCESS != iErr) ? iErr : DM_SHUTTING_DOWN;
      return;
    }
    g_oScStream = oNewSt;
    g_oScDel = oNewDel;
    af__ps_attach_sleep_obs(pstCfg->u64Gen);
  });
  *pbEff444 = bEff;
  return iErr;
}

static void af__ps_finish_stop_on_q(dm_in uint64 u64Gen, dm_in af_plat_vid_stop_cb pfOnDone,
                                    dm_io void *pvUser)
{
  /* H1: 严格校验代次，过期停流绝不破坏新流 pVt 也不向新流回调 STOPPED */
  if (u64Gen != atomic_load(&g_u64PsGen))
  {
    return;
  }
  af__ps_teardown_all_locked();
  if (NULL != pfOnDone)
  {
    pfOnDone(u64Gen, DM_SUCCESS, pvUser);
  }
}

void af_plat_stream_stop_async(dm_in uint64 u64Gen, dm_in af_plat_vid_stop_cb pfOnDone,
                               dm_io void *pvUser)
{
  dispatch_group_enter(af__ps_stop_grp());
  dispatch_async(af__ps_queue(), ^{
    SCStream *oSt = nil;
    if (u64Gen != atomic_load(&g_u64PsGen))
    {
      dispatch_group_leave(af__ps_stop_grp());
      return;
    }
    oSt = g_oScStream;
    g_oScStream = nil;
    g_stAfPs.bStopping = DM_TRUE;
    g_stAfPs.bActive = DM_FALSE;
    af__ps_cancel_idle_timer();
    af__ps_remove_sleep_obs();
    if (nil == oSt)
    {
      af__ps_finish_stop_on_q(u64Gen, pfOnDone, pvUser);
      dispatch_group_leave(af__ps_stop_grp());
      return;
    }
    g_u32StopInflight++;
    [oSt stopCaptureWithCompletionHandler:^(NSError *oErr) {
      (void)oErr;
      dispatch_async(af__ps_queue(), ^{
        af__ps_finish_stop_on_q(u64Gen, pfOnDone, pvUser);
        af__ps_leave_stop_on_q();
      });
    }];
  });
}

void af_plat_stream_stop_sync(void)
{
  uint64 u64Gen = atomic_load(&g_u64PsGen);

  af_plat_stream_stop_async(u64Gen, NULL, NULL);
  (void)dispatch_group_wait(af__ps_stop_grp(),
                            dispatch_time(DISPATCH_TIME_NOW, AF_PLAT_STOP_WAIT_NS));
  dispatch_sync(af__ps_queue(), ^{
    if (u64Gen == atomic_load(&g_u64PsGen))
    {
      af__ps_teardown_all_locked();
    }
  });
}

void af_plat_stream_force_idr(dm_in uint64 u64Gen)
{
  dispatch_async(af__ps_queue(), ^{
    if ((!g_stAfPs.bActive) || g_stAfPs.bStopping || (g_stAfPs.stCfg.u64Gen != u64Gen))
    {
      return;
    }
    if ((NULL != g_stAfPs.pLastBuf) && (NULL != g_stAfPs.pVt))
    {
      af__ps_vt_encode_buf(g_stAfPs.pLastBuf, DM_TRUE, 0u);
    }
    else
    {
      g_stAfPs.bForceNextIdr = DM_TRUE;
    }
  });
}

void af_plat_stream_set_bitrate(dm_in uint64 u64Gen, dm_in uint32 u32AvgBps)
{
  uint32 u32Clamped = (u32AvgBps < AF_PLAT_MIN_BITRATE)
                          ? AF_PLAT_MIN_BITRATE
                          : ((u32AvgBps > AF_PLAT_AVG_BITRATE) ? AF_PLAT_AVG_BITRATE : u32AvgBps);
  uint32 u32Peak = (u32Clamped >= AF_PLAT_AVG_BITRATE)
                       ? AF_PLAT_PEAK_BYTES_PER_S
                       : (uint32)(((uint64)u32Clamped * 3ULL) / 16ULL);

  dispatch_async(af__ps_queue(), ^{
    if ((!g_stAfPs.bActive) || g_stAfPs.bStopping || (g_stAfPs.stCfg.u64Gen != u64Gen) ||
        (NULL == g_stAfPs.pVt))
    {
      return;
    }
    af__ps_vt_apply_bitrate(g_stAfPs.pVt, u32Clamped, u32Peak);
  });
}

static void af__ps_rebuild_sc_async(dm_in const af_plat_stream_cfg_st *pstSnap,
                                    dm_in bool_t bEff444)
{
  af_plat_stream_cfg_st stCfg = *pstSnap;

  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    SCDisplay *oDisp = af__ps_find_display(stCfg.u32DispId);
    SCStream *oNewSt = nil;
    AFPlatStreamDel *oNewDel = nil;
    int iErr = (nil != oDisp) ? af__ps_build_and_start_sc(oDisp, &stCfg, bEff444, &oNewSt, &oNewDel)
                              : DM_NOT_FOUND;
    dispatch_async(af__ps_queue(), ^{
      bool_t bSame = g_stAfPs.bActive && (!g_stAfPs.bStopping) &&
                     (g_stAfPs.stCfg.u64Gen == stCfg.u64Gen) &&
                     (atomic_load(&g_u64PsGen) == stCfg.u64Gen) &&
                     (g_stAfPs.stCfg.u32DispId == stCfg.u32DispId);
      if (DM_SUCCESS != iErr)
      {
        af__ps_on_sc_error(stCfg.u64Gen, iErr);
      }
      else if (!bSame)
      {
        af__ps_stop_orphan_sc(oNewSt);
      }
      else
      {
        g_oScStream = oNewSt;
        g_oScDel = oNewDel;
      }
    });
  });
}

void af_plat_stream_switch_disp(dm_in uint64 u64Gen, dm_in uint32 u32NewDispId)
{
  dispatch_async(af__ps_queue(), ^{
    SCStream *oOld = nil;
    af_plat_stream_cfg_st stSnap;
    bool_t bEff444 = DM_FALSE;

    if ((!g_stAfPs.bActive) || g_stAfPs.bStopping || (g_stAfPs.stCfg.u64Gen != u64Gen) ||
        (atomic_load(&g_u64PsGen) != u64Gen))
    {
      return;
    }
    oOld = g_oScStream;
    g_oScStream = nil;
    g_stAfPs.stCfg.u32DispId = u32NewDispId;
    g_stAfPs.bForceNextIdr = DM_TRUE;
    stSnap = g_stAfPs.stCfg;
    bEff444 = g_stAfPs.bEff444;
    if (nil == oOld)
    {
      af__ps_rebuild_sc_async(&stSnap, bEff444);
      return;
    }
    dispatch_group_enter(af__ps_stop_grp());
    g_u32StopInflight++;
    [oOld stopCaptureWithCompletionHandler:^(NSError *oErr) {
      (void)oErr;
      dispatch_async(af__ps_queue(), ^{
        if (g_stAfPs.bActive && (!g_stAfPs.bStopping) && (g_stAfPs.stCfg.u64Gen == u64Gen) &&
            (atomic_load(&g_u64PsGen) == u64Gen))
        {
          af__ps_rebuild_sc_async(&stSnap, bEff444);
        }
        af__ps_leave_stop_on_q();
      });
    }];
  });
}
