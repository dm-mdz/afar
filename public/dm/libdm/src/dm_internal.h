/**
 ******************************************************************************
 * @file    dm_internal.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   DM 内部各文件之间共用的东西：阶段、总队列、在途 work、停读。DM 以外不许包含。
 ******************************************************************************
 */
#ifndef DM_INTERNAL_H
#define DM_INTERNAL_H

#include <dm/module.h> /* dm_cfg_st */
#include <dm.h>
#include <dm/event.h> /* DM_PEER_ID_MAX */
#include <dm/link.h>
#include <dm/list.h>
#include <dm/check.h>
#include <dm/pool.h>
#include <stdatomic.h>
#include <dm/log.h>
#include "uv_adapter.h"
#include "infra/fs/fs_internal.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* ============================ 阶段（2.2） ============================ */
typedef enum dm__phase
{
  DM__PHASE_NONE = 0, /* 不在 dm_main_ex 里（白盒测试直接调接口的情形） */
  DM__PHASE_INIT,     /* 各模块的 pfInit 正在跑 */
  DM__PHASE_RUN,      /* 运行 */
  DM__PHASE_DRAIN,    /* 收尾 */
  DM__PHASE_TEARDOWN, /* 拆除 */
  DM__PHASE_END       /* 结束：pfFini 正在跑 */
} dm__phase_et;

dm__phase_et dm__phase(void);

/* DM 自己只许主线程调的接口开头用：调错线程回 DM_WRONG_THREAD 并记 ERROR，发布版也查 */
#define DM__CHECK_LOOP()                                                                           \
  do                                                                                               \
  {                                                                                                \
    if (!dm_in_loop())                                                                             \
    {                                                                                              \
      dm__wrong_thread(__func__);                                                                  \
      return DM_WRONG_THREAD;                                                                      \
    }                                                                                              \
  } while (0)

void dm__wrong_thread(in const char *pcFunc);

#define DM__REPLY_MAGIC 0x52504C59u /* "RPLY"：凭据由 dm_reply_take 填充 */

/* ============================ 总队列（2.8） ============================ */

/* 负载：引用计数，事件的几个订阅者共用一份 */
typedef struct dm__qbuf
{
  _Atomic uint32 u32Refs;
  uint32 u32Len;
  uint8 u8Flags; /* 帧标志位（含 DM_FRAME_F_SENSITIVE） */
  uint8 au8Pad[7];
  uint8 au8Data[];
} dm__qbuf_st;

dm__qbuf_st *dm__qbuf_new(in const void *pvData,
                          in uint32 u32Len); /* 拷一份；u32Len 为 0 返回 NULL */
dm__qbuf_st *dm__qbuf_new_ex(in const void *pvData, in uint32 u32Len, in uint8 u8Flags);
void dm__qbuf_get(io dm__qbuf_st *pstBuf);
void dm__qbuf_put(io dm__qbuf_st *pstBuf);

/* 应答要的：哪条连接、哪个请求（业务经 dm_reply_take 拿凭据，不读这个） */
typedef struct dm__reply_ctx
{
  uint32 u32ConnID;
  uint32 u32ReqID;
  uint8 u8Src;
} dm__reply_ctx_st;

typedef struct dm__qitem dm__qitem_st;
typedef void (*dm__qrun_cb)(
    io dm__qitem_st *pstItem); /* DM 内部的动作（开始收尾等），不走 before */
typedef bool_t (*dm__qstale_cb)(
    io dm__qitem_st *pstItem); /* 取出时再查一次：返回真就丢掉不交（定时器用） */

