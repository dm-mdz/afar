/**
 ******************************************************************************
 * @file    http.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   HTTP 和 WebSocket（详设 2.10）。只在主线程调。
 *
 * HTTP 口只听 127.0.0.1。/ws 是 DM 自己的（WebSocket，升级以后按 link 的规矩走：握手、请求和应答、
 * 推送、反向调用）；别的路径由应用用 dm_http_route 登记。所有请求先过 Host 和 Origin
 * 两道校验（2.3）： Host 只认 127.0.0.1 和 localhost 加本端口；Origin 带了就必须在 dm_cfg
 * 的白名单里，没带的放行。
 ******************************************************************************
 */
#ifndef DM_HTTP_H
#define DM_HTTP_H

#include <dm.h>
#include <dm/link.h>
#include <dm/rpc.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* init 里登记：pcMethod（"GET" 等，NULL = 什么方法都收）+ 路径前缀 -> 处理项。
 * 路由按前缀匹配，最长的前缀优先；同一前缀可以按方法分别登记。字符串只借用，要一直有效。
 * 编号没登记三段回调回 DM_LINK_ERR_NOT_FOUND；重了回 DM_EXISTS；前缀不以 / 开头、或者是 /ws 回
 * DM_INVALID */
DM_API int dm_http_route(in const char *pcMethod, in const char *pcPrefix, in uint uiLinkID);

/* 下面三个只在 before 里有效（消息来源是 DM_SRC_HTTP）；取不到返回 NULL。查询参数不做 % 解码 */
DM_API const char *dm_http_path(in const dm_link_msg_st *pstMsg);
DM_API const char *dm_http_query(in const dm_link_msg_st *pstMsg, in const char *pcKey);
DM_API const char *dm_http_header(in const dm_link_msg_st *pstMsg,
                                  in const char *pcName); /* 名字不分大小写 */

/* 应答：凭据用 dm_reply_take 取（可以存起来晚点答）。同一条连接上按到达的顺序发。
 * pcHeaders 是额外的头，每个以 \r\n 结尾；可为 NULL。连接没了回 DM_CONN_GONE，答过了回 DM_EXISTS */
DM_API int dm_http_reply(in const dm_reply_st *pstReply, in uint uiStatus, in const char *pcType,
                         in const char *pcHeaders, in const void *pvBody, in uint32 u32Len);

/* 发一个文件：DM 在线程池里分块读、写出去，大文件不占内存；支持 Range（206；超出文件大小 416）。
 * 文件打不开当场答 404，返回 DM_NOT_FOUND */
DM_API int dm_http_reply_file(in const dm_reply_st *pstReply, in const char *pcPath,
                              in const char *pcType);

/* 流式分块应答（AUD-03，如 SSE text/event-stream，采用 HTTP/1.1 Transfer-Encoding: chunked）：
 *   begin：写出状态行与响应头（不含 Content-Length）；凭据已答过回 DM_EXISTS，断开回 DM_CONN_GONE；
 *   write：以 chunked 帧写出数据片段（0 长度无操作）；
 *   end  ：写出终止分块 0\r\n\r\n 并释放本次请求，允许同连接继续后续请求。
 * 流式期间对端断开时仍会投递 DM_SRC_HTTP_GONE，后续 write/end 返回 DM_CONN_GONE。 */
DM_API int dm_http_stream_begin(in const dm_reply_st *pstReply, in uint uiStatus,
                                in const char *pcType, in const char *pcHeaders);
DM_API int dm_http_stream_write(in const dm_reply_st *pstReply, in const void *pvData,
                                in uint32 u32Len);
DM_API int dm_http_stream_end(in const dm_reply_st *pstReply);

#ifdef __cplusplus
}
#endif

#endif /* DM_HTTP_H */
