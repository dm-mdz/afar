/**
 ******************************************************************************
 * @file    af_cli.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   afar-agent 10 条命令行子命令、沙箱化 install/uninstall 与本机 dmc 协同实现（DD-05 §3）。
 ******************************************************************************
 */

#define _DARWIN_C_SOURCE
#define DM_LOG_MOD "cli"

#include "af_cli.h"
#include "af_migrate.h"
#include "af_modules.h"
#include "af_store.h"
#include "plat/af_plat.h"
#include "power/af_power.h"

#include <dm/os.h>
#include <dmc_client.h>

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
#include <sys/stat.h>
#include <unistd.h>

#define AF_CLI_VERIFY_DEFAULT_MS 25000u
#define AF_CLI_LEGACY_DAEMON     "/Library/LaunchDaemons/com.afar.restart.plist"

static af_cli_ops_st g_stCliOps;

void af_cli_set_ops(dm_in const af_cli_ops_st *pstOps)
{
  if (NULL == pstOps)
  {
    memset(&g_stCliOps, 0, sizeof(g_stCliOps));
  }
  else
  {
    g_stCliOps = *pstOps;
  }
}

static const char *af__home_dir(void)
{
  if ((NULL != g_stCliOps.pcHomeDir) && ('\0' != g_stCliOps.pcHomeDir[0]))
  {
    return g_stCliOps.pcHomeDir;
  }
  {
    const char *pcH = getenv("HOME");
    return ((NULL != pcH) && ('\0' != pcH[0])) ? pcH : "/tmp";
  }
}

void af_cli_default_data_dir(dm_out char *pcBuf, dm_in size_t uCap)
{
  if ((NULL == pcBuf) || (0u == uCap)) { return; }
  if ((NULL != g_stCliOps.pcDataDir) && ('\0' != g_stCliOps.pcDataDir[0]))
  {
    snprintf(pcBuf, uCap, "%s", g_stCliOps.pcDataDir);
    return;
  }
  snprintf(pcBuf, uCap, "%s/Library/Application Support/Afar", af__home_dir());
}

static void af__resolve_exe_path(dm_in const char *pcOverride, dm_out char *pcBuf, dm_in size_t uCap)
{
  char acRaw[PATH_MAX];
  char acReal[PATH_MAX];
  uint32 u32Sz = (uint32)sizeof(acRaw);

  if ((NULL != pcOverride) && ('\0' != pcOverride[0]))
  {
    snprintf(pcBuf, uCap, "%s", pcOverride);
    return;
  }
  if ((NULL != g_stCliOps.pcExePath) && ('\0' != g_stCliOps.pcExePath[0]))
  {
    snprintf(pcBuf, uCap, "%s", g_stCliOps.pcExePath);
    return;
  }
  if (0 == _NSGetExecutablePath(acRaw, &u32Sz))
  {
    snprintf(pcBuf, uCap, "%s", (NULL != realpath(acRaw, acReal)) ? acReal : acRaw);
    return;
  }
  pcBuf[0] = '\0';
}

static int af__mkdir_p(dm_in const char *pcDir)
{
  char acTmp[600];
  size_t uLen = 0u;
  char *pcP = NULL;

  if ((NULL == pcDir) || ('\0' == pcDir[0])) { return DM_INVALID; }
  uLen = strlen(pcDir);
  if (uLen >= sizeof(acTmp)) { return DM_TOO_BIG; }
  memcpy(acTmp, pcDir, uLen + 1u);
  for (pcP = acTmp + 1; '\0' != *pcP; pcP++)
  {
    if ('/' == *pcP)
    {
      *pcP = '\0';
      if ((0 != mkdir(acTmp, 0700)) && (EEXIST != errno)) { return DM_IO; }
      *pcP = '/';
    }
  }
  if ((0 != mkdir(acTmp, 0700)) && (EEXIST != errno)) { return DM_IO; }
  return DM_SUCCESS;
}

