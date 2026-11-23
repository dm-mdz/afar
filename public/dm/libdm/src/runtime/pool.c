/**
 ******************************************************************************
 * @file    pool.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   自有动态泳道线程池（DD-01 §3.1，AUD-01）：
 *          - 3 条独立泳道（DEFAULT / DB / HEAVY），独立互斥锁、条件变量与 FIFO 队列；
 *          - 按需动态扩容（busy + queued > live && live < max）与空闲限时缩容（live > min）；
 *          - 工作线程退出前先调用 dm__ext_on_thread_exit() 回收 TLS SQLite 连接与槽位；
 *          - 退出线程句柄入待回收链表，由主线程经 dm_thread_join() 严格回收（零句柄/栈泄漏）；
 *          - 完工任务经 g_stPoolAsync 唤醒主循环并统一回流总队列（拆除阶段直调保证不漏释放）；
 *          - 优雅拆除使用互斥+条件变量严格等待活跃工作线程归零后，再关闭异步句柄（零 UAF）。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "pool"
#include <dm.h>
#include <dm/pool.h>
#include <dm/limits.h>
#include <dm/list.h>
#include <dm/log.h>

#include <stdatomic.h>
#include <string.h>

#include "../dm_internal.h"

#define DM__POOL_THREADS_MAX 64u
#define DM__POOL_DEFAULT_MIN 2u
#define DM__POOL_DEFAULT_MAX 8u
#define DM__POOL_DB_MIN 1u
#define DM__POOL_DB_MAX 4u
#define DM__POOL_HEAVY_MIN 1u
#define DM__POOL_HEAVY_MAX 4u
#define DM__POOL_IDLE_MS_DEFAULT 60000u
#define DM__POOL_QUEUE_CAP_DEFAULT 4096u
#define DM__POOL_PREWARM_CAP 8u

typedef struct dm__pool_job
{
  dm_list_node_st stNode;
  dm_pool_work_fn pfWork;
  dm_pool_after_fn pfAfter;
  void *pvArg;
  uint32 u32StatKey;
  uint16 u16Depth;
  uint64 u64TraceID;
  uint64 u64SubmitNs;
  uint64 u64StartNs;
  int iStatus;
} dm__pool_job_st;

typedef struct dm__pool_worker
{
  dm_list_node_st stNode;
  dm_thread_t stThread;
  dm_pool_lane_et eLane;
} dm__pool_worker_st;

typedef struct dm__pool_lane
{
  dm_mutex_t stLock;
  dm_cond_t stCond;
  dm_list_st stQueue;
  dm_pool_lane_cfg_st stCfg;
  uint16 u16Live;
  uint16 u16Busy;
  uint16 u16Ready;
  uint32 u32Queued;
  uint64 u64DoneCount;
  uint64 u64WaitMaxNs;
  uint64 u64RunMaxNs;
  bool_t bStopping;
} dm__pool_lane_st;

/* 用户经 dm_pool_set_lane 配置的泳道覆写（跨 dm_main_ex 保留，传 NULL 清除） */
static dm_pool_lane_cfg_st g_astUserLaneCfg[DM_LANE_MAX];
static bool_t g_abUserLaneSet[DM_LANE_MAX];

/* 运行期池状态 */
static dm__pool_lane_st g_astLane[DM_LANE_MAX];
static dm_mutex_t g_stDoneLock;
static dm_list_st g_stDoneList;
static dm_mutex_t g_stReapLock;
static dm_list_st g_stReapList;
static dm_mutex_t g_stLiveLock;
static dm_cond_t g_stLiveZeroCond;
static dm_cond_t g_stReadyCond;
static uint32 g_u32TotalLive = 0;

static dm_async_st g_stPoolAsync;
static bool_t g_bPoolSyncInit = DM_FALSE;
static bool_t g_bPoolUp = DM_FALSE;
static bool_t g_bPoolAsyncRef = DM_FALSE;
static uint32 g_u32PoolInflight = 0;
static uint g_uiPoolReportedThreads = DM__POOL_DEFAULT_MAX;
static _Atomic uint64 g_u64ReapedThreads = 0;

/* ============================ 内部工具 ============================ */

