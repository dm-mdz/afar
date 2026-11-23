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

#define AF_SCALE_COUNT      3u
#define AF_EXTRA_SIZES_MAX  16u
#define AF_PHYS_GUARD_MS    8000u  /* 实体屏刚插上 8 秒不做显示配置 */
#define AF_PHYS_RETRY_MS    3000u  /* 每 3 秒恢复一次分辨率 */
#define AF_PHYS_RETRY_MAX   8u     /* 最多 8 次恢复尝试 */
#define AF_VD_DROP_MS       3000u  /* 不再需要虚拟屏后等 3 秒再销毁 */
#define AF_VD_STABLE_MS     2000u  /* 防拔线需随航稳定 2 秒才建备用虚拟屏 */
#define AF_DESK_TRIES       2u
#define AF_DESK_LOW_WIN_MS  60000u
#define AF_DESK_LOW_MAX     2u

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
  void (*pfRunOnMain)(in af_main_fn pfFn, io void *pvCtx);
  uint32 (*pfDispOnline)(out af_disp_info_st *pastOut, in uint32 u32Cap);
  uint32 (*pfDispModes)(in uint32 u32DispId, out af_disp_mode_st *pastOut, in uint32 u32Cap);
  int (*pfDispSetMode)(in uint32 u32DispId, in const af_disp_mode_st *pstWant,
                       in bool_t bAllowCgsFallback);
  int (*pfDispSetHidpiUnmirrored)(in uint32 u32DispId, in uint32 u32W, in uint32 u32H);
  int (*pfDispMirror)(in uint32 u32DispId, in uint32 u32MasterId);
  void (*pfDispWatchStart)(in uint uiLinkID);
  void (*pfDispWatchStop)(void);
  int (*pfVdCreate)(in uint32 u32Serial, in uint32 u32MaxPw, in uint32 u32MaxPh,
                    in const af_vd_mode_req_st *pastModes, in uint32 u32Count,
                    out uint32 *pu32DispId, out uint64 *pu64Handle);
  int (*pfVdApply)(in uint64 u64Handle, in const af_vd_mode_req_st *pastModes, in uint32 u32Count);
  void (*pfVdDestroy)(in uint64 u64Handle);
  void (*pfVdDestroyAll)(void);
  uint64 (*pfNowMs)(void);
} af_display_ops_st;

/* 模块描述符 */
extern const dm_module_st g_stAfDisplayMod;
const dm_module_st *af_display_module(void);

/* 纯 C 算法工具（可无 macOS 单测） */
bool_t af_scale_normalize_screen(in uint32 u32InW, in uint32 u32InH, out uint32 *pu32Long,
                                 out uint32 *pu32Short);
void af_scale_presets(in uint32 u32PxW, in uint32 u32PxH,
                      out af_scale_preset_st astOut[AF_SCALE_COUNT]);
int af_scale_key_index(in const char *pcKey);
uint32 af_vd_build_modes(in const af_scale_preset_st astPresets[AF_SCALE_COUNT],
                         in const char *pcWantKey, in const uint32 aau32Extra[][2],
                         in uint32 u32ExtraCnt, in uint32 u32Style,
                         out af_vd_mode_req_st *pastOut, in uint32 u32Cap, out uint32 *pu32MaxPw,
                         out uint32 *pu32MaxPh);
af_disp_kind_et af_disp_classify(in bool_t bIsOurs, in uint32 u32Vendor, in uint32 u32Model);

/* 分 iPad 配置读写辅助（供 af_display 内部及单测使用） */
const char *af_disp_store_target_scale(in const char *pcName);
void af_disp_store_ipad_pixels(in const char *pcName, out uint32 *pu32W, out uint32 *pu32H);
void af_disp_store_resolve_presets(in const char *pcName,
                                   out af_scale_preset_st astOut[AF_SCALE_COUNT]);
void af_disp_store_save_sizes(in const char *pcName,
                              in const af_scale_preset_st astP[AF_SCALE_COUNT]);