struct dm__qitem
{
  dm_list_node_st stNode;
  dm__qrun_cb pfRun;     /* 非 NULL：DM 内部动作，下面的回调都不用 */
  dm__qstale_cb pfStale; /* 非 NULL：交出之前先问它 */
  void *pvCtx;           /* 给 pfRun / pfStale 的 */
  uint uiLinkID;
  dm_link_before_cb pfBefore; /* 目标的三段（登记的处理项，或者事件的订阅者） */
  dm_link_work_cb pfWork;
  dm_link_after_cb pfAfter;
  uint8 u8Src;
  uint8 u8Flags;
  bool_t bExternal; /* 外面来的（连接、HTTP、子进程）：占总队列的额度 */
  uint32 u32Role;
  uint64 u64Tag;
  int iStatus;
  void *pvPriv;
  dm__qbuf_st *pstBuf;      /* 负载，可以为 NULL */
  uint32 u32Aux;            /* 给 pfStale 用（定时器：到点时的代数） */
  dm__reply_ctx_st stReply; /* 连接来的请求：交给 before 时 pvPriv 指向它 */
  dm_pool_lane_et eLane;    /* 目标泳道（默认 DM_LANE_DEFAULT） */
  uint64 u64TraceID;        /* 链路追踪号 */
  uint16 u16Depth;          /* 传播跳数 */
};

int dm__queue_init(void);                      /* dm_main_ex 起来时调 */
void dm__queue_close(void);                    /* 结束时调：剩下的丢掉、关 idle 句柄 */
void dm__queue_set_max(in uint32 u32Max);      /* 外面来的最多占多少字节 */
dm__qitem_st *dm__qitem_new(void);             /* 清零的一条；失败返回 NULL */
void dm__qitem_free(io dm__qitem_st *pstItem); /* 连同负载的引用一起放掉 */
int dm__queue_push(
    io dm__qitem_st *pstItem); /* 接管这一条；拆除以后回 DM_SHUTTING_DOWN（条目由它放掉） */
int dm__queue_run(in dm__qrun_cb pfRun, in void *pvCtx); /* 放一条 DM 内部动作 */
size_t dm__queue_len(void);
bool_t dm__queue_idle_on(void);   /* idle 句柄开着没有（白盒测试看）*/
uint64 dm__queue_ext_bytes(void); /* 外面来的占了多少字节 */
void dm__queue_ext_add(in uint32 u32Bytes);
void dm__queue_ext_release(in uint32 u32Bytes);
uint64 dm__queue_room(void); /* 离上限还有多少字节（子进程按它定一次读多少） */
void dm__queue_stop_async(void);
uint64 dm__trace_next(void);
void dm__dispatch_ctx_get(out bool_t *pbInDispatch, out uint16 *pu16Depth, out uint64 *pu64TraceID);
void dm__dispatch_ctx_set(in bool_t bInDispatch, in uint16 u16Depth, in uint64 u64TraceID);
int dm__dispatch_next_hop(in bool_t bExternal, in uint uiTargetID, out uint16 *pu16Depth,
                          out uint64 *pu64TraceID);
uint16 dm__queue_max_depth(void);
void dm__queue_set_loop_assert(in bool_t bAssert);

/* ============================ 停读（2.8 背压） ============================ */
#define DM__STOP_QUEUE 0x01u   /* 总队列满了 */
#define DM__STOP_USER 0x02u    /* 调用方自己 pause（子进程） */
#define DM__STOP_HTTP 0x04u    /* HTTP 一条连接上没答的请求满了 */
#define DM__STOP_CLOSING 0x08u /* 答完错误、等着关（不再开读） */

typedef struct dm__flow
{
  dm_list_node_st stNode;
  dm_stream_st *pstStream;
  dm_alloc_cb pfAlloc;
  dm_read_cb pfRead;
  uint8 u8Stop;    /* 停读原因的位；全清了才真的读 */
  bool_t bReading; /* 现在是不是在读 */
} dm__flow_st;

int dm__flow_start(io dm__flow_st *pstFlow, in dm_stream_st *pstStream, in dm_alloc_cb pfAlloc,
                   in dm_read_cb pfRead);      /* 登记并开始读（总队列满着就先不读） */
