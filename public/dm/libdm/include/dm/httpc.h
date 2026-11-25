/**
 ******************************************************************************
 * @file    httpc.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   非阻塞异步 HTTP/HTTPS 客户端（DD-01 §6，FW-05，libuv + llhttp + mbedTLS）。
 ******************************************************************************
 */
#ifndef DM_HTTPC_H
#define DM_HTTPC_H

#include <dm.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct dm_httpc_hdr
{
  const char *pcName;
  const char *pcVal;
} dm_httpc_hdr_st;

typedef struct dm_httpc_req
{
  const char *pcMethod;            /* "GET" / "POST" / "PUT" / "DELETE" */
  const char *pcUrl;               /* http:// 或 https:// */
  const dm_httpc_hdr_st *pastHdrs; /* 额外请求头（可 NULL） */
  uint32 u32HdrCount;
  const void *pvBody; /* 请求体（可 NULL） */
  uint32 u32BodyLen;
  const char *pcProxyUrl;     /* 可选出站 HTTP/SOCKS5 代理 */
  uint32 u32ConnectTimeoutMs; /* 默认 10000 */
  uint32 u32ReadTimeoutMs;    /* 默认 30000 */
  uint32 u32MaxRespBytes;     /* 默认 4MB，防爆内存 */
} dm_httpc_req_st;

typedef struct dm_httpc_resp
{
  int iErr;        /* DM_SUCCESS / DM_TIMEOUT / DM_TOO_BIG / DM_IO */
  int iStatusCode; /* HTTP 状态码，传输失败为 0 */
  uint32 u32TtfbMs;
  uint32 u32TotalMs;
  const uint8 *pu8Body;
  uint32 u32BodyLen;
} dm_httpc_resp_st;

typedef void (*dm_httpc_cb)(dm_in void *pvUser, dm_in const dm_httpc_resp_st *pstResp);

/* 返回静态链接的 mbedTLS 版本号（0xMMmmpp00），同时验证 TLS/X509/Crypto/llhttp 静态装配 */
DM_API uint32 dm_httpc_tls_version(void);

/* 附加可信根证书 PEM（供企业内网 PKI 或离线 TLS 单测注入；传 NULL/0 清除），与系统根证书叠加生效 */
DM_API int dm_httpc_set_extra_ca_pem(dm_in const char *pcPem, dm_in uint32 u32Len);

/* 主线程调用，立即返回请求句柄（0 表示参数错或未实现）；回调经总队列在主线程触发 */
DM_API uint64 dm_httpc_send(dm_in const dm_httpc_req_st *pstReq, dm_in dm_httpc_cb pfCb,
                            dm_in void *pvUser);
DM_API int dm_httpc_cancel(dm_in uint64 u64ReqID);

/* ---- 非阻塞流式 HTTP/HTTPS 客户端扩展（DD-20 §1.2.2，零缓冲 SSE 透传与背压） ---- */

typedef void (*dm_httpc_headers_cb)(dm_in void *pvUser, dm_in int iStatusCode,
                                    dm_in const dm_httpc_hdr_st *pastHdrs,
                                    dm_in uint32 u32HdrCount);
typedef void (*dm_httpc_chunk_cb)(dm_in void *pvUser, dm_in const uint8 *pu8Data,
                                  dm_in uint32 u32Len);
typedef void (*dm_httpc_done_cb)(dm_in void *pvUser, dm_in int iErr, dm_in uint32 u32TtfbMs,
                                 dm_in uint32 u32TotalMs);

typedef struct dm_httpc_stream_req
{
  const char *pcMethod;
  const char *pcUrl;
  const dm_httpc_hdr_st *pastHdrs;
  uint32 u32HdrCount;
  const void *pvBody;
  uint32 u32BodyLen;
  const char *pcProxyUrl;          /* 复用出站 HTTP/SOCKS5 代理 */
  uint32 u32ConnectTimeoutMs;      /* 默认 10000ms */
  uint32 u32FirstByteTimeoutMs;    /* 首字节超时，默认 30000ms */
  uint32 u32StreamIdleTimeoutMs;   /* 流式逐片空闲超时，默认 120000ms */
  bool_t bSensitiveHeaders;        /* DM_TRUE = 发送完毕与释放时对请求缓冲执行 dm_secure_zero */
  dm_httpc_headers_cb pfOnHeaders; /* 收到响应头即回调 */
  dm_httpc_chunk_cb pfOnChunk;     /* 零缓冲逐片回调 */
  dm_httpc_done_cb pfOnDone;       /* 终态回调：无论成功/失败/取消，均异步精确触发 1 次 */
  void *pvUser;
} dm_httpc_stream_req_st;

DM_API int dm_httpc_stream_open(dm_in const dm_httpc_stream_req_st *pstReq,
                                dm_out uint64 *pu64ReqID);
DM_API int dm_httpc_stream_pause(dm_in uint64 u64ReqID, dm_in bool_t bPause);
DM_API int dm_httpc_stream_cancel(dm_in uint64 u64ReqID);

#ifdef __cplusplus
}
#endif

#endif /* DM_HTTPC_H */
