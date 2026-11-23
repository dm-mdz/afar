/**
 ******************************************************************************
 * @file    af_ui_ipc.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   基础层共享 UI 通信纯数据契约（供业务层与 shell 壳共同包含，零越层依赖）。
 ******************************************************************************
 */
#ifndef AF_UI_IPC_H
#define AF_UI_IPC_H

#include <dm/type.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum af_menu_act
{
  AF_MENU_CONNECT = 1,
  AF_MENU_DISCONNECT,
  AF_MENU_TOGGLE_BOOT_AUTO,
  AF_MENU_TOGGLE_ANTI_UNPLUG,
  AF_MENU_PAIR_NEW,
  AF_MENU_OPEN_SETUP,
  AF_MENU_RESET_SETUP,
  AF_MENU_REOPEN,
  AF_MENU_NOTIFY_DEVICES
} af_menu_act_et;

typedef struct af_ui_action_msg
{
  af_menu_act_et eAct;
  bool_t bVal;
} af_ui_action_msg_st;

typedef struct af_menu_snap
{
  uint8 u8IconState;
  char acStatusLine[96];
  bool_t bShowing;
  bool_t bBootAuto;
  bool_t bAntiUnplug;
  uint32 u32TodoCount;
  char acVersion[32];
} af_menu_snap_st;

typedef void (*af_menu_update_cb)(in const af_menu_snap_st *pstSnap);
typedef void (*af_setup_push_cb)(in const char *pcViewJson);
typedef void (*af_setup_win_cb)(in bool_t bShow);

#ifdef __cplusplus
}
#endif

#endif /* AF_UI_IPC_H */
