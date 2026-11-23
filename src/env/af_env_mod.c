/**
 ******************************************************************************
 * @file    af_env_mod.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   af_env DM 模块（ModID 21）：6 项只读环境探测、防重入合并、可取消子进程与 60s 巡检。
 ******************************************************************************
 */

#define _DARWIN_C_SOURCE
#define DM_LOG_MOD "env"

#include "af_env.h"
#include "../af_modules.h"
#include "../plat/af_plat.h"

#include <dm/event.h>
#include <dm/link.h>
#include <dm/log.h>
#include <dm/pool.h>
#include <dm/timer.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define AF_ENV_POLL_MS       60000u
#define AF_ENV_CMD_TIMEOUT   5000u
#define AF_ENV_TCP_TIMEOUT   2000u
#define AF_ENV_OUT_CAP       8192u

typedef struct af__env_job
{
  af_env_hooks_st stHooks;
  char acSshDir[512];
  char acPlistPath[512];
  char acExePath[512];
  af_env_snap_st stSnap;
  bool_t bCancelled;
} af__env_job_st;

static af_env_snap_st g_stEnvSnap;
static af_env_hooks_st g_stEnvHooks;
static bool_t g_bEnvProbing = DM_FALSE;
static bool_t g_bEnvNeedReprobe = DM_FALSE;
static bool_t g_bEnvDraining = DM_FALSE;
static uint32 g_u32EnvTimerId = 0u;
static int g_iEnvDrainHold = -1;

/* ============================ 纯解析函数 ============================ */

static const char *af__strcasestr(in const char *pcHay, in const char *pcNeedle)
{
  size_t uNLen = 0u;

  if ((NULL == pcHay) || (NULL == pcNeedle))
  {
    return NULL;
  }
  uNLen = strlen(pcNeedle);
  if (0u == uNLen)
  {
    return pcHay;
  }
  while ('\0' != *pcHay)
  {
    size_t i = 0u;
    while ((i < uNLen)
           && (tolower((unsigned char)pcHay[i]) == tolower((unsigned char)pcNeedle[i])))
    {
      i++;
    }
    if (i == uNLen)
    {
      return pcHay;
    }
    pcHay++;
  }
  return NULL;
}

bool_t af_env_parse_agent_plist(in const char *pcXml, in const char *pcExePath)
{
  const char *pcKey = NULL;
  const char *pcArr = NULL;
  const char *pcS = NULL;
  const char *pcE = NULL;
  char acFound[PATH_MAX];
  char acRealA[PATH_MAX];
  char acRealB[PATH_MAX];
  size_t uLen = 0u;

  if ((NULL == pcXml) || (NULL == pcExePath) || ('\0' == pcExePath[0]))
  {
    return DM_FALSE;
  }
  pcKey = strstr(pcXml, "<key>ProgramArguments</key>");
  pcArr = (NULL != pcKey) ? strstr(pcKey, "<array>") : NULL;
  pcS = (NULL != pcArr) ? strstr(pcArr, "<string>") : NULL;
  if (NULL == pcS)
  {
    return DM_FALSE;
  }
  pcS += 8;
  pcE = strstr(pcS, "</string>");
  if (NULL == pcE)
  {
    return DM_FALSE;
  }
  while ((pcS < pcE) && (0 != isspace((unsigned char)*pcS))) { pcS++; }
  while ((pcE > pcS) && (0 != isspace((unsigned char)pcE[-1]))) { pcE--; }
  uLen = (size_t)(pcE - pcS);
  if ((0u == uLen) || (uLen >= sizeof(acFound)))
  {
    return DM_FALSE;
  }
  memcpy(acFound, pcS, uLen);
  acFound[uLen] = '\0';
  if (0 == strcmp(acFound, pcExePath))
  {
    return DM_TRUE;
  }
  if ((NULL != realpath(acFound, acRealA)) && (NULL != realpath(pcExePath, acRealB)))
  {
    return (0 == strcmp(acRealA, acRealB)) ? DM_TRUE : DM_FALSE;
  }
  return DM_FALSE;
}

bool_t af_env_parse_launchctl_ok(in int iExitCode, in const char *pcOut)
{
  if (0 != iExitCode)
  {
    return DM_FALSE;
  }
  if ((NULL != pcOut) && (NULL != af__strcasestr(pcOut, "could not find service")))
  {
    return DM_FALSE;
  }
  return DM_TRUE;
}

