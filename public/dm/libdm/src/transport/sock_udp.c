/**
 ******************************************************************************
 * @file    sock_udp.c
 * @author  MDM
 * @version 2.1.0
 * @brief   DM 通用异步 UDP 原语实现（DD-DM-2.1 §2.3，AUD-V21-P1-06 / P1-11）：
 *          支持单播/广播/组播、65507B 上限拦截、0 字节数据报安全分配、线程亲和与 bClosing 守卫。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "sock_internal.h"

#include <string.h>

#define DM__UDP_RECV_BUF_SIZE 65536u

struct dm_udp
{
  dm_uv_udp_st stUv;
  dm_loop_st *pstLoop;
  void *pvData;
  dm_udp_recv_cb pfOnRecv;
  void *pvRecvUser;
  dm_close_fn pfOnClose;
  void *pvCloseUser;
  bool_t bClosing;
};

typedef struct dm__udp_send_req
{
  dm_uv_udp_send_st stReq;
  dm_udp_st *pstUdp;
  dm_udp_send_cb pfCb;
  void *pvUser;
  uint8 au8Data[];
} dm__udp_send_req_st;

/* ============================ 内部守卫与回调 ============================ */

static int dm__udp_guard_mutator(dm_in const dm_udp_st *pstUdp)
{
  int iErr = 0;

  if (NULL == pstUdp)
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstUdp->pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (DM_TRUE == pstUdp->bClosing)
  {
    return DM_SHUTTING_DOWN;
  }
  return DM_SUCCESS;
}

static void dm__udp_on_alloc(dm_io dm_uv_handle_st *pstUv, dm_in size_t uSuggested,
                             dm_out dm_uv_buf_st *pstBuf)
{
  size_t uAlloc = (uSuggested > DM__UDP_RECV_BUF_SIZE) ? DM__UDP_RECV_BUF_SIZE : uSuggested;
  char *pcMem = NULL;

  (void)pstUv;
  if (0u == uAlloc)
  {
    uAlloc = DM__UDP_RECV_BUF_SIZE;
  }
  pcMem = (char *)dm_malloc_tag(uAlloc, DM_MEM_TAG_SOCK);
  *pstBuf = dm_uv_buf_init(pcMem, (NULL != pcMem) ? (unsigned int)uAlloc : 0u);
  return;
}

static void dm__udp_on_recv(dm_io dm_uv_udp_st *pstUv, dm_in ssize_t iNRead,
                            dm_in const dm_uv_buf_st *pstBuf, dm_in const struct sockaddr *pstAddr,
                            dm_in unsigned int uiFlags)
{
  dm_udp_st *pstUdp = (dm_udp_st *)dm_uv_handle_get_data((dm_uv_handle_st *)pstUv);
  char *pcBase = (NULL != pstBuf) ? pstBuf->base : NULL;
  dm_udp_recv_cb pfCb = NULL;
  void *pvUser = NULL;

  if ((NULL == pstUdp) || (DM_TRUE == pstUdp->bClosing) || ((0 == iNRead) && (NULL == pstAddr)))
  {
    if (NULL != pcBase)
    {
      dm_free_tag(pcBase);
    }
    return;
  }
  pfCb = pstUdp->pfOnRecv;
  pvUser = pstUdp->pvRecvUser;
  if (iNRead < 0)
  {
    if (NULL != pcBase)
    {
      dm_free_tag(pcBase);
    }
    if (NULL != pfCb)
    {
      pfCb(pstUdp, dm__uv_err_map((int)iNRead), NULL, pvUser);
    }
    return;
  }
  if (NULL != pfCb)
  {
    dm_udp_rx_st stRx;
    memset(&stRx, 0, sizeof(stRx));
    stRx.pu8Data = (iNRead > 0) ? (const uint8 *)pcBase : NULL;
    stRx.uLen = (size_t)iNRead;
    if (NULL != pstAddr)
    {
      (void)dm__sockaddr_from_os(pstAddr, &stRx.stPeer);
    }
    if (0u != (uiFlags & DM_UV_UDP_PARTIAL))
    {
      stRx.u32Flags |= DM_UDP_RECV_F_PARTIAL;
    }
    pfCb(pstUdp, DM_SUCCESS, &stRx, pvUser);
  }
  if (NULL != pcBase)
  {
    dm_free_tag(pcBase);
  }
  return;
}

