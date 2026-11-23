/**
 ******************************************************************************
 * @file    queue.c
 * @author  MDM
 * @version 0.0.0.3
 * @brief   总队列、停读背压、跳数熔断（DM_MAX_DISPATCH_DEPTH）、u64TraceID、
 *          时间片批处理（64 条 / 4ms）与跨线程安全唤醒 dm_queue_post_async（DD-01 §3.2,
 *          AUD-06/08/11）。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "link"
#include <dm.h>
#include <dm/frame.h>
#include <dm/limits.h>
#include <dm/link.h>
#include <dm/list.h>
#include <dm/log.h>

#include <stdatomic.h>
#include <stdint.h> /* SIZE_MAX */
#include <string.h> /* memset / memcpy */

#include "../dm_internal.h"

#define DM__QUEUE_MAX_DEFAULT (64u * 1024u * 1024u)
#define DM__ASYNC_PENDING_MAX 65536u

typedef struct dm__async_item
{
  dm_list_node_st stNode;
  uint uiLinkID;
  dm__qbuf_st *pstBuf;
} dm__async_item_st;

static dm_list_st g_stQueue; /* 排着的消息 */
static size_t g_uQueueLen = 0;
static _Atomic size_t g_uQueueLenAtomic = 0;
static uint64 g_u64QueueExt = 0; /* 外面来的占了多少字节 */
static uint64 g_u64QueueMax = DM__QUEUE_MAX_DEFAULT;
static _Atomic uint64 g_u64QueueExtAtomic = 0;
static _Atomic uint64 g_u64QueueMaxAtomic = DM__QUEUE_MAX_DEFAULT;
static _Atomic uint64 g_u64AsyncDrainingAtomic = 0;
static bool_t g_bQueueFull = DM_FALSE; /* 正在因为满了停读 */
static dm_idle_st g_stQueueIdle;
static dm_async_st g_stQueueAsync;
static bool_t g_bQueueUp = DM_FALSE; /* idle/async 句柄已 init */
static bool_t g_bQueueIdleOn = DM_FALSE;
static dm_list_st g_stFlows; /* 登记了的流（连接、子进程管道） */

/* 派发上下文与跳数熔断（仅主循环线程读写） */
static bool_t g_bInDispatch = DM_FALSE;
static uint16 g_u16CurDepth = 0;
static uint64 g_u64CurTraceID = 0;
static uint64 g_u64TraceSeq = 0;
static uint16 g_u16MaxDepthSeen = 0;
static bool_t g_bLoopAssert = DM_TRUE;

/* 跨线程 dm_queue_post_async 同步原语（进程级单次初始化，绝不销毁以防退出后并发调 UAF） */
static dm_once_t g_stAsyncOnce = DM_ONCE_INIT;
static dm_mutex_t g_stAsyncLock;
static bool_t g_bAsyncLockReady = DM_FALSE;
static dm_list_st g_stAsyncList;
static uint64 g_u64AsyncPendingBytes = 0;
static uint32 g_u32AsyncPendingCount = 0;
static _Atomic bool_t g_bAsyncAccepting = DM_FALSE;

static void dm__async_lock_init(void)
{
  if (DM_SUCCESS == dm_mutex_init(&g_stAsyncLock))
  {
    dm_list_init(&g_stAsyncList);
    g_bAsyncLockReady = DM_TRUE;
  }
  return;
}

static void dm__queue_ext_sync(void)
{
  atomic_store_explicit(&g_u64QueueExtAtomic, g_u64QueueExt, memory_order_release);
  return;
}

/* ============================ 负载 ============================ */

dm__qbuf_st *dm__qbuf_new_ex(in const void *pvData, in uint32 u32Len, in uint8 u8Flags)
{
  dm__qbuf_st *pstBuf = NULL;
  size_t uLen = (size_t)u32Len;

  if ((0u == uLen) || (uLen > (SIZE_MAX - sizeof(dm__qbuf_st))))
  {
    return NULL;
  }
  pstBuf = (dm__qbuf_st *)dm_malloc_tag(sizeof(dm__qbuf_st) + uLen, DM_MEM_TAG_QUEUE);
  if (NULL == pstBuf)
  {
    return NULL;
  }
  atomic_init(&pstBuf->u32Refs, 1u);
  pstBuf->u32Len = u32Len;
  pstBuf->u8Flags = u8Flags;
  memset(pstBuf->au8Pad, 0, sizeof(pstBuf->au8Pad));
  if (NULL != pvData)
  {
    memcpy(pstBuf->au8Data, pvData, u32Len);
  }
  return pstBuf;
}

