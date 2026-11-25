/**
 ******************************************************************************
 * @file    af_kbd.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   服务层 af_kbd 蓝牙键盘交接状态机、设备过滤与 DM 模块契约（DD-04 §2）。
 ******************************************************************************
 */
#ifndef AF_KBD_H
#define AF_KBD_H

#include <dm/module.h>
#include <dm/type.h>
#include "../plat/af_plat.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define AF_KBD_ARM_MS 30000u       /* 点连接后 30s 内连上随航才接管 */
#define AF_KBD_GAP_MS 6000u        /* 断开指令后 6s 内的已连接可能是旧状态 */
#define AF_KBD_TAKE_MAX_MS 60000u  /* 等用户在 iPad 断开后 Mac 接管的最长重试时间 */
#define AF_KBD_TAKE_STEP 2000u     /* 每 2s 重试一次 openConnection */
#define AF_KBD_RELEASED_MS 600000u /* 松开后 10 分钟内 keyboard_released=true */

typedef enum af_kbd_act
{
  AF_KBD_ACT_NONE = 0,
  AF_KBD_ACT_TAKE,          /* 启动 60s/2s 接管重试 */
  AF_KBD_ACT_RELEASE,       /* 松开键盘 */
  AF_KBD_ACT_CLEAR_RELEASED /* 不接管本次：清掉上次 released 提示 */
} af_kbd_act_et;

/* 纯逻辑决策机（完全对应老版 kbflow.rs::KbFlow） */
typedef struct af_kbd_flow
{
  bool_t bWasShowing;
  bool_t bHasTakeFor;
  char acTakeFor[65];
  uint64 u64TakeUntilMs;
  uint64 u64GapUntilMs;
  uint64 u64HoldUntilMs; /* POST /front 期间不松开 */
  bool_t bHeldDrop;      /* hold 期间压下过一次断开，未连回则补松开 */
  bool_t bTaken;         /* 本次由 Afar 接管，同步持久化到 state.keyboard_taken */
} af_kbd_flow_st;

typedef struct af_kbd_pair_snap
{
  bool_t bActive;
  af_bt_pair_stage_et eStage;
  uint32 u32FoundCnt;
  af_bt_device_st astFound[AF_BT_DEV_MAX];
  char acAddr[32];
  char acCode[32]; /* PIN / Passkey 数字，无需时 "" */
  char acMsg[128];
} af_kbd_pair_snap_st;

typedef struct af_kbd_snap
{
  bool_t bHasSelected;
  af_bt_device_st stSelected; /* bConnected 为实时状态 */
  bool_t bAuto;
  bool_t bTaking;
  char acError[128];
  bool_t bReleased; /* now - released_at < 600s */
  af_kbd_pair_snap_st stPair;
} af_kbd_snap_st;

/* 平台操作可注入钩子（默认走 af_plat_* 与系统单调时钟，供 L1/L2 确定性单测） */
typedef struct af_kbd_ops
{
  af_bt_auth_et (*pfBtAuth)(void);
  uint32 (*pfBtPaired)(dm_out af_bt_device_st *pastOut, dm_in uint32 u32Cap);
  bool_t (*pfBtIsConnected)(dm_in const char *pcAddr);
  int (*pfBtConnect)(dm_in const char *pcAddr);
  int (*pfBtDisconnect)(dm_in const char *pcAddr);
  int (*pfBtPairSearch)(dm_in uint uiCbLinkID);
  int (*pfBtPairStart)(dm_in const char *pcAddr, dm_in uint uiCbLinkID);
  void (*pfBtPairCancel)(void);
  void (*pfRunOnMain)(dm_in af_main_fn pfFn, dm_io void *pvCtx);
  int (*pfOpenUrl)(dm_in const char *pcUrl);
  uint64 (*pfNowMs)(void);
  uint32 u32TakeMaxMs;  /* 0 = AF_KBD_TAKE_MAX_MS */
  uint32 u32TakeStepMs; /* 0 = AF_KBD_TAKE_STEP */
} af_kbd_ops_st;

/* DM 模块描述符获取与测试平台钩子注入 */
extern const dm_module_st g_stAfKbdMod;
const dm_module_st *af_kbd_module(void);
void af_kbd_set_ops(dm_in const af_kbd_ops_st *pstOps);

/* 纯逻辑 API（供单测直接驱动） */
void af_kbd_flow_init(dm_out af_kbd_flow_st *pstFlow, dm_in bool_t bShowing, dm_in bool_t bTaken);
void af_kbd_flow_on_connect(dm_io af_kbd_flow_st *pstFlow, dm_in const char *pcTarget,
                            dm_in bool_t bKeyboard, dm_in uint64 u64NowMs);
void af_kbd_flow_hold(dm_io af_kbd_flow_st *pstFlow, dm_in uint64 u64UntilMs);
void af_kbd_flow_on_disconnect(dm_io af_kbd_flow_st *pstFlow, dm_in bool_t bShowingNow,
                               dm_in uint64 u64NowMs);
af_kbd_act_et af_kbd_flow_tick(dm_io af_kbd_flow_st *pstFlow, dm_in const char *const *ppcConnected,
                               dm_in uint32 u32ConnCnt, dm_in bool_t bKbOnMac,
                               dm_in uint64 u64NowMs);
af_kbd_act_et af_kbd_flow_take(dm_io af_kbd_flow_st *pstFlow, dm_in bool_t bKbOnMac);
af_kbd_act_et af_kbd_flow_release(dm_io af_kbd_flow_st *pstFlow);
af_kbd_act_et af_kbd_flow_release_now(dm_io af_kbd_flow_st *pstFlow);
bool_t af_kbd_is_apple_non_kb(dm_in const char *pcName);
bool_t af_kbd_device_allowed(dm_in const af_bt_device_st *pstDev);

/* DM 线程业务调用接口 */
void af_kbd_get_snap(dm_out af_kbd_snap_st *pstOut);
void af_kbd_notify_connect(dm_in const char *pcTarget, dm_in bool_t bKeyboard);
void af_kbd_notify_disconnect(dm_in bool_t bShowingNow);
void af_kbd_hold(dm_in uint64 u64UntilMs);

#ifdef __cplusplus
}
#endif

#endif /* AF_KBD_H */
