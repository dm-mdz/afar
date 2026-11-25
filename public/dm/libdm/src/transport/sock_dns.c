/**
 ******************************************************************************
 * @file    sock_dns.c
 * @author  MDM
 * @version 2.1.0
 * @brief   DM 同步与异步 DNS 解析实现（DD-DM-2.1 §2.3，AUD-V21-P0-02 / R2-P2-01）：
 *          - 同步 IP 字面量零线程零分配快速路径；
 *          - 同步域名解析走独立引用计数分离线程 + uv_getaddrinfo(NULL, ...) + cond_timedwait；
 *          - 异步 DNS 走两段式完成栅栏（bGaiDone && bTimerClosed）+ 单次投递保护（bDelivered）。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "sock_internal.h"
#include <dm/thread.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define DM__DNS_DEFAULT_TIMEOUT_MS 5000u
#define DM__DNS_MAX_RESULTS 64u
#define DM__DNS_HOST_MAX 256u
#define DM__DNS_SVC_MAX 64u

struct dm_dns_req
{
  dm_uv_getaddrinfo_st stGai;
  dm_uv_timer_st stTimer;
  dm_loop_st *pstLoop;
  dm_af_et eFamily;
  dm_dns_cb pfCb;
  void *pvUser;
  bool_t bGaiDone;
  bool_t bTimerClosing;
  bool_t bTimerClosed;
  bool_t bDelivered;
};

typedef struct dm__dns_sync_req
{
  _Atomic uint32 u32Ref;
  char acHost[DM__DNS_HOST_MAX];
  char acService[DM__DNS_SVC_MAX];
  bool_t bHasService;
  dm_af_et eFamily;
  dm_mutex_st *pstMtx;
  dm_cond_st *pstCond;
  bool_t bDone;
  bool_t bTimedOut;
  int iErr;
  dm_sockaddr_st astRes[DM__DNS_MAX_RESULTS];
  uint32 u32ResCount;
} dm__dns_sync_req_st;

/* ============================ 内部辅助 ============================ */

static bool_t dm__dns_valid_family(dm_in dm_af_et eFamily)
{
  return ((DM_AF_UNSPEC == eFamily) || (DM_AF_IPV4 == eFamily) || (DM_AF_IPV6 == eFamily))
             ? DM_TRUE
             : DM_FALSE;
}

static int dm__dns_map_gai_err(dm_in int iUvErr)
{
  int iMapped = dm__uv_err_map(iUvErr);

  if ((DM_SUCCESS == iMapped) || (DM_ALLOC == iMapped) || (DM_CANCELED == iMapped) ||
      (DM_TIMEOUT == iMapped))
  {
    return iMapped;
  }
  return DM_DNS_FAIL;
}

static void dm__dns_fill_hints(dm_in dm_af_et eFamily, dm_out struct addrinfo *pstHints)
{
  memset(pstHints, 0, sizeof(*pstHints));
  pstHints->ai_socktype = SOCK_STREAM;
  if (DM_AF_IPV4 == eFamily)
  {
    pstHints->ai_family = AF_INET;
  }
  else if (DM_AF_IPV6 == eFamily)
  {
    pstHints->ai_family = AF_INET6;
  }
  else
  {
    pstHints->ai_family = AF_UNSPEC;
  }
  return;
}

static bool_t dm__dns_addr_exists(dm_in const dm_sockaddr_st *pastList, dm_in uint32 u32Count,
                                  dm_in const dm_sockaddr_st *pstCand)
{
  uint32 u32Idx = 0u;

  for (u32Idx = 0u; u32Idx < u32Count; u32Idx++)
  {
    if ((pastList[u32Idx].eFamily == pstCand->eFamily) &&
        (pastList[u32Idx].u16Port == pstCand->u16Port) &&
        (pastList[u32Idx].u32ScopeId == pstCand->u32ScopeId) &&
        (0 == memcmp(pastList[u32Idx].au8Addr, pstCand->au8Addr, 16u)))
    {
      return DM_TRUE;
    }
  }
  return DM_FALSE;
}