static void dm__pool_default_cfg(in dm_pool_lane_et eLane, in const dm_cfg_st *pstCfg,
                                 out dm_pool_lane_cfg_st *pstOut)
{
  uint32 u32Idle = ((NULL != pstCfg) && (0u != pstCfg->u32PoolIdleMs)) ? pstCfg->u32PoolIdleMs
                                                                       : DM__POOL_IDLE_MS_DEFAULT;

  memset(pstOut, 0, sizeof(*pstOut));
  pstOut->u32IdleMs = u32Idle;
  pstOut->u32QueueCap = DM__POOL_QUEUE_CAP_DEFAULT;

  if (DM_LANE_DEFAULT == eLane)
  {
    if ((NULL != pstCfg) && (0u != pstCfg->uiWorkThreads))
    {
      uint uiT = pstCfg->uiWorkThreads;
      if (uiT > DM__POOL_THREADS_MAX)
      {
        DM_LOG_WARN("uiWorkThreads %u is over %u, using %u", uiT, DM__POOL_THREADS_MAX,
                    DM__POOL_THREADS_MAX);
        uiT = DM__POOL_THREADS_MAX;
      }
      pstOut->u16MinThreads = (uint16)uiT;
      pstOut->u16MaxThreads = (uint16)uiT;
    }
    else
    {
      uint16 u16Min = ((NULL != pstCfg) && (0u != pstCfg->u16PoolMin)) ? pstCfg->u16PoolMin
                                                                       : DM__POOL_DEFAULT_MIN;
      uint16 u16Max = ((NULL != pstCfg) && (0u != pstCfg->u16PoolMax)) ? pstCfg->u16PoolMax
                                                                       : DM__POOL_DEFAULT_MAX;
      if (u16Max > DM__POOL_THREADS_MAX)
      {
        u16Max = DM__POOL_THREADS_MAX;
      }
      if (u16Min > u16Max)
      {
        u16Min = u16Max;
      }
      pstOut->u16MinThreads = u16Min;
      pstOut->u16MaxThreads = u16Max;
    }
  }
  else if (DM_LANE_DB == eLane)
  {
    pstOut->u16MinThreads = DM__POOL_DB_MIN;
    pstOut->u16MaxThreads = DM__POOL_DB_MAX;
  }
  else
  {
    pstOut->u16MinThreads = DM__POOL_HEAVY_MIN;
    pstOut->u16MaxThreads = DM__POOL_HEAVY_MAX;
  }

  if (g_abUserLaneSet[eLane])
  {
    *pstOut = g_astUserLaneCfg[eLane];
    if (0u == pstOut->u32IdleMs)
    {
      pstOut->u32IdleMs = u32Idle;
    }
    if (0u == pstOut->u32QueueCap)
    {
      pstOut->u32QueueCap = DM__POOL_QUEUE_CAP_DEFAULT;
    }
    if (0u == pstOut->u16MaxThreads)
    {
      pstOut->u16MaxThreads = 1u;
    }
    if (pstOut->u16MaxThreads > DM__POOL_THREADS_MAX)
    {
      pstOut->u16MaxThreads = DM__POOL_THREADS_MAX;
    }
    if (pstOut->u16MinThreads > pstOut->u16MaxThreads)
    {
      pstOut->u16MinThreads = pstOut->u16MaxThreads;
    }
  }
  return;
}

/* 主线程回收所有已退出工作线程句柄（AUD-01：uv_thread_join 防栈/HANDLE 泄漏） */
static void dm__pool_reap_workers(void)
{
  dm_list_st stLocal = {0};
  dm_list_node_st *pstNode = NULL;

  dm_list_init(&stLocal);
  dm_mutex_lock(&g_stReapLock);
  while (NULL != (pstNode = dm_list_del_head(&g_stReapList)))
  {
    dm_list_add_tail(&stLocal, pstNode);
  }
  dm_mutex_unlock(&g_stReapLock);

  while (NULL != (pstNode = dm_list_del_head(&stLocal)))
  {
    dm__pool_worker_st *pstW = DM_LIST_ENTRY(pstNode, dm__pool_worker_st, stNode);
    (void)dm_thread_join(&pstW->stThread);
    atomic_fetch_add_explicit(&g_u64ReapedThreads, 1u, memory_order_relaxed);
    dm_free_tag(pstW);
  }
  return;
}

