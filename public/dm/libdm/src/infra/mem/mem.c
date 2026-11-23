/**
 ******************************************************************************
 * @file    mem.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   DM 内存调试与标签统计分配器实现（见 dm/mem.h）：
 *          1) 轻量标签统计（常驻、无锁）：dm_mem_tag_* —— 固定 ID + 原子计数；
 *          2) 全量内存定位（仅 DM_MEM_DEBUG）：dm_mem_debug_* —— 调用栈 + 哨兵 + 毒药；
 *          3) 防编译器死存储消除（DSE）的安全擦零：dm_secure_zero()。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include <dm/mem.h>

#include <dm.h>
#include "../../uv_adapter.h"

#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h> /* CaptureStackBackTrace */
#else
#include <execinfo.h> /* backtrace / backtrace_symbols */
#endif

/* ============================ 第 1 档：轻量标签统计（常驻、无锁） ============================
 * 每块数据前带一个块头 {标签 ID, 大小}（64 位下共 16 字节）；
 * 按标签 ID 用定长数组 + 原子计数（fetch_add/sub），无锁、精确。 */
#define DM_MEM_TAG_MAXID DM_MEM_TAG_COUNT /* 定长槽数(与 dm.h 对齐) */

typedef struct
{
  int iTag;     /* 业务标签 ID(dm.h 的 DM_MEM_TAG_*) */
  size_t uSize; /* 请求大小 */
} dm_mem_tag_hdr_st;

_Static_assert((sizeof(dm_mem_tag_hdr_st) % _Alignof(max_align_t)) == 0u,
               "dm_mem_tag_hdr_st must stay aligned to max_align_t");

static _Atomic uint64 g_au64TagBytes[DM_MEM_TAG_MAXID];
static _Atomic uint64 g_au64TagBlocks[DM_MEM_TAG_MAXID];
static _Atomic uint64 g_u64LiveBlocks;

/* tag ID -> 名称。DM 自己的写在这里；别的模块 init 时用 dm_mem_tag_name 登记。没名字的显示编号 */
static const char *g_apcTagName[DM_MEM_TAG_MAXID] = {
    [DM_MEM_TAG_UV_LOOP] = "uv_loop",
    [DM_MEM_TAG_UV_TIMER] = "uv_timer",
    [DM_MEM_TAG_UV_ASYNC] = "uv_async",
    [DM_MEM_TAG_UV_TCP] = "uv_tcp",
    [DM_MEM_TAG_UV_PIPE] = "uv_pipe",
    [DM_MEM_TAG_UV_FS] = "uv_fs",
    [DM_MEM_TAG_UV_WORK] = "uv_work",
    [DM_MEM_TAG_UV_DNS] = "uv_dns",
    [DM_MEM_TAG_UV_SIGNAL] = "uv_signal",
    [DM_MEM_TAG_UV_OTHER] = "uv_other",
    [DM_MEM_TAG_LINK_CALLBACK] = "link_callback",
    [DM_MEM_TAG_LINK_CONN] = "link_conn",
    [DM_MEM_TAG_LINK_SESSION] = "link_session",
    [DM_MEM_TAG_LINK_FRAME] = "link_frame",
    [DM_MEM_TAG_LINK_TX] = "link_tx",
    [DM_MEM_TAG_LINK_RX] = "link_rx",
    [DM_MEM_TAG_TIMER] = "timer",
    [DM_MEM_TAG_CHILD] = "child",
    [DM_MEM_TAG_HTTP] = "http",
    [DM_MEM_TAG_WS] = "ws",
    [DM_MEM_TAG_RPC] = "rpc",
    [DM_MEM_TAG_PUSH] = "push",
    [DM_MEM_TAG_DB] = "db",
    [DM_MEM_TAG_JSON] = "json",
    [DM_MEM_TAG_LOG] = "log",
    [DM_MEM_TAG_QUEUE] = "queue",
    [DM_MEM_TAG_EVENT] = "event",
    [DM_MEM_TAG_HASH] = "hash",
    [DM_MEM_TAG_OS] = "os",
    [DM_MEM_TAG_POOL] = "pool",
    [DM_MEM_TAG_SECRET] = "secret",
    [DM_MEM_TAG_HTTPC] = "httpc",
};

