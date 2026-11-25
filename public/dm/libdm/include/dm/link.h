/**
 ******************************************************************************
 * @file    link.h
 * @brief   dm 通信层（link）：处理项登记、三段回调（before/work/after）、往别的处理项交、线程池。
 *
 * 一条消息的路（详设 2.8）：
 *   ① 不论从哪来（连接、定时器、事件、子进程、别的模块 dispatch），先进总队列；
 *   ② 事件循环每轮取一条，交给目标处理项的 before（主线程）；
 *   ③ 要慢活就在 before 里 dm_link_work_submit，work 在线程池跑，跑完回主线程调 after；
 *      没有慢活的 before 干完直接返回。
 * 连接收在 link 层里，业务不碰（2.3）：应答用 dm_reply_take 取的凭据（dm/rpc.h）。
 ******************************************************************************
 */
#ifndef DM_LINK_H
#define DM_LINK_H

#include <stddef.h>
#include <dm.h>
#include <dm/type.h>
#include <dm/event.h>
#include <dm/frame.h>
#include <dm/pool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 枚举:由 DM_LINKID_MAP 生成(第二条参数 _ 在枚举中忽略) */
typedef enum
{
  DM_LINKID_INVALID = 0,
#define XX(code, _) DM_LINKID_##code,
  DM_LINKID_MAP(XX)
#undef XX
} dm_linkid_et;

/*
 * 交给 before 的消息。负载只在 before 里有效，要留给 work 就拷贝。
 */
typedef struct dm_link_msg
{
  uint uiLinkID;            /* 目标处理项的编号 */
  uint8 *pu8Data;           /* 负载 */
  uint32 u32Len;            /* 负载长度 */
  dm_link_work_cb pfWork;   /* 本编号登记的 work，NULL = 无 */
  dm_link_after_cb pfAfter; /* 本编号登记的 after，NULL = 无 */
  uint8 u8Src;              /* 从哪来，见 dm_src_et */
  uint8 u8Flags;            /* 帧标志位（含 DM_FRAME_F_SENSITIVE） */
  uint8 u8Rsv[2];
  uint32 u32Role;    /* 连接来的：对方的角色位；别的来源是 0 */
  uint64 u64Tag;     /* 登记时调用方给的标记（子进程、定时器、反向调用、事件号） */
  uint64 u64TraceID; /* 跨跳全链路追踪号（外部入口分配，dispatch/event/after 透传） */
  uint16 u16Depth;   /* 当前派发跳数（根入口为 0，每跳 +1，> DM_MAX_DISPATCH_DEPTH 熔断） */
  uint16 u16Rsv;
  int iStatus;  /* 反向调用的应答、子进程的「超长行」带的结果；别的来源是 0 */
  void *pvPriv; /* DM 私有：应答要的连接和请求号、HTTP 请求的上下文。业务不碰，用 dm_reply_take
                   取凭据 */
  dm_pool_lane_et eLane; /* 本处理项绑定的 dm_pool 泳道（默认 DM_LANE_DEFAULT） */
} dm_link_msg_st;

/* 无状态直通 (NULL, work, after) 传给 pfWork / pfAfter 的只读载荷与结果包（DD-01 §3.3） */
typedef struct dm_stateless_job
{
  uint uiLinkID;
  const uint8 *pu8Data; /* 零拷贝指向不可变原子 qbuf，work/after 期间有效 */
  uint32 u32Len;
  uint8 u8Src;
  uint8 u8Flags; /* 帧标志位（含 DM_FRAME_F_SENSITIVE） */
  uint8 u8Rsv[2];
  uint32 u32Role;
  uint64 u64Tag;
  uint64 u64TraceID;
  uint16 u16Depth;
  uint16 u16Rsv;
  int iResultErr;      /* work 填：业务结果码（默认 DM_SUCCESS） */
  void *pvResult;      /* work 填（可 NULL）：pfAfter==NULL 且为请求时由框架回发并 dm_free_tag */
  uint32 u32ResultLen; /* work 填：pvResult 字节数 */
  bool_t bHasReply;    /* 是否携带有效应答凭据 */
  uint64
      au64Reply[4]; /* 8 字节对齐的不透明应答凭据缓冲（严禁直接强转，由 libdm_net memcpy 存取） */
} dm_stateless_job_st;

/* 消息从哪来 */
typedef enum dm_src
{
  DM_SRC_LOCAL = 0,  /* 模块自己新拼了一条 dm_link_dispatch 过来的（原样转交的保留原来的来源） */
  DM_SRC_LINK,       /* 二进制口（dmcmd） */
  DM_SRC_WS,         /* WebSocket（页面、壳） */
  DM_SRC_HTTP,       /* HTTP 请求 */
  DM_SRC_HTTP_GONE,  /* 一个还没答的 HTTP 请求作废了（对方断开） */
  DM_SRC_CHILD_OUT,  /* 子进程输出了一行 / 一块 */
  DM_SRC_CHILD_EXIT, /* 子进程结束了（退出通知到了，并且输出读完了） */
  DM_SRC_TIMER,      /* 定时器到点 */
  DM_SRC_EVENT,      /* 订阅的事件（u64Tag 是事件号）。DM 自己的「都初始化完了」「连接建立 /
                        断开」也是它 */
  DM_SRC_REPLY       /* 反向调用的应答回来了 */
} dm_src_et;

