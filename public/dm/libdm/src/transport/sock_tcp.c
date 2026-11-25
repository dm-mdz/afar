/**
 ******************************************************************************
 * @file    sock_tcp.c
 * @author  MDM
 * @version 2.1.0
 * @brief   DM 通用异步 TCP 原语实现（DD-DM-2.1 §2.3，AUD-V21-P1-06 / P1-11）：
 *          绑定显式 dm_loop_st*、线程亲和守卫、bClosing 守卫、16MiB 背压熔断与重入关闭安全。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "sock_internal.h"

#include <string.h>

#define DM__TCP_READ_BUF_MAX 65536u
#define DM__TCP_READ_BUF_DEF 4096u

struct dm_tcp
{
  dm_uv_tcp_st stUv;
  dm_loop_st *pstLoop;
  void *pvData;
  dm_tcp_accept_cb pfOnAccept;
  void *pvAcceptUser;
  dm_tcp_read_cb pfOnRead;
  void *pvReadUser;
  dm_close_fn pfOnClose;
  void *pvCloseUser;
  bool_t bClosing;
};

typedef struct dm__tcp_connect_req
{
  dm_uv_connect_st stReq;
  dm_tcp_st *pstTcp;
  dm_tcp_connect_cb pfCb;
  void *pvUser;
} dm__tcp_connect_req_st;

typedef struct dm__tcp_write_req
{
  dm_uv_write_st stReq;
  dm_tcp_st *pstTcp;
  dm_tcp_write_cb pfCb;
  void *pvUser;
  uint8 au8Data[];
} dm__tcp_write_req_st;

typedef struct dm__tcp_shutdown_req
{
  dm_uv_shutdown_st stReq;
  dm_tcp_st *pstTcp;
  dm_tcp_shutdown_cb pfCb;
  void *pvUser;
} dm__tcp_shutdown_req_st;

/* ============================ 内部守卫与回调 ============================ */

static int dm__tcp_guard_mutator(dm_in const dm_tcp_st *pstTcp)
{
  int iErr = 0;

  if (NULL == pstTcp)
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstTcp->pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (DM_TRUE == pstTcp->bClosing)
  {
    return DM_SHUTTING_DOWN;
  }
  return DM_SUCCESS;
}

static void dm__tcp_on_conn(dm_io dm_uv_stream_st *pstStream, dm_in int iStatus)
{
  dm_tcp_st *pstTcp = (dm_tcp_st *)dm_uv_handle_get_data((dm_uv_handle_st *)pstStream);

  if ((NULL == pstTcp) || (DM_TRUE == pstTcp->bClosing) || (NULL == pstTcp->pfOnAccept))
  {
    return;
  }
  pstTcp->pfOnAccept(pstTcp, dm__uv_err_map(iStatus), pstTcp->pvAcceptUser);
  return;
}

static void dm__tcp_on_connect(dm_io dm_uv_connect_st *pstUvReq, dm_in int iStatus)
{
  dm__tcp_connect_req_st *pstReq =
      (dm__tcp_connect_req_st *)dm_uv_req_get_data((dm_uv_req_st *)pstUvReq);
  dm_tcp_st *pstTcp = NULL;
  dm_tcp_connect_cb pfCb = NULL;
  void *pvUser = NULL;

  if (NULL == pstReq)
  {
    return;
  }
  pstTcp = pstReq->pstTcp;
  pfCb = pstReq->pfCb;
  pvUser = pstReq->pvUser;
  dm_free_tag(pstReq);
  if (NULL != pfCb)
  {
    pfCb(pstTcp, dm__uv_err_map(iStatus), pvUser);
  }
  return;
}

static void dm__tcp_on_alloc(dm_io dm_uv_handle_st *pstUv, dm_in size_t uSuggested,
                             dm_out dm_uv_buf_st *pstBuf)
{
  size_t uAlloc = uSuggested;
  char *pcMem = NULL;

  (void)pstUv;
  if (0u == uAlloc)
  {
    uAlloc = DM__TCP_READ_BUF_DEF;
  }
  else if (uAlloc > DM__TCP_READ_BUF_MAX)
  {
    uAlloc = DM__TCP_READ_BUF_MAX;
  }
  pcMem = (char *)dm_malloc_tag(uAlloc, DM_MEM_TAG_SOCK);
  *pstBuf = dm_uv_buf_init(pcMem, (NULL != pcMem) ? (unsigned int)uAlloc : 0u);
  return;
}

