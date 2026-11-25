/**
 ******************************************************************************
 * @file    rpc.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   接口表、应答（详设 2.5）。全部只在主线程调。推送、反向调用见 P-02 的后半。
 *
 * 谁能从外面叫到一个处理项（DM 在进总队列之前查）：
 *   只调了 dm_link_register_handler 的：连接上来的帧叫不到（回 DM_LINK_ERR_NOT_FOUND）；
 *   又调了 dm_iface_register 的：能，但连接的角色位要和接口的 u32Roles 有交集，否则回 DM_PERM。
 ******************************************************************************
 */
#ifndef DM_RPC_H
#define DM_RPC_H

#include <dm.h>
#include <dm/link.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_IFACE_MUTATE (1u << 0)    /* 会改东西。DM 自己不看它，只在接口清单里带给客户端 */
#define DM_IFACE_ADVANCED (1u << 1)  /* 高级/高危接口 */
#define DM_IFACE_DIAG (1u << 2)      /* 诊断接口 */
#define DM_IFACE_TEST_ONLY (1u << 3) /* 仅测试用接口 */
#define DM_IFACE_STATELESS (1u << 4) /* 无状态直通接口 */
#define DM_IFACE_SENSITIVE (1u << 5) /* 敏感载荷接口：客户端须自动置 DM_FRAME_F_SENSITIVE */

#define DM_ROLE_UI (1u << 0)
#define DM_ROLE_SHELL (1u << 1)
#define DM_ROLE_CLI (1u << 2)
#define DM_ROLE_DIAG (1u << 3)
#define DM_ROLE_PEER (1u << 4)
#define DM_ROLE_MOBILE (1u << 5)
#define DM_ROLE_ALL 0xFFFFFFFFu

/* ① 接口表：一个接口 = 一个处理项 + 一个名字 */
typedef struct dm_iface
{
  uint uiLinkID;
  const char *pcName; /* 例："demo.echo"。页面按名字调，dmcmd 先取清单再按编号调 */
  uint32 u32Roles;    /* 谁能调：角色位的并集。连接的位和它没有交集就回 DM_PERM */
  uint32 u32Flags;    /* DM_IFACE_MUTATE 等 */
  const char *pcDesc; /* 一句话说明，dmcmd 的帮助里显示 */
  const char *pcArgs; /* 参数说明（JSON Schema 文本），dmcmd 据此解析命令行；可为 NULL */
} dm_iface_st;

/* 只在 init 里调（别的时候回 DM_INVALID）。字符串只借用，要一直有效（一般是字面量）。
 * 这个编号要先用 dm_link_register_handler 登记过，没登记回 DM_LINK_ERR_NOT_FOUND；
 * 名字或编号和已有的重了回 DM_EXISTS */
DM_API int dm_iface_register(dm_in const dm_iface_st *pstIface);

/* ② 应答：before 里取一张「应答凭据」，可以当场答，也可以存起来晚点答。
 *    凭据里是 DM 内部的连接编号、请求号，业务只拷贝、不读（连接收在 link 层里，2.3） */
typedef struct dm_reply
{
  uint32 au32Priv[4];
} dm_reply_st;

/* 两张是不是同一个请求 */
DM_API bool_t dm_reply_equal(dm_in const dm_reply_st *pstA, dm_in const dm_reply_st *pstB);

/* 对方不要应答（请求号是 0）、或者这条消息不是连接来的：回 DM_NOT_FOUND */
DM_API int dm_reply_take(dm_in const dm_link_msg_st *pstMsg, dm_out dm_reply_st *pstReply);

/* 连接已经断了：返回 DM_CONN_GONE，什么也不发，记一行 DEBUG；同一个请求答第二次回 DM_EXISTS；
 * 体超过单条上限回 DM_TOO_BIG，没答，可以改答一个错误 */
DM_API int dm_reply_send(dm_in const dm_reply_st *pstReply, dm_in int iStatus,
                         dm_in const void *pvBody, dm_in uint32 u32Len);

