/**
 ******************************************************************************
 * @file    af_shell_menu.m
 * @author  Afar_C
 * @version 1.0.0
 * @brief   macOS 原生 NSStatusItem 菜单栏壳实现（仅通过 dm_queue_post_async 与 DM 通信）。
 ******************************************************************************
 */

#include <dm/link.h>
#include <dm/module.h>
#include "af_modules.h"
#include "af_ui_ipc.h"
#include "plat/af_plat.h"
#include "af_shell_menu.h"
#include "af_shell_win.h"
#include "plat/af_objc_undef.h"

#import <Cocoa/Cocoa.h>

#include <stdio.h>
#include <string.h>

static NSString *const kAfLangDefaultsKey = @"AfarLang";

static void af__post_menu_act(af_menu_act_et eAct, bool_t bVal)
{
  af_ui_action_msg_st stMsg = {.eAct = eAct, .bVal = bVal};
  (void)dm_queue_post_async(AF_LINK_UI_ACTION, &stMsg, (uint32)sizeof(stMsg));
}

static NSString *af__lang_pref(void)
{
  NSString *oPref = [[NSUserDefaults standardUserDefaults] stringForKey:kAfLangDefaultsKey];
  if ([oPref isEqualToString:@"zh"] || [oPref isEqualToString:@"en"])
  {
    return oPref;
  }
  return @"";
}

bool_t af_shell_lang_is_zh(void)
{
  @autoreleasepool
  {
    NSString *oPref = af__lang_pref();
    if ([oPref isEqualToString:@"zh"])
    {
      return DM_TRUE;
    }
    if ([oPref isEqualToString:@"en"])
    {
      return DM_FALSE;
    }
    NSString *oSys = [NSLocale preferredLanguages].firstObject ?: @"en";
    return [oSys hasPrefix:@"zh"] ? DM_TRUE : DM_FALSE;
  }
}

void af_shell_lang_set(const char *pcLang)
{
  @autoreleasepool
  {
    NSUserDefaults *oDef = [NSUserDefaults standardUserDefaults];
    if ((NULL == pcLang) || ('\0' == pcLang[0]))
    {
      [oDef removeObjectForKey:kAfLangDefaultsKey];
    }
    else if (0 == strcmp(pcLang, "zh"))
    {
      [oDef setObject:@"zh" forKey:kAfLangDefaultsKey];
    }
    else if (0 == strcmp(pcLang, "en"))
    {
      [oDef setObject:@"en" forKey:kAfLangDefaultsKey];
    }
    [oDef synchronize];
  }
}

static NSString *af__menu_tr_status(const char *pcLine, BOOL bZh)
{
  NSString *oRaw = ((NULL != pcLine) && ('\0' != pcLine[0]))
                       ? ([NSString stringWithUTF8String:pcLine] ?: @"")
                       : @"";
  if (bZh)
  {
    return (oRaw.length > 0u) ? oRaw : @"未连接";
  }
  if (0u == oRaw.length || [oRaw isEqualToString:@"未连接"])
  {
    return @"Not Connected";
  }
  if ([oRaw hasPrefix:@"正在直连显示到 "])
  {
    return [@"Direct Streaming to " stringByAppendingString:[oRaw substringFromIndex:8u]];
  }
  if ([oRaw hasPrefix:@"正在显示到 "])
  {
    return [@"Showing on " stringByAppendingString:[oRaw substringFromIndex:6u]];
  }
  if ([oRaw hasPrefix:@"正在连接"])
  {
    return @"Connecting…";
  }
  if ([oRaw isEqualToString:@"需要处理"])
  {
    return @"Needs Attention";
  }
  if ([oRaw isEqualToString:@"已就绪，未连接"])
  {
    return @"Ready · Not Connected";
  }
  unsigned uCnt = 0u;
  if (1 == sscanf(pcLine, "还有 %u 项待配置", &uCnt))
  {
    return [NSString
        stringWithFormat:(1u == uCnt) ? @"%u setup item left" : @"%u setup items left", uCnt];
  }
  if (1 == sscanf(pcLine, "已就绪，附近有 %u 台 iPad", &uCnt))
  {
    return [NSString
        stringWithFormat:(1u == uCnt) ? @"Ready · %u iPad nearby" : @"Ready · %u iPads nearby",
                         uCnt];
  }
  return oRaw;
}

