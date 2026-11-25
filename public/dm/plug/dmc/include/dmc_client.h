/**
 ******************************************************************************
 * @file    dmc_client.h
 * @brief   dmc 上层连接会话 SDK（DD-02 §2.4，PLUG-01/03）：
 *          统一封装 local://、tcp:// 与可扩展传输 vtable（ws:// / serial:// 预留）；
 *          内置自动握手、DM_LINKID_IFACE_LIST 缓存与按名调用、心跳、断线重连与推送续传。
 ******************************************************************************
 */
#ifndef DMC_CLIENT_H
#define DMC_CLIENT_H

#include <dmc_proto.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ============================ 可插拔传输层 vtable ============================ */

typedef struct dmc_transport_vtbl
{
  const char *pcScheme; /* "local" | "tcp" | "ws" | "serial" 等 */
  /* 打开传输连接；成功返 DMC_OK 并写出 *ppvHandle */
  int (*pfOpen)(dm_in const char *pcUri, dm_in uint32 u32TimeoutMs, dm_out void **ppvHandle);
  /* 读字节（最多等 iTimeoutMs，<0 阻塞）：>0 读到字节数，0 对端关闭，-2 超时，-1 错误 */
  int (*pfRead)(dm_io void *pvHandle, dm_out void *pvBuf, dm_in size_t uCap, dm_in int iTimeoutMs);
  /* 写全 uLen 字节：0 成功，非 0 失败 */
  int (*pfWrite)(dm_io void *pvHandle, dm_in const void *pvBuf, dm_in size_t uLen);
  /* 关闭并释放句柄 */
  void (*pfClose)(dm_io void *pvHandle);
} dmc_transport_vtbl_st;

/* 注册或覆盖指定 scheme 的传输实现（ws://、serial:// 等可按需插入） */
DMC_API int dmc_transport_register(dm_in const dmc_transport_vtbl_st *pstVtbl);

/* 查询指定 scheme 已注册的传输表项（未注册返回 NULL） */
DMC_API const dmc_transport_vtbl_st *dmc_transport_find(dm_in const char *pcScheme);

/* 工具函数：单调时钟（毫秒）与 UTF-8 路径整文件读取（调用方 free 释放） */
DMC_API uint64 dmc_now_ms(void);
DMC_API char *dmc_read_file_utf8(dm_in const char *pcPath, dm_out size_t *puLen);

/* ============================ 上层会话 API ============================ */

typedef struct dmc_client dmc_client_st;

typedef struct dmc_client_opt
{
  const char *pcTarget;  /* NULL/"local://" | "tcp://host:port" | "ws://..." | "serial://..." */
  const char *pcDataDir; /* local:// 模式读 dm.run 的目录（NULL = "."） */
  const char *pcToken;   /* 显式口令（tcp/ws/serial 或覆盖运行文件） */
  const char *pcRole;    /* "cli"（默认）| "diag" | "ui" | "shell" */
  const char *pcPeerID;  /* 节点标识（NULL = "cli-<pid>"） */
  uint32 u32ConnectTimeoutMs; /* 建连超时毫秒，0 = 默认 5000 */
  uint32 u32DefaultTimeoutMs; /* 单次调用默认超时毫秒，0 = 默认 15000 */
  uint32 u32PingIntervalMs;   /* 自动心跳周期毫秒，0 = 默认 15000 */
  bool_t bAutoReconnect;      /* 断线自动重连 + 按 last_seq 续传推送 */
  bool_t bWantPush;           /* 握手是否订阅推送 */
  bool_t bHasSinceSeq;        /* 是否携带初始续传序号 u64SinceSeq */
  uint64 u64SinceSeq;         /* 初始续传序号（watch --since） */
  const char *const
      *ppcPushTopics; /* NULL 结尾的订阅主题名列表；bWantPush 为真且 NULL 时订阅 "*" */
  void (*pfOnPush)(dm_in void *pvUser, dm_in const dmc_push_st *pstPush);
  void (*pfOnCall)(dm_in void *pvUser, dm_in uint16 u16Func, dm_in uint32 u32ReqID,
                   dm_in const uint8 *pu8Body, dm_in uint32 u32Len);
  void *pvUser;
} dmc_client_opt_st;

