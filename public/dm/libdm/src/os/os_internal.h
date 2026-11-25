/**
 ******************************************************************************
 * @file    os_internal.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   libdm/src/os 内部共用声明（禁止引入 <uv.h> 或 dm_internal.h）。
 ******************************************************************************
 */
#ifndef DM_OS_INTERNAL_H
#define DM_OS_INTERNAL_H

#include <dm/os.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct dm__os_node
{
  int iPid;
  int iPpid;
  uint64 u64StartMs;
  char acName[DM_OS_PROC_NAME_MAX];
  char acCmd[DM_OS_PROC_CMD_MAX];
} dm__os_node_st;

/* 懒解析回调：用于 Windows 等按需取进程启动时间并判活（返回 DM_NOT_FOUND 时剔除僵尸/已退出节点） */
typedef int (*dm__os_start_fn)(dm_in int iPid, dm_out uint64 *pu64StartMs, dm_io void *pvUser);

/* 公共无环 BFS 子孙遍历（FIX-05/06）：
 * - 使用 abVisited[] 数组与 PID 去重杜绝 PPID 环路及重复 PID 节点；
 * - 校对启动时间戳防止 PID 复用把无关进程误认成子进程（pfResolve 非 NULL 时按需解析并校验存活）；
 * - 写入不超过 u32Cap 条到 pau32Idx（按层序父先子后），返回实际写入数。 */
uint32 dm__os_walk_descendants_ex(dm_io dm__os_node_st *pastAll, dm_in uint32 u32All,
                                  dm_in int iRootPid, dm_out uint32 *pau32Idx, dm_in uint32 u32Cap,
                                  dm_in dm__os_start_fn pfResolve, dm_io void *pvUser);

uint32 dm__os_walk_descendants(dm_in const dm__os_node_st *pastAll, dm_in uint32 u32All,
                               dm_in int iRootPid, dm_out uint32 *pau32Idx, dm_in uint32 u32Cap);

#ifdef __cplusplus
}
#endif

#endif /* DM_OS_INTERNAL_H */
