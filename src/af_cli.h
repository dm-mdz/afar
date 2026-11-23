/**
 ******************************************************************************
 * @file    af_cli.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   afar-agent 命令行子命令分发、安装卸载与本机 dmc 协同契约（DD-05 §3）。
 ******************************************************************************
 */
#ifndef AF_CLI_H
#define AF_CLI_H

#include <dm/type.h>
#include "plat/af_plat.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef int (*af_cli_cmd_fn)(in const char *const *ppcArgv, in const _Atomic bool_t *pbCancel,
                             out char *pcOut, in size_t uOutCap, in uint32 u32TimeoutMs,
                             out int *piExitCode);

/* 可选测试注入钩子（NULL 成员回退真实 af_plat_* / dmc_client_*，保证零宿主机污染） */
typedef struct af_cli_ops
{
  const char *pcHomeDir;
  const char *pcDataDir;
  const char *pcExePath;
  const char *pcLegacyDaemon;
  uint32 u32VerifyTimeoutMs;
  af_cli_cmd_fn pfRunCmd;
  bool_t (*pfTcpCheck)(in const char *pcIp, in uint16 u16Port, in uint32 u32TimeoutMs);
  uint32 (*pfScList)(out af_sc_device_st *pastOut, in uint32 u32Cap);
  int (*pfScConnect)(in const char *pcName, out char *pcErr, in size_t uErrCap);
  int (*pfScDisconnect)(in const char *pcName, out char *pcErr, in size_t uErrCap);
  uint32 (*pfDispOnline)(out af_disp_info_st *pastOut, in uint32 u32Cap);
  uint32 (*pfBtPaired)(out af_bt_device_st *pastOut, in uint32 u32Cap);
  int (*pfBtConnect)(in const char *pcAddr);
  int (*pfBtDisconnect)(in const char *pcAddr);
  void (*pfKeychainDelLegacyPw)(void);
  int (*pfDmcCall)(in const char *pcDataDir, in const char *pcIface, in const char *pcBody,
                   out char *pcOut, in size_t uOutCap);
  int (*pfRunDaemon)(in int argc, in char **argv);
  bool_t (*pfIsExternalVol)(in const char *pcPath);
} af_cli_ops_st;

void af_cli_set_ops(in const af_cli_ops_st *pstOps);
void af_cli_default_data_dir(out char *pcBuf, in size_t uCap);
bool_t af_cli_is_external_vol(in const char *pcPath);
bool_t af_cli_is_subcommand(in const char *pcArg);
int af_cli_wake_existing(in const char *pcDataDir);
int af_cli_install_agent(in const char *pcExePath);
int af_cli_run(in int argc, in char **argv);

#ifdef __cplusplus
}
#endif

#endif /* AF_CLI_H */
