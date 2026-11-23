/**
 ******************************************************************************
 * @file    af_shell_app.m
 * @author  Afar_C
 * @version 1.0.0
 * @brief   B2 [NSApp run] 唯一主控代理与 DMG 自升级顺序实现（DD-01 §3）。
 ******************************************************************************
 */

#include <dm/link.h>
#include <dm/module.h>
#include "af_modules.h"
#include "af_ui_ipc.h"
#include "af_store.h"
#include "plat/af_plat.h"
#include "af_shell_app.h"
#include "af_shell_menu.h"
#include "af_shell_win.h"
#include "plat/af_objc_undef.h"

#import <Cocoa/Cocoa.h>
#import <UserNotifications/UserNotifications.h>

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void af__parse_ver3(const char *pcVer, int aiOut[3])
{
  aiOut[0] = 0;
  aiOut[1] = 0;
  aiOut[2] = 0;
  if (NULL == pcVer)
  {
    return;
  }
  if (('v' == pcVer[0]) || ('V' == pcVer[0]))
  {
    pcVer++;
  }
  (void)sscanf(pcVer, "%d.%d.%d", &aiOut[0], &aiOut[1], &aiOut[2]);
}

bool_t af_shell_ver_newer(const char *pcNewVer, const char *pcCurVer)
{
  int aiNew[3];
  int aiCur[3];
  int i = 0;

  af__parse_ver3(pcNewVer, aiNew);
  af__parse_ver3(pcCurVer, aiCur);
  for (i = 0; i < 3; i++)
  {
    if (aiNew[i] != aiCur[i])
    {
      return (aiNew[i] > aiCur[i]) ? DM_TRUE : DM_FALSE;
    }
  }
  return DM_FALSE;
}

bool_t af_shell_in_applications(const char *pcAppPath)
{
  const char *pcHome = getenv("HOME");
  char acUserApps[512];

  if (NULL == pcAppPath)
  {
    return DM_FALSE;
  }
  if (0 == strncmp(pcAppPath, "/Applications/", 14u))
  {
    return DM_TRUE;
  }
  if ((NULL != pcHome) && ('\0' != pcHome[0]))
  {
    snprintf(acUserApps, sizeof(acUserApps), "%s/Applications/", pcHome);
    if (0 == strncmp(pcAppPath, acUserApps, strlen(acUserApps)))
    {
      return DM_TRUE;
    }
  }
  return DM_FALSE;
}

bool_t af_shell_find_app_bundle(const char *pcExePath, char *pcAppOut, size_t uCap)
{
  char acReal[PATH_MAX];
  const char *pcPath = pcExePath;
  const char *pcMark = NULL;
  size_t uLen = 0u;

  if ((NULL == pcExePath) || (NULL == pcAppOut) || (0u == uCap))
  {
    return DM_FALSE;
  }
  pcAppOut[0] = '\0';
  if (NULL != realpath(pcExePath, acReal))
  {
    pcPath = acReal;
  }
  pcMark = strstr(pcPath, ".app/Contents/MacOS/");
  if (NULL == pcMark)
  {
    return DM_FALSE;
  }
  uLen = (size_t)(pcMark - pcPath) + 4u;
  if (uLen >= uCap)
  {
    return DM_FALSE;
  }
  memcpy(pcAppOut, pcPath, uLen);
  pcAppOut[uLen] = '\0';
  return DM_TRUE;
}

