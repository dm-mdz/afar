/**
 ******************************************************************************
 * @file    pool.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   自有动态泳道线程池（DD-01 §3.1，FW-02，AUD-01）：
 *          3 条隔离泳道（DEFAULT / DB / HEAVY），独立队列与动态扩缩容，
 *          退出线程经 pfOnThreadExit 回收 TLS 资源并由主线程 uv_thread_join 回收。
 ******************************************************************************
 */
#ifndef DM_POOL_H
#define DM_POOL_H

#include <dm.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum dm_pool_lane
{
  DM_LANE_DEFAULT = 0, /* 默认通用慢活 */
  DM_LANE_DB = 1,      /* 短平快 SQLite 读写泳道（防被重活饿死） */
  DM_LANE_HEAVY = 2,   /* 会话日志扫描、大快照、重计算 */
  DM_LANE_MAX = 3
} dm_pool_lane_et;

typedef struct dm_pool_lane_cfg
{
  uint16 u16MinThreads; /* 最小常驻线程数（可为 0 表示闲时可缩至 0） */
  uint16 u16MaxThreads; /* 最大扩容线程数（上限 64） */
  uint32 u32IdleMs;     /* 空闲超时回收毫秒数，0 = 默认 60000 */
  uint32 u32QueueCap;   /* 最大排队任务数，0 = 默认 4096，超限返回 DM_BUSY */
} dm_pool_lane_cfg_st;

typedef struct dm_pool_stat
{
  uint16 u16LiveThreads;
  uint16 u16BusyThreads;
  uint32 u32Queued;
  uint64 u64DoneCount;
  uint64 u64WaitMaxNs;
  uint64 u64RunMaxNs;
} dm_pool_stat_st;

typedef void (*dm_pool_work_fn)(io void *pvArg);
typedef void (*dm_pool_after_fn)(io void *pvArg, in int iStatus);

/* 配置指定泳道参数（须在 dm_main_ex 启动线程池之前于主线程调用；运行中调用回 DM_LINK_ERR_STATE；
 * pstCfg == NULL 恢复该泳道默认配置；u32IdleMs 须 <= 0x7FFFFFFFu，0 表示保持默认） */
DM_API int dm_pool_set_lane(in dm_pool_lane_et eLane, in const dm_pool_lane_cfg_st *pstCfg);

/* 向指定泳道提交异步任务（仅主循环线程调用；完工回调经总队列回到主线程执行） */
DM_API int dm_pool_submit(in dm_pool_lane_et eLane, in dm_pool_work_fn pfWork,
                          in dm_pool_after_fn pfAfter, in void *pvArg);

/* 查询指定泳道实时统计快照（仅主循环线程或在 dm_main_ex 之外调用） */
DM_API int dm_pool_stat(in dm_pool_lane_et eLane, out dm_pool_stat_st *pstOut);

/* 累计经主线程 thread_join 回收的已退出工作线程数（原子读取，用于诊断与 T-DM-03 校验） */
DM_API uint64 dm_pool_reaped_threads(void);

#ifdef __cplusplus
}
#endif

#endif /* DM_POOL_H */
