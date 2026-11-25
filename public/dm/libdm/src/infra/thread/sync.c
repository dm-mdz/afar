/**
 ******************************************************************************
 * @file    sync.c
 * @author  MDM
 * @version 2.1.0
 * @brief   DM 跨平台同步原语实现（DD-DM-2.1 §2.1）：互斥锁、读写锁、条件变量
 *          （含安全分片与 Windows 亚毫秒向上取整）、计数信号量与 TLS 键。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include <dm/thread.h>
#include <dm/mem.h>
#include "../../uv_adapter.h"

#include <limits.h>

extern int dm__uv_err_map(dm_in int iUvErr);

/* 单次条件变量分片等待上限：~24.8 天，严格小于 Windows DWORD INFINITE 与 POSIX 溢出边界 */
#define DM__COND_CHUNK_NS ((uint64)0x7FFFFFFEu * 1000000ull)
#define DM__NS_PER_MS 1000000ull

struct dm_mutex
{
  dm_uv_mutex_t stUv;
};

struct dm_rwlock
{
  dm_uv_rwlock_t stUv;
};

struct dm_cond
{
  dm_uv_cond_t stUv;
};

struct dm_sem
{
#ifdef __APPLE__
  /* macOS uv_sem_t 使用 Mach semaphore_t（Apple Clang TSan 不可见），用 mutex+cond 保持语义等价 */
  dm_uv_mutex_t stMtx;
  dm_uv_cond_t stCond;
  uint32 u32Val;
#else
  dm_uv_sem_t stUv;
#endif
};

struct dm_key
{
  dm_uv_key_t stUv;
};

static int dm__sync_try_err_map(dm_in int iUvErr)
{
  if (0 == iUvErr)
  {
    return DM_SUCCESS;
  }
  if ((DM_UV_EBUSY == iUvErr) || (DM_UV_EAGAIN == iUvErr))
  {
    return DM_AGAIN;
  }
  return dm__uv_err_map(iUvErr);
}

/* ============================ 互斥锁 ============================ */

static int dm__mutex_create_impl(dm_in bool_t bRecursive, dm_out dm_mutex_st **ppstOut)
{
  dm_mutex_st *pstMtx = NULL;
  int iErr = 0;

  if (NULL == ppstOut)
  {
    return DM_INVALID;
  }
  *ppstOut = NULL;
  pstMtx = (dm_mutex_st *)dm_calloc_tag(1u, sizeof(dm_mutex_st), DM_MEM_TAG_THREAD);
  if (NULL == pstMtx)
  {
    return DM_ALLOC;
  }
  iErr = (DM_TRUE == bRecursive) ? dm_uv_mutex_init_recursive(&pstMtx->stUv)
                                 : dm_uv_mutex_init(&pstMtx->stUv);
  if (0 != iErr)
  {
    dm_free_tag(pstMtx);
    return dm__uv_err_map(iErr);
  }
  *ppstOut = pstMtx;
  return DM_SUCCESS;
}

int dm_mutex_create(dm_out dm_mutex_st **ppstOut)
{
  return dm__mutex_create_impl(DM_FALSE, ppstOut);
}

int dm_mutex_create_recursive(dm_out dm_mutex_st **ppstOut)
{
  return dm__mutex_create_impl(DM_TRUE, ppstOut);
}

void dm_mutex_destroy(dm_io dm_mutex_st *pstMutex)
{
  if (NULL == pstMutex)
  {
    return;
  }
  dm_uv_mutex_destroy(&pstMutex->stUv);
  dm_free_tag(pstMutex);
  return;
}

void dm_mutex_lock(dm_io dm_mutex_st *pstMutex)
{
  if (NULL == pstMutex)
  {
    return;
  }
  dm_uv_mutex_lock(&pstMutex->stUv);
  return;
}

int dm_mutex_trylock(dm_io dm_mutex_st *pstMutex)
{
  if (NULL == pstMutex)
  {
    return DM_INVALID;
  }
  return dm__sync_try_err_map(dm_uv_mutex_trylock(&pstMutex->stUv));
}