static uint32 dm__dns_extract_addrs(dm_in const struct addrinfo *pstAi, dm_in dm_af_et eFamily,
                                    dm_out dm_sockaddr_st *pastOut, dm_in uint32 u32Cap)
{
  const struct addrinfo *pstCur = pstAi;
  uint32 u32Count = 0u;

  while ((NULL != pstCur) && (u32Count < u32Cap))
  {
    dm_sockaddr_st stAddr;
    if ((NULL != pstCur->ai_addr) && (DM_SUCCESS == dm__sockaddr_from_os(pstCur->ai_addr, &stAddr)))
    {
      bool_t bFamMatch =
          ((DM_AF_UNSPEC == eFamily) || (eFamily == stAddr.eFamily)) ? DM_TRUE : DM_FALSE;
      if ((DM_TRUE == bFamMatch) && (DM_TRUE != dm__dns_addr_exists(pastOut, u32Count, &stAddr)))
      {
        pastOut[u32Count++] = stAddr;
      }
    }
    pstCur = pstCur->ai_next;
  }
  return u32Count;
}

static bool_t dm__dns_try_literal(dm_in const dm_dns_query_st *pstQuery,
                                  dm_out dm_sockaddr_st *pstOut, dm_out int *piErr)
{
  uint16 u16Port = 0u;
  dm_sockaddr_st stTmp;

  if (DM_TRUE != dm__sockaddr_parse_port_str(pstQuery->pcService, &u16Port))
  {
    return DM_FALSE;
  }
  if (DM_SUCCESS == dm_sockaddr_ip4(pstQuery->pcHost, u16Port, &stTmp))
  {
    if ((DM_AF_UNSPEC == pstQuery->eFamily) || (DM_AF_IPV4 == pstQuery->eFamily))
    {
      *pstOut = stTmp;
      *piErr = DM_SUCCESS;
    }
    else
    {
      *piErr = DM_DNS_FAIL;
    }
    return DM_TRUE;
  }
  if (DM_SUCCESS == dm_sockaddr_ip6(pstQuery->pcHost, u16Port, &stTmp))
  {
    if ((DM_AF_UNSPEC == pstQuery->eFamily) || (DM_AF_IPV6 == pstQuery->eFamily))
    {
      *pstOut = stTmp;
      *piErr = DM_SUCCESS;
    }
    else
    {
      *piErr = DM_DNS_FAIL;
    }
    return DM_TRUE;
  }
  return DM_FALSE;
}

/* ============================ 异步 DNS 两段式栅栏 ============================ */

static void dm__dns_try_free(dm_io dm_dns_req_st *pstReq)
{
  if ((NULL != pstReq) && (DM_TRUE == pstReq->bGaiDone) && (DM_TRUE == pstReq->bTimerClosed))
  {
    dm_free_tag(pstReq);
  }
  return;
}

static void dm__dns_on_timer_close(dm_io dm_uv_handle_st *pstUv)
{
  dm_dns_req_st *pstReq = (dm_dns_req_st *)dm_uv_handle_get_data(pstUv);

  if (NULL == pstReq)
  {
    return;
  }
  pstReq->bTimerClosed = DM_TRUE;
  dm__loop_handle_dec(pstReq->pstLoop);
  dm__dns_try_free(pstReq);
  return;
}

static void dm__dns_close_timer(dm_io dm_dns_req_st *pstReq)
{
  if ((NULL == pstReq) || (DM_TRUE == pstReq->bTimerClosing))
  {
    return;
  }
  pstReq->bTimerClosing = DM_TRUE;
  (void)dm_uv_timer_stop(&pstReq->stTimer);
  dm_uv_close((dm_uv_handle_st *)&pstReq->stTimer, dm__dns_on_timer_close);
  return;
}

static void dm__dns_deliver(dm_io dm_dns_req_st *pstReq, dm_in int iErr,
                            dm_in const dm_sockaddr_st *pastAddrs, dm_in uint32 u32Count)
{
  dm_dns_cb pfCb = NULL;
  void *pvUser = NULL;

  if ((NULL == pstReq) || (DM_TRUE == pstReq->bDelivered))
  {
    return;
  }
  pstReq->bDelivered = DM_TRUE;
  dm__dns_close_timer(pstReq);
  pfCb = pstReq->pfCb;
  pvUser = pstReq->pvUser;
  if (NULL != pfCb)
  {
    pfCb(iErr, pastAddrs, u32Count, pvUser);
  }
  return;
}