/* 越界 tag 归入兜底,避免数组越界 */
static int dm__mem_tag_clamp(int iTag)
{
  if (iTag < 0 || iTag >= DM_MEM_TAG_MAXID)
  {
    return DM_MEM_TAG_UV_OTHER;
  }
  return iTag;
}

void dm_mem_tag_name(in int iTag, in const char *pcName)
{
  if ((iTag >= 0) && (iTag < DM_MEM_TAG_MAXID))
  {
    g_apcTagName[iTag] = pcName;
  }
  return;
}

void dm__mem_tag_each(void (*pfEach)(int iTag, const char *pcName, uint64 u64Blocks,
                                     uint64 u64Bytes, void *pvArg),
                      void *pvArg)
{
  int iTag = 0;
  for (iTag = 0; iTag < DM_MEM_TAG_MAXID; iTag++)
  {
    uint64 u64Blocks = atomic_load(&g_au64TagBlocks[iTag]);
    if (0u != u64Blocks)
    {
      pfEach(iTag, g_apcTagName[iTag], u64Blocks, atomic_load(&g_au64TagBytes[iTag]), pvArg);
    }
  }
  return;
}

void dm_mem_tag_init(void)
{
  /* 无锁版：数组零初始化即用，无需任何初始化 */
  return;
}

void *dm_mem_tag_alloc(size_t uSize, int iTag)
{
  dm_mem_tag_hdr_st *pstHdr = NULL;
  uint8_t *pu8Raw = NULL;

  iTag = dm__mem_tag_clamp(iTag);
  if (uSize > (((size_t)-1) - sizeof(dm_mem_tag_hdr_st)))
  {
    return NULL;
  }
  pu8Raw = dm_malloc(sizeof(dm_mem_tag_hdr_st) + uSize);
  if (NULL == pu8Raw)
  {
    return NULL;
  }
  pstHdr = (dm_mem_tag_hdr_st *)pu8Raw;
  pstHdr->iTag = iTag;
  pstHdr->uSize = uSize;
  atomic_fetch_add(&g_au64TagBytes[iTag], (uint64_t)uSize);
  atomic_fetch_add(&g_au64TagBlocks[iTag], 1);
  atomic_fetch_add(&g_u64LiveBlocks, 1);
  return pu8Raw + sizeof(dm_mem_tag_hdr_st);
}

void *dm_mem_tag_calloc(size_t uCount, size_t uSize, int iTag)
{
  void *pvPtr = NULL;

  if ((0u != uSize) && (uCount > ((size_t)-1) / uSize))
  {
    return NULL; /* uCount * uSize 溢出 */
  }
  pvPtr = dm_mem_tag_alloc(uCount * uSize, iTag);

  if (NULL != pvPtr)
  {
    memset(pvPtr, 0, uCount * uSize);
  }
  return pvPtr;
}

void *dm_mem_tag_realloc(void *pvPtr, size_t uSize)
{
  dm_mem_tag_hdr_st *pstHdr = NULL;
  dm_mem_tag_hdr_st *pstNewHdr = NULL;
  uint8_t *pu8NewRaw = NULL;
  int iTag = DM_MEM_TAG_UV_OTHER;
  size_t uOldSize = 0u;

  if (NULL == pvPtr)
  {
    return dm_mem_tag_alloc(uSize, DM_MEM_TAG_UV_OTHER);
  }
  if (0u == uSize)
  {
    dm_mem_tag_free(pvPtr);
    return NULL;
  }
  pstHdr = (dm_mem_tag_hdr_st *)pvPtr - 1;
  iTag = dm__mem_tag_clamp(pstHdr->iTag);
  uOldSize = pstHdr->uSize;

  if (uSize > (((size_t)-1) - sizeof(dm_mem_tag_hdr_st)))
  {
    return NULL;
  }
  /* 先 realloc 再改计数:失败时原块仍有效、计数不动,之后 free 才扣得对 */
  pu8NewRaw = dm_realloc(pstHdr, sizeof(dm_mem_tag_hdr_st) + uSize);
  if (NULL == pu8NewRaw)
  {
    return NULL;
  }
  pstNewHdr = (dm_mem_tag_hdr_st *)pu8NewRaw;
  pstNewHdr->uSize = uSize;
  atomic_fetch_sub(&g_au64TagBytes[iTag], (uint64_t)uOldSize);
  atomic_fetch_add(&g_au64TagBytes[iTag], (uint64_t)uSize);
  return pu8NewRaw + sizeof(dm_mem_tag_hdr_st);
}