@interface AFMenuController : NSObject
@property(nonatomic, strong) NSStatusItem *statusItem;
@property(nonatomic, strong) NSMenu *menu;
@property(nonatomic, strong) NSMenuItem *statusLineItem;
@property(nonatomic, strong) NSMenuItem *permWarnItem;
@property(nonatomic, strong) NSMenuItem *connItem;
@property(nonatomic, strong) NSMenuItem *bootAutoItem;
@property(nonatomic, strong) NSMenuItem *antiUnplugItem;
@property(nonatomic, strong) NSMenuItem *pairItem;
@property(nonatomic, strong) NSMenuItem *setupItem;
@property(nonatomic, strong) NSMenuItem *resetItem;
@property(nonatomic, strong) NSMenuItem *langMenuItem;
@property(nonatomic, strong) NSMenuItem *langSysItem;
@property(nonatomic, strong) NSMenuItem *langZhItem;
@property(nonatomic, strong) NSMenuItem *langEnItem;
@property(nonatomic, strong) NSMenuItem *versionItem;
@property(nonatomic, strong) NSMenuItem *quitItem;
@property(nonatomic, assign) af_menu_snap_st snap;
- (void)applySnap:(af_menu_snap_st)stSnap;
- (void)refreshLang;
- (void)teardown;
@end

static AFMenuController *g_oMenuCtrl = nil;

@implementation AFMenuController

- (instancetype)init
{
  self = [super init];
  if (nil == self)
  {
    return nil;
  }
  memset(&_snap, 0, sizeof(_snap));
  _snap.bPermSc = DM_TRUE;
  _snap.bPermAx = DM_TRUE;
  snprintf(_snap.acStatusLine, sizeof(_snap.acStatusLine), "未连接");
  snprintf(_snap.acVersion, sizeof(_snap.acVersion), "%s", AF_VERSION);
  [self buildMenu];
  return self;
}

- (NSMenuItem *)makeItem:(NSString *)oTitle action:(SEL)tSel key:(NSString *)oKey
{
  NSMenuItem *oItem = [[NSMenuItem alloc] initWithTitle:oTitle action:tSel keyEquivalent:oKey];
  if (NULL != tSel)
  {
    oItem.target = self;
  }
  return oItem;
}

- (void)buildLangSubmenu
{
  NSMenu *oSub = [[NSMenu alloc] initWithTitle:@"Language"];
  self.langSysItem = [self makeItem:@"跟随系统 / System" action:@selector(onLangSys:) key:@""];
  self.langZhItem = [self makeItem:@"中文" action:@selector(onLangZh:) key:@""];
  self.langEnItem = [self makeItem:@"English" action:@selector(onLangEn:) key:@""];
  [oSub addItem:self.langSysItem];
  [oSub addItem:self.langZhItem];
  [oSub addItem:self.langEnItem];
  self.langMenuItem = [self makeItem:@"语言 / Language" action:NULL key:@""];
  self.langMenuItem.submenu = oSub;
  [self.menu addItem:self.langMenuItem];
}

- (void)buildMenu
{
  if (NSApplicationActivationPolicyProhibited == [NSApp activationPolicy])
  {
    return;
  }
  self.statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
  self.statusItem.button.title = @"▢";
  self.statusItem.button.toolTip = @"Afar";

  self.menu = [[NSMenu alloc] initWithTitle:@"Afar"];
  self.menu.autoenablesItems = NO;

  self.statusLineItem = [self makeItem:@"" action:NULL key:@""];
  self.statusLineItem.enabled = NO;
  [self.menu addItem:self.statusLineItem];
  self.permWarnItem = [self makeItem:@"" action:@selector(onOpenPermWarn:) key:@""];
  self.permWarnItem.hidden = YES;
  [self.menu addItem:self.permWarnItem];
  [self.menu addItem:[NSMenuItem separatorItem]];

  self.connItem = [self makeItem:@"" action:@selector(onToggleConnect:) key:@""];
  [self.menu addItem:self.connItem];
  [self.menu addItem:[NSMenuItem separatorItem]];

  self.bootAutoItem = [self makeItem:@"" action:@selector(onToggleBootAuto:) key:@""];
  [self.menu addItem:self.bootAutoItem];

  self.antiUnplugItem = [self makeItem:@"" action:@selector(onToggleAntiUnplug:) key:@""];
  [self.menu addItem:self.antiUnplugItem];
  [self.menu addItem:[NSMenuItem separatorItem]];

  self.pairItem = [self makeItem:@"" action:@selector(onPairNew:) key:@""];
  [self.menu addItem:self.pairItem];
  self.setupItem = [self makeItem:@"" action:@selector(onOpenSetup:) key:@""];
  [self.menu addItem:self.setupItem];
  self.resetItem = [self makeItem:@"" action:@selector(onResetSetup:) key:@""];
  [self.menu addItem:self.resetItem];
  [self.menu addItem:[NSMenuItem separatorItem]];

  [self buildLangSubmenu];
  NSString *oVer = [NSString stringWithFormat:@"Afar %s", AF_VERSION];
  self.versionItem = [self makeItem:oVer action:NULL key:@""];
  self.versionItem.enabled = NO;
  [self.menu addItem:self.versionItem];

  self.quitItem = [self makeItem:@"" action:@selector(onQuit:) key:@"q"];
  [self.menu addItem:self.quitItem];
  self.statusItem.menu = self.menu;
  [self applyLabels];
}