bool_t af_env_parse_fw_global_off(in int iExitCode, in const char *pcOut)
{
  if ((0 != iExitCode) || (NULL == pcOut))
  {
    return DM_FALSE;
  }
  if ((NULL != af__strcasestr(pcOut, "state = 0")) || (NULL != af__strcasestr(pcOut, "disabled")))
  {
    return DM_TRUE;
  }
  return DM_FALSE;
}

bool_t af_env_parse_fw_app_permitted(in int iExitCode, in const char *pcOut)
{
  if ((0 != iExitCode) || (NULL == pcOut))
  {
    return DM_FALSE;
  }
  return ((NULL != af__strcasestr(pcOut, "permitted"))
          && (NULL == af__strcasestr(pcOut, "blocked")))
             ? DM_TRUE
             : DM_FALSE;
}

bool_t af_env_parse_pmset_autorestart(in const char *pcOut)
{
  const char *pcLine = pcOut;

  if (NULL == pcOut)
  {
    return DM_FALSE;
  }
  while ('\0' != *pcLine)
  {
    const char *pcP = pcLine;
    const char *pcNext = strchr(pcLine, '\n');
    while ((' ' == *pcP) || ('\t' == *pcP)) { pcP++; }
    if ((0 == strncmp(pcP, "autorestart", 11u)) && ((' ' == pcP[11]) || ('\t' == pcP[11])))
    {
      pcP += 11;
      while ((' ' == *pcP) || ('\t' == *pcP)) { pcP++; }
      if (('1' == pcP[0])
          && (('\0' == pcP[1]) || ('\r' == pcP[1]) || ('\n' == pcP[1]) || (' ' == pcP[1])
              || ('\t' == pcP[1])))
      {
        return DM_TRUE;
      }
    }
    if (NULL == pcNext) { break; }
    pcLine = pcNext + 1;
  }
  return DM_FALSE;
}

bool_t af_env_parse_filevault(in int iExitCode, in const char *pcOut)
{
  if ((0 != iExitCode) || (NULL == pcOut))
  {
    return DM_FALSE;
  }
  return ((NULL != af__strcasestr(pcOut, "true")) && (NULL == af__strcasestr(pcOut, "false")))
             ? DM_TRUE
             : DM_FALSE;
}

bool_t af_env_parse_autologin(in int iExitCode, in const char *pcOut)
{
  const char *pcP = pcOut;

  if ((0 != iExitCode) || (NULL == pcOut))
  {
    return DM_FALSE;
  }
  while (('\0' != *pcP) && (0 != isspace((unsigned char)*pcP)))
  {
    pcP++;
  }
  return ('\0' != *pcP) ? DM_TRUE : DM_FALSE;
}

uint32 af_env_parse_ssh_pub(in const char *pcText, out char aacOut[][AF_SSH_KEY_LEN],
                            io uint32 *pu32Cnt, in uint32 u32Cap)
{
  const char *pcLine = pcText;
  uint32 u32Added = 0u;

  if ((NULL == pcText) || (NULL == aacOut) || (NULL == pu32Cnt))
  {
    return 0u;
  }
  while (('\0' != *pcLine) && (*pu32Cnt < u32Cap))
  {
    const char *pcNext = strchr(pcLine, '\n');
    const char *pcEnd = (NULL != pcNext) ? pcNext : (pcLine + strlen(pcLine));
    const char *pcT1 = pcLine;
    const char *pcT1End = NULL;
    const char *pcT2 = NULL;
    const char *pcT2End = NULL;

    while ((pcT1 < pcEnd) && ((' ' == *pcT1) || ('\t' == *pcT1) || ('\r' == *pcT1))) { pcT1++; }
    if ((pcT1 < pcEnd) && ('#' != *pcT1))
    {
      pcT1End = pcT1;
      while ((pcT1End < pcEnd) && (0 == isspace((unsigned char)*pcT1End))) { pcT1End++; }
      pcT2 = pcT1End;
      while ((pcT2 < pcEnd) && ((' ' == *pcT2) || ('\t' == *pcT2))) { pcT2++; }
      pcT2End = pcT2;
      while ((pcT2End < pcEnd) && (0 == isspace((unsigned char)*pcT2End))) { pcT2End++; }
      if ((pcT1End > pcT1) && (pcT2End > pcT2))
      {
        size_t uL1 = (size_t)(pcT1End - pcT1);
        size_t uL2 = (size_t)(pcT2End - pcT2);
        if ((uL1 + 1u + uL2) < AF_SSH_KEY_LEN)
        {
          memcpy(aacOut[*pu32Cnt], pcT1, uL1);
          aacOut[*pu32Cnt][uL1] = ' ';
          memcpy(aacOut[*pu32Cnt] + uL1 + 1u, pcT2, uL2);
          aacOut[*pu32Cnt][uL1 + 1u + uL2] = '\0';
          (*pu32Cnt)++;
          u32Added++;
        }
      }
    }
    if (NULL == pcNext) { break; }
    pcLine = pcNext + 1;
  }
  return u32Added;
}

