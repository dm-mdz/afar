/**
 ******************************************************************************
 * @file    limits.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   DM 的上限和时长常量（详设 2.13）。
 *
 * 应用真要调的几个（线程数、单条消息上限、总队列上限、收尾最多等多久）在 dm_cfg 里；
 * 其余的都在这里。每个都包在 #ifndef 里：测试构建用 -D 改小，平时不用动。
 * 改这里的默认值等于改设计，要先过评审。
 ******************************************************************************
 */
#ifndef DM_LIMITS_H
#define DM_LIMITS_H

/* ---- 连接 ---- */
#ifndef DM_PRE_AUTH_MAX
#define DM_PRE_AUTH_MAX (4u * 1024u) /* 握手前一条消息最大 */
#endif
#ifndef DM_STAT_SLOW_MS
#define DM_STAT_SLOW_MS 10u /* 主线程一次回调、一圈循环超过它记 WARN（概设 7.1） */
#endif
#ifndef DM_STAT_POOL_MS
#define DM_STAT_POOL_MS 100u /* 线程池排队平均超过它记 WARN */
#endif
#ifndef DM_HANDSHAKE_MS
#define DM_HANDSHAKE_MS 5000u /* 连上后多久必须握手 */
#endif
#ifndef DM_SLOW_CONN
#define DM_SLOW_CONN (32u * 1024u * 1024u) /* 一条连接上没写出去的超过它就断开 */
#endif

/* ---- HTTP、WebSocket ---- */
#ifndef DM_HTTP_HEAD_MAX
#define DM_HTTP_HEAD_MAX (16u * 1024u) /* HTTP 请求头 */
#endif
#ifndef DM_HTTP_BODY_MAX
#define DM_HTTP_BODY_MAX (16u * 1024u * 1024u) /* HTTP 正文 */
#endif
#ifndef DM_HTTP_PENDING
#define DM_HTTP_PENDING 64u /* 一条 HTTP 连接上没答的请求 */
#endif
#ifndef DM_HTTP_IDLE_MS
#define DM_HTTP_IDLE_MS 60000u /* HTTP 空闲多久关 */
#endif
#ifndef DM_WS_PING_MS
#define DM_WS_PING_MS 30000u /* WebSocket 心跳周期 */
#endif

/* ---- 推送 ---- */
#ifndef DM_PUSH_COUNT
#define DM_PUSH_COUNT 4096u /* 推送缓冲最多几条 */
#endif
#ifndef DM_PUSH_BYTES
#define DM_PUSH_BYTES (16u * 1024u * 1024u) /* 推送缓冲最多多少字节 */
#endif
#ifndef DM_STREAM_HIGH
#define DM_STREAM_HIGH (1u * 1024u * 1024u) /* 流式：连接上积压超过它，这一拍不给它发 */
#endif
#ifndef DM_STREAM_MS
#define DM_STREAM_MS 50u /* 流式合并的间隔 */
#endif

/* ---- 子进程 ---- */
#ifndef DM_LINE_MAX
#define DM_LINE_MAX (64u * 1024u * 1024u) /* 子进程一行最长 */
#endif
#ifndef DM_CHILD_WRITE_MAX
#define DM_CHILD_WRITE_MAX (8u * 1024u * 1024u) /* 往一个子进程写的队列 */
#endif
#ifndef DM_CHILD_EOF_MS
#define DM_CHILD_EOF_MS 2000u /* 子进程退出后最多再等多久读剩下的输出 */
#endif
#ifndef DM_KILL_GRACE_MS
#define DM_KILL_GRACE_MS 1000u /* SIGTERM 到 SIGKILL */
#endif

/* ---- 退出 ---- */
#ifdef _WIN32
#ifndef DM_WORK_WAIT_MS
#define DM_WORK_WAIT_MS 1500u /* 拆除时最多等 work */
#endif
#else
#ifndef DM_WORK_WAIT_MS
#define DM_WORK_WAIT_MS 3000u
#endif
#endif
#ifndef DM_CHILD_REAP_MS
#define DM_CHILD_REAP_MS 1000u /* 拆除时最多等子进程 */
#endif

/* ---- 数据库 ---- */
#ifndef DM_DB_BUSY_MS
#define DM_DB_BUSY_MS 5000u /* 几个线程同时写时排队等多久（busy_timeout） */
#endif

/* ---- 日志 ---- */
#ifndef DM_LOG_RECENT
#define DM_LOG_RECENT 2000u /* 内存里留的最近日志行数 */
#endif

/* ---- 总队列调度（DD-01 §3.2） ---- */
#ifndef DM_MAX_DISPATCH_DEPTH
#define DM_MAX_DISPATCH_DEPTH 16u /* 派发传播最大跳数：>16 熔断返 DM_LOOP_DETECTED */
#endif
#ifndef DM_QUEUE_TICK_MAX
#define DM_QUEUE_TICK_MAX 64u /* 单圈 idle 批处理最大消息数 */
#endif
#ifndef DM_QUEUE_TICK_BUDGET_MS
#define DM_QUEUE_TICK_BUDGET_MS 4u /* 单圈 idle 批处理时间片预算（毫秒） */
#endif
#ifndef DM_LOG_RECENT_LINE
#define DM_LOG_RECENT_LINE 512u /* 最近一批里每行最多存多少字节（定长，热路径不分配） */
#endif

#endif /* DM_LIMITS_H */