void dm_mutex_unlock(dm_io dm_mutex_st *pstMutex)
{
  if (NULL == pstMutex)
  {
    return;
  }
  dm_uv_mutex_unlock(&pstMutex->stUv);
  return;
}

/* ============================ 读写锁 ============================ */

int dm_rwlock_create(dm_out dm_rwlock_st **ppstOut)
{
  dm_rwlock_st *pstLock = NULL;
  int iErr = 0;

  if (NULL == ppstOut)
  {
    return DM_INVALID;
  }
  *ppstOut = NULL;
  pstLock = (dm_rwlock_st *)dm_calloc_tag(1u, sizeof(dm_rwlock_st), DM_MEM_TAG_THREAD);
  if (NULL == pstLock)
  {
    return DM_ALLOC;
  }
  iErr = dm_uv_rwlock_init(&pstLock->stUv);
  if (0 != iErr)
  {
    dm_free_tag(pstLock);
    return dm__uv_err_map(iErr);
  }
  *ppstOut = pstLock;
  return DM_SUCCESS;
}

void dm_rwlock_destroy(dm_io dm_rwlock_st *pstLock)
{
  if (NULL == pstLock)
  {
    return;
  }
  dm_uv_rwlock_destroy(&pstLock->stUv);
  dm_free_tag(pstLock);
  return;
}

void dm_rwlock_rdlock(dm_io dm_rwlock_st *pstLock)
{
  if (NULL == pstLock)
  {
    return;
  }
  dm_uv_rwlock_rdlock(&pstLock->stUv);
  return;
}

int dm_rwlock_tryrdlock(dm_io dm_rwlock_st *pstLock)
{
  if (NULL == pstLock)
  {
    return DM_INVALID;
  }
  return dm__sync_try_err_map(dm_uv_rwlock_tryrdlock(&pstLock->stUv));
}

void dm_rwlock_rdunlock(dm_io dm_rwlock_st *pstLock)
{
  if (NULL == pstLock)
  {
    return;
  }
  dm_uv_rwlock_rdunlock(&pstLock->stUv);
  return;
}

void dm_rwlock_wrlock(dm_io dm_rwlock_st *pstLock)
{
  if (NULL == pstLock)
  {
    return;
  }
  dm_uv_rwlock_wrlock(&pstLock->stUv);
  return;
}

int dm_rwlock_trywrlock(dm_io dm_rwlock_st *pstLock)
{
  if (NULL == pstLock)
  {
    return DM_INVALID;
  }
  return dm__sync_try_err_map(dm_uv_rwlock_trywrlock(&pstLock->stUv));
}

void dm_rwlock_wrunlock(dm_io dm_rwlock_st *pstLock)
{
  if (NULL == pstLock)
  {
    return;
  }
  dm_uv_rwlock_wrunlock(&pstLock->stUv);
  return;
}

/* ============================ 条件变量 ============================ */

int dm_cond_create(dm_out dm_cond_st **ppstOut)
{
  dm_cond_st *pstCond = NULL;
  int iErr = 0;

  if (NULL == ppstOut)
  {
    return DM_INVALID;
  }
  *ppstOut = NULL;
  pstCond = (dm_cond_st *)dm_calloc_tag(1u, sizeof(dm_cond_st), DM_MEM_TAG_THREAD);
  if (NULL == pstCond)
  {
    return DM_ALLOC;
  }
  iErr = dm_uv_cond_init(&pstCond->stUv);
  if (0 != iErr)
  {
    dm_free_tag(pstCond);
    return dm__uv_err_map(iErr);
  }
  *ppstOut = pstCond;
  return DM_SUCCESS;
}

void dm_cond_destroy(dm_io dm_cond_st *pstCond)
{
  if (NULL == pstCond)
  {
    return;
  }
  dm_uv_cond_destroy(&pstCond->stUv);
  dm_free_tag(pstCond);
  return;
}

void dm_cond_signal(dm_io dm_cond_st *pstCond)
{
  if (NULL == pstCond)
  {
    return;
  }
  dm_uv_cond_signal(&pstCond->stUv);
  return;
}