/* ③ 推送：带全局序号，进环形缓冲，发给握手时说要收推送的连接（详设 2.5.2） */
/* init 里登记推送的类（0~0xFFFF）。名字只借用，要一直有效（一般是字面量） */
DM_API int dm_push_topic(dm_in uint uiTopic, dm_in const char *pcName);
/* 类没登记过回 DM_NOT_FOUND；值加 18 字节前缀超过单条上限回 DM_TOO_BIG；这两种序号都不动 */
DM_API int dm_push(dm_in uint uiTopic, dm_in uint64 u64Key, dm_in const void *pvVal,
                   dm_in uint32 u32Len);
/* 按 peer_id 单播推送：不进全局广播重放环（seq=0），仅发给已握手、peer_id 匹配且订阅该类的连接；
 * 若无该 peer_id 的在线已握手连接，返回 DM_CONN_GONE */
DM_API int dm_push_peer(dm_in const char *pcPeerID, dm_in uint uiTopic, dm_in uint64 u64Key,
                        dm_in const void *pvVal, dm_in uint32 u32Len);
DM_API uint64
dm_push_seq(void); /* 当前最新的序号：拼快照时一起带给页面。只许主线程，别的线程回 0 */

/* ④ 流式推送：不带序号、不进缓冲，同一 (类, 键) 约 50ms
 * 拼起来发一次，写不动就丢，不补（概设 3.1）。 一片就超过单条上限回 DM_TOO_BIG；0 长度什么也不做 */
DM_API int dm_push_stream(dm_in uint uiTopic, dm_in uint64 u64Key, dm_in const void *pvData,
                          dm_in uint32 u32Len);
DM_API int dm_push_stream_end(dm_in uint uiTopic,
                              dm_in uint64 u64Key); /* 把还没发的立即发掉，然后结束这个键 */

/* ⑤ 反向调用请求体（零初始化友好）：核心调客户端。
 * 发给最早连上、带 u32Role 的那条连接（pcPeerID 非 NULL 时仅匹配该 peer_id）；uiFunc
 * 为对端功能编号。 应答进总队列交给 uiReplyLinkID：u8Src = DM_SRC_REPLY，u64Tag
 * 为调用方透传标签，iStatus 为对方结果； 超时或断连时同样单次投递 DM_TIMEOUT /
 * DM_CONN_GONE；当场发送失败直接用返回值报。 u8ExtraFlags 允许透传额外帧标志（如
 * DM_FRAME_F_SENSITIVE）；u32TimeoutMs 为 0 表示不超时。 */
typedef struct dm_call_req
{
  const char *pcPeerID; /* 目标 peer_id（NULL 表示按 u32Role 选最早连上的连接） */
  uint32 u32Role;       /* 目标角色位 */
  uint uiFunc;          /* 对端功能编号 */
  uint8 u8ExtraFlags;   /* 额外帧标志（普通调用填 0，敏感帧填 DM_FRAME_F_SENSITIVE） */
  const void *pvBody;   /* 请求体（可 NULL） */
  uint32 u32Len;        /* 请求体字节数 */
  uint uiReplyLinkID;   /* 本地应答回调 linkID */
  uint64 u64Tag;        /* 调用方透传标签 */
  uint32 u32TimeoutMs;  /* 超时毫秒（0 = 不超时） */
} dm_call_req_st;

DM_API int dm_call(dm_in const dm_call_req_st *pstReq);
/* 按 peer_id 定向反向调用：覆盖 pstReq->pcPeerID（传 NULL 等价于 dm_call） */
DM_API int dm_call_peer(dm_in const char *pcPeerID, dm_in const dm_call_req_st *pstReq);
/* 扩展反向调用：覆盖 pcPeerID 与 u8ExtraFlags（如 DM_FRAME_F_SENSITIVE） */
DM_API int dm_call_peer_ex(dm_in const char *pcPeerID, dm_in uint8 u8ExtraFlags,
                           dm_in const dm_call_req_st *pstReq);

#ifdef __cplusplus
}
#endif

#endif /* DM_RPC_H */