dm__qbuf_st *dm__qbuf_new(in const void *pvData, in uint32 u32Len)
{
  return dm__qbuf_new_ex(pvData, u32Len, 0u);
}

void dm__qbuf_get(io dm__qbuf_st *pstBuf)
{
  if (NULL != pstBuf)
  {
    atomic_fetch_add_explicit(&pstBuf->u32Refs, 1u, memory_order_relaxed);
  }
  return;
}

void dm__qbuf_put(io dm__qbuf_st *pstBuf)
{
  if (NULL == pstBuf)
  {
    return;
  }
  if (1u == atomic_fetch_sub_explicit(&pstBuf->u32Refs, 1u, memory_order_acq_rel))
  {
    /* AUD-06：带 DM_FRAME_F_SENSITIVE 标志的 qbuf 释放前必须安全擦零 */
    if (0u != (pstBuf->u8Flags & DM_FRAME_F_SENSITIVE))
    {
      dm_secure_zero(pstBuf->au8Data, pstBuf->u32Len);
    }
    dm_free_tag(pstBuf);
  }
  return;
}

/* ============================ 派发跳数与追踪 ============================ */

uint64 dm__trace_next(void)
{
  g_u64TraceSeq++;
  if (0u == g_u64TraceSeq)
  {
    g_u64TraceSeq = 1u;
  }
  return g_u64TraceSeq;
}

void dm__dispatch_ctx_get(out bool_t *pbInDispatch, out uint16 *pu16Depth, out uint64 *pu64TraceID)
{
  if (NULL != pbInDispatch)
  {
    *pbInDispatch = g_bInDispatch;
  }
  if (NULL != pu16Depth)
  {
    *pu16Depth = g_u16CurDepth;
  }
  if (NULL != pu64TraceID)
  {
    *pu64TraceID = g_u64CurTraceID;
  }
  return;
}

void dm__dispatch_ctx_set(in bool_t bInDispatch, in uint16 u16Depth, in uint64 u64TraceID)
{
  g_bInDispatch = bInDispatch;
  g_u16CurDepth = u16Depth;
  g_u64CurTraceID = u64TraceID;
  return;
}

int dm__dispatch_next_hop(in bool_t bExternal, in uint uiTargetID, out uint16 *pu16Depth,
                          out uint64 *pu64TraceID)
{
  uint16 u16Next = 0;
  uint64 u64Trace = 0;

  if (bExternal || (!g_bInDispatch))
  {
    u16Next = 0u;
    u64Trace = dm__trace_next();
  }
  else
  {
    u16Next = (g_u16CurDepth < 0xFFFFu) ? (uint16)(g_u16CurDepth + 1u) : 0xFFFFu;
    u64Trace = (0u != g_u64CurTraceID) ? g_u64CurTraceID : dm__trace_next();
    if (u16Next > (uint16)DM_MAX_DISPATCH_DEPTH)
    {
      DM_LOG_ERROR("dispatch depth %u exceeded (trace=%llu, target=0x%04x)", (unsigned)u16Next,
                   (unsigned long long)u64Trace, uiTargetID);
      if (g_bLoopAssert)
      {
        DMASSERT(!"dispatch depth exceeded");
      }
      return DM_LOOP_DETECTED;
    }
    if (u16Next > g_u16MaxDepthSeen)
    {
      g_u16MaxDepthSeen = u16Next;
    }
  }
  if (NULL != pu16Depth)
  {
    *pu16Depth = u16Next;
  }
  if (NULL != pu64TraceID)
  {
    *pu64TraceID = u64Trace;
  }
  return DM_SUCCESS;
}

uint16 dm__queue_max_depth(void)
{
  return g_u16MaxDepthSeen;
}