static void dm__tcp_on_read(dm_io dm_uv_stream_st *pstStream, dm_in ssize_t iNRead,
                            dm_in const dm_uv_buf_st *pstBuf)
{
  dm_tcp_st *pstTcp = (dm_tcp_st *)dm_uv_handle_get_data((dm_uv_handle_st *)pstStream);
  char *pcBase = (NULL != pstBuf) ? pstBuf->base : NULL;

  if ((NULL == pstTcp) || (DM_TRUE == pstTcp->bClosing))
  {
    if (NULL != pcBase)
    {
      dm_free_tag(pcBase);
    }
    return;
  }
  if (iNRead > 0)
  {
    dm_tcp_read_cb pfCb = pstTcp->pfOnRead;
    void *pvUser = pstTcp->pvReadUser;
    if (NULL != pfCb)
    {
      pfCb(pstTcp, DM_SUCCESS, (const uint8 *)pcBase, (size_t)iNRead, pvUser);
    }
    if (NULL != pcBase)
    {
      dm_free_tag(pcBase);
    }
    return;
  }
  if (NULL != pcBase)
  {
    dm_free_tag(pcBase);
  }
  if (iNRead < 0)
  {
    dm_tcp_read_cb pfCb = pstTcp->pfOnRead;
    void *pvUser = pstTcp->pvReadUser;
    (void)dm_uv_read_stop(pstStream);
    if (NULL != pfCb)
    {
      pfCb(pstTcp, dm__uv_err_map((int)iNRead), NULL, 0u, pvUser);
    }
  }
  return;
}

static void dm__tcp_on_write(dm_io dm_uv_write_st *pstUvReq, dm_in int iStatus)
{
  dm__tcp_write_req_st *pstReq =
      (dm__tcp_write_req_st *)dm_uv_req_get_data((dm_uv_req_st *)pstUvReq);
  dm_tcp_st *pstTcp = NULL;
  dm_tcp_write_cb pfCb = NULL;
  void *pvUser = NULL;

  if (NULL == pstReq)
  {
    return;
  }
  pstTcp = pstReq->pstTcp;
  pfCb = pstReq->pfCb;
  pvUser = pstReq->pvUser;
  dm_free_tag(pstReq);
  if (NULL != pfCb)
  {
    pfCb(pstTcp, dm__uv_err_map(iStatus), pvUser);
  }
  return;
}

static void dm__tcp_on_shutdown(dm_io dm_uv_shutdown_st *pstUvReq, dm_in int iStatus)
{
  dm__tcp_shutdown_req_st *pstReq =
      (dm__tcp_shutdown_req_st *)dm_uv_req_get_data((dm_uv_req_st *)pstUvReq);
  dm_tcp_st *pstTcp = NULL;
  dm_tcp_shutdown_cb pfCb = NULL;
  void *pvUser = NULL;

  if (NULL == pstReq)
  {
    return;
  }
  pstTcp = pstReq->pstTcp;
  pfCb = pstReq->pfCb;
  pvUser = pstReq->pvUser;
  dm_free_tag(pstReq);
  if (NULL != pfCb)
  {
    pfCb(pstTcp, dm__uv_err_map(iStatus), pvUser);
  }
  return;
}

static void dm__tcp_on_close(dm_io dm_uv_handle_st *pstUv)
{
  dm_tcp_st *pstTcp = (dm_tcp_st *)dm_uv_handle_get_data(pstUv);
  dm_loop_st *pstLoop = NULL;
  dm_close_fn pfOnClose = NULL;
  void *pvCloseUser = NULL;

  if (NULL == pstTcp)
  {
    return;
  }
  pstLoop = pstTcp->pstLoop;
  pfOnClose = pstTcp->pfOnClose;
  pvCloseUser = pstTcp->pvCloseUser;
  dm__loop_handle_dec(pstLoop);
  dm_free_tag(pstTcp);
  if (NULL != pfOnClose)
  {
    pfOnClose(pvCloseUser);
  }
  return;
}

/* ============================ 公开 TCP API ============================ */