void af_disp_store_save_scale(in const char *pcName, in const char *pcKey);

/* 纯 C 状态机与校准/跟随接口 */
af_vd_state_et af_vd_eval_state(in uint32 u32PhysCount, in bool_t bSidecarShowing,
                                in bool_t bSidecarStable2s, in bool_t bAntiUnplug,
                                in bool_t bMirror, in bool_t bHoldActive, in bool_t bHasVd,
                                out bool_t *pbWantVd);
af_vd_plan_act_et af_vd_plan_step(io af_vd_plan_st *pstPlan, in bool_t bWantVd, in bool_t bHasVd,
                                  in bool_t bHoldActive, in uint64 u64NowMs);

void af_phys_guard_init(out af_phys_guard_st *pstG);
bool_t af_phys_guard_is_active(in const af_phys_guard_st *pstG, in uint64 u64NowMs);
uint32 af_phys_guard_update(io af_phys_guard_st *pstG, in const af_disp_info_st *pastOnline,
                            in uint32 u32OnlineCnt, in uint32 u32OurVdId, in uint64 u64NowMs);
bool_t af_phys_guard_recover(io af_phys_guard_st *pstG, in const af_disp_info_st *pastOnline,
                             in uint32 u32OnlineCnt, in uint32 u32OurVdId, in uint64 u64NowMs,
                             in const af_display_ops_st *pstOps);

void af_desk_init(out af_desk_st *pstD);
void af_desk_load_saved(io af_desk_st *pstD, in uint32 u32W, in uint32 u32H, in uint32 u32Pw,
                        in uint32 u32Ph);
void af_desk_on_ipad(io af_desk_st *pstD);
void af_desk_on_enter_desk(io af_desk_st *pstD);
void af_desk_on_vd_rebuilt(io af_desk_st *pstD);
void af_desk_reset(io af_desk_st *pstD);
af_desk_act_et af_desk_step(io af_desk_st *pstD, in const af_disp_mode_st *pstCur,
                            in uint64 u64NowMs, out af_disp_mode_st *pstRestoreOut);

bool_t af_disp_from_sidecar(in uint32 u32PxW, in uint32 u32PxH, in const af_disp_mode_st *pastModes,
                            in uint32 u32ModeCnt, out af_scale_preset_st astOut[AF_SCALE_COUNT]);
af_follow_res_et af_disp_follow_eval(in const af_disp_mode_st *pstCur, in bool_t bHasCur,
                                     in const af_disp_mode_st *pastModes, in uint32 u32ModeCnt,
                                     in const af_scale_preset_st astPresets[AF_SCALE_COUNT],
                                     in const char *pcWantKey, out af_disp_mode_st *pstTargetOut);
int af_disp_scale_fallback_exec(in const af_display_ops_st *pstOps,
                                in const af_scale_preset_st astPresets[AF_SCALE_COUNT],
                                in const char *pcWantKey, in const uint32 aau32Extra[][2],
                                in uint32 u32ExtraCnt, in uint32 u32SidecarDispId,
                                in bool_t bMirror, io uint32 *pu32VdDispId,
                                io uint64 *pu64VdHandle, io uint32 *pu32VdSerial,
                                out char *pcErrOut, in size_t uErrCap);

/* DM 线程业务接口 */
void af_display_get_snap(in const char *pcSidecarName, out af_disp_snap_st *pstOut);
int af_display_set_scale(in const char *pcSidecarName, in const char *pcKey);
void af_display_set_ipad_screen(in const char *pcSidecarName, in uint32 u32W, in uint32 u32H);
void af_display_set_anti_unplug(in bool_t bEnable);
void af_display_hold(in uint64 u64UntilMs);

/* 测试注入与诊断接口 */
void af_display_set_ops(in const af_display_ops_st *pstOps);
void af_display_set_tick_ms(in uint32 u32TickMs);
void af_display_reconcile_now(void);

#ifdef __cplusplus
}
#endif

#endif /* AF_DISPLAY_H */