void dm__queue_set_loop_assert(in bool_t bAssert)
{
  g_bLoopAssert = bAssert;
  return;
}

/* ============================ 停读 ============================ */

static void dm__flow_apply(io dm__flow_st *pstFlow)
{
  if ((0u == pstFlow->u8Stop) && (!pstFlow->bReading))
  {
    if (DM_SUCCESS == dm_read_start(pstFlow->pstStream, pstFlow->pfAlloc, pstFlow->pfRead))
    {
      pstFlow->bReading = DM_TRUE;
    }
  }
  else if ((0u != pstFlow->u8Stop) && pstFlow->bReading)
  {
    (void)dm_read_stop(pstFlow->pstStream); /* 对已关的流也只是返回 0 */
    pstFlow->bReading = DM_FALSE;
  }
  return;
}

int dm__flow_start(io dm__flow_st *pstFlow, in dm_stream_st *pstStream, in dm_alloc_cb pfAlloc,
                   in dm_read_cb pfRead)
{
  if ((NULL == pstFlow) || (NULL == pstStream) || (NULL == pfAlloc) || (NULL == pfRead))
  {
    return DM_INVALID;
  }
  if (NULL == g_stFlows.stHead.pstNext)
  {
    dm_list_init(&g_stFlows);
  }
  pstFlow->pstStream = pstStream;
  pstFlow->pfAlloc = pfAlloc;
  pstFlow->pfRead = pfRead;
  pstFlow->u8Stop = g_bQueueFull ? DM__STOP_QUEUE : 0u;
  pstFlow->bReading = DM_FALSE;
  dm_list_add_tail(&g_stFlows, &pstFlow->stNode);
  dm__flow_apply(pstFlow);
  return ((0u != pstFlow->u8Stop) || pstFlow->bReading) ? DM_SUCCESS : DM_IO;
}

void dm__flow_remove(io dm__flow_st *pstFlow)
{
  if ((NULL != pstFlow) && (NULL != pstFlow->pstStream))
  {
    dm_list_del_node(&pstFlow->stNode);
    pstFlow->pstStream = NULL;
  }
  return;
}

void dm__flow_stop(io dm__flow_st *pstFlow, in uint8 u8Why)
{
  pstFlow->u8Stop |= u8Why;
  dm__flow_apply(pstFlow);
  return;
}

void dm__flow_resume(io dm__flow_st *pstFlow, in uint8 u8Why)
{
  pstFlow->u8Stop &= (uint8)~u8Why;
  dm__flow_apply(pstFlow);
  return;
}

/* 总队列满了 / 降下来了：对所有登记了的流加上 / 去掉「队列满」那一位 */
static void dm__queue_set_full(in bool_t bFull)
{
  dm_list_node_st *pstNode = NULL;
  dm_list_node_st *pstNext = NULL;

  if (bFull == g_bQueueFull)
  {
    return;
  }
  g_bQueueFull = bFull;
  if (bFull)
  {
    DM_LOG_WARN("queue over %llu bytes, stop reading", (unsigned long long)g_u64QueueMax);
  }
  if (NULL == g_stFlows.stHead.pstNext)
  {
    return;
  }
  DM_LIST_FOREACH_SAFE(&g_stFlows, pstNode, pstNext)
  {
    if (bFull)
    {
      dm__flow_stop(DM_LIST_ENTRY(pstNode, dm__flow_st, stNode), DM__STOP_QUEUE);
    }
    else
    {
      dm__flow_resume(DM_LIST_ENTRY(pstNode, dm__flow_st, stNode), DM__STOP_QUEUE);
    }
  }
  return;
}

/* ============================ 队列 ============================ */

dm__qitem_st *dm__qitem_new(void)
{
  dm__qitem_st *pstItem = (dm__qitem_st *)dm_malloc_tag(sizeof(dm__qitem_st), DM_MEM_TAG_QUEUE);
  if (NULL != pstItem)
  {
    memset(pstItem, 0, sizeof(*pstItem));
  }
  return pstItem;
}

void dm__qitem_free(io dm__qitem_st *pstItem)
{
  if (NULL == pstItem)
  {
    return;
  }
  dm__qbuf_put(pstItem->pstBuf);
  dm_free_tag(pstItem);
  return;
}

