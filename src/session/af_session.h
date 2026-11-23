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

#define AF_FRONT_GAP_MS     1500u
#define AF_FRONT_STUCK_MS   5000u
#define AF_FRONT_STABLE_MS  3000u
#define AF_FRONT_TIMEOUT_MS 20000u
#define AF_NAME_MAX_BYTES   63u

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
  void (*pfRunOnMain)(in af_main_fn pfFn, io void *pvCtx);
  int (*pfBonjourStart)(in const char *pcName, in uint16 u16Port);
  void (*pfBonjourStop)(void);
  void (*pfHostName)(out char *pcBuf, in size_t uCap);
  void (*pfCurrentUser)(out char *pcBuf, in size_t uCap);
  void (*pfNotify)(in const char *pcTitle, in const char *pcBody, in const char *pcTag);
  int (*pfRunCmd)(in const char *const *ppcArgv, in const char *pcStdin,
                  in const _Atomic bool_t *pbCancel, out char *pcOut, in size_t uOutCap,
                  in uint32 u32TimeoutMs, out int *piExitCode);
  uint64 (*pfNowMs)(void);
  uint32 u32FrontTickMs; /* 0 = 1000ms 默认 */
} af_session_ops_st;

extern const dm_module_st g_stAfSessionMod;
const dm_module_st *af_session_module(void);

/* 壳回调与测试钩子装配 */
void af_session_set_menu_cb(in af_menu_update_cb pfCb);
void af_session_set_ops(in const af_session_ops_st *pstOps);

/* 纯逻辑单测接口 */
void af_front_start(out af_front_st *pstF, in const char *pcName, in uint64 u64NowMs);
af_front_act_et af_front_step(io af_front_st *pstF, in uint64 u64NowMs, in bool_t bConnected,
                              in bool_t bAvailable);
size_t af_session_trunc_utf8_bytes(in const char *pcSrc, out char *pcDst, in size_t uDstCap,
                                   in size_t uMaxBytes);

#ifdef __cplusplus
}
#endif

#endif /* AF_SESSION_H */
