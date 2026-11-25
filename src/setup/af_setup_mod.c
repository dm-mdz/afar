/**
 ******************************************************************************
 * @file    af_setup_mod.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   af_setup DM 模块（ModID 23）：12 条向导指令、21 字段 View 推送、todo 事件与管理员脚本执行。
 ******************************************************************************
 */

#define _DARWIN_C_SOURCE
#define DM_LOG_MOD "setup"

#include "af_setup.h"
#include "../af_events.h"
#include "../af_modules.h"
#include "../af_store.h"
#include "../env/af_env.h"
#include "../plat/af_plat.h"
#include "../sidecar/af_sidecar.h"

#include <dm/event.h>
#include <dm/json.h>
#include <dm/link.h>
#include <dm/log.h>
#include <dm/pool.h>
#include <dm/rpc.h>
#include <dm/timer.h>

#include <ctype.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AF_SETUP_CMD_TIMEOUT_MS 120000u
#define AF_SETUP_OUT_CAP        4096u
#define AF_SETUP_TICK_MS        1000u

typedef struct af__setup_cmd_job
{
  char acKind[8];
  char acScript[2560];
  af_setup_cmd_fn pfRunCmd;
  uint32 u32TimeoutMs;
  int iRc;
  int iExit;
  char acOut[AF_SETUP_OUT_CAP];
} af__setup_cmd_job_st;

typedef struct af__setup_ctx
{
  af_setup_push_cb pfPush;
  af_setup_win_cb pfWin;
  af_setup_ops_st stOps;
  char acGo[16];
  char acPairDone[65];
  char acBusy[8];
  char acError[256];
  bool_t bWinOpen;
  bool_t bDraining;
  bool_t bTodoPublished;
  bool_t bPairWasActive;
  uint32 u32LastTodo;
  uint32 u32TickTimerId;
  int iDrainHold;
} af__setup_ctx_st;

static af__setup_ctx_st g_stSetup = {.iDrainHold = -1};

/* ============================ 辅助与 View 序列化 ============================ */

static void af__resolve_exe(dm_out char *pcBuf, dm_in size_t uCap)
{
  char acRaw[PATH_MAX];
  char acReal[PATH_MAX];
  uint32 u32Sz = (uint32)sizeof(acRaw);

  if ((NULL != g_stSetup.stOps.pcExePath) && ('\0' != g_stSetup.stOps.pcExePath[0]))
  {
    snprintf(pcBuf, uCap, "%s", g_stSetup.stOps.pcExePath);
    return;
  }
  if (0 == _NSGetExecutablePath(acRaw, &u32Sz))
  {
    snprintf(pcBuf, uCap, "%s", (NULL != realpath(acRaw, acReal)) ? acReal : acRaw);
    return;
  }
  snprintf(pcBuf, uCap, "/Applications/Afar.app/Contents/MacOS/afar-agent");
}

static void af__fill_view_devices(dm_out af_setup_view_st *pstV, dm_in const af_sc_snap_st *pstSc)
{
  af_device_st astDevs[AF_DEV_MAX];
  uint32 u32Cnt = af_auth_list_devices(astDevs, AF_DEV_MAX);
  time_t tNow = time(NULL);
  struct tm stNow;
  int iYear = 2026;
  uint32 i = 0u;

  if (NULL != localtime_r(&tNow, &stNow))
  {
    iYear = stNow.tm_year + 1900;
  }
  pstV->u32Paired = u32Cnt;
  pstV->u32DevCnt = u32Cnt;
  for (i = 0u; i < u32Cnt; i++)
  {
    af_setup_dev_view_st *pstDv = &pstV->astDevs[i];
    const char *pcMatch =
        ('\0' != astDevs[i].acSidecarName[0]) ? astDevs[i].acSidecarName : astDevs[i].acName;
    snprintf(pstDv->acId, sizeof(pstDv->acId), "%s", astDevs[i].acId);
    snprintf(pstDv->acName, sizeof(pstDv->acName), "%s", astDevs[i].acName);
    af_setup_format_date(astDevs[i].u64PairedAt, iYear, pstDv->acDate, sizeof(pstDv->acDate));
    pstDv->bShowing = (('\0' != pstSc->acShowing[0]) && (0 == strcmp(pstSc->acShowing, pcMatch)))
                          ? DM_TRUE
                          : DM_FALSE;
  }
}