/* ---- 口 ---------------------------------------------------------------- */
/* 起口（只听 127.0.0.1）。dm_main_ex 调；口已经起了再调直接返回成功。登记表不碰（DM-06） */
DM_API int dm_link_init(void);

/* 关口和所有连接 */
DM_API void dm_link_fini(void);

/* ---- 处理项 ------------------------------------------------------------ */
/*
 * 按编号登记三段回调（默认 DM_LANE_DEFAULT）。支持对称三形态：(before,NULL,NULL)、
 * (before,work,after)、(NULL,work,after)；(NULL,NULL,*) 非法。
 * 只登记三段的编号，连接上来的帧叫不到（要再 dm_iface_register，dm/rpc.h）。
 * 同一编号重复登记是编程错误：调试版断言；发布版覆盖旧的、告警并返回 DM_LINK_ERR_REREGIST。
 * dm_main_ex 之前调回 DM_LINK_ERR_STATE。
 */
DM_API int dm_link_register_handler(dm_in uint uiLinkID, dm_in dm_link_before_cb pfBefore,
                                    dm_in dm_link_work_cb pfWork, dm_in dm_link_after_cb pfAfter);

/* 扩展登记：支持泳道指定与对称三形态 (before,NULL,NULL) / (NULL,work,after) / (before,work,after)
 */
DM_API int dm_link_register_handler_ex(dm_in uint uiLinkID, dm_in dm_pool_lane_et eLane,
                                       dm_in dm_link_before_cb pfBefore,
                                       dm_in dm_link_work_cb pfWork,
                                       dm_in dm_link_after_cb pfAfter);

/* 查询某 linkID 是否已登记处理项（仅主线程） */
DM_API bool_t dm_link_has_handler(dm_in uint uiLinkID);

/* ---- 交给别的处理项 ---------------------------------------------------------- */
/*
 * 放进总队列，轮到了交给 uiLinkID 的 before（详设 2.8）。不当场调对方，所以 before、after
 * 里都能调， 也不会一层套一层；在 work 里调回 DM_WRONG_THREAD（要回到 after）。
 * 负载拷一份；u8Src、u32Role、u64Tag、iStatus
 * 照抄：把收到的消息原样转交，下一段看得出最早从哪来； 自己新拼的消息 u8Src 填
 * DM_SRC_LOCAL。pvPriv 不带（一个请求只归第一段答）。 编号没登记的当场回
 * DM_LINK_ERR_NOT_FOUND；拆除阶段回 DM_SHUTTING_DOWN。
 */
DM_API int dm_link_dispatch(dm_in uint uiLinkID, dm_in const dm_link_msg_st *pstMsg);

/* ---- 线程池 ------------------------------------------------------------ */
/*
 * 在 before 里调：把 pstMsg 对应的 work 交给线程池，pvArg 是调用方填的参数
 * （调用方分配、after 里自己释放）。跑完回主线程调 pstMsg 对应的 after。
 * 主线程调；拆除阶段回 DM_SHUTTING_DOWN。排着队还没开始的在拆除时被取消，after 收到
 * DM_ECANCELED。
 */
DM_API int dm_link_work_submit(dm_in dm_link_msg_st *pstMsg, dm_in void *pvArg);

/* 向指定泳道提交慢活（绕过 pstMsg->eLane 默认泳道） */
DM_API int dm_link_work_submit_lane(dm_in dm_pool_lane_et eLane, dm_in dm_link_msg_st *pstMsg,
                                    dm_in void *pvArg);

/* 无状态直通 (NULL, work, after) 在主线程 pfAfter 内合法发送 RPC 应答的公开辅助接口；
 * 发送后清零 pstJob->bHasReply 防重发；非请求或已应答返回 DM_NOT_FOUND */
DM_API int dm_stateless_reply_send(dm_io dm_stateless_job_st *pstJob, dm_in int iStatus,
                                   dm_in const void *pvBody, dm_in uint32 u32Len);

/* 跨线程安全投递消息并唤醒主循环（AUD-11：供常驻外部线程在不暴露 <uv.h> 下安全唤醒主循环）。
 * 任意线程可调用；积压超过总队列 64MB 背压上限返回 DM_BUSY；拆除期或未启动返回 DM_SHUTTING_DOWN */
DM_API int dm_queue_post_async(dm_in uint uiLinkID, dm_in const void *pvData, dm_in uint32 u32Len);

