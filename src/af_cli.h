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

typedef int (*af_cli_cmd_fn)(dm_in const char *const *ppcArgv, dm_in const _Atomic bool_t *pbCancel,
                             dm_out char *pcOut, dm_in size_t uOutCap, dm_in uint32 u32TimeoutMs,
                             dm_out int *piExitCode);

/* 可选测试注入钩子（NULL 成员回退真实 af_plat_* / dmc_client_*，保证零宿主机污染） */
typedef struct af_cli_ops
{
  const char *pcHomeDir;
  const char *pcDataDir;
  const char *pcExePath;
  const char *pcLegacyDaemon;
  uint32 u32VerifyTimeoutMs;
  af_cli_cmd_fn pfRunCmd;
  bool_t (*pfTcpCheck)(dm_in const char *pcIp, dm_in uint16 u16Port, dm_in uint32 u32TimeoutMs);
  uint32 (*pfScList)(dm_out af_sc_device_st *pastOut, dm_in uint32 u32Cap);
  int (*pfScConnect)(dm_in const char *pcName, dm_out char *pcErr, dm_in size_t uErrCap);
  int (*pfScDisconnect)(dm_in const char *pcName, dm_out char *pcErr, dm_in size_t uErrCap);
  uint32 (*pfDispOnline)(dm_out af_disp_info_st *pastOut, dm_in uint32 u32Cap);
  uint32 (*pfBtPaired)(dm_out af_bt_device_st *pastOut, dm_in uint32 u32Cap);
  int (*pfBtConnect)(dm_in const char *pcAddr);
  int (*pfBtDisconnect)(dm_in const char *pcAddr);
  void (*pfKeychainDelLegacyPw)(void);
  int (*pfDmcCall)(dm_in const char *pcDataDir, dm_in const char *pcIface, dm_in const char *pcBody,
                   dm_out char *pcOut, dm_in size_t uOutCap);
  int (*pfRunDaemon)(dm_in int argc, dm_in char **argv);
  bool_t (*pfIsExternalVol)(dm_in const char *pcPath);
} af_cli_ops_st;

void af_cli_set_ops(dm_in const af_cli_ops_st *pstOps);
void af_cli_default_data_dir(dm_out char *pcBuf, dm_in size_t uCap);
bool_t af_cli_is_external_vol(dm_in const char *pcPath);
bool_t af_cli_is_subcommand(dm_in const char *pcArg);
int af_cli_wake_existing(dm_in const char *pcDataDir);
int af_cli_install_agent(dm_in const char *pcExePath);
int af_cli_run(dm_in int argc, dm_in char **argv);

#ifdef __cplusplus
}
#endif

#endif /* AF_CLI_H */
