/**
 ******************************************************************************
 * @file    loop.c
 * @author  MDM
 * @version 2.1.0
 * @brief   DM 独立事件循环、跨线程异步唤醒与循环绑定定时器实现（DD-DM-2.1 §2.2）。
 *          完全独立于 DM 默认主循环，严禁包含 dm_internal.h。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include <dm/loop.h>
#include <dm/mem.h>
#include <dm/thread.h>
#include "loop_internal.h"

#include <stdatomic.h>

struct dm_loop
{
  dm_uv_loop_st stUv;
  dm_thread_id_st stOwnerTid;
  void *pvData;
  uint32 u32OpenHandles;
};

struct dm_async
{
  dm_uv_async_st stUv;
  dm_loop_st *pstLoop;
  dm_async_fn pfCb;
  void *pvUser;
  void *pvData;
  dm_close_fn pfOnClose;
  void *pvCloseUser;
  _Atomic bool bClosing;
};

struct dm_loop_timer
{
  dm_uv_timer_st stUv;
  dm_loop_st *pstLoop;
  dm_loop_timer_fn pfCb;
  void *pvUser;
  void *pvData;
  dm_close_fn pfOnClose;
  void *pvCloseUser;
  bool_t bClosing;
};

/* ============================ 内部桥接契约 ============================ */

dm_uv_loop_st *dm__loop_raw(dm_in const dm_loop_st *pstLoop)
{
  if (NULL == pstLoop)
  {
    return NULL;
  }
  return (dm_uv_loop_st *)&pstLoop->stUv;
}

dm_uv_loop_st *dm__loop_uv(dm_in const dm_loop_st *pstLoop)
{
  return dm__loop_raw(pstLoop);
}

int dm__loop_check_owner(dm_in const dm_loop_st *pstLoop)
{
  if (NULL == pstLoop)
  {
    return DM_INVALID;
  }
  if (DM_TRUE != dm_thread_equal(dm_thread_self(), pstLoop->stOwnerTid))
  {
    DMASSERT(0 && "dm_loop API called from non-owner thread");
    return DM_WRONG_THREAD;
  }
  return DM_SUCCESS;
}

int dm__loop_check_thread(dm_in const dm_loop_st *pstLoop)
{
  return dm__loop_check_owner(pstLoop);
}

void dm__loop_handle_inc(dm_io dm_loop_st *pstLoop)
{
  if (NULL != pstLoop)
  {
    pstLoop->u32OpenHandles++;
  }
  return;
}

void dm__loop_handle_dec(dm_io dm_loop_st *pstLoop)
{
  if ((NULL != pstLoop) && (pstLoop->u32OpenHandles > 0u))
  {
    pstLoop->u32OpenHandles--;
  }
  return;
}

/* ============================ 独立事件循环 ============================ */

int dm_loop_create(dm_out dm_loop_st **ppstLoop)
{
  dm_loop_st *pstLoop = NULL;
  int iErr = 0;

  if (NULL == ppstLoop)
  {
    return DM_INVALID;
  }
  *ppstLoop = NULL;
  pstLoop = (dm_loop_st *)dm_calloc_tag(1u, sizeof(dm_loop_st), DM_MEM_TAG_LOOP);
  if (NULL == pstLoop)
  {
    return DM_ALLOC;
  }
  iErr = dm_uv_loop_init(&pstLoop->stUv);
  if (0 != iErr)
  {
    dm_free_tag(pstLoop);
    return dm__uv_err_map(iErr);
  }
  pstLoop->stOwnerTid = dm_thread_self();
  *ppstLoop = pstLoop;
  return DM_SUCCESS;
}