void dm_cond_broadcast(dm_io dm_cond_st *pstCond)
{
  if (NULL == pstCond)
  {
    return;
  }
  dm_uv_cond_broadcast(&pstCond->stUv);
  return;
}

void dm_cond_wait(dm_io dm_cond_st *pstCond, dm_io dm_mutex_st *pstMutex)
{
  if ((NULL == pstCond) || (NULL == pstMutex))
  {
    return;
  }
  dm_uv_cond_wait(&pstCond->stUv, &pstMutex->stUv);
  return;
}

int dm_cond_timedwait(dm_io dm_cond_st *pstCond, dm_io dm_mutex_st *pstMutex,
                      dm_in uint64 u64TimeoutNs)
{
  uint64 u64Remain = u64TimeoutNs;
  uint64 u64Start = 0;
  int iErr = 0;

  if ((NULL == pstCond) || (NULL == pstMutex))
  {
    return DM_INVALID;
  }
  if (0u == u64Remain)
  {
    iErr = dm_uv_cond_timedwait(&pstCond->stUv, &pstMutex->stUv, 0u);
    return dm__uv_err_map(iErr);
  }
#ifdef _WIN32
  /* AUD-V21-P0-04：Windows 毫秒精度下 1..999999 ns 向上取整为 1 ms，避免瞬时截断为 0 ms */
  if (u64Remain < DM__NS_PER_MS)
  {
    u64Remain = DM__NS_PER_MS;
  }
#endif
  if (u64Remain <= DM__COND_CHUNK_NS)
  {
    iErr = dm_uv_cond_timedwait(&pstCond->stUv, &pstMutex->stUv, u64Remain);
    return dm__uv_err_map(iErr);
  }
  u64Start = dm_hrtime();
  while (u64Remain > 0u)
  {
    uint64 u64Slice = (u64Remain > DM__COND_CHUNK_NS) ? DM__COND_CHUNK_NS : u64Remain;
    uint64 u64Elapsed = 0;
#ifdef _WIN32
    if (u64Slice < DM__NS_PER_MS)
    {
      u64Slice = DM__NS_PER_MS;
    }
#endif
    iErr = dm_uv_cond_timedwait(&pstCond->stUv, &pstMutex->stUv, u64Slice);
    if (DM_UV_ETIMEDOUT != iErr)
    {
      return dm__uv_err_map(iErr);
    }
    u64Elapsed = dm_hrtime() - u64Start;
    if (u64Elapsed >= u64TimeoutNs)
    {
      return DM_TIMEOUT;
    }
    u64Remain = u64TimeoutNs - u64Elapsed;
  }
  return DM_TIMEOUT;
}

int dm_cond_timedwait_ms(dm_io dm_cond_st *pstCond, dm_io dm_mutex_st *pstMutex,
                         dm_in uint64 u64TimeoutMs)
{
  uint64 u64Ns = UINT64_MAX;

  if (u64TimeoutMs <= (UINT64_MAX / DM__NS_PER_MS))
  {
    u64Ns = u64TimeoutMs * DM__NS_PER_MS;
  }
  return dm_cond_timedwait(pstCond, pstMutex, u64Ns);
}

/* ============================ 计数信号量 ============================ */

int dm_sem_create(dm_in uint32 u32InitValue, dm_out dm_sem_st **ppstOut)
{
  dm_sem_st *pstSem = NULL;
  int iErr = 0;

  if (NULL == ppstOut)
  {
    return DM_INVALID;
  }
  *ppstOut = NULL;
  pstSem = (dm_sem_st *)dm_calloc_tag(1u, sizeof(dm_sem_st), DM_MEM_TAG_THREAD);
  if (NULL == pstSem)
  {
    return DM_ALLOC;
  }
#ifdef __APPLE__
  iErr = dm_uv_mutex_init(&pstSem->stMtx);
  if (0 != iErr)
  {
    dm_free_tag(pstSem);
    return dm__uv_err_map(iErr);
  }
  iErr = dm_uv_cond_init(&pstSem->stCond);
  if (0 != iErr)
  {
    dm_uv_mutex_destroy(&pstSem->stMtx);
    dm_free_tag(pstSem);
    return dm__uv_err_map(iErr);
  }
  pstSem->u32Val = u32InitValue;
#else
  iErr = dm_uv_sem_init(&pstSem->stUv, (unsigned int)u32InitValue);
  if (0 != iErr)
  {
    dm_free_tag(pstSem);
    return dm__uv_err_map(iErr);
  }
#endif
  *ppstOut = pstSem;
  return DM_SUCCESS;
}

