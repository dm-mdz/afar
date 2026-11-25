/**
 ******************************************************************************
 * @file    af_display.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   服务层 af_display（ModID 17）公共契约与纯 C 状态机/算法接口（DD-03 §1）。
 ******************************************************************************
 */
#ifndef AF_DISPLAY_H
#define AF_DISPLAY_H

#include <dm/module.h>
#include <dm/type.h>
#include "../af_events.h"
#include "../plat/af_plat.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define AF_SCALE_COUNT 3u
#define AF_EXTRA_SIZES_MAX 16u
#define AF_PHYS_GUARD_MS 8000u /* 实体屏刚插上 8 秒不做显示配置 */
#define AF_PHYS_RETRY_MS 3000u /* 每 3 秒恢复一次分辨率 */
#define AF_PHYS_RETRY_MAX 8u   /* 最多 8 次恢复尝试 */
#define AF_VD_DROP_MS 3000u    /* 不再需要虚拟屏后等 3 秒再销毁 */
#define AF_VD_STABLE_MS 2000u  /* 防拔线需随航稳定 2 秒才建备用虚拟屏 */
#define AF_DESK_TRIES 3u
#define AF_DESK_RETRY_MS 1000u
#define AF_DESK_LOW_WIN_MS 60000u
#define AF_DESK_LOW_MAX 2u

typedef enum af_disp_kind
{
  AF_DISP_PHYSICAL = 0,
  AF_DISP_SIDECAR,
  AF_DISP_OURS,
  AF_DISP_OTHER
} af_disp_kind_et;

typedef enum af_vd_plan_act
{
  AF_VD_PLAN_NONE = 0,
  AF_VD_PLAN_CREATE,
  AF_VD_PLAN_KEEP,
  AF_VD_PLAN_DESTROY
} af_vd_plan_act_et;

typedef enum af_desk_act
{
  AF_DESK_NONE = 0,
  AF_DESK_REMEMBER,
  AF_DESK_RESTORE,
  AF_DESK_YIELD_LOW
} af_desk_act_et;

typedef enum af_follow_res
{
  AF_FOLLOW_OK = 0,
  AF_FOLLOW_SWITCH,
  AF_FOLLOW_MISSING
} af_follow_res_et;

typedef struct af_scale_preset
{
  const char *pcKey;   /* "larger" | "default" | "more" */
  const char *pcLabel; /* "更大文字" | "默认" | "更多空间" */
  uint32 u32W;         /* 点宽 */
  uint32 u32H;         /* 点高 */
} af_scale_preset_st;

typedef struct af_disp_snap
{
  af_vd_state_et eVdState;
  uint32 u32PhysCount;
  char acActualScale[16]; /* 当前真实生效档位；对不上为空 */
  af_scale_preset_st astScales[AF_SCALE_COUNT];
  bool_t bScaleBusy;
  char acScaleError[128];
} af_disp_snap_st;

typedef struct af_vd_plan
{
  bool_t bDropPending;
  uint64 u64DropSinceMs;
} af_vd_plan_st;

typedef struct af_desk
{
  bool_t bHasSaved;
  uint32 u32SavedW;
  uint32 u32SavedH;
  uint32 u32SavedPw;
  uint32 u32SavedPh;
  bool_t bNeedRestore;
  uint32 u32RestoreTries;
  bool_t bHasLastTry;
  uint64 u64LastTryMs;
  bool_t bHasSeen;
  uint32 u32SeenW;
  uint32 u32SeenH;
  uint32 u32SeenPw;
  uint32 u32SeenPh;
  bool_t bYieldLow;
  uint32 u32LowCnt;
  uint64 au64LowTimes[AF_DESK_LOW_MAX + 1u];
} af_desk_st;

typedef struct af_phys_guard_item
{
  uint32 u32DispId;
  char acKey[48];
  uint32 u32Retries;
  bool_t bHasRetry;
  uint64 u64LastRetryMs;
} af_phys_guard_item_st;

typedef struct af_phys_guard
{
  bool_t bGuardActive;
  uint64 u64GuardUntilMs;
  uint32 u32KnownCnt;
  af_phys_guard_item_st astKnown[AF_DISP_MAX];
} af_phys_guard_st;

