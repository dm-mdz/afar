/**
 ******************************************************************************
 * @file    af_sidecar.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   服务层 af_sidecar（ModID 18）公共契约与 Keep/冷却/节流纯 C 状态机（DD-03 §2）。
 ******************************************************************************
 */
#ifndef AF_SIDECAR_H
#define AF_SIDECAR_H

#include <dm/module.h>
#include <dm/type.h>
#include "../af_events.h"
#include "../plat/af_plat.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define AF_SC_CONNECT_TIMEOUT_MS 15000u
#define AF_SC_RETRY_GAP_MS       15000u
#define AF_SC_RETRY_MAX          2u
#define AF_SC_GRACE_MS           10000u /* 主动断开宽限期，防异步旧态误判 */
#define AF_SC_COOLDOWN_MS        10000u /* 意外断开冷却期，期间拒 auto=true */
#define AF_SC_ERR_THROTTLE_MS    60000u /* 相同底层报错 60s 内只记首条与汇总 */

typedef struct af_sc_snap
{
  af_sc_state_et eState;
  char acShowing[65]; /* 正在显示的 iPad 随航名，空=无 */
  uint32 u32NearbyCnt;
  char aacNearby[AF_SC_DEV_MAX][65]; /* 附近可用 iPad 列表 */
  bool_t bInCooldown;                /* 意外断开 10s 内 */
} af_sc_snap_st;

typedef struct af_sc_keep
{
  bool_t bPending;
  char acTarget[65];
  uint64 u64NextRetryMs;
  uint32 u32RetriesLeft;
} af_sc_keep_st;

typedef struct af_sc_err_throttle
{
  bool_t bActive;
  char acLastErr[128];
  uint64 u64WinStartMs;
  uint32 u32Suppressed;
  uint32 u32TotalEmitted;
  uint32 u32TotalSuppressed;
} af_sc_err_throttle_st;

typedef struct af_sc_diag
{
  uint32 u32PollDispatched;
  uint32 u32PollSkipped;
  uint32 u32PollCompleted;
  uint32 u32ErrEmitted;
  uint32 u32ErrSuppressed;
  uint32 u32RetryTriggered;
} af_sc_diag_st;

/* 平台操作注入表（用于 L1/L2 无宿主机污染测试；NULL 成员默认回退真实 af_plat_*） */
typedef struct af_sidecar_ops
{
  void (*pfRunOnMain)(in af_main_fn pfFn, io void *pvCtx);
  uint32 (*pfScList)(out af_sc_device_st *pastOut, in uint32 u32Cap);
  int (*pfScConnect)(in const char *pcName, out char *pcErr, in size_t uErrCap);
  int (*pfScDisconnect)(in const char *pcName, out char *pcErr, in size_t uErrCap);
  uint64 (*pfNowMs)(void);
} af_sidecar_ops_st;

extern const dm_module_st g_stAfSidecarMod;
const dm_module_st *af_sidecar_module(void);

/* 纯 C Keep 与错误节流状态机（可直接单测） */
void af_sc_keep_want(out af_sc_keep_st *pstK, in const char *pcName, in uint64 u64NowMs);
void af_sc_keep_once(out af_sc_keep_st *pstK, in const char *pcName);
void af_sc_keep_on_connected(io af_sc_keep_st *pstK);
void af_sc_keep_clear(io af_sc_keep_st *pstK);
bool_t af_sc_keep_due(io af_sc_keep_st *pstK, in uint64 u64NowMs, out char *pcTargetOut,
                      in size_t uCap);

void af_sc_err_throttle_init(out af_sc_err_throttle_st *pstT);
bool_t af_sc_err_throttle_check(io af_sc_err_throttle_st *pstT, in const char *pcErr,
                                in uint64 u64NowMs, out uint32 *pu32SummarySuppressed);

/* DM 线程业务接口 */
void af_sidecar_get_snap(out af_sc_snap_st *pstOut);
int af_sidecar_connect(in const char *pcName, in bool_t bWithRetry);
int af_sidecar_disconnect(in const char *pcName);
bool_t af_sidecar_in_cooldown(void);

/* 测试注入与诊断接口 */
void af_sidecar_set_ops(in const af_sidecar_ops_st *pstOps);
void af_sidecar_set_poll_ms(in uint32 u32PollMs);
void af_sidecar_get_diag(out af_sc_diag_st *pstOut);

#ifdef __cplusplus
}
#endif

#endif /* AF_SIDECAR_H */
