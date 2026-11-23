/**
 ******************************************************************************
 * @file    af_plat_os.m
 * @author  Afar_C
 * @version 1.0.0
 * @brief   macOS 基础系统能力实现（主线程投递、电源、屏保、Bonjour、OD、钥匙串、可取消子进程）。
 ******************************************************************************
 */

#include <dm.h>
#include "plat/af_plat.h"
#include "plat/af_objc_undef.h"

#import <Cocoa/Cocoa.h>
#import <CoreFoundation/CoreFoundation.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <OpenDirectory/OpenDirectory.h>
#import <Security/Security.h>
#import <UserNotifications/UserNotifications.h>
#import <WebKit/WebKit.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

_Atomic bool_t g_bAfQuitting = DM_FALSE;
_Atomic bool_t g_bAfQuitRequested = DM_FALSE;

static IOPMAssertionID g_u32PwrAssertId = kIOPMNullAssertionID;
static NSNetService *g_oBonjourSvc = nil;

void af_plat_run_on_main(af_main_fn pfFn, void *pvCtx)
{
  if (NULL == pfFn)
  {
    return;
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    pfFn(pvCtx);
  });
}

int af_plat_open_url(const char *pcUrl)
{
  if ((NULL == pcUrl) || ('\0' == pcUrl[0]))
  {
    return DM_INVALID;
  }
  @autoreleasepool
  {
    NSString *oStr = [NSString stringWithUTF8String:pcUrl];
    NSURL *oUrl = (nil != oStr) ? [NSURL URLWithString:oStr] : nil;
    if (nil == oUrl)
    {
      return DM_INVALID;
    }
    return [[NSWorkspace sharedWorkspace] openURL:oUrl] ? DM_SUCCESS : DM_FAILD;
  }
}

void af_plat_notify(const char *pcTitle, const char *pcBody, const char *pcTag)
{
  @autoreleasepool
  {
    if ((nil == [[NSBundle mainBundle] bundleIdentifier]) || (NULL == pcTitle))
    {
      return;
    }
    UNMutableNotificationContent *oContent = [[UNMutableNotificationContent alloc] init];
    oContent.title = [NSString stringWithUTF8String:pcTitle] ?: @"Afar";
    oContent.body = (NULL != pcBody) ? ([NSString stringWithUTF8String:pcBody] ?: @"") : @"";
    NSString *oId = (NULL != pcTag) ? ([NSString stringWithUTF8String:pcTag] ?: @"afar") : @"afar";
    UNNotificationRequest *oReq = [UNNotificationRequest requestWithIdentifier:oId
                                                                       content:oContent
                                                                       trigger:nil];
    [[UNUserNotificationCenter currentNotificationCenter] addNotificationRequest:oReq
                                                           withCompletionHandler:nil];
  }
}

int af_plat_power_hold(bool_t bHold)
{
  if (bHold)
  {
    if (kIOPMNullAssertionID != g_u32PwrAssertId)
    {
      return DM_SUCCESS;
    }
    IOReturn iRet = IOPMAssertionCreateWithName(kIOPMAssertionTypeNoDisplaySleep,
                                                kIOPMAssertionLevelOn,
                                                CFSTR("Afar keeping display active"),
                                                &g_u32PwrAssertId);
    return (kIOReturnSuccess == iRet) ? DM_SUCCESS : DM_FAILD;
  }
  if (kIOPMNullAssertionID != g_u32PwrAssertId)
  {
    (void)IOPMAssertionRelease(g_u32PwrAssertId);
    g_u32PwrAssertId = kIOPMNullAssertionID;
  }
  return DM_SUCCESS;
}

int af_plat_screensaver_get(int *piSec, bool_t *pbExists)
{
  CFPropertyListRef pVal = NULL;
  int iVal = 0;

  if ((NULL == piSec) || (NULL == pbExists))
  {
    return DM_INVALID;
  }
  *piSec = 0;
  *pbExists = DM_FALSE;
  pVal = CFPreferencesCopyValue(CFSTR("idleTime"), CFSTR("com.apple.screensaver"),
                                kCFPreferencesCurrentUser, kCFPreferencesCurrentHost);
  if (NULL == pVal)
  {
    return DM_SUCCESS;
  }
  if ((CFGetTypeID(pVal) == CFNumberGetTypeID())
      && CFNumberGetValue((CFNumberRef)pVal, kCFNumberIntType, &iVal))
  {
    *piSec = iVal;
    *pbExists = DM_TRUE;
  }
  CFRelease(pVal);
  return DM_SUCCESS;
}

