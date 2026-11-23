/**
 ******************************************************************************
 * @file    af_shell_win.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   macOS 原生设置向导窗口 NSWindow + WKWebView 壳契约（DD-05 §3）。
 ******************************************************************************
 */
#ifndef AF_SHELL_WIN_H
#define AF_SHELL_WIN_H

#include "af_ui_ipc.h"

#ifdef __cplusplus
extern "C"
{
#endif

void af_shell_win_set_html_path(in const char *pcPath);
void af_shell_win_show(in bool_t bShow);
void af_shell_win_push(in const char *pcViewJson);
void af_shell_win_destroy(void);
bool_t af_shell_win_is_visible(void);
void af_shell_win_refresh_lang(void);

#ifdef __cplusplus
}
#endif

#endif /* AF_SHELL_WIN_H */