static int af__run_cmd(dm_in const char *const *ppcArgv, dm_out char *pcOut, dm_in size_t uCap,
                       dm_in uint32 u32Ms, dm_out int *piExit)
{
  if (NULL != g_stCliOps.pfRunCmd)
  {
    return g_stCliOps.pfRunCmd(ppcArgv, NULL, pcOut, uCap, u32Ms, piExit);
  }
  return af_plat_run_cmd_cancellable(ppcArgv, NULL, NULL, pcOut, uCap, u32Ms, piExit);
}

static bool_t af__default_tcp_check(dm_in const char *pcIp, dm_in uint16 u16Port, dm_in uint32 u32TimeoutMs)
{
  struct sockaddr_in stAddr;
  int iFd = socket(AF_INET, SOCK_STREAM, 0);
  bool_t bOk = DM_FALSE;

  if (iFd < 0) { return DM_FALSE; }
  (void)fcntl(iFd, F_SETFL, O_NONBLOCK);
  memset(&stAddr, 0, sizeof(stAddr));
  stAddr.sin_family = AF_INET;
  stAddr.sin_port = htons(u16Port);
  if (1 == inet_pton(AF_INET, pcIp, &stAddr.sin_addr))
  {
    int iRc = connect(iFd, (struct sockaddr *)&stAddr, sizeof(stAddr));
    if (0 == iRc)
    {
      bOk = DM_TRUE;
    }
    else if (EINPROGRESS == errno)
    {
      struct pollfd stPfd = {.fd = iFd, .events = POLLOUT};
      if (poll(&stPfd, 1u, (int)((u32TimeoutMs > 0u) ? u32TimeoutMs : 200u)) > 0)
      {
        int iSo = 0;
        socklen_t uSl = sizeof(iSo);
        if ((0 == getsockopt(iFd, SOL_SOCKET, SO_ERROR, &iSo, &uSl)) && (0 == iSo))
        {
          bOk = DM_TRUE;
        }
      }
    }
  }
  close(iFd);
  return bOk;
}

static int af__dmc_call(dm_in const char *pcDataDir, dm_in const char *pcIface, dm_in const char *pcBody,
                        dm_out char *pcOut, dm_in size_t uOutCap)
{
  dmc_client_opt_st stOpt;
  dmc_client_st *pstCli = NULL;
  dmc_resp_st stResp;
  int iErr = 0;
  int iRc = 0;

  if ((NULL != pcOut) && (uOutCap > 0u)) { pcOut[0] = '\0'; }
  if (NULL != g_stCliOps.pfDmcCall)
  {
    return g_stCliOps.pfDmcCall(pcDataDir, pcIface, pcBody, pcOut, uOutCap);
  }
  memset(&stOpt, 0, sizeof(stOpt));
  stOpt.pcTarget = "local://";
  stOpt.pcDataDir = pcDataDir;
  stOpt.pcRole = "cli";
  stOpt.u32ConnectTimeoutMs = 3000u;
  stOpt.u32DefaultTimeoutMs = 5000u;
  pstCli = dmc_client_open(&stOpt, &iErr);
  if (NULL == pstCli)
  {
    return (0 != iErr) ? iErr : DM_FAILD;
  }
  memset(&stResp, 0, sizeof(stResp));
  iRc = dmc_client_call_name(pstCli, pcIface, (NULL != pcBody) ? pcBody : "{}", 5000u, &stResp);
  if ((0 == iRc) && (0 == stResp.iStatus))
  {
    if ((NULL != pcOut) && (uOutCap > 0u) && (NULL != stResp.pcBody))
    {
      snprintf(pcOut, uOutCap, "%s", stResp.pcBody);
    }
    iRc = DM_SUCCESS;
  }
  else
  {
    iRc = (0 != stResp.iStatus) ? stResp.iStatus : ((0 != iRc) ? iRc : DM_FAILD);
  }
  if ((NULL != stResp.pcBody) && (stResp.u32Len > 0u))
  {
    dm_secure_zero(stResp.pcBody, stResp.u32Len);
  }
  dmc_resp_free(&stResp);
  dmc_client_close(pstCli);
  return iRc;
}