typedef struct dmc_resp
{
  int iStatus;  /* 0 成功；负数为本地 DMC_ERR_* 或核心 DM_* 错误码 */
  uint8 u8Flag; /* 帧头标志位 */
  char *pcBody; /* 末尾补 '\0'，调用方 dmc_resp_free 释放 */
  uint32 u32Len;
} dmc_resp_st;

/* 建连并完成握手；失败返回 NULL 并在 *piErr（可 NULL）带出 DMC_ERR_* 或核心错误码 */
DMC_API dmc_client_st *dmc_client_open(dm_in const dmc_client_opt_st *pstOpt, dm_out int *piErr);

/* 断开并释放客户端会话（NULL 安全） */
DMC_API void dmc_client_close(dm_io dmc_client_st *pstCli);

/* 同步按接口名调用（内置 dm.* 直映 + 自动缓存/刷新 DM_LINKID_IFACE_LIST；
 * 命中 DM_IFACE_SENSITIVE 时自动置 DM_FRAME_F_SENSITIVE） */
DMC_API int dmc_client_call_name(dm_io dmc_client_st *pstCli, dm_in const char *pcName,
                                 dm_in const char *pcJsonBody, dm_in uint32 u32TimeoutMs,
                                 dm_out dmc_resp_st *pstResp);

/* 扩展按接口名调用参数（零初始化友好） */
typedef struct dmc_call_name_req
{
  const char *pcName;     /* 接口名（如 "dm.stat"） */
  uint8 u8ExtraFlags;     /* 额外帧标志（如 DM_FRAME_F_SENSITIVE） */
  const char *pcJsonBody; /* JSON 请求体（可 NULL） */
  uint32 u32TimeoutMs;    /* 超时毫秒（0 用默认超时） */
} dmc_call_name_req_st;

/* 同步按 linkID 调用参数（零初始化友好） */
typedef struct dmc_call_id_req
{
  uint16 u16LinkID;    /* 目标接口 linkID */
  uint8 u8Flag;        /* 帧头标志 */
  const void *pvBody;  /* 请求体（可 NULL） */
  uint32 u32Len;       /* 请求体字节数 */
  uint32 u32TimeoutMs; /* 超时毫秒（0 用默认超时） */
} dmc_call_id_req_st;

/* 扩展按接口名调用：支持额外透传帧标志（如 DM_FRAME_F_SENSITIVE） */
DMC_API int dmc_client_call_name_ex(dm_io dmc_client_st *pstCli,
                                    dm_in const dmc_call_name_req_st *pstReq,
                                    dm_out dmc_resp_st *pstResp);

/* 同步按 linkID 调用 */
DMC_API int dmc_client_call_id(dm_io dmc_client_st *pstCli, dm_in const dmc_call_id_req_st *pstReq,
                               dm_out dmc_resp_st *pstResp);

/* 长连事件驱动泵：收推送/反向调用、定时发心跳、断线自动重连续传 */
DMC_API int dmc_client_poll(dm_io dmc_client_st *pstCli, dm_in uint32 u32WaitMs);

/* 获取（或刷新）缓存的 dm.iface.list 原始 JSON 文本（生命周期归 pstCli 所有） */
DMC_API const char *dmc_client_ifaces_json(dm_io dmc_client_st *pstCli, dm_in bool_t bRefresh);

/* 读取最新推送序号、核心启动号与 local:// 运行文件里的 HTTP 端口 */
DMC_API uint64 dmc_client_last_seq(dm_in const dmc_client_st *pstCli);
DMC_API const char *dmc_client_boot(dm_in const dmc_client_st *pstCli);
DMC_API int dmc_client_http_port(dm_in const dmc_client_st *pstCli);

/* 释放 dmc_resp_st 内部响应体 */
DMC_API void dmc_resp_free(dm_io dmc_resp_st *pstResp);

#ifdef __cplusplus
}
#endif

#endif /* DMC_CLIENT_H */