- (void)applyPermWarn:(BOOL)bZh
{
  if (nil == self.permWarnItem)
  {
    return;
  }
  if (self.snap.bPermSc && self.snap.bPermAx)
  {
    self.permWarnItem.hidden = YES;
    return;
  }
  NSString *oText = nil;
  if (!self.snap.bPermSc)
  {
    oText = bZh ? @"⚠ 需开启「屏幕录制」权限（授权后重启生效）…"
                : @"⚠ Enable Screen Recording (restart required)…";
  }
  else
  {
    oText = bZh ? @"⚠ 需开启「辅助功能」权限…" : @"⚠ Enable Accessibility Permission…";
  }
  NSDictionary *oAttrs = @{
    NSForegroundColorAttributeName : [NSColor systemRedColor],
    NSFontAttributeName : [NSFont menuFontOfSize:0.0]
  };
  self.permWarnItem.title = oText;
  self.permWarnItem.attributedTitle = [[NSAttributedString alloc] initWithString:oText
                                                                      attributes:oAttrs];
  self.permWarnItem.hidden = NO;
}

- (void)applyLabels
{
  if (nil == self.statusItem)
  {
    return;
  }
  BOOL bZh = (DM_TRUE == af_shell_lang_is_zh());
  NSString *oPref = af__lang_pref();
  self.statusLineItem.title = af__menu_tr_status(self.snap.acStatusLine, bZh);
  [self applyPermWarn:bZh];
  self.connItem.title = self.snap.bShowing ? (bZh ? @"断开 iPad" : @"Disconnect iPad")
                                           : (bZh ? @"立即连接" : @"Connect Now");
  self.bootAutoItem.title = bZh ? @"开机自动连接" : @"Connect at Startup";
  self.antiUnplugItem.title = bZh ? @"防拔线（保留虚拟屏）" : @"Keep Virtual Display on Unplug";
  self.pairItem.title = bZh ? @"配对新 iPad…" : @"Pair New iPad…";
  self.resetItem.title = bZh ? @"重新设置…" : @"Run Setup Again…";
  self.langMenuItem.title = bZh ? @"语言 / Language" : @"Language / 语言";
  self.langSysItem.title = bZh ? @"跟随系统" : @"Follow System";
  self.langSysItem.state = (0u == oPref.length) ? NSControlStateValueOn : NSControlStateValueOff;
  self.langZhItem.state =
      [oPref isEqualToString:@"zh"] ? NSControlStateValueOn : NSControlStateValueOff;
  self.langEnItem.state =
      [oPref isEqualToString:@"en"] ? NSControlStateValueOn : NSControlStateValueOff;
  self.quitItem.title = bZh ? @"退出" : @"Quit";
  if (self.snap.u32TodoCount > 0u)
  {
    self.setupItem.title = bZh ? [NSString stringWithFormat:@"检查并配置… (%u 项待办)",
                                                            (unsigned)self.snap.u32TodoCount]
                               : [NSString stringWithFormat:@"Check & Configure… (%u left)",
                                                            (unsigned)self.snap.u32TodoCount];
  }
  else
  {
    self.setupItem.title = bZh ? @"检查并配置…" : @"Check & Configure…";
  }
}

- (void)applySnap:(af_menu_snap_st)stSnap
{
  self.snap = stSnap;
  if (nil == self.statusItem)
  {
    return;
  }
  NSString *oIcon = @"▢";
  if (1u == stSnap.u8IconState)
  {
    oIcon = @"▣";
  }
  else if (2u == stSnap.u8IconState)
  {
    oIcon = @"⚠︎";
  }
  self.statusItem.button.title = oIcon;
  self.bootAutoItem.state = stSnap.bBootAuto ? NSControlStateValueOn : NSControlStateValueOff;
  self.antiUnplugItem.state = stSnap.bAntiUnplug ? NSControlStateValueOn : NSControlStateValueOff;
  const char *pcVer = ('\0' != stSnap.acVersion[0]) ? stSnap.acVersion : AF_VERSION;
  self.versionItem.title = [NSString stringWithFormat:@"Afar %s", pcVer];
  [self applyLabels];
}