int af_plat_screensaver_set(int iSec)
{
  CFNumberRef pNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &iSec);
  if (NULL == pNum)
  {
    return DM_ALLOC;
  }
  CFPreferencesSetValue(CFSTR("idleTime"), pNum, CFSTR("com.apple.screensaver"),
                        kCFPreferencesCurrentUser, kCFPreferencesCurrentHost);
  CFRelease(pNum);
  (void)CFPreferencesSynchronize(CFSTR("com.apple.screensaver"), kCFPreferencesCurrentUser,
                                 kCFPreferencesCurrentHost);
  return DM_SUCCESS;
}

int af_plat_screensaver_delete(void)
{
  CFPreferencesSetValue(CFSTR("idleTime"), NULL, CFSTR("com.apple.screensaver"),
                        kCFPreferencesCurrentUser, kCFPreferencesCurrentHost);
  (void)CFPreferencesSynchronize(CFSTR("com.apple.screensaver"), kCFPreferencesCurrentUser,
                                 kCFPreferencesCurrentHost);
  return DM_SUCCESS;
}

static void af__trunc_utf8_bytes(const char *pcSrc, char *pcDst, size_t uMaxBytes)
{
  size_t uLen = (NULL != pcSrc) ? strlen(pcSrc) : 0u;
  if (uLen > uMaxBytes)
  {
    uLen = uMaxBytes;
    while ((uLen > 0u) && (0x80u == (((unsigned char)pcSrc[uLen]) & 0xc0u)))
    {
      uLen--;
    }
  }
  if (uLen > 0u)
  {
    memcpy(pcDst, pcSrc, uLen);
  }
  pcDst[uLen] = '\0';
}

