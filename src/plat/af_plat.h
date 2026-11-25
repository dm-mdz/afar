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

#define AF_DISP_MAX 16u
#define AF_DISP_MODES_MAX 64u
#define AF_SC_DEV_MAX 16u
#define AF_BT_DEV_MAX 32u
#define AF_VD_MODES_MAX 32u

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
typedef void (*af_main_fn)(dm_io void *pvCtx);
void af_plat_run_on_main(dm_in af_main_fn pfFn, dm_io void *pvCtx);
int af_plat_open_url(dm_in const char *pcUrl);
void af_plat_notify(dm_in const char *pcTitle, dm_in const char *pcBody, dm_in const char *pcTag);

/* 2. 显示器与虚拟屏 */
uint32 af_plat_disp_online(dm_out af_disp_info_st *pastOut, dm_in uint32 u32Cap);
uint32 af_plat_disp_modes(dm_in uint32 u32DispId, dm_out af_disp_mode_st *pastOut,
                          dm_in uint32 u32Cap);
int af_plat_disp_set_mode(dm_in uint32 u32DispId, dm_in const af_disp_mode_st *pstWant,
                          dm_in bool_t bAllowCgsFallback);
int af_plat_disp_set_hidpi_unmirrored(dm_in uint32 u32DispId, dm_in uint32 u32W, dm_in uint32 u32H);
int af_plat_disp_mirror(dm_in uint32 u32DispId, dm_in uint32 u32MasterId);
void af_plat_disp_watch_start(dm_in uint uiLinkID);
void af_plat_disp_watch_stop(void);

int af_plat_vd_create(dm_in uint32 u32Serial, dm_in uint32 u32MaxPw, dm_in uint32 u32MaxPh,
                      dm_in const af_vd_mode_req_st *pastModes, dm_in uint32 u32Count,
                      dm_out uint32 *pu32DispId, dm_out uint64 *pu64Handle);
int af_plat_vd_apply(dm_in uint64 u64Handle, dm_in const af_vd_mode_req_st *pastModes,
                     dm_in uint32 u32Count);
void af_plat_vd_destroy(dm_in uint64 u64Handle);
void af_plat_vd_destroy_all(void);

/* 3. 随航（主线程调用） */
int af_plat_sc_check_signatures(void); /* debug 启动校验 SidecarCore 方法签名 */
uint32 af_plat_sc_list(dm_out af_sc_device_st *pastOut, dm_in uint32 u32Cap);
int af_plat_sc_connect(dm_in const char *pcName, dm_out char *pcErr, dm_in size_t uErrCap);
int af_plat_sc_disconnect(dm_in const char *pcName, dm_out char *pcErr, dm_in size_t uErrCap);

/* 4. 电源与屏保 */
int af_plat_power_hold(dm_in bool_t bHold);
int af_plat_screensaver_get(dm_out int *piSec, dm_out bool_t *pbExists);
int af_plat_screensaver_set(dm_in int iSec);
int af_plat_screensaver_delete(void);

/* 5. 蓝牙 */
af_bt_auth_et af_plat_bt_auth(void);
uint32 af_plat_bt_paired(dm_out af_bt_device_st *pastOut, dm_in uint32 u32Cap);
bool_t af_plat_bt_is_connected(dm_in const char *pcAddr);
int af_plat_bt_connect(dm_in const char *pcAddr);
int af_plat_bt_disconnect(dm_in const char *pcAddr);
int af_plat_bt_pair_search(dm_in uint uiCbLinkID);
int af_plat_bt_pair_start(dm_in const char *pcAddr, dm_in uint uiCbLinkID);
void af_plat_bt_pair_cancel(void);

/* 6. Bonjour、OS、WebView */
int af_plat_bonjour_start(dm_in const char *pcName, dm_in uint16 u16Port);
void af_plat_bonjour_stop(void);
int af_plat_od_verify(dm_in const char *pcUser, dm_in const char *pcPass);
bool_t af_plat_keychain_has_legacy_pw(void);
void af_plat_keychain_del_legacy_pw(void);
void af_plat_current_user(dm_out char *pcBuf, dm_in size_t uCap);
void af_plat_host_name(dm_out char *pcBuf, dm_in size_t uCap);
int af_plat_run_cmd_cancellable(dm_in const char *const *ppcArgv, dm_in const char *pcStdin,
                                dm_in const _Atomic bool_t *pbCancel, dm_out char *pcOut,
                                dm_in size_t uOutCap, dm_in uint32 u32TimeoutMs,
                                dm_out int *piExitCode);