- (void)refreshLang
{
  [self applyLabels];
}

- (void)selectLang:(const char *)pcLang
{
  af_shell_lang_set(pcLang);
  [self applyLabels];
  af_shell_win_refresh_lang();
}

- (void)onLangSys:(id)oSender
{
  (void)oSender;
  [self selectLang:""];
}

- (void)onLangZh:(id)oSender
{
  (void)oSender;
  [self selectLang:"zh"];
}

- (void)onLangEn:(id)oSender
{
  (void)oSender;
  [self selectLang:"en"];
}

- (void)onOpenPermWarn:(id)oSender
{
  (void)oSender;
  if (!self.snap.bPermSc)
  {
    af__post_menu_act(AF_MENU_OPEN_PERM_SC, DM_FALSE);
  }
  else if (!self.snap.bPermAx)
  {
    af__post_menu_act(AF_MENU_OPEN_PERM_AX, DM_FALSE);
  }
}

- (void)onToggleConnect:(id)oSender
{
  (void)oSender;
  af__post_menu_act(self.snap.bShowing ? AF_MENU_DISCONNECT : AF_MENU_CONNECT, DM_FALSE);
}

- (void)onToggleBootAuto:(id)oSender
{
  (void)oSender;
  af__post_menu_act(AF_MENU_TOGGLE_BOOT_AUTO, self.snap.bBootAuto ? DM_FALSE : DM_TRUE);
}

- (void)onToggleAntiUnplug:(id)oSender
{
  (void)oSender;
  af__post_menu_act(AF_MENU_TOGGLE_ANTI_UNPLUG, self.snap.bAntiUnplug ? DM_FALSE : DM_TRUE);
}

- (void)onPairNew:(id)oSender
{
  (void)oSender;
  af__post_menu_act(AF_MENU_PAIR_NEW, DM_FALSE);
}

- (void)onOpenSetup:(id)oSender
{
  (void)oSender;
  af__post_menu_act(AF_MENU_OPEN_SETUP, DM_FALSE);
}

- (void)onResetSetup:(id)oSender
{
  (void)oSender;
  af__post_menu_act(AF_MENU_RESET_SETUP, DM_FALSE);
}

- (void)onQuit:(id)oSender
{
  (void)oSender;
  [NSApp terminate:nil];
}

- (void)teardown
{
  if (nil != self.statusItem)
  {
    [[NSStatusBar systemStatusBar] removeStatusItem:self.statusItem];
    self.statusItem = nil;
  }
  self.menu = nil;
}

@end

static void af__run_main_sync_or_async(void (^oBlock)(void))
{
  if ([NSThread isMainThread])
  {
    oBlock();
  }
  else
  {
    dispatch_async(dispatch_get_main_queue(), oBlock);
  }
}

void af_shell_menu_init(void)
{
  @autoreleasepool
  {
    af__run_main_sync_or_async(^{
      if (nil == g_oMenuCtrl)
      {
        g_oMenuCtrl = [[AFMenuController alloc] init];
      }
    });
  }
}

void af_shell_menu_update(const af_menu_snap_st *pstSnap)
{
  af_menu_snap_st stCopy;

  if (NULL == pstSnap)
  {
    return;
  }
  stCopy = *pstSnap;
  @autoreleasepool
  {
    af__run_main_sync_or_async(^{
      if (nil == g_oMenuCtrl)
      {
        g_oMenuCtrl = [[AFMenuController alloc] init];
      }
      [g_oMenuCtrl applySnap:stCopy];
    });
  }
}

void af_shell_menu_refresh_lang(void)
{
  @autoreleasepool
  {
    af__run_main_sync_or_async(^{
      if (nil != g_oMenuCtrl)
      {
        [g_oMenuCtrl refreshLang];
      }
    });
  }
}

void af_shell_menu_destroy(void)
{
  @autoreleasepool
  {
    af__run_main_sync_or_async(^{
      if (nil != g_oMenuCtrl)
      {
        [g_oMenuCtrl teardown];
        g_oMenuCtrl = nil;
      }
    });
  }
}

void af_shell_menu_trigger_quit(void)
{
  @autoreleasepool
  {
    af__run_main_sync_or_async(^{
      /* Must run on CFRunLoop outside a main-queue GCD block so NSTerminateLater does not deadlock
       * the serial dispatch_get_main_queue() waiting for onDmExited:. */
      [NSApp performSelector:@selector(terminate:)
                  withObject:nil
                  afterDelay:0.0
                     inModes:@[ NSRunLoopCommonModes ]];
    });
  }
}