int dm_tcp_create(dm_io dm_loop_st *pstLoop, dm_out dm_tcp_st **ppstOut)
{
  dm_tcp_st *pstTcp = NULL;
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
  pstTcp = (dm_tcp_st *)dm_calloc_tag(1u, sizeof(dm_tcp_st), DM_MEM_TAG_SOCK);
  if (NULL == pstTcp)
  {
    return DM_ALLOC;
  }
  pstTcp->pstLoop = pstLoop;
  iErr = dm_uv_tcp_init(dm__loop_raw(pstLoop), &pstTcp->stUv);
  if (0 != iErr)
  {
    dm_free_tag(pstTcp);
    return dm__uv_err_map(iErr);
  }
  dm_uv_handle_set_data((dm_uv_handle_st *)&pstTcp->stUv, pstTcp);
  dm__loop_handle_inc(pstLoop);
  *ppstOut = pstTcp;
  return DM_SUCCESS;
}

int dm_tcp_bind(dm_io dm_tcp_st *pstTcp, dm_in const dm_sockaddr_st *pstAddr, dm_in uint32 u32Flags)
{
  struct sockaddr_storage stOs;
  int iLen = 0;
  unsigned int uiUvFlags = 0u;
  int iErr = 0;

  iErr = dm__tcp_guard_mutator(pstTcp);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if ((NULL == pstAddr) || (0u != (u32Flags & ~DM_TCP_F_IPV6ONLY)))
  {
    return DM_INVALID;
  }
  iErr = dm__sockaddr_to_os(pstAddr, &stOs, &iLen);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (0u != (u32Flags & DM_TCP_F_IPV6ONLY))
  {
    uiUvFlags = DM_UV_TCP_IPV6ONLY;
  }
  iErr = dm_uv_tcp_bind(&pstTcp->stUv, (const struct sockaddr *)&stOs, uiUvFlags);
  return dm__uv_err_map(iErr);
}

int dm_tcp_listen(dm_io dm_tcp_st *pstTcp, dm_in int iBacklog, dm_in dm_tcp_accept_cb pfOnConn,
                  dm_in void *pvUser)
{
  int iErr = 0;

  iErr = dm__tcp_guard_mutator(pstTcp);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if ((NULL == pfOnConn) || (iBacklog < 0))
  {
    return DM_INVALID;
  }
  if (0 == iBacklog)
  {
    iBacklog = 128;
  }
  pstTcp->pfOnAccept = pfOnConn;
  pstTcp->pvAcceptUser = pvUser;
  iErr = dm_uv_listen((dm_uv_stream_st *)&pstTcp->stUv, iBacklog, dm__tcp_on_conn);
  return dm__uv_err_map(iErr);
}

int dm_tcp_accept(dm_io dm_tcp_st *pstServer, dm_out dm_tcp_st **ppstClient)
{
  dm_tcp_st *pstCli = NULL;
  int iErr = 0;

  if (NULL == ppstClient)
  {
    return DM_INVALID;
  }
  *ppstClient = NULL;
  iErr = dm__tcp_guard_mutator(pstServer);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr = dm_tcp_create(pstServer->pstLoop, &pstCli);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr = dm_uv_accept((dm_uv_stream_st *)&pstServer->stUv, (dm_uv_stream_st *)&pstCli->stUv);
  if (0 != iErr)
  {
    (void)dm_tcp_close(pstCli, NULL, NULL);
    return dm__uv_err_map(iErr);
  }
  *ppstClient = pstCli;
  return DM_SUCCESS;
}

int dm_tcp_connect(dm_io dm_tcp_st *pstTcp, dm_in const dm_sockaddr_st *pstAddr,
                   dm_in dm_tcp_connect_cb pfOnConn, dm_in void *pvUser)
{
  struct sockaddr_storage stOs;
  int iLen = 0;
  dm__tcp_connect_req_st *pstReq = NULL;
  int iErr = 0;

  iErr = dm__tcp_guard_mutator(pstTcp);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (NULL == pstAddr)
  {
    return DM_INVALID;
  }
  iErr = dm__sockaddr_to_os(pstAddr, &stOs, &iLen);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  pstReq =
      (dm__tcp_connect_req_st *)dm_calloc_tag(1u, sizeof(dm__tcp_connect_req_st), DM_MEM_TAG_SOCK);
  if (NULL == pstReq)
  {
    return DM_ALLOC;
  }
  pstReq->pstTcp = pstTcp;
  pstReq->pfCb = pfOnConn;
  pstReq->pvUser = pvUser;
  dm_uv_req_set_data((dm_uv_req_st *)&pstReq->stReq, pstReq);
  iErr = dm_uv_tcp_connect(&pstReq->stReq, &pstTcp->stUv, (const struct sockaddr *)&stOs,
                           dm__tcp_on_connect);
  if (0 != iErr)
  {
    dm_free_tag(pstReq);
    return dm__uv_err_map(iErr);
  }
  return DM_SUCCESS;
}