static void dm__udp_on_send(dm_io dm_uv_udp_send_st *pstUvReq, dm_in int iStatus)
{
  dm__udp_send_req_st *pstReq = (dm__udp_send_req_st *)dm_uv_req_get_data((dm_uv_req_st *)pstUvReq);
  dm_udp_st *pstUdp = NULL;
  dm_udp_send_cb pfCb = NULL;
  void *pvUser = NULL;

  if (NULL == pstReq)
  {
    return;
  }
  pstUdp = pstReq->pstUdp;
  pfCb = pstReq->pfCb;
  pvUser = pstReq->pvUser;
  dm_free_tag(pstReq);
  if (NULL != pfCb)
  {
    pfCb(pstUdp, dm__uv_err_map(iStatus), pvUser);
  }
  return;
}

static void dm__udp_on_close(dm_io dm_uv_handle_st *pstUv)
{
  dm_udp_st *pstUdp = (dm_udp_st *)dm_uv_handle_get_data(pstUv);
  dm_loop_st *pstLoop = NULL;
  dm_close_fn pfOnClose = NULL;
  void *pvCloseUser = NULL;

  if (NULL == pstUdp)
  {
    return;
  }
  pstLoop = pstUdp->pstLoop;
  pfOnClose = pstUdp->pfOnClose;
  pvCloseUser = pstUdp->pvCloseUser;
  dm__loop_handle_dec(pstLoop);
  dm_free_tag(pstUdp);
  if (NULL != pfOnClose)
  {
    pfOnClose(pvCloseUser);
  }
  return;
}

/* ============================ 公开 UDP API ============================ */

int dm_udp_create(dm_io dm_loop_st *pstLoop, dm_out dm_udp_st **ppstOut)
{
  dm_udp_st *pstUdp = NULL;
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
  pstUdp = (dm_udp_st *)dm_calloc_tag(1u, sizeof(dm_udp_st), DM_MEM_TAG_SOCK);
  if (NULL == pstUdp)
  {
    return DM_ALLOC;
  }
  pstUdp->pstLoop = pstLoop;
  iErr = dm_uv_udp_init(dm__loop_raw(pstLoop), &pstUdp->stUv);
  if (0 != iErr)
  {
    dm_free_tag(pstUdp);
    return dm__uv_err_map(iErr);
  }
  dm_uv_handle_set_data((dm_uv_handle_st *)&pstUdp->stUv, pstUdp);
  dm__loop_handle_inc(pstLoop);
  *ppstOut = pstUdp;
  return DM_SUCCESS;
}

