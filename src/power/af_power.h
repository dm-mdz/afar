/**
 ******************************************************************************
 * @file    af_power.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   服务层 af_power（ModID 19）公共契约：Q8 保活、屏保先落盘再改 0 与离线恢复（DD-03 §3）。
 ******************************************************************************
 */
#ifndef AF_POWER_H
#define AF_POWER_H

#include <dm/module.h>
#include <dm/type.h>
#include "../af_events.h"
#include "../af_store.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* 平台操作注入表（用于 L1/L2 无宿主机污染测试；NULL 成员默认回退真实 af_plat_*） */
typedef struct af_power_ops
{
  int (*pfPowerHold)(dm_in bool_t bHold);
  int (*pfSsGet)(dm_out int *piSec, dm_out bool_t *pbExists);
  int (*pfSsSet)(dm_in int iSec);
  int (*pfSsDelete)(void);
} af_power_ops_st;

extern const dm_module_st g_stAfPowerMod;
const dm_module_st *af_power_module(void);

/* 纯 C 判定公式（Q8 + v0.6 直连保活）：无实体屏、随航显示中或日常直连中即需保活 */
bool_t af_power_should_hold_ex(dm_in uint32 u32PhysCount, dm_in af_sc_state_et eScState,
                               dm_in bool_t bDirectStreaming);
bool_t af_power_should_hold(dm_in uint32 u32PhysCount, dm_in af_sc_state_et eScState);

/* 退出 pfDrain 与离线 afar-agent uninstall 共用的屏保恢复函数（幂等，读写
 * af_store_state()->stOrigSs） */
void af_power_restore_screensaver(dm_io af_orig_ss_st *pstOrig, dm_in bool_t bSyncSave,
                                  dm_in const char *pcDataDir);

/* 测试注入与状态查询接口 */
void af_power_set_ops(dm_in const af_power_ops_st *pstOps);
bool_t af_power_is_holding(void);

#ifdef __cplusplus
}
#endif

#endif /* AF_POWER_H */