static void af__build_view(dm_out af_setup_view_st *pstV)
{
  const af_state_st *pstSt = af_store_state();
  af_env_snap_st stEnv;
  af_sc_snap_st stSc;
  af_pair_view_st stPair;

  memset(pstV, 0, sizeof(*pstV));
  af_env_get_snapshot(&stEnv);
  af_sidecar_get_snap(&stSc);
  af_auth_get_pair_view(&stPair);

  snprintf(pstV->acVersion, sizeof(pstV->acVersion), "%s", AF_VERSION);
  if ('\0' != pstSt->acName[0])
  {
    snprintf(pstV->acName, sizeof(pstV->acName), "%s", pstSt->acName);
  }
  else
  {
    af_plat_host_name(pstV->acName, sizeof(pstV->acName));
  }
  snprintf(pstV->acCode, sizeof(pstV->acCode), "%s", stPair.acCode);
  pstV->u64PairLeft = stPair.u64LeftSec;
  pstV->u64PairLock = stPair.u64LockSec;
  snprintf(pstV->acPairDone, sizeof(pstV->acPairDone), "%s", g_stSetup.acPairDone);
  snprintf(pstV->acGo, sizeof(pstV->acGo), "%s", g_stSetup.acGo);
  pstV->bDone = pstSt->bSetupDone;
  pstV->bAgent = stEnv.bAgent;
  pstV->bFw = stEnv.bFw;
  pstV->bPower = stEnv.bAutoRestart;
  if ('\0' != stSc.acShowing[0])
  {
    snprintf(pstV->acIpad, sizeof(pstV->acIpad), "%s", stSc.acShowing);
  }
  else if (stSc.u32NearbyCnt > 0u)
  {
    snprintf(pstV->acIpad, sizeof(pstV->acIpad), "%s", stSc.aacNearby[0]);
  }
  snprintf(pstV->acShowing, sizeof(pstV->acShowing), "%s", stSc.acShowing);
  af__fill_view_devices(pstV, &stSc);
  pstV->bFileVault = stEnv.bFileVault;
  pstV->bAutoLogin = stEnv.bAutoLogin;
  pstV->bRemoteLogin = stEnv.bRemoteLogin;
  pstV->bPermSc = stEnv.bPermSc;
  pstV->bPermAx = stEnv.bPermAx;
  snprintf(pstV->acPlan, sizeof(pstV->acPlan), "%s",
           (0 == strcmp(pstSt->acPlan, "fv")) ? "fv" : "auto");
  snprintf(pstV->acBusy, sizeof(pstV->acBusy), "%s", g_stSetup.acBusy);
  snprintf(pstV->acError, sizeof(pstV->acError), "%s", g_stSetup.acError);
}

static char *af__serialize_view(dm_in const af_setup_view_st *pstV)
{
  yyjson_mut_doc *pstDoc = yyjson_mut_doc_new(dm_json_alc());
  yyjson_mut_val *pstRoot = NULL;
  yyjson_mut_val *pstArr = NULL;
  char *pcJson = NULL;
  size_t uLen = 0u;
  uint32 i = 0u;

  if (NULL == pstDoc) { return NULL; }
  pstRoot = yyjson_mut_obj(pstDoc);
  yyjson_mut_doc_set_root(pstDoc, pstRoot);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "version", pstV->acVersion);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "name", pstV->acName);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "code", pstV->acCode);
  yyjson_mut_obj_add_uint(pstDoc, pstRoot, "pair_left", pstV->u64PairLeft);
  yyjson_mut_obj_add_uint(pstDoc, pstRoot, "pair_lock", pstV->u64PairLock);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "pair_done", pstV->acPairDone);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "go", pstV->acGo);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "done", pstV->bDone ? true : false);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "agent", pstV->bAgent ? true : false);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "fw", pstV->bFw ? true : false);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "power", pstV->bPower ? true : false);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "ipad", pstV->acIpad);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "showing", pstV->acShowing);
  yyjson_mut_obj_add_uint(pstDoc, pstRoot, "paired", pstV->u32Paired);
  pstArr = yyjson_mut_arr(pstDoc);
  for (i = 0u; i < pstV->u32DevCnt; i++)
  {
    yyjson_mut_val *pstD = yyjson_mut_obj(pstDoc);
    yyjson_mut_obj_add_str(pstDoc, pstD, "id", pstV->astDevs[i].acId);
    yyjson_mut_obj_add_str(pstDoc, pstD, "name", pstV->astDevs[i].acName);
    yyjson_mut_obj_add_str(pstDoc, pstD, "date", pstV->astDevs[i].acDate);
    yyjson_mut_obj_add_bool(pstDoc, pstD, "showing", pstV->astDevs[i].bShowing ? true : false);
    yyjson_mut_arr_add_val(pstArr, pstD);
  }
  yyjson_mut_obj_add_val(pstDoc, pstRoot, "devices", pstArr);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "filevault", pstV->bFileVault ? true : false);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "auto_login", pstV->bAutoLogin ? true : false);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "remote_login", pstV->bRemoteLogin ? true : false);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "perm_sc", pstV->bPermSc ? true : false);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "perm_ax", pstV->bPermAx ? true : false);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "plan", pstV->acPlan);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "busy", pstV->acBusy);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "error", pstV->acError);
  pcJson = dm_json_write_mut(pstDoc, &uLen);
  yyjson_mut_doc_free(pstDoc);
  return pcJson;
}

