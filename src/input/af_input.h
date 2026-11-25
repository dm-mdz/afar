/**
 ******************************************************************************
 * @file    af_input.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   服务层 af_input（ModID 25）公共契约：UDP AEAD 实时输入会话与键鼠/Pencil/中文注入（DD v2.1 §6.2）。
 ******************************************************************************
 */
#ifndef AF_INPUT_H
#define AF_INPUT_H

#include <dm/module.h>
#include <dm/type.h>
#include "../af_events.h"
#include "../plat/af_plat.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct af_input_start_req
{
  uint8 au8SidBe[8];
  uint8 au8KeyI2M[32]; /* k_in_i2m */
  uint8 au8KeyM2I[32]; /* k_ctl_m2i */
  uint32 u32DispId;
} af_input_start_req_st;

/* 测试注入表（NULL 成员回退真实 af_plat_inject_* 实现，供 L1 无辅助功能权限单测） */
typedef struct af_input_ops
{
  bool_t (*pfGetBounds)(dm_in uint32 u32DispId, dm_out af_rect_f_st *pstOut);
  void (*pfInjectMouse)(dm_in const af_inject_mouse_st *pstEv);
  void (*pfInjectScroll)(dm_in int32 i32Dx, dm_in int32 i32Dy, dm_in uint64 u64Flags);
  void (*pfInjectKey)(dm_in uint16 u16HidUsage, dm_in bool_t bDown, dm_io uint64 *pu64Flags);
  void (*pfInjectPencil)(dm_in const af_inject_pencil_st *pstEv);
  void (*pfInjectUnicode)(dm_in const uint16 *pu16Units, dm_in size_t uCount);
  void (*pfInjectFlags)(dm_in uint64 u64Flags);
  bool_t (*pfIsSecureInput)(void);
  uint32 u32WatchdogMs;   /* 0 = 默认 1500ms */
  uint32 u32SecPollMs;    /* 0 = 默认 500ms */
} af_input_ops_st;

extern const dm_module_st g_stAfInputMod;
const dm_module_st *af_input_module(void);

void af_input_set_ops(dm_in const af_input_ops_st *pstOps);
int af_input_start(dm_in const af_input_start_req_st *pstReq, dm_out uint16 *pu16InputPort);
void af_input_force_stop(void);
bool_t af_input_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* AF_INPUT_H */