static void dm__dns_on_gai(dm_io dm_uv_getaddrinfo_st *pstUvGai, dm_in int iStatus,
                           dm_io struct addrinfo *pstRes)
{
  dm_dns_req_st *pstReq = (dm_dns_req_st *)dm_uv_req_get_data((dm_uv_req_st *)pstUvGai);
  dm_sockaddr_st astAddrs[DM__DNS_MAX_RESULTS];
  uint32 u32Count = 0u;

  if (NULL == pstReq)
  {
    if (NULL != pstRes)
    {
      dm_uv_freeaddrinfo(pstRes);
    }
    return;
  }
  pstReq->bGaiDone = DM_TRUE;
  if (DM_TRUE != pstReq->bDelivered)
  {
    if ((0 == iStatus) && (NULL != pstRes))
    {
      u32Count = dm__dns_extract_addrs(pstRes, pstReq->eFamily, astAddrs, DM__DNS_MAX_RESULTS);
      if (u32Count > 0u)
      {
        dm__dns_deliver(pstReq, DM_SUCCESS, astAddrs, u32Count);
      }
      else
      {
        dm__dns_deliver(pstReq, DM_DNS_FAIL, NULL, 0u);
      }
    }
    else
    {
      dm__dns_deliver(pstReq, dm__dns_map_gai_err(iStatus), NULL, 0u);
    }
  }
  if (NULL != pstRes)
  {
    dm_uv_freeaddrinfo(pstRes);
  }
  dm__dns_try_free(pstReq);
  return;
}

static void dm__dns_on_timeout(dm_io dm_uv_timer_st *pstUvTimer)
{
  dm_dns_req_st *pstReq = (dm_dns_req_st *)dm_uv_handle_get_data((dm_uv_handle_st *)pstUvTimer);

  if (NULL == pstReq)
  {
    return;
  }
  if (DM_TRUE != pstReq->bDelivered)
  {
    (void)dm_uv_cancel((dm_uv_req_st *)&pstReq->stGai);
    dm__dns_deliver(pstReq, DM_TIMEOUT, NULL, 0u);
  }
  dm__dns_try_free(pstReq);
  return;
}

int dm_dns_resolve_async(dm_io dm_loop_st *pstLoop, dm_in const dm_dns_query_st *pstQuery,
                         dm_in dm_dns_cb pfCb, dm_in void *pvUser, dm_out dm_dns_req_st **ppstReq)
{
  dm_dns_req_st *pstReq = NULL;
  struct addrinfo stHints;
  const char *pcSvc = NULL;
  uint64 u64TimeoutMs = DM__DNS_DEFAULT_TIMEOUT_MS;
  int iErr = 0;

  if (NULL != ppstReq)
  {
    *ppstReq = NULL;
  }
  if ((NULL == pstLoop) || (NULL == pstQuery) || (NULL == pstQuery->pcHost) ||
      ('\0' == pstQuery->pcHost[0]) || (NULL == pfCb) ||
      (DM_TRUE != dm__dns_valid_family(pstQuery->eFamily)))
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  pstReq = (dm_dns_req_st *)dm_calloc_tag(1u, sizeof(dm_dns_req_st), DM_MEM_TAG_SOCK);
  if (NULL == pstReq)
  {
    return DM_ALLOC;
  }
  pstReq->pstLoop = pstLoop;
  pstReq->eFamily = pstQuery->eFamily;
  pstReq->pfCb = pfCb;
  pstReq->pvUser = pvUser;
  if ((NULL != pstQuery->pcService) && ('\0' != pstQuery->pcService[0]))
  {
    pcSvc = pstQuery->pcService;
  }
  dm__dns_fill_hints(pstQuery->eFamily, &stHints);
  dm_uv_req_set_data((dm_uv_req_st *)&pstReq->stGai, pstReq);
  iErr = dm_uv_getaddrinfo(dm__loop_raw(pstLoop), &pstReq->stGai, dm__dns_on_gai, pstQuery->pcHost,
                           pcSvc, &stHints);
  if (0 != iErr)
  {
    dm_free_tag(pstReq);
    return dm__uv_err_map(iErr);
  }
  (void)dm_uv_timer_init(dm__loop_raw(pstLoop), &pstReq->stTimer);
  dm_uv_handle_set_data((dm_uv_handle_st *)&pstReq->stTimer, pstReq);
  dm__loop_handle_inc(pstLoop);
  if (0u != pstQuery->u32TimeoutMs)
  {
    u64TimeoutMs = (uint64)pstQuery->u32TimeoutMs;
  }
  (void)dm_uv_timer_start(&pstReq->stTimer, dm__dns_on_timeout, u64TimeoutMs, 0u);
  if (NULL != ppstReq)
  {
    *ppstReq = pstReq;
  }
  return DM_SUCCESS;
}