/* 交一条：DM 内部动作直接跑；否则拼好消息交给目标的 before。
 * 返回 DM_TRUE 表示外部字节计数所有权已移交给无状态直通任务（dm__queue_on_idle 勿重复扣减）。 */
static bool_t dm__queue_deliver(io dm__qitem_st *pstItem)
{
  dm_link_msg_st stMsg = {0};
  uint64 u64Start = 0;
  bool_t bPrevIn = g_bInDispatch;
  uint16 u16PrevDepth = g_u16CurDepth;
  uint64 u64PrevTrace = g_u64CurTraceID;

  if ((NULL != pstItem->pfStale) && pstItem->pfStale(pstItem))
  {
    return DM_FALSE; /* 比如：定时器到点以后、交出之前被取消了 */
  }
  if (NULL != pstItem->pfRun)
  {
    pstItem->pfRun(pstItem);
    return DM_FALSE;
  }
  memset(&stMsg, 0, sizeof(stMsg));
  stMsg.uiLinkID = pstItem->uiLinkID;
  stMsg.pu8Data = (NULL != pstItem->pstBuf) ? pstItem->pstBuf->au8Data : NULL;
  stMsg.u32Len = (NULL != pstItem->pstBuf) ? pstItem->pstBuf->u32Len : 0u;
  stMsg.pfWork = pstItem->pfWork;
  stMsg.pfAfter = pstItem->pfAfter;
  stMsg.u8Src = pstItem->u8Src;
  stMsg.u8Flags = pstItem->u8Flags;
  stMsg.u32Role = pstItem->u32Role;
  stMsg.u64Tag = pstItem->u64Tag;
  stMsg.u64TraceID = (0u != pstItem->u64TraceID) ? pstItem->u64TraceID : dm__trace_next();
  stMsg.u16Depth = pstItem->u16Depth;
  stMsg.iStatus = pstItem->iStatus;
  stMsg.pvPriv = (0u != pstItem->stReply.u32ConnID) ? (void *)&pstItem->stReply : pstItem->pvPriv;
  stMsg.eLane = pstItem->eLane;
  if ((NULL == pstItem->pfBefore) && (NULL != pstItem->pfWork))
  {
    bool_t bExtOwned = (pstItem->bExternal && (NULL != pstItem->pstBuf)) ? DM_TRUE : DM_FALSE;
    /* 对称三形态 (NULL, work, after)：零重复计数移交 g_u64QueueExt 所有权，零拷贝直投 dm_pool */
    (void)dm__stateless_submit(pstItem->uiLinkID, pstItem->eLane, pstItem->pfWork, pstItem->pfAfter,
                               pstItem->u8Src, pstItem->u8Flags, pstItem->u32Role, pstItem->u64Tag,
                               stMsg.u16Depth, stMsg.u64TraceID, pstItem->pstBuf, &stMsg,
                               bExtOwned);
    return bExtOwned;
  }

  g_bInDispatch = DM_TRUE;
  g_u16CurDepth = stMsg.u16Depth;
  g_u64CurTraceID = stMsg.u64TraceID;
  u64Start = dm_hrtime();
  (void)pstItem->pfBefore(&stMsg); /* before 的结果由它自己负责（记日志、应答） */
  {
    uint64 u64End = dm_hrtime();
    uint64 u64Dur = (u64End >= u64Start) ? (u64End - u64Start) : 0u;
    dm__stat_call(dm__stat_key(pstItem->uiLinkID, pstItem->u8Src, pstItem->u64Tag), u64Dur,
                  DM_FALSE);
  }
  g_bInDispatch = bPrevIn;
  g_u16CurDepth = u16PrevDepth;
  g_u64CurTraceID = u64PrevTrace;
  return DM_FALSE;
}