/* ---- 双向 Peer 与移动端原生 fd 纳管（DD-18 §1.1, DD-19 §6.1，零 <uv.h>） ---- */

/* 跨节点 Peer HELLO 验签回调（返回 DM_SUCCESS 放行并将 dm_malloc_tag 分配的应答 JSON 写入
 * *ppcAckJson，由 libdm 发送完毕后 dm_secure_zero + dm_free_tag；返回负错码则拒连） */
typedef int (*dm_peer_hello_verify_cb)(dm_in const char *pcConnPeerID, dm_in const uint8 *pu8Body,
                                       dm_in uint32 u32Len, dm_in void *pvCtx,
                                       dm_out char **ppcAckJson);

/* 已认证 DM_ROLE_PEER 连接收到对端入站 DM_FRAME_T_PUSH 帧负载 */
typedef struct dm_peer_push_msg
{
  const char *pcPeerID;
  uint16 u16Topic;
  uint64 u64Key;
  const uint8 *pu8Val;
  uint32 u32Len;
} dm_peer_push_msg_st;

typedef void (*dm_peer_push_cb)(dm_in const dm_peer_push_msg_st *pstMsg, dm_in void *pvCtx);

/* 主动出站 Peer 发送首帧签名 HELLO 的打包入参 */
typedef struct dm_peer_hello_req
{
  const char *pcPeerID;
  uint64 u64ConnGen;
  const void *pvBody;
  uint32 u32Len;
  uint uiReplyLinkID;
  uint64 u64Tag;
  uint32 u32TimeoutMs;
} dm_peer_hello_req_st;

DM_API int dm_link_set_peer_hello_verifier(dm_in dm_peer_hello_verify_cb pfVerify,
                                           dm_in void *pvCtx);
DM_API int dm_link_set_peer_push_cb(dm_in dm_peer_push_cb pfPush, dm_in void *pvCtx);

/* 纳管外部已完成前导校验的原生流描述符为 DM_ROLE_PEER 连接：
 * 返回 DM_SUCCESS 时 tFd 所有权移交 libdm；返回错误码时调用方仍保有 tFd 所有权。 */
DM_API int dm_link_adopt_peer_fd(dm_in dm_peer_fd_t tFd, dm_in const char *pcPeerID,
                                 dm_in uint64 u64ConnGen, dm_in bool_t bIsPipe,
                                 dm_in bool_t bIsInitiator);

DM_API int dm_link_peer_send_hello(dm_in const dm_peer_hello_req_st *pstReq);

DM_API int dm_link_peer_mark_ready(dm_in const char *pcPeerID, dm_in uint64 u64ConnGen,
                                   dm_in const char *pcRemotePushJson);

/* 移动端最小权限连接（DM_ROLE_MOBILE）验签入参/出参与纳管（DD-19 §6.1） */
typedef struct dm_mobile_hello_in
{
  const char *pcHintPeerID; /* Path A 为预设 peer_id；Path B LAN TLS 可为 NULL（延迟绑定） */
  const uint8 *pu8Body;
  uint32 u32Len;
  const char *pcResume; /* 由 libdm 预计算："ok" | "full" | "none" */
  uint64 u64CurSeq;     /* 由 dm_push_seq() 预采集 */
} dm_mobile_hello_in_st;

typedef struct dm_mobile_hello_out
{
  char *pcAckJson; /* dm_malloc_tag 分配，由 libdm 发送后擦零释放 */
  char acVerifiedPeerID[DM_PEER_ID_MAX];
  uint64 u64VerifiedConnGen;
} dm_mobile_hello_out_st;

typedef int (*dm_mobile_hello_verify_cb)(dm_in const dm_mobile_hello_in_st *pstIn,
                                         dm_in void *pvCtx, dm_out dm_mobile_hello_out_st *pstOut);

DM_API int dm_link_set_mobile_hello_verifier(dm_in dm_mobile_hello_verify_cb pfVerify,
                                             dm_in void *pvCtx);

DM_API int dm_link_adopt_mobile_fd(dm_in dm_peer_fd_t tFd, dm_in const char *pcHintPeerID,
                                   dm_in uint64 u64ConnGen, dm_in bool_t bIsPipe);

/* 主动断开指定 peer_id 且角色匹配 u32RoleMask 的所有活跃连接（返回断开条数；仅主线程） */
DM_API uint32 dm_link_disconnect_peer(dm_in const char *pcPeerID, dm_in uint32 u32RoleMask);

/* 查询消息来源连接已认证的 peer_id（连接有效且已认证返回 DM_SUCCESS，否则返回 DM_NOT_FOUND） */
DM_API int dm_link_msg_peer_id(dm_in const dm_link_msg_st *pstMsg, dm_out char *pcBuf,
                               dm_in size_t uCap);

struct dm_ext_hooks;
DM_API const struct dm_ext_hooks *dm_net_hooks(void);

#ifdef __cplusplus
}
#endif

#endif /* DM_LINK_H */