static bool_t af__prod_is_running(void *pvCtx, char *pcVerOut, size_t uCap)
{
  char acTarget[64];
  char acOut[512];
  int iExit = -1;
  _Atomic bool_t bCancel = DM_FALSE;

  (void)pvCtx;
  if ((NULL != pcVerOut) && (uCap > 0u))
  {
    pcVerOut[0] = '\0';
  }
  @autoreleasepool
  {
    snprintf(acTarget, sizeof(acTarget), "gui/%u/com.afar.agent", (unsigned)getuid());
    const char *apcPrint[4] = {"/bin/launchctl", "print", acTarget, NULL};
    if ((DM_SUCCESS != af_plat_run_cmd_cancellable(apcPrint, NULL, &bCancel, acOut, sizeof(acOut),
                                                   3000u, &iExit))
        || (0 != iExit) || (NULL == strstr(acOut, "state = running")))
    {
      return DM_FALSE;
    }
    NSDictionary *oPlist = [NSDictionary
        dictionaryWithContentsOfFile:@"/Applications/Afar.app/Contents/Info.plist"];
    NSString *oVer = [oPlist objectForKey:@"CFBundleShortVersionString"];
    if ([oVer isKindOfClass:[NSString class]] && (NULL != pcVerOut) && (uCap > 0u))
    {
      snprintf(pcVerOut, uCap, "%s", [oVer UTF8String] ?: "");
    }
    else if ((NULL != pcVerOut) && (uCap > 0u))
    {
      const char *apcVer[3] = {"/Applications/Afar.app/Contents/MacOS/afar-agent", "--version",
                               NULL};
      if ((DM_SUCCESS == af_plat_run_cmd_cancellable(apcVer, NULL, &bCancel, acOut, sizeof(acOut),
                                                     3000u, &iExit))
          && (0 == iExit))
      {
        size_t uOutLen = strlen(acOut);
        const char *pcSp = NULL;
        while ((uOutLen > 0u) && (('\n' == acOut[uOutLen - 1u]) || ('\r' == acOut[uOutLen - 1u])
                                  || (' ' == acOut[uOutLen - 1u]) || ('\t' == acOut[uOutLen - 1u])))
        {
          acOut[--uOutLen] = '\0';
        }
        pcSp = strrchr(acOut, ' ');
        snprintf(pcVerOut, uCap, "%s", (NULL != pcSp) ? (pcSp + 1) : acOut);
      }
    }
    return DM_TRUE;
  }
}

static int af__prod_replace_app(void *pvCtx, const char *pcSrcApp, const char *pcDstApp)
{
  char acNew[PATH_MAX];
  char acOld[PATH_MAX];
  char acOut[512];
  int iExit = -1;
  bool_t bHadOld = DM_FALSE;
  _Atomic bool_t bCancel = DM_FALSE;

  (void)pvCtx;
  if ((NULL == pcSrcApp) || (NULL == pcDstApp))
  {
    return DM_INVALID;
  }
  snprintf(acNew, sizeof(acNew), "%s.new", pcDstApp);
  snprintf(acOld, sizeof(acOld), "%s.old", pcDstApp);
  {
    const char *apcRmNew[4] = {"/bin/rm", "-rf", acNew, NULL};
    const char *apcRmOld[4] = {"/bin/rm", "-rf", acOld, NULL};
    const char *apcDitto[4] = {"/usr/bin/ditto", pcSrcApp, acNew, NULL};
    (void)af_plat_run_cmd_cancellable(apcRmNew, NULL, &bCancel, acOut, sizeof(acOut), 10000u,
                                      &iExit);
    (void)af_plat_run_cmd_cancellable(apcRmOld, NULL, &bCancel, acOut, sizeof(acOut), 10000u,
                                      &iExit);
    if ((DM_SUCCESS != af_plat_run_cmd_cancellable(apcDitto, NULL, &bCancel, acOut, sizeof(acOut),
                                                   60000u, &iExit))
        || (0 != iExit))
    {
      (void)af_plat_run_cmd_cancellable(apcRmNew, NULL, &bCancel, acOut, sizeof(acOut), 10000u,
                                        &iExit);
      return DM_IO;
    }
    if (0 == access(pcDstApp, F_OK))
    {
      if (0 != rename(pcDstApp, acOld))
      {
        (void)af_plat_run_cmd_cancellable(apcRmNew, NULL, &bCancel, acOut, sizeof(acOut), 10000u,
                                          &iExit);
        return DM_IO;
      }
      bHadOld = DM_TRUE;
    }
    if (0 != rename(acNew, pcDstApp))
    {
      if (bHadOld)
      {
        (void)rename(acOld, pcDstApp);
      }
      (void)af_plat_run_cmd_cancellable(apcRmNew, NULL, &bCancel, acOut, sizeof(acOut), 10000u,
                                        &iExit);
      return DM_IO;
    }
    if (bHadOld)
    {
      (void)af_plat_run_cmd_cancellable(apcRmOld, NULL, &bCancel, acOut, sizeof(acOut), 10000u,
                                        &iExit);
    }
  }
  return DM_SUCCESS;
}