static void af__publish_todo_and_push(dm_in bool_t bForcePush)
{
  af_setup_view_st stV;
  uint32 u32Todo = 0u;

  af__build_view(&stV);
  g_stSetup.bPairWasActive = ((stV.u64PairLeft > 0u) || (stV.u64PairLock > 0u)) ? DM_TRUE : DM_FALSE;
  u32Todo = af_setup_calc_todo(&stV);
  if ((!g_stSetup.bTodoPublished) || (u32Todo != g_stSetup.u32LastTodo))
  {
    af_ev_setup_todo_st stEv = {.u32TodoCount = u32Todo};
    g_stSetup.bTodoPublished = DM_TRUE;
    g_stSetup.u32LastTodo = u32Todo;
    (void)dm_event_publish(AF_EV_SETUP_TODO, &stEv, (uint32)sizeof(stEv));
  }
  if ((NULL != g_stSetup.pfPush) && (bForcePush || g_stSetup.bWinOpen))
  {
    char *pcJson = af__serialize_view(&stV);
    if (NULL != pcJson)
    {
      g_stSetup.pfPush(pcJson);
      dm_json_free_str(pcJson);
    }
  }
}

static void af__set_win(dm_in bool_t bShow)
{
  g_stSetup.bWinOpen = bShow;
  if (bShow) { af_env_start_fast_perm_poll(120000u); }
  if (NULL != g_stSetup.pfWin) { g_stSetup.pfWin(bShow); }
}

static void af__env_probe_now(void)
{
  if (NULL != g_stSetup.stOps.pfEnvProbeNow)
  {
    g_stSetup.stOps.pfEnvProbeNow();
  }
  else
  {
    af_env_probe_now();
  }
}

/* ============================ 管理员脚本后台任务 ============================ */

static void af__cmd_work(dm_io void *pvArg)
{
  af__setup_cmd_job_st *pstJob = (af__setup_cmd_job_st *)pvArg;
  char acAsArg[3200];
  char acExpr[3400];
  const char *apcArgv[4];

  if (DM_SUCCESS != af_setup_applescript_str(pstJob->acScript, acAsArg, sizeof(acAsArg)))
  {
    pstJob->iRc = DM_TOO_BIG;
    return;
  }
  snprintf(acExpr, sizeof(acExpr), "do shell script %s with administrator privileges", acAsArg);
  apcArgv[0] = "/usr/bin/osascript";
  apcArgv[1] = "-e";
  apcArgv[2] = acExpr;
  apcArgv[3] = NULL;
  pstJob->iRc = pstJob->pfRunCmd(apcArgv, NULL, &g_bAfQuitting, pstJob->acOut,
                                 sizeof(pstJob->acOut), pstJob->u32TimeoutMs, &pstJob->iExit);
}

static void af__trim_err(dm_in const char *pcSrc, dm_out char *pcDst, dm_in size_t uCap)
{
  const char *pcS = pcSrc;
  const char *pcE = NULL;
  size_t uLen = 0u;

  if ((NULL == pcDst) || (0u == uCap)) { return; }
  pcDst[0] = '\0';
  if (NULL == pcSrc) { return; }
  while (('\0' != *pcS) && (0 != isspace((unsigned char)*pcS))) { pcS++; }
  pcE = pcS + strlen(pcS);
  while ((pcE > pcS) && (0 != isspace((unsigned char)pcE[-1]))) { pcE--; }
  uLen = (size_t)(pcE - pcS);
  if (uLen >= uCap) { uLen = uCap - 1u; }
  if (uLen > 0u) { memcpy(pcDst, pcS, uLen); }
  pcDst[uLen] = '\0';
}