int af_cli_wake_existing(dm_in const char *pcDataDir)
{
  char acDir[512];
  char acResp[256];
  const char *pcDir = pcDataDir;

  if ((NULL == pcDir) || ('\0' == pcDir[0]))
  {
    af_cli_default_data_dir(acDir, sizeof(acDir));
    pcDir = acDir;
  }
  return af__dmc_call(pcDir, "setup.window_open", "{}", acResp, sizeof(acResp));
}

/* ============================ install / uninstall ============================ */

static void af__xml_escape(dm_in const char *pcIn, dm_out char *pcOut, dm_in size_t uCap)
{
  size_t uPos = 0u;
  const char *pcP = pcIn;

  if ((NULL == pcOut) || (0u == uCap)) { return; }
  while ((NULL != pcP) && ('\0' != *pcP) && ((uPos + 6u) < uCap))
  {
    const char *pcRep = NULL;
    if ('&' == *pcP) { pcRep = "&amp;"; }
    else if ('<' == *pcP) { pcRep = "&lt;"; }
    else if ('>' == *pcP) { pcRep = "&gt;"; }
    else if ('"' == *pcP) { pcRep = "&quot;"; }
    else if ('\'' == *pcP) { pcRep = "&apos;"; }
    if (NULL != pcRep)
    {
      size_t uL = strlen(pcRep);
      memcpy(pcOut + uPos, pcRep, uL);
      uPos += uL;
    }
    else
    {
      pcOut[uPos++] = *pcP;
    }
    pcP++;
  }
  pcOut[uPos] = '\0';
}

bool_t af_cli_is_external_vol(dm_in const char *pcPath)
{
  char acReal[PATH_MAX];

  if (NULL == pcPath) { return DM_FALSE; }
  if (0 == strncmp(pcPath, "/Volumes/", 9u)) { return DM_TRUE; }
  if ((NULL != realpath(pcPath, acReal)) && (0 == strncmp(acReal, "/Volumes/", 9u)))
  {
    return DM_TRUE;
  }
  return DM_FALSE;
}

static bool_t af__is_external_vol(dm_in const char *pcPath)
{
  if (NULL != g_stCliOps.pfIsExternalVol)
  {
    return g_stCliOps.pfIsExternalVol(pcPath);
  }
  return af_cli_is_external_vol(pcPath);
}

static void af__choose_log_path(dm_in const char *pcExe, dm_in const char *pcDataDir, dm_out char *pcOut,
                                dm_in size_t uCap)
{
  const char *pcHome = af__home_dir();
  char acLogsDir[512];

  if (af__is_external_vol(pcExe) || af__is_external_vol(pcDataDir))
  {
    snprintf(acLogsDir, sizeof(acLogsDir), "%s/Library/Logs", pcHome);
    (void)af__mkdir_p(acLogsDir);
    snprintf(pcOut, uCap, "%s/com.afar.agent.log", acLogsDir);
  }
  else
  {
    (void)af__mkdir_p(pcDataDir);
    snprintf(pcOut, uCap, "%s/agent.log", pcDataDir);
  }
}

