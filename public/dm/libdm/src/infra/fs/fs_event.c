/**
 ******************************************************************************
 * @file    fs_event.c
 * @author  MDM
 * @version 2.1.0
 * @brief   DM 文件/目录变更监控实现（DD-DM-2.1 §2.5）：
 *          绑定显式 dm_loop_st*、线程亲和守卫、bClosing 守卫、Linux 递归显式 DM_UNSUPPORTED。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include <dm/fs.h>
#include <dm/mem.h>
#include "../../runtime/loop_internal.h"

#include <string.h>

struct dm_fs_event
{
  dm_uv_fs_event_st stUv;
  dm_loop_st *pstLoop;
  dm_fs_event_cb pfCb;
  void *pvUser;
  dm_close_fn pfOnClose;
  void *pvCloseUser;
  bool_t bClosing;
};

static void dm__fs_event_on_uv(dm_io dm_uv_fs_event_st *pstUv, dm_in const char *pcFilename,
                               dm_in int iEvents, dm_in int iStatus)
{
  dm_fs_event_st *pstEv = (dm_fs_event_st *)dm_uv_handle_get_data((dm_uv_handle_st *)pstUv);
  uint32 u32Ev = 0u;
  int iDmErr = DM_SUCCESS;

  if ((NULL == pstEv) || (DM_TRUE == pstEv->bClosing))
  {
    return;
  }
  if (0 != (iEvents & DM_UV_RENAME))
  {
    u32Ev |= DM_FS_EV_RENAME;
  }
  if (0 != (iEvents & DM_UV_CHANGE))
  {
    u32Ev |= DM_FS_EV_CHANGE;
  }
  if (0 != iStatus)
  {
    iDmErr = dm__uv_err_map(iStatus);
  }
  if (NULL != pstEv->pfCb)
  {
    pstEv->pfCb(pstEv, iDmErr, pcFilename, u32Ev, pstEv->pvUser);
  }
  return;
}

static void dm__fs_event_on_close(dm_io dm_uv_handle_st *pstUv)
{
  dm_fs_event_st *pstEv = (dm_fs_event_st *)dm_uv_handle_get_data(pstUv);
  dm_loop_st *pstLoop = NULL;
  dm_close_fn pfOnClose = NULL;
  void *pvCloseUser = NULL;

  if (NULL == pstEv)
  {
    return;
  }
  pstLoop = pstEv->pstLoop;
  pfOnClose = pstEv->pfOnClose;
  pvCloseUser = pstEv->pvCloseUser;
  dm__loop_handle_dec(pstLoop);
  dm_free_tag(pstEv);
  if (NULL != pfOnClose)
  {
    pfOnClose(pvCloseUser);
  }
  return;
}

int dm_fs_event_create(dm_io dm_loop_st *pstLoop, dm_out dm_fs_event_st **ppstOut)
{
  dm_fs_event_st *pstEv = NULL;
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
  pstEv = (dm_fs_event_st *)dm_calloc_tag(1u, sizeof(dm_fs_event_st), DM_MEM_TAG_FS);
  if (NULL == pstEv)
  {
    return DM_ALLOC;
  }
  pstEv->pstLoop = pstLoop;
  iErr = dm_uv_fs_event_init(dm__loop_raw(pstLoop), &pstEv->stUv);
  if (0 != iErr)
  {
    dm_free_tag(pstEv);
    return dm__uv_err_map(iErr);
  }
  dm_uv_handle_set_data((dm_uv_handle_st *)&pstEv->stUv, pstEv);
  dm__loop_handle_inc(pstLoop);
  *ppstOut = pstEv;
  return DM_SUCCESS;
}

int dm_fs_event_start(dm_io dm_fs_event_st *pstEv, dm_in const char *pcPathUtf8,
                      dm_in uint32 u32Flags, dm_in dm_fs_event_cb pfCb, dm_in void *pvUser)
{
  unsigned int uiUvFlags = 0u;
  int iErr = 0;

  if ((NULL == pstEv) || (NULL == pcPathUtf8) || ('\0' == pcPathUtf8[0]) || (NULL == pfCb))
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstEv->pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (DM_TRUE == pstEv->bClosing)
  {
    return DM_SHUTTING_DOWN;
  }
  /* AUD-V21-P0-06：未知标志位统一拒返 DM_INVALID；非 Win/macOS 递归显式返 DM_UNSUPPORTED */
  if (0u != (u32Flags & ~DM_FS_EVENT_F_RECURSIVE))
  {
    return DM_INVALID;
  }