static void af__prod_open_app(void *pvCtx, const char *pcDstApp)
{
  char acOut[256];
  int iExit = -1;
  _Atomic bool_t bCancel = DM_FALSE;
  const char *apcOpen[4] = {"/usr/bin/open", "-n", pcDstApp, NULL};

  (void)pvCtx;
  if (NULL != pcDstApp)
  {
    (void)af_plat_run_cmd_cancellable(apcOpen, NULL, &bCancel, acOut, sizeof(acOut), 10000u,
                                      &iExit);
  }
}

static void af__prod_show_alert(NSString *oTitle, NSString *oBody)
{
  @autoreleasepool
  {
    NSApplication *oApp = [NSApplication sharedApplication];
    [oApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [oApp activateIgnoringOtherApps:YES];
    NSAlert *oAlert = [[NSAlert alloc] init];
    oAlert.messageText = oTitle;
    oAlert.informativeText = oBody;
    [oAlert addButtonWithTitle:@"好"];
    [oAlert runModal];
  }
}

static void af__prod_drag_alert(void *pvCtx)
{
  (void)pvCtx;
  af__prod_show_alert(@"请先拖进“应用程序”",
                      @"请将 Afar 拖入“应用程序”(Applications) 文件夹后再打开。");
}

static void af__prod_same_ver_alert(void *pvCtx, const char *pcCurVer)
{
  (void)pvCtx;
  @autoreleasepool
  {
    NSString *oVer = [NSString stringWithUTF8String:(NULL != pcCurVer) ? pcCurVer : ""] ?: @"";
    NSString *oBody = [NSString stringWithFormat:@"当前已在运行 Afar %@（相同或更新版本）。", oVer];
    af__prod_show_alert(@"Afar 已在运行", oBody);
  }
}

static int af__prod_ensure_installed(void *pvCtx, const char *pcExePath)
{
  const af_bundle_launch_cb_st *pstCb = (const af_bundle_launch_cb_st *)pvCtx;
  return ((NULL != pstCb) && (NULL != pstCb->pfEnsureInstalled))
             ? pstCb->pfEnsureInstalled(pcExePath)
             : 0;
}

static int af__prod_wake_running(void *pvCtx)
{
  const af_bundle_launch_cb_st *pstCb = (const af_bundle_launch_cb_st *)pvCtx;
  return ((NULL != pstCb) && (NULL != pstCb->pfWakeRunning)) ? pstCb->pfWakeRunning() : 0;
}

af_bundle_action_et af_shell_launch_bundle(const char *pcAppPath, const char *pcExePath,
                                           const char *pcNewVer,
                                           const af_bundle_launch_cb_st *pstCb)
{
  static const af_bundle_ops_st stProdOps = {
      .pfIsRunning = af__prod_is_running,
      .pfReplaceApp = af__prod_replace_app,
      .pfOpenApp = af__prod_open_app,
      .pfShowDragAlert = af__prod_drag_alert,
      .pfShowSameVerAlert = af__prod_same_ver_alert,
      .pfEnsureInstalled = af__prod_ensure_installed,
      .pfWakeRunning = af__prod_wake_running
  };

  @autoreleasepool
  {
    af_bundle_action_et eAct = af_shell_launch_bundle_ex(pcAppPath, pcExePath, pcNewVer, &stProdOps,
                                                         (void *)(uintptr_t)pstCb);
    if (AF_BUNDLE_ACT_UPGRADE_FAILED == eAct)
    {
      af__prod_show_alert(@"Afar 升级失败",
                          @"无法写入 /Applications/Afar.app，请检查权限或手动拖入应用程序。");
    }
    return eAct;
  }
}

af_bundle_action_et af_shell_launch_bundle_ex(const char *pcAppPath, const char *pcExePath,
                                              const char *pcNewVer, const af_bundle_ops_st *pstOps,
                                              void *pvCtx)
{
  char acCurVer[64] = {0};

  if ((NULL == pcAppPath) || (NULL == pstOps))
  {
    return AF_BUNDLE_ACT_NONE;
  }
  if (!af_shell_in_applications(pcAppPath))
  {
    /* N-08: 先查后台是否已有旧版在跑 -> 有则走版本比对与 ditto 自升级；无才弹拖入提示 */
    if ((NULL != pstOps->pfIsRunning) && pstOps->pfIsRunning(pvCtx, acCurVer, sizeof(acCurVer)))
    {
      if (!af_shell_ver_newer(pcNewVer, acCurVer))
      {
        if (NULL != pstOps->pfShowSameVerAlert)
        {
          pstOps->pfShowSameVerAlert(pvCtx, acCurVer);
        }
        return AF_BUNDLE_ACT_SAME_OR_OLDER;
      }
      if ((NULL == pstOps->pfReplaceApp)
          || (DM_SUCCESS != pstOps->pfReplaceApp(pvCtx, pcAppPath, "/Applications/Afar.app")))
      {
        return AF_BUNDLE_ACT_UPGRADE_FAILED;
      }
      if (NULL != pstOps->pfOpenApp)
      {
        pstOps->pfOpenApp(pvCtx, "/Applications/Afar.app");
      }
      return AF_BUNDLE_ACT_UPGRADED;
    }
    if (NULL != pstOps->pfShowDragAlert)
    {
      pstOps->pfShowDragAlert(pvCtx);
    }
    return AF_BUNDLE_ACT_DRAG_PROMPT;
  }

  /* F-03: 已在 /Applications 内 -> 确保 plist 指向 [exe, "run"] 并唤醒后台开配置窗 */
  if (NULL != pstOps->pfEnsureInstalled)
  {
    (void)pstOps->pfEnsureInstalled(pvCtx, pcExePath);
  }
  if (NULL != pstOps->pfWakeRunning)
  {
    (void)pstOps->pfWakeRunning(pvCtx);
  }
  return AF_BUNDLE_ACT_INSTALLED_AND_WOKEN;
}

@interface AFAppDelegate : NSObject <NSApplicationDelegate, UNUserNotificationCenterDelegate>
@property (nonatomic, assign) const af_shell_run_cfg_st *cfg;
@property (nonatomic, assign) pthread_t dmTid;
@property (nonatomic, assign) BOOL dmStarted;
@property (nonatomic, assign) BOOL terminating;
@property (nonatomic, assign) int mainExit;
@property (nonatomic, assign) int mainLastErr;
- (void)onDmExited:(int)iExitCode lastErr:(int)iLastErr;
@end

static void *af__dm_thread_entry(void *pvArg)
{
  AFAppDelegate *oDel = (__bridge AFAppDelegate *)pvArg;
  const af_shell_run_cfg_st *pstC = oDel.cfg;
  int iRc = dm_main_ex(pstC->argc, pstC->argv, pstC->pstDmCfg, pstC->pastMods, pstC->uiModCount);
  int iLastErr = dm_main_last_err();
  dispatch_async(dispatch_get_main_queue(), ^{
    [oDel onDmExited:iRc lastErr:iLastErr];
  });
  return NULL;
}

static void af__stop_nsapp(void)
{
  [NSApp stop:nil];
  NSEvent *oEvt = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                     location:NSZeroPoint
                                modifierFlags:0
                                    timestamp:0
                                 windowNumber:0
                                      context:nil
                                      subtype:0
                                        data1:0
                                        data2:0];
  [NSApp postEvent:oEvt atStart:YES];
}