void dm_mem_tag_free(void *pvPtr)
{
  dm_mem_tag_hdr_st *pstHdr = NULL;
  int iTag = DM_MEM_TAG_UV_OTHER;

  if (NULL == pvPtr)
  {
    return;
  }
  pstHdr = (dm_mem_tag_hdr_st *)pvPtr - 1;
  iTag = dm__mem_tag_clamp(pstHdr->iTag);
  atomic_fetch_sub(&g_au64TagBytes[iTag], (uint64_t)pstHdr->uSize);
  atomic_fetch_sub(&g_au64TagBlocks[iTag], 1);
  atomic_fetch_sub(&g_u64LiveBlocks, 1);
  dm_free(pstHdr);
  return;
}

size_t dm_mem_tag_count(void)
{
  return (size_t)atomic_load(&g_u64LiveBlocks);
}

void dm_mem_tag_stat(in int iTag, out uint64 *pu64Blocks, out uint64 *pu64Bytes)
{
  uint64 u64Blocks = 0u;
  uint64 u64Bytes = 0u;

  if ((iTag >= 0) && (iTag < DM_MEM_TAG_MAXID))
  {
    u64Blocks = atomic_load(&g_au64TagBlocks[iTag]);
    u64Bytes = atomic_load(&g_au64TagBytes[iTag]);
  }
  if (NULL != pu64Blocks)
  {
    *pu64Blocks = u64Blocks;
  }
  if (NULL != pu64Bytes)
  {
    *pu64Bytes = u64Bytes;
  }
  return;
}

void dm_mem_tag_dump(void)
{
  int i = 0;
  uint64_t uLive = atomic_load(&g_u64LiveBlocks);
  uint64_t uTotal = 0;

  if (0 == uLive)
  {
    fprintf(stderr, "[mem-tag] no leaks\n");
    return;
  }
  fprintf(stderr, "[mem-tag] ==== %llu live blocks ====\n", (unsigned long long)uLive);
  fprintf(stderr, "[mem-tag] -- by tag --\n");
  for (i = 0; i < DM_MEM_TAG_MAXID; i++)
  {
    uint64_t uBlocks = atomic_load(&g_au64TagBlocks[i]);
    uint64_t uBytes = atomic_load(&g_au64TagBytes[i]);

    if (uBlocks > 0)
    {
      if (NULL != g_apcTagName[i])
      {
        fprintf(stderr, "  [%s] %llu blocks, %llu bytes\n", g_apcTagName[i],
                (unsigned long long)uBlocks, (unsigned long long)uBytes);
      }
      else
      {
        fprintf(stderr, "  [id %d] %llu blocks, %llu bytes\n", i, (unsigned long long)uBlocks,
                (unsigned long long)uBytes);
      }
      uTotal += uBytes;
    }
  }
  fprintf(stderr, "[mem-tag] total live %llu bytes\n", (unsigned long long)uTotal);
  return;
}

/* ============================ 第 2 档：全量内存定位（仅 DM_MEM_DEBUG）
 * ============================ backtrace 调用栈 + 越界哨兵 + 毒药填充 + 记录表，精确到文件:行。 */
#ifdef DM_MEM_DEBUG

#define DM_MEM_GUARD 16u /* 数据前后各 16 字节红区（对齐 max_align_t，防 UBSan 未对齐） */
_Static_assert((DM_MEM_GUARD % _Alignof(max_align_t)) == 0u,
               "DM_MEM_GUARD must be aligned to max_align_t");

static const uint8 g_au8MemBefore[DM_MEM_GUARD] = {
    0xFD, 0xFD, 0xFD, 0xFD, 0xFD, 0xFD, 0xFD, 0xFD, 0xFD, 0xFD, 0xFD, 0xFD, 0xFD, 0xFD, 0xFD, 0xFD,
};
static const uint8 g_au8MemAfter[DM_MEM_GUARD] = {
    0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE,
};
#define DM_MEM_POISON 0xDDu /* free 后填充值,抓到 use-after-free */

#define DM_MEM_REC_INIT_CAP 1024u /* 动态开放寻址哈希表初始容量（2 的幂，无固定 8192 上限） */
#define DM_MEM_BT_DEPTH 10        /* 每个分配记录 10 帧调用栈 */

