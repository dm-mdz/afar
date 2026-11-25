/**
 ******************************************************************************
 * @file    af_setup.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   业务层 af_setup（ModID 23）公共契约：向导 View、管理员脚本与测试钩子（DD-05 §2）。
 ******************************************************************************
 */
#ifndef AF_SETUP_H
#define AF_SETUP_H

#include <dm/module.h>
#include <stdatomic.h>
#include "../af_ui_ipc.h"
#include "../auth/af_auth.h"
#include "../plat/af_plat.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct af_setup_dev_view
{
  char acId[37];
  char acName[65];
  char acDate[32]; /* "11 月 9 日" 或 "2025 年 11 月 9 日" */
  bool_t bShowing;
} af_setup_dev_view_st;

/* 21 字段 View（对齐 shell/ui/setup.html 与 DD-05 §2.1） */
typedef struct af_setup_view
{
  char acVersion[32];
  char acName[65];
  char acCode[7];
  uint64 u64PairLeft;
  uint64 u64PairLock;
  char acPairDone[65];
  char acGo[16]; /* "pair" | "devices" | "status" | "" */
  bool_t bDone;
  bool_t bAgent;
  bool_t bFw;
  bool_t bPower;
  char acIpad[65];
  char acShowing[65];
  uint32 u32Paired;
  uint32 u32DevCnt;
  af_setup_dev_view_st astDevs[AF_DEV_MAX];
  bool_t bFileVault;
  bool_t bAutoLogin;
  bool_t bRemoteLogin;
  char acPlan[8]; /* "auto" | "fv" */
  char acBusy[8]; /* "one" | "ssh" | "" */
  char acError[256];
  bool_t bPermSc; /* 屏幕录制权限 */
  bool_t bPermAx; /* 辅助功能权限 */
} af_setup_view_st;

/* 可选测试注入钩子（NULL 成员回退真实 af_plat_* / af_env_*） */
typedef int (*af_setup_cmd_fn)(dm_in const char *const *ppcArgv, dm_in const char *pcStdin,
                               dm_in const _Atomic bool_t *pbCancel, dm_out char *pcOut,
                               dm_in size_t uOutCap, dm_in uint32 u32TimeoutMs,
                               dm_out int *piExitCode);

typedef struct af_setup_ops
{
  af_setup_cmd_fn pfRunCmd;
  int (*pfOpenUrl)(dm_in const char *pcUrl);
  void (*pfRunOnMain)(dm_in af_main_fn pfFn, dm_io void *pvCtx);
  void (*pfEnvProbeNow)(void);
  const char *pcExePath;
  uint32 u32CmdTimeoutMs;
} af_setup_ops_st;

extern const dm_module_st g_stAfSetupMod;
const dm_module_st *af_setup_module(void);

/* 由 src/main.c 注入壳回调（提供双名兼容 DD-05 与装配调用） */
void af_setup_set_ui_cb(dm_in af_setup_push_cb pfPush, dm_in af_setup_win_cb pfWin);
void af_setup_set_ui_cbs(dm_in af_setup_push_cb pfPush, dm_in af_setup_win_cb pfWin);
void af_setup_set_ops(dm_in const af_setup_ops_st *pstOps);

/* 纯逻辑单测接口 */
uint32 af_setup_calc_todo(dm_in const af_setup_view_st *pstV);
int af_setup_sh_quote(dm_in const char *pcStr, dm_out char *pcBuf, dm_in size_t uCap);
int af_setup_applescript_str(dm_in const char *pcStr, dm_out char *pcBuf, dm_in size_t uCap);
int af_setup_build_one_script(dm_in const char *pcExePath, dm_out char *pcBuf, dm_in size_t uCap);
int af_setup_build_ssh_script(dm_out char *pcBuf, dm_in size_t uCap);
bool_t af_setup_parse_fail(dm_in const char *pcOut, dm_out char *pcErrOut, dm_in size_t uErrCap);
const char *af_setup_settings_url(dm_in const char *pcWhat);
void af_setup_format_date(dm_in uint64 u64EpochSec, dm_in int iCurYear, dm_out char *pcBuf,
                          dm_in size_t uCap);

#ifdef __cplusplus
}
#endif

#endif /* AF_SETUP_H */