int dm_tcp_nodelay(dm_io dm_tcp_st *pstTcp, dm_in bool_t bEnable)
{
  int iErr = dm__tcp_guard_mutator(pstTcp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr = dm_uv_tcp_nodelay(&pstTcp->stUv, (DM_TRUE == bEnable) ? 1 : 0);
  return dm__uv_err_map(iErr);
}

int dm_tcp_keepalive(dm_io dm_tcp_st *pstTcp, dm_in bool_t bEnable, dm_in uint32 u32DelaySec)
{
  int iErr = dm__tcp_guard_mutator(pstTcp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr =
      dm_uv_tcp_keepalive(&pstTcp->stUv, (DM_TRUE == bEnable) ? 1 : 0, (unsigned int)u32DelaySec);
  return dm__uv_err_map(iErr);
}

int dm_tcp_getsockname(dm_in const dm_tcp_st *pstTcp, dm_out dm_sockaddr_st *pstOut)
{
  struct sockaddr_storage stOs;
  int iLen = (int)sizeof(stOs);
  int iErr = dm__tcp_guard_mutator(pstTcp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (NULL == pstOut)
  {
    return DM_INVALID;
  }
  memset(&stOs, 0, sizeof(stOs));
  iErr = dm_uv_tcp_getsockname(&pstTcp->stUv, (struct sockaddr *)&stOs, &iLen);
  if (0 != iErr)
  {
    return dm__uv_err_map(iErr);
  }
  return dm__sockaddr_from_os((const struct sockaddr *)&stOs, pstOut);
}

int dm_tcp_getpeername(dm_in const dm_tcp_st *pstTcp, dm_out dm_sockaddr_st *pstOut)
{
  struct sockaddr_storage stOs;
  int iLen = (int)sizeof(stOs);
  int iErr = dm__tcp_guard_mutator(pstTcp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (NULL == pstOut)
  {
    return DM_INVALID;
  }
  memset(&stOs, 0, sizeof(stOs));
  iErr = dm_uv_tcp_getpeername(&pstTcp->stUv, (struct sockaddr *)&stOs, &iLen);
  if (0 != iErr)
  {
    return dm__uv_err_map(iErr);
  }
  return dm__sockaddr_from_os((const struct sockaddr *)&stOs, pstOut);
}

int dm_tcp_read_start(dm_io dm_tcp_st *pstTcp, dm_in dm_tcp_read_cb pfOnRead, dm_in void *pvUser)
{
  int iErr = dm__tcp_guard_mutator(pstTcp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (NULL == pfOnRead)
  {
    return DM_INVALID;
  }
  pstTcp->pfOnRead = pfOnRead;
  pstTcp->pvReadUser = pvUser;
  iErr = dm_uv_read_start((dm_uv_stream_st *)&pstTcp->stUv, dm__tcp_on_alloc, dm__tcp_on_read);
  return dm__uv_err_map(iErr);
}

int dm_tcp_read_stop(dm_io dm_tcp_st *pstTcp)
{
  int iErr = dm__tcp_guard_mutator(pstTcp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr = dm_uv_read_stop((dm_uv_stream_st *)&pstTcp->stUv);
  return dm__uv_err_map(iErr);
}

int dm_tcp_write(dm_io dm_tcp_st *pstTcp, dm_in const void *pvData, dm_in size_t uLen,
                 dm_in dm_tcp_write_cb pfOnWrite, dm_in void *pvUser)
{
  dm__tcp_write_req_st *pstReq = NULL;
  dm_uv_buf_st stBuf;
  size_t uQueued = 0u;
  int iErr = 0;

  if ((NULL == pstTcp) || (NULL == pvData) || (0u == uLen))
  {
    return DM_INVALID;
  }
  iErr = dm__tcp_guard_mutator(pstTcp);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (uLen > DM_SOCK_TX_QUEUE_MAX)
  {
    return DM_TOO_BIG;
  }
  uQueued = dm_uv_stream_get_write_queue_size((const dm_uv_stream_st *)&pstTcp->stUv);
  if (uQueued > (DM_SOCK_TX_QUEUE_MAX - uLen))
  {
    return DM_BUSY;
  }
  pstReq =
      (dm__tcp_write_req_st *)dm_malloc_tag(sizeof(dm__tcp_write_req_st) + uLen, DM_MEM_TAG_SOCK);
  if (NULL == pstReq)
  {
    return DM_ALLOC;
  }
  memset(&pstReq->stReq, 0, sizeof(pstReq->stReq));
  pstReq->pstTcp = pstTcp;
  pstReq->pfCb = pfOnWrite;
  pstReq->pvUser = pvUser;
  memcpy(pstReq->au8Data, pvData, uLen);
  stBuf = dm_uv_buf_init((char *)pstReq->au8Data, (unsigned int)uLen);
  dm_uv_req_set_data((dm_uv_req_st *)&pstReq->stReq, pstReq);
  iErr =
      dm_uv_write(&pstReq->stReq, (dm_uv_stream_st *)&pstTcp->stUv, &stBuf, 1u, dm__tcp_on_write);
  if (0 != iErr)
  {
    dm_free_tag(pstReq);
    return dm__uv_err_map(iErr);
  }
  return DM_SUCCESS;
}

size_t dm_tcp_write_queue_size(dm_in const dm_tcp_st *pstTcp)
{
  if (NULL == pstTcp)
  {
    return 0u;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstTcp->pstLoop))
  {
    return 0u;
  }
  return dm_uv_stream_get_write_queue_size((const dm_uv_stream_st *)&pstTcp->stUv);
}

int dm_tcp_shutdown(dm_io dm_tcp_st *pstTcp, dm_in dm_tcp_shutdown_cb pfOnDone, dm_in void *pvUser)
{
  dm__tcp_shutdown_req_st *pstReq = NULL;
  int iErr = dm__tcp_guard_mutator(pstTcp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  pstReq = (dm__tcp_shutdown_req_st *)dm_calloc_tag(1u, sizeof(dm__tcp_shutdown_req_st),
                                                    DM_MEM_TAG_SOCK);
  if (NULL == pstReq)
  {
    return DM_ALLOC;
  }
  pstReq->pstTcp = pstTcp;
  pstReq->pfCb = pfOnDone;
  pstReq->pvUser = pvUser;
  dm_uv_req_set_data((dm_uv_req_st *)&pstReq->stReq, pstReq);
  iErr = dm_uv_shutdown(&pstReq->stReq, (dm_uv_stream_st *)&pstTcp->stUv, dm__tcp_on_shutdown);
  if (0 != iErr)
  {
    dm_free_tag(pstReq);
    return dm__uv_err_map(iErr);
  }
  return DM_SUCCESS;
}

int dm_tcp_close(dm_io dm_tcp_st *pstTcp, dm_in dm_close_fn pfOnClose, dm_in void *pvUser)
{
  int iErr = 0;

  if (NULL == pstTcp)
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstTcp->pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (DM_TRUE == pstTcp->bClosing)
  {
    return DM_SUCCESS;
  }
  pstTcp->bClosing = DM_TRUE;
  pstTcp->pfOnClose = pfOnClose;
  pstTcp->pvCloseUser = pvUser;
  (void)dm_uv_read_stop((dm_uv_stream_st *)&pstTcp->stUv);
  dm_uv_close((dm_uv_handle_st *)&pstTcp->stUv, dm__tcp_on_close);
  return DM_SUCCESS;
}

void dm_tcp_set_data(dm_io dm_tcp_st *pstTcp, dm_in void *pvData)
{
  if (NULL == pstTcp)
  {
    return;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstTcp->pstLoop))
  {
    return;
  }
  pstTcp->pvData = pvData;
  return;
}

void *dm_tcp_get_data(dm_in const dm_tcp_st *pstTcp)
{
  if (NULL == pstTcp)
  {
    return NULL;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstTcp->pstLoop))
  {
    return NULL;
  }
  return pstTcp->pvData;
}

#ifdef __cplusplus
}
#endif