/* DD-01 §3.2 单圈时间片批处理：最多 DM_QUEUE_TICK_MAX(64) 条或 DM_QUEUE_TICK_BUDGET_MS(4ms) */
static void dm__queue_on_idle(in dm_idle_st *pstIdle)
{
  uint64 u64TickStart = dm_hrtime();
  uint64 u64BudgetNs = (uint64)DM_QUEUE_TICK_BUDGET_MS * 1000000ULL;
  uint32 u32Done = 0;
  (void)pstIdle;

  while (g_bQueueUp && (u32Done < (uint32)DM_QUEUE_TICK_MAX))
  {
    dm_list_node_st *pstNode = dm_list_del_head(&g_stQueue);
    dm__qitem_st *pstItem = NULL;
    uint32 u32ExtLen = 0;
    bool_t bExtTransferred = DM_FALSE;

    if (NULL == pstNode)
    {
      break;
    }
    pstItem = DM_LIST_ENTRY(pstNode, dm__qitem_st, stNode);
    g_uQueueLen--;
    atomic_store_explicit(&g_uQueueLenAtomic, g_uQueueLen, memory_order_release);
    if (pstItem->bExternal && (NULL != pstItem->pstBuf))
    {
      u32ExtLen = pstItem->pstBuf->u32Len;
    }
    bExtTransferred = dm__queue_deliver(pstItem);
    if ((!bExtTransferred) && (u32ExtLen > 0u))
    {
      dm__queue_ext_release(u32ExtLen);
    }
    dm__qitem_free(pstItem);
    u32Done++;
    if ((u32Done < (uint32)DM_QUEUE_TICK_MAX) && (g_uQueueLen > 0u))
    {
      uint64 u64Now = dm_hrtime();
      if ((u64Now >= u64TickStart) && ((u64Now - u64TickStart) >= u64BudgetNs))
      {
        break;
      }
    }
  }
  if (g_bQueueUp && (0u == g_uQueueLen) && g_bQueueIdleOn)
  {
    (void)dm_idle_stop(&g_stQueueIdle);
    g_bQueueIdleOn = DM_FALSE;
  }
  return;
}

void dm__queue_ext_add(in uint32 u32Bytes)
{
  if (0u == u32Bytes)
  {
    return;
  }
  g_u64QueueExt += u32Bytes;
  dm__queue_ext_sync();
  if (g_u64QueueExt > g_u64QueueMax)
  {
    dm__queue_set_full(DM_TRUE);
  }
  return;
}

void dm__queue_ext_release(in uint32 u32Bytes)
{
  if (0u == u32Bytes)
  {
    return;
  }
  g_u64QueueExt = (g_u64QueueExt >= u32Bytes) ? (g_u64QueueExt - u32Bytes) : 0u;
  dm__queue_ext_sync();
  if (g_bQueueFull && (g_u64QueueExt <= g_u64QueueMax / 2u))
  {
    dm__queue_set_full(DM_FALSE);
  }
  return;
}

/* ============================ 跨线程异步投递（AUD-11） ============================ */