#define DM_MEM_SLOT_EMPTY 0u
#define DM_MEM_SLOT_LIVE 1u
#define DM_MEM_SLOT_TOMB 2u

typedef struct
{
  uint8 u8State;
  void *pvPtr;
  size_t uSize;
  int iTag;
  const char *pcFile;
  int iLine;
  void *apvBt[DM_MEM_BT_DEPTH];
  int iBtDepth;
} dm_mem_rec_st;

static dm_mem_rec_st *g_pstRecTab = NULL;
static size_t g_uRecCap = 0;
static size_t g_uRecCount = 0; /* LIVE 槽数 */
static size_t g_uRecUsed = 0;  /* LIVE + TOMB 槽数 */
static dm_mutex_t g_stMutex;
static dm_once_t g_stInitOnce = DM_ONCE_INIT;
static dm_mem_free_hook_fn g_pfFreeHook = NULL;
static _Atomic uint64 g_u64GuardHits = 0;

static void dm__mem_lock(void)
{
  dm_mutex_lock(&g_stMutex);
  return;
}

static void dm__mem_unlock(void)
{
  dm_mutex_unlock(&g_stMutex);
  return;
}

static void dm__mem_init_once(void)
{
  dm_mutex_init(&g_stMutex);
  return;
}

/* 多线程首次分配可能并发进来:用 dm_once 保证锁只初始化一次 */
static void dm__mem_ensure_init(void)
{
  dm_once(&g_stInitOnce, dm__mem_init_once);
  return;
}

static size_t dm__mem_hash_ptr(const void *pvPtr)
{
  uintptr_t uKey = (uintptr_t)pvPtr;
  uKey ^= uKey >> 16;
  uKey *= (uintptr_t)0x45d9f3bu;
  uKey ^= uKey >> 16;
  return (size_t)uKey;
}

static int dm__mem_bt_capture(void **apvOut, int iMax)
{
#if defined(_WIN32)
  return CaptureStackBackTrace(2, iMax, apvOut, NULL);
#else
  return backtrace(apvOut, iMax);
#endif
}

static void dm__mem_bt_print(void *const *apvBt, int iDepth)
{
  int i = 0;

  if (iDepth <= 0)
  {
    return;
  }
#if defined(_WIN32)
  for (i = 0; i < iDepth; i++)
  {
    fprintf(stderr, "      [bt] %p\n", apvBt[i]);
  }
#else
  char **ppcSym = backtrace_symbols(apvBt, iDepth);

  if (NULL == ppcSym)
  {
    return;
  }
  for (i = 0; i < iDepth; i++)
  {
    fprintf(stderr, "      [bt] %s\n", ppcSym[i]);
  }
  free(ppcSym);
#endif
  return;
}

static int dm__mem_tab_rehash(size_t uNewCap)
{
  dm_mem_rec_st *pstNew = NULL;
  size_t uMask = uNewCap - 1u;
  size_t i = 0;

  /* 调试元数据表走系统 calloc/free，避免与业务 dm_replace_allocator OOM 注入互相干扰 */
  pstNew = (dm_mem_rec_st *)calloc(uNewCap, sizeof(dm_mem_rec_st));
  if (NULL == pstNew)
  {
    return -1;
  }
  for (i = 0; i < g_uRecCap; i++)
  {
    if (DM_MEM_SLOT_LIVE == g_pstRecTab[i].u8State)
    {
      size_t uSlot = dm__mem_hash_ptr(g_pstRecTab[i].pvPtr) & uMask;
      while (DM_MEM_SLOT_EMPTY != pstNew[uSlot].u8State)
      {
        uSlot = (uSlot + 1u) & uMask;
      }
      pstNew[uSlot] = g_pstRecTab[i];
    }
  }
  free(g_pstRecTab);
  g_pstRecTab = pstNew;
  g_uRecCap = uNewCap;
  g_uRecUsed = g_uRecCount;
  return 0;
}

