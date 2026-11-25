/**
 ******************************************************************************
 * @file    http_srv.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   轻量本机 HTTP/1.1 流式服务端原语（DD-20 §1.2.1，入 libdm_net.a）：
 *          严格仅监听 127.0.0.1，支持一次性应答、零缓冲 chunked/SSE 流式应答、
 *          RFC 9112 §6.3 头校验、RFC 9112 §7.1 中途异常断流（stream_abort）与背压通知。
 ******************************************************************************
 */
#ifndef DM_HTTP_SRV_H
#define DM_HTTP_SRV_H

#include <dm.h>
#include <dm/httpc.h> /* 复用 dm_httpc_hdr_st */

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct dm_http_srv dm_http_srv_st;
typedef struct dm_http_srv_req dm_http_srv_req_st;

typedef struct dm_http_srv_req_info
{
  const char *pcMethod;            /* "POST" / "GET" / ... */
  const char *pcPath;              /* 含 query，如 "/v1/messages" */
  const dm_httpc_hdr_st *pastHdrs; /* 请求头数组（仅回调期内有效） */
  uint32 u32HdrCount;
  const uint8 *pu8Body; /* 完整请求体（受 u32MaxReqBodyBytes 约束） */
  uint32 u32BodyLen;
} dm_http_srv_req_info_st;

typedef void (*dm_http_srv_req_cb)(dm_in dm_http_srv_req_st *pstReq,
                                   dm_in const dm_http_srv_req_info_st *pstInfo,
                                   dm_in void *pvUser);
typedef void (*dm_http_srv_state_cb)(dm_in dm_http_srv_req_st *pstReq, dm_in bool_t bAborted,
                                     dm_in bool_t bBackpressure, dm_in void *pvUser);

typedef struct dm_http_srv_opt
{
  uint16 u16Port;            /* 0 = 操作系统分配临时空闲端口；严格仅绑定 127.0.0.1 */
  uint32 u32MaxReqBodyBytes; /* 默认 16MB，超限直接回 413 并断开 */
  uint32 u32ReadTimeoutMs;   /* 默认 30000ms，防 Slowloris */
  uint32 u32WriteHighWater;  /* 默认 2MB，写队列超水位触发 bBackpressure=DM_TRUE */
  dm_http_srv_req_cb pfOnReq;
  dm_http_srv_state_cb pfOnState;
  void *pvUser;
} dm_http_srv_opt_st;

DM_API int dm_http_srv_create(dm_in const dm_http_srv_opt_st *pstOpt,
                              dm_out dm_http_srv_st **ppstOut);
DM_API uint16 dm_http_srv_port(dm_in const dm_http_srv_st *pstSrv);
DM_API void dm_http_srv_destroy(dm_io dm_http_srv_st *pstSrv);

DM_API void dm_http_srv_req_set_user(dm_io dm_http_srv_req_st *pstReq, dm_in void *pvCtx);
DM_API void *dm_http_srv_req_get_user(dm_in const dm_http_srv_req_st *pstReq);

/* 一次性应答与零缓冲分块/SSE 流式应答（全部 <= 5 参）：
 * 注意：dm_http_srv_stream_begin 自动写出 Transfer-Encoding: chunked，
 * 内部自动过滤 pcHeaders 中残留的 Content-Length / Transfer-Encoding 头（RFC 9112 §6.3）。 */
DM_API int dm_http_srv_reply(dm_io dm_http_srv_req_st *pstReq, dm_in uint uiStatus,
                             dm_in const char *pcHeaders, dm_in const void *pvBody,
                             dm_in uint32 u32Len);
DM_API int dm_http_srv_stream_begin(dm_io dm_http_srv_req_st *pstReq, dm_in uint uiStatus,
                                    dm_in const char *pcHeaders);
DM_API int dm_http_srv_stream_write(dm_io dm_http_srv_req_st *pstReq, dm_in const void *pvData,
                                    dm_in uint32 u32Len);
DM_API int dm_http_srv_stream_end(dm_io dm_http_srv_req_st *pstReq);

/* RFC 9112 §7.1 中途异常中止：直接关闭下游 TCP 连接且严禁写出 "0\r\n\r\n" 终止块，
 * 确保下游 HTTP/SSE 客户端明确感知流截断而非误判为正常 200 OK EOF */
DM_API void dm_http_srv_stream_abort(dm_io dm_http_srv_req_st *pstReq);

#ifdef __cplusplus
}
#endif

#endif /* DM_HTTP_SRV_H */
