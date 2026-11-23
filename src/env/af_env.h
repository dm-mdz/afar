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
#define AF_SSH_KEY_LEN  1024u

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
} af_env_snap_st;

extern const dm_module_st g_stAfEnvMod;
const dm_module_st *af_env_module(void);

void af_env_get_snapshot(out af_env_snap_st *pstOut);
void af_env_probe_now(void);

/* ---- 纯解析辅助（供单元测试与内部共用） ---- */
bool_t af_env_parse_agent_plist(in const char *pcXml, in const char *pcExePath);
bool_t af_env_parse_launchctl_ok(in int iExitCode, in const char *pcOut);
bool_t af_env_parse_fw_global_off(in int iExitCode, in const char *pcOut);
bool_t af_env_parse_fw_app_permitted(in int iExitCode, in const char *pcOut);
bool_t af_env_parse_pmset_autorestart(in const char *pcOut);
bool_t af_env_parse_filevault(in int iExitCode, in const char *pcOut);
bool_t af_env_parse_autologin(in int iExitCode, in const char *pcOut);
uint32 af_env_parse_ssh_pub(in const char *pcText, out char aacOut[][AF_SSH_KEY_LEN],
                            io uint32 *pu32Cnt, in uint32 u32Cap);

/* ---- 可选测试注入钩子（NULL 恢复生产默认） ---- */
typedef int (*af_env_cmd_fn)(in const char *const *ppcArgv, in const _Atomic bool_t *pbCancel,
                             out char *pcOut, in size_t uOutCap, in uint32 u32TimeoutMs,
                             out int *piExitCode);
typedef bool_t (*af_env_tcp_probe_fn)(in const char *pcIp, in uint16 u16Port,
                                      in uint32 u32TimeoutMs, in const _Atomic bool_t *pbCancel);

typedef struct af_env_hooks
{
  af_env_cmd_fn pfRunCmd;
  af_env_tcp_probe_fn pfTcpProbe;
  const char *pcSshDir;
  const char *pcPlistPath;
  const char *pcExePath;
} af_env_hooks_st;

void af_env_set_hooks(in const af_env_hooks_st *pstHooks);

#ifdef __cplusplus
}
#endif

#endif /* AF_ENV_H */
