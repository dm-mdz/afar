/**
 ******************************************************************************
 * @file    conn.h
 * @author  MDM
 * @brief   link 层内部：连接对象和几个文件共用的连接函数（link_proto.c、conn.c、http.c、ws.c）。
 *          只给 src/link 下的文件包含，别的地方不许包含。
 ******************************************************************************
 */
#ifndef DM_LINK_CONN_H
#define DM_LINK_CONN_H

#include <dm.h>
#include <dm/link.h>
#include <dm/event.h>
#include <dm/frame.h>
#include <dm/hash.h>
#include <dm/list.h>
#include <dm/json.h>

#include "../dm_internal.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define DM__RX_KEEP (64u * 1024u) /* 接收缓冲平时的大小；临时长大，处理完缩回来 */
#define DM__CONN_BUCKETS 64u
#define DM__PEND_BUCKETS 8u
#define DM__SUB_MAX 32u /* 一条连接最多点名收几类推送 */

typedef struct dm__conn
{
  dm_tcp_st stTcp;            /* 首成员：libuv 回调给的句柄指针就是连接指针 */
  dm__flow_st stFlow;         /* 停读的原因（总队列满了等） */
  dm_list_node_st stNode;     /* 挂在 g_stConnList，拆除时逐个关 */
  dm_timer_st stHsTimer;      /* 握手超时 */
  dm_shutdown_st stShutdown;  /* 答完错误再关：先等写完 */
  dm_hash_table_st stPending; /* 还没答的请求号 */
  uint8 u8Kind;               /* dm__conn_kind_et：二进制口、HTTP、WebSocket */
  void *pvHttp;               /* HTTP 的解析状态和挂着的请求（http.c） */
  void *pvWs;                 /* WebSocket 的解码状态和保活（ws.c） */
  uint32 u32Id;
  uint32 u32Role;
  char acPeerID[DM_PEER_ID_MAX]; /* 握手 peer_id（默认 "local"） */
  bool_t bAuthed;
  bool_t bClosing; /* 已经在关（摘了表、关句柄中） */
  bool_t bDoomed;  /* 答完一个错误、等写完再关：不再处理它发来的任何东西 */
  bool_t bKill;    /* 写失败、对方太慢：已经排了一条去关，不再往它写（不在写的调用栈里同步关） */
  const char *pcKillWhy;
  dm__qitem_st *pstKillItem; /* 接入时预先分好的「去关」那条队列项：排的时候不用再分内存 */
  bool_t bHsTimer;
  bool_t bPushAll;  /* 握手时 push:["*"]：推送全收 */
  uint8 u8SubCount; /* 只收这几类（push:["类名",…]） */
  uint16 au16Sub[DM__SUB_MAX];
  uint8 *pu8Rx;
  uint32 u32RxLen;
  uint32 u32RxCap;
  uint32 u32RxHigh;     /* WS/HTTP 单次读入高水位，确保多片 WS 敏感帧擦清早期更大分片残留 */
  bool_t bHadSensitive; /* AUD-06：曾收过 DM_FRAME_F_SENSITIVE 帧，缩容/关闭前强制擦零 pu8Rx */
} dm__conn_st;

extern dm_list_st g_stConnList;        /* 按连上的先后排（反向调用找「最早连上的」） */
extern dm_hash_table_st g_stConnTable; /* connID -> 连接 */
extern uint32 g_u32ConnNext;

bool_t dm__conn_dead(in const dm__conn_st *pstConn);
dm__conn_st *dm__conn_find(in uint32 u32Id);
void dm__conn_close(io dm__conn_st *pstConn, in const char *pcWhy);
void dm__conn_close_after_write(io dm__conn_st *pstConn);
/* 排一条去关（总队列里的 DM 内部动作）：在别人的调用栈里发现要断时用，调用方回来以后对象都还在 */
void dm__conn_close_later(io dm__conn_st *pstConn, in const char *pcWhy);
int dm__conn_write_parts(io dm__conn_st *pstConn, in const uint8 *pu8Head, in const void *pvRest,
                         in uint32 u32RestLen);
int dm__conn_write(io dm__conn_st *pstConn, in const dm_frame_head_st *pstHead,
                   in const void *pvBody);
int dm__conn_reply(io dm__conn_st *pstConn, in uint uiLinkID, in uint32 u32ReqID, in int iStatus,
                   in const void *pvBody, in uint32 u32Len);
void dm__conn_parse_push(io dm__conn_st *pstConn, in yyjson_val *pstRoot);

int dm__conn_write_raw(io dm__conn_st *pstConn, in const void *pv, in uint32 u32Len);

/* link_proto.c 里的：一帧交给 link（二进制口读出来的、WebSocket 解出来的都走这里）；返回非 0 =
 * 连接开始关了 */
int dm__conn_on_frame(io dm__conn_st *pstConn, in const dm_frame_head_st *pstHead,
                      in uint8 *pu8Body);
void dm__conn_start_link(io dm__conn_st *pstConn); /* 开始走 link 规矩：要握手的起握手定时器 */
uint32 dm__link_frame_max(void);                   /* 握手后单条上限 */

void dm__hs_timer_closed(in dm_handle_st *pstHandle); /* 握手定时器关完再关连接（conn.c） */

/* http.c、ws.c 的钩子 */
void dm__http_conn_new(io dm__conn_st *pstConn);
void dm__http_conn_data(io dm__conn_st *pstConn, in const uint8 *pu8, in uint32 u32Len);
void dm__http_conn_gone(io dm__conn_st *pstConn);
void dm__ws_conn_data(io dm__conn_st *pstConn, in const uint8 *pu8, in uint32 u32Len);
void dm__ws_conn_gone(io dm__conn_st *pstConn);
uint32 dm__ws_head(out uint8 *pu8Out, in uint8 u8Op,
                   in uint64 u64Len); /* 核心发的帧头（不带掩码），返回长度 */
int dm__ws_upgrade(io dm__conn_st *pstConn, in const char *pcKey, in const uint8 *pu8Rest,
                   in uint32 u32Rest);
void dm__sha1(in const void *pv, in size_t uLen, out uint8 au8Out[20]);
void dm__base64(in const uint8 *pu8, in size_t uLen, out char *pcOut);
void dm__ws_accept_key(in const char *pcKey, out char acOut[32]);
void dm__http_config(in const dm_cfg_st *pstCfg);
void dm__http_routes_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* DM_LINK_CONN_H */