int dm_dns_cancel(dm_io dm_dns_req_st *pstReq)
{
  int iErr = 0;

  if (NULL == pstReq)
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstReq->pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (DM_TRUE == pstReq->bDelivered)
  {
    return DM_SUCCESS;
  }
  (void)dm_uv_cancel((dm_uv_req_st *)&pstReq->stGai);
  dm__dns_deliver(pstReq, DM_CANCELED, NULL, 0u);
  dm__dns_try_free(pstReq);
  return DM_SUCCESS;
}

/* ============================ 同步 DNS（零悬空循环） ============================ */

static void dm__dns_sync_release(dm_io dm__dns_sync_req_st *pstSync)
{
  if (NULL == pstSync)
  {
    return;
  }
  if (1u == atomic_fetch_sub_explicit(&pstSync->u32Ref, 1u, memory_order_acq_rel))
  {
    dm_cond_destroy(pstSync->pstCond);
    dm_mutex_destroy(pstSync->pstMtx);
    dm_free_tag(pstSync);
  }
  return;
}

static void dm__dns_sync_worker(dm_io void *pvArg)
{
  dm__dns_sync_req_st *pstSync = (dm__dns_sync_req_st *)pvArg;
  dm_uv_getaddrinfo_st stGai;
  struct addrinfo stHints;
  const char *pcSvc = NULL;
  int iUvErr = 0;
  int iDmErr = DM_SUCCESS;
  uint32 u32Count = 0u;

  memset(&stGai, 0, sizeof(stGai));
  dm__dns_fill_hints(pstSync->eFamily, &stHints);
  if (DM_TRUE == pstSync->bHasService)
  {
    pcSvc = pstSync->acService;
  }
  /* AUD-V21-R2-P2-01：同步形态 uv_getaddrinfo(NULL, &stGai, NULL, ...)，零临时 uv_loop_t */
  iUvErr = dm_uv_getaddrinfo(NULL, &stGai, NULL, pstSync->acHost, pcSvc, &stHints);
  if ((0 == iUvErr) && (NULL != stGai.addrinfo))
  {
    u32Count = dm__dns_extract_addrs(stGai.addrinfo, pstSync->eFamily, pstSync->astRes,
                                     DM__DNS_MAX_RESULTS);
    iDmErr = (u32Count > 0u) ? DM_SUCCESS : DM_DNS_FAIL;
  }
  else
  {
    iDmErr = (0 != iUvErr) ? dm__dns_map_gai_err(iUvErr) : DM_DNS_FAIL;
  }
  if (NULL != stGai.addrinfo)
  {
    dm_uv_freeaddrinfo(stGai.addrinfo);
  }
  dm_mutex_lock(pstSync->pstMtx);
  pstSync->iErr = iDmErr;
  pstSync->u32ResCount = u32Count;
  pstSync->bDone = DM_TRUE;
  dm_cond_signal(pstSync->pstCond);
  dm_mutex_unlock(pstSync->pstMtx);
  dm__dns_sync_release(pstSync);
  return;
}