/* ============================ 底层命令与探测执行 ============================ */

static int af__default_run_cmd(in const char *const *ppcArgv, in const _Atomic bool_t *pbCancel,
                               out char *pcOut, in size_t uOutCap, in uint32 u32TimeoutMs,
                               out int *piExitCode)
{
  return af_plat_run_cmd_cancellable(ppcArgv, NULL, pbCancel, pcOut, uOutCap, u32TimeoutMs,
                                     piExitCode);
}

static bool_t af__default_tcp_probe(in const char *pcIp, in uint16 u16Port, in uint32 u32TimeoutMs,
                                    in const _Atomic bool_t *pbCancel)
{
  struct sockaddr_in stAddr;
  int iFd = -1;
  int iRc = 0;
  uint64 u64Deadline = 0u;
  bool_t bOk = DM_FALSE;

  iFd = socket(AF_INET, SOCK_STREAM, 0);
  if (iFd < 0)
  {
    return DM_FALSE;
  }
  (void)fcntl(iFd, F_SETFL, O_NONBLOCK);
  memset(&stAddr, 0, sizeof(stAddr));
  stAddr.sin_family = AF_INET;
  stAddr.sin_port = htons(u16Port);
  if (1 != inet_pton(AF_INET, pcIp, &stAddr.sin_addr))
  {
    close(iFd);
    return DM_FALSE;
  }
  iRc = connect(iFd, (struct sockaddr *)&stAddr, sizeof(stAddr));
  if (0 == iRc)
  {
    close(iFd);
    return DM_TRUE;
  }
  if (EINPROGRESS != errno)
  {
    close(iFd);
    return DM_FALSE;
  }
  u64Deadline = dm_hrtime() + ((uint64)u32TimeoutMs * 1000000ULL);
  while (dm_hrtime() < u64Deadline)
  {
    struct pollfd stPfd = {.fd = iFd, .events = POLLOUT};
    int iPr = 0;
    if ((NULL != pbCancel) && atomic_load(pbCancel))
    {
      break;
    }
    iPr = poll(&stPfd, 1u, 50);
    if (iPr > 0)
    {
      int iSoErr = 0;
      socklen_t uLen = sizeof(iSoErr);
      if ((0 == getsockopt(iFd, SOL_SOCKET, SO_ERROR, &iSoErr, &uLen)) && (0 == iSoErr))
      {
        bOk = DM_TRUE;
      }
      break;
    }
    if (iPr < 0)
    {
      if (EINTR == errno) { continue; }
      break;
    }
  }
  close(iFd);
  return bOk;
}

static bool_t af__read_small_file(in const char *pcPath, out char *pcBuf, in size_t uCap)
{
  FILE *pf = fopen(pcPath, "rb");
  size_t uRead = 0u;

  if ((NULL == pf) || (NULL == pcBuf) || (uCap < 2u))
  {
    if (NULL != pf) { fclose(pf); }
    return DM_FALSE;
  }
  uRead = fread(pcBuf, 1u, uCap - 1u, pf);
  pcBuf[uRead] = '\0';
  fclose(pf);
  return (uRead > 0u) ? DM_TRUE : DM_FALSE;
}