static void dm__pool_async_update_ref(void)
{
  if (!g_bPoolUp)
  {
    return;
  }
  if ((g_u32PoolInflight > 0u) && (!g_bPoolAsyncRef))
  {
    dm_ref((dm_handle_st *)&g_stPoolAsync);
    g_bPoolAsyncRef = DM_TRUE;
  }
  else if ((0u == g_u32PoolInflight) && g_bPoolAsyncRef)
  {
    dm_unref((dm_handle_st *)&g_stPoolAsync);
    g_bPoolAsyncRef = DM_FALSE;
  }
  return;
}

static void dm__pool_finish_job(io dm__pool_job_st *pstJob)
{
  dm_pool_after_fn pfAfter = pstJob->pfAfter;
  void *pvArg = pstJob->pvArg;
  int iStatus = pstJob->iStatus;
  uint32 u32StatKey = pstJob->u32StatKey;
  uint64 u64Start = pstJob->u64StartNs;
  uint64 u64Submit = pstJob->u64SubmitNs;
  uint16 u16Depth = pstJob->u16Depth;
  uint64 u64TraceID = pstJob->u64TraceID;

  if (g_u32PoolInflight > 0u)
  {
    g_u32PoolInflight--;
  }
  dm__pool_async_update_ref();
  dm_free_tag(pstJob);

  if (0u != u64Start)
  {
    uint64 u64WaitNs = (u64Start >= u64Submit) ? (u64Start - u64Submit) : 0u;
    dm__stat_pool(u64WaitNs);
  }
  if (NULL != pfAfter)
  {
    bool_t bPrevIn = DM_FALSE;
    uint16 u16PrevDepth = 0;
    uint64 u64PrevTrace = 0;
    uint64 u64T0 = dm_hrtime();

    dm__dispatch_ctx_get(&bPrevIn, &u16PrevDepth, &u64PrevTrace);
    dm__dispatch_ctx_set(DM_TRUE, u16Depth, (0u != u64TraceID) ? u64TraceID : dm__trace_next());
    pfAfter(pvArg, iStatus);
    dm__dispatch_ctx_set(bPrevIn, u16PrevDepth, u64PrevTrace);
    {
      uint64 u64End = dm_hrtime();
      uint64 u64Dur = (u64End >= u64T0) ? (u64End - u64T0) : 0u;
      dm__stat_call(u32StatKey, u64Dur, DM_TRUE);
    }
  }
  return;
}

static void dm__pool_run_after_item(io dm__qitem_st *pstItem)
{
  dm__pool_job_st *pstJob = (dm__pool_job_st *)pstItem->pvCtx;
  if (NULL != pstJob)
  {
    dm__pool_finish_job(pstJob);
  }
  return;
}

static void dm__pool_drain_done(in bool_t bDirect)
{
  dm_list_st stLocal = {0};
  dm_list_node_st *pstNode = NULL;

  dm_list_init(&stLocal);
  dm_mutex_lock(&g_stDoneLock);
  while (NULL != (pstNode = dm_list_del_head(&g_stDoneList)))
  {
    dm_list_add_tail(&stLocal, pstNode);
  }
  dm_mutex_unlock(&g_stDoneLock);

  while (NULL != (pstNode = dm_list_del_head(&stLocal)))
  {
    dm__pool_job_st *pstJob = DM_LIST_ENTRY(pstNode, dm__pool_job_st, stNode);
    if (bDirect || (dm__phase() >= DM__PHASE_TEARDOWN))
    {
      dm__pool_finish_job(pstJob);
    }
    else
    {
      dm__qitem_st *pstItem = dm__qitem_new();
      if (NULL == pstItem)
      {
        dm__pool_finish_job(pstJob);
        continue;
      }
      pstItem->pfRun = dm__pool_run_after_item;
      pstItem->pvCtx = pstJob;
      pstItem->u16Depth = pstJob->u16Depth;
      pstItem->u64TraceID = pstJob->u64TraceID;
      if (DM_SUCCESS != dm__queue_push(pstItem))
      {
        /* 队列正在关闭或不可用时直调 pfAfter，保证内存释放与计数归零 */
        dm__pool_finish_job(pstJob);
      }
    }
  }
  return;
}

static void dm__pool_on_async(in dm_async_st *pstAsync)
{
  (void)pstAsync;
  if (!g_bPoolUp)
  {
    return;
  }
  dm__pool_reap_workers();
  dm__pool_drain_done(DM_FALSE);
  return;
}

/* ============================ 工作线程主循环 ============================ */