int dm_loop_destroy(dm_io dm_loop_st *pstLoop)
{
  int iErr = 0;

  if (NULL == pstLoop)
  {
    return DM_SUCCESS;
  }
  iErr = dm__loop_check_owner(pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (0u != pstLoop->u32OpenHandles)
  {
    return DM_BUSY;
  }
  iErr = dm_uv_loop_close(&pstLoop->stUv);
  if (0 != iErr)
  {
    return dm__uv_err_map(iErr);
  }
  dm_free_tag(pstLoop);
  return DM_SUCCESS;
}

int dm_loop_run(dm_io dm_loop_st *pstLoop, dm_in dm_loop_run_mode_et eMode)
{
  dm_uv_run_mode_et eUvMode = DM_UV_RUN_DEFAULT;
  int iErr = 0;
  int iRet = 0;

  if (NULL == pstLoop)
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  switch (eMode)
  {
  case DM_LOOP_RUN_DEFAULT:
    eUvMode = DM_UV_RUN_DEFAULT;
    break;
  case DM_LOOP_RUN_ONCE:
    eUvMode = DM_UV_RUN_ONCE;
    break;
  case DM_LOOP_RUN_NOWAIT:
    eUvMode = DM_UV_RUN_NOWAIT;
    break;
  default:
    return DM_INVALID;
  }
  iRet = dm_uv_run(&pstLoop->stUv, eUvMode);
  if (iRet < 0)
  {
    return dm__uv_err_map(iRet);
  }
  return iRet;
}

void dm_loop_stop(dm_io dm_loop_st *pstLoop)
{
  if (NULL == pstLoop)
  {
    return;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstLoop))
  {
    return;
  }
  dm_uv_stop(&pstLoop->stUv);
  return;
}

bool_t dm_loop_alive(dm_in const dm_loop_st *pstLoop)
{
  if (NULL == pstLoop)
  {
    return DM_FALSE;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstLoop))
  {
    return DM_FALSE;
  }
  return (0 != dm_uv_loop_alive(&pstLoop->stUv)) ? DM_TRUE : DM_FALSE;
}

uint64 dm_loop_now(dm_in const dm_loop_st *pstLoop)
{
  if (NULL == pstLoop)
  {
    return 0u;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstLoop))
  {
    return 0u;
  }
  return dm_uv_now(&pstLoop->stUv);
}

void dm_loop_update_time(dm_io dm_loop_st *pstLoop)
{
  if (NULL == pstLoop)
  {
    return;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstLoop))
  {
    return;
  }
  dm_uv_update_time(&pstLoop->stUv);
  return;
}

void dm_loop_set_data(dm_io dm_loop_st *pstLoop, dm_in void *pvData)
{
  if (NULL == pstLoop)
  {
    return;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstLoop))
  {
    return;
  }
  pstLoop->pvData = pvData;
  return;
}

void *dm_loop_get_data(dm_in const dm_loop_st *pstLoop)
{
  if (NULL == pstLoop)
  {
    return NULL;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstLoop))
  {
    return NULL;
  }
  return pstLoop->pvData;
}

/* ============================ 跨线程异步唤醒（dm_async） ============================ */

static void dm__async_on_uv(dm_io dm_uv_async_st *pstUv)
{
  dm_async_st *pstAsync = (dm_async_st *)dm_uv_handle_get_data((dm_uv_handle_st *)pstUv);

  if ((NULL == pstAsync) || atomic_load_explicit(&pstAsync->bClosing, memory_order_acquire))
  {
    return;
  }
  if (NULL != pstAsync->pfCb)
  {
    pstAsync->pfCb(pstAsync, pstAsync->pvUser);
  }
  return;
}

static void dm__async_on_close(dm_io dm_uv_handle_st *pstUv)
{
  dm_async_st *pstAsync = (dm_async_st *)dm_uv_handle_get_data(pstUv);
  dm_loop_st *pstLoop = NULL;
  dm_close_fn pfOnClose = NULL;
  void *pvCloseUser = NULL;

  if (NULL == pstAsync)
  {
    return;
  }
  pstLoop = pstAsync->pstLoop;
  pfOnClose = pstAsync->pfOnClose;
  pvCloseUser = pstAsync->pvCloseUser;
  dm__loop_handle_dec(pstLoop);
  dm_free_tag(pstAsync);
  if (NULL != pfOnClose)
  {
    pfOnClose(pvCloseUser);
  }
  return;
}