static int dm__mem_tab_ensure(void)
{
  if (0u == g_uRecCap)
  {
    return dm__mem_tab_rehash(DM_MEM_REC_INIT_CAP);
  }
  /* 装载因子 > 60% 时扩容；墓碑太多但 live 少时原地重建清墓碑 */
  if ((g_uRecUsed + 1u) * 10u >= g_uRecCap * 6u)
  {
    size_t uNewCap = ((g_uRecCount + 1u) * 10u >= g_uRecCap * 4u) ? (g_uRecCap * 2u) : g_uRecCap;
    if (uNewCap < g_uRecCap)
    {
      return -1;
    }
    return dm__mem_tab_rehash(uNewCap);
  }
  return 0;
}

static dm_mem_rec_st *dm__mem_rec_find(void *pvPtr)
{
  size_t uMask = 0;
  size_t uSlot = 0;
  size_t uStep = 0;

  if (0u == g_uRecCap)
  {
    return NULL;
  }
  uMask = g_uRecCap - 1u;
  uSlot = dm__mem_hash_ptr(pvPtr) & uMask;
  for (uStep = 0; uStep < g_uRecCap; uStep++)
  {
    uint8 u8St = g_pstRecTab[uSlot].u8State;
    if (DM_MEM_SLOT_EMPTY == u8St)
    {
      return NULL;
    }
    if ((DM_MEM_SLOT_LIVE == u8St) && (g_pstRecTab[uSlot].pvPtr == pvPtr))
    {
      return &g_pstRecTab[uSlot];
    }
    uSlot = (uSlot + 1u) & uMask;
  }
  return NULL;
}

static int dm__mem_rec_insert(const dm_mem_rec_st *pstRec)
{
  size_t uMask = 0;
  size_t uSlot = 0;
  size_t uFirstTomb = (size_t)-1;
  size_t uStep = 0;

  if (0 != dm__mem_tab_ensure())
  {
    return -1;
  }
  uMask = g_uRecCap - 1u;
  uSlot = dm__mem_hash_ptr(pstRec->pvPtr) & uMask;
  for (uStep = 0; uStep < g_uRecCap; uStep++)
  {
    uint8 u8St = g_pstRecTab[uSlot].u8State;
    if (DM_MEM_SLOT_EMPTY == u8St)
    {
      size_t uDst = ((size_t)-1 != uFirstTomb) ? uFirstTomb : uSlot;
      if (DM_MEM_SLOT_EMPTY == g_pstRecTab[uDst].u8State)
      {
        g_uRecUsed++;
      }
      g_pstRecTab[uDst] = *pstRec;
      g_pstRecTab[uDst].u8State = DM_MEM_SLOT_LIVE;
      g_uRecCount++;
      return 0;
    }
    if ((DM_MEM_SLOT_TOMB == u8St) && ((size_t)-1 == uFirstTomb))
    {
      uFirstTomb = uSlot;
    }
    uSlot = (uSlot + 1u) & uMask;
  }
  return -1;
}

static void dm__mem_rec_erase(dm_mem_rec_st *pstRec)
{
  pstRec->u8State = DM_MEM_SLOT_TOMB;
  pstRec->pvPtr = NULL;
  g_uRecCount--;
  return;
}

static int dm__mem_sentinel_check(const uint8 *pu8Data, size_t uSize)
{
  const uint8 *pu8Raw = pu8Data - DM_MEM_GUARD;

  if (memcmp(pu8Raw, g_au8MemBefore, DM_MEM_GUARD) != 0)
  {
    atomic_fetch_add(&g_u64GuardHits, 1u);
    return 1;
  }
  if (memcmp(pu8Raw + DM_MEM_GUARD + uSize, g_au8MemAfter, DM_MEM_GUARD) != 0)
  {
    atomic_fetch_add(&g_u64GuardHits, 1u);
    return 2;
  }
  return 0;
}

static void dm__mem_sentinel_place(uint8 *pu8Data, size_t uSize)
{
  memcpy(pu8Data - DM_MEM_GUARD, g_au8MemBefore, DM_MEM_GUARD);
  memcpy(pu8Data + uSize, g_au8MemAfter, DM_MEM_GUARD);
  return;
}

void dm_mem_debug_init(void)
{
  dm__mem_ensure_init();
  return;
}

void dm_mem_debug_set_free_hook(in dm_mem_free_hook_fn pfHook)
{
  dm__mem_ensure_init();
  dm__mem_lock();
  g_pfFreeHook = pfHook;
  dm__mem_unlock();
  return;
}

uint64 dm_mem_debug_guard_hits(void)
{
  return atomic_load(&g_u64GuardHits);
}