static void dm__pool_worker_main(io void *pvArg)
{
  dm__pool_worker_st *pstW = (dm__pool_worker_st *)pvArg;
  dm__pool_lane_st *pstLane = &g_astLane[pstW->eLane];
  bool_t bSignaledReady = DM_FALSE;

  dm_mutex_lock(&pstLane->stLock);
  for (;;)
  {
    dm_list_node_st *pstNode = NULL;
    dm__pool_job_st *pstJob = NULL;
    uint64 u64Start = 0;
    uint64 u64End = 0;
    uint64 u64WaitNs = 0;
    uint64 u64RunNs = 0;

    if (!bSignaledReady)
    {
      bSignaledReady = DM_TRUE;
      if (DM_LANE_DEFAULT == pstW->eLane)
      {
        pstLane->u16Ready++;
        dm_cond_broadcast(&g_stReadyCond);
      }
    }

    while ((NULL == (pstNode = dm_list_del_head(&pstLane->stQueue))) && (!pstLane->bStopping))
    {
      uint32 u32IdleMs =
          (pstLane->stCfg.u32IdleMs > 0x7FFFFFFFu) ? 0x7FFFFFFFu : pstLane->stCfg.u32IdleMs;
      uint64 u64TimeoutNs = (uint64)u32IdleMs * 1000000ULL;
      int iRc = dm_cond_timedwait(&pstLane->stCond, &pstLane->stLock, u64TimeoutNs);
      if ((DM_ETIMEDOUT == iRc) && dm_list_is_empty(&pstLane->stQueue) &&
          (pstLane->u16Live > pstLane->stCfg.u16MinThreads))
      {
        break; /* 闲时缩容自退出 */
      }
    }

    if (NULL == pstNode)
    {
      break; /* 正在停止或闲时缩容 */
    }

    pstJob = DM_LIST_ENTRY(pstNode, dm__pool_job_st, stNode);
    if (pstLane->u32Queued > 0u)
    {
      pstLane->u32Queued--;
    }
    pstLane->u16Busy++;
    dm_mutex_unlock(&pstLane->stLock);

    u64Start = dm_hrtime();
    pstJob->u64StartNs = u64Start;
    u64WaitNs = (u64Start >= pstJob->u64SubmitNs) ? (u64Start - pstJob->u64SubmitNs) : 0u;
    if (NULL != pstJob->pfWork)
    {
      pstJob->pfWork(pstJob->pvArg);
      dm__ext_after_work(pstJob->u32StatKey);
    }
    u64End = dm_hrtime();
    u64RunNs = (u64End >= u64Start) ? (u64End - u64Start) : 0u;

    dm_mutex_lock(&pstLane->stLock);
    if (pstLane->u16Busy > 0u)
    {
      pstLane->u16Busy--;
    }
    pstLane->u64DoneCount++;
    if (u64WaitNs > pstLane->u64WaitMaxNs)
    {
      pstLane->u64WaitMaxNs = u64WaitNs;
    }
    if (u64RunNs > pstLane->u64RunMaxNs)
    {
      pstLane->u64RunMaxNs = u64RunNs;
    }
    dm_mutex_unlock(&pstLane->stLock);

    dm_mutex_lock(&g_stDoneLock);
    dm_list_add_tail(&g_stDoneList, &pstJob->stNode);
    dm_mutex_unlock(&g_stDoneLock);
    (void)dm_async_send(&g_stPoolAsync);

    dm_mutex_lock(&pstLane->stLock);
  }

  /* 退出前更新本泳道存活计数 */
  if (pstLane->u16Live > 0u)
  {
    pstLane->u16Live--;
  }
  if ((DM_LANE_DEFAULT == pstW->eLane) && (pstLane->u16Ready > 0u))
  {
    pstLane->u16Ready--;
  }
  dm_mutex_unlock(&pstLane->stLock);

  /* AUD-01：工作线程退出前强制回收 TLS SQLite 连接与槽位 */
  dm__ext_on_thread_exit();

  /* 挂入主线程待 join 链表并唤醒主循环 */
  dm_mutex_lock(&g_stReapLock);
  dm_list_add_tail(&g_stReapList, &pstW->stNode);
  dm_mutex_unlock(&g_stReapLock);
  (void)dm_async_send(&g_stPoolAsync);

  /* 最后递减总存活数，通知 dm__pool_shutdown 可以安全回收句柄 */
  dm_mutex_lock(&g_stLiveLock);
  if (g_u32TotalLive > 0u)
  {
    g_u32TotalLive--;
  }
  if (0u == g_u32TotalLive)
  {
    dm_cond_broadcast(&g_stLiveZeroCond);
  }
  dm_mutex_unlock(&g_stLiveLock);
  return;
}