void dm__flow_remove(io dm__flow_st *pstFlow); /* 关之前摘掉 */
void dm__flow_stop(io dm__flow_st *pstFlow, in uint8 u8Why);
void dm__flow_resume(io dm__flow_st *pstFlow, in uint8 u8Why);

/* ============================ 在途 work（2.2 拆除） ============================ */
uint32 dm__work_inflight(void);    /* 已经提交、after 还没回来的 */
void dm__work_cancel_queued(void); /* 排着队还没开始的 uv_cancel 掉（after 收到 UV_ECANCELED） */

/* ============================ link 内部 ============================ */
dm_link_handler_st *dm__link_lookup(in uint uiLinkID); /* 查登记表；没有返回 NULL */
int dm__link_register(in uint uiLinkID, in dm_link_before_cb pfBefore, in dm_link_work_cb pfWork,
                      in dm_link_after_cb pfAfter); /* 不查线程（DM 自己登记用） */
int dm__link_enqueue(in uint uiLinkID, in const dm_link_msg_st *pstMsg, in bool_t bExternal,
                     in uint32 u32ConnID, in uint32 u32ReqID);
void dm__link_config(
    in const dm_cfg_st *pstCfg); /* dm_main_ex 交配置：数据目录、角色表、单条上限 */
int dm__link_port(void);         /* 二进制口实际的端口 */
int dm__proto_init(void);        /* 起二进制口 */
void dm__proto_fini(void);       /* 关二进制口和所有连接 */
int dm__link_enqueue_ex(in uint uiLinkID, in const dm_link_msg_st *pstMsg, in bool_t bExternal,
                        in uint32 u32ConnID, in uint32 u32ReqID, in uint8 u8ReplySrc);
void dm__http_routes_clear(void);
int dm__conn_answer(in uint32 u32ConnID, in uint uiLinkID, in uint32 u32ReqID, in int iStatus,
                    in const void *pvBody, in uint32 u32Len);
int dm__conn_answer_ex(in uint32 u32ConnID, in uint uiLinkID, in uint32 u32ReqID, in int iStatus,
                       in uint8 u8ExtraFlag, in const void *pvBody, in uint32 u32Len);
uint32 dm__conn_role(in uint32 u32ConnID);
int dm__iface_allow(in uint uiLinkID, in uint32 u32Role); /* 连接上来的帧能不能叫到它 */
int dm__rpc_builtin_init(void);                           /* 登记 DM 自带的接口 */
void dm__rpc_destroy(void);
const char *dm__boot_hex(void);               /* 启动号（16 个十六进制字符） */
const char *dm__iface_name(in uint uiLinkID); /* 登记了接口的返回名字，没有返回 NULL */
uint32 dm__conn_count(void);                  /* 现在有几条连接 */
uint dm__threads_count(void);                 /* 线程池实际几条线程 */

/* ============================ 统计（stat.c） ============================ */
#define DM__STAT_EVENT_BIT 0x10000u /* 事件订阅统计键高位标记（stat.c 与 stat_rpc.c 共享） */
int dm__stat_init(void);
void dm__stat_fini(void);
void dm__stat_reset(void);
uint32 dm__stat_key(in uint uiLinkID, in uint8 u8Src, in uint64 u64Tag);
void dm__stat_call(in uint32 u32Key, in uint64 u64Ns, in bool_t bAfter);
void dm__stat_pool(in uint64 u64WaitNs);
uint64 dm__stat_loop_turns(void);
int dm__stat_builtin(in void *pvArg);
int dm__diag_snapshot_builtin(in void *pvArg);

typedef enum dm__conn_kind
{
  DM__CONN_BIN = 0, /* 二进制口：直接是 16 字节帧 */
  DM__CONN_HTTP,    /* HTTP 口上的普通请求 */
  DM__CONN_WS       /* HTTP 口上升级成 WebSocket 的：每条消息装一帧 */
} dm__conn_kind_et;