void dm_mem_debug_reset_guard_hits(void)
{
  atomic_store(&g_u64GuardHits, 0u);
  return;
}

void *dm_mem_debug_alloc_tag(size_t uSize, int iTag, in const char *pcFile, in int iLine)
{
  uint8 *pu8Raw = NULL;
  uint8 *pu8Data = NULL;
  dm_mem_rec_st stRec = {0};
  int iClamped = dm__mem_tag_clamp(iTag);

  dm__mem_ensure_init();
  if (uSize > (((size_t)-1) - (2u * DM_MEM_GUARD)))
  {
    return NULL;
  }
  pu8Raw = (uint8 *)dm_malloc(uSize + (2u * DM_MEM_GUARD));
  if (NULL == pu8Raw)
  {
    return NULL;
  }
  pu8Data = pu8Raw + DM_MEM_GUARD;
  dm__mem_sentinel_place(pu8Data, uSize);

  memset(&stRec, 0, sizeof(stRec));
  stRec.pvPtr = pu8Data;
  stRec.uSize = uSize;
  stRec.iTag = iClamped;
  stRec.pcFile = pcFile;
  stRec.iLine = iLine;
  stRec.iBtDepth = dm__mem_bt_capture(stRec.apvBt, DM_MEM_BT_DEPTH);

  dm__mem_lock();
  if (0 != dm__mem_rec_insert(&stRec))
  {
    dm__mem_unlock();
    dm_free(pu8Raw);
    return NULL;
  }
  dm__mem_unlock();

  atomic_fetch_add(&g_au64TagBytes[iClamped], (uint64_t)uSize);
  atomic_fetch_add(&g_au64TagBlocks[iClamped], 1u);
  atomic_fetch_add(&g_u64LiveBlocks, 1u);
  return pu8Data;
}

void *dm_mem_debug_alloc(size_t uSize, in const char *pcFile, in int iLine)
{
  return dm_mem_debug_alloc_tag(uSize, DM_MEM_TAG_UV_OTHER, pcFile, iLine);
}

void *dm_mem_debug_calloc_tag(size_t uCount, size_t uSize, int iTag, in const char *pcFile,
                              in int iLine)
{
  void *pvPtr = NULL;

  if ((0u != uSize) && (uCount > ((size_t)-1) / uSize))
  {
    return NULL; /* uCount * uSize 溢出 */
  }
  pvPtr = dm_mem_debug_alloc_tag(uCount * uSize, iTag, pcFile, iLine);
  if (NULL != pvPtr)
  {
    memset(pvPtr, 0, uCount * uSize);
  }
  return pvPtr;
}

void *dm_mem_debug_calloc(size_t uCount, size_t uSize, in const char *pcFile, in int iLine)
{
  return dm_mem_debug_calloc_tag(uCount, uSize, DM_MEM_TAG_UV_OTHER, pcFile, iLine);
}

