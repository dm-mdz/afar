/**
 ******************************************************************************
 * @file    af_modules.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   Afar 全局模块号、LinkID、事件号、角色位与统一错误码分配表（DD-00）。
 ******************************************************************************
 */
#ifndef AF_MODULES_H
#define AF_MODULES_H

#include <dm/link.h>
#include <dm/rpc.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define AF_VERSION             "0.5.0"

/* ---- 1. 模块号（8 个，紧接 libdm 0..15 保留段） ---- */
#define AF_MOD_AUTH            16u
#define AF_MOD_DISPLAY         17u
#define AF_MOD_SIDECAR         18u
#define AF_MOD_POWER           19u
#define AF_MOD_KBD             20u
#define AF_MOD_ENV             21u
#define AF_MOD_SESSION         22u
#define AF_MOD_SETUP           23u

/* ---- 2. 模块内 LinkID 总表 ---- */
#define AF_LINK_CLI_PAIR_CODE  DM_MOD_LINKID(AF_MOD_AUTH, 0x01u)
#define AF_LINK_UI_PAIR_OPEN   DM_MOD_LINKID(AF_MOD_AUTH, 0x02u)
#define AF_LINK_UI_PAIR_CLOSE  DM_MOD_LINKID(AF_MOD_AUTH, 0x03u)
#define AF_LINK_UI_UNPAIR      DM_MOD_LINKID(AF_MOD_AUTH, 0x04u)

#define AF_LINK_DISP_MAIN_DONE DM_MOD_LINKID(AF_MOD_DISPLAY, 0x01u)
#define AF_LINK_DISP_RECONF    DM_MOD_LINKID(AF_MOD_DISPLAY, 0x02u)

#define AF_LINK_SC_MAIN_DONE   DM_MOD_LINKID(AF_MOD_SIDECAR, 0x01u)

#define AF_LINK_KBD_PAIR_CB    DM_MOD_LINKID(AF_MOD_KBD, 0x01u)

#define AF_LINK_ENV_PROBE      DM_MOD_LINKID(AF_MOD_ENV, 0x01u)

#define AF_LINK_UI_ACTION      DM_MOD_LINKID(AF_MOD_SESSION, 0x01u)
#define AF_LINK_QUIT           DM_MOD_LINKID(AF_MOD_SESSION, 0x02u)
#define AF_LINK_CLI_DISCONNECT DM_MOD_LINKID(AF_MOD_SESSION, 0x03u)

#define AF_LINK_UI_SETUP_MSG   DM_MOD_LINKID(AF_MOD_SETUP, 0x01u)
#define AF_LINK_CLI_WIN_OPEN   DM_MOD_LINKID(AF_MOD_SETUP, 0x02u)
#define AF_LINK_UI_SETUP_GO    DM_MOD_LINKID(AF_MOD_SETUP, 0x03u)

/* ---- 3. 内部事件号（避开 0x0001~0x00FF 内核保留段） ---- */
#define AF_EV_AUTH_CHANGED     DM_MOD_LINKID(AF_MOD_AUTH, 0x01u)
#define AF_EV_DISPLAY_CHANGED  DM_MOD_LINKID(AF_MOD_DISPLAY, 0x01u)
#define AF_EV_SIDECAR_CHANGED  DM_MOD_LINKID(AF_MOD_SIDECAR, 0x01u)
#define AF_EV_KBD_CHANGED      DM_MOD_LINKID(AF_MOD_KBD, 0x01u)
#define AF_EV_ENV_UPDATED      DM_MOD_LINKID(AF_MOD_ENV, 0x01u)
#define AF_EV_SETUP_TODO       DM_MOD_LINKID(AF_MOD_SETUP, 0x01u)

/* ---- 4. 本机口角色位（复用 libdm 原生 DM_ROLE_CLI = 1u<<2） ---- */
#define AF_ROLE_CLI            DM_ROLE_CLI

/* ---- 5. 统一错误码（与 DD-00 §2 及 HTTP ErrorResp 1:1 映射） ---- */
#define AF_ERR_BAD_REQ         DM_MOD_ERR(AF_MOD_AUTH, 1)
#define AF_ERR_UNAUTH          DM_MOD_ERR(AF_MOD_AUTH, 2)
#define AF_ERR_WRONG_CODE      DM_MOD_ERR(AF_MOD_AUTH, 3)
#define AF_ERR_LOCKED          DM_MOD_ERR(AF_MOD_AUTH, 4)
#define AF_ERR_PAIR_CLOSED     DM_MOD_ERR(AF_MOD_AUTH, 5)
#define AF_ERR_UPGRADE_REQ     DM_MOD_ERR(AF_MOD_AUTH, 6)
#define AF_ERR_WRONG_PW        DM_MOD_ERR(AF_MOD_AUTH, 7)
#define AF_ERR_NOT_FOUND       DM_MOD_ERR(AF_MOD_AUTH, 8)
#define AF_ERR_INTERNAL        DM_MOD_ERR(AF_MOD_AUTH, 9)
#define AF_ERR_BT_DENIED       DM_MOD_ERR(AF_MOD_KBD, 1)
#define AF_ERR_BT_BUSY         DM_MOD_ERR(AF_MOD_KBD, 2)
#define AF_ERR_CHOOSE_IPAD     DM_MOD_ERR(AF_MOD_SESSION, 1)
#define AF_ERR_NO_IPAD         DM_MOD_ERR(AF_MOD_SESSION, 2)

#ifdef __cplusplus
}
#endif

#endif /* AF_MODULES_H */
