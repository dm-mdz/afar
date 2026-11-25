/**
 ******************************************************************************
 * @file    stat.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   主线程卡顿监测和 dm.stat（DD-01）。
 *
 * 三样：
 *   ① 处理项计时：总队列交 before、线程池回来调 after 时，前后各取一次时间，按编号累计
 *      次数、总耗时、最长一次；单次超过 DM_STAT_SLOW_MS 记 WARN。
 *   ② 一圈干了多久：不加定时器。开 libuv 的空闲计时，挂一个不占住循环的 check 句柄，
 *      每圈用「这一圈的总时长 − 睡在等事件上的时长」，超过 DM_STAT_SLOW_MS 记 WARN。
 *      没事时循环不转，它也不醒。
 *   ③ 线程池排队：提交到开始跑的时间，最近 8 个的平均超过 DM_STAT_POOL_MS 记 WARN（最多每秒一行）。
 * 日志的等锁和写盘耗时在 log.c 里量，这里只取来放进 dm.stat。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "stat"
#include <dm.h>
#include <dm/link.h>
#include <dm/hash.h>
#include <dm/list.h>
#include <dm/log.h>
#include <dm/limits.h>

#include <string.h> /* memset */

#include "../dm_internal.h"

#define DM__STAT_BUCKETS 64u
/* 事件订阅者的条目按「事件号 | 这一位」记，和处理项编号分开 */
#define DM__POOL_WIN 8u /* 线程池排队取最近几个的平均 */
#define DM__NS_PER_MS 1000000u

typedef struct dm__stat_one
{
  uint32 u32Key;
  uint32 u32Before; /* before 调了几次 */
  uint32 u32After;  /* after 调了几次 */
  uint64 u64TotalNs;
  uint64 u64MaxNs;
} dm__stat_one_st;

static dm_hash_table_st g_stStatTable;
static dm_uv_check_st g_stStatCheck;
static bool_t g_bStatCheckUp = DM_FALSE;
static uint64 g_u64LoopLast = 0;     /* 上一圈 check 的时刻 */
static uint64 g_u64LoopLastIdle = 0; /* 那时累计的空闲时间 */
static uint64 g_u64LoopMaxNs = 0;
static uint32 g_u32LoopSlow = 0;  /* 超过阈值的圈数 */
static uint64 g_u64LoopTurns = 0; /* 转了几圈（白盒测试看「没事不醒」） */
static uint64 g_au64PoolWait[DM__POOL_WIN];
static uint32 g_u32PoolN = 0; /* 一共记了几个 */
static uint64 g_u64PoolTotalNs = 0;
static uint64 g_u64PoolMaxNs = 0;
static uint64 g_u64PoolWarnAt = 0;
static const char *(*g_pfStatName)(dm_in uint uiLinkID) = NULL;

/* ============================ ① 处理项计时 ============================ */

static dm__stat_one_st *dm__stat_get(dm_in uint32 u32Key)
{
  dm__stat_one_st *pstOne = NULL;

  if ((NULL == g_stStatTable.pstBuckets) &&
      (DM_SUCCESS != dm_hash_init(&g_stStatTable, DM__STAT_BUCKETS, NULL)))
  {
    return NULL;
  }
  pstOne = (dm__stat_one_st *)dm_hash_find(&g_stStatTable, u32Key);
  if (NULL != pstOne)
  {
    return pstOne;
  }
  pstOne = (dm__stat_one_st *)dm_calloc_tag(1u, sizeof(dm__stat_one_st), DM_MEM_TAG_RPC);
  if (NULL == pstOne)
  {
    return NULL;
  }
  pstOne->u32Key = u32Key;
  if (DM_SUCCESS != dm_hash_add(&g_stStatTable, u32Key, pstOne))
  {
    dm_free_tag(pstOne);
    return NULL;
  }
  return pstOne;
}

uint32 dm__stat_key(dm_in uint uiLinkID, dm_in uint8 u8Src, dm_in uint64 u64Tag)
{
  if (DM_SRC_EVENT == u8Src)
  {
    return DM__STAT_EVENT_BIT | (uint32)(u64Tag & 0xFFFFu);
  }
  return (uint32)uiLinkID;
}