/* 必须在持有 pstLane->stLock 时调用，保证 pstW->stThread 写完前新线程无法退出 */
static int dm__pool_spawn_locked(in dm_pool_lane_et eLane)
{
  dm__pool_lane_st *pstLane = &g_astLane[eLane];
  dm__pool_worker_st *pstW = NULL;
  int iErr = DM_SUCCESS;

  pstW = (dm__pool_worker_st *)dm_calloc_tag(1u, sizeof(dm__pool_worker_st), DM_MEM_TAG_POOL);
  if (NULL == pstW)
  {
    return DM_ALLOC;
  }
  pstW->eLane = eLane;

  dm_mutex_lock(&g_stLiveLock);
  g_u32TotalLive++;
  dm_mutex_unlock(&g_stLiveLock);

  iErr = dm_thread_create(&pstW->stThread, dm__pool_worker_main, pstW);
  if (DM_SUCCESS != iErr)
  {
    dm_mutex_lock(&g_stLiveLock);
    if (g_u32TotalLive > 0u)
    {
      g_u32TotalLive--;
    }
    if (0u == g_u32TotalLive)
    {
      dm_cond_broadcast(&g_stLiveZeroCond);
    }
    dm_mutex_unlock(&g_stLiveLock);
    dm_free_tag(pstW);
    return ((DM_ENOMEM == iErr) || (DM_ALLOC == iErr)) ? DM_ALLOC : DM_FAILD;
  }
  pstLane->u16Live++;
  return DM_SUCCESS;
}

/* ============================ 内部生命周期 ============================ */

static int dm__pool_init_sync(void)
{
  uint uiIdx = 0;

  if (g_bPoolSyncInit)
  {
    return DM_SUCCESS;
  }
  if (DM_SUCCESS != dm_mutex_init(&g_stDoneLock))
  {
    return DM_FAILD;
  }
  if (DM_SUCCESS != dm_mutex_init(&g_stReapLock))
  {
    dm_mutex_destroy(&g_stDoneLock);
    return DM_FAILD;
  }
  if (DM_SUCCESS != dm_mutex_init(&g_stLiveLock))
  {
    dm_mutex_destroy(&g_stReapLock);
    dm_mutex_destroy(&g_stDoneLock);
    return DM_FAILD;
  }
  if (DM_SUCCESS != dm_cond_init(&g_stLiveZeroCond))
  {
    dm_mutex_destroy(&g_stLiveLock);
    dm_mutex_destroy(&g_stReapLock);
    dm_mutex_destroy(&g_stDoneLock);
    return DM_FAILD;
  }
  if (DM_SUCCESS != dm_cond_init(&g_stReadyCond))
  {
    dm_cond_destroy(&g_stLiveZeroCond);
    dm_mutex_destroy(&g_stLiveLock);
    dm_mutex_destroy(&g_stReapLock);
    dm_mutex_destroy(&g_stDoneLock);
    return DM_FAILD;
  }
  for (uiIdx = 0; uiIdx < (uint)DM_LANE_MAX; uiIdx++)
  {
    bool_t bLockOk = (DM_SUCCESS == dm_mutex_init(&g_astLane[uiIdx].stLock));
    bool_t bCondOk = bLockOk && (DM_SUCCESS == dm_cond_init(&g_astLane[uiIdx].stCond));
    if (!bCondOk)
    {
      uint uiBack = 0;
      if (bLockOk)
      {
        dm_mutex_destroy(&g_astLane[uiIdx].stLock);
      }
      for (uiBack = 0; uiBack < uiIdx; uiBack++)
      {
        dm_cond_destroy(&g_astLane[uiBack].stCond);
        dm_mutex_destroy(&g_astLane[uiBack].stLock);
      }
      dm_cond_destroy(&g_stReadyCond);
      dm_cond_destroy(&g_stLiveZeroCond);
      dm_mutex_destroy(&g_stLiveLock);
      dm_mutex_destroy(&g_stReapLock);
      dm_mutex_destroy(&g_stDoneLock);
      return DM_FAILD;
    }
  }
  g_bPoolSyncInit = DM_TRUE;
  return DM_SUCCESS;
}