/* 平台操作注入表（用于 L1/L2 无宿主机污染测试；未设置或 NULL 成员回退真实 af_plat_*） */
typedef struct af_display_ops
{
  void (*pfRunOnMain)(dm_in af_main_fn pfFn, dm_io void *pvCtx);
  uint32 (*pfDispOnline)(dm_out af_disp_info_st *pastOut, dm_in uint32 u32Cap);
  uint32 (*pfDispModes)(dm_in uint32 u32DispId, dm_out af_disp_mode_st *pastOut,
                        dm_in uint32 u32Cap);
  int (*pfDispSetMode)(dm_in uint32 u32DispId, dm_in const af_disp_mode_st *pstWant,
                       dm_in bool_t bAllowCgsFallback);
  int (*pfDispSetHidpiUnmirrored)(dm_in uint32 u32DispId, dm_in uint32 u32W, dm_in uint32 u32H);
  int (*pfDispMirror)(dm_in uint32 u32DispId, dm_in uint32 u32MasterId);
  void (*pfDispWatchStart)(dm_in uint uiLinkID);
  void (*pfDispWatchStop)(void);
  int (*pfVdCreate)(dm_in uint32 u32Serial, dm_in uint32 u32MaxPw, dm_in uint32 u32MaxPh,
                    dm_in const af_vd_mode_req_st *pastModes, dm_in uint32 u32Count,
                    dm_out uint32 *pu32DispId, dm_out uint64 *pu64Handle);
  int (*pfVdApply)(dm_in uint64 u64Handle, dm_in const af_vd_mode_req_st *pastModes,
                   dm_in uint32 u32Count);
  void (*pfVdDestroy)(dm_in uint64 u64Handle);
  void (*pfVdDestroyAll)(void);
  uint64 (*pfNowMs)(void);
} af_display_ops_st;

/* 模块描述符 */
extern const dm_module_st g_stAfDisplayMod;
const dm_module_st *af_display_module(void);

/* 纯 C 算法工具（可无 macOS 单测） */
bool_t af_scale_normalize_screen(dm_in uint32 u32InW, dm_in uint32 u32InH, dm_out uint32 *pu32Long,
                                 dm_out uint32 *pu32Short);
void af_scale_presets(dm_in uint32 u32PxW, dm_in uint32 u32PxH,
                      dm_out af_scale_preset_st astOut[AF_SCALE_COUNT]);
int af_scale_key_index(dm_in const char *pcKey);
uint32 af_vd_build_modes(dm_in const af_scale_preset_st astPresets[AF_SCALE_COUNT],
                         dm_in const char *pcWantKey, dm_in const uint32 aau32Extra[][2],
                         dm_in uint32 u32ExtraCnt, dm_in uint32 u32Style,
                         dm_out af_vd_mode_req_st *pastOut, dm_in uint32 u32Cap,
                         dm_out uint32 *pu32MaxPw, dm_out uint32 *pu32MaxPh);
af_disp_kind_et af_disp_classify(dm_in bool_t bIsOurs, dm_in uint32 u32Vendor,
                                 dm_in uint32 u32Model);

/* 分 iPad 配置读写辅助（供 af_display 内部及单测使用） */
const char *af_disp_store_target_scale(dm_in const char *pcName);
void af_disp_store_ipad_pixels(dm_in const char *pcName, dm_out uint32 *pu32W,
                               dm_out uint32 *pu32H);
void af_disp_store_resolve_presets(dm_in const char *pcName,
                                   dm_out af_scale_preset_st astOut[AF_SCALE_COUNT]);
void af_disp_store_save_sizes(dm_in const char *pcName,
                              dm_in const af_scale_preset_st astP[AF_SCALE_COUNT]);
void af_disp_store_save_scale(dm_in const char *pcName, dm_in const char *pcKey);

/* 纯 C 状态机与校准/跟随接口 */
typedef struct af_vd_eval_in
{
  uint32 u32PhysCount;
  bool_t bSidecarShowing;
  bool_t bSidecarStable2s;
  bool_t bAntiUnplug;
  bool_t bMirror;
  bool_t bHoldActive;
  bool_t bHasVd;
  bool_t bDirectStreaming;
} af_vd_eval_in_st;

typedef struct af_direct_vd_req
{
  const char *pcKeyId;
  uint32 u32PxW;
  uint32 u32PxH;
  const char *pcScale; /* 可 NULL，省略时读 af_store 既有档位 */
} af_direct_vd_req_st;

typedef void (*af_vd_ready_cb)(dm_in int iErr, dm_in uint32 u32VdDispId, dm_in uint16 u16EncW,
                               dm_in uint16 u16EncH, dm_io void *pvCtx);

af_vd_state_et af_vd_eval_state_ex(dm_in const af_vd_eval_in_st *pstIn, dm_out bool_t *pbWantVd);
af_vd_state_et af_vd_eval_state(dm_in uint32 u32PhysCount, dm_in bool_t bSidecarShowing,
                                dm_in bool_t bSidecarStable2s, dm_in bool_t bAntiUnplug,
                                dm_in bool_t bMirror, dm_in bool_t bHoldActive, dm_in bool_t bHasVd,
                                dm_out bool_t *pbWantVd);
