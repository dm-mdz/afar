/**
 ******************************************************************************
 * @file    af_session.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   业务层 af_session（ModID 22）公共契约、菜单回调装配与 Front 状态机接口（DD-05 §1）。
 ******************************************************************************
 */
#ifndef AF_SESSION_H
#define AF_SESSION_H

#include <dm/module.h>
#include "../af_ui_ipc.h"
#include "../plat/af_plat.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define AF_FRONT_GAP_MS 1500u
#define AF_FRONT_STUCK_MS 5000u
#define AF_FRONT_STABLE_MS 3000u
#define AF_FRONT_TIMEOUT_MS 20000u
#define AF_NAME_MAX_BYTES 63u

typedef enum af_front_act
{
  AF_FRONT_WAIT = 0,
  AF_FRONT_CONNECT,
  AF_FRONT_DONE,
  AF_FRONT_GIVEUP
} af_front_act_et;

typedef struct af_front
{
  bool_t bActive;
  char acName[65];
  uint64 u64ReconnectAtMs;
  uint64 u64SentAtMs;
  uint64 u64SeenAtMs;
  uint64 u64UntilMs;
} af_front_st;

/* 平台操作可注入钩子（NULL 成员回退真实 af_plat_* 与单调时钟，供 L1/L2 零宿主机污染测试） */
typedef struct af_session_ops
{
  void (*pfRunOnMain)(dm_in af_main_fn pfFn, dm_io void *pvCtx);
  int (*pfBonjourStart)(dm_in const char *pcName, dm_in uint16 u16Port);
  void (*pfBonjourStop)(void);
  void (*pfHostName)(dm_out char *pcBuf, dm_in size_t uCap);
  void (*pfCurrentUser)(dm_out char *pcBuf, dm_in size_t uCap);
  void (*pfNotify)(dm_in const char *pcTitle, dm_in const char *pcBody, dm_in const char *pcTag);
  int (*pfRunCmd)(dm_in const char *const *ppcArgv, dm_in const char *pcStdin,
                  dm_in const _Atomic bool_t *pbCancel, dm_out char *pcOut, dm_in size_t uOutCap,
                  dm_in uint32 u32TimeoutMs, dm_out int *piExitCode);
  uint64 (*pfNowMs)(void);
  uint32 u32FrontTickMs; /* 0 = 1000ms 默认 */
} af_session_ops_st;

extern const dm_module_st g_stAfSessionMod;
const dm_module_st *af_session_module(void);

/* 壳回调与测试钩子装配 */
void af_session_set_menu_cb(dm_in af_menu_update_cb pfCb);
void af_session_set_ops(dm_in const af_session_ops_st *pstOps);

/* 纯逻辑单测接口 */
void af_front_start(dm_out af_front_st *pstF, dm_in const char *pcName, dm_in uint64 u64NowMs);
af_front_act_et af_front_step(dm_io af_front_st *pstF, dm_in uint64 u64NowMs,
                              dm_in bool_t bConnected, dm_in bool_t bAvailable);
size_t af_session_trunc_utf8_bytes(dm_in const char *pcSrc, dm_out char *pcDst,
                                   dm_in size_t uDstCap, dm_in size_t uMaxBytes);

#ifdef __cplusplus
}
#endif

#endif /* AF_SESSION_H */