void dm__pool_destroy_sync(void)
{
  uint uiIdx = 0;

  if (!g_bPoolSyncInit)
  {
    return;
  }
  for (uiIdx = 0; uiIdx < (uint)DM_LANE_MAX; uiIdx++)
  {
    dm_cond_destroy(&g_astLane[uiIdx].stCond);
    dm_mutex_destroy(&g_astLane[uiIdx].stLock);
  }
  dm_cond_destroy(&g_stReadyCond);
  dm_cond_destroy(&g_stLiveZeroCond);
  dm_mutex_destroy(&g_stLiveLock);
  dm_mutex_destroy(&g_stReapLock);
  dm_mutex_destroy(&g_stDoneLock);
  g_bPoolSyncInit = DM_FALSE;
  return;
}

int dm__pool_init(in const dm_cfg_st *pstCfg)
{
  uint uiIdx = 0;
  int iErr = DM_SUCCESS;
  uint16 u16Prewarm = 0;

  if (g_bPoolUp)
  {
    return DM_SUCCESS;
  }
  iErr = dm__pool_init_sync();
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  dm_list_init(&g_stDoneList);
  dm_list_init(&g_stReapList);
  g_u32TotalLive = 0;
  g_u32PoolInflight = 0;
  g_bPoolAsyncRef = DM_FALSE;

  for (uiIdx = 0; uiIdx < (uint)DM_LANE_MAX; uiIdx++)
  {
    dm__pool_lane_st *pstLane = &g_astLane[uiIdx];
    dm_list_init(&pstLane->stQueue);
    dm__pool_default_cfg((dm_pool_lane_et)uiIdx, pstCfg, &pstLane->stCfg);
    pstLane->u16Live = 0;
    pstLane->u16Busy = 0;
    pstLane->u16Ready = 0;
    pstLane->u32Queued = 0;
    pstLane->u64DoneCount = 0;
    pstLane->u64WaitMaxNs = 0;
    pstLane->u64RunMaxNs = 0;
    pstLane->bStopping = DM_FALSE;
  }

  g_uiPoolReportedThreads = (uint)g_astLane[DM_LANE_DEFAULT].stCfg.u16MaxThreads;

  iErr = dm_async_init(dm_default_loop(), &g_stPoolAsync, dm__pool_on_async);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  dm_unref((dm_handle_st *)&g_stPoolAsync);
  g_bPoolUp = DM_TRUE;

  /* 若显式配了 uiWorkThreads 或 u16PoolMin（且 <= 8），预先拉起常驻线程并等待进入待命态，
   * 保证 pfInit 中立即投任务 + dm_quit 时首任务确定性立即开始（T-LIFE-15） */
  if ((NULL != pstCfg) && ((0u != pstCfg->uiWorkThreads) || (0u != pstCfg->u16PoolMin)))
  {
    u16Prewarm = g_astLane[DM_LANE_DEFAULT].stCfg.u16MinThreads;
    if (u16Prewarm > DM__POOL_PREWARM_CAP)
    {
      u16Prewarm = DM__POOL_PREWARM_CAP;
    }
    dm_mutex_lock(&g_astLane[DM_LANE_DEFAULT].stLock);
    for (uiIdx = 0; uiIdx < (uint)u16Prewarm; uiIdx++)
    {
      if (DM_SUCCESS != dm__pool_spawn_locked(DM_LANE_DEFAULT))
      {
        break;
      }
    }
    while (g_astLane[DM_LANE_DEFAULT].u16Ready < g_astLane[DM_LANE_DEFAULT].u16Live)
    {
      dm_cond_wait(&g_stReadyCond, &g_astLane[DM_LANE_DEFAULT].stLock);
    }
    dm_mutex_unlock(&g_astLane[DM_LANE_DEFAULT].stLock);
  }
  return DM_SUCCESS;
}

uint dm__threads_count(void)
{
  return g_uiPoolReportedThreads;
}

uint32 dm__work_inflight(void)
{
  return g_u32PoolInflight;
}