void dm__stat_call(dm_in uint32 u32Key, dm_in uint64 u64Ns, dm_in bool_t bAfter)
{
  dm__stat_one_st *pstOne = dm__stat_get(u32Key);
  const char *pcName = NULL;

  if (NULL != pstOne)
  {
    if (bAfter)
    {
      pstOne->u32After++;
    }
    else
    {
      pstOne->u32Before++;
    }
    pstOne->u64TotalNs += u64Ns;
    if (u64Ns > pstOne->u64MaxNs)
    {
      pstOne->u64MaxNs = u64Ns;
    }
  }
  if (u64Ns > (uint64)DM_STAT_SLOW_MS * DM__NS_PER_MS)
  {
    if (0u != (u32Key & DM__STAT_EVENT_BIT))
    {
      DM_LOG_WARN("event 0x%04x subscriber %s took %u ms", (unsigned)(u32Key & 0xFFFFu),
                  bAfter ? "after" : "before", (unsigned)(u64Ns / DM__NS_PER_MS));
    }
    else
    {
      pcName = (NULL != g_pfStatName) ? g_pfStatName((uint)u32Key) : NULL;
      DM_LOG_WARN("linkID 0x%04x%s%s%s %s took %u ms", (unsigned)u32Key,
                  (NULL != pcName) ? " (" : "", (NULL != pcName) ? pcName : "",
                  (NULL != pcName) ? ")" : "", bAfter ? "after" : "before",
                  (unsigned)(u64Ns / DM__NS_PER_MS));
    }
  }
  return;
}

/* ============================ ② 一圈干了多久 ============================ */

static void dm__stat_on_check(dm_in dm_uv_check_st *pstCheck)
{
  uint64 u64Now = dm_hrtime();
  uint64 u64Idle = dm_uv_metrics_idle_time(dm_uv_default_loop());
  uint64 u64Busy = 0;
  (void)pstCheck;

  g_u64LoopTurns++;
  if (0u != g_u64LoopLast)
  {
    uint64 u64Cycle = u64Now - g_u64LoopLast;
    uint64 u64IdleDelta = u64Idle - g_u64LoopLastIdle;
    u64Busy = (u64Cycle > u64IdleDelta) ? (u64Cycle - u64IdleDelta) : 0u;
    if (u64Busy > g_u64LoopMaxNs)
    {
      g_u64LoopMaxNs = u64Busy;
    }
    if (u64Busy > (uint64)DM_STAT_SLOW_MS * DM__NS_PER_MS)
    {
      g_u32LoopSlow++;
      DM_LOG_WARN("loop iteration was busy for %u ms", (unsigned)(u64Busy / DM__NS_PER_MS));
    }
  }
  g_u64LoopLast = dm_hrtime(); /* 记日志本身的耗时不算进下一圈 */
  g_u64LoopLastIdle = u64Idle;
  return;
}

uint64 dm__stat_loop_turns(void)
{
  return g_u64LoopTurns;
}

/* ============================ ③ 线程池排队 ============================ */

void dm__stat_pool(dm_in uint64 u64WaitNs)
{
  uint64 u64Sum = 0;
  uint32 u32Win = 0;
  uint32 u32Idx = 0;
  uint64 u64Now = 0;

  g_au64PoolWait[g_u32PoolN % DM__POOL_WIN] = u64WaitNs;
  g_u32PoolN++;
  g_u64PoolTotalNs += u64WaitNs;
  if (u64WaitNs > g_u64PoolMaxNs)
  {
    g_u64PoolMaxNs = u64WaitNs;
  }
  u32Win = (g_u32PoolN < DM__POOL_WIN) ? g_u32PoolN : DM__POOL_WIN;
  for (u32Idx = 0; u32Idx < u32Win; u32Idx++)
  {
    u64Sum += g_au64PoolWait[u32Idx];
  }
  u64Now = dm_hrtime();
  if ((u64Sum / u32Win > (uint64)DM_STAT_POOL_MS * DM__NS_PER_MS) &&
      ((0u == g_u64PoolWarnAt) || (u64Now - g_u64PoolWarnAt > 1000u * (uint64)DM__NS_PER_MS)))
  {
    g_u64PoolWarnAt = u64Now;
    DM_LOG_WARN("work waits %u ms in the thread pool on average (last %u), threads too few?",
                (unsigned)(u64Sum / u32Win / DM__NS_PER_MS), (unsigned)u32Win);
  }
  return;
}