@implementation AFAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)oNote
{
  (void)oNote;
  if (nil != [[NSBundle mainBundle] bundleIdentifier])
  {
    [UNUserNotificationCenter currentNotificationCenter].delegate = self;
  }
  if ((NULL != self.cfg) && (!self.cfg->bNoMenubar))
  {
    af_shell_menu_init();
  }
  pthread_t tTid;
  if (0 != pthread_create(&tTid, NULL, af__dm_thread_entry, (__bridge void *)self))
  {
    self.dmStarted = NO;
    self.mainExit = 1;
    self.mainLastErr = DM_FAILD;
    af__stop_nsapp();
    return;
  }
  self.dmTid = tTid;
  self.dmStarted = YES;
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)oApp hasVisibleWindows:(BOOL)bFlag
{
  (void)oApp;
  (void)bFlag;
  af_ui_action_msg_st stMsg = {.eAct = AF_MENU_REOPEN, .bVal = DM_FALSE};
  (void)dm_queue_post_async(AF_LINK_UI_ACTION, &stMsg, (uint32)sizeof(stMsg));
  return YES;
}

- (void)userNotificationCenter:(UNUserNotificationCenter *)oCenter
didReceiveNotificationResponse:(UNNotificationResponse *)oResp
         withCompletionHandler:(void (^)(void))oDone
{
  (void)oCenter;
  NSString *oId = oResp.notification.request.identifier;
  if ((nil != oId) && [oId hasPrefix:@"paired"])
  {
    af_ui_action_msg_st stMsg = {.eAct = AF_MENU_NOTIFY_DEVICES, .bVal = DM_FALSE};
    (void)dm_queue_post_async(AF_LINK_UI_ACTION, &stMsg, (uint32)sizeof(stMsg));
  }
  if (nil != oDone)
  {
    oDone();
  }
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)oSender
{
  (void)oSender;
  if (!self.dmStarted)
  {
    return NSTerminateNow;
  }
  if (!self.terminating)
  {
    self.terminating = YES;
    atomic_store(&g_bAfQuitRequested, DM_TRUE);
    (void)dm_queue_post_async(AF_LINK_QUIT, NULL, 0u);
  }
  return NSTerminateLater;
}