static bool_t af__verify_agent_up(dm_in const char *pcTarget, dm_in uint16 u16Port, dm_in uint32 u32MaxMs)
{
  uint64 u64Deadline = dm_hrtime() + ((uint64)u32MaxMs * 1000000ULL);
  const char *apcPrint[4] = {"/bin/launchctl", "print", pcTarget, NULL};
  char acOut[512];
  int iExit = -1;

  for (;;)
  {
    bool_t bTcpOk = (NULL != g_stCliOps.pfTcpCheck)
                        ? g_stCliOps.pfTcpCheck("127.0.0.1", u16Port, 200u)
                        : af__default_tcp_check("127.0.0.1", u16Port, 200u);
    if (bTcpOk && (DM_SUCCESS == af__run_cmd(apcPrint, acOut, sizeof(acOut), 3000u, &iExit))
        && (0 == iExit))
    {
      return DM_TRUE;
    }
    if (dm_hrtime() >= u64Deadline)
    {
      return DM_FALSE;
    }
    dm_sleep(50u);
  }
}

int af_cli_install_agent(dm_in const char *pcExePath)
{
  char acExe[PATH_MAX];
  char acDataDir[512];
  char acAgentDir[512];
  char acPlistPath[600];
  char acLogPath[600];
  char acExeXml[1024];
  char acLogXml[1024];
  char acXml[3072];
  char acDomain[64];
  char acTarget[96];
  char acOut[512];
  int iExit = -1;
  uint32 u32Timeout = (0u != g_stCliOps.u32VerifyTimeoutMs) ? g_stCliOps.u32VerifyTimeoutMs
                                                            : AF_CLI_VERIFY_DEFAULT_MS;

  af__resolve_exe_path(pcExePath, acExe, sizeof(acExe));
  if (('\0' == acExe[0]) || (0 != access(acExe, X_OK)))
  {
    fprintf(stderr, "install failed: executable not found or not executable: %s\n", acExe);
    return 1;
  }
  af_cli_default_data_dir(acDataDir, sizeof(acDataDir));
  (void)af_migrate_run(acDataDir);
  (void)af_store_load(acDataDir);
  snprintf(acAgentDir, sizeof(acAgentDir), "%s/Library/LaunchAgents", af__home_dir());
  if (DM_SUCCESS != af__mkdir_p(acAgentDir))
  {
    fprintf(stderr, "install failed: cannot create %s\n", acAgentDir);
    return 1;
  }
  af__choose_log_path(acExe, acDataDir, acLogPath, sizeof(acLogPath));
  af__xml_escape(acExe, acExeXml, sizeof(acExeXml));
  af__xml_escape(acLogPath, acLogXml, sizeof(acLogXml));
  snprintf(acXml, sizeof(acXml),
           "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\"><dict>\n"
           "  <key>Label</key><string>com.afar.agent</string>\n"
           "  <key>ProgramArguments</key><array><string>%s</string><string>run</string></array>\n"
           "  <key>RunAtLoad</key><true/>\n"
           "  <key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>\n"
           "  <key>StandardOutPath</key><string>%s</string>\n"
           "  <key>StandardErrorPath</key><string>%s</string>\n"
           "</dict></plist>\n",
           acExeXml, acLogXml, acLogXml);
  snprintf(acPlistPath, sizeof(acPlistPath), "%s/com.afar.agent.plist", acAgentDir);
  if (DM_SUCCESS != af_store_write_atomic(acPlistPath, acXml, strlen(acXml)))
  {
    fprintf(stderr, "install failed: cannot write %s\n", acPlistPath);
    return 1;
  }
  snprintf(acDomain, sizeof(acDomain), "gui/%u", (unsigned)getuid());
  snprintf(acTarget, sizeof(acTarget), "gui/%u/com.afar.agent", (unsigned)getuid());
  {
    const char *apcOut[4] = {"/bin/launchctl", "bootout", acTarget, NULL};
    const char *apcBoot[5] = {"/bin/launchctl", "bootstrap", acDomain, acPlistPath, NULL};
    (void)af__run_cmd(apcOut, acOut, sizeof(acOut), 5000u, &iExit);
    if ((DM_SUCCESS != af__run_cmd(apcBoot, acOut, sizeof(acOut), 5000u, &iExit)) || (0 != iExit))
    {
      fprintf(stderr, "install failed: launchctl bootstrap exited %d\n", iExit);
      return 1;
    }
  }
  if (!af__verify_agent_up(acTarget, af_store_cfg()->u16Port, u32Timeout))
  {
    fprintf(stderr, "install failed: agent did not come up within %u ms\n", u32Timeout);
    return 1;
  }
  printf("installed %s\n", acPlistPath);
  return 0;
}