#if !defined(_WIN32) && !defined(__APPLE__)
  if (0u != (u32Flags & DM_FS_EVENT_F_RECURSIVE))
  {
    return DM_UNSUPPORTED;
  }
#endif
  if (0u != (u32Flags & DM_FS_EVENT_F_RECURSIVE))
  {
    uiUvFlags = DM_UV_FS_EVENT_RECURSIVE;
  }
  pstEv->pfCb = pfCb;
  pstEv->pvUser = pvUser;
  iErr = dm_uv_fs_event_start(&pstEv->stUv, dm__fs_event_on_uv, pcPathUtf8, uiUvFlags);
  return dm__uv_err_map(iErr);
}

int dm_fs_event_stop(dm_io dm_fs_event_st *pstEv)
{
  int iErr = 0;

  if (NULL == pstEv)
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstEv->pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (DM_TRUE == pstEv->bClosing)
  {
    return DM_SHUTTING_DOWN;
  }
  iErr = dm_uv_fs_event_stop(&pstEv->stUv);
  return dm__uv_err_map(iErr);
}

int dm_fs_event_getpath(dm_in const dm_fs_event_st *pstEv, dm_out char *pcBuf, dm_io size_t *puSize)
{
  char acTmp[4096] = {0};
  size_t uGot = sizeof(acTmp);
  char *pcHeap = NULL;
  const char *pcSrc = acTmp;
  size_t uLen = 0;
  int iErr = 0;

  if ((NULL == pstEv) || (NULL == pcBuf) || (NULL == puSize) || (0u == *puSize))
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstEv->pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr = dm_uv_fs_event_getpath((dm_uv_fs_event_st *)&pstEv->stUv, acTmp, &uGot);
  if (DM_UV_ENOBUFS == iErr)
  {
    size_t uAlloc = (uGot > sizeof(acTmp)) ? uGot : 32768u;
    pcHeap = (char *)dm_malloc_tag(uAlloc, DM_MEM_TAG_FS);
    if (NULL == pcHeap)
    {
      return DM_ALLOC;
    }
    iErr = dm_uv_fs_event_getpath((dm_uv_fs_event_st *)&pstEv->stUv, pcHeap, &uAlloc);
    if (0 != iErr)
    {
      dm_free_tag(pcHeap);
      return dm__uv_err_map(iErr);
    }
    pcHeap[uAlloc] = '\0';
    pcSrc = pcHeap;
    uLen = uAlloc;
  }
  else if (0 != iErr)
  {
    return dm__uv_err_map(iErr);
  }
  else
  {
    acTmp[uGot] = '\0';
    uLen = uGot;
  }
  if (*puSize <= uLen)
  {
    *puSize = uLen + 1u;
    dm_free_tag(pcHeap);
    return DM_TOO_BIG;
  }
  memcpy(pcBuf, pcSrc, uLen + 1u);
  *puSize = uLen;
  dm_free_tag(pcHeap);
  return DM_SUCCESS;
}

int dm_fs_event_close(dm_io dm_fs_event_st *pstEv, dm_in dm_close_fn pfOnClose, dm_in void *pvUser)
{
  int iErr = 0;

  if (NULL == pstEv)
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstEv->pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (DM_TRUE == pstEv->bClosing)
  {
    return DM_SUCCESS;
  }
  pstEv->bClosing = DM_TRUE;
  pstEv->pfOnClose = pfOnClose;
  pstEv->pvCloseUser = pvUser;
  (void)dm_uv_fs_event_stop(&pstEv->stUv);
  dm_uv_close((dm_uv_handle_st *)&pstEv->stUv, dm__fs_event_on_close);
  return DM_SUCCESS;
}

#ifdef __cplusplus
}
#endif