void dm_sem_destroy(dm_io dm_sem_st *pstSem)
{
  if (NULL == pstSem)
  {
    return;
  }
#ifdef __APPLE__
  dm_uv_cond_destroy(&pstSem->stCond);
  dm_uv_mutex_destroy(&pstSem->stMtx);
#else
  dm_uv_sem_destroy(&pstSem->stUv);
#endif
  dm_free_tag(pstSem);
  return;
}

void dm_sem_post(dm_io dm_sem_st *pstSem)
{
  if (NULL == pstSem)
  {
    return;
  }
#ifdef __APPLE__
  dm_uv_mutex_lock(&pstSem->stMtx);
  pstSem->u32Val++;
  dm_uv_cond_signal(&pstSem->stCond);
  dm_uv_mutex_unlock(&pstSem->stMtx);
#else
  dm_uv_sem_post(&pstSem->stUv);
#endif
  return;
}

void dm_sem_wait(dm_io dm_sem_st *pstSem)
{
  if (NULL == pstSem)
  {
    return;
  }
#ifdef __APPLE__
  dm_uv_mutex_lock(&pstSem->stMtx);
  while (0u == pstSem->u32Val)
  {
    dm_uv_cond_wait(&pstSem->stCond, &pstSem->stMtx);
  }
  pstSem->u32Val--;
  dm_uv_mutex_unlock(&pstSem->stMtx);
#else
  dm_uv_sem_wait(&pstSem->stUv);
#endif
  return;
}

int dm_sem_trywait(dm_io dm_sem_st *pstSem)
{
  if (NULL == pstSem)
  {
    return DM_INVALID;
  }
#ifdef __APPLE__
  dm_uv_mutex_lock(&pstSem->stMtx);
  if (0u == pstSem->u32Val)
  {
    dm_uv_mutex_unlock(&pstSem->stMtx);
    return DM_AGAIN;
  }
  pstSem->u32Val--;
  dm_uv_mutex_unlock(&pstSem->stMtx);
  return DM_SUCCESS;
#else
  return dm__sync_try_err_map(dm_uv_sem_trywait(&pstSem->stUv));
#endif
}

/* ============================ 线程局部存储（TLS） ============================ */

int dm_key_create(dm_out dm_key_st **ppstOut)
{
  dm_key_st *pstKey = NULL;
  int iErr = 0;

  if (NULL == ppstOut)
  {
    return DM_INVALID;
  }
  *ppstOut = NULL;
  pstKey = (dm_key_st *)dm_calloc_tag(1u, sizeof(dm_key_st), DM_MEM_TAG_THREAD);
  if (NULL == pstKey)
  {
    return DM_ALLOC;
  }
  iErr = dm_uv_key_create(&pstKey->stUv);
  if (0 != iErr)
  {
    dm_free_tag(pstKey);
    return dm__uv_err_map(iErr);
  }
  *ppstOut = pstKey;
  return DM_SUCCESS;
}

void dm_key_destroy(dm_io dm_key_st *pstKey)
{
  if (NULL == pstKey)
  {
    return;
  }
  dm_uv_key_delete(&pstKey->stUv);
  dm_free_tag(pstKey);
  return;
}

void *dm_key_get(dm_in const dm_key_st *pstKey)
{
  if (NULL == pstKey)
  {
    return NULL;
  }
  return dm_uv_key_get((dm_uv_key_t *)&pstKey->stUv);
}

void dm_key_set(dm_io dm_key_st *pstKey, dm_in void *pvValue)
{
  if (NULL == pstKey)
  {
    return;
  }
  dm_uv_key_set(&pstKey->stUv, pvValue);
  return;
}

#ifdef __cplusplus
}
#endif