int dm_async_create(dm_io dm_loop_st *pstLoop, dm_in dm_async_fn pfCb, dm_in void *pvUser,
                    dm_out dm_async_st **ppstOut)
{
  dm_async_st *pstAsync = NULL;
  int iErr = 0;

  if (NULL == ppstOut)
  {
    return DM_INVALID;
  }
  *ppstOut = NULL;
  if ((NULL == pstLoop) || (NULL == pfCb))
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  pstAsync = (dm_async_st *)dm_calloc_tag(1u, sizeof(dm_async_st), DM_MEM_TAG_LOOP);
  if (NULL == pstAsync)
  {
    return DM_ALLOC;
  }
  pstAsync->pstLoop = pstLoop;
  pstAsync->pfCb = pfCb;
  pstAsync->pvUser = pvUser;
  atomic_init(&pstAsync->bClosing, false);

  iErr = dm_uv_async_init(&pstLoop->stUv, &pstAsync->stUv, dm__async_on_uv);
  if (0 != iErr)
  {
    dm_free_tag(pstAsync);
    return dm__uv_err_map(iErr);
  }
  dm_uv_handle_set_data((dm_uv_handle_st *)&pstAsync->stUv, pstAsync);
  dm__loop_handle_inc(pstLoop);
  *ppstOut = pstAsync;
  return DM_SUCCESS;
}

int dm_async_send(dm_io dm_async_st *pstAsync)
{
  int iErr = 0;

  if (NULL == pstAsync)
  {
    return DM_INVALID;
  }
  if (atomic_load_explicit(&pstAsync->bClosing, memory_order_acquire))
  {
    return DM_SHUTTING_DOWN;
  }
  iErr = dm_uv_async_send(&pstAsync->stUv);
  return dm__uv_err_map(iErr);
}

int dm_async_close(dm_io dm_async_st *pstAsync, dm_in dm_close_fn pfOnClose, dm_in void *pvUser)
{
  int iErr = 0;

  if (NULL == pstAsync)
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstAsync->pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (atomic_exchange_explicit(&pstAsync->bClosing, true, memory_order_acq_rel))
  {
    return DM_SUCCESS;
  }
  pstAsync->pfOnClose = pfOnClose;
  pstAsync->pvCloseUser = pvUser;
  dm_uv_close((dm_uv_handle_st *)&pstAsync->stUv, dm__async_on_close);
  return DM_SUCCESS;
}

void dm_async_set_data(dm_io dm_async_st *pstAsync, dm_in void *pvData)
{
  if (NULL == pstAsync)
  {
    return;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstAsync->pstLoop))
  {
    return;
  }
  pstAsync->pvData = pvData;
  return;
}

void *dm_async_get_data(dm_in const dm_async_st *pstAsync)
{
  if (NULL == pstAsync)
  {
    return NULL;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstAsync->pstLoop))
  {
    return NULL;
  }
  return pstAsync->pvData;
}

/* ============================ 循环绑定定时器（dm_loop_timer） ============================ */

static void dm__timer_on_uv(dm_io dm_uv_timer_st *pstUv)
{
  dm_loop_timer_st *pstTimer = (dm_loop_timer_st *)dm_uv_handle_get_data((dm_uv_handle_st *)pstUv);

  if ((NULL == pstTimer) || (DM_TRUE == pstTimer->bClosing))
  {
    return;
  }
  if (NULL != pstTimer->pfCb)
  {
    pstTimer->pfCb(pstTimer, pstTimer->pvUser);
  }
  return;
}

static void dm__timer_on_close(dm_io dm_uv_handle_st *pstUv)
{
  dm_loop_timer_st *pstTimer = (dm_loop_timer_st *)dm_uv_handle_get_data(pstUv);
  dm_loop_st *pstLoop = NULL;
  dm_close_fn pfOnClose = NULL;
  void *pvCloseUser = NULL;

  if (NULL == pstTimer)
  {
    return;
  }
  pstLoop = pstTimer->pstLoop;
  pfOnClose = pstTimer->pfOnClose;
  pvCloseUser = pstTimer->pvCloseUser;
  dm__loop_handle_dec(pstLoop);
  dm_free_tag(pstTimer);
  if (NULL != pfOnClose)
  {
    pfOnClose(pvCloseUser);
  }
  return;
}

int dm_loop_timer_create(dm_io dm_loop_st *pstLoop, dm_out dm_loop_timer_st **ppstOut)
{
  dm_loop_timer_st *pstTimer = NULL;
  int iErr = 0;

  if (NULL == ppstOut)
  {
    return DM_INVALID;
  }
  *ppstOut = NULL;
  if (NULL == pstLoop)
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  pstTimer = (dm_loop_timer_st *)dm_calloc_tag(1u, sizeof(dm_loop_timer_st), DM_MEM_TAG_LOOP);
  if (NULL == pstTimer)
  {
    return DM_ALLOC;
  }
  pstTimer->pstLoop = pstLoop;
  iErr = dm_uv_timer_init(&pstLoop->stUv, &pstTimer->stUv);
  if (0 != iErr)
  {
    dm_free_tag(pstTimer);
    return dm__uv_err_map(iErr);
  }
  dm_uv_handle_set_data((dm_uv_handle_st *)&pstTimer->stUv, pstTimer);
  dm__loop_handle_inc(pstLoop);
  *ppstOut = pstTimer;
  return DM_SUCCESS;
}

