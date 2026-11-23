/**
 ******************************************************************************
 * @file    af_shell_app.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   B2 [NSApp run] 唯一主控壳入口与 DMG 自升级纯逻辑契约（DD-01 §3）。
 ******************************************************************************
 */
#ifndef AF_SHELL_APP_H
#define AF_SHELL_APP_H

#include <dm/module.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct af_shell_run_cfg
{
  int argc;
  char **argv;
  const dm_cfg_st *pstDmCfg;
  const dm_module_st *pastMods;
  uint uiModCount;
  bool_t bNoMenubar;
  void (*pfOnWakeExisting)(void);
} af_shell_run_cfg_st;

typedef enum af_bundle_action
{
  AF_BUNDLE_ACT_NONE = 0,
  AF_BUNDLE_ACT_DRAG_PROMPT,
  AF_BUNDLE_ACT_UPGRADED,
  AF_BUNDLE_ACT_SAME_OR_OLDER,
  AF_BUNDLE_ACT_UPGRADE_FAILED,
  AF_BUNDLE_ACT_INSTALLED_AND_WOKEN
} af_bundle_action_et;

typedef struct af_bundle_ops
{
  bool_t (*pfIsRunning)(void *pvCtx, char *pcVerOut, size_t uCap);
  int (*pfReplaceApp)(void *pvCtx, const char *pcSrcApp, const char *pcDstApp);
  void (*pfOpenApp)(void *pvCtx, const char *pcDstApp);
  void (*pfShowDragAlert)(void *pvCtx);
  void (*pfShowSameVerAlert)(void *pvCtx, const char *pcCurVer);
  int (*pfEnsureInstalled)(void *pvCtx, const char *pcExePath);
  int (*pfWakeRunning)(void *pvCtx);
} af_bundle_ops_st;

typedef struct af_bundle_launch_cb
{
  int (*pfEnsureInstalled)(in const char *pcExePath);
  int (*pfWakeRunning)(void);
} af_bundle_launch_cb_st;

bool_t af_shell_ver_newer(in const char *pcNewVer, in const char *pcCurVer);
bool_t af_shell_in_applications(in const char *pcAppPath);
bool_t af_shell_find_app_bundle(in const char *pcExePath, out char *pcAppOut, in size_t uCap);
af_bundle_action_et af_shell_launch_bundle_ex(in const char *pcAppPath, in const char *pcExePath,
                                              in const char *pcNewVer,
                                              in const af_bundle_ops_st *pstOps, io void *pvCtx);
af_bundle_action_et af_shell_launch_bundle(in const char *pcAppPath, in const char *pcExePath,
                                           in const char *pcNewVer,
                                           in const af_bundle_launch_cb_st *pstCb);
int af_shell_run_app(in const af_shell_run_cfg_st *pstCfg);

#ifdef __cplusplus
}
#endif

#endif /* AF_SHELL_APP_H */