static bool_t af__probe_agent(in const af__env_job_st *pstJob, in af_env_cmd_fn pfCmd,
                              out char *pcBuf, in size_t uBufCap)
{
  char acTarget[64];
  int iExit = -1;
  bool_t bPlistOk = DM_FALSE;

  if (af__read_small_file(pstJob->acPlistPath, pcBuf, uBufCap))
  {
    bPlistOk = af_env_parse_agent_plist(pcBuf, pstJob->acExePath);
    if ((!bPlistOk) && (0 == strncmp(pcBuf, "bplist", 6u)))
    {
      const char *apcPl[] = {"/usr/bin/plutil", "-convert", "xml1", "-o", "-",
                             pstJob->acPlistPath, NULL};
      if ((DM_SUCCESS == pfCmd(apcPl, &g_bAfQuitting, pcBuf, uBufCap, AF_ENV_CMD_TIMEOUT, &iExit))
          && (0 == iExit))
      {
        bPlistOk = af_env_parse_agent_plist(pcBuf, pstJob->acExePath);
      }
    }
  }
  if (!bPlistOk || atomic_load(&g_bAfQuitting))
  {
    return DM_FALSE;
  }
  snprintf(acTarget, sizeof(acTarget), "gui/%u/com.afar.agent", (unsigned)getuid());
  {
    const char *apcLc[] = {"/bin/launchctl", "print", acTarget, NULL};
    if (DM_SUCCESS != pfCmd(apcLc, &g_bAfQuitting, pcBuf, uBufCap, AF_ENV_CMD_TIMEOUT, &iExit))
    {
      return DM_FALSE;
    }
  }
  return af_env_parse_launchctl_ok(iExit, pcBuf);
}

static bool_t af__probe_fw(in const af__env_job_st *pstJob, in af_env_cmd_fn pfCmd, out char *pcBuf,
                           in size_t uBufCap)
{
  int iExit = -1;
  const char *apcGlobal[] = {"/usr/libexec/ApplicationFirewall/socketfilterfw", "--getglobalstate",
                             NULL};
  const char *apcApp[] = {"/usr/libexec/ApplicationFirewall/socketfilterfw", "--getappblocked",
                          pstJob->acExePath, NULL};

  if (DM_SUCCESS != pfCmd(apcGlobal, &g_bAfQuitting, pcBuf, uBufCap, AF_ENV_CMD_TIMEOUT, &iExit))
  {
    return DM_FALSE;
  }
  if (af_env_parse_fw_global_off(iExit, pcBuf))
  {
    return DM_TRUE;
  }
  if (atomic_load(&g_bAfQuitting))
  {
    return DM_FALSE;
  }
  if (DM_SUCCESS != pfCmd(apcApp, &g_bAfQuitting, pcBuf, uBufCap, AF_ENV_CMD_TIMEOUT, &iExit))
  {
    return DM_FALSE;
  }
  return af_env_parse_fw_app_permitted(iExit, pcBuf);
}

static void af__probe_ssh_keys(in const char *pcSshDir, io af_env_snap_st *pstSnap, out char *pcBuf,
                               in size_t uBufCap)
{
  static const char *const apcFiles[3] = {
      "ssh_host_ecdsa_key.pub",
      "ssh_host_rsa_key.pub",
      "ssh_host_ed25519_key.pub"
  };
  char acPath[600];
  int i = 0;

  for (i = 0; i < 3; i++)
  {
    snprintf(acPath, sizeof(acPath), "%s/%s", pcSshDir, apcFiles[i]);
    if (af__read_small_file(acPath, pcBuf, uBufCap))
    {
      (void)af_env_parse_ssh_pub(pcBuf, pstSnap->aacSshKeys, &pstSnap->u32SshKeyCnt,
                                 AF_SSH_KEYS_MAX);
    }
  }
}

