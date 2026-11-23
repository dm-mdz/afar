/**
 ******************************************************************************
 * @file    main.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   Afar Mac 端唯一入口（S3 全 8 模块装配、原生壳回调注入、老版迁移与 10 条 CLI 分发）。
 ******************************************************************************
 */

#include <dm.h>
#include <dm/https.h>
#include <dm/link.h>
#include <dm/module.h>
#include <dm/pool.h>

#include "af_cli.h"
#include "af_migrate.h"
#include "af_modules.h"
#include "af_store.h"
#include "auth/af_auth.h"
#include "display/af_display.h"
#include "env/af_env.h"
#include "kbd/af_kbd.h"
#include "plat/af_plat.h"
#include "power/af_power.h"
#include "session/af_session.h"
#include "setup/af_setup.h"
#include "sidecar/af_sidecar.h"
#include "../shell/af_shell_app.h"
#include "../shell/af_shell_menu.h"
#include "../shell/af_shell_win.h"

#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_acMainDataDir[512];

static int af__wake_default(void)
{
  return af_cli_wake_existing(NULL);
}

static bool_t af__try_bundle_launch(in int argc, in char **argv, out int *piExit)
{
  char acRaw[PATH_MAX];
  char acReal[PATH_MAX];
  char acApp[PATH_MAX];
  uint32 u32Sz = (uint32)sizeof(acRaw);
  const char *pcExe = NULL;

  *piExit = 0;
  if ((argc > 1) && (0 == strcmp(argv[1], "run")))
  {
    return DM_FALSE;
  }
  if (0 == _NSGetExecutablePath(acRaw, &u32Sz))
  {
    pcExe = acRaw;
  }
  else if ((argc > 0) && (NULL != argv[0]))
  {
    pcExe = argv[0];
  }
  if (NULL == pcExe)
  {
    return DM_FALSE;
  }
  if (NULL != realpath(pcExe, acReal))
  {
    pcExe = acReal;
  }
  if (!af_shell_find_app_bundle(pcExe, acApp, sizeof(acApp)))
  {
    return DM_FALSE;
  }
  {
    static const af_bundle_launch_cb_st stCb = {
        .pfEnsureInstalled = af_cli_install_agent,
        .pfWakeRunning = af__wake_default
    };
    af_bundle_action_et eAct = af_shell_launch_bundle(acApp, pcExe, AF_VERSION, &stCb);
    if (AF_BUNDLE_ACT_UPGRADE_FAILED == eAct)
    {
      *piExit = 1;
    }
  }
  return DM_TRUE;
}

static void af__on_https_drain(void)
{
  atomic_store(&g_bAfQuitting, DM_TRUE);
}

static void af__on_wake_existing(void)
{
  (void)af_cli_wake_existing(g_acMainDataDir);
}

static void af__setup_lanes(void)
{
  dm_pool_lane_cfg_st stDb = {1u, 1u, 30000u, 0u};
  dm_pool_lane_cfg_st stDef = {1u, 4u, 30000u, 0u};
  dm_pool_lane_cfg_st stHvy = {1u, 3u, 30000u, 0u};

  (void)dm_ext_register(dm_net_hooks());
  (void)dm_ext_register(dm_https_hooks());
  (void)dm_pool_set_lane(DM_LANE_DB, &stDb);
  (void)dm_pool_set_lane(DM_LANE_DEFAULT, &stDef);
  (void)dm_pool_set_lane(DM_LANE_HEAVY, &stHvy);
}

static void af__fill_modules(out dm_module_st astMods[8])
{
  astMods[0] = *af_auth_module();
  astMods[1] = *af_env_module();
  astMods[2] = *af_power_module();
  astMods[3] = *af_display_module();
  astMods[4] = *af_sidecar_module();
  astMods[5] = *af_kbd_module();
  astMods[6] = *af_session_module();
  astMods[7] = *af_setup_module();
}

static int af__run_daemon(in int argc, io char **argv)
{
  char acCert[600];
  char acKey[600];
  char acCliTok[65];
  uint8 au8CertFp[32];
  dm_role_st stCliRole;
  dm_https_cfg_st stHttps;
  dm_cfg_st stCfg;
  dm_module_st astMods[8];
  af_shell_run_cfg_st stShellCfg;
  const char *pcNoBar = getenv("AFAR_NO_MENUBAR");
  int iRc = 0;

  af_cli_default_data_dir(g_acMainDataDir, sizeof(g_acMainDataDir));
  (void)af_migrate_run(g_acMainDataDir);
  (void)af_store_load(g_acMainDataDir);
  if ((DM_SUCCESS != af_auth_ensure_tls_cert(g_acMainDataDir, au8CertFp))
      || (DM_SUCCESS != dm_token_make(acCliTok, sizeof(acCliTok))))
  {
    dm_secure_zero(acCliTok, sizeof(acCliTok));
    return 1;
  }
  af__setup_lanes();
  af_session_set_menu_cb(af_shell_menu_update);
  af_setup_set_ui_cb(af_shell_win_push, af_shell_win_show);

  memset(&stCliRole, 0, sizeof(stCliRole));
  stCliRole.u32Bit = AF_ROLE_CLI;
  stCliRole.pcName = "cli";
  stCliRole.pcToken = acCliTok;
  stCliRole.bInRunFile = DM_TRUE;

  snprintf(acCert, sizeof(acCert), "%s/tls-cert.der", g_acMainDataDir);
  snprintf(acKey, sizeof(acKey), "%s/tls-key.der", g_acMainDataDir);
  memset(&stHttps, 0, sizeof(stHttps));
  stHttps.pcBindAddr = "::";
  stHttps.u16Port = af_store_cfg()->u16Port;
  stHttps.pcCertDerPath = acCert;
  stHttps.pcKeyDerPath = acKey;
  stHttps.pfOnDrain = af__on_https_drain;

  memset(&stCfg, 0, sizeof(stCfg));
  stCfg.pcDataDir = g_acMainDataDir;
  stCfg.pcLockName = "agent.lock";
  stCfg.bDisableLocalHttp = DM_TRUE;
  stCfg.u32DrainMs = 10000u;
  stCfg.pastRole = &stCliRole;
  stCfg.uiRoleCount = 1u;
  stCfg.pstHttps = &stHttps;

  af__fill_modules(astMods);
  memset(&stShellCfg, 0, sizeof(stShellCfg));
  stShellCfg.argc = argc;
  stShellCfg.argv = argv;
  stShellCfg.pstDmCfg = &stCfg;
  stShellCfg.pastMods = astMods;
  stShellCfg.uiModCount = 8u;
  stShellCfg.bNoMenubar = ((NULL != pcNoBar) && ('1' == pcNoBar[0])) ? DM_TRUE : DM_FALSE;
  stShellCfg.pfOnWakeExisting = af__on_wake_existing;
  iRc = af_shell_run_app(&stShellCfg);
  dm_secure_zero(acCliTok, sizeof(acCliTok));
  return iRc;
}

int main(int argc, char **argv)
{
  int iBundleRc = 0;

  if ((argc > 1) && (0 != strcmp(argv[1], "run")) && (0 != strncmp(argv[1], "-psn_", 5u)))
  {
    return af_cli_run(argc, argv);
  }
  if (af__try_bundle_launch(argc, argv, &iBundleRc))
  {
    return iBundleRc;
  }
#ifndef NDEBUG
  (void)af_plat_sc_check_signatures();
#endif
  return af__run_daemon(argc, argv);
}
