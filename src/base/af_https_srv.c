/**
 ******************************************************************************
 * @file    af_https_srv.c
 * @author  Afar_C
 * @version 2.0.0
 * @brief   独立线程 T_https 服务管理：线程/事件循环、64 槽位表、跨线程桥接与生命周期钩子。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "base/af_https_int.h"

#include <psa/crypto.h>

#include <stdio.h>
#include <string.h>

typedef struct af__https_route
{
  char acMethod[12];
  char acPath[AF_HTTPS_PATH_MAX];
  af_https_handler_fn pfHandler;
} af__https_route_st;

typedef struct af__https_boot
{
  dm_mutex_st *pstMtx;
  dm_cond_st *pstCond;
  bool_t bDone;
  int iErr;
} af__https_boot_st;

typedef struct af__https_batch
{
  af__https_reap_st astReap[AF_HTTPS_SLOTS_MAX];
  af__https_conn_st *apstSend[AF_HTTPS_SLOTS_MAX];
  const uint8 *apu8Buf[AF_HTTPS_SLOTS_MAX];
  uint32 au32Len[AF_HTTPS_SLOTS_MAX];
  af__https_conn_st *apstClose[AF_HTTPS_SLOTS_MAX];
  uint32 u32ReapCnt;
  uint32 u32SendCnt;
  uint32 u32CloseCnt;
  bool_t bCloseListen;
  bool_t bStop;
  dm_async_st *pstAsyncClose;
} af__https_batch_st;

af__https_srv_st g_stAfHttpsSrv;
static af__https_route_st g_astRoutes[AF_HTTPS_ROUTES_MAX];
static uint32 g_u32RouteCnt = 0u;

/* ============================ 槽位释前擦零与状态机 ============================ */

static void af__https_zero_free_reap(dm_io af__https_reap_st *pstR)
{
  af_https_on_free_fn pfCb = atomic_load(&g_stAfHttpsSrv.pfTestOnFree);
  void *apv[3] = {pstR->pcAuth, pstR->pu8Body, pstR->pu8Resp};
  size_t au[3] = {pstR->u32AuthCap, pstR->u32BodyCap, pstR->u32RespLen};
  uint32 i = 0u;

  for (i = 0u; i < 3u; i++)
  {
    if (NULL != apv[i])
    {
      dm_secure_zero(apv[i], au[i]);
    }
  }
  if (NULL != pfCb)
  {
    af_https_free_view_st stV = {(const uint8 *)apv[0], au[0], (const uint8 *)apv[1], au[1],
                                 (const uint8 *)apv[2], au[2]};
    pfCb(&stV);
  }
  for (i = 0u; i < 3u; i++)
  {
    dm_free_tag(apv[i]);
  }
  memset(pstR, 0, sizeof(*pstR));
}

static void af__https_detach_slot_locked(dm_io af__https_slot_st *pstS,
                                         dm_out af__https_reap_st *pstR)
{
  *pstR = (af__https_reap_st){pstS->stReq.pcAuthBuf,  pstS->stReq.u32AuthCap, pstS->stReq.pu8Body,
                              pstS->stReq.u32BodyCap, pstS->stResp.pu8Data,   pstS->stResp.u32Len};
  dm_secure_zero(pstS->stReq.acMethod, sizeof(pstS->stReq.acMethod));
  dm_secure_zero(pstS->stReq.acPath, sizeof(pstS->stReq.acPath));
  memset(pstS, 0, sizeof(*pstS));
}

static void af__https_wake_locked(void)
{
  if ((!g_stAfHttpsSrv.bStop) && (NULL != g_stAfHttpsSrv.pstAsync))
  {
    (void)dm_async_send(g_stAfHttpsSrv.pstAsync);
  }
}

int af__https_slot_alloc(dm_io af__https_conn_st *pstConn, dm_out uint32 *pu32Slot,
                         dm_out uint32 *pu32Gen)
{
  uint32 i = 0u;

  dm_mutex_lock(g_stAfHttpsSrv.pstStateMtx);
  for (i = 0u; i < AF_HTTPS_SLOTS_MAX; i++)
  {
    if (AF_HTTPS_SLOT_FREE == g_stAfHttpsSrv.astSlots[i].eState)
    {
      af__https_slot_st *pstS = &g_stAfHttpsSrv.astSlots[i];
      uint32 u32Gen = g_stAfHttpsSrv.u32NextGen++;
      g_stAfHttpsSrv.u32NextGen =
          (0u == g_stAfHttpsSrv.u32NextGen) ? 1u : g_stAfHttpsSrv.u32NextGen;
      memset(pstS, 0, sizeof(*pstS));
      pstS->u32Gen = u32Gen;
      pstS->eState = AF_HTTPS_SLOT_OPEN;
      pstS->pstConn = pstConn;
      dm_mutex_unlock(g_stAfHttpsSrv.pstStateMtx);
      *pu32Slot = i;
      *pu32Gen = u32Gen;
      return DM_SUCCESS;
    }
  }
  dm_mutex_unlock(g_stAfHttpsSrv.pstStateMtx);
  return DM_BUSY;
}