static int af__escape_as_sh(dm_in const char *pcPath, dm_out char *pcOut, dm_in size_t uCap)
{
  char acSh[512];
  size_t uSh = 0u;
  size_t uOut = 0u;
  const char *pcP = pcPath;

  if ((NULL == pcPath) || (NULL == pcOut) || (uCap < 4u)) { return DM_INVALID; }
  acSh[uSh++] = '\'';
  while ('\0' != *pcP)
  {
    if ('\'' == *pcP)
    {
      if ((uSh + 4u) >= sizeof(acSh)) { return DM_TOO_BIG; }
      memcpy(acSh + uSh, "'\\''", 4u);
      uSh += 4u;
    }
    else
    {
      if ((uSh + 1u) >= sizeof(acSh)) { return DM_TOO_BIG; }
      acSh[uSh++] = *pcP;
    }
    pcP++;
  }
  if ((uSh + 2u) > sizeof(acSh)) { return DM_TOO_BIG; }
  acSh[uSh++] = '\'';
  acSh[uSh] = '\0';
  for (pcP = acSh; '\0' != *pcP; pcP++)
  {
    if (('\\' == *pcP) || ('"' == *pcP))
    {
      if ((uOut + 2u) >= uCap) { return DM_TOO_BIG; }
      pcOut[uOut++] = '\\';
      pcOut[uOut++] = *pcP;
    }
    else
    {
      if ((uOut + 1u) >= uCap) { return DM_TOO_BIG; }
      pcOut[uOut++] = *pcP;
    }
  }
  pcOut[uOut] = '\0';
  return DM_SUCCESS;
}

static int af__cmd_uninstall(void)
{
  char acDataDir[512];
  char acPlist[600];
  char acTarget[96];
  char acOut[512];
  int iExit = -1;
  const char *pcLegacy = (NULL != g_stCliOps.pcLegacyDaemon) ? g_stCliOps.pcLegacyDaemon
                                                             : AF_CLI_LEGACY_DAEMON;

  snprintf(acTarget, sizeof(acTarget), "gui/%u/com.afar.agent", (unsigned)getuid());
  {
    const char *apcOut[4] = {"/bin/launchctl", "bootout", acTarget, NULL};
    (void)af__run_cmd(apcOut, acOut, sizeof(acOut), 5000u, &iExit);
  }
  snprintf(acPlist, sizeof(acPlist), "%s/Library/LaunchAgents/com.afar.agent.plist", af__home_dir());
  (void)unlink(acPlist);

  af_cli_default_data_dir(acDataDir, sizeof(acDataDir));
  (void)af_store_load(acDataDir);
  af_power_restore_screensaver(&af_store_state_mut()->stOrigSs, DM_TRUE, acDataDir);

  if (NULL != g_stCliOps.pfKeychainDelLegacyPw)
  {
    g_stCliOps.pfKeychainDelLegacyPw();
  }
  else
  {
    af_plat_keychain_del_legacy_pw();
  }

  if (0 == access(pcLegacy, F_OK))
  {
    char acQuoted[640];
    char acExpr[896];
    const char *apcOsa[4] = {"/usr/bin/osascript", "-e", acExpr, NULL};
    if (DM_SUCCESS == af__escape_as_sh(pcLegacy, acQuoted, sizeof(acQuoted)))
    {
      snprintf(acExpr, sizeof(acExpr),
               "do shell script \"/bin/launchctl bootout system/com.afar.restart >/dev/null 2>&1; "
               "/bin/rm -f %s\" with administrator privileges",
               acQuoted);
      (void)af__run_cmd(apcOsa, acOut, sizeof(acOut), 30000u, &iExit);
    }
  }
  printf("uninstalled com.afar.agent\n");
  return 0;
}