/* ============================ 起停 ============================ */

int dm__stat_init(void)
{
  int iErrCode = DM_SUCCESS;

  dm__stat_reset();
  (void)dm_uv_loop_configure(dm_uv_default_loop(), DM_UV_METRICS_IDLE_TIME);
  iErrCode = dm_uv_check_init(dm_uv_default_loop(), &g_stStatCheck);
  if (DM_SUCCESS == iErrCode)
  {
    iErrCode = dm_uv_check_start(&g_stStatCheck, dm__stat_on_check);
  }
  if (DM_SUCCESS != iErrCode)
  {
    return iErrCode;
  }
  dm_uv_unref((dm_uv_handle_st *)&g_stStatCheck); /* 不让循环为它活着 */
  g_bStatCheckUp = DM_TRUE;
  return DM_SUCCESS;
}

static void dm__stat_free_one(dm_in void *pvValue)
{
  dm_free_tag(pvValue);
  return;
}

void dm__stat_reset(void)
{
  dm_hash_destroy(&g_stStatTable, dm__stat_free_one);
  memset(&g_stStatTable, 0, sizeof(g_stStatTable));
  g_u64LoopLast = 0;
  g_u64LoopLastIdle = 0;
  g_u64LoopMaxNs = 0;
  g_u32LoopSlow = 0;
  g_u64LoopTurns = 0;
  memset(g_au64PoolWait, 0, sizeof(g_au64PoolWait));
  g_u32PoolN = 0;
  g_u64PoolTotalNs = 0;
  g_u64PoolMaxNs = 0;
  g_u64PoolWarnAt = 0;
  return;
}

void dm__stat_fini(void)
{
  if (g_bStatCheckUp)
  {
    (void)dm_uv_check_stop(&g_stStatCheck);
    dm_uv_close((dm_uv_handle_st *)&g_stStatCheck, NULL);
    g_bStatCheckUp = DM_FALSE;
  }
  dm__stat_reset();
  return;
}

/* ============================ 可选诊断视图导出（供 rpc/stat_rpc.c） ============================
 */

void dm__stat_set_name_cb(dm_in const char *(*pfName)(dm_in uint uiLinkID))
{
  g_pfStatName = pfName;
  return;
}

void dm__stat_each(dm_in dm__stat_row_cb pfRow, dm_in void *pvArg)
{
  uint32 u32Idx = 0;
  dm_list_node_st *pstNode = NULL;

  if ((NULL == pfRow) || (NULL == g_stStatTable.pstBuckets))
  {
    return;
  }
  for (u32Idx = 0; u32Idx < g_stStatTable.size; u32Idx++)
  {
    DM_LIST_FOREACH(&g_stStatTable.pstBuckets[u32Idx], pstNode)
    {
      const dm__stat_one_st *pstOne = (const dm__stat_one_st *)DM_HASH_NODE_ENTRY(pstNode)->value;
      dm__stat_row_st stRow = {0};
      stRow.u32Key = pstOne->u32Key;
      stRow.u32Before = pstOne->u32Before;
      stRow.u32After = pstOne->u32After;
      stRow.u64TotalNs = pstOne->u64TotalNs;
      stRow.u64MaxNs = pstOne->u64MaxNs;
      pfRow(&stRow, pvArg);
    }
  }
  return;
}

void dm__stat_summary(dm_out dm__stat_summary_st *pstOut)
{
  if (NULL == pstOut)
  {
    return;
  }
  pstOut->u64LoopMaxNs = g_u64LoopMaxNs;
  pstOut->u32LoopSlow = g_u32LoopSlow;
  pstOut->u32PoolN = g_u32PoolN;
  pstOut->u64PoolTotalNs = g_u64PoolTotalNs;
  pstOut->u64PoolMaxNs = g_u64PoolMaxNs;
  return;
}

#ifdef __cplusplus
}
#endif