typedef struct dm__conn_info
{
  uint32 u32Id;
  uint32 u32Role;
  uint8 u8Kind;
  bool_t bAuthed;
  bool_t bPushAll;
  char acPeerID[DM_PEER_ID_MAX];
} dm__conn_info_st;
typedef void (*dm__conn_info_cb)(in const dm__conn_info_st *pstInfo, in void *pvArg);
void dm__conn_each(in dm__conn_info_cb pfEach, in void *pvArg);
const char *dm__conn_kind_str(in uint8 u8Kind);
bool_t dm__conn_peer_exists(in const char *pcPeerID);

typedef dm_log_stat_st dm__log_stat_st;
void dm__log_stat(out dm__log_stat_st *pstOut); /* log.c */
void dm__mem_tag_each(in void (*pfEach)(in int iTag, in const char *pcName, in uint64 u64Blocks,
                                        in uint64 u64Bytes, in void *pvArg),
                      in void *pvArg);
/* mem.c：只列有块的 */
/* ---- 推送、反向调用（push.c、call.c 和 link_proto.c 之间） ---- */
void dm__conn_push_fanout(in uint uiTopic, in const uint8 *pu8Frame, in uint32 u32Len,
                          in bool_t bStream);
int dm__conn_push_peer(in const char *pcPeerID, in uint uiTopic, in const uint8 *pu8Frame,
                       in uint32 u32Len);
int dm__conn_push_one(in uint32 u32ConnID, in uint uiTopic, in const uint8 *pu8Frame,
                      in uint32 u32Len);
int dm__conn_send(in uint32 u32ConnID, in const struct dm_frame_head *pstHead,
                  in const void *pvBody);
uint32 dm__conn_pick(in uint32 u32Role);
uint32 dm__conn_pick_peer(in const char *pcPeerID, in uint32 u32Role);
int dm__push_topic_id(in const char *pcName);
const char *dm__push_resume(in const char *pcBoot, in bool_t bHasSeq, in uint64 u64Seen);
void dm__push_replay(in uint32 u32ConnID, in uint64 u64Seen);
uint64 dm__push_ring_bytes(void);
uint32 dm__push_ring_count(void);
bool_t dm__push_stream_timer_on(void);
void dm__push_destroy(void);
bool_t dm__call_reply(in uint32 u32ConnID, in const struct dm_frame_head *pstHead,
                      in const uint8 *pu8Body);
void dm__call_conn_gone(in uint32 u32ConnID);
uint32 dm__call_pending(void);
void dm__call_destroy(void);
void dm__call_set_next_req_id(in uint32 u32Next);                  /* 白盒边界测试 T-BND-01 专用 */
int dm__port_file_read(in const char *pcDataDir, out int *piHttp); /* portfile.c：没有返回 0 */
void dm__port_file_write(in const char *pcDataDir, in int iPort, in int iHttp); /* portfile.c */
int dm__http_port(void); /* HTTP 口实际的端口 */
/* ---- 子进程（child/child.c，2.6） ---- */
int dm__child_startup(in const char *pcDataDir,
                      in const char *pcPtyHelper); /* ④ 清残留、开登记文件 */
void dm__child_teardown(void);  /* 拆除：还活着的整树 SIGKILL，最多等 DM_CHILD_REAP_MS */
void dm__child_close_all(void); /* 结束：剩下的句柄都关、关登记文件 */
uint32 dm__child_count(void);   /* 编号还有效的子进程数（dm.stat） */
/* ---- 数据库（db.c，2.15） ---- */
int dm__db_open(in const char *pcPath);   /* ④ 开库：NULL = 不开；主线程这条现在就开 */
void dm__db_init_done(void);              /* init 都做完：关掉主线程那条 */
void dm__db_close_all(void);              /* 结束阶段（work 都停了）：关掉所有连接 */
void dm__db_after_work(in uint32 u32Key); /* work 线程跑完一个 work：还开着的事务回滚，记 ERROR */
void dm__run_file_remove(void);           /* 删运行文件（写过才删） */
int dm__fs_replace(in const char *pcFrom, in const char *pcTo); /* 改名，目标已有就替换 */
/* 整个读进来（末尾补 0，最多 uMax 字节），用 dm_free_tag 释放；打不开回 NULL。路径 UTF-8 */
char *dm__file_read_all(in const char *pcPath, in size_t uMax, out size_t *puLen);
/* 写一个只有自己能读的文件（0600）：先写 .tmp 再改名，读的一方不会读到半个。路径 UTF-8 */
int dm__file_write_atomic(in const char *pcPath, in const char *pcText, in size_t uLen);
int dm__link_table_init(void);     /* dm_main_ex 建登记表（只建一次） */
void dm__link_table_destroy(void); /* 结束阶段销 */
void dm__link_stop_accept(void);   /* 收尾：不再接新连接（关监听） */