/* ============================ 离线查询与本机协同子命令 ============================ */

static int af__cmd_config(void)
{
  char acDataDir[512];
  const af_config_st *pstCfg = NULL;
  const af_state_st *pstSt = NULL;

  af_cli_default_data_dir(acDataDir, sizeof(acDataDir));
  (void)af_migrate_run(acDataDir);
  (void)af_store_load(acDataDir);
  pstCfg = af_store_cfg();
  pstSt = af_store_state();
  printf("data_dir: %s\nport: %u\nauto_connect: %s\nmirror: %s\nscale: %s\n"
         "ipad_name: %s\nsetup_done: %s\nplan: %s\nanti_unplug: %s\n",
         acDataDir, (unsigned)pstCfg->u16Port, pstCfg->bAutoConnect ? "true" : "false",
         pstCfg->bMirror ? "true" : "false", pstCfg->acScale, pstCfg->acIpadName,
         pstSt->bSetupDone ? "true" : "false", pstSt->acPlan,
         pstSt->bAntiUnplug ? "true" : "false");
  return 0;
}

static bool_t af__is_valid_bt_addr(dm_in const char *pcAddr)
{
  size_t i = 0u;

  if ((NULL == pcAddr) || (17u != strlen(pcAddr)))
  {
    return DM_FALSE;
  }
  for (i = 0u; i < 17u; i++)
  {
    if (2u == (i % 3u))
    {
      if (('-' != pcAddr[i]) && (':' != pcAddr[i])) { return DM_FALSE; }
    }
    else if (0 == isxdigit((unsigned char)pcAddr[i]))
    {
      return DM_FALSE;
    }
  }
  return DM_TRUE;
}

static int af__cmd_bt_dump(dm_in int argc, dm_in char **argv)
{
  if (argc > 2)
  {
    const char *pcAct = argv[2];
    if (((0 == strcmp(pcAct, "open")) || (0 == strcmp(pcAct, "close"))) && (argc >= 4)
        && af__is_valid_bt_addr(argv[3]))
    {
      int iRc = (0 == strcmp(pcAct, "open"))
                    ? ((NULL != g_stCliOps.pfBtConnect) ? g_stCliOps.pfBtConnect(argv[3])
                                                        : af_plat_bt_connect(argv[3]))
                    : ((NULL != g_stCliOps.pfBtDisconnect) ? g_stCliOps.pfBtDisconnect(argv[3])
                                                           : af_plat_bt_disconnect(argv[3]));
      return (DM_SUCCESS == iRc) ? 0 : 1;
    }
    fprintf(stderr, "usage: afar-agent bt-dump [open|close <aa-bb-cc-dd-ee-ff>]\n");
    return 1;
  }
  {
    af_bt_device_st astDevs[AF_BT_DEV_MAX];
    uint32 u32Cnt = (NULL != g_stCliOps.pfBtPaired)
                        ? g_stCliOps.pfBtPaired(astDevs, AF_BT_DEV_MAX)
                        : af_plat_bt_paired(astDevs, AF_BT_DEV_MAX);
    uint32 i = 0u;
    for (i = 0u; i < u32Cnt; i++)
    {
      printf("%s\t%s\tmajor=%u\tble=%s\tconnected=%s\n", astDevs[i].acAddr, astDevs[i].acName,
             (unsigned)astDevs[i].u32Major, astDevs[i].bBle ? "true" : "false",
             astDevs[i].bConnected ? "true" : "false");
    }
  }
  return 0;
}