void af__https_slot_on_conn_closed(dm_in uint32 u32Slot)
{
  af__https_slot_st *pstS = NULL;
  af__https_reap_st stReap;
  bool_t bDoReap = DM_FALSE;

  if (u32Slot >= AF_HTTPS_SLOTS_MAX)
  {
    return;
  }
  dm_mutex_lock(g_stAfHttpsSrv.pstStateMtx);
  pstS = &g_stAfHttpsSrv.astSlots[u32Slot];
  pstS->pstConn = NULL;
  pstS->bConnClosed = DM_TRUE;
  pstS->bRespPending = DM_FALSE;
  pstS->eState = AF_HTTPS_SLOT_CLOSING;
  if (0u == pstS->u32CoreRefs)
  {
    af__https_detach_slot_locked(pstS, &stReap);
    bDoReap = DM_TRUE;
  }
  dm_mutex_unlock(g_stAfHttpsSrv.pstStateMtx);
  if (bDoReap)
  {
    af__https_zero_free_reap(&stReap);
  }
}

int af__https_slot_dispatch(dm_in uint32 u32Slot, dm_in uint32 u32Gen)
{
  uint64 u64Rid = (((uint64)u32Slot) << 32) | (uint64)u32Gen;
  bool_t bOk = DM_FALSE;

  if (u32Slot >= AF_HTTPS_SLOTS_MAX)
  {
    return DM_INVALID;
  }
  dm_mutex_lock(g_stAfHttpsSrv.pstStateMtx);
  if (g_stAfHttpsSrv.bAccepting && (g_stAfHttpsSrv.astSlots[u32Slot].u32Gen == u32Gen) &&
      (AF_HTTPS_SLOT_OPEN == g_stAfHttpsSrv.astSlots[u32Slot].eState))
  {
    g_stAfHttpsSrv.astSlots[u32Slot].eState = AF_HTTPS_SLOT_DISPATCHED;
    bOk = DM_TRUE;
  }
  dm_mutex_unlock(g_stAfHttpsSrv.pstStateMtx);
  return bOk ? dm_queue_post_async(AF_LINK_HTTPS_REQ, &u64Rid, (uint32)sizeof(u64Rid))
             : DM_SHUTTING_DOWN;
}

static void af__https_cleanup_all_slots(void)
{
  af__https_reap_st astReap[AF_HTTPS_SLOTS_MAX];
  uint32 u32Cnt = 0u;
  uint32 i = 0u;

  dm_mutex_lock(g_stAfHttpsSrv.pstStateMtx);
  for (i = 0u; i < AF_HTTPS_SLOTS_MAX; i++)
  {
    if (AF_HTTPS_SLOT_FREE != g_stAfHttpsSrv.astSlots[i].eState)
    {
      af__https_detach_slot_locked(&g_stAfHttpsSrv.astSlots[i], &astReap[u32Cnt++]);
    }
  }
  dm_mutex_unlock(g_stAfHttpsSrv.pstStateMtx);
  for (i = 0u; i < u32Cnt; i++)
  {
    af__https_zero_free_reap(&astReap[i]);
  }
}

/* ============================ TLS 加载与端口监听 ============================ */

static int af__https_read_der(dm_in const char *pcPath, dm_out uint8 **ppu8Out,
                              dm_out size_t *puOutLen)
{
  FILE *pf = fopen(pcPath, "rb");
  size_t uCap = AF_HTTPS_CERT_FILE_MAX + 1u;
  uint8 *pu8Buf = NULL;
  size_t uTot = 0u;

  *ppu8Out = NULL;
  *puOutLen = 0u;
  if (NULL == pf)
  {
    return DM_IO;
  }
  pu8Buf = (uint8 *)dm_malloc_tag(uCap, DM_MEM_TAG_HTTP);
  if (NULL != pu8Buf)
  {
    uTot = fread(pu8Buf, 1u, uCap, pf);
    if ((0 == ferror(pf)) && (uTot > 0u) && (uTot <= AF_HTTPS_CERT_FILE_MAX))
    {
      (void)fclose(pf);
      *ppu8Out = pu8Buf;
      *puOutLen = uTot;
      return DM_SUCCESS;
    }
    dm_secure_zero(pu8Buf, uCap);
    dm_free_tag(pu8Buf);
  }
  (void)fclose(pf);
  return (NULL == pu8Buf) ? DM_ALLOC : ((uTot > AF_HTTPS_CERT_FILE_MAX) ? DM_TOO_BIG : DM_IO);
}