static void af__env_work(io void *pvArg)
{
  af__env_job_st *pstJob = (af__env_job_st *)pvArg;
  af_env_cmd_fn pfCmd =
      (NULL != pstJob->stHooks.pfRunCmd) ? pstJob->stHooks.pfRunCmd : af__default_run_cmd;
  af_env_tcp_probe_fn pfTcp =
      (NULL != pstJob->stHooks.pfTcpProbe) ? pstJob->stHooks.pfTcpProbe : af__default_tcp_probe;
  char *pcBuf = (char *)dm_malloc_tag(AF_ENV_OUT_CAP, DM_MEM_TAG_JSON);
  int iExit = -1;

  if (NULL == pcBuf)
  {
    pstJob->bCancelled = DM_TRUE;
    return;
  }
  af_plat_current_user(pstJob->stSnap.acUser, sizeof(pstJob->stSnap.acUser));

  if (!atomic_load(&g_bAfQuitting))
  {
    pstJob->stSnap.bAgent = af__probe_agent(pstJob, pfCmd, pcBuf, AF_ENV_OUT_CAP);
  }
  if (!atomic_load(&g_bAfQuitting))
  {
    pstJob->stSnap.bFw = af__probe_fw(pstJob, pfCmd, pcBuf, AF_ENV_OUT_CAP);
  }
  if (!atomic_load(&g_bAfQuitting))
  {
    const char *apcPm[] = {"/usr/bin/pmset", "-g", NULL};
    if ((DM_SUCCESS == pfCmd(apcPm, &g_bAfQuitting, pcBuf, AF_ENV_OUT_CAP, AF_ENV_CMD_TIMEOUT,
                             &iExit))
        && (0 == iExit))
    {
      pstJob->stSnap.bAutoRestart = af_env_parse_pmset_autorestart(pcBuf);
    }
  }
  if (!atomic_load(&g_bAfQuitting))
  {
    const char *apcFv[] = {"/usr/bin/fdesetup", "isactive", NULL};
    if (DM_SUCCESS == pfCmd(apcFv, &g_bAfQuitting, pcBuf, AF_ENV_OUT_CAP, AF_ENV_CMD_TIMEOUT,
                            &iExit))
    {
      pstJob->stSnap.bFileVault = af_env_parse_filevault(iExit, pcBuf);
    }
  }
  if (!atomic_load(&g_bAfQuitting))
  {
    const char *apcAl[] = {"/usr/bin/defaults", "read", "/Library/Preferences/com.apple.loginwindow",
                           "autoLoginUser", NULL};
    if (DM_SUCCESS == pfCmd(apcAl, &g_bAfQuitting, pcBuf, AF_ENV_OUT_CAP, AF_ENV_CMD_TIMEOUT,
                            &iExit))
    {
      pstJob->stSnap.bAutoLogin = af_env_parse_autologin(iExit, pcBuf);
    }
  }
  if (!atomic_load(&g_bAfQuitting))
  {
    pstJob->stSnap.bRemoteLogin = pfTcp("127.0.0.1", 22u, AF_ENV_TCP_TIMEOUT, &g_bAfQuitting);
  }
  if (!atomic_load(&g_bAfQuitting))
  {
    af__probe_ssh_keys(pstJob->acSshDir, &pstJob->stSnap, pcBuf, AF_ENV_OUT_CAP);
  }
  pstJob->bCancelled = atomic_load(&g_bAfQuitting);
  dm_free_tag(pcBuf);
}

static void af__env_after(io void *pvArg, in int iStatus)
{
  af__env_job_st *pstJob = (af__env_job_st *)pvArg;
  bool_t bOk = ((0 == iStatus) && (!pstJob->bCancelled) && (!atomic_load(&g_bAfQuitting))
                && (!g_bEnvDraining))
                   ? DM_TRUE
                   : DM_FALSE;

  g_bEnvProbing = DM_FALSE;
  if (bOk)
  {
    g_stEnvSnap = pstJob->stSnap;
    g_stEnvSnap.bReady = DM_TRUE;
    (void)dm_event_publish(AF_EV_ENV_UPDATED, &g_stEnvSnap, (uint32)sizeof(g_stEnvSnap));
  }
  dm_free_tag(pstJob);

  if (g_iEnvDrainHold >= 0)
  {
    dm_drain_release(g_iEnvDrainHold);
    g_iEnvDrainHold = -1;
    return;
  }
  if (g_bEnvNeedReprobe && (!g_bEnvDraining) && (!atomic_load(&g_bAfQuitting)))
  {
    g_bEnvNeedReprobe = DM_FALSE;
    af_env_probe_now();
  }
}

static void af__fill_job_paths(out af__env_job_st *pstJob)
{
  char acHome[384];
  size_t uHomeCap = sizeof(acHome);
  char acRawExe[512];
  uint32 u32ExeSz = (uint32)sizeof(acRawExe);

  pstJob->stHooks = g_stEnvHooks;
  snprintf(pstJob->acSshDir, sizeof(pstJob->acSshDir), "%s",
           ((NULL != g_stEnvHooks.pcSshDir) && ('\0' != g_stEnvHooks.pcSshDir[0]))
               ? g_stEnvHooks.pcSshDir
               : "/etc/ssh");
  if ((NULL != g_stEnvHooks.pcPlistPath) && ('\0' != g_stEnvHooks.pcPlistPath[0]))
  {
    snprintf(pstJob->acPlistPath, sizeof(pstJob->acPlistPath), "%s", g_stEnvHooks.pcPlistPath);
  }
  else if (DM_SUCCESS == dm_os_homedir(acHome, &uHomeCap))
  {
    snprintf(pstJob->acPlistPath, sizeof(pstJob->acPlistPath),
             "%s/Library/LaunchAgents/com.afar.agent.plist", acHome);
  }
  if ((NULL != g_stEnvHooks.pcExePath) && ('\0' != g_stEnvHooks.pcExePath[0]))
  {
    snprintf(pstJob->acExePath, sizeof(pstJob->acExePath), "%s", g_stEnvHooks.pcExePath);
  }
  else if (0 == _NSGetExecutablePath(acRawExe, &u32ExeSz))
  {
    char acReal[PATH_MAX];
    snprintf(pstJob->acExePath, sizeof(pstJob->acExePath), "%s",
             (NULL != realpath(acRawExe, acReal)) ? acReal : acRawExe);
  }
}

