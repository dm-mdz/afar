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

typedef enum dm__stream_kind
{
  DM__STREAM_TCP = 0, /* 127.0.0.1 loopback TCP（三端支持，Windows 唯一路径） */
  DM__STREAM_PIPE = 1 /* POSIX 0600 Unix Domain Socket（macOS/Linux 本地侧） */
} dm__stream_kind_et;

typedef struct dm__conn
{
  /* 首成员为 libuv 流联合体（C11 §6.7.2.1p16 保证联合体指针与首成员指针地址一致） */
  union
  {
    dm_uv_tcp_st stTcp;
    dm_uv_pipe_st stPipe;
  } uStream;
  uint8 u8StreamKind;           /* dm__stream_kind_et */
  bool_t bPeerAdopted;          /* DM_TRUE = 经 dm_link_adopt_peer_fd() 纳管 */
  bool_t bPeerInitiator;        /* DM_TRUE = 主动拨号方（未认证前仅收 HELLO REP） */
  bool_t bMobileAdopted;        /* DM_TRUE = 经 dm_link_adopt_mobile_fd() 纳管 */
  uint64 u64ConnGen;            /* 单调递增连接代数 */
  bool_t bPeerHelloSent;        /* 主动拨号方已发首帧 HELLO，等待对端 REP */
  uint32 u32PeerHelloReq;       /* 主动拨号方 HELLO 请求号 */
  uint uiPeerHelloReply;        /* 收到对端 HELLO REP 后回投的处理项 */
  uint64 u64PeerHelloTag;       /* 透传给 uiPeerHelloReply 的上下文标签 */
  dm__flow_st stFlow;           /* 停读的原因（总队列满了等） */
  dm_list_node_st stNode;       /* 挂在 g_stConnList，拆除时逐个关 */
  dm_uv_timer_st stHsTimer;     /* 握手超时 */
  dm_uv_shutdown_st stShutdown; /* 答完错误再关：先等写完 */
  dm_hash_table_st stPending;   /* 还没答的请求号 */
  uint8 u8Kind;                 /* dm__conn_kind_et：二进制口、HTTP、WebSocket */
  void *pvHttp;                 /* HTTP 的解析状态和挂着的请求（http.c） */
  void *pvWs;                   /* WebSocket 的解码状态和保活（ws.c） */
  uint32 u32Id;
  uint32 u32Role;
  char acPeerID[DM_PEER_ID_MAX];     /* 握手 peer_id（默认 "local"；移动端须验签后绑定） */
  char acHintPeerID[DM_PEER_ID_MAX]; /* 移动端 Path A 纳管提示 peer_id（验签前不参与顶替判定） */
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

static inline dm_uv_stream_st *dm__conn_stream(dm_io dm__conn_st *pstConn)
{
  return (DM__STREAM_PIPE == pstConn->u8StreamKind) ? (dm_uv_stream_st *)&pstConn->uStream.stPipe
                                                    : (dm_uv_stream_st *)&pstConn->uStream.stTcp;
}

extern dm_list_st g_stConnList;        /* 按连上的先后排（反向调用找「最早连上的」） */
extern dm_hash_table_st g_stConnTable; /* connID -> 连接 */
extern uint32 g_u32ConnNext;

bool_t dm__conn_dead(dm_in const dm__conn_st *pstConn);
dm__conn_st *dm__conn_find(dm_in uint32 u32Id);
void dm__conn_close(dm_io dm__conn_st *pstConn, dm_in const char *pcWhy);
void dm__conn_close_after_write(dm_io dm__conn_st *pstConn);
/* 排一条去关（总队列里的 DM 内部动作）：在别人的调用栈里发现要断时用，调用方回来以后对象都还在 */
void dm__conn_close_later(dm_io dm__conn_st *pstConn, dm_in const char *pcWhy);
int dm__conn_write_parts(dm_io dm__conn_st *pstConn, dm_in const uint8 *pu8Head,
                         dm_in const void *pvRest, dm_in uint32 u32RestLen);
int dm__conn_write(dm_io dm__conn_st *pstConn, dm_in const dm_frame_head_st *pstHead,
                   dm_in const void *pvBody);
int dm__conn_reply_ex(dm_io dm__conn_st *pstConn, dm_in uint uiLinkID, dm_in uint32 u32ReqID,
                      dm_in int iStatus, dm_in uint8 u8ExtraFlag, dm_in const void *pvBody,
                      dm_in uint32 u32Len);
int dm__conn_reply(dm_io dm__conn_st *pstConn, dm_in uint uiLinkID, dm_in uint32 u32ReqID,
                   dm_in int iStatus, dm_in const void *pvBody, dm_in uint32 u32Len);
void dm__conn_parse_push(dm_io dm__conn_st *pstConn, dm_in yyjson_val *pstRoot);
void dm__conn_parse_push_arr(dm_io dm__conn_st *pstConn, dm_in yyjson_val *pstArr);
dm__conn_st *dm__conn_find_peer_preauth(dm_in const char *pcPeerID, dm_in uint64 u64ConnGen);
void dm__conn_notify_peer_hello(dm_io dm__conn_st *pstConn, dm_in int iStatus, dm_in uint8 u8Flags,
                                dm_in const uint8 *pu8Body, dm_in uint32 u32Len);

int dm__conn_write_raw(dm_io dm__conn_st *pstConn, dm_in const void *pv, dm_in uint32 u32Len);

/* link_proto.c 里的：一帧交给 link（二进制口读出来的、WebSocket 解出来的都走这里）；返回非 0 =
 * 连接开始关了 */
int dm__conn_on_frame(dm_io dm__conn_st *pstConn, dm_in const dm_frame_head_st *pstHead,
                      dm_in uint8 *pu8Body);
void dm__conn_start_link(dm_io dm__conn_st *pstConn); /* 开始走 link 规矩：要握手的起握手定时器 */
uint32 dm__link_frame_max(void);                      /* 握手后单条上限 */

void dm__hs_timer_closed(dm_in dm_uv_handle_st *pstHandle); /* 握手定时器关完再关连接（conn.c） */

/* http.c、ws.c 的钩子 */
void dm__http_conn_new(dm_io dm__conn_st *pstConn);
void dm__http_conn_data(dm_io dm__conn_st *pstConn, dm_in const uint8 *pu8, dm_in uint32 u32Len);
void dm__http_conn_gone(dm_io dm__conn_st *pstConn);
void dm__ws_conn_data(dm_io dm__conn_st *pstConn, dm_in const uint8 *pu8, dm_in uint32 u32Len);
void dm__ws_conn_gone(dm_io dm__conn_st *pstConn);
uint32 dm__ws_head(dm_out uint8 *pu8Out, dm_in uint8 u8Op,
                   dm_in uint64 u64Len); /* 核心发的帧头（不带掩码），返回长度 */
int dm__ws_upgrade(dm_io dm__conn_st *pstConn, dm_in const char *pcKey, dm_in const uint8 *pu8Rest,
                   dm_in uint32 u32Rest);
void dm__sha1(dm_in const void *pv, dm_in size_t uLen, dm_out uint8 au8Out[20]);
void dm__base64(dm_in const uint8 *pu8, dm_in size_t uLen, dm_out char *pcOut);
void dm__ws_accept_key(dm_in const char *pcKey, dm_out char acOut[32]);
void dm__http_config(dm_in const dm_cfg_st *pstCfg);
void dm__http_routes_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* DM_LINK_CONN_H */