void *dm_mem_debug_realloc(void *pvPtr, size_t uSize, in const char *pcFile, in int iLine)
{
  uint8 *pu8NewRaw = NULL;
  uint8 *pu8NewData = NULL;
  dm_mem_rec_st *pstRec = NULL;
  dm_mem_rec_st stUpdated = {0};
  size_t uOldSize = 0;
  int iTag = DM_MEM_TAG_UV_OTHER;
  int iBadGuard = 0;

  if (NULL == pvPtr)
  {
    return dm_mem_debug_alloc(uSize, pcFile, iLine);
  }
  if (0u == uSize)
  {
    dm_mem_debug_free(pvPtr);
    return NULL;
  }
  if (uSize > (((size_t)-1) - (2u * DM_MEM_GUARD)))
  {
    return NULL;
  }
  dm__mem_ensure_init();

  memset(&stUpdated, 0, sizeof(stUpdated));
  stUpdated.pcFile = pcFile;
  stUpdated.iLine = iLine;
  stUpdated.iBtDepth = dm__mem_bt_capture(stUpdated.apvBt, DM_MEM_BT_DEPTH);

  /* 全程持锁覆盖查表、容量预留、dm_realloc 与表更新，消除并发 ABA 复用与 rehash OOM
   * 丢记录（W1-AUD-01） */
  dm__mem_lock();
  pstRec = dm__mem_rec_find(pvPtr);
  if (NULL == pstRec)
  {
    dm__mem_unlock();
    fprintf(stderr, "[mem-dbg] realloc 未分配/已释放的指针 @ %p\n", pvPtr);
    return NULL;
  }
  uOldSize = pstRec->uSize;
  iTag = pstRec->iTag;
  if (dm__mem_sentinel_check((const uint8 *)pvPtr, uOldSize) != 0)
  {
    iBadGuard = 1;
  }
  /* 若搬迁地址需要新槽位，先预扩容元数据表：失败则直接返回 NULL，原块与原记录毫发无损 */
  if (0 != dm__mem_tab_ensure())
  {
    dm__mem_unlock();
    if (0 != iBadGuard)
    {
      fprintf(stderr, "[mem-dbg] realloc 发现越界写 @ %s:%d\n", pcFile, iLine);
    }
    return NULL;
  }
  /* dm__mem_tab_ensure 可能触发了 rehash，重新定位 pstRec */
  pstRec = dm__mem_rec_find(pvPtr);

  pu8NewRaw = (uint8 *)dm_realloc((uint8 *)pvPtr - DM_MEM_GUARD, uSize + (2u * DM_MEM_GUARD));
  if (NULL == pu8NewRaw)
  {
    dm__mem_unlock();
    if (0 != iBadGuard)
    {
      fprintf(stderr, "[mem-dbg] realloc 发现越界写 @ %s:%d\n", pcFile, iLine);
    }
    return NULL;
  }
  pu8NewData = pu8NewRaw + DM_MEM_GUARD;
  dm__mem_sentinel_place(pu8NewData, uSize);

  stUpdated.pvPtr = pu8NewData;
  stUpdated.uSize = uSize;
  stUpdated.iTag = iTag;
  if (pu8NewData == pvPtr)
  {
    *pstRec = stUpdated;
    pstRec->u8State = DM_MEM_SLOT_LIVE;
  }
  else
  {
    dm__mem_rec_erase(pstRec);
    (void)dm__mem_rec_insert(&stUpdated); /* 前已预扩容，此处必有空槽 */
  }
  dm__mem_unlock();

  if (0 != iBadGuard)
  {
    fprintf(stderr, "[mem-dbg] realloc 发现越界写 @ %s:%d\n", pcFile, iLine);
  }
  atomic_fetch_sub(&g_au64TagBytes[iTag], (uint64_t)uOldSize);
  atomic_fetch_add(&g_au64TagBytes[iTag], (uint64_t)uSize);
  return pu8NewData;
}

void dm_mem_debug_free(void *pvPtr)
{
  uint8 *pu8Raw = NULL;
  const char *pcFile = NULL;
  dm_mem_rec_st *pstRec = NULL;
  dm_mem_free_hook_fn pfHook = NULL;
  size_t uSize = 0;
  int iTag = DM_MEM_TAG_UV_OTHER;
  int iLine = 0;

  if (NULL == pvPtr)
  {
    return;
  }
  dm__mem_ensure_init();

  dm__mem_lock();
  pstRec = dm__mem_rec_find(pvPtr);
  if (NULL == pstRec)
  {
    dm__mem_unlock();
    fprintf(stderr, "[mem-dbg] free 未分配/已释放/被踩的指针 @ %p\n", pvPtr);
    return;
  }
  uSize = pstRec->uSize;
  iTag = pstRec->iTag;
  pcFile = pstRec->pcFile;
  iLine = pstRec->iLine;
  pfHook = g_pfFreeHook;
  dm__mem_rec_erase(pstRec);
  dm__mem_unlock();

  if (dm__mem_sentinel_check((const uint8 *)pvPtr, uSize) != 0)
  {
    fprintf(stderr, "[mem-dbg] 越界写! %s:%d (size=%u)\n", pcFile, iLine, (unsigned)uSize);
  }
  if (NULL != pfHook)
  {
    pfHook(pvPtr, uSize);
  }

  atomic_fetch_sub(&g_au64TagBytes[iTag], (uint64_t)uSize);
  atomic_fetch_sub(&g_au64TagBlocks[iTag], 1u);
  atomic_fetch_sub(&g_u64LiveBlocks, 1u);

  pu8Raw = (uint8 *)pvPtr - DM_MEM_GUARD;
  memset(pu8Raw, DM_MEM_POISON, uSize + (2u * DM_MEM_GUARD));
  dm_free(pu8Raw);
  return;
}