static void af__https_free_tls(void)
{
  if (g_stAfHttpsSrv.bTlsReady)
  {
    mbedtls_ssl_config_free(&g_stAfHttpsSrv.stSslCfg);
    mbedtls_pk_free(&g_stAfHttpsSrv.stKey);
    mbedtls_x509_crt_free(&g_stAfHttpsSrv.stCert);
    mbedtls_ctr_drbg_free(&g_stAfHttpsSrv.stDrbg);
    mbedtls_entropy_free(&g_stAfHttpsSrv.stEntropy);
    g_stAfHttpsSrv.bTlsReady = DM_FALSE;
  }
}

static int af__https_load_tls(void)
{
  static const unsigned char aucPers[] = "afar_https_srv";
  uint8 *apu8[2] = {NULL, NULL};
  size_t auLen[2] = {0u, 0u};
  int iErr = DM_SUCCESS;
  int iRet = 0;

  if (('\0' == g_stAfHttpsSrv.acCertPath[0]) || ('\0' == g_stAfHttpsSrv.acKeyPath[0]) ||
      (PSA_SUCCESS != psa_crypto_init()))
  {
    return (('\0' == g_stAfHttpsSrv.acCertPath[0]) || ('\0' == g_stAfHttpsSrv.acKeyPath[0]))
               ? DM_INVALID
               : DM_FAILD;
  }
  mbedtls_entropy_init(&g_stAfHttpsSrv.stEntropy);
  mbedtls_ctr_drbg_init(&g_stAfHttpsSrv.stDrbg);
  mbedtls_x509_crt_init(&g_stAfHttpsSrv.stCert);
  mbedtls_pk_init(&g_stAfHttpsSrv.stKey);
  mbedtls_ssl_config_init(&g_stAfHttpsSrv.stSslCfg);
  g_stAfHttpsSrv.bTlsReady = DM_TRUE;
  if (0 != mbedtls_ctr_drbg_seed(&g_stAfHttpsSrv.stDrbg, mbedtls_entropy_func,
                                 &g_stAfHttpsSrv.stEntropy, aucPers, sizeof(aucPers) - 1u))
  {
    return DM_FAILD;
  }
  iErr = af__https_read_der(g_stAfHttpsSrv.acCertPath, &apu8[0], &auLen[0]);
  iErr = (DM_SUCCESS == iErr) ? af__https_read_der(g_stAfHttpsSrv.acKeyPath, &apu8[1], &auLen[1])
                              : iErr;
  if (DM_SUCCESS == iErr)
  {
    iRet = mbedtls_x509_crt_parse_der(&g_stAfHttpsSrv.stCert, apu8[0], auLen[0]);
    iRet = (0 == iRet) ? mbedtls_pk_parse_key(&g_stAfHttpsSrv.stKey, apu8[1], auLen[1], NULL, 0u,
                                              mbedtls_ctr_drbg_random, &g_stAfHttpsSrv.stDrbg)
                       : iRet;
    iErr = (0 == iRet) ? DM_SUCCESS : DM_INVALID;
  }
  for (int k = 0; k < 2; k++)
  {
    if (NULL != apu8[k])
    {
      dm_secure_zero(apu8[k], AF_HTTPS_CERT_FILE_MAX + 1u);
      dm_free_tag(apu8[k]);
    }
  }
  if ((DM_SUCCESS != iErr) ||
      (0 != mbedtls_ssl_config_defaults(&g_stAfHttpsSrv.stSslCfg, MBEDTLS_SSL_IS_SERVER,
                                        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)))
  {
    return (DM_SUCCESS != iErr) ? iErr : DM_FAILD;
  }
  mbedtls_ssl_conf_min_tls_version(&g_stAfHttpsSrv.stSslCfg, MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_max_tls_version(&g_stAfHttpsSrv.stSslCfg, MBEDTLS_SSL_VERSION_TLS1_3);
  mbedtls_ssl_conf_rng(&g_stAfHttpsSrv.stSslCfg, mbedtls_ctr_drbg_random, &g_stAfHttpsSrv.stDrbg);
  return (0 == mbedtls_ssl_conf_own_cert(&g_stAfHttpsSrv.stSslCfg, &g_stAfHttpsSrv.stCert,
                                         &g_stAfHttpsSrv.stKey))
             ? DM_SUCCESS
             : DM_FAILD;
}

static int af__https_bind_listen(void)
{
  const char *pcAddr = ('\0' != g_stAfHttpsSrv.acBindAddr[0]) ? g_stAfHttpsSrv.acBindAddr : "::";
  uint16 u16Port = g_stAfHttpsSrv.stCfg.u16Port;
  dm_tcp_st *pstL = NULL;
  dm_sockaddr_st stAddr;
  int iErr = DM_IO;

  if (DM_SUCCESS != dm_tcp_create(g_stAfHttpsSrv.pstLoop, &pstL))
  {
    return DM_IO;
  }
  if (DM_SUCCESS == dm_sockaddr_ip6(pcAddr, u16Port, &stAddr))
  {
    iErr = dm_tcp_bind(pstL, &stAddr, DM_TCP_F_NONE);
    if ((DM_SUCCESS != iErr) && (0 == strcmp(pcAddr, "::")) && (DM_ADDR_IN_USE != iErr) &&
        (DM_PERM != iErr))
    {
      (void)dm_tcp_close(pstL, NULL, NULL);
      pstL = NULL;
      if ((DM_SUCCESS == dm_tcp_create(g_stAfHttpsSrv.pstLoop, &pstL)) &&
          (DM_SUCCESS == dm_sockaddr_ip4("0.0.0.0", u16Port, &stAddr)))
      {
        iErr = dm_tcp_bind(pstL, &stAddr, DM_TCP_F_NONE);
      }
    }
  }
  else if (DM_SUCCESS == dm_sockaddr_ip4(pcAddr, u16Port, &stAddr))
  {
    iErr = dm_tcp_bind(pstL, &stAddr, DM_TCP_F_NONE);
  }
  if ((DM_SUCCESS == iErr) && (DM_SUCCESS == dm_tcp_listen(pstL, 128, af__https_on_new_conn, NULL)))
  {
    memset(&stAddr, 0, sizeof(stAddr));
    if (DM_SUCCESS == dm_tcp_getsockname(pstL, &stAddr))
    {
      atomic_store(&g_stAfHttpsSrv.u16BoundPort, stAddr.u16Port);
    }
    g_stAfHttpsSrv.pstListen = pstL;
    DM_LOG_INFO("https listening on [%s]:%u", pcAddr,
                (uint)atomic_load(&g_stAfHttpsSrv.u16BoundPort));
    return DM_SUCCESS;
  }
  if (NULL != pstL)
  {
    (void)dm_tcp_close(pstL, NULL, NULL);
  }
  return DM_IO;
}

/* ============================ T_https 异步事件与线程入口 ============================ */

static void af__https_async_collect(dm_out af__https_batch_st *pstB)
{
  uint32 i = 0u;

  memset(pstB, 0, sizeof(*pstB));
  dm_mutex_lock(g_stAfHttpsSrv.pstStateMtx);
  pstB->bStop = g_stAfHttpsSrv.bStop;
  pstB->bCloseListen = g_stAfHttpsSrv.bCloseListen || pstB->bStop;
  g_stAfHttpsSrv.bCloseListen = DM_FALSE;
  if (pstB->bStop)
  {
    pstB->pstAsyncClose = g_stAfHttpsSrv.pstAsync;
    g_stAfHttpsSrv.pstAsync = NULL;
  }
  for (i = 0u; i < AF_HTTPS_SLOTS_MAX; i++)
  {
    af__https_slot_st *pstS = &g_stAfHttpsSrv.astSlots[i];
    if (pstS->bConnClosed && (0u == pstS->u32CoreRefs))
    {
      af__https_detach_slot_locked(pstS, &pstB->astReap[pstB->u32ReapCnt++]);
    }
    else if (pstB->bStop || (AF_HTTPS_SLOT_CLOSING == pstS->eState))
    {
      pstS->bRespPending = DM_FALSE;
      if ((NULL != pstS->pstConn) && (!pstS->pstConn->bClosing))
      {
        pstB->apstClose[pstB->u32CloseCnt++] = pstS->pstConn;
      }
    }
    else if (pstS->bRespPending)
    {
      pstS->bRespPending = DM_FALSE;
      if ((NULL != pstS->pstConn) && (!pstS->pstConn->bClosing))
      {
        pstB->apstSend[pstB->u32SendCnt] = pstS->pstConn;
        pstB->apu8Buf[pstB->u32SendCnt] = pstS->stResp.pu8Data;
        pstB->au32Len[pstB->u32SendCnt++] = pstS->stResp.u32Len;
      }
    }
  }
  dm_mutex_unlock(g_stAfHttpsSrv.pstStateMtx);
}

static void af__https_on_async(dm_io dm_async_st *pstAsync, dm_io void *pvUser)
{
  af__https_batch_st stB;
  uint32 i = 0u;

  (void)pstAsync;
  (void)pvUser;
  af__https_async_collect(&stB);
  for (i = 0u; i < stB.u32ReapCnt; i++)
  {
    af__https_zero_free_reap(&stB.astReap[i]);
  }
  if (stB.bCloseListen && (NULL != g_stAfHttpsSrv.pstListen))
  {
    (void)dm_tcp_close(g_stAfHttpsSrv.pstListen, NULL, NULL);
    g_stAfHttpsSrv.pstListen = NULL;
  }
  for (i = 0u; i < stB.u32CloseCnt; i++)
  {
    af__https_conn_close(stB.apstClose[i]);
  }
  (void)dm_async_close(stB.bStop ? stB.pstAsyncClose : NULL, NULL, NULL);
  for (i = 0u; i < stB.u32SendCnt; i++)
  {
    af__https_conn_send_resp(stB.apstSend[i], stB.apu8Buf[i], stB.au32Len[i]);
  }
}

static void af__https_thread_entry(dm_io void *pvArg)
{
  af__https_boot_st *pstBoot = (af__https_boot_st *)pvArg;
  int iErr = af__https_load_tls();

  iErr = (DM_SUCCESS == iErr) ? dm_loop_create(&g_stAfHttpsSrv.pstLoop) : iErr;
  iErr = (DM_SUCCESS == iErr) ? dm_async_create(g_stAfHttpsSrv.pstLoop, af__https_on_async, NULL,
                                                &g_stAfHttpsSrv.pstAsync)
                              : iErr;
  iErr = (DM_SUCCESS == iErr) ? af__https_bind_listen() : iErr;
  if (DM_SUCCESS != iErr)
  {
    if (NULL != g_stAfHttpsSrv.pstAsync)
    {
      (void)dm_async_close(g_stAfHttpsSrv.pstAsync, NULL, NULL);
      g_stAfHttpsSrv.pstAsync = NULL;
    }
    if (NULL != g_stAfHttpsSrv.pstLoop)
    {
      (void)dm_loop_run(g_stAfHttpsSrv.pstLoop, DM_LOOP_RUN_DEFAULT);
    }
    af__https_free_tls();
  }
  dm_mutex_lock(pstBoot->pstMtx);
  pstBoot->iErr = iErr;
  pstBoot->bDone = DM_TRUE;
  dm_cond_signal(pstBoot->pstCond);
  dm_mutex_unlock(pstBoot->pstMtx);
  if (DM_SUCCESS == iErr)
  {
    (void)dm_loop_run(g_stAfHttpsSrv.pstLoop, DM_LOOP_RUN_DEFAULT);
    af__https_cleanup_all_slots();
  }
  if (NULL != g_stAfHttpsSrv.pstLoop)
  {
    (void)dm_loop_destroy(g_stAfHttpsSrv.pstLoop);
    g_stAfHttpsSrv.pstLoop = NULL;
  }
}

/* ============================ T_core 路由派发 ============================ */

static int af__https_on_core_req(dm_io void *pvArg)
{
  static const uint8 au8Empty[1] = {0};
  const dm_link_msg_st *pstLink = (const dm_link_msg_st *)pvArg;
  uint64 u64Rid = 0u;
  uint32 u32Slot = 0u;
  af__https_slot_st *pstS = NULL;
  af_https_msg_st stMsg;
  bool_t bPathHit = DM_FALSE;
  af_https_handler_fn pfHit = NULL;
  uint32 i = 0u;

  if ((NULL == pstLink) || (NULL == pstLink->pu8Data) || (sizeof(uint64) != pstLink->u32Len) ||
      (NULL == g_stAfHttpsSrv.pstStateMtx))
  {
    return DM_SUCCESS;
  }
  memcpy(&u64Rid, pstLink->pu8Data, sizeof(uint64));
  u32Slot = (uint32)(u64Rid >> 32);
  dm_mutex_lock(g_stAfHttpsSrv.pstStateMtx);
  pstS = (u32Slot < AF_HTTPS_SLOTS_MAX) ? &g_stAfHttpsSrv.astSlots[u32Slot] : NULL;
  if ((!g_stAfHttpsSrv.bAccepting) || (NULL == pstS) ||
      (pstS->u32Gen != (uint32)(u64Rid & 0xffffffffu)) ||
      (AF_HTTPS_SLOT_DISPATCHED != pstS->eState))
  {
    dm_mutex_unlock(g_stAfHttpsSrv.pstStateMtx);
    return DM_SUCCESS;
  }
  pstS->u32CoreRefs++;
  stMsg = (af_https_msg_st){u64Rid,
                            pstS->stReq.acMethod,
                            pstS->stReq.acPath,
                            pstS->stReq.pcAuthBuf,
                            (NULL != pstS->stReq.pu8Body) ? pstS->stReq.pu8Body : au8Empty,
                            pstS->stReq.u32BodyLen};
  dm_mutex_unlock(g_stAfHttpsSrv.pstStateMtx);

  for (i = 0u; i < g_u32RouteCnt; i++)
  {
    if (0 != strcmp(g_astRoutes[i].acPath, stMsg.pcPath))
    {
      continue;
    }
    bPathHit = DM_TRUE;
    if (0 == strcmp(g_astRoutes[i].acMethod, stMsg.pcMethod))
    {
      pfHit = g_astRoutes[i].pfHandler;
      break;
    }
  }
  if (NULL != pfHit)
  {
    pfHit(&stMsg);
  }
  else
  {
    const char *pcB = bPathHit ? "{\"code\":\"method_not_allowed\"}" : "{\"code\":\"not_found\"}";
    (void)af_https_reply(u64Rid, bPathHit ? 405u : 404u, pcB, (uint32)strlen(pcB));
  }
  dm_mutex_lock(g_stAfHttpsSrv.pstStateMtx);
  pstS->u32CoreRefs = (pstS->u32CoreRefs > 0u) ? (pstS->u32CoreRefs - 1u) : 0u;
  if (pstS->bConnClosed && (0u == pstS->u32CoreRefs))
  {
    af__https_wake_locked();
  }
  dm_mutex_unlock(g_stAfHttpsSrv.pstStateMtx);
  return DM_SUCCESS;
}

/* ============================ 生命周期钩子与公开 API ============================ */

static void af__https_stop_thread(void)
{
  if (NULL != g_stAfHttpsSrv.pstStateMtx)
  {
    dm_mutex_lock(g_stAfHttpsSrv.pstStateMtx);
    if (!g_stAfHttpsSrv.bStop)
    {
      g_stAfHttpsSrv.bAccepting = DM_FALSE;
      g_stAfHttpsSrv.bStop = DM_TRUE;
      (void)dm_async_send(g_stAfHttpsSrv.pstAsync);
    }
    dm_mutex_unlock(g_stAfHttpsSrv.pstStateMtx);
  }
  if (NULL != g_stAfHttpsSrv.pstThread)
  {
    (void)dm_thread_join(g_stAfHttpsSrv.pstThread);
    g_stAfHttpsSrv.pstThread = NULL;
  }
}

static void af__https_reset_cfg_state(void)
{
  dm_mutex_destroy(g_stAfHttpsSrv.pstStateMtx);
  g_stAfHttpsSrv.pstStateMtx = NULL;
  g_stAfHttpsSrv.bAccepting = DM_FALSE;
  g_stAfHttpsSrv.bCloseListen = DM_FALSE;
  g_stAfHttpsSrv.bStop = DM_FALSE;
  g_stAfHttpsSrv.bInited = DM_FALSE;
  atomic_store(&g_stAfHttpsSrv.u16BoundPort, 0u);
  af_https_set_cfg(NULL);
}

static int af__https_start_thread(void)
{
  af__https_boot_st stBoot = {0};
  dm_thread_opt_st stOpt = {.pcName = "af_https"};
  int iErr = DM_ALLOC;

  if ((DM_SUCCESS == dm_mutex_create(&stBoot.pstMtx)) &&
      (DM_SUCCESS == dm_cond_create(&stBoot.pstCond)) &&
      (DM_SUCCESS == (iErr = dm_thread_create_ex(&stOpt, af__https_thread_entry, &stBoot,
                                                 &g_stAfHttpsSrv.pstThread))))
  {
    dm_mutex_lock(stBoot.pstMtx);
    while (!stBoot.bDone)
    {
      dm_cond_wait(stBoot.pstCond, stBoot.pstMtx);
    }
    iErr = stBoot.iErr;
    dm_mutex_unlock(stBoot.pstMtx);
    if (DM_SUCCESS != iErr)
    {
      (void)dm_thread_join(g_stAfHttpsSrv.pstThread);
      g_stAfHttpsSrv.pstThread = NULL;
    }
  }
  dm_cond_destroy(stBoot.pstCond);
  dm_mutex_destroy(stBoot.pstMtx);
  return iErr;
}

static uint32 af__https_def(dm_in uint32 u32Val, dm_in uint32 u32Def, dm_in uint32 u32Max)
{
  uint32 u32Out = (0u == u32Val) ? u32Def : u32Val;
  return ((0u != u32Max) && (u32Out > u32Max)) ? u32Max : u32Out;
}

static int af__https_before_init(dm_in const dm_cfg_st *pstCfg)
{
  af_https_cfg_st *pstC = &g_stAfHttpsSrv.stCfg;
  int iErr = DM_SUCCESS;

  (void)pstCfg;
  if (!g_stAfHttpsSrv.bCfgSet)
  {
    return DM_SUCCESS;
  }
  pstC->u32MaxConns = af__https_def(pstC->u32MaxConns, 32u, AF_HTTPS_SLOTS_MAX);
  pstC->u32MaxPerIp = af__https_def(pstC->u32MaxPerIp, 8u, 0u);
  pstC->u32BodyMax = af__https_def(pstC->u32BodyMax, 65536u, AF_HTTPS_BODY_HARD_MAX);
  pstC->u32HandshakeMs = af__https_def(pstC->u32HandshakeMs, 3000u, 0u);
  pstC->u32IdleMs = af__https_def(pstC->u32IdleMs, 15000u, 0u);
  pstC->u32ReplyMs = af__https_def(pstC->u32ReplyMs, 30000u, 0u);
  iErr = dm_link_has_handler(AF_LINK_HTTPS_REQ)
             ? DM_SUCCESS
             : dm_link_register_handler(AF_LINK_HTTPS_REQ, af__https_on_core_req, NULL, NULL);
  if (((DM_SUCCESS != iErr) && (DM_LINK_ERR_REREGIST != iErr)) ||
      (DM_SUCCESS != (iErr = dm_mutex_create(&g_stAfHttpsSrv.pstStateMtx))))
  {
    return iErr;
  }
  memset(g_stAfHttpsSrv.astSlots, 0, sizeof(g_stAfHttpsSrv.astSlots));
  g_stAfHttpsSrv.u32NextGen = 1u;
  g_stAfHttpsSrv.bAccepting = DM_TRUE;
  g_stAfHttpsSrv.bCloseListen = DM_FALSE;
  g_stAfHttpsSrv.bStop = DM_FALSE;
  atomic_store(&g_stAfHttpsSrv.u16BoundPort, 0u);
  if (DM_SUCCESS != (iErr = af__https_start_thread()))
  {
    af__https_free_tls();
    af__https_reset_cfg_state();
    return iErr;
  }
  g_stAfHttpsSrv.bInited = DM_TRUE;
  return DM_SUCCESS;
}

static void af__https_on_drain(void)
{
  if (g_stAfHttpsSrv.bInited && (NULL != g_stAfHttpsSrv.stCfg.pfOnDrain))
  {
    g_stAfHttpsSrv.stCfg.pfOnDrain();
  }
  if (NULL != g_stAfHttpsSrv.pstStateMtx)
  {
    dm_mutex_lock(g_stAfHttpsSrv.pstStateMtx);
    g_stAfHttpsSrv.bCloseListen = DM_TRUE;
    af__https_wake_locked();
    dm_mutex_unlock(g_stAfHttpsSrv.pstStateMtx);
  }
}

static void af__https_on_end(void)
{
  af__https_stop_thread();
  af__https_free_tls();
  memset(g_astRoutes, 0, sizeof(g_astRoutes));
  g_u32RouteCnt = 0u;
  memset(g_stAfHttpsSrv.astSlots, 0, sizeof(g_stAfHttpsSrv.astSlots));
  af__https_reset_cfg_state();
}

static const dm_ext_hooks_st g_stAfHttpsHooks = {
    .pfBeforeInit = af__https_before_init,
    .pfOnDrain = af__https_on_drain,
    .pfOnTeardown = af__https_stop_thread,
    .pfOnEnd = af__https_on_end,
};

void af_https_set_cfg(dm_in const af_https_cfg_st *pstCfg)
{
  memset(&g_stAfHttpsSrv.stCfg, 0, sizeof(g_stAfHttpsSrv.stCfg));
  g_stAfHttpsSrv.acBindAddr[0] = '\0';
  g_stAfHttpsSrv.acCertPath[0] = '\0';
  g_stAfHttpsSrv.acKeyPath[0] = '\0';
  g_stAfHttpsSrv.bCfgSet = (NULL != pstCfg) ? DM_TRUE : DM_FALSE;
  if (NULL != pstCfg)
  {
    g_stAfHttpsSrv.stCfg = *pstCfg;
    snprintf(g_stAfHttpsSrv.acBindAddr, sizeof(g_stAfHttpsSrv.acBindAddr), "%s",
             (NULL != pstCfg->pcBindAddr) ? pstCfg->pcBindAddr : "");
    snprintf(g_stAfHttpsSrv.acCertPath, sizeof(g_stAfHttpsSrv.acCertPath), "%s",
             (NULL != pstCfg->pcCertDerPath) ? pstCfg->pcCertDerPath : "");
    snprintf(g_stAfHttpsSrv.acKeyPath, sizeof(g_stAfHttpsSrv.acKeyPath), "%s",
             (NULL != pstCfg->pcKeyDerPath) ? pstCfg->pcKeyDerPath : "");
  }
}

const dm_ext_hooks_st *af_https_hooks(void)
{
  return &g_stAfHttpsHooks;
}

int af_https_route_exact(dm_in const char *pcMethod, dm_in const char *pcPath,
                         dm_in af_https_handler_fn pfHandler)
{
  af__https_route_st *pstR = NULL;
  uint32 i = 0u;

  if ((NULL == pcMethod) || (NULL == pcPath) || (NULL == pfHandler))
  {
    return DM_INVALID;
  }
  if ((strlen(pcMethod) >= sizeof(pstR->acMethod)) || (strlen(pcPath) >= sizeof(pstR->acPath)))
  {
    return DM_TOO_BIG;
  }
  for (i = 0u; i < g_u32RouteCnt; i++)
  {
    if ((0 == strcmp(g_astRoutes[i].acMethod, pcMethod)) &&
        (0 == strcmp(g_astRoutes[i].acPath, pcPath)))
    {
      return DM_EXISTS;
    }
  }
  if (g_u32RouteCnt >= AF_HTTPS_ROUTES_MAX)
  {
    return DM_RANGE;
  }
  pstR = &g_astRoutes[g_u32RouteCnt++];
  snprintf(pstR->acMethod, sizeof(pstR->acMethod), "%s", pcMethod);
  snprintf(pstR->acPath, sizeof(pstR->acPath), "%s", pcPath);
  pstR->pfHandler = pfHandler;
  return DM_SUCCESS;
}

int af_https_reply(dm_in uint64 u64ReplyId, dm_in uint uiStatus, dm_in const void *pvBody,
                   dm_in uint32 u32Len)
{
  uint32 u32Slot = (uint32)(u64ReplyId >> 32);
  af__https_slot_st *pstS = NULL;
  uint8 *pu8Resp = NULL;
  uint32 u32RespLen = 0u;
  int iErr = DM_CONN_GONE;

  if (((u32Len > 0u) && (NULL == pvBody)) || (NULL == g_stAfHttpsSrv.pstStateMtx))
  {
    return ((u32Len > 0u) && (NULL == pvBody)) ? DM_INVALID : DM_CONN_GONE;
  }
  dm_mutex_lock(g_stAfHttpsSrv.pstStateMtx);
  pstS = (u32Slot < AF_HTTPS_SLOTS_MAX) ? &g_stAfHttpsSrv.astSlots[u32Slot] : NULL;
  if (g_stAfHttpsSrv.bAccepting && (NULL != pstS) &&
      (pstS->u32Gen == (uint32)(u64ReplyId & 0xffffffffu)))
  {
    if (pstS->bReplied)
    {
      iErr = DM_EXISTS;
    }
    else if (AF_HTTPS_SLOT_DISPATCHED == pstS->eState)
    {
      iErr = af__https_build_resp(uiStatus, pvBody, u32Len, &pu8Resp, &u32RespLen);
      if (DM_SUCCESS == iErr)
      {
        pstS->stResp.pu8Data = pu8Resp;
        pstS->stResp.u32Len = u32RespLen;
        pstS->bReplied = DM_TRUE;
        pstS->eState = AF_HTTPS_SLOT_REPLIED;
        pstS->bRespPending = DM_TRUE;
      }
      else
      {
        pstS->eState = AF_HTTPS_SLOT_CLOSING;
      }
      af__https_wake_locked();
    }
  }
  dm_mutex_unlock(g_stAfHttpsSrv.pstStateMtx);
  return iErr;
}

uint16 af_https_get_port(void)
{
  return atomic_load(&g_stAfHttpsSrv.u16BoundPort);
}

void af_https_set_test_on_free(dm_in af_https_on_free_fn pfCb)
{
  atomic_store(&g_stAfHttpsSrv.pfTestOnFree, pfCb);
}

#ifdef __cplusplus
}
#endif