void dm__work_cancel_queued(void)
{
  dm_list_st stCanceled = {0};
  dm_list_node_st *pstNode = NULL;
  uint uiIdx = 0;

  if (!g_bPoolUp)
  {
    return;
  }
  dm_list_init(&stCanceled);
  for (uiIdx = 0; uiIdx < (uint)DM_LANE_MAX; uiIdx++)
  {
    dm__pool_lane_st *pstLane = &g_astLane[uiIdx];
    dm_mutex_lock(&pstLane->stLock);
    while (NULL != (pstNode = dm_list_del_head(&pstLane->stQueue)))
    {
      dm_list_add_tail(&stCanceled, pstNode);
    }
    pstLane->u32Queued = 0;
    dm_mutex_unlock(&pstLane->stLock);
  }

  while (NULL != (pstNode = dm_list_del_head(&stCanceled)))
  {
    dm__pool_job_st *pstJob = DM_LIST_ENTRY(pstNode, dm__pool_job_st, stNode);
    pstJob->iStatus = DM_ECANCELED;
    dm__pool_finish_job(pstJob);
  }
  return;
}

void dm__pool_shutdown(void)
{
  uint uiIdx = 0;

  if (!g_bPoolUp)
  {
    return;
  }
  /* 1. 取消所有尚未出队的排队任务（pfAfter 收到 DM_ECANCELED） */
  dm__work_cancel_queued();

  /* 2. 通知所有泳道停止并唤醒空闲工作线程 */
  for (uiIdx = 0; uiIdx < (uint)DM_LANE_MAX; uiIdx++)
  {
    dm__pool_lane_st *pstLane = &g_astLane[uiIdx];
    dm_mutex_lock(&pstLane->stLock);
    pstLane->bStopping = DM_TRUE;
    dm_cond_broadcast(&pstLane->stCond);
    dm_mutex_unlock(&pstLane->stLock);
  }

  /* 3. AUD-01：严格等待全部工作线程执行完 pfOnThreadExit 并归零后再回收异步句柄（零 UAF） */
  dm_mutex_lock(&g_stLiveLock);
  while (g_u32TotalLive > 0u)
  {
    dm_cond_wait(&g_stLiveZeroCond, &g_stLiveLock);
  }
  dm_mutex_unlock(&g_stLiveLock);

  /* 4. 回收全部已退出工作线程句柄并排干尾包完工任务 */
  dm__pool_reap_workers();
  dm__pool_drain_done(DM_TRUE);

  /* 5. 关闭异步唤醒句柄（此时零工作线程存活，不可能再触发 dm_async_send） */
  g_bPoolUp = DM_FALSE;
  g_bPoolAsyncRef = DM_FALSE;
  if (!dm_is_closing((dm_handle_st *)&g_stPoolAsync))
  {
    dm_close((dm_handle_st *)&g_stPoolAsync, NULL);
  }
  return;
}

int dm__pool_submit_internal(in dm_pool_lane_et eLane, in dm_pool_work_fn pfWork,
                             in dm_pool_after_fn pfAfter, in void *pvArg, in uint32 u32StatKey,
                             in uint16 u16Depth, in uint64 u64TraceID)
{
  dm__pool_lane_st *pstLane = NULL;
  dm__pool_job_st *pstJob = NULL;
  int iErr = DM_SUCCESS;

  if (((uint)eLane >= (uint)DM_LANE_MAX) || ((NULL == pfWork) && (NULL == pfAfter)))
  {
    return DM_INVALID;
  }
  DM__CHECK_LOOP();
  if (dm__phase() >= DM__PHASE_TEARDOWN)
  {
    return DM_SHUTTING_DOWN;
  }
  if ((!g_bPoolUp) && (DM__PHASE_NONE == dm__phase()))
  {
    iErr = dm__pool_init(NULL);
    if (DM_SUCCESS != iErr)
    {
      return iErr;
    }
  }
  if (!g_bPoolUp)
  {
    return DM_SHUTTING_DOWN;
  }

  pstJob = (dm__pool_job_st *)dm_calloc_tag(1u, sizeof(dm__pool_job_st), DM_MEM_TAG_POOL);
  if (NULL == pstJob)
  {
    return DM_ALLOC;
  }
  pstJob->pfWork = pfWork;
  pstJob->pfAfter = pfAfter;
  pstJob->pvArg = pvArg;
  pstJob->u32StatKey = u32StatKey;
  pstJob->u16Depth = u16Depth;
  pstJob->u64TraceID = u64TraceID;
  pstJob->u64SubmitNs = dm_hrtime();
  pstJob->iStatus = DM_SUCCESS;

  pstLane = &g_astLane[eLane];
  dm_mutex_lock(&pstLane->stLock);
  if (pstLane->bStopping)
  {
    dm_mutex_unlock(&pstLane->stLock);
    dm_free_tag(pstJob);
    return DM_SHUTTING_DOWN;
  }
  if (pstLane->u32Queued >= pstLane->stCfg.u32QueueCap)
  {
    dm_mutex_unlock(&pstLane->stLock);
    dm_free_tag(pstJob);
    return DM_BUSY;
  }
  dm_list_add_tail(&pstLane->stQueue, &pstJob->stNode);
  pstLane->u32Queued++;

  /* 动态扩容：busy + queued > live 且未达 max，当场拉起 1 条工作线程 */
  if (((uint32)pstLane->u16Busy + pstLane->u32Queued > (uint32)pstLane->u16Live) &&
      (pstLane->u16Live < pstLane->stCfg.u16MaxThreads))
  {
    iErr = dm__pool_spawn_locked(eLane);
    if ((DM_SUCCESS != iErr) && (0u == pstLane->u16Live))
    {
      /* 一条存活线程都没有且扩容失败：回退本次入队 */
      dm_list_del_node(&pstJob->stNode);
      pstLane->u32Queued--;
      dm_mutex_unlock(&pstLane->stLock);
      dm_free_tag(pstJob);
      return iErr;
    }
  }
  dm_cond_signal(&pstLane->stCond);
  dm_mutex_unlock(&pstLane->stLock);

  g_u32PoolInflight++;
  dm__pool_async_update_ref();
  return DM_SUCCESS;
}

