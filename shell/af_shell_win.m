/**
 ******************************************************************************
 * @file    af_shell_win.m
 * @author  Afar_C
 * @version 1.0.0
 * @brief   macOS 原生 NSWindow + WKWebView 设置向导壳实现（仅通过 dm_queue_post_async 通信）。
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
#import <WebKit/WebKit.h>

#include <string.h>

@interface AFWeakScriptHandler : NSObject <WKScriptMessageHandler>
@property (nonatomic, weak) id<WKScriptMessageHandler> target;
@end

@implementation AFWeakScriptHandler
- (void)userContentController:(WKUserContentController *)oUcc
      didReceiveScriptMessage:(WKScriptMessage *)oMsg
{
  [self.target userContentController:oUcc didReceiveScriptMessage:oMsg];
}
@end

@interface AFSetupWinController : NSObject <NSWindowDelegate, WKNavigationDelegate,
                                            WKScriptMessageHandler>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) WKWebView *webView;
@property (nonatomic, copy) NSString *lastJson;
@property (nonatomic, assign) BOOL loaded;
@property (nonatomic, assign) BOOL visible;
@property (nonatomic, assign) uint64 hideGen;
- (void)showWindow;
- (void)hideWindow;
- (void)pushJson:(NSString *)oJson;
- (void)refreshLang;
- (void)teardown;
@end

static AFSetupWinController *g_oWinCtrl = nil;
static char g_acHtmlPathOverride[512];

static NSString *af__find_setup_html(void)
{
  NSFileManager *oFm = [NSFileManager defaultManager];
  if ('\0' != g_acHtmlPathOverride[0])
  {
    NSString *oCustom = [NSString stringWithUTF8String:g_acHtmlPathOverride];
    if ((nil != oCustom) && [oFm isReadableFileAtPath:oCustom])
    {
      return oCustom;
    }
  }
  NSString *oRes = [[NSBundle mainBundle] pathForResource:@"setup" ofType:@"html"];
  if ((nil != oRes) && [oFm isReadableFileAtPath:oRes])
  {
    return oRes;
  }
  NSString *oCwd = [[oFm currentDirectoryPath] stringByAppendingPathComponent:@"shell/ui/setup.html"];
  if ([oFm isReadableFileAtPath:oCwd])
  {
    return oCwd;
  }
  return nil;
}

@implementation AFSetupWinController

- (void)ensureCreated
{
  if (nil != self.window)
  {
    return;
  }
  NSRect stRect = NSMakeRect(0.0, 0.0, 520.0, 720.0);
  NSWindowStyleMask uStyle = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                             | NSWindowStyleMaskMiniaturizable;
  self.window = [[NSWindow alloc] initWithContentRect:stRect
                                            styleMask:uStyle
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
  self.window.title = (DM_TRUE == af_shell_lang_is_zh()) ? @"设置 Afar" : @"Set Up Afar";
  self.window.releasedWhenClosed = NO;
  self.window.delegate = self;
  [self.window center];

  WKWebViewConfiguration *oCfg = [[WKWebViewConfiguration alloc] init];
  WKUserContentController *oUcc = [[WKUserContentController alloc] init];
  AFWeakScriptHandler *oProxy = [[AFWeakScriptHandler alloc] init];
  oProxy.target = self;
  [oUcc addScriptMessageHandler:oProxy name:@"afar"];
  oCfg.userContentController = oUcc;

  self.webView = [[WKWebView alloc] initWithFrame:stRect configuration:oCfg];
  self.webView.navigationDelegate = self;
  self.webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  self.window.contentView = self.webView;
  self.loaded = NO;

  NSString *oPath = af__find_setup_html();
  NSString *oHtml = nil;
  if (nil != oPath)
  {
    oHtml = [NSString stringWithContentsOfFile:oPath encoding:NSUTF8StringEncoding error:nil];
  }
  if (nil != oHtml)
  {
    NSURL *oBase = [NSURL fileURLWithPath:[oPath stringByDeletingLastPathComponent]];
    [self.webView loadHTMLString:oHtml baseURL:oBase];
  }
  else
  {
    [self.webView loadHTMLString:@"<!doctype html><html><body></body></html>" baseURL:nil];
  }
}

- (void)releaseWebView
{
  if (nil != self.webView)
  {
    [self.webView.configuration.userContentController removeScriptMessageHandlerForName:@"afar"];
    self.webView.navigationDelegate = nil;
    af_plat_webview_force_close((__bridge void *)self.webView);
    [self.webView removeFromSuperview];
    self.webView = nil;
  }
  self.loaded = NO;
  if (nil != self.window)
  {
    self.window.delegate = nil;
    [self.window setContentView:nil];
    [self.window orderOut:nil];
    [self.window close];
    self.window = nil;
  }
}

- (void)showWindow
{
  self.hideGen++;
  [self ensureCreated];
  if (NSApplicationActivationPolicyProhibited != [NSApp activationPolicy])
  {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  }
  [self.window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  self.visible = YES;
  if (self.loaded && (nil != self.lastJson))
  {
    [self evalPush:self.lastJson];
  }
}

- (void)scheduleHiddenRelease
{
  if (NSApplicationActivationPolicyProhibited != [NSApp activationPolicy])
  {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
  }
  uint64 u64Gen = ++self.hideGen;
  __weak AFSetupWinController *oWeak = self;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(30LL * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   AFSetupWinController *oStrong = oWeak;
                   if ((nil != oStrong) && (!oStrong.visible) && (oStrong.hideGen == u64Gen))
                   {
                     [oStrong releaseWebView];
                   }
                 });
}

- (void)hideWindow
{
  self.visible = NO;
  if (nil != self.window)
  {
    [self.window orderOut:nil];
  }
  [self scheduleHiddenRelease];
}

- (void)evalPush:(NSString *)oJson
{
  if ((nil == self.webView) || (!self.loaded) || (nil == oJson))
  {
    return;
  }
  NSString *oSafe = [oJson stringByReplacingOccurrencesOfString:@"\u2028" withString:@"\\u2028"];
  oSafe = [oSafe stringByReplacingOccurrencesOfString:@"\u2029" withString:@"\\u2029"];
  NSString *oJs = [NSString stringWithFormat:@"window.afar && window.afar.update(%@);", oSafe];
  [self.webView evaluateJavaScript:oJs completionHandler:nil];
}

- (void)pushJson:(NSString *)oJson
{
  self.lastJson = oJson;
  [self evalPush:oJson];
}

- (BOOL)windowShouldClose:(NSWindow *)oSender
{
  (void)oSender;
  return YES;
}

- (void)windowWillClose:(NSNotification *)oNote
{
  static const char acClose[] = "{\"cmd\":\"close\"}";
  (void)oNote;
  if (!self.visible)
  {
    return;
  }
  self.visible = NO;
  [self scheduleHiddenRelease];
  (void)dm_queue_post_async(AF_LINK_UI_SETUP_MSG, acClose, (uint32)(sizeof(acClose) - 1u));
}

- (void)evalLang
{
  if ((nil == self.webView) || (!self.loaded))
  {
    return;
  }
  const char *pcLang = (DM_TRUE == af_shell_lang_is_zh()) ? "zh" : "en";
  NSString *oJs =
      [NSString stringWithFormat:@"window.afar && window.afar.setLang && window.afar.setLang('%s');",
                                 pcLang];
  [self.webView evaluateJavaScript:oJs completionHandler:nil];
}

- (void)refreshLang
{
  if (nil != self.window)
  {
    self.window.title = (DM_TRUE == af_shell_lang_is_zh()) ? @"设置 Afar" : @"Set Up Afar";
  }
  [self evalLang];
}

- (void)webView:(WKWebView *)oWv didFinishNavigation:(WKNavigation *)oNav
{
  (void)oWv;
  (void)oNav;
  self.loaded = YES;
  [self evalLang];
  if (nil != self.lastJson)
  {
    [self evalPush:self.lastJson];
  }
}

- (BOOL)handleLangMessage:(NSString *)oBody
{
  NSData *oData = [oBody dataUsingEncoding:NSUTF8StringEncoding];
  if (nil == oData)
  {
    return NO;
  }
  id oObj = [NSJSONSerialization JSONObjectWithData:oData options:0 error:nil];
  if (![oObj isKindOfClass:[NSDictionary class]])
  {
    return NO;
  }
  NSDictionary *oDict = (NSDictionary *)oObj;
  if (![oDict[@"cmd"] isEqualToString:@"lang"])
  {
    return NO;
  }
  NSString *oLang = [oDict[@"lang"] isKindOfClass:[NSString class]] ? oDict[@"lang"] : @"";
  af_shell_lang_set([oLang UTF8String]);
  if (nil != self.window)
  {
    self.window.title = (DM_TRUE == af_shell_lang_is_zh()) ? @"设置 Afar" : @"Set Up Afar";
  }
  af_shell_menu_refresh_lang();
  return YES;
}

- (void)userContentController:(WKUserContentController *)oUcc
      didReceiveScriptMessage:(WKScriptMessage *)oMsg
{
  (void)oUcc;
  if (![oMsg.body isKindOfClass:[NSString class]])
  {
    return;
  }
  NSString *oBody = (NSString *)oMsg.body;
  if ([self handleLangMessage:oBody])
  {
    return;
  }
  const char *pcUtf8 = [oBody UTF8String];
  if (NULL != pcUtf8)
  {
    (void)dm_queue_post_async(AF_LINK_UI_SETUP_MSG, pcUtf8, (uint32)strlen(pcUtf8));
  }
}

- (void)teardown
{
  self.hideGen++;
  self.visible = NO;
  [self releaseWebView];
  self.lastJson = nil;
}

@end

static void af__win_on_main(void (^oBlock)(void))
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

void af_shell_win_set_html_path(const char *pcPath)
{
  if (NULL == pcPath)
  {
    g_acHtmlPathOverride[0] = '\0';
  }
  else
  {
    snprintf(g_acHtmlPathOverride, sizeof(g_acHtmlPathOverride), "%s", pcPath);
  }
}

void af_shell_win_show(bool_t bShow)
{
  @autoreleasepool
  {
    af__win_on_main(^{
      if (bShow)
      {
        if (nil == g_oWinCtrl)
        {
          g_oWinCtrl = [[AFSetupWinController alloc] init];
        }
        [g_oWinCtrl showWindow];
      }
      else if (nil != g_oWinCtrl)
      {
        [g_oWinCtrl hideWindow];
      }
    });
  }
}

void af_shell_win_push(const char *pcViewJson)
{
  if (NULL == pcViewJson)
  {
    return;
  }
  @autoreleasepool
  {
    NSString *oCopy = [[NSString alloc] initWithUTF8String:pcViewJson];
    if (nil == oCopy)
    {
      return;
    }
    af__win_on_main(^{
      if (nil == g_oWinCtrl)
      {
        g_oWinCtrl = [[AFSetupWinController alloc] init];
      }
      [g_oWinCtrl pushJson:oCopy];
    });
  }
}

void af_shell_win_destroy(void)
{
  @autoreleasepool
  {
    af__win_on_main(^{
      if (nil != g_oWinCtrl)
      {
        [g_oWinCtrl teardown];
        g_oWinCtrl = nil;
      }
    });
  }
}

void af_shell_win_refresh_lang(void)
{
  @autoreleasepool
  {
    af__win_on_main(^{
      if (nil != g_oWinCtrl)
      {
        [g_oWinCtrl refreshLang];
      }
    });
  }
}

bool_t af_shell_win_is_visible(void)
{
  @autoreleasepool
  {
    __block bool_t bVis = DM_FALSE;
    if ([NSThread isMainThread])
    {
      bVis = ((nil != g_oWinCtrl) && g_oWinCtrl.visible) ? DM_TRUE : DM_FALSE;
    }
    else
    {
      dispatch_sync(dispatch_get_main_queue(), ^{
        bVis = ((nil != g_oWinCtrl) && g_oWinCtrl.visible) ? DM_TRUE : DM_FALSE;
      });
    }
    return bVis;
  }
}