static void dm__queue_on_async(in dm_async_st *pstAsync)
{
  dm_list_st stBatch = {0};
  dm_list_node_st *pstNode = NULL;
  (void)pstAsync;

  dm_once(&g_stAsyncOnce, dm__async_lock_init);
  if (!g_bAsyncLockReady)
  {
    return;
  }
  dm_list_init(&stBatch);

  dm_mutex_lock(&g_stAsyncLock);
  if (g_u64AsyncPendingBytes > 0u)
  {
    atomic_fetch_add_explicit(&g_u64AsyncDrainingAtomic, g_u64AsyncPendingBytes,
                              memory_order_acq_rel);
  }
  while (NULL != (pstNode = dm_list_del_head(&g_stAsyncList)))
  {
    dm_list_add_tail(&stBatch, pstNode);
  }
  g_u64AsyncPendingBytes = 0u;
  g_u32AsyncPendingCount = 0u;
  dm_mutex_unlock(&g_stAsyncLock);

  while (NULL != (pstNode = dm_list_del_head(&stBatch)))
  {
    dm__async_item_st *pstA = DM_LIST_ENTRY(pstNode, dm__async_item_st, stNode);
    uint32 u32Len = (NULL != pstA->pstBuf) ? pstA->pstBuf->u32Len : 0u;
    dm_link_handler_st *pstHandler = dm__link_lookup(pstA->uiLinkID);

    if ((NULL != pstHandler) && (dm__phase() < DM__PHASE_TEARDOWN))
    {
      dm__qitem_st *pstItem = dm__qitem_new();
      if (NULL != pstItem)
      {
        pstItem->uiLinkID = pstA->uiLinkID;
        pstItem->pfBefore = pstHandler->pfBefore;
        pstItem->pfWork = pstHandler->pfWork;
        pstItem->pfAfter = pstHandler->pfAfter;
        pstItem->eLane = (dm_pool_lane_et)pstHandler->u8Lane;
        pstItem->u8Src = DM_SRC_LOCAL;
        pstItem->bExternal = DM_TRUE;
        pstItem->u16Depth = 0u;
        pstItem->u64TraceID = dm__trace_next();
        pstItem->pstBuf = pstA->pstBuf;
        pstA->pstBuf = NULL;
        (void)dm__queue_push(pstItem);
      }
      else
      {
        DM_LOG_ERROR("queue async drain oom for link 0x%04x", pstA->uiLinkID);
        dm__qbuf_put(pstA->pstBuf);
      }
    }
    else
    {
      if (NULL == pstHandler)
      {
        DM_LOG_WARN("queue async target 0x%04x not found, dropped", pstA->uiLinkID);
      }
      dm__qbuf_put(pstA->pstBuf);
    }
    if (u32Len > 0u)
    {
      atomic_fetch_sub_explicit(&g_u64AsyncDrainingAtomic, u32Len, memory_order_release);
    }
    dm_free_tag(pstA);
  }
  return;
}

int dm_queue_post_async(in uint uiLinkID, in const void *pvData, in uint32 u32Len)
{
  dm__async_item_st *pstItem = NULL;
  dm__qbuf_st *pstBuf = NULL;
  uint64 u64Max = 0;
  uint64 u64Cur = 0;

  if ((DM_LINKID_INVALID == uiLinkID) || ((NULL == pvData) && (u32Len > 0u)))
  {
    return DM_INVALID;
  }
  dm_once(&g_stAsyncOnce, dm__async_lock_init);
  if (!g_bAsyncLockReady)
  {
    return DM_ALLOC;
  }
  if (!atomic_load_explicit(&g_bAsyncAccepting, memory_order_acquire))
  {
    return DM_SHUTTING_DOWN;
  }
  /* 快速无锁超限预检，避免为必然被拒的大包先分配内存 */
  u64Max = atomic_load_explicit(&g_u64QueueMaxAtomic, memory_order_acquire);
  if ((uint64)u32Len > u64Max)
  {
    return DM_BUSY;
  }

  pstItem = (dm__async_item_st *)dm_calloc_tag(1u, sizeof(dm__async_item_st), DM_MEM_TAG_QUEUE);
  if (NULL == pstItem)
  {
    return DM_ALLOC;
  }
  if (u32Len > 0u)
  {
    pstBuf = dm__qbuf_new(pvData, u32Len);
    if (NULL == pstBuf)
    {
      dm_free_tag(pstItem);
      return DM_ALLOC;
    }
  }
  pstItem->uiLinkID = uiLinkID;
  pstItem->pstBuf = pstBuf;

  dm_mutex_lock(&g_stAsyncLock);
  if (!atomic_load_explicit(&g_bAsyncAccepting, memory_order_relaxed))
  {
    dm_mutex_unlock(&g_stAsyncLock);
    dm__qbuf_put(pstBuf);
    dm_free_tag(pstItem);
    return DM_SHUTTING_DOWN;
  }
  u64Max = atomic_load_explicit(&g_u64QueueMaxAtomic, memory_order_acquire);
  {
    /* 严格先读 draining 后读 ext：drain 先加 ext 后减 draining，此顺序永不漏计 */
    uint64 u64Draining = atomic_load_explicit(&g_u64AsyncDrainingAtomic, memory_order_acquire);
    uint64 u64Ext = atomic_load_explicit(&g_u64QueueExtAtomic, memory_order_acquire);
    size_t uTotalCount = atomic_load_explicit(&g_uQueueLenAtomic, memory_order_acquire) +
                         (size_t)g_u32AsyncPendingCount;
    u64Cur = u64Draining + u64Ext + g_u64AsyncPendingBytes;
    if ((uTotalCount >= (size_t)DM__ASYNC_PENDING_MAX) || ((uint64)u32Len > u64Max) ||
        (u64Cur > u64Max) || ((uint64)u32Len > (u64Max - u64Cur)))
    {
      dm_mutex_unlock(&g_stAsyncLock);
      dm__qbuf_put(pstBuf);
      dm_free_tag(pstItem);
      return DM_BUSY;
    }
  }
  dm_list_add_tail(&g_stAsyncList, &pstItem->stNode);
  g_u64AsyncPendingBytes += u32Len;
  g_u32AsyncPendingCount++;
  /* 在持锁期内发 dm_async_send，与 dm__queue_close 严格互斥（杜绝句柄关闭竞态 UAF） */
  (void)dm_async_send(&g_stQueueAsync);
  dm_mutex_unlock(&g_stAsyncLock);
  return DM_SUCCESS;
}

