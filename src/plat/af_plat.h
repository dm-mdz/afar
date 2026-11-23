/**
 ******************************************************************************
 * @file    af_plat.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   基础层 macOS 平台纯 C 隔离接口（DD-01 §2.1）。
 ******************************************************************************
 */
#ifndef AF_PLAT_H
#define AF_PLAT_H

#include <dm/type.h>
#include <stdatomic.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define AF_DISP_MAX       16u
#define AF_DISP_MODES_MAX 64u
#define AF_SC_DEV_MAX     16u
#define AF_BT_DEV_MAX     32u
#define AF_VD_MODES_MAX   32u

typedef struct af_disp_mode
{
  uint32 u32W;
  uint32 u32H;
  uint32 u32Pw;
  uint32 u32Ph;
  uint32 u32Hz100;
  bool_t bUsable;
} af_disp_mode_st;

typedef struct af_disp_info
{
  uint32 u32Id;
  uint32 u32Vendor;
  uint32 u32Model;
  uint32 u32Serial;
  bool_t bBuiltin;
  uint32 u32MirrorOf;
  af_disp_mode_st stCur;
  bool_t bHasCur;
} af_disp_info_st;

typedef struct af_vd_mode_req
{
  uint32 u32W;
  uint32 u32H;
} af_vd_mode_req_st;

typedef struct af_sc_device
{
  char acName[65];
  bool_t bConnected;
} af_sc_device_st;

typedef struct af_bt_device
{
  char acAddr[32];
  char acName[128];
  bool_t bConnected;
  bool_t bBle;
  uint32 u32Major;
} af_bt_device_st;

typedef enum af_bt_auth
{
  AF_BT_AUTH_NOT_DETERMINED = 0,
  AF_BT_AUTH_RESTRICTED = 1,
  AF_BT_AUTH_DENIED = 2,
  AF_BT_AUTH_ALLOWED = 3
} af_bt_auth_et;

typedef enum af_bt_pair_stage
{
  AF_BT_PAIR_IDLE = 0,
  AF_BT_PAIR_SEARCHING,
  AF_BT_PAIR_SEARCHED,
  AF_BT_PAIR_PAIRING,
  AF_BT_PAIR_DONE,
  AF_BT_PAIR_FAILED
} af_bt_pair_stage_et;

typedef struct af_bt_pair_cb_msg
{
  af_bt_pair_stage_et eStage;
  bool_t bHasDev;
  af_bt_device_st stDev;
  char acAddr[32];
  char acCode[32];
  char acMsg[128];
} af_bt_pair_cb_msg_st;

/* 1. 主线程异步投递与系统交互 */
typedef void (*af_main_fn)(io void *pvCtx);
void af_plat_run_on_main(in af_main_fn pfFn, io void *pvCtx);
int af_plat_open_url(in const char *pcUrl);
void af_plat_notify(in const char *pcTitle, in const char *pcBody, in const char *pcTag);

/* 2. 显示器与虚拟屏 */
uint32 af_plat_disp_online(out af_disp_info_st *pastOut, in uint32 u32Cap);
uint32 af_plat_disp_modes(in uint32 u32DispId, out af_disp_mode_st *pastOut, in uint32 u32Cap);
int af_plat_disp_set_mode(in uint32 u32DispId, in const af_disp_mode_st *pstWant,
                          in bool_t bAllowCgsFallback);
int af_plat_disp_set_hidpi_unmirrored(in uint32 u32DispId, in uint32 u32W, in uint32 u32H);
int af_plat_disp_mirror(in uint32 u32DispId, in uint32 u32MasterId);
void af_plat_disp_watch_start(in uint uiLinkID);
void af_plat_disp_watch_stop(void);

int af_plat_vd_create(in uint32 u32Serial, in uint32 u32MaxPw, in uint32 u32MaxPh,
                      in const af_vd_mode_req_st *pastModes, in uint32 u32Count,
                      out uint32 *pu32DispId, out uint64 *pu64Handle);
int af_plat_vd_apply(in uint64 u64Handle, in const af_vd_mode_req_st *pastModes,
                     in uint32 u32Count);
void af_plat_vd_destroy(in uint64 u64Handle);
void af_plat_vd_destroy_all(void);

/* 3. 随航（主线程调用） */
int af_plat_sc_check_signatures(void); /* debug 启动校验 SidecarCore 方法签名 */
uint32 af_plat_sc_list(out af_sc_device_st *pastOut, in uint32 u32Cap);
int af_plat_sc_connect(in const char *pcName, out char *pcErr, in size_t uErrCap);
int af_plat_sc_disconnect(in const char *pcName, out char *pcErr, in size_t uErrCap);

/* 4. 电源与屏保 */
int af_plat_power_hold(in bool_t bHold);
int af_plat_screensaver_get(out int *piSec, out bool_t *pbExists);
int af_plat_screensaver_set(in int iSec);
int af_plat_screensaver_delete(void);

/* 5. 蓝牙 */
af_bt_auth_et af_plat_bt_auth(void);
uint32 af_plat_bt_paired(out af_bt_device_st *pastOut, in uint32 u32Cap);
bool_t af_plat_bt_is_connected(in const char *pcAddr);
int af_plat_bt_connect(in const char *pcAddr);
int af_plat_bt_disconnect(in const char *pcAddr);
int af_plat_bt_pair_search(in uint uiCbLinkID);
int af_plat_bt_pair_start(in const char *pcAddr, in uint uiCbLinkID);
void af_plat_bt_pair_cancel(void);

/* 6. Bonjour、OS、WebView */
int af_plat_bonjour_start(in const char *pcName, in uint16 u16Port);
void af_plat_bonjour_stop(void);
int af_plat_od_verify(in const char *pcUser, in const char *pcPass);
bool_t af_plat_keychain_has_legacy_pw(void);
void af_plat_keychain_del_legacy_pw(void);
void af_plat_current_user(out char *pcBuf, in size_t uCap);
void af_plat_host_name(out char *pcBuf, in size_t uCap);
int af_plat_run_cmd_cancellable(in const char *const *ppcArgv, in const char *pcStdin,
                                in const _Atomic bool_t *pbCancel, out char *pcOut,
                                in size_t uOutCap, in uint32 u32TimeoutMs, out int *piExitCode);
void af_plat_webview_force_close(io void *pvWebView);

/* 7. 全局退出标志 */
extern _Atomic bool_t g_bAfQuitting;
extern _Atomic bool_t g_bAfQuitRequested;

#ifdef __cplusplus
}
#endif

#endif /* AF_PLAT_H */