static void af__cmd_after(dm_io void *pvArg, dm_in int iStatus)
{
  af__setup_cmd_job_st *pstJob = (af__setup_cmd_job_st *)pvArg;
  char acFail[256];

  g_stSetup.acBusy[0] = '\0';
  if (g_stSetup.bDraining || atomic_load(&g_bAfQuitting))
  {
    dm_free_tag(pstJob);
    if (g_stSetup.iDrainHold >= 0)
    {
      dm_drain_release(g_stSetup.iDrainHold);
      g_stSetup.iDrainHold = -1;
    }
    return;
  }
  if ((0 != iStatus) || (DM_SUCCESS != pstJob->iRc) || (0 != pstJob->iExit))
  {
    if (NULL != strstr(pstJob->acOut, "-128"))
    {
      snprintf(g_stSetup.acError, sizeof(g_stSetup.acError), "取消了");
    }
    else if (af_setup_parse_fail(pstJob->acOut, acFail, sizeof(acFail)))
    {
      snprintf(g_stSetup.acError, sizeof(g_stSetup.acError), "%s", acFail);
    }
    else
    {
      af__trim_err(pstJob->acOut, g_stSetup.acError, sizeof(g_stSetup.acError));
      if ('\0' == g_stSetup.acError[0])
      {
        snprintf(g_stSetup.acError, sizeof(g_stSetup.acError), "配置失败");
      }
    }
  }
  else if (af_setup_parse_fail(pstJob->acOut, acFail, sizeof(acFail)))
  {
    snprintf(g_stSetup.acError, sizeof(g_stSetup.acError), "%s", acFail);
  }
  else
  {
    g_stSetup.acError[0] = '\0';
  }
  dm_free_tag(pstJob);
  af__env_probe_now();
  af__publish_todo_and_push(DM_TRUE);
}

static void af__start_admin_cmd(dm_in const char *pcKind)
{
  af__setup_cmd_job_st *pstJob = NULL;
  char acExe[PATH_MAX];
  int iRc = DM_SUCCESS;

  if (('\0' != g_stSetup.acBusy[0]) || g_stSetup.bDraining)
  {
    return;
  }
  pstJob = (af__setup_cmd_job_st *)dm_calloc_tag(1u, sizeof(*pstJob), DM_MEM_TAG_JSON);
  if (NULL == pstJob)
  {
    return;
  }
  snprintf(pstJob->acKind, sizeof(pstJob->acKind), "%s", pcKind);
  if (0 == strcmp(pcKind, "one"))
  {
    af__resolve_exe(acExe, sizeof(acExe));
    iRc = af_setup_build_one_script(acExe, pstJob->acScript, sizeof(pstJob->acScript));
  }
  else
  {
    iRc = af_setup_build_ssh_script(pstJob->acScript, sizeof(pstJob->acScript));
  }
  if (DM_SUCCESS != iRc)
  {
    dm_free_tag(pstJob);
    snprintf(g_stSetup.acError, sizeof(g_stSetup.acError), "脚本构造失败");
    af__publish_todo_and_push(DM_TRUE);
    return;
  }
  pstJob->pfRunCmd = (NULL != g_stSetup.stOps.pfRunCmd) ? g_stSetup.stOps.pfRunCmd
                                                        : af_plat_run_cmd_cancellable;
  pstJob->u32TimeoutMs = (0u != g_stSetup.stOps.u32CmdTimeoutMs) ? g_stSetup.stOps.u32CmdTimeoutMs
                                                                 : AF_SETUP_CMD_TIMEOUT_MS;
  snprintf(g_stSetup.acBusy, sizeof(g_stSetup.acBusy), "%s", pcKind);
  g_stSetup.acError[0] = '\0';
  af__publish_todo_and_push(DM_TRUE);
  if (DM_SUCCESS != dm_pool_submit(DM_LANE_HEAVY, af__cmd_work, af__cmd_after, pstJob))
  {
    g_stSetup.acBusy[0] = '\0';
    snprintf(g_stSetup.acError, sizeof(g_stSetup.acError), "后台任务提交失败");
    dm_free_tag(pstJob);
    af__publish_todo_and_push(DM_TRUE);
  }
}