static int dm__dns_sync_alloc(dm_in const dm_dns_query_st *pstQuery,
                              dm_out dm__dns_sync_req_st **ppstSync)
{
  dm__dns_sync_req_st *pstSync = NULL;
  size_t uHostLen = strlen(pstQuery->pcHost);
  size_t uSvcLen = 0u;
  int iErr = 0;

  if (uHostLen >= DM__DNS_HOST_MAX)
  {
    return DM_TOO_BIG;
  }
  if ((NULL != pstQuery->pcService) && ('\0' != pstQuery->pcService[0]))
  {
    uSvcLen = strlen(pstQuery->pcService);
    if (uSvcLen >= DM__DNS_SVC_MAX)
    {
      return DM_TOO_BIG;
    }
  }
  pstSync = (dm__dns_sync_req_st *)dm_calloc_tag(1u, sizeof(dm__dns_sync_req_st), DM_MEM_TAG_SOCK);
  if (NULL == pstSync)
  {
    return DM_ALLOC;
  }
  iErr = dm_mutex_create(&pstSync->pstMtx);
  if (DM_SUCCESS != iErr)
  {
    dm_free_tag(pstSync);
    return iErr;
  }
  iErr = dm_cond_create(&pstSync->pstCond);
  if (DM_SUCCESS != iErr)
  {
    dm_mutex_destroy(pstSync->pstMtx);
    dm_free_tag(pstSync);
    return iErr;
  }
  memcpy(pstSync->acHost, pstQuery->pcHost, uHostLen + 1u);
  if (uSvcLen > 0u)
  {
    memcpy(pstSync->acService, pstQuery->pcService, uSvcLen + 1u);
    pstSync->bHasService = DM_TRUE;
  }
  pstSync->eFamily = pstQuery->eFamily;
  atomic_init(&pstSync->u32Ref, 2u);
  *ppstSync = pstSync;
  return DM_SUCCESS;
}

int dm_dns_resolve_sync(dm_in const dm_dns_query_st *pstQuery, dm_out dm_sockaddr_st *pastOut,
                        dm_in uint32 u32Cap, dm_out uint32 *pu32Count)
{
  dm__dns_sync_req_st *pstSync = NULL;
  dm_thread_st *pstThr = NULL;
  dm_sockaddr_st stLit;
  uint64 u64TimeoutMs = DM__DNS_DEFAULT_TIMEOUT_MS;
  uint64 u64StartMs = 0u;
  int iErr = 0;

  if (NULL != pu32Count)
  {
    *pu32Count = 0u;
  }
  if ((NULL == pstQuery) || (NULL == pstQuery->pcHost) || ('\0' == pstQuery->pcHost[0]) ||
      (NULL == pastOut) || (0u == u32Cap) || (NULL == pu32Count) ||
      (DM_TRUE != dm__dns_valid_family(pstQuery->eFamily)))
  {
    return DM_INVALID;
  }
  /* 快速路径：IP 字面量 + 空/纯数字端口，零线程零堆分配 */
  if (DM_TRUE == dm__dns_try_literal(pstQuery, &stLit, &iErr))
  {
    if (DM_SUCCESS == iErr)
    {
      pastOut[0] = stLit;
      *pu32Count = 1u;
    }
    return iErr;
  }
  iErr = dm__dns_sync_alloc(pstQuery, &pstSync);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr = dm_thread_create(dm__dns_sync_worker, pstSync, &pstThr);
  if (DM_SUCCESS != iErr)
  {
    dm_cond_destroy(pstSync->pstCond);
    dm_mutex_destroy(pstSync->pstMtx);
    dm_free_tag(pstSync);
    return iErr;
  }
  (void)dm_thread_detach(pstThr);

  if (0u != pstQuery->u32TimeoutMs)
  {
    u64TimeoutMs = (uint64)pstQuery->u32TimeoutMs;
  }
  u64StartMs = dm_hrtime() / 1000000ull;
  dm_mutex_lock(pstSync->pstMtx);
  while (DM_TRUE != pstSync->bDone)
  {
    uint64 u64Elapsed = (dm_hrtime() / 1000000ull) - u64StartMs;
    if (u64Elapsed >= u64TimeoutMs)
    {
      break;
    }
    if (DM_TIMEOUT ==
        dm_cond_timedwait_ms(pstSync->pstCond, pstSync->pstMtx, u64TimeoutMs - u64Elapsed))
    {
      break;
    }
  }
  if (DM_TRUE != pstSync->bDone)
  {
    pstSync->bTimedOut = DM_TRUE;
    iErr = DM_TIMEOUT;
  }
  else
  {
    iErr = pstSync->iErr;
    if (DM_SUCCESS == iErr)
    {
      uint32 u32Copy = (pstSync->u32ResCount < u32Cap) ? pstSync->u32ResCount : u32Cap;
      memcpy(pastOut, pstSync->astRes, (size_t)u32Copy * sizeof(dm_sockaddr_st));
      *pu32Count = u32Copy;
    }
  }
  dm_mutex_unlock(pstSync->pstMtx);
  dm__dns_sync_release(pstSync);
  return iErr;
}

#ifdef __cplusplus
}
#endif