static int af__cmd_ipads(void)
{
  af_sc_device_st astDevs[AF_SC_DEV_MAX];
  uint32 u32Cnt = (NULL != g_stCliOps.pfScList) ? g_stCliOps.pfScList(astDevs, AF_SC_DEV_MAX)
                                                : af_plat_sc_list(astDevs, AF_SC_DEV_MAX);
  uint32 i = 0u;

  for (i = 0u; i < u32Cnt; i++)
  {
    printf("%s\tconnected=%s\n", astDevs[i].acName, astDevs[i].bConnected ? "true" : "false");
  }
  return 0;
}

static int af__cmd_connect(dm_in int argc, dm_in char **argv)
{
  char acTarget[65] = {0};
  char acErr[128] = {0};
  int iRc = DM_FAILD;

  if ((argc >= 3) && ('\0' != argv[2][0]))
  {
    snprintf(acTarget, sizeof(acTarget), "%s", argv[2]);
  }
  else
  {
    af_sc_device_st astDevs[AF_SC_DEV_MAX];
    uint32 u32Cnt = (NULL != g_stCliOps.pfScList) ? g_stCliOps.pfScList(astDevs, AF_SC_DEV_MAX)
                                                  : af_plat_sc_list(astDevs, AF_SC_DEV_MAX);
    if (1u != u32Cnt)
    {
      fprintf(stderr, "connect requires target name (found %u nearby iPad(s))\n", (unsigned)u32Cnt);
      return 1;
    }
    snprintf(acTarget, sizeof(acTarget), "%s", astDevs[0].acName);
  }
  iRc = (NULL != g_stCliOps.pfScConnect) ? g_stCliOps.pfScConnect(acTarget, acErr, sizeof(acErr))
                                         : af_plat_sc_connect(acTarget, acErr, sizeof(acErr));
  if (DM_SUCCESS != iRc)
  {
    fprintf(stderr, "connect failed: %s\n", ('\0' != acErr[0]) ? acErr : "unknown");
    return 1;
  }
  printf("connected %s\n", acTarget);
  return 0;
}

static int af__cmd_disconnect(void)
{
  char acDataDir[512];
  char acResp[256];
  char acErr[128] = {0};
  int iRc = DM_FAILD;

  af_cli_default_data_dir(acDataDir, sizeof(acDataDir));
  if (DM_SUCCESS == af__dmc_call(acDataDir, "session.disconnect", "{}", acResp, sizeof(acResp)))
  {
    printf("disconnected (via agent)\n");
    return 0;
  }
  iRc = (NULL != g_stCliOps.pfScDisconnect) ? g_stCliOps.pfScDisconnect(NULL, acErr, sizeof(acErr))
                                            : af_plat_sc_disconnect(NULL, acErr, sizeof(acErr));
  if (DM_SUCCESS != iRc)
  {
    fprintf(stderr, "disconnect failed: %s\n", ('\0' != acErr[0]) ? acErr : "unknown");
    return 1;
  }
  printf("disconnected (offline)\n");
  return 0;
}

static int af__cmd_displays(void)
{
  af_disp_info_st astDisps[AF_DISP_MAX];
  uint32 u32Cnt = (NULL != g_stCliOps.pfDispOnline)
                      ? g_stCliOps.pfDispOnline(astDisps, AF_DISP_MAX)
                      : af_plat_disp_online(astDisps, AF_DISP_MAX);
  uint32 i = 0u;

  for (i = 0u; i < u32Cnt; i++)
  {
    printf("id=%u vendor=0x%04x model=0x%04x serial=%u builtin=%u mirror=%u %ux%u (%ux%u)\n",
           (unsigned)astDisps[i].u32Id, (unsigned)astDisps[i].u32Vendor,
           (unsigned)astDisps[i].u32Model, (unsigned)astDisps[i].u32Serial,
           astDisps[i].bBuiltin ? 1u : 0u, (unsigned)astDisps[i].u32MirrorOf,
           (unsigned)astDisps[i].stCur.u32W, (unsigned)astDisps[i].stCur.u32H,
           (unsigned)astDisps[i].stCur.u32Pw, (unsigned)astDisps[i].stCur.u32Ph);
  }
  return 0;
}