/* ============================ 12 条 setup.html 指令处理 ============================ */

static void af__open_url_on_main(dm_io void *pvCtx)
{
  char *pcUrl = (char *)pvCtx;
  if (NULL != pcUrl)
  {
    if (NULL != strstr(pcUrl, "Privacy_ScreenCapture")) { af_plat_request_screen_capture(); }
    else if (NULL != strstr(pcUrl, "Privacy_Accessibility")) { af_plat_request_accessibility(); }
    (void)af_plat_open_url(pcUrl);
    dm_free_tag(pcUrl);
  }
}

static void af__handle_open_cmd(dm_in const char *pcWhat)
{
  const char *pcUrl = af_setup_settings_url(pcWhat);
  if (NULL == pcUrl) { return; }
  if ((0 == strcmp(pcWhat, "screen_capture")) || (0 == strcmp(pcWhat, "accessibility")))
  {
    af_env_start_fast_perm_poll(120000u);
  }
  if (NULL != g_stSetup.stOps.pfOpenUrl)
  {
    (void)g_stSetup.stOps.pfOpenUrl(pcUrl);
    return;
  }
  {
    size_t uLen = strlen(pcUrl) + 1u;
    char *pcDup = (char *)dm_malloc_tag(uLen, DM_MEM_TAG_JSON);
    if (NULL != pcDup)
    {
      af_main_fn pfMain = af__open_url_on_main;
      memcpy(pcDup, pcUrl, uLen);
      if (NULL != g_stSetup.stOps.pfRunOnMain)
      {
        g_stSetup.stOps.pfRunOnMain(pfMain, pcDup);
      }
      else
      {
        af_plat_run_on_main(pfMain, pcDup);
      }
    }
  }
}

static void af__handle_unpair_cmd(dm_in const char *pcId)
{
  yyjson_mut_doc *pstDoc = NULL;
  yyjson_mut_val *pstRoot = NULL;
  char *pcBody = NULL;
  size_t uLen = 0u;

  if ((NULL == pcId) || ('\0' == pcId[0]) || (strlen(pcId) > 40u))
  {
    return;
  }
  pstDoc = yyjson_mut_doc_new(dm_json_alc());
  if (NULL == pstDoc) { return; }
  pstRoot = yyjson_mut_obj(pstDoc);
  yyjson_mut_doc_set_root(pstDoc, pstRoot);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "id", pcId);
  pcBody = dm_json_write_mut(pstDoc, &uLen);
  yyjson_mut_doc_free(pstDoc);
  if (NULL != pcBody)
  {
    (void)dm_queue_post_async(AF_LINK_UI_UNPAIR, pcBody, (uint32)uLen);
    dm_json_free_str(pcBody);
  }
}

static void af__dispatch_setup_cmd(dm_in const char *pcCmd, dm_in yyjson_val *pstRoot)
{
  if (0 == strcmp(pcCmd, "ready"))
  {
    af__publish_todo_and_push(DM_TRUE);
  }
  else if (0 == strcmp(pcCmd, "plan"))
  {
    const char *pcPlan = yyjson_get_str(yyjson_obj_get(pstRoot, "plan"));
    if ((NULL != pcPlan) && ((0 == strcmp(pcPlan, "auto")) || (0 == strcmp(pcPlan, "fv"))))
    {
      snprintf(af_store_state_mut()->acPlan, sizeof(af_store_state_mut()->acPlan), "%s", pcPlan);
      (void)af_store_save_async(NULL, NULL);
      af__publish_todo_and_push(DM_TRUE);
    }
  }
  else if (0 == strcmp(pcCmd, "done"))
  {
    af_store_state_mut()->bSetupDone = DM_TRUE;
    (void)af_store_save_async(NULL, NULL);
    af__publish_todo_and_push(DM_TRUE);
  }
  else if (0 == strcmp(pcCmd, "again"))
  {
    af_store_state_mut()->bSetupDone = DM_FALSE;
    g_stSetup.acError[0] = '\0';
    (void)af_store_save_async(NULL, NULL);
    af__publish_todo_and_push(DM_TRUE);
  }
  else if (0 == strcmp(pcCmd, "pair_open"))
  {
    g_stSetup.acPairDone[0] = '\0';
    (void)dm_queue_post_async(AF_LINK_UI_PAIR_OPEN, NULL, 0u);
  }
  else if (0 == strcmp(pcCmd, "pair_close"))
  {
    (void)dm_queue_post_async(AF_LINK_UI_PAIR_CLOSE, NULL, 0u);
  }
  else if (0 == strcmp(pcCmd, "unpair"))
  {
    af__handle_unpair_cmd(yyjson_get_str(yyjson_obj_get(pstRoot, "id")));
  }
  else if (0 == strcmp(pcCmd, "went"))
  {
    g_stSetup.acGo[0] = '\0';
  }
  else if (0 == strcmp(pcCmd, "open"))
  {
    af__handle_open_cmd(yyjson_get_str(yyjson_obj_get(pstRoot, "what")));
  }
  else if ((0 == strcmp(pcCmd, "one")) || (0 == strcmp(pcCmd, "ssh")))
  {
    af__start_admin_cmd(pcCmd);
  }
  else if (0 == strcmp(pcCmd, "close"))
  {
    (void)dm_queue_post_async(AF_LINK_UI_PAIR_CLOSE, NULL, 0u);
    af__set_win(DM_FALSE);
  }
}

