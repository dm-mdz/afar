/**
 ******************************************************************************
 * @file    af_env.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   服务层 af_env 6 项系统环境只读探测公共契约头（DD-02 §4）。
 ******************************************************************************
 */
#ifndef AF_ENV_H
#define AF_ENV_H

#include <dm/module.h>
#include <dm/type.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define AF_SSH_KEYS_MAX 6u
#define AF_SSH_KEY_LEN 1024u

typedef struct af_env_snap
{
  bool_t bReady;
  bool_t bAgent;       /* ~/Library/LaunchAgents/com.afar.agent.plist 指向当前程序且加载 */
  bool_t bFw;          /* 防火墙关闭或已放行当前程序 */
  bool_t bAutoRestart; /* pmset -g autorestart == 1 */
  bool_t bFileVault;   /* fdesetup isactive 退出码 0 且输出 true */
  bool_t bAutoLogin;   /* com.apple.loginwindow autoLoginUser 非空 */
  bool_t bRemoteLogin; /* TCP 127.0.0.1:22 可连（2s 超时） */
  char acUser[64];     /* 当前 macOS 短用户名 */
  uint32 u32SshKeyCnt;
  char aacSshKeys[AF_SSH_KEYS_MAX][AF_SSH_KEY_LEN]; /* OpenSSH 公钥前两段 type base64 */
  bool_t bPermSc;                                   /* 屏幕录制权限 */
  bool_t bPermAx;                                   /* 辅助功能权限 */
} af_env_snap_st;

extern const dm_module_st g_stAfEnvMod;
const dm_module_st *af_env_module(void);

void af_env_get_snapshot(dm_out af_env_snap_st *pstOut);
void af_env_probe_now(void);
void af_env_start_fast_perm_poll(dm_in uint32 u32WindowMs);

/* ---- 纯解析辅助（供单元测试与内部共用） ---- */
bool_t af_env_parse_agent_plist(dm_in const char *pcXml, dm_in const char *pcExePath);
bool_t af_env_parse_launchctl_ok(dm_in int iExitCode, dm_in const char *pcOut);
bool_t af_env_parse_fw_global_off(dm_in int iExitCode, dm_in const char *pcOut);
bool_t af_env_parse_fw_app_permitted(dm_in int iExitCode, dm_in const char *pcOut);
bool_t af_env_parse_pmset_autorestart(dm_in const char *pcOut);
bool_t af_env_parse_filevault(dm_in int iExitCode, dm_in const char *pcOut);
bool_t af_env_parse_autologin(dm_in int iExitCode, dm_in const char *pcOut);
uint32 af_env_parse_ssh_pub(dm_in const char *pcText, dm_out char aacOut[][AF_SSH_KEY_LEN],
                            dm_io uint32 *pu32Cnt, dm_in uint32 u32Cap);

/* ---- 可选测试注入钩子（NULL 恢复生产默认） ---- */
typedef int (*af_env_cmd_fn)(dm_in const char *const *ppcArgv, dm_in const _Atomic bool_t *pbCancel,
                             dm_out char *pcOut, dm_in size_t uOutCap, dm_in uint32 u32TimeoutMs,
                             dm_out int *piExitCode);
typedef bool_t (*af_env_tcp_probe_fn)(dm_in const char *pcIp, dm_in uint16 u16Port,
                                      dm_in uint32 u32TimeoutMs,
                                      dm_in const _Atomic bool_t *pbCancel);

typedef struct af_env_hooks
{
  af_env_cmd_fn pfRunCmd;
  af_env_tcp_probe_fn pfTcpProbe;
  const char *pcSshDir;
  const char *pcPlistPath;
  const char *pcExePath;
  bool_t (*pfPermSc)(void);
  bool_t (*pfPermAx)(void);
  uint32 u32FastPollMs; /* 0 = 默认 2500ms */
} af_env_hooks_st;

void af_env_set_hooks(dm_in const af_env_hooks_st *pstHooks);

#ifdef __cplusplus
}
#endif

#endif /* AF_ENV_H */