/* ============================ 公开 API 与 DM 模块 ============================ */

void af_env_get_snapshot(out af_env_snap_st *pstOut)
{
  if (NULL != pstOut)
  {
    *pstOut = g_stEnvSnap;
  }
}

void af_env_set_hooks(in const af_env_hooks_st *pstHooks)
{
  if (NULL == pstHooks)
  {
    memset(&g_stEnvHooks, 0, sizeof(g_stEnvHooks));
  }
  else
  {
    g_stEnvHooks = *pstHooks;
  }
}

void af_env_probe_now(void)
{
  af__env_job_st *pstJob = NULL;

  if (g_bEnvDraining || atomic_load(&g_bAfQuitting))
  {
    return;
  }
  if (g_bEnvProbing)
  {
    g_bEnvNeedReprobe = DM_TRUE;
    return;
  }
  pstJob = (af__env_job_st *)dm_calloc_tag(1u, sizeof(*pstJob), DM_MEM_TAG_JSON);
  if (NULL == pstJob)
  {
    return;
  }
  af__fill_job_paths(pstJob);
  g_bEnvProbing = DM_TRUE;
  g_bEnvNeedReprobe = DM_FALSE;
  if (DM_SUCCESS != dm_pool_submit(DM_LANE_HEAVY, af__env_work, af__env_after, pstJob))
  {
    g_bEnvProbing = DM_FALSE;
    dm_free_tag(pstJob);
  }
}

static int af__on_env_probe_link(io void *pvArg)
{
  (void)pvArg;
  af_env_probe_now();
  return DM_SUCCESS;
}

static int af__on_env_ready(io void *pvArg)
{
  (void)pvArg;
  (void)dm_timer_add(AF_ENV_POLL_MS, AF_ENV_POLL_MS, AF_LINK_ENV_PROBE, 0u, &g_u32EnvTimerId);
  af_env_probe_now();
  return DM_SUCCESS;
}

static int af__env_init(void)
{
  memset(&g_stEnvSnap, 0, sizeof(g_stEnvSnap));
  g_bEnvProbing = DM_FALSE;
  g_bEnvNeedReprobe = DM_FALSE;
  g_bEnvDraining = DM_FALSE;
  g_u32EnvTimerId = 0u;
  g_iEnvDrainHold = -1;

  if ((DM_SUCCESS != dm_link_register_handler(AF_LINK_ENV_PROBE, af__on_env_probe_link, NULL, NULL))
      || (DM_SUCCESS != dm_event_subscribe(DM_EV_READY, af__on_env_ready, NULL, NULL)))
  {
    return DM_FAILD;
  }
  return DM_SUCCESS;
}

static void af__env_drain(io dm_drain_st *pstDrain)
{
  g_bEnvDraining = DM_TRUE;
  g_bEnvNeedReprobe = DM_FALSE;
  if (0u != g_u32EnvTimerId)
  {
    (void)dm_timer_cancel(g_u32EnvTimerId);
    g_u32EnvTimerId = 0u;
  }
  if (g_bEnvProbing)
  {
    g_iEnvDrainHold = dm_drain_hold(pstDrain);
  }
}

static void af__env_fini(void)
{
  memset(&g_stEnvSnap, 0, sizeof(g_stEnvSnap));
  g_bEnvProbing = DM_FALSE;
  g_bEnvNeedReprobe = DM_FALSE;
  g_bEnvDraining = DM_FALSE;
  g_u32EnvTimerId = 0u;
  g_iEnvDrainHold = -1;
}

const dm_module_st g_stAfEnvMod = {
    .u16ModID = AF_MOD_ENV,
    .pcName = "af_env",
    .pfInit = af__env_init,
    .pfDrain = af__env_drain,
    .pfFini = af__env_fini,
    .pastErrMap = NULL
};

const dm_module_st *af_env_module(void)
{
  return &g_stAfEnvMod;
}