/* ============================ LinkID / RPC / 事件回调 ============================ */

static int af__on_ui_setup_msg(dm_io void *pvArg)
{
  dm_link_msg_st *pstMsg = (dm_link_msg_st *)pvArg;
  yyjson_doc *pstDoc = NULL;
  yyjson_val *pstRoot = NULL;
  const char *pcCmd = NULL;
  int iErr = 0;

  if ((NULL == pstMsg) || (NULL == pstMsg->pu8Data) || (0u == pstMsg->u32Len))
  {
    return DM_SUCCESS;
  }
  pstDoc = dm_json_read((const char *)pstMsg->pu8Data, pstMsg->u32Len, 4096u, &iErr);
  if (NULL == pstDoc)
  {
    return DM_SUCCESS;
  }
  pstRoot = yyjson_doc_get_root(pstDoc);
  pcCmd = yyjson_get_str(yyjson_obj_get(pstRoot, "cmd"));
  if (NULL != pcCmd)
  {
    af__dispatch_setup_cmd(pcCmd, pstRoot);
  }
  yyjson_doc_free(pstDoc);
  return DM_SUCCESS;
}

static void af__apply_go_target(dm_in const char *pcTarget, dm_in bool_t bReset)
{
  if (bReset || ((NULL != pcTarget) && ((0 == strcmp(pcTarget, "w1"))
                                        || (0 == strcmp(pcTarget, "reset")))))
  {
    af_store_state_mut()->bSetupDone = DM_FALSE;
    g_stSetup.acError[0] = '\0';
    (void)af_store_save_async(NULL, NULL);
    snprintf(g_stSetup.acGo, sizeof(g_stSetup.acGo), "status");
  }
  else if ((NULL != pcTarget) && ('\0' != pcTarget[0]))
  {
    snprintf(g_stSetup.acGo, sizeof(g_stSetup.acGo), "%s", pcTarget);
    if (0 == strcmp(pcTarget, "pair"))
    {
      g_stSetup.acPairDone[0] = '\0';
      (void)dm_queue_post_async(AF_LINK_UI_PAIR_OPEN, NULL, 0u);
    }
  }
  af__set_win(DM_TRUE);
  af__env_probe_now();
  af__publish_todo_and_push(DM_TRUE);
}