/* ============================ 公共 API ============================ */

int dm_pool_set_lane(in dm_pool_lane_et eLane, in const dm_pool_lane_cfg_st *pstCfg)
{
  DM__CHECK_LOOP();
  if ((uint)eLane >= (uint)DM_LANE_MAX)
  {
    return DM_INVALID;
  }
  if (g_bPoolUp)
  {
    return DM_LINK_ERR_STATE;
  }
  if (NULL == pstCfg)
  {
    memset(&g_astUserLaneCfg[eLane], 0, sizeof(g_astUserLaneCfg[eLane]));
    g_abUserLaneSet[eLane] = DM_FALSE;
    return DM_SUCCESS;
  }
  if ((0u == pstCfg->u16MaxThreads) || (pstCfg->u16MaxThreads > DM__POOL_THREADS_MAX) ||
      (pstCfg->u16MinThreads > pstCfg->u16MaxThreads) || (pstCfg->u32IdleMs > 0x7FFFFFFFu))
  {
    return DM_INVALID;
  }
  g_astUserLaneCfg[eLane] = *pstCfg;
  g_abUserLaneSet[eLane] = DM_TRUE;
  return DM_SUCCESS;
}

int dm_pool_submit(in dm_pool_lane_et eLane, in dm_pool_work_fn pfWork, in dm_pool_after_fn pfAfter,
                   in void *pvArg)
{
  uint16 u16Depth = 0;
  uint64 u64TraceID = 0;

  dm__dispatch_ctx_get(NULL, &u16Depth, &u64TraceID);
  return dm__pool_submit_internal(eLane, pfWork, pfAfter, pvArg, 0u, u16Depth, u64TraceID);
}

int dm_pool_stat(in dm_pool_lane_et eLane, out dm_pool_stat_st *pstOut)
{
  dm__pool_lane_st *pstLane = NULL;

  DM__CHECK_LOOP();
  if (((uint)eLane >= (uint)DM_LANE_MAX) || (NULL == pstOut))
  {
    return DM_INVALID;
  }
  memset(pstOut, 0, sizeof(*pstOut));
  if (!g_bPoolUp)
  {
    return DM_SUCCESS;
  }
  pstLane = &g_astLane[eLane];
  dm_mutex_lock(&pstLane->stLock);
  pstOut->u16LiveThreads = pstLane->u16Live;
  pstOut->u16BusyThreads = pstLane->u16Busy;
  pstOut->u32Queued = pstLane->u32Queued;
  pstOut->u64DoneCount = pstLane->u64DoneCount;
  pstOut->u64WaitMaxNs = pstLane->u64WaitMaxNs;
  pstOut->u64RunMaxNs = pstLane->u64RunMaxNs;
  dm_mutex_unlock(&pstLane->stLock);
  return DM_SUCCESS;
}

uint64 dm_pool_reaped_threads(void)
{
  return atomic_load_explicit(&g_u64ReapedThreads, memory_order_relaxed);
}

#ifdef __cplusplus
}
#endif