af_vd_plan_act_et af_vd_plan_step(dm_io af_vd_plan_st *pstPlan, dm_in bool_t bWantVd,
                                  dm_in bool_t bHasVd, dm_in bool_t bHoldActive,
                                  dm_in uint64 u64NowMs);

void af_phys_guard_init(dm_out af_phys_guard_st *pstG);
bool_t af_phys_guard_is_active(dm_in const af_phys_guard_st *pstG, dm_in uint64 u64NowMs);
uint32 af_phys_guard_update(dm_io af_phys_guard_st *pstG, dm_in const af_disp_info_st *pastOnline,
                            dm_in uint32 u32OnlineCnt, dm_in uint32 u32OurVdId,
                            dm_in uint64 u64NowMs);
bool_t af_phys_guard_recover(dm_io af_phys_guard_st *pstG, dm_in const af_disp_info_st *pastOnline,
                             dm_in uint32 u32OnlineCnt, dm_in uint32 u32OurVdId,
                             dm_in uint64 u64NowMs, dm_in const af_display_ops_st *pstOps);

void af_desk_init(dm_out af_desk_st *pstD);
void af_desk_load_saved(dm_io af_desk_st *pstD, dm_in uint32 u32W, dm_in uint32 u32H,
                        dm_in uint32 u32Pw, dm_in uint32 u32Ph);
void af_desk_on_ipad(dm_io af_desk_st *pstD);
void af_desk_on_enter_desk(dm_io af_desk_st *pstD);
void af_desk_on_vd_rebuilt(dm_io af_desk_st *pstD);
void af_desk_reset(dm_io af_desk_st *pstD);
af_desk_act_et af_desk_step(dm_io af_desk_st *pstD, dm_in const af_disp_mode_st *pstCur,
                            dm_in uint64 u64NowMs, dm_out af_disp_mode_st *pstRestoreOut);

bool_t af_disp_from_sidecar(dm_in uint32 u32PxW, dm_in uint32 u32PxH,
                            dm_in const af_disp_mode_st *pastModes, dm_in uint32 u32ModeCnt,
                            dm_out af_scale_preset_st astOut[AF_SCALE_COUNT]);
af_follow_res_et af_disp_follow_eval(dm_in const af_disp_mode_st *pstCur, dm_in bool_t bHasCur,
                                     dm_in const af_disp_mode_st *pastModes,
                                     dm_in uint32 u32ModeCnt,
                                     dm_in const af_scale_preset_st astPresets[AF_SCALE_COUNT],
                                     dm_in const char *pcWantKey,
                                     dm_out af_disp_mode_st *pstTargetOut);
int af_disp_scale_fallback_exec(dm_in const af_display_ops_st *pstOps,
                                dm_in const af_scale_preset_st astPresets[AF_SCALE_COUNT],
                                dm_in const char *pcWantKey, dm_in const uint32 aau32Extra[][2],
                                dm_in uint32 u32ExtraCnt, dm_in uint32 u32SidecarDispId,
                                dm_in bool_t bMirror, dm_io uint32 *pu32VdDispId,
                                dm_io uint64 *pu64VdHandle, dm_io uint32 *pu32VdSerial,
                                dm_out char *pcErrOut, dm_in size_t uErrCap);

/* DM 线程业务接口 */
void af_display_get_snap(dm_in const char *pcSidecarName, dm_out af_disp_snap_st *pstOut);
int af_display_set_scale(dm_in const char *pcSidecarName, dm_in const char *pcKey);
void af_display_set_ipad_screen(dm_in const char *pcSidecarName, dm_in uint32 u32W,
                                dm_in uint32 u32H);
void af_display_set_anti_unplug(dm_in bool_t bEnable);
void af_display_hold(dm_in uint64 u64UntilMs);
int af_display_ensure_direct_vd(dm_in const af_direct_vd_req_st *pstReq,
                                dm_in af_vd_ready_cb pfCb, dm_io void *pvCtx);
void af_display_direct_stop(void);
uint32 af_display_get_vd_id(void);
bool_t af_display_has_sidecar_disp(void);

/* 测试注入与诊断接口 */
void af_display_set_ops(dm_in const af_display_ops_st *pstOps);
void af_display_set_tick_ms(dm_in uint32 u32TickMs);
void af_display_reconcile_now(void);

#ifdef __cplusplus
}
#endif

#endif /* AF_DISPLAY_H */
