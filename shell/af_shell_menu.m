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
#include "plat/af_objc_undef.h"

#import <Cocoa/Cocoa.h>

#include <string.h>

static void af__post_menu_act(af_menu_act_et eAct, bool_t bVal)
{
  af_ui_action_msg_st stMsg = {.eAct = eAct, .bVal = bVal};
  (void)dm_queue_post_async(AF_LINK_UI_ACTION, &stMsg, (uint32)sizeof(stMsg));
}

@interface AFMenuController : NSObject
@property (nonatomic, strong) NSStatusItem *statusItem;
@property (nonatomic, strong) NSMenu *menu;
@property (nonatomic, strong) NSMenuItem *statusLineItem;
@property (nonatomic, strong) NSMenuItem *connItem;
@property (nonatomic, strong) NSMenuItem *bootAutoItem;
@property (nonatomic, strong) NSMenuItem *antiUnplugItem;
@property (nonatomic, strong) NSMenuItem *setupItem;
@property (nonatomic, strong) NSMenuItem *versionItem;
@property (nonatomic, assign) af_menu_snap_st snap;
- (void)applySnap:(af_menu_snap_st)stSnap;
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

  self.statusLineItem = [self makeItem:@"未连接" action:NULL key:@""];
  self.statusLineItem.enabled = NO;
  [self.menu addItem:self.statusLineItem];
  [self.menu addItem:[NSMenuItem separatorItem]];

  self.connItem = [self makeItem:@"立即连接" action:@selector(onToggleConnect:) key:@""];
  [self.menu addItem:self.connItem];
  [self.menu addItem:[NSMenuItem separatorItem]];

  self.bootAutoItem = [self makeItem:@"开机自动连接" action:@selector(onToggleBootAuto:) key:@""];
  [self.menu addItem:self.bootAutoItem];

  self.antiUnplugItem =
      [self makeItem:@"防拔线（保留虚拟屏）" action:@selector(onToggleAntiUnplug:) key:@""];
  [self.menu addItem:self.antiUnplugItem];
  [self.menu addItem:[NSMenuItem separatorItem]];

  [self.menu addItem:[self makeItem:@"配对新 iPad…" action:@selector(onPairNew:) key:@""]];
  self.setupItem = [self makeItem:@"检查并配置…" action:@selector(onOpenSetup:) key:@""];
  [self.menu addItem:self.setupItem];
  [self.menu addItem:[self makeItem:@"重新设置…" action:@selector(onResetSetup:) key:@""]];
  [self.menu addItem:[NSMenuItem separatorItem]];

  NSString *oVer = [NSString stringWithFormat:@"Afar %s", AF_VERSION];
  self.versionItem = [self makeItem:oVer action:NULL key:@""];
  self.versionItem.enabled = NO;
  [self.menu addItem:self.versionItem];

  [self.menu addItem:[self makeItem:@"退出" action:@selector(onQuit:) key:@"q"]];
  self.statusItem.menu = self.menu;
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

  const char *pcLine = ('\0' != stSnap.acStatusLine[0]) ? stSnap.acStatusLine : "未连接";
  self.statusLineItem.title = [NSString stringWithUTF8String:pcLine] ?: @"未连接";
  self.connItem.title = stSnap.bShowing ? @"断开 iPad" : @"立即连接";
  self.bootAutoItem.state = stSnap.bBootAuto ? NSControlStateValueOn : NSControlStateValueOff;
  self.antiUnplugItem.state = stSnap.bAntiUnplug ? NSControlStateValueOn : NSControlStateValueOff;
  if (stSnap.u32TodoCount > 0u)
  {
    self.setupItem.title =
        [NSString stringWithFormat:@"检查并配置… (%u 项待办)", (unsigned)stSnap.u32TodoCount];
  }
  else
  {
    self.setupItem.title = @"检查并配置…";
  }
  const char *pcVer = ('\0' != stSnap.acVersion[0]) ? stSnap.acVersion : AF_VERSION;
  self.versionItem.title = [NSString stringWithFormat:@"Afar %s", pcVer];
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