- (void)onDmExited:(int)iExitCode lastErr:(int)iLastErr
{
  if (self.dmStarted)
  {
    (void)pthread_join(self.dmTid, NULL);
    self.dmStarted = NO;
  }
  af_plat_vd_destroy_all();
  af_shell_win_destroy();
  af_shell_menu_destroy();
  self.mainExit = iExitCode;
  self.mainLastErr = iLastErr;
  if (self.terminating)
  {
    [NSApp replyToApplicationShouldTerminate:YES];
    return;
  }
  af__stop_nsapp();
}

@end

int af_shell_run_app(const af_shell_run_cfg_st *pstCfg)
{
  if ((NULL == pstCfg) || (NULL == pstCfg->pstDmCfg))
  {
    return 1;
  }
  atomic_store(&g_bAfQuitting, DM_FALSE);
  atomic_store(&g_bAfQuitRequested, DM_FALSE);
  @autoreleasepool
  {
    NSApplication *oApp = [NSApplication sharedApplication];
    [oApp setActivationPolicy:pstCfg->bNoMenubar ? NSApplicationActivationPolicyProhibited
                                                 : NSApplicationActivationPolicyAccessory];
    AFAppDelegate *oDel = [[AFAppDelegate alloc] init];
    oDel.cfg = pstCfg;
    oApp.delegate = oDel;
    [oApp run];
    oApp.delegate = nil;
    if ((0 != oDel.mainExit) && (DM_BUSY == oDel.mainLastErr))
    {
      if (NULL != pstCfg->pfOnWakeExisting)
      {
        pstCfg->pfOnWakeExisting();
      }
      return 0;
    }
    return oDel.mainExit;
  }
}