static int af__cmd_rpc_print(dm_in const char *pcIface)
{
  char acDataDir[512];
  char acResp[4096];

  af_cli_default_data_dir(acDataDir, sizeof(acDataDir));
  if (DM_SUCCESS != af__dmc_call(acDataDir, pcIface, "{}", acResp, sizeof(acResp)))
  {
    dm_secure_zero(acResp, sizeof(acResp));
    fprintf(stderr, "%s failed: agent not running or rejected\n", pcIface);
    return 1;
  }
  printf("%s\n", acResp);
  dm_secure_zero(acResp, sizeof(acResp));
  return 0;
}

bool_t af_cli_is_subcommand(dm_in const char *pcArg)
{
  static const char *const apcCmds[] = {
      "--version", "-V",      "-v",         "run",        "install",   "uninstall",
      "config",    "bt-dump", "ipads",      "connect",    "disconnect", "displays",
      "pair-code", "pair",    "setup",      "status",     "diag",      "help",
      "--help",    "-h"
  };
  size_t i = 0u;

  if (NULL == pcArg) { return DM_FALSE; }
  for (i = 0u; i < (sizeof(apcCmds) / sizeof(apcCmds[0])); i++)
  {
    if (0 == strcmp(pcArg, apcCmds[i])) { return DM_TRUE; }
  }
  return DM_FALSE;
}

int af_cli_run(dm_in int argc, dm_in char **argv)
{
  const char *pcCmd = ((argc > 1) && (NULL != argv[1])) ? argv[1] : "";

  if ((0 == strcmp(pcCmd, "--version")) || (0 == strcmp(pcCmd, "-V")) || (0 == strcmp(pcCmd, "-v")))
  {
    printf("afar-agent %s\n", AF_VERSION);
    return 0;
  }
  if ((0 == strcmp(pcCmd, "help")) || (0 == strcmp(pcCmd, "--help")) || (0 == strcmp(pcCmd, "-h")))
  {
    printf("usage: afar-agent <run|install|uninstall|config|bt-dump|ipads|connect|disconnect|"
           "displays|pair-code|setup|status|--version|help>\n");
    return 0;
  }
  if (0 == strcmp(pcCmd, "run"))
  {
    return (NULL != g_stCliOps.pfRunDaemon) ? g_stCliOps.pfRunDaemon(argc, argv) : 0;
  }
  if (0 == strcmp(pcCmd, "install")) { return af_cli_install_agent(NULL); }
  if (0 == strcmp(pcCmd, "uninstall")) { return af__cmd_uninstall(); }
  if (0 == strcmp(pcCmd, "config")) { return af__cmd_config(); }
  if (0 == strcmp(pcCmd, "bt-dump")) { return af__cmd_bt_dump(argc, argv); }
  if (0 == strcmp(pcCmd, "ipads")) { return af__cmd_ipads(); }
  if (0 == strcmp(pcCmd, "connect")) { return af__cmd_connect(argc, argv); }
  if (0 == strcmp(pcCmd, "disconnect")) { return af__cmd_disconnect(); }
  if (0 == strcmp(pcCmd, "displays")) { return af__cmd_displays(); }
  if ((0 == strcmp(pcCmd, "pair-code")) || (0 == strcmp(pcCmd, "pair")))
  {
    return af__cmd_rpc_print("auth.pair_code");
  }
  if (0 == strcmp(pcCmd, "setup")) { return af__cmd_rpc_print("setup.window_open"); }
  if ((0 == strcmp(pcCmd, "status")) || (0 == strcmp(pcCmd, "diag")))
  {
    return af__cmd_rpc_print("dm.diag.snapshot");
  }
  fprintf(stderr, "unknown command: %s\n", pcCmd);
  return 1;
}
