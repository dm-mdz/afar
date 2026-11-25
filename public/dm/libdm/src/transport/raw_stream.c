/**
 ******************************************************************************
 * @file    raw_stream.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   零 <uv.h> 异步原始字节流实现（DD-18 §1.1.4，入 libdm_net.a）：
 *          基于主循环 union { dm_tcp_st; dm_pipe_st; } 驱动，0 额外线程。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "raw"
#include <dm.h>
#include <dm/raw_stream.h>
#include <dm/log.h>

#include <stdint.h> /* SIZE_MAX */
#include <string.h> /* memcpy / memset */

#include "../dm_internal.h"
#include "conn.h" /* dm__stream_kind_et */

#define DM__RAW_RX_BUF_CAP 16384u

struct dm_raw_stream
{
  union
  {
    dm_uv_tcp_st stTcp;
    dm_uv_pipe_st stPipe;
  } uStream;
  uint8 u8StreamKind;
  bool_t bClosing;
  dm_raw_read_cb pfOnRead;
  void *pvUser;
  uint8 *pu8ReadBuf;
};

typedef struct dm__raw_write
{
  dm_uv_write_st stReq;
  uint8 au8Data[];
} dm__raw_write_st;

static inline dm_uv_stream_st *dm__raw_stream(dm_io dm_raw_stream_st *pstS)
{
  return (DM__STREAM_PIPE == pstS->u8StreamKind) ? (dm_uv_stream_st *)&pstS->uStream.stPipe
                                                 : (dm_uv_stream_st *)&pstS->uStream.stTcp;
}

static void dm__raw_on_closed(dm_in dm_uv_handle_st *pstHandle)
{
  dm_raw_stream_st *pstS = (dm_raw_stream_st *)pstHandle;
  dm_free_tag(pstS->pu8ReadBuf);
  dm_free_tag(pstS);
  return;
}

static void dm__raw_on_alloc(dm_in dm_uv_handle_st *pstHandle, dm_in size_t uSuggested,
                             dm_out dm_uv_buf_st *pstBuf)
{
  dm_raw_stream_st *pstS = (dm_raw_stream_st *)pstHandle;
  (void)uSuggested;
  pstBuf->base = (char *)pstS->pu8ReadBuf;
  pstBuf->len = DM__RAW_RX_BUF_CAP;
  return;
}

static void dm__raw_on_read(dm_in dm_uv_stream_st *pstStream, dm_in ssize_t iNRead,
                            dm_in const dm_uv_buf_st *pstBuf)
{
  dm_raw_stream_st *pstS = (dm_raw_stream_st *)pstStream;
  (void)pstBuf;

  if (pstS->bClosing)
  {
    return;
  }
  if (iNRead < 0)
  {
    (void)dm_uv_read_stop(pstStream);
    if (NULL != pstS->pfOnRead)
    {
      pstS->pfOnRead(pstS, NULL, 0u, (DM_UV_EOF == iNRead) ? DM_CONN_GONE : DM_IO, pstS->pvUser);
    }
    return;
  }
  if ((iNRead > 0) && (NULL != pstS->pfOnRead))
  {
    pstS->pfOnRead(pstS, pstS->pu8ReadBuf, (uint32)iNRead, DM_SUCCESS, pstS->pvUser);
  }
  return;
}

static void dm__raw_on_written(dm_in dm_uv_write_st *pstReq, dm_in int iStatus)
{
  dm__raw_write_st *pstW = container_of(pstReq, dm__raw_write_st, stReq);
  (void)iStatus;
  dm_free_tag(pstW);
  return;
}

