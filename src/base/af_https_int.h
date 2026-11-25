/**
 ******************************************************************************
 * @file    af_https_int.h
 * @author  Afar_C
 * @version 2.0.0
 * @brief   独立线程 HTTPS 服务内部共享头（仅 af_https_srv.c 与 af_https_conn.c 包含）。
 ******************************************************************************
 */
#ifndef AF_HTTPS_INT_H
#define AF_HTTPS_INT_H

#define DM_LOG_MOD "https"
#include <dm.h>
#include <dm/link.h>
#include <dm/log.h>
#include <dm/loop.h>
#include <dm/module.h>
#include <dm/sock.h>
#include <dm/thread.h>

#include "af_modules.h"
#include "base/af_https_srv.h"

#include <llhttp.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <stdatomic.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define AF_HTTPS_ROUTES_MAX 32u
#define AF_HTTPS_SLOTS_MAX 64u
#define AF_HTTPS_IN_BIO_MAX 65536u
#define AF_HTTPS_AUTH_MAX 4096u
#define AF_HTTPS_PATH_MAX 256u
#define AF_HTTPS_CERT_FILE_MAX 65536u
#define AF_HTTPS_BODY_HARD_MAX (16u * 1024u * 1024u)
#define AF_HTTPS_TX_QUEUE_MAX (4u * 1024u * 1024u)
#define AF_HTTPS_ADDR_MAX 64u
#define AF_HTTPS_FS_PATH_MAX 1024u

typedef enum af__https_slot_state
{
  AF_HTTPS_SLOT_FREE = 0,
  AF_HTTPS_SLOT_OPEN = 1,
  AF_HTTPS_SLOT_DISPATCHED = 2,
  AF_HTTPS_SLOT_REPLIED = 3,
  AF_HTTPS_SLOT_CLOSING = 4
} af__https_slot_state_et;

typedef struct af__https_buf
{
  uint8 *pu8Data;
  uint32 u32Len;
  uint32 u32Cap;
  uint32 u32Off;
} af__https_buf_st;

typedef struct af__https_req
{
  char acMethod[12];
  char acPath[AF_HTTPS_PATH_MAX];
  uint32 u32PathLen;
  char *pcAuthBuf;
  uint32 u32AuthLen;
  uint32 u32AuthCap;
  uint8 *pu8Body;
  uint32 u32BodyLen;
  uint32 u32BodyCap;
} af__https_req_st;

typedef struct af__https_resp
{
  uint8 *pu8Data;
  uint32 u32Len;
} af__https_resp_st;

typedef struct af__https_reap
{
  char *pcAuth;
  uint32 u32AuthCap;
  uint8 *pu8Body;
  uint32 u32BodyCap;
  uint8 *pu8Resp;
  uint32 u32RespLen;
} af__https_reap_st;

typedef struct af__https_conn
{
  dm_tcp_st *pstTcp;
  dm_loop_timer_st *pstTimer;
  uint8 u8ClosePending;
  uint32 u32Slot;
  uint32 u32Gen;
  uint8 au8PeerIp[16];
  bool_t bHasSlot;
  bool_t bSslInit;
  bool_t bHandshook;
  bool_t bReqDone;
  bool_t bClosing;
  bool_t bTooBig;
  bool_t bShutPending;
  uint32 u32PostJunk;

  mbedtls_ssl_context stSsl;
  af__https_buf_st stIn;
  af__https_buf_st stOut;

  llhttp_t stParser;
  llhttp_settings_t stParseSet;
  bool_t bCurHdrAuth;
  bool_t bSeenAuth;
  char acCurHdrName[32];
  uint32 u32CurHdrNameLen;
} af__https_conn_st;

typedef struct af__https_slot
{
  uint32 u32Gen;
  af__https_slot_state_et eState;
  uint32 u32CoreRefs;
  bool_t bReplied;
  bool_t bConnClosed;
  bool_t bRespPending;
  af__https_conn_st *pstConn;
  af__https_req_st stReq;
  af__https_resp_st stResp;
} af__https_slot_st;

typedef struct af__https_srv
{
  bool_t bCfgSet;
  bool_t bInited;
  bool_t bTlsReady;
  af_https_cfg_st stCfg;
  char acBindAddr[AF_HTTPS_ADDR_MAX];
  char acCertPath[AF_HTTPS_FS_PATH_MAX];
  char acKeyPath[AF_HTTPS_FS_PATH_MAX];

  dm_mutex_st *pstStateMtx;
  dm_thread_st *pstThread;
  dm_loop_st *pstLoop;
  dm_async_st *pstAsync;
  dm_tcp_st *pstListen;

  bool_t bAccepting;
  bool_t bCloseListen;
  bool_t bStop;
  _Atomic uint16 u16BoundPort;
  _Atomic(af_https_on_free_fn) pfTestOnFree;

  mbedtls_entropy_context stEntropy;
  mbedtls_ctr_drbg_context stDrbg;
  mbedtls_x509_crt stCert;
  mbedtls_pk_context stKey;
  mbedtls_ssl_config stSslCfg;

  af__https_slot_st astSlots[AF_HTTPS_SLOTS_MAX];
  uint32 u32NextGen;
} af__https_srv_st;

extern af__https_srv_st g_stAfHttpsSrv;

/* ---- af_https_srv.c 提供给 af_https_conn.c 的内部原语 ---- */
int af__https_slot_alloc(dm_io af__https_conn_st *pstConn, dm_out uint32 *pu32Slot,
                         dm_out uint32 *pu32Gen);
void af__https_slot_on_conn_closed(dm_in uint32 u32Slot);
int af__https_slot_dispatch(dm_in uint32 u32Slot, dm_in uint32 u32Gen);

/* ---- af_https_conn.c 提供给 af_https_srv.c 的内部原语 ---- */
int af__https_build_resp(dm_in uint uiStatus, dm_in const void *pvBody, dm_in uint32 u32Len,
                         dm_out uint8 **ppu8Out, dm_out uint32 *pu32OutLen);
void af__https_on_new_conn(dm_io dm_tcp_st *pstServer, dm_in int iErr, dm_io void *pvUser);
void af__https_conn_send_resp(dm_io af__https_conn_st *pstC, dm_in const uint8 *pu8Resp,
                              dm_in uint32 u32Len);
void af__https_conn_close(dm_io af__https_conn_st *pstC);

#ifdef __cplusplus
}
#endif

#endif /* AF_HTTPS_INT_H */
