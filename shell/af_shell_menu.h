/**
 ******************************************************************************
 * @file    af_shell_menu.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   macOS 原生菜单栏 NSStatusItem 壳契约（DD-05 §3）。
 ******************************************************************************
 */
#ifndef AF_SHELL_MENU_H
#define AF_SHELL_MENU_H

#include "af_ui_ipc.h"

#ifdef __cplusplus
extern "C"
{
#endif

void af_shell_menu_init(void);
void af_shell_menu_update(in const af_menu_snap_st *pstSnap);
void af_shell_menu_destroy(void);
void af_shell_menu_trigger_quit(void);

#ifdef __cplusplus
}
#endif

#endif /* AF_SHELL_MENU_H */