int dm_raw_stream_adopt_fd(dm_in dm_peer_fd_t tFd, dm_in bool_t bIsPipe,
                           dm_in dm_raw_read_cb pfOnRead, dm_in void *pvUser,
                           dm_out dm_raw_stream_st **ppstOut)
{
  dm_raw_stream_st *pstS = NULL;
  int iErrCode = DM_SUCCESS;

  DM__CHECK_LOOP();
  if (NULL != ppstOut)
  {
    *ppstOut = NULL;
  }
  if ((DM_PEER_FD_INVALID == tFd) || (NULL == ppstOut))
  {
    return DM_INVALID;
  }
  if (dm__phase() >= DM__PHASE_TEARDOWN)
  {
    return DM_SHUTTING_DOWN;
  }
  pstS = (dm_raw_stream_st *)dm_calloc_tag(1u, sizeof(dm_raw_stream_st), DM_MEM_TAG_LINK_CONN);
  if (NULL == pstS)
  {
    return DM_ALLOC;
  }
  pstS->pu8ReadBuf = (uint8 *)dm_malloc_tag(DM__RAW_RX_BUF_CAP, DM_MEM_TAG_LINK_RX);
  if (NULL == pstS->pu8ReadBuf)
  {
    dm_free_tag(pstS);
    return DM_ALLOC;
  }
  pstS->u8StreamKind = bIsPipe ? DM__STREAM_PIPE : DM__STREAM_TCP;
  pstS->pfOnRead = pfOnRead;
  pstS->pvUser = pvUser;

  if (bIsPipe)
  {
    iErrCode = dm_uv_pipe_init(dm_uv_default_loop(), &pstS->uStream.stPipe, 0);
    if (DM_SUCCESS != iErrCode)
    {
      dm_free_tag(pstS->pu8ReadBuf);
      dm_free_tag(pstS);
      return iErrCode;
    }
    iErrCode = dm_uv_pipe_open(&pstS->uStream.stPipe, (dm_uv_file)tFd);
  }
  else
  {
    iErrCode = dm_uv_tcp_init(dm_uv_default_loop(), &pstS->uStream.stTcp);
    if (DM_SUCCESS != iErrCode)
    {
      dm_free_tag(pstS->pu8ReadBuf);
      dm_free_tag(pstS);
      return iErrCode;
    }
    iErrCode = dm_uv_tcp_open(&pstS->uStream.stTcp, (dm_uv_os_sock_t)tFd);
  }
  if (DM_SUCCESS != iErrCode)
  {
    /* open 失败时句柄未接管 tFd，异步关闭空 libuv 句柄，调用方保留 tFd 所有权 */
    pstS->bClosing = DM_TRUE;
    pstS->pfOnRead = NULL;
    dm_uv_close((dm_uv_handle_st *)dm__raw_stream(pstS), dm__raw_on_closed);
    return iErrCode;
  }
  if (!bIsPipe)
  {
    (void)dm_uv_tcp_nodelay(&pstS->uStream.stTcp, 1);
  }
  iErrCode = dm_uv_read_start(dm__raw_stream(pstS), dm__raw_on_alloc, dm__raw_on_read);
  if (DM_SUCCESS != iErrCode)
  {
    DM_LOG_WARN("raw_stream read_start failed: %s", dm_errno_str(iErrCode));
  }
  *ppstOut = pstS;
  return DM_SUCCESS;
}

int dm_raw_stream_write(dm_io dm_raw_stream_st *pstS, dm_in const void *pvData, dm_in uint32 u32Len)
{
  dm__raw_write_st *pstW = NULL;
  dm_uv_buf_st stBuf = {0};
  int iErrCode = DM_SUCCESS;

  DM__CHECK_LOOP();
  if ((NULL == pstS) || ((NULL == pvData) && (u32Len > 0u)))
  {
    return DM_INVALID;
  }
  if (pstS->bClosing)
  {
    return DM_CONN_GONE;
  }
  if (0u == u32Len)
  {
    return DM_SUCCESS;
  }
  if (u32Len > (64u * 1024u * 1024u))
  {
    return DM_TOO_BIG;
  }
  pstW = (dm__raw_write_st *)dm_malloc_tag(sizeof(dm__raw_write_st) + (size_t)u32Len,
                                           DM_MEM_TAG_LINK_TX);
  if (NULL == pstW)
  {
    return DM_ALLOC;
  }
  memcpy(pstW->au8Data, pvData, u32Len);
  stBuf = dm_uv_buf_init((char *)pstW->au8Data, u32Len);
  iErrCode = dm_uv_write(&pstW->stReq, dm__raw_stream(pstS), &stBuf, 1u, dm__raw_on_written);
  if (DM_SUCCESS != iErrCode)
  {
    dm_free_tag(pstW);
    return DM_CONN_GONE;
  }
  return DM_SUCCESS;
}

uint32 dm_raw_stream_write_queue(dm_in const dm_raw_stream_st *pstS)
{
  size_t uSz = 0u;
  if ((NULL == pstS) || pstS->bClosing)
  {
    return 0u;
  }
  uSz = dm_uv_stream_get_write_queue_size(dm__raw_stream((dm_raw_stream_st *)pstS));
  return (uSz > (size_t)0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32)uSz;
}

void dm_raw_stream_close(dm_io dm_raw_stream_st *pstS)
{
  if ((NULL == pstS) || pstS->bClosing)
  {
    return;
  }
  pstS->bClosing = DM_TRUE;
  pstS->pfOnRead = NULL;
  (void)dm_uv_read_stop(dm__raw_stream(pstS));
  dm_uv_close((dm_uv_handle_st *)dm__raw_stream(pstS), dm__raw_on_closed);
  return;
}

#ifdef __cplusplus
}
#endif