int af_plat_bonjour_start(const char *pcName, uint16 u16Port)
{
  char acSafe[64];
  af__trunc_utf8_bytes((NULL != pcName) ? pcName : "Afar", acSafe, 63u);
  @autoreleasepool
  {
    NSString *oName = [[NSString stringWithUTF8String:acSafe] copy] ?: @"Afar";
    dispatch_async(dispatch_get_main_queue(), ^{
      if (nil != g_oBonjourSvc)
      {
        [g_oBonjourSvc stop];
        [g_oBonjourSvc removeFromRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
        g_oBonjourSvc = nil;
      }
      g_oBonjourSvc = [[NSNetService alloc] initWithDomain:@"local."
                                                      type:@"_afar._tcp."
                                                      name:oName
                                                      port:(int)u16Port];
      if (nil != g_oBonjourSvc)
      {
        [g_oBonjourSvc scheduleInRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
        [g_oBonjourSvc publish];
      }
    });
  }
  return DM_SUCCESS;
}

void af_plat_bonjour_stop(void)
{
  dispatch_async(dispatch_get_main_queue(), ^{
    if (nil != g_oBonjourSvc)
    {
      [g_oBonjourSvc stop];
      [g_oBonjourSvc removeFromRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
      g_oBonjourSvc = nil;
    }
  });
}

int af_plat_od_verify(const char *pcUser, const char *pcPass)
{
  if ((NULL == pcUser) || (NULL == pcPass) || ('\0' == pcUser[0]))
  {
    return -2;
  }
  @autoreleasepool
  {
    NSError *oErr = nil;
    ODNode *oNode = [ODNode nodeWithSession:[ODSession defaultSession]
                                       type:kODNodeTypeLocalNodes
                                      error:&oErr];
    if (nil == oNode)
    {
      return -2;
    }
    NSString *oUser = [NSString stringWithUTF8String:pcUser];
    NSString *oPass = [NSString stringWithUTF8String:pcPass];
    if ((nil == oUser) || (nil == oPass))
    {
      return -2;
    }
    ODRecord *oRec = [oNode recordWithRecordType:kODRecordTypeUsers
                                            name:oUser
                                      attributes:nil
                                           error:&oErr];
    if (nil == oRec)
    {
      return -2;
    }
    oErr = nil;
    BOOL bOk = [oRec verifyPassword:oPass error:&oErr];
    if (bOk)
    {
      return 0;
    }
    return ((nil == oErr) || (kODErrorCredentialsInvalid == oErr.code)) ? -1 : -2;
  }
}

bool_t af_plat_keychain_has_legacy_pw(void)
{
  @autoreleasepool
  {
    NSDictionary *oQuery = @{
      (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
      (__bridge id)kSecAttrService : @"com.afar.agent.login",
      (__bridge id)kSecMatchLimit : (__bridge id)kSecMatchLimitOne,
      (__bridge id)kSecReturnAttributes : @YES
    };
    CFTypeRef pOut = NULL;
    OSStatus iSt = SecItemCopyMatching((__bridge CFDictionaryRef)oQuery, &pOut);
    if (NULL != pOut)
    {
      CFRelease(pOut);
    }
    return (errSecSuccess == iSt) ? DM_TRUE : DM_FALSE;
  }
}

void af_plat_keychain_del_legacy_pw(void)
{
  @autoreleasepool
  {
    NSDictionary *oQuery = @{
      (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
      (__bridge id)kSecAttrService : @"com.afar.agent.login"
    };
    (void)SecItemDelete((__bridge CFDictionaryRef)oQuery);
  }
}

void af_plat_current_user(char *pcBuf, size_t uCap)
{
  if ((NULL == pcBuf) || (0u == uCap))
  {
    return;
  }
  @autoreleasepool
  {
    const char *pcU = [NSUserName() UTF8String];
    snprintf(pcBuf, uCap, "%s", (NULL != pcU) ? pcU : "");
  }
}

void af_plat_host_name(char *pcBuf, size_t uCap)
{
  if ((NULL == pcBuf) || (0u == uCap))
  {
    return;
  }
  if (0 != gethostname(pcBuf, uCap))
  {
    snprintf(pcBuf, uCap, "Mac");
  }
  pcBuf[uCap - 1u] = '\0';
}

static void af__set_cloexec(int iFd)
{
  int iFl = fcntl(iFd, F_GETFD);
  if (iFl >= 0)
  {
    (void)fcntl(iFd, F_SETFD, iFl | FD_CLOEXEC);
  }
}

static void af__write_all_nosigpipe(int iFd, const char *pcData, size_t uLen)
{
  size_t uOff = 0u;
  (void)fcntl(iFd, F_SETNOSIGPIPE, 1);
  while (uOff < uLen)
  {
    ssize_t iW = write(iFd, pcData + uOff, uLen - uOff);
    if (iW < 0)
    {
      if (EINTR == errno) { continue; }
      break;
    }
    if (0 == iW) { break; }
    uOff += (size_t)iW;
  }
}

static void af__waitpid_reap(pid_t tPid, int *piStatus)
{
  while ((-1 == waitpid(tPid, piStatus, 0)) && (EINTR == errno))
  {
  }
}

static bool_t af__drain_fd(int iFd, char *pcOut, size_t uOutCap, size_t *puUsed)
{
  char acBuf[512];
  for (;;)
  {
    ssize_t iR = read(iFd, acBuf, sizeof(acBuf));
    if (iR < 0)
    {
      if (EINTR == errno) { continue; }
      return ((EAGAIN == errno) || (EWOULDBLOCK == errno)) ? DM_FALSE : DM_TRUE;
    }
    if (0 == iR)
    {
      return DM_TRUE;
    }
    if ((NULL != pcOut) && (uOutCap > 1u) && (*puUsed < (uOutCap - 1u)))
    {
      size_t uCopy = (size_t)iR;
      if (uCopy > (uOutCap - 1u - *puUsed))
      {
        uCopy = uOutCap - 1u - *puUsed;
      }
      memcpy(pcOut + *puUsed, acBuf, uCopy);
      *puUsed += uCopy;
      pcOut[*puUsed] = '\0';
    }
  }
}

static int af__spawn_cmd(const char *const *ppcArgv, const char *pcStdin, int *piOutRd, pid_t *ptPid)
{
  int aiOut[2] = {-1, -1};
  int aiIn[2] = {-1, -1};
  posix_spawn_file_actions_t stAct;
  posix_spawnattr_t stAttr;
  int iRet = DM_SUCCESS;

  if (0 != pipe(aiOut))
  {
    return DM_IO;
  }
  af__set_cloexec(aiOut[0]);
  af__set_cloexec(aiOut[1]);
  if (NULL != pcStdin)
  {
    if (0 != pipe(aiIn))
    {
      close(aiOut[0]);
      close(aiOut[1]);
      return DM_IO;
    }
    af__set_cloexec(aiIn[0]);
    af__set_cloexec(aiIn[1]);
  }
  (void)posix_spawn_file_actions_init(&stAct);
  (void)posix_spawn_file_actions_adddup2(&stAct, aiOut[1], STDOUT_FILENO);
  (void)posix_spawn_file_actions_adddup2(&stAct, aiOut[1], STDERR_FILENO);
  if (NULL != pcStdin)
  {
    (void)posix_spawn_file_actions_adddup2(&stAct, aiIn[0], STDIN_FILENO);
  }
  else
  {
    (void)posix_spawn_file_actions_addopen(&stAct, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  }
  (void)posix_spawnattr_init(&stAttr);
  (void)posix_spawnattr_setflags(&stAttr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT);
  (void)posix_spawnattr_setpgroup(&stAttr, 0);
  if (0 != posix_spawnp(ptPid, ppcArgv[0], &stAct, &stAttr, (char *const *)ppcArgv, environ))
  {
    iRet = DM_IO;
  }
  (void)posix_spawn_file_actions_destroy(&stAct);
  (void)posix_spawnattr_destroy(&stAttr);
  close(aiOut[1]);
  if (NULL != pcStdin)
  {
    close(aiIn[0]);
    if (DM_SUCCESS == iRet)
    {
      size_t uLen = strlen(pcStdin);
      if (uLen > 0u) { af__write_all_nosigpipe(aiIn[1], pcStdin, uLen); }
    }
    close(aiIn[1]);
  }
  if (DM_SUCCESS != iRet)
  {
    close(aiOut[0]);
    return iRet;
  }
  (void)fcntl(aiOut[0], F_SETFL, O_NONBLOCK);
  *piOutRd = aiOut[0];
  return DM_SUCCESS;
}

int af_plat_run_cmd_cancellable(const char *const *ppcArgv, const char *pcStdin,
                                const _Atomic bool_t *pbCancel, char *pcOut, size_t uOutCap,
                                uint32 u32TimeoutMs, int *piExitCode)
{
  int iOutRd = -1;
  pid_t tPid = -1;
  uint64 u64Deadline = 0u;
  size_t uUsed = 0u;
  int iStatus = 0;
  int iRet = DM_SUCCESS;

  if ((NULL == ppcArgv) || (NULL == ppcArgv[0])) { return DM_INVALID; }
  if ((NULL != pcOut) && (uOutCap > 0u)) { pcOut[0] = '\0'; }
  if (NULL != piExitCode) { *piExitCode = -1; }
  iRet = af__spawn_cmd(ppcArgv, pcStdin, &iOutRd, &tPid);
  if (DM_SUCCESS != iRet) { return iRet; }
  u64Deadline = dm_hrtime() + ((uint64)((0u != u32TimeoutMs) ? u32TimeoutMs : 30000u) * 1000000ULL);
  for (;;)
  {
    pid_t tW = waitpid(tPid, &iStatus, WNOHANG);
    if ((iOutRd >= 0) && af__drain_fd(iOutRd, pcOut, uOutCap, &uUsed))
    {
      close(iOutRd);
      iOutRd = -1;
    }
    if (tW == tPid)
    {
      if (NULL != piExitCode) { *piExitCode = WIFEXITED(iStatus) ? WEXITSTATUS(iStatus) : -1; }
      break;
    }
    if ((NULL != pbCancel) && atomic_load(pbCancel))
    {
      (void)kill(-tPid, SIGKILL);
      af__waitpid_reap(tPid, &iStatus);
      iRet = DM_SHUTTING_DOWN;
      break;
    }
    if (dm_hrtime() >= u64Deadline)
    {
      (void)kill(-tPid, SIGKILL);
      af__waitpid_reap(tPid, &iStatus);
      iRet = DM_TIMEOUT;
      break;
    }
    if (iOutRd >= 0)
    {
      struct pollfd stPfd = {.fd = iOutRd, .events = POLLIN};
      if ((poll(&stPfd, 1u, 50) > 0) && (0 != (stPfd.revents & (POLLHUP | POLLERR | POLLNVAL))))
      {
        (void)af__drain_fd(iOutRd, pcOut, uOutCap, &uUsed);
        close(iOutRd);
        iOutRd = -1;
      }
    }
    else
    {
      dm_sleep(20u);
    }
  }
  if (iOutRd >= 0)
  {
    (void)af__drain_fd(iOutRd, pcOut, uOutCap, &uUsed);
    close(iOutRd);
  }
  return iRet;
}

void af_plat_webview_force_close(void *pvWebView)
{
  if (NULL == pvWebView)
  {
    return;
  }
  @autoreleasepool
  {
    WKWebView *oWv = (__bridge WKWebView *)pvWebView;
    [oWv stopLoading];
    [oWv loadHTMLString:@"" baseURL:nil];
  }
}
