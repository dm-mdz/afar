/**
 ******************************************************************************
 * @file    dmc_proto.h
 * @author  MDM
 * @version 0.0.1.0
 * @brief   dmc 下层协议引擎（DD-02 §2.1~2.2，PLUG-01/02，AUD-10）：
 *          纯 Sans-IO 状态机：零 IO、零 libuv、零 libdm 链接；支持自定义内存分配钩子、
 *          单向通知 dmc_notify(reqID=0) 与独立客户端错误码段 [-90999, -90000]。
 ******************************************************************************
 */
#ifndef DMC_PROTO_H
#define DMC_PROTO_H

#include <dm/frame.h>

#ifdef __cplusplus
extern "C"
{
#endif

#if defined(_WIN32) && defined(DMC_SHARED)
#if defined(DMC_BUILDING_SHARED)
#define DMC_API __declspec(dllexport)
#else
#define DMC_API __declspec(dllimport)
#endif
#else
#define DMC_API
#endif

/* 客户端本地错误码段：[-90999, -90000]，与核心 [-100000, ...] 及业务模块 [-200000, ...] 永不重叠 */
#define DMC_ERRNO_BASE (-90000)
#define DMC_OK 0
#define DMC_ERR_TIMEOUT (DMC_ERRNO_BASE)         /* 宿主等待超时 */
#define DMC_ERR_NOT_CON (DMC_ERRNO_BASE - 1)     /* 未连接 / 已断线 */
#define DMC_ERR_ARG (DMC_ERRNO_BASE - 2)         /* 参数非法 */
#define DMC_ERR_QUEUE (DMC_ERRNO_BASE - 3)       /* 在途请求满 / 内存不足 */
#define DMC_ERR_WIRE (DMC_ERRNO_BASE - 4)        /* 线缆帧非法（超长 / 未知类型 / COBS 坏帧） */
#define DMC_ERR_CRC (DMC_ERRNO_BASE - 5)         /* 同步帧 CRC16 校验失败 */
#define DMC_ERR_AUTH (DMC_ERRNO_BASE - 6)        /* 握手口令或角色被拒 */
#define DMC_ERR_NOT_FOUND (DMC_ERRNO_BASE - 7)   /* 按接口名未找到 */
#define DMC_ERR_UNSUPPORTED (DMC_ERRNO_BASE - 8) /* 传输协议暂未实现（ws/serial） */

#define DMC_PENDING_MAX 64u /* 同时在途请求上限 */

typedef struct dmc dmc_st; /* 不透明协议实例 */

/* 一条推送消息 */
typedef struct dmc_push
{
  uint64 u64Seq;   /* 全局序号；流式或单播推送为 0 */
  uint16 u16Topic; /* 推送类别 */
  uint8 u8Flag;    /* 帧标志位（DM_FRAME_F_JSON / DM_FRAME_F_STREAM 等） */
  uint64 u64Key;   /* 键 */
  const uint8 *pu8Val;
  uint32 u32Len;
} dmc_push_st;

typedef struct dmc_host
{
  /* 发一帧：16 字节帧头 + 负载（可为空）。返回 0 成功；非 0 视作发送失败 */
  int (*pfSend)(dm_in void *pvUser, dm_in const uint8 *pu8Head, dm_in const void *pvBody,
                dm_in uint32 u32Len);
  /* 收到推送；NULL = 忽略 */
  void (*pfOnPush)(dm_in void *pvUser, dm_in const dmc_push_st *pstPush);
  /* 核心反向调用；NULL = 自动回复 DM_NOT_FOUND */
  void (*pfOnCall)(dm_in void *pvUser, dm_in uint16 u16Func, dm_in uint32 u32ReqID,
                   dm_in const uint8 *pu8Body, dm_in uint32 u32Len);
  /* 收到坏帧回调；到下次 dmc_on_connected 前后续字节丢弃；可 NULL */
  void (*pfOnWireError)(dm_in void *pvUser, dm_in int iErr);
  /* 可选内存分配钩子（嵌入式零堆碎片）：pfAlloc 非 NULL 时 pfFree 必填，pfRealloc 可 NULL */
  void *(*pfAlloc)(dm_in void *pvUser, dm_in size_t uSize);
  void *(*pfRealloc)(dm_in void *pvUser, dm_io void *pvPtr, dm_in size_t uSize);
  void (*pfFree)(dm_in void *pvUser, dm_io void *pvPtr);
  void *pvUser;       /* 原样透传给全部宿主回调 */
  uint32 u32FrameMax; /* 单帧负载上限；0 = DM_FRAME_MAX_DEFAULT */
} dmc_host_st;

/* 应答回调：iStatus 为核心返回值或 DMC_ERR_*；每个请求最多回调一次 */
typedef void (*dmc_reply_cb)(dm_in void *pvArg, dm_in int iStatus, dm_in uint8 u8Flag,
                             dm_in const uint8 *pu8Body, dm_in uint32 u32Len);

/* 错误码转人类可读文本（永不返回 NULL） */
DMC_API const char *dmc_err_str(dm_in int iErr);

/* 创建与销毁：pfSend 必填；若提供 pfAlloc 则 pfFree 亦必填 */
DMC_API dmc_st *dmc_new(dm_in const dmc_host_st *pstHost);
DMC_API void dmc_free(dm_io dmc_st *pstDmc);

/* 连接生命周期通知 */
DMC_API void dmc_on_connected(dm_io dmc_st *pstDmc);
DMC_API void dmc_on_disconnected(dm_io dmc_st *pstDmc);
DMC_API bool_t dmc_connected(dm_in const dmc_st *pstDmc);

/* 喂入收到的字节流，返回消费字节数 */
DMC_API size_t dmc_feed(dm_io dmc_st *pstDmc, dm_in const void *pvData, dm_in size_t uLen);

/* 发起请求参数（零初始化友好） */
typedef struct dmc_req_opt
{
  uint16 u16LinkID;     /* 目标接口 linkID */
  uint8 u8Flag;         /* 帧标志（如 DM_FRAME_F_JSON / DM_FRAME_F_SENSITIVE） */
  const void *pvBody;   /* 请求体（可 NULL） */
  uint32 u32Len;        /* 请求体字节数 */
  dmc_reply_cb pfReply; /* 应答回调（可 NULL） */
  void *pvArg;          /* 回调透传上下文 */
} dmc_req_opt_st;

/* 发送请求（占 1 个在途槽）；非 DMC_OK 不触发 pfReply */
DMC_API int dmc_request(dm_io dmc_st *pstDmc, dm_in const dmc_req_opt_st *pstReq,
                        dm_out uint32 *pu32ReqID);

/* 发送单向通知（reqID = 0，不占 64 在途槽，适合高频遥测/心跳） */
DMC_API int dmc_notify(dm_io dmc_st *pstDmc, dm_in uint16 u16LinkID, dm_in uint8 u8Flag,
                       dm_in const void *pvBody, dm_in uint32 u32Len);

/* 取消在途请求并以 iStatus 立即回调一次 */
DMC_API int dmc_cancel(dm_io dmc_st *pstDmc, dm_in uint32 u32ReqID, dm_in int iStatus);

/* 回复反向调用参数（零初始化友好） */
typedef struct dmc_ans_opt
{
  uint16 u16Func;     /* 反向调用功能号 */
  uint32 u32ReqID;    /* 对应请求号（非 0） */
  int32 i32Status;    /* 状态码 */
  uint8 u8Flag;       /* 帧标志 */
  const void *pvBody; /* 应答体（可 NULL） */
  uint32 u32Len;      /* 应答体字节数 */
} dmc_ans_opt_st;

/* 回复核心的反向调用 */
DMC_API int dmc_answer(dm_io dmc_st *pstDmc, dm_in const dmc_ans_opt_st *pstAns);

/* 当前在途请求数 */
DMC_API uint32 dmc_inflight(dm_in const dmc_st *pstDmc);

#ifdef __cplusplus
}
#endif

#endif /* DMC_PROTO_H */