int dm_udp_bind(dm_io dm_udp_st *pstUdp, dm_in const dm_sockaddr_st *pstAddr, dm_in uint32 u32Flags)
{
  struct sockaddr_storage stOs;
  int iLen = 0;
  unsigned int uiUvFlags = 0u;
  int iErr = dm__udp_guard_mutator(pstUdp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if ((NULL == pstAddr) || (0u != (u32Flags & ~(DM_UDP_F_IPV6ONLY | DM_UDP_F_REUSEADDR))))
  {
    return DM_INVALID;
  }
  iErr = dm__sockaddr_to_os(pstAddr, &stOs, &iLen);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (0u != (u32Flags & DM_UDP_F_IPV6ONLY))
  {
    uiUvFlags |= DM_UV_UDP_IPV6ONLY;
  }
  if (0u != (u32Flags & DM_UDP_F_REUSEADDR))
  {
    uiUvFlags |= DM_UV_UDP_REUSEADDR;
  }
  iErr = dm_uv_udp_bind(&pstUdp->stUv, (const struct sockaddr *)&stOs, uiUvFlags);
  return dm__uv_err_map(iErr);
}

int dm_udp_connect(dm_io dm_udp_st *pstUdp, dm_in const dm_sockaddr_st *pstAddr)
{
  struct sockaddr_storage stOs;
  int iLen = 0;
  int iErr = dm__udp_guard_mutator(pstUdp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (NULL == pstAddr)
  {
    iErr = dm_uv_udp_connect(&pstUdp->stUv, NULL);
    return dm__uv_err_map(iErr);
  }
  iErr = dm__sockaddr_to_os(pstAddr, &stOs, &iLen);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr = dm_uv_udp_connect(&pstUdp->stUv, (const struct sockaddr *)&stOs);
  return dm__uv_err_map(iErr);
}

int dm_udp_getsockname(dm_in const dm_udp_st *pstUdp, dm_out dm_sockaddr_st *pstOut)
{
  struct sockaddr_storage stOs;
  int iLen = (int)sizeof(stOs);
  int iErr = dm__udp_guard_mutator(pstUdp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (NULL == pstOut)
  {
    return DM_INVALID;
  }
  memset(&stOs, 0, sizeof(stOs));
  iErr = dm_uv_udp_getsockname(&pstUdp->stUv, (struct sockaddr *)&stOs, &iLen);
  if (0 != iErr)
  {
    return dm__uv_err_map(iErr);
  }
  return dm__sockaddr_from_os((const struct sockaddr *)&stOs, pstOut);
}

int dm_udp_getpeername(dm_in const dm_udp_st *pstUdp, dm_out dm_sockaddr_st *pstOut)
{
  struct sockaddr_storage stOs;
  int iLen = (int)sizeof(stOs);
  int iErr = dm__udp_guard_mutator(pstUdp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (NULL == pstOut)
  {
    return DM_INVALID;
  }
  memset(&stOs, 0, sizeof(stOs));
  iErr = dm_uv_udp_getpeername(&pstUdp->stUv, (struct sockaddr *)&stOs, &iLen);
  if (0 != iErr)
  {
    return dm__uv_err_map(iErr);
  }
  return dm__sockaddr_from_os((const struct sockaddr *)&stOs, pstOut);
}

int dm_udp_set_broadcast(dm_io dm_udp_st *pstUdp, dm_in bool_t bEnable)
{
  int iErr = dm__udp_guard_mutator(pstUdp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr = dm_uv_udp_set_broadcast(&pstUdp->stUv, (DM_TRUE == bEnable) ? 1 : 0);
  return dm__uv_err_map(iErr);
}

int dm_udp_set_ttl(dm_io dm_udp_st *pstUdp, dm_in int iTtl)
{
  int iErr = dm__udp_guard_mutator(pstUdp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if ((iTtl < 1) || (iTtl > 255))
  {
    return DM_INVALID;
  }
  iErr = dm_uv_udp_set_ttl(&pstUdp->stUv, iTtl);
  return dm__uv_err_map(iErr);
}

int dm_udp_set_multicast_loop(dm_io dm_udp_st *pstUdp, dm_in bool_t bEnable)
{
  int iErr = dm__udp_guard_mutator(pstUdp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr = dm_uv_udp_set_multicast_loop(&pstUdp->stUv, (DM_TRUE == bEnable) ? 1 : 0);
  return dm__uv_err_map(iErr);
}

int dm_udp_set_multicast_ttl(dm_io dm_udp_st *pstUdp, dm_in int iTtl)
{
  int iErr = dm__udp_guard_mutator(pstUdp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if ((iTtl < 0) || (iTtl > 255))
  {
    return DM_INVALID;
  }
  iErr = dm_uv_udp_set_multicast_ttl(&pstUdp->stUv, iTtl);
  return dm__uv_err_map(iErr);
}

int dm_udp_set_multicast_interface(dm_io dm_udp_st *pstUdp, dm_in const char *pcIfaceAddr)
{
  int iErr = dm__udp_guard_mutator(pstUdp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if ((NULL == pcIfaceAddr) || ('\0' == pcIfaceAddr[0]))
  {
    return DM_INVALID;
  }
  iErr = dm_uv_udp_set_multicast_interface(&pstUdp->stUv, pcIfaceAddr);
  return dm__uv_err_map(iErr);
}

int dm_udp_set_membership(dm_io dm_udp_st *pstUdp, dm_in const char *pcMcastAddr,
                          dm_in const char *pcIfaceAddr, dm_in dm_mcast_op_et eOp)
{
  dm_uv_membership_et eUvOp = DM_UV_LEAVE_GROUP;
  int iErr = dm__udp_guard_mutator(pstUdp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if ((NULL == pcMcastAddr) || ('\0' == pcMcastAddr[0]))
  {
    return DM_INVALID;
  }
  if (DM_MCAST_JOIN == eOp)
  {
    eUvOp = DM_UV_JOIN_GROUP;
  }
  else if (DM_MCAST_LEAVE == eOp)
  {
    eUvOp = DM_UV_LEAVE_GROUP;
  }
  else
  {
    return DM_INVALID;
  }
  iErr = dm_uv_udp_set_membership(&pstUdp->stUv, pcMcastAddr, pcIfaceAddr, eUvOp);
  return dm__uv_err_map(iErr);
}

int dm_udp_try_send(dm_io dm_udp_st *pstUdp, dm_in const void *pvData, dm_in size_t uLen,
                    dm_in const dm_sockaddr_st *pstAddr)
{
  struct sockaddr_storage stOs;
  const struct sockaddr *pstOsAddr = NULL;
  char cDummy = 0;
  dm_uv_buf_st stBuf;
  int iLen = 0;
  int iErr = dm__udp_guard_mutator(pstUdp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (uLen > DM_UDP_PAYLOAD_MAX)
  {
    return DM_TOO_BIG;
  }
  if ((uLen > 0u) && (NULL == pvData))
  {
    return DM_INVALID;
  }
  if (NULL != pstAddr)
  {
    iErr = dm__sockaddr_to_os(pstAddr, &stOs, &iLen);
    if (DM_SUCCESS != iErr)
    {
      return iErr;
    }
    pstOsAddr = (const struct sockaddr *)&stOs;
  }
  stBuf = dm_uv_buf_init((uLen > 0u) ? (char *)pvData : &cDummy, (unsigned int)uLen);
  iErr = dm_uv_udp_try_send(&pstUdp->stUv, &stBuf, 1u, pstOsAddr);
  if (iErr < 0)
  {
    return dm__uv_err_map(iErr);
  }
  return DM_SUCCESS;
}

int dm_udp_send(dm_io dm_udp_st *pstUdp, dm_in const dm_udp_dgram_st *pstDgram,
                dm_in dm_udp_send_cb pfOnSend, dm_in void *pvUser)
{
  struct sockaddr_storage stOs;
  const struct sockaddr *pstOsAddr = NULL;
  dm__udp_send_req_st *pstReq = NULL;
  dm_uv_buf_st stBuf;
  size_t uQueued = 0u;
  int iLen = 0;
  int iErr = dm__udp_guard_mutator(pstUdp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (NULL == pstDgram)
  {
    return DM_INVALID;
  }
  if (pstDgram->uLen > DM_UDP_PAYLOAD_MAX)
  {
    return DM_TOO_BIG;
  }
  if ((pstDgram->uLen > 0u) && (NULL == pstDgram->pvData))
  {
    return DM_INVALID;
  }
  uQueued = dm_uv_udp_get_send_queue_size(&pstUdp->stUv);
  if (uQueued > (DM_SOCK_TX_QUEUE_MAX - pstDgram->uLen))
  {
    return DM_BUSY;
  }
  if (NULL != pstDgram->pstAddr)
  {
    iErr = dm__sockaddr_to_os(pstDgram->pstAddr, &stOs, &iLen);
    if (DM_SUCCESS != iErr)
    {
      return iErr;
    }
    pstOsAddr = (const struct sockaddr *)&stOs;
  }
  /* 单块分配 sizeof(req) + uLen，uLen == 0 亦绝不触发 dm_malloc(0) */
  pstReq = (dm__udp_send_req_st *)dm_malloc_tag(sizeof(dm__udp_send_req_st) + pstDgram->uLen,
                                                DM_MEM_TAG_SOCK);
  if (NULL == pstReq)
  {
    return DM_ALLOC;
  }
  memset(&pstReq->stReq, 0, sizeof(pstReq->stReq));
  pstReq->pstUdp = pstUdp;
  pstReq->pfCb = pfOnSend;
  pstReq->pvUser = pvUser;
  if (pstDgram->uLen > 0u)
  {
    memcpy(pstReq->au8Data, pstDgram->pvData, pstDgram->uLen);
  }
  stBuf = dm_uv_buf_init((char *)pstReq->au8Data, (unsigned int)pstDgram->uLen);
  dm_uv_req_set_data((dm_uv_req_st *)&pstReq->stReq, pstReq);
  iErr = dm_uv_udp_send(&pstReq->stReq, &pstUdp->stUv, &stBuf, 1u, pstOsAddr, dm__udp_on_send);
  if (0 != iErr)
  {
    dm_free_tag(pstReq);
    return dm__uv_err_map(iErr);
  }
  return DM_SUCCESS;
}

int dm_udp_recv_start(dm_io dm_udp_st *pstUdp, dm_in dm_udp_recv_cb pfOnRecv, dm_in void *pvUser)
{
  int iErr = dm__udp_guard_mutator(pstUdp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (NULL == pfOnRecv)
  {
    return DM_INVALID;
  }
  pstUdp->pfOnRecv = pfOnRecv;
  pstUdp->pvRecvUser = pvUser;
  iErr = dm_uv_udp_recv_start(&pstUdp->stUv, dm__udp_on_alloc, dm__udp_on_recv);
  return dm__uv_err_map(iErr);
}

int dm_udp_recv_stop(dm_io dm_udp_st *pstUdp)
{
  int iErr = dm__udp_guard_mutator(pstUdp);

  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  iErr = dm_uv_udp_recv_stop(&pstUdp->stUv);
  return dm__uv_err_map(iErr);
}

size_t dm_udp_send_queue_size(dm_in const dm_udp_st *pstUdp)
{
  if (NULL == pstUdp)
  {
    return 0u;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstUdp->pstLoop))
  {
    return 0u;
  }
  return dm_uv_udp_get_send_queue_size(&pstUdp->stUv);
}

int dm_udp_close(dm_io dm_udp_st *pstUdp, dm_in dm_close_fn pfOnClose, dm_in void *pvUser)
{
  int iErr = 0;

  if (NULL == pstUdp)
  {
    return DM_INVALID;
  }
  iErr = dm__loop_check_owner(pstUdp->pstLoop);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (DM_TRUE == pstUdp->bClosing)
  {
    return DM_SUCCESS;
  }
  pstUdp->bClosing = DM_TRUE;
  pstUdp->pfOnClose = pfOnClose;
  pstUdp->pvCloseUser = pvUser;
  (void)dm_uv_udp_recv_stop(&pstUdp->stUv);
  dm_uv_close((dm_uv_handle_st *)&pstUdp->stUv, dm__udp_on_close);
  return DM_SUCCESS;
}

void dm_udp_set_data(dm_io dm_udp_st *pstUdp, dm_in void *pvData)
{
  if (NULL == pstUdp)
  {
    return;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstUdp->pstLoop))
  {
    return;
  }
  pstUdp->pvData = pvData;
  return;
}

void *dm_udp_get_data(dm_in const dm_udp_st *pstUdp)
{
  if (NULL == pstUdp)
  {
    return NULL;
  }
  if (DM_SUCCESS != dm__loop_check_owner(pstUdp->pstLoop))
  {
    return NULL;
  }
  return pstUdp->pvData;
}

#ifdef __cplusplus
}
#endif