void dm__queue_stop_async(void)
{
  dm_once(&g_stAsyncOnce, dm__async_lock_init);
  if (g_bAsyncLockReady)
  {
    dm_mutex_lock(&g_stAsyncLock);
    atomic_store_explicit(&g_bAsyncAccepting, DM_FALSE, memory_order_release);
    dm_mutex_unlock(&g_stAsyncLock);
  }
  return;
}

/* ============================ 生命周期 ============================ */

int dm__queue_init(void)
{
  int iErrCode = DM_SUCCESS;

  if (g_bQueueUp)
  {
    return DM_SUCCESS;
  }
  dm_once(&g_stAsyncOnce, dm__async_lock_init);
  if (!g_bAsyncLockReady)
  {
    return DM_ALLOC;
  }
  dm_list_init(&g_stQueue);
  if (NULL == g_stFlows.stHead.pstNext)
  {
    dm_list_init(&g_stFlows);
  }
  g_uQueueLen = 0;
  atomic_store_explicit(&g_uQueueLenAtomic, 0u, memory_order_release);
  g_u64QueueExt = 0;
  dm__queue_ext_sync();
  atomic_store_explicit(&g_u64AsyncDrainingAtomic, 0u, memory_order_release);
  g_bQueueFull = DM_FALSE;
  g_bInDispatch = DM_FALSE;
  g_u16CurDepth = 0;
  g_u64CurTraceID = 0;
  g_u16MaxDepthSeen = 0;

  iErrCode = dm_idle_init(dm_default_loop(), &g_stQueueIdle);
  if (DM_SUCCESS != iErrCode)
  {
    return iErrCode;
  }
  iErrCode = dm_async_init(dm_default_loop(), &g_stQueueAsync, dm__queue_on_async);
  if (DM_SUCCESS != iErrCode)
  {
    dm_close((dm_handle_st *)&g_stQueueIdle, NULL);
    return iErrCode;
  }
  /* 不靠 g_stQueueAsync 吊住空循环；有活跃句柄（服务/定时器等）时唤醒立即生效 */
  dm_unref((dm_handle_st *)&g_stQueueAsync);

  dm_mutex_lock(&g_stAsyncLock);
  g_u64AsyncPendingBytes = 0u;
  g_u32AsyncPendingCount = 0u;
  atomic_store_explicit(&g_bAsyncAccepting, DM_TRUE, memory_order_release);
  dm_mutex_unlock(&g_stAsyncLock);

  g_bQueueUp = DM_TRUE;
  g_bQueueIdleOn = DM_FALSE;
  return DM_SUCCESS;
}