/* ============================ 定时器、事件（W5、W6 挂进来） ============================ */
uint32 dm__timer_count(void);    /* 活跃定时器数量（dm.diag.snapshot） */
void dm__timer_cancel_all(void); /* 拆除：取消所有定时器 */
void dm__event_destroy(void);    /* 结束：销订阅表 */

/* ============================ W2 核心微内核与扩展钩子内部接口 ============================ */
void dm__check_set_in_init(in bool_t bInInit);
void dm__check_unbind_main(void);

int dm__errno_mod_register(in uint16 u16ModID, in const dm_err_entry_st *pastErrMap);
void dm__errno_mod_clear(void);

void dm__ext_after_work(in uint32 u32StatKey);
void dm__ext_on_thread_exit(void);
uint32 dm__ext_child_count(void);

bool_t dm__link_table_ready(void);

typedef struct dm__stat_row
{
  uint32 u32Key;
  uint32 u32Before;
  uint32 u32After;
  uint64 u64TotalNs;
  uint64 u64MaxNs;
} dm__stat_row_st;

typedef void (*dm__stat_row_cb)(in const dm__stat_row_st *pstRow, in void *pvArg);

typedef struct dm__stat_summary
{
  uint64 u64LoopMaxNs;
  uint32 u32LoopSlow;
  uint32 u32PoolN;
  uint64 u64PoolTotalNs;
  uint64 u64PoolMaxNs;
} dm__stat_summary_st;

void dm__stat_set_name_cb(in const char *(*pfName)(in uint uiLinkID));
void dm__stat_each(in dm__stat_row_cb pfRow, in void *pvArg);
void dm__stat_summary(out dm__stat_summary_st *pstOut);

/* ============================ W3 dm_pool 与无状态直通内部接口 ============================ */
int dm__pool_init(in const dm_cfg_st *pstCfg);
void dm__pool_shutdown(void);
void dm__pool_destroy_sync(void);
int dm__pool_submit_internal(in dm_pool_lane_et eLane, in dm_pool_work_fn pfWork,
                             in dm_pool_after_fn pfAfter, in void *pvArg, in uint32 u32StatKey,
                             in uint16 u16Depth, in uint64 u64TraceID);
int dm__stateless_submit(in uint uiLinkID, in dm_pool_lane_et eLane, in dm_link_work_cb pfWork,
                         in dm_link_after_cb pfAfter, in uint8 u8Src, in uint8 u8Flags,
                         in uint32 u32Role, in uint64 u64Tag, in uint16 u16Depth,
                         in uint64 u64TraceID, in dm__qbuf_st *pstBuf,
                         in const dm_link_msg_st *pstReplyMsg, in bool_t bExtOwned);
int dm__ext_reply_take(in const void *pvMsg, out void *pvReply);
int dm__ext_reply_send(in const void *pvReply, in int iStatus, in const void *pvBody,
                       in uint32 u32Len);
int dm__link_register_ex(in uint uiLinkID, in dm_pool_lane_et eLane, in dm_link_before_cb pfBefore,
                         in dm_link_work_cb pfWork, in dm_link_after_cb pfAfter);

#ifdef __cplusplus
}
#endif

#endif /* DM_INTERNAL_H */