int dm_loop_timer_start(dm_io dm_loop_timer_st *pstTimer, dm_in uint64 u64TimeoutMs,
                        dm_in uint64 u64RepeatMs, dm_in dm_loop_timer_fn pfCb, dm_in void *pvUser)
{
  int iErr = 0;

  if ((NULL == pstTimer) || (NULL == pfCb))
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstTimer->pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (DM_TRUE == pstTimer->bClosing)
  {
    return DM_SHUTTING_DOWN;
  }
  pstTimer->pfCb = pfCb;
  pstTimer->pvUser = pvUser;
  iErr = dm_uv_timer_start(&pstTimer->stUv, dm__timer_on_uv, u64TimeoutMs, u64RepeatMs);
  return dm__uv_err_map(iErr);
}

int dm_loop_timer_stop(dm_io dm_loop_timer_st *pstTimer)
{
  int iErr = 0;

  if (NULL == pstTimer)
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstTimer->pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (DM_TRUE == pstTimer->bClosing)
  {
    return DM_SHUTTING_DOWN;
  }
  iErr = dm_uv_timer_stop(&pstTimer->stUv);
  return dm__uv_err_map(iErr);
}

int dm_loop_timer_again(dm_io dm_loop_timer_st *pstTimer)
{
  int iErr = 0;

  if (NULL == pstTimer)
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstTimer->pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (DM_TRUE == pstTimer->bClosing)
  {
    return DM_SHUTTING_DOWN;
  }
  iErr = dm_uv_timer_again(&pstTimer->stUv);
  return dm__uv_err_map(iErr);
}

void dm_loop_timer_set_repeat(dm_io dm_loop_timer_st *pstTimer, dm_in uint64 u64RepeatMs)
{
  if (NULL == pstTimer)
  {
    return;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstTimer->pstLoop))
  {
    return;
  }
  if (DM_TRUE == pstTimer->bClosing)
  {
    return;
  }
  dm_uv_timer_set_repeat(&pstTimer->stUv, u64RepeatMs);
  return;
}

uint64 dm_loop_timer_get_repeat(dm_in const dm_loop_timer_st *pstTimer)
{
  if (NULL == pstTimer)
  {
    return 0u;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstTimer->pstLoop))
  {
    return 0u;
  }
  return dm_uv_timer_get_repeat(&pstTimer->stUv);
}

uint64 dm_loop_timer_get_due_in(dm_in const dm_loop_timer_st *pstTimer)
{
  if (NULL == pstTimer)
  {
    return 0u;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstTimer->pstLoop))
  {
    return 0u;
  }
  return dm_uv_timer_get_due_in(&pstTimer->stUv);
}

int dm_loop_timer_close(dm_io dm_loop_timer_st *pstTimer, dm_in dm_close_fn pfOnClose,
                        dm_in void *pvUser)
{
  int iErr = 0;

  if (NULL == pstTimer)
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstTimer->pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (DM_TRUE == pstTimer->bClosing)
  {
    return DM_SUCCESS;
  }
  pstTimer->bClosing = DM_TRUE;
  pstTimer->pfOnClose = pfOnClose;
  pstTimer->pvCloseUser = pvUser;
  (void)dm_uv_timer_stop(&pstTimer->stUv);
  dm_uv_close((dm_uv_handle_st *)&pstTimer->stUv, dm__timer_on_close);
  return DM_SUCCESS;
}

void dm_loop_timer_set_data(dm_io dm_loop_timer_st *pstTimer, dm_in void *pvData)
{
  if (NULL == pstTimer)
  {
    return;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstTimer->pstLoop))
  {
    return;
  }
  pstTimer->pvData = pvData;
  return;
}

void *dm_loop_timer_get_data(dm_in const dm_loop_timer_st *pstTimer)
{
  if (NULL == pstTimer)
  {
    return NULL;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstTimer->pstLoop))
  {
    return NULL;
  }
  return pstTimer->pvData;
}

#ifdef __cplusplus
}
#endif