void dm__queue_close(void)
{
  dm_list_node_st *pstNode = NULL;
  dm_list_st stLeft = {0};

  if (!g_bQueueUp)
  {
    return;
  }
  dm_list_init(&stLeft);
  if (g_bAsyncLockReady)
  {
    dm_mutex_lock(&g_stAsyncLock);
    atomic_store_explicit(&g_bAsyncAccepting, DM_FALSE, memory_order_release);
    while (NULL != (pstNode = dm_list_del_head(&g_stAsyncList)))
    {
      dm_list_add_tail(&stLeft, pstNode);
    }
    g_u64AsyncPendingBytes = 0u;
    g_u32AsyncPendingCount = 0u;
    dm_mutex_unlock(&g_stAsyncLock);
  }
  while (NULL != (pstNode = dm_list_del_head(&stLeft)))
  {
    dm__async_item_st *pstA = DM_LIST_ENTRY(pstNode, dm__async_item_st, stNode);
    dm__qbuf_put(pstA->pstBuf);
    dm_free_tag(pstA);
  }
  while (NULL != (pstNode = dm_list_del_head(&g_stQueue)))
  {
    dm__qitem_free(DM_LIST_ENTRY(pstNode, dm__qitem_st, stNode));
  }
  g_uQueueLen = 0;
  atomic_store_explicit(&g_uQueueLenAtomic, 0u, memory_order_release);
  g_u64QueueExt = 0;
  dm__queue_ext_sync();
  g_u64QueueMax = DM__QUEUE_MAX_DEFAULT;
  atomic_store_explicit(&g_u64QueueMaxAtomic, g_u64QueueMax, memory_order_release);
  atomic_store_explicit(&g_u64AsyncDrainingAtomic, 0u, memory_order_release);
  g_bQueueFull = DM_FALSE;
  g_bInDispatch = DM_FALSE;
  g_u16CurDepth = 0;
  g_u64CurTraceID = 0;
  dm_close((dm_handle_st *)&g_stQueueIdle, NULL);
  dm_close((dm_handle_st *)&g_stQueueAsync, NULL);
  g_bQueueUp = DM_FALSE;
  g_bQueueIdleOn = DM_FALSE;
  return;
}

void dm__queue_set_max(in uint32 u32Max)
{
  g_u64QueueMax = (0u != u32Max) ? (uint64)u32Max : DM__QUEUE_MAX_DEFAULT;
  atomic_store_explicit(&g_u64QueueMaxAtomic, g_u64QueueMax, memory_order_release);
  return;
}

int dm__queue_push(io dm__qitem_st *pstItem)
{
  if (NULL == pstItem)
  {
    return DM_INVALID;
  }
  if ((!g_bQueueUp) && (DM__PHASE_NONE == dm__phase()))
  {
    (void)dm__queue_init(); /* 不经 dm_main_ex 的白盒用法：第一次放的时候建 */
  }
  if ((!g_bQueueUp) || (dm__phase() >= DM__PHASE_TEARDOWN))
  {
    dm__qitem_free(pstItem);
    return DM_SHUTTING_DOWN;
  }
  dm_list_add_tail(&g_stQueue, &pstItem->stNode);
  g_uQueueLen++;
  atomic_store_explicit(&g_uQueueLenAtomic, g_uQueueLen, memory_order_release);
  if (pstItem->bExternal && (NULL != pstItem->pstBuf))
  {
    g_u64QueueExt += pstItem->pstBuf->u32Len;
    dm__queue_ext_sync();
    if (g_u64QueueExt > g_u64QueueMax)
    {
      dm__queue_set_full(DM_TRUE);
    }
  }
  if (!g_bQueueIdleOn)
  {
    if (DM_SUCCESS == dm_idle_start(&g_stQueueIdle, dm__queue_on_idle))
    {
      g_bQueueIdleOn = DM_TRUE;
    }
  }
  return DM_SUCCESS;
}

int dm__queue_run(in dm__qrun_cb pfRun, in void *pvCtx)
{
  dm__qitem_st *pstItem = dm__qitem_new();
  if (NULL == pstItem)
  {
    return DM_ALLOC;
  }
  pstItem->pfRun = pfRun;
  pstItem->pvCtx = pvCtx;
  return dm__queue_push(pstItem);
}

size_t dm__queue_len(void)
{
  return g_uQueueLen;
}

bool_t dm__queue_idle_on(void)
{
  return g_bQueueIdleOn;
}

uint64 dm__queue_ext_bytes(void)
{
  return g_u64QueueExt;
}

uint64 dm__queue_room(void)
{
  return (g_u64QueueExt < g_u64QueueMax) ? (g_u64QueueMax - g_u64QueueExt) : 0u;
}

#ifdef __cplusplus
}
#endif