static int af__on_ui_setup_go(dm_io void *pvArg)
{
  dm_link_msg_st *pstMsg = (dm_link_msg_st *)pvArg;
  char acTarget[32] = {0};
  bool_t bReset = DM_FALSE;

  if ((NULL != pstMsg) && (NULL != pstMsg->pu8Data) && (pstMsg->u32Len > 0u))
  {
    const char *pcRaw = (const char *)pstMsg->pu8Data;
    if ('{' == pcRaw[0])
    {
      int iErr = 0;
      yyjson_doc *pstDoc = dm_json_read(pcRaw, pstMsg->u32Len, 1024u, &iErr);
      if (NULL != pstDoc)
      {
        yyjson_val *pstRoot = yyjson_doc_get_root(pstDoc);
        const char *pcG = yyjson_get_str(yyjson_obj_get(pstRoot, "go"));
        yyjson_val *pstR = yyjson_obj_get(pstRoot, "reset");
        if (NULL != pcG) { snprintf(acTarget, sizeof(acTarget), "%s", pcG); }
        if (yyjson_is_bool(pstR) && yyjson_get_bool(pstR)) { bReset = DM_TRUE; }
        yyjson_doc_free(pstDoc);
      }
    }
    else
    {
      size_t uCopy = pstMsg->u32Len;
      while ((uCopy > 0u) && ('\0' == pcRaw[uCopy - 1u])) { uCopy--; }
      if (uCopy >= sizeof(acTarget)) { uCopy = sizeof(acTarget) - 1u; }
      memcpy(acTarget, pcRaw, uCopy);
      acTarget[uCopy] = '\0';
    }
  }
  af__apply_go_target(acTarget, bReset);
  return DM_SUCCESS;
}

static int af__on_cli_win_open(dm_io void *pvArg)
{
  dm_link_msg_st *pstMsg = (dm_link_msg_st *)pvArg;
  dm_reply_st stReply;
  static const char acOk[] = "{\"ok\":true}";

  af__apply_go_target("status", DM_FALSE);
  if (DM_SUCCESS == dm_reply_take(pstMsg, &stReply))
  {
    (void)dm_reply_send(&stReply, DM_SUCCESS, acOk, (uint32)(sizeof(acOk) - 1u));
  }
  return DM_SUCCESS;
}

static int af__on_setup_ev(dm_io void *pvArg)
{
  const dm_link_msg_st *pstMsg = (const dm_link_msg_st *)pvArg;

  if ((NULL != pstMsg) && (AF_EV_AUTH_CHANGED == pstMsg->u64Tag)
      && (NULL != pstMsg->pu8Data) && (pstMsg->u32Len >= sizeof(af_ev_auth_st)))
  {
    const af_ev_auth_st *pstAuth = (const af_ev_auth_st *)pstMsg->pu8Data;
    if (AF_AUTH_EV_PAIRED == pstAuth->eKind)
    {
      snprintf(g_stSetup.acPairDone, sizeof(g_stSetup.acPairDone), "%s", pstAuth->acDevName);
    }
  }
  af__publish_todo_and_push(DM_FALSE);
  return DM_SUCCESS;
}

static int af__on_setup_tick(dm_io void *pvArg)
{
  af_pair_view_st stPair;

  (void)pvArg;
  if (!g_stSetup.bWinOpen)
  {
    return DM_SUCCESS;
  }
  af_auth_get_pair_view(&stPair);
  if ((stPair.u64LeftSec > 0u) || (stPair.u64LockSec > 0u))
  {
    g_stSetup.bPairWasActive = DM_TRUE;
    af__publish_todo_and_push(DM_TRUE);
  }
  else if (g_stSetup.bPairWasActive)
  {
    g_stSetup.bPairWasActive = DM_FALSE;
    af__publish_todo_and_push(DM_TRUE);
  }
  return DM_SUCCESS;
}

static int af__on_setup_ready(dm_io void *pvArg)
{
  (void)pvArg;
  (void)dm_timer_add(AF_SETUP_TICK_MS, AF_SETUP_TICK_MS, DM_MOD_LINKID(AF_MOD_SETUP, 0x04u), 0u,
                     &g_stSetup.u32TickTimerId);
  af__publish_todo_and_push(DM_FALSE);
  return DM_SUCCESS;
}

/* ============================ 公开装配与 DM 模块生命周期 ============================ */

void af_setup_set_ui_cb(dm_in af_setup_push_cb pfPush, dm_in af_setup_win_cb pfWin)
{
  g_stSetup.pfPush = pfPush;
  g_stSetup.pfWin = pfWin;
}

void af_setup_set_ui_cbs(dm_in af_setup_push_cb pfPush, dm_in af_setup_win_cb pfWin)
{
  af_setup_set_ui_cb(pfPush, pfWin);
}

void af_setup_set_ops(dm_in const af_setup_ops_st *pstOps)
{
  if (NULL == pstOps)
  {
    memset(&g_stSetup.stOps, 0, sizeof(g_stSetup.stOps));
  }
  else
  {
    g_stSetup.stOps = *pstOps;
  }
}

