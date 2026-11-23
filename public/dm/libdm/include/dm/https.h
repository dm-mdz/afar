/**
 ******************************************************************************
 * @file    https.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   Afar 定制局域网单连接单请求 TLS HTTPS 服务公开接口（libdm_net.a）。
 ******************************************************************************
 */
#ifndef DM_HTTPS_H
#define DM_HTTPS_H

#include <dm/module.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct dm_https_cfg
{
  const char *pcBindAddr;      /* "::" 双栈（IPV6_V6ONLY=0，失败回退 "0.0.0.0"） */
  uint16 u16Port;              /* 固定端口（0 = 系统分配临时口，非 0 被占直接失败） */
  const char *pcCertDerPath;   /* ECDSA P-256 X.509 DER 路径 */
  const char *pcKeyDerPath;    /* DER 私钥路径（兼容 PKCS#8 与 SEC1 ECPrivateKey） */
  uint32 u32MaxConns;          /* 全局并发上限（0 = 默认 32） */
  uint32 u32MaxPerIp;          /* 单 IP 并发上限，IPv4 归一化 ::ffff:a.b.c.d（0 = 默认 8） */
  uint32 u32BodyMax;           /* 单请求 Body 上限（0 = 默认 65536） */
  uint32 u32HandshakeMs;       /* TLS 握手超时（0 = 默认 3000） */
  uint32 u32IdleMs;            /* 读请求总超时（0 = 默认 15000） */
  uint32 u32ReplyMs;           /* 异步路由兜底应答超时（0 = 默认 30000） */
  void (*pfOnDrain)(void);     /* 可选：排空入口最先触发（零反向链接业务符号） */
} dm_https_cfg_st;

typedef struct dm_https_msg
{
  uint64 u64ReplyId;           /* 不透明句柄 (slot<<32 | gen)，可跨 dm_pool 保存并传给 dm_https_reply */
  const char *pcMethod;        /* "GET" | "POST" */
  const char *pcPath;          /* 精确路径，如 "/status" */
  const char *pcAuthHeader;    /* Authorization 头原值（无则为 NULL） */
  const uint8 *pu8Body;        /* 请求体（末尾额外补 '\0' 方便 JSON 解析） */
  uint32 u32BodyLen;
} dm_https_msg_st;

typedef void (*dm_https_handler_fn)(in const dm_https_msg_st *pstMsg);

/* 测试期释前擦零校验钩子（在 free 之前检查敏感缓冲是否已全零，零 ASan UAF） */
typedef void (*dm_https_on_free_fn)(in const uint8 *pu8Auth, in size_t uAuthLen,
                                    in const uint8 *pu8Body, in size_t uBodyLen,
                                    in const uint8 *pu8Resp, in size_t uRespLen);

DM_API const dm_ext_hooks_st *dm_https_hooks(void);
DM_API int dm_https_route_exact(in const char *pcMethod, in const char *pcPath,
                                in dm_https_handler_fn pfHandler);
DM_API int dm_https_reply(in uint64 u64ReplyId, in uint uiStatus, in const void *pvBody,
                          in uint32 u32Len);
DM_API uint16 dm_https_get_port(void);
DM_API void dm_https_set_test_on_free(in dm_https_on_free_fn pfCb);

#ifdef __cplusplus
}
#endif

#endif /* DM_HTTPS_H */