size_t dm_mem_debug_count(void)
{
  size_t n = 0;

  dm__mem_ensure_init();
  dm__mem_lock();
  n = g_uRecCount;
  dm__mem_unlock();
  return n;
}

void dm_mem_debug_dump(void)
{
  typedef struct
  {
    int iTag;
    size_t uCount;
    size_t uBytes;
  } dm_tag_stat_st;

  dm_tag_stat_st aTag[64] = {{0}};
  size_t uTagCnt = 0;
  size_t i = 0;
  size_t k = 0;
  size_t uTotal = 0;

  dm__mem_ensure_init();
  dm__mem_lock();
  if (0u == g_uRecCount)
  {
    fprintf(stderr, "[mem-dbg] no leaks\n");
    dm__mem_unlock();
    return;
  }
  fprintf(stderr, "[mem-dbg] ==== %u blocks leaked ====\n", (unsigned)g_uRecCount);

  for (i = 0; i < g_uRecCap; i++)
  {
    int iTag = 0;
    if (DM_MEM_SLOT_LIVE != g_pstRecTab[i].u8State)
    {
      continue;
    }
    iTag = g_pstRecTab[i].iTag;
    for (k = 0; k < uTagCnt; k++)
    {
      if (aTag[k].iTag == iTag)
      {
        break;
      }
    }
    if (k == uTagCnt)
    {
      if (uTagCnt < 64u)
      {
        aTag[uTagCnt].iTag = iTag;
        aTag[uTagCnt].uCount = 0;
        aTag[uTagCnt].uBytes = 0;
        uTagCnt++;
      }
      k = (uTagCnt > 0u) ? (uTagCnt - 1u) : 0u;
    }
    aTag[k].uCount++;
    aTag[k].uBytes += g_pstRecTab[i].uSize;
  }

  fprintf(stderr, "[mem-dbg] -- by tag --\n");
  for (k = 0; k < uTagCnt; k++)
  {
    fprintf(
        stderr, "  [%s] %u blocks, %u bytes\n",
        (aTag[k].iTag >= 0 && aTag[k].iTag < DM_MEM_TAG_MAXID && g_apcTagName[aTag[k].iTag] != NULL)
            ? g_apcTagName[aTag[k].iTag]
            : "?",
        (unsigned)aTag[k].uCount, (unsigned)aTag[k].uBytes);
    uTotal += aTag[k].uBytes;
  }

  fprintf(stderr, "[mem-dbg] -- details --\n");
  for (i = 0; i < g_uRecCap; i++)
  {
    if (DM_MEM_SLOT_LIVE != g_pstRecTab[i].u8State)
    {
      continue;
    }
    fprintf(stderr, "  LEAK %u bytes  tag=%s  from %s:%d\n", (unsigned)g_pstRecTab[i].uSize,
            (g_pstRecTab[i].iTag >= 0 && g_pstRecTab[i].iTag < DM_MEM_TAG_MAXID &&
             g_apcTagName[g_pstRecTab[i].iTag] != NULL)
                ? g_apcTagName[g_pstRecTab[i].iTag]
                : "?",
            g_pstRecTab[i].pcFile, g_pstRecTab[i].iLine);
    dm__mem_bt_print(g_pstRecTab[i].apvBt, g_pstRecTab[i].iBtDepth);
  }
  fprintf(stderr, "[mem-dbg] total leaked %u bytes\n", (unsigned)uTotal);
  dm__mem_unlock();
  return;
}

#endif /* DM_MEM_DEBUG */

/* ============================ 安全擦零（AUD-06） ============================ */

/* 防编译器死存储消除（DSE）的安全擦零（AUD-06） */
void dm_secure_zero(out void *pvBuf, in size_t uLen)
{
  if ((NULL == pvBuf) || (0u == uLen))
  {
    return;
  }
#if defined(_WIN32)
  SecureZeroMemory(pvBuf, uLen);
#else
  {
    volatile uint8 *pu8 = (volatile uint8 *)pvBuf;
    size_t uIdx = 0;
    for (uIdx = 0; uIdx < uLen; uIdx++)
    {
      pu8[uIdx] = 0u;
    }
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : "r"(pvBuf) : "memory");
#endif
  }
#endif
  return;
}

#ifdef __cplusplus
}
#endif