void af_plat_webview_force_close(dm_io void *pvWebView);

/* 7. 全局退出标志 */
extern _Atomic bool_t g_bAfQuitting;
extern _Atomic bool_t g_bAfQuitRequested;

/* 8. v0.6 直连串流、实时注入、权限与安全输入平台原语（DD v2.1 §6，全部 <= 5 参） */
typedef struct af_rect_f
{
  double dX;
  double dY;
  double dW;
  double dH;
} af_rect_f_st;

typedef struct af_inject_mouse
{
  double dX;
  double dY;
  int32 i32DeltaX;
  int32 i32DeltaY;
  uint8 u8Kind; /* 0=move, 1=down, 2=up, 3=drag */
  uint8 u8Btn;  /* 0=L, 1=R, 2=M */
  uint64 u64Flags;
} af_inject_mouse_st;

typedef struct af_inject_pencil
{
  double dX;
  double dY;
  float fPressure;
  float fTiltX;
  float fTiltY;
  uint8 u8Phase; /* 0=hover, 1=down, 2=move, 3=up, 4=cancel */
  bool_t bEnterProx;
  bool_t bExitProx;
  uint64 u64Flags;
} af_inject_pencil_st;

typedef struct af_plat_vid_frame
{
  uint64 u64Gen;
  uint32 u32Flags;
  uint64 u64CaptureUs;
  uint8 *pu8Data; /* dm_malloc_tag(..., AF_TAG_STREAM_FRAME)，所有权移交回调 */
  uint32 u32Len;
} af_plat_vid_frame_st;

typedef void (*af_plat_vid_frame_cb)(dm_in const af_plat_vid_frame_st *pstFrame, dm_io void *pvUser);
typedef void (*af_plat_vid_stop_cb)(dm_in uint64 u64Gen, dm_in int iReason, dm_io void *pvUser);

typedef struct af_plat_stream_cfg
{
  uint64 u64Gen;
  uint32 u32DispId;
  uint16 u16EncW;
  uint16 u16EncH;
  uint16 u16Fps;
  bool_t bChroma444;
  af_plat_vid_frame_cb pfOnFrame;
  af_plat_vid_stop_cb pfOnStopped;
  void *pvUser;
} af_plat_stream_cfg_st;

bool_t af_plat_perm_screen_capture(void);
bool_t af_plat_perm_accessibility(void);
void af_plat_request_screen_capture(void);
void af_plat_request_accessibility(void);
bool_t af_plat_disp_bounds(dm_in uint32 u32DispId, dm_out af_rect_f_st *pstOut);
bool_t af_plat_is_secure_input(void);
bool_t af_plat_hevc444_supported(void);

int af_plat_stream_start(dm_in const af_plat_stream_cfg_st *pstCfg, dm_out bool_t *pbEff444);
void af_plat_stream_stop_async(dm_in uint64 u64Gen, dm_in af_plat_vid_stop_cb pfOnDone,
                               dm_io void *pvUser);
void af_plat_stream_stop_sync(void);
void af_plat_stream_force_idr(dm_in uint64 u64Gen);
void af_plat_stream_switch_disp(dm_in uint64 u64Gen, dm_in uint32 u32NewDispId);
void af_plat_stream_set_bitrate(dm_in uint64 u64Gen, dm_in uint32 u32AvgBps);

void af_plat_inject_mouse(dm_in const af_inject_mouse_st *pstEv);
void af_plat_inject_scroll(dm_in int32 i32Dx, dm_in int32 i32Dy, dm_in uint64 u64Flags);
void af_plat_inject_key(dm_in uint16 u16HidUsage, dm_in bool_t bDown, dm_io uint64 *pu64Flags);
void af_plat_inject_pencil(dm_in const af_inject_pencil_st *pstEv);
void af_plat_inject_unicode(dm_in const uint16 *pu16Units, dm_in size_t uCount);
void af_plat_inject_flags(dm_in uint64 u64Flags);
int af_plat_hid_to_vk(dm_in uint16 u16HidUsage);

#ifdef __cplusplus
}
#endif

#endif /* AF_PLAT_H */
