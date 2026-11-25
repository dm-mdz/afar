/**
 ******************************************************************************
 * @file    thread.c
 * @author  MDM
 * @version 2.1.0
 * @brief   DM 跨平台线程实现（DD-DM-2.1 §2.1）：不透明句柄、启动就绪栅栏、
 *          零竞态 detach 原子引用计数、三端双向优先级映射与防截断线程名查询。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "dm.thread"
#include <dm/thread.h>
#include <dm/mem.h>
#include <dm/log.h>
#include "../../uv_adapter.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

extern int dm__uv_err_map(dm_in int iUvErr);

#ifndef _WIN32
_Static_assert(sizeof(pthread_t) <= sizeof(dm_thread_id_st),
               "pthread_t must fit inside dm_thread_id_st");
#endif

struct dm_thread
{
  dm_uv_thread_t hUvTid;
  dm_thread_id_st stId;
#if defined(__linux__)
  pid_t iLinuxTid;
#endif
  _Atomic uint32 u32Ref;
  _Atomic bool bExited;
};

typedef struct dm__thread_start
{
  dm_thread_st *pstThread;
  dm_thread_entry_fn pfEntry;
  void *pvArg;
  dm_thread_priority_et ePriority;
  bool_t bSetPriority;
  const char *pcName;
  dm_uv_mutex_t stReadyMtx;
  dm_uv_cond_t stReadyCond;
  bool_t bReady;
} dm__thread_start_st;

static bool_t dm__thread_prio_valid(dm_in dm_thread_priority_et ePriority)
{
  return ((ePriority >= DM_THREAD_PRIO_LOWEST) && (ePriority <= DM_THREAD_PRIO_HIGHEST)) ? DM_TRUE
                                                                                         : DM_FALSE;
}

static void dm__thread_release(dm_io dm_thread_st *pstThread)
{
  if (1u == atomic_fetch_sub_explicit(&pstThread->u32Ref, 1u, memory_order_acq_rel))
  {
    dm_free_tag(pstThread);
  }
  return;
}

static void dm__thread_trampoline(dm_io void *pvArg)
{
  dm__thread_start_st *pstStart = (dm__thread_start_st *)pvArg;
  dm_thread_st *pstThread = pstStart->pstThread;
  dm_thread_entry_fn pfEntry = pstStart->pfEntry;
  void *pvUser = pstStart->pvArg;

  pstThread->stId = dm_thread_self();
#if defined(__linux__)
  pstThread->iLinuxTid = (pid_t)syscall(SYS_gettid);
#endif
  if (NULL != pstStart->pcName)
  {
    (void)dm_thread_setname(pstStart->pcName);
  }
  if (DM_TRUE == pstStart->bSetPriority)
  {
    int iPrioErr = dm_thread_setpriority(NULL, pstStart->ePriority);
    if (DM_PERM == iPrioErr)
    {
      DM_LOG_WARN("dm_thread_create_ex: setpriority(%d) permission denied, continuing",
                  (int)pstStart->ePriority);
    }
  }
  dm_uv_mutex_lock(&pstStart->stReadyMtx);
  pstStart->bReady = DM_TRUE;
  dm_uv_cond_signal(&pstStart->stReadyCond);
  dm_uv_mutex_unlock(&pstStart->stReadyMtx);

  pfEntry(pvUser);

  atomic_store_explicit(&pstThread->bExited, true, memory_order_release);
  dm__thread_release(pstThread);
  return;
}

int dm_thread_create(dm_in dm_thread_entry_fn pfEntry, dm_io void *pvArg,
                     dm_out dm_thread_st **ppstThread)
{
  return dm_thread_create_ex(NULL, pfEntry, pvArg, ppstThread);
}

int dm_thread_create_ex(dm_in const dm_thread_opt_st *pstOpt, dm_in dm_thread_entry_fn pfEntry,
                        dm_io void *pvArg, dm_out dm_thread_st **ppstThread)
{
  dm_thread_st *pstThread = NULL;
  dm__thread_start_st stStart;
  int iErr = 0;

  if (NULL == ppstThread)
  {
    return DM_INVALID;
  }
  *ppstThread = NULL;
  if (NULL == pfEntry)
  {
    return DM_INVALID;
  }
  if ((NULL != pstOpt) && (DM_TRUE == pstOpt->bSetPriority) &&
      (!dm__thread_prio_valid(pstOpt->ePriority)))
  {
    return DM_INVALID;
  }
  pstThread = (dm_thread_st *)dm_calloc_tag(1u, sizeof(dm_thread_st), DM_MEM_TAG_THREAD);
  if (NULL == pstThread)
  {
    return DM_ALLOC;
  }
  atomic_init(&pstThread->u32Ref, 2u);
  atomic_init(&pstThread->bExited, false);

  memset(&stStart, 0, sizeof(stStart));
  stStart.pstThread = pstThread;
  stStart.pfEntry = pfEntry;
  stStart.pvArg = pvArg;
  if (NULL != pstOpt)
  {
    stStart.ePriority = pstOpt->ePriority;
    stStart.bSetPriority = pstOpt->bSetPriority;
    stStart.pcName = pstOpt->pcName;
  }

  iErr = dm_uv_mutex_init(&stStart.stReadyMtx);
  if (0 != iErr)
  {
    dm_free_tag(pstThread);
    return dm__uv_err_map(iErr);
  }
  iErr = dm_uv_cond_init(&stStart.stReadyCond);
  if (0 != iErr)
  {
    dm_uv_mutex_destroy(&stStart.stReadyMtx);
    dm_free_tag(pstThread);
    return dm__uv_err_map(iErr);
  }
  if ((NULL != pstOpt) && (pstOpt->uStackSize > 0u))
  {
    dm_uv_thread_options_st stUvOpt;
    memset(&stUvOpt, 0, sizeof(stUvOpt));
    stUvOpt.flags = DM_UV_THREAD_HAS_STACK_SIZE;
    stUvOpt.stack_size = pstOpt->uStackSize;
    iErr = dm_uv_thread_create_ex(&pstThread->hUvTid, &stUvOpt, dm__thread_trampoline, &stStart);
  }
  else
  {
    iErr = dm_uv_thread_create(&pstThread->hUvTid, dm__thread_trampoline, &stStart);
  }
  if (0 != iErr)
  {
    dm_uv_cond_destroy(&stStart.stReadyCond);
    dm_uv_mutex_destroy(&stStart.stReadyMtx);
    dm_free_tag(pstThread);
    return dm__uv_err_map(iErr);
  }

  dm_uv_mutex_lock(&stStart.stReadyMtx);
  while (DM_TRUE != stStart.bReady)
  {
    dm_uv_cond_wait(&stStart.stReadyCond, &stStart.stReadyMtx);
  }
  dm_uv_mutex_unlock(&stStart.stReadyMtx);
  dm_uv_cond_destroy(&stStart.stReadyCond);
  dm_uv_mutex_destroy(&stStart.stReadyMtx);
  *ppstThread = pstThread;
  return DM_SUCCESS;
}

int dm_thread_join(dm_io dm_thread_st *pstThread)
{
  int iErr = 0;

  if (NULL == pstThread)
  {
    return DM_INVALID;
  }
  if (DM_TRUE == dm_thread_equal(dm_thread_self(), pstThread->stId))
  {
    return DM_INVALID;
  }
  iErr = dm_uv_thread_join(&pstThread->hUvTid);
  if (0 != iErr)
  {
    return dm__uv_err_map(iErr);
  }
  dm__thread_release(pstThread);
  return DM_SUCCESS;
}

int dm_thread_detach(dm_io dm_thread_st *pstThread)
{
  int iErr = 0;

  if (NULL == pstThread)
  {
    return DM_INVALID;
  }
  iErr = dm_uv_thread_detach(&pstThread->hUvTid);
  if (0 != iErr)
  {
    return dm__uv_err_map(iErr);
  }
  dm__thread_release(pstThread);
  return DM_SUCCESS;
}

dm_thread_id_st dm_thread_self(void)
{
  dm_thread_id_st stId = {{0u, 0u}};
#ifdef _WIN32
  stId.au64Opaque[0] = (uint64)GetCurrentThreadId();
#else
  pthread_t hSelf = pthread_self();
  memcpy(stId.au64Opaque, &hSelf, sizeof(hSelf));
#endif
  return stId;
}

dm_thread_id_st dm_thread_id(dm_in const dm_thread_st *pstThread)
{
  dm_thread_id_st stZero = {{0u, 0u}};
  if (NULL == pstThread)
  {
    return stZero;
  }
  return pstThread->stId;
}

bool_t dm_thread_equal(dm_in dm_thread_id_st stA, dm_in dm_thread_id_st stB)
{
#ifdef _WIN32
  return ((stA.au64Opaque[0] == stB.au64Opaque[0]) && (stA.au64Opaque[1] == stB.au64Opaque[1]))
             ? DM_TRUE
             : DM_FALSE;
#else
  bool_t bZeroA = ((0u == stA.au64Opaque[0]) && (0u == stA.au64Opaque[1])) ? DM_TRUE : DM_FALSE;
  bool_t bZeroB = ((0u == stB.au64Opaque[0]) && (0u == stB.au64Opaque[1])) ? DM_TRUE : DM_FALSE;
  pthread_t hA;
  pthread_t hB;

  if ((DM_TRUE == bZeroA) || (DM_TRUE == bZeroB))
  {
    return (bZeroA == bZeroB) ? DM_TRUE : DM_FALSE;
  }
  memcpy(&hA, stA.au64Opaque, sizeof(hA));
  memcpy(&hB, stB.au64Opaque, sizeof(hB));
  return (0 != pthread_equal(hA, hB)) ? DM_TRUE : DM_FALSE;
#endif
}

int dm_thread_setname(dm_in const char *pcName)
{
  int iErr = 0;

  if (NULL == pcName)
  {
    return DM_INVALID;
  }
  iErr = dm_uv_thread_setname(pcName);
  return dm__uv_err_map(iErr);
}

int dm_thread_getname(dm_in const dm_thread_st *pstThread, dm_out char *pcBuf, dm_io size_t *puSize)
{
  char acTmp[256] = {0};
  dm_uv_thread_t hTid;
  size_t uLen = 0;
  int iErr = 0;

  if ((NULL == pcBuf) || (NULL == puSize) || (0u == *puSize))
  {
    return DM_INVALID;
  }
  if ((NULL != pstThread) && atomic_load_explicit(&pstThread->bExited, memory_order_acquire))
  {
    return DM_NOT_FOUND;
  }
#ifdef _WIN32
  hTid = (NULL == pstThread) ? GetCurrentThread() : pstThread->hUvTid;
#else
  hTid = (NULL == pstThread) ? pthread_self() : pstThread->hUvTid;
#endif
  iErr = dm_uv_thread_getname(&hTid, acTmp, sizeof(acTmp));
  if (0 != iErr)
  {
    return dm__uv_err_map(iErr);
  }
  acTmp[sizeof(acTmp) - 1u] = '\0';
  uLen = strlen(acTmp);
  if (*puSize <= uLen)
  {
    *puSize = uLen + 1u;
    return DM_TOO_BIG;
  }
  memcpy(pcBuf, acTmp, uLen + 1u);
  *puSize = uLen;
  return DM_SUCCESS;
}

#ifdef _WIN32
static int dm__win_err_map(dm_in DWORD dwErr)
{
  switch (dwErr)
  {
  case ERROR_ACCESS_DENIED:
    return DM_PERM;
  case ERROR_INVALID_HANDLE:
    return DM_NOT_FOUND;
  case ERROR_INVALID_PARAMETER:
    return DM_INVALID;
  default:
    return DM_IO;
  }
}

static dm_thread_priority_et dm__win_prio_to_dm(dm_in int iWinPrio)
{
  if (iWinPrio <= THREAD_PRIORITY_LOWEST)
  {
    return DM_THREAD_PRIO_LOWEST;
  }
  if (iWinPrio < THREAD_PRIORITY_NORMAL)
  {
    return DM_THREAD_PRIO_BELOW_NORMAL;
  }
  if (iWinPrio == THREAD_PRIORITY_NORMAL)
  {
    return DM_THREAD_PRIO_NORMAL;
  }
  if (iWinPrio < THREAD_PRIORITY_HIGHEST)
  {
    return DM_THREAD_PRIO_ABOVE_NORMAL;
  }
  return DM_THREAD_PRIO_HIGHEST;
}
#elif defined(__linux__)
static int dm__posix_err_map(dm_in int iSysErr)
{
  switch (iSysErr)
  {
  case EACCES:
  case EPERM:
    return DM_PERM;
  case ESRCH:
    return DM_NOT_FOUND;
  case EINVAL:
    return DM_INVALID;
  default:
    return DM_IO;
  }
}

static dm_thread_priority_et dm__linux_nice_to_dm(dm_in int iNice)
{
  if (iNice >= 4)
  {
    return DM_THREAD_PRIO_LOWEST;
  }
  if (iNice >= 1)
  {
    return DM_THREAD_PRIO_BELOW_NORMAL;
  }
  if (0 == iNice)
  {
    return DM_THREAD_PRIO_NORMAL;
  }
  if (iNice >= -3)
  {
    return DM_THREAD_PRIO_ABOVE_NORMAL;
  }
  return DM_THREAD_PRIO_HIGHEST;
}
#else
static void dm__posix_prio_points(dm_in int iPolicy, dm_out int aiPts[5])
{
  int iMin = sched_get_priority_min(iPolicy);
  int iMax = sched_get_priority_max(iPolicy);
  int iRange = iMax - iMin;

  aiPts[0] = iMin;
  aiPts[1] = iMin + (iRange / 4);
  aiPts[2] = iMin + (iRange / 2);
  aiPts[3] = iMin + ((iRange * 3) / 4);
  aiPts[4] = iMax;
  return;
}
#endif

int dm_thread_setpriority(dm_in const dm_thread_st *pstThread,
                          dm_in dm_thread_priority_et ePriority)
{
  if (!dm__thread_prio_valid(ePriority))
  {
    return DM_INVALID;
  }
  if ((NULL != pstThread) && atomic_load_explicit(&pstThread->bExited, memory_order_acquire))
  {
    return DM_NOT_FOUND;
  }
#ifdef _WIN32
  {
    static const int s_aiWinMap[5] = {
        THREAD_PRIORITY_LOWEST,       THREAD_PRIORITY_BELOW_NORMAL, THREAD_PRIORITY_NORMAL,
        THREAD_PRIORITY_ABOVE_NORMAL, THREAD_PRIORITY_HIGHEST,
    };
    HANDLE hThr = (NULL == pstThread) ? GetCurrentThread() : (HANDLE)pstThread->hUvTid;
    if (!SetThreadPriority(hThr, s_aiWinMap[(int)ePriority]))
    {
      return dm__win_err_map(GetLastError());
    }
    return DM_SUCCESS;
  }
#elif defined(__linux__)
  {
    pid_t iKtid = (NULL == pstThread) ? (pid_t)syscall(SYS_gettid) : pstThread->iLinuxTid;
    int iNice = 0 - (((int)ePriority - 2) * 2);
    if (0 != setpriority(PRIO_PROCESS, (id_t)iKtid, iNice))
    {
      return dm__posix_err_map(errno);
    }
    return DM_SUCCESS;
  }
#else
  {
    pthread_t hTid = (NULL == pstThread) ? pthread_self() : (pthread_t)pstThread->hUvTid;
    int iPolicy = 0;
    struct sched_param stParam;
    int aiPts[5] = {0};
    int iErr = 0;

    memset(&stParam, 0, sizeof(stParam));
    iErr = pthread_getschedparam(hTid, &iPolicy, &stParam);
    if (0 != iErr)
    {
      return dm__uv_err_map(-iErr);
    }
    dm__posix_prio_points(iPolicy, aiPts);
    stParam.sched_priority = aiPts[(int)ePriority];
    iErr = pthread_setschedparam(hTid, iPolicy, &stParam);
    return (0 == iErr) ? DM_SUCCESS : dm__uv_err_map(-iErr);
  }
#endif
}

int dm_thread_getpriority(dm_in const dm_thread_st *pstThread,
                          dm_out dm_thread_priority_et *pePriority)
{
  if (NULL == pePriority)
  {
    return DM_INVALID;
  }
  if ((NULL != pstThread) && atomic_load_explicit(&pstThread->bExited, memory_order_acquire))
  {
    return DM_NOT_FOUND;
  }
#ifdef _WIN32
  {
    HANDLE hThr = (NULL == pstThread) ? GetCurrentThread() : (HANDLE)pstThread->hUvTid;
    int iPrio = GetThreadPriority(hThr);
    if (THREAD_PRIORITY_ERROR_RETURN == iPrio)
    {
      return dm__win_err_map(GetLastError());
    }
    *pePriority = dm__win_prio_to_dm(iPrio);
    return DM_SUCCESS;
  }
#elif defined(__linux__)
  {
    pid_t iKtid = (NULL == pstThread) ? (pid_t)syscall(SYS_gettid) : pstThread->iLinuxTid;
    int iNice = 0;

    errno = 0;
    iNice = getpriority(PRIO_PROCESS, (id_t)iKtid);
    if ((-1 == iNice) && (0 != errno))
    {
      return dm__posix_err_map(errno);
    }
    *pePriority = dm__linux_nice_to_dm(iNice);
    return DM_SUCCESS;
  }
#else
  {
    pthread_t hTid = (NULL == pstThread) ? pthread_self() : (pthread_t)pstThread->hUvTid;
    int iPolicy = 0;
    struct sched_param stParam;
    int aiPts[5] = {0};
    int iBest = 0;
    int iBestDiff = 0;
    int iIdx = 0;
    int iErr = 0;

    memset(&stParam, 0, sizeof(stParam));
    iErr = pthread_getschedparam(hTid, &iPolicy, &stParam);
    if (0 != iErr)
    {
      return dm__uv_err_map(-iErr);
    }
    dm__posix_prio_points(iPolicy, aiPts);
    iBestDiff = abs(stParam.sched_priority - aiPts[0]);
    for (iIdx = 1; iIdx < 5; iIdx++)
    {
      int iDiff = abs(stParam.sched_priority - aiPts[iIdx]);
      if (iDiff < iBestDiff)
      {
        iBestDiff = iDiff;
        iBest = iIdx;
      }
    }
    *pePriority = (dm_thread_priority_et)iBest;
    return DM_SUCCESS;
  }
#endif
}

#ifdef __cplusplus
}
#endif