static int af__setup_init(void)
{
  static const dm_iface_st stCliWinOpen = {
      .uiLinkID = AF_LINK_CLI_WIN_OPEN,
      .pcName = "setup.window_open",
      .u32Roles = AF_ROLE_CLI,
      .u32Flags = DM_IFACE_MUTATE,
      .pcDesc = "Open setup window and push current view",
      .pcArgs = NULL
  };
  uint uiTickLink = DM_MOD_LINKID(AF_MOD_SETUP, 0x04u);

  g_stSetup.acGo[0] = '\0';
  g_stSetup.acPairDone[0] = '\0';
  g_stSetup.acBusy[0] = '\0';
  g_stSetup.acError[0] = '\0';
  g_stSetup.bWinOpen = DM_FALSE;
  g_stSetup.bDraining = DM_FALSE;
  g_stSetup.bTodoPublished = DM_FALSE;
  g_stSetup.bPairWasActive = DM_FALSE;
  g_stSetup.u32LastTodo = 0u;
  g_stSetup.u32TickTimerId = 0u;
  g_stSetup.iDrainHold = -1;

  if ((DM_SUCCESS != dm_link_register_handler(AF_LINK_UI_SETUP_MSG, af__on_ui_setup_msg, NULL, NULL))
      || (DM_SUCCESS != dm_link_register_handler(AF_LINK_CLI_WIN_OPEN, af__on_cli_win_open, NULL,
                                                 NULL))
      || (DM_SUCCESS != dm_iface_register(&stCliWinOpen))
      || (DM_SUCCESS != dm_link_register_handler(AF_LINK_UI_SETUP_GO, af__on_ui_setup_go, NULL,
                                                 NULL))
      || (DM_SUCCESS != dm_link_register_handler(uiTickLink, af__on_setup_tick, NULL, NULL))
      || (DM_SUCCESS != dm_event_subscribe(DM_EV_READY, af__on_setup_ready, NULL, NULL))
      || (DM_SUCCESS != dm_event_subscribe(AF_EV_ENV_UPDATED, af__on_setup_ev, NULL, NULL))
      || (DM_SUCCESS != dm_event_subscribe(AF_EV_AUTH_CHANGED, af__on_setup_ev, NULL, NULL))
      || (DM_SUCCESS != dm_event_subscribe(AF_EV_SIDECAR_CHANGED, af__on_setup_ev, NULL, NULL)))
  {
    return DM_FAILD;
  }
  return DM_SUCCESS;
}

static void af__setup_drain(dm_io dm_drain_st *pstDrain)
{
  g_stSetup.bDraining = DM_TRUE;
  if (0u != g_stSetup.u32TickTimerId)
  {
    (void)dm_timer_cancel(g_stSetup.u32TickTimerId);
    g_stSetup.u32TickTimerId = 0u;
  }
  if ('\0' != g_stSetup.acBusy[0])
  {
    atomic_store(&g_bAfQuitting, DM_TRUE);
    g_stSetup.iDrainHold = dm_drain_hold(pstDrain);
  }
}

static void af__setup_fini(void)
{
  (void)dm_event_unsubscribe(AF_EV_SIDECAR_CHANGED, af__on_setup_ev);
  (void)dm_event_unsubscribe(AF_EV_AUTH_CHANGED, af__on_setup_ev);
  (void)dm_event_unsubscribe(AF_EV_ENV_UPDATED, af__on_setup_ev);
  (void)dm_event_unsubscribe(DM_EV_READY, af__on_setup_ready);
  g_stSetup.acGo[0] = '\0'; g_stSetup.acPairDone[0] = '\0';
  g_stSetup.acBusy[0] = '\0'; g_stSetup.acError[0] = '\0';
  g_stSetup.bWinOpen = DM_FALSE; g_stSetup.bDraining = DM_FALSE;
  g_stSetup.bTodoPublished = DM_FALSE; g_stSetup.bPairWasActive = DM_FALSE;
  g_stSetup.u32TickTimerId = 0u; g_stSetup.iDrainHold = -1;
}

const dm_module_st g_stAfSetupMod = {
    .u16ModID = AF_MOD_SETUP,
    .pcName = "af_setup",
    .pfInit = af__setup_init,
    .pfDrain = af__setup_drain,
    .pfFini = af__setup_fini,
    .pastErrMap = NULL
};

const dm_module_st *af_setup_module(void)
{
  return &g_stAfSetupMod;
}
