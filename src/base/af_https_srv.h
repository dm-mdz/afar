/**
 ******************************************************************************
 * @file    af_https_srv.h
 * @author  Afar_C
 * @version 2.0.0
 * @brief   独立线程局域网单连接单请求 TLS HTTPS 服务公开接口（基于 DM v2.1.0 公开原语）。
 ******************************************************************************
 */
#ifndef AF_HTTPS_SRV_H
#define AF_HTTPS_SRV_H

#include <dm/module.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct af_https_cfg
{
  const char *pcBindAddr;    /* "::" 双栈（DM_TCP_F_NONE，失败回退 "0.0.0.0"） */
  uint16 u16Port;            /* 固定端口（0 = 系统分配临时口，非 0 被占返 DM_IO） */
  const char *pcCertDerPath; /* ECDSA P-256 X.509 DER 路径 */
  const char *pcKeyDerPath;  /* DER 私钥路径（兼容 PKCS#8 与 SEC1 ECPrivateKey） */
  uint32 u32MaxConns;        /* 全局并发上限（0 = 默认 32） */
  uint32 u32MaxPerIp;        /* 单 IP 并发上限，IPv4 归一化 ::ffff:a.b.c.d（0 = 默认 8） */
  uint32 u32BodyMax;         /* 单请求 Body 上限（0 = 默认 65536） */
  uint32 u32HandshakeMs;     /* TLS 握手超时（0 = 默认 3000） */
  uint32 u32IdleMs;          /* 读请求总超时（0 = 默认 15000） */
  uint32 u32ReplyMs;         /* 异步路由兜底应答超时（0 = 默认 30000） */
  void (*pfOnDrain)(void);   /* 可选：排空入口最先触发（零反向链接业务符号） */
} af_https_cfg_st;

typedef struct af_https_msg
{
  uint64 u64ReplyId;        /* 不透明值句柄 (slot<<32 | gen)，供 af_https_reply 精确匹配 */
  const char *pcMethod;     /* "GET" | "POST" */
  const char *pcPath;       /* 精确路径，如 "/status" */
  const char *pcAuthHeader; /* Authorization 头原值（无则为 NULL） */
  const uint8 *pu8Body;     /* 请求体（末尾额外补 '\0' 方便 JSON 解析） */
  uint32 u32BodyLen;
} af_https_msg_st;

typedef struct af_https_free_view
{
  const uint8 *pu8Auth;
  size_t uAuthLen;
  const uint8 *pu8Body;
  size_t uBodyLen;
  const uint8 *pu8Resp;
  size_t uRespLen;
} af_https_free_view_st;

typedef void (*af_https_handler_fn)(dm_in const af_https_msg_st *pstMsg);
typedef void (*af_https_on_free_fn)(dm_in const af_https_free_view_st *pstView);

void af_https_set_cfg(dm_in const af_https_cfg_st *pstCfg);
const dm_ext_hooks_st *af_https_hooks(void);
int af_https_route_exact(dm_in const char *pcMethod, dm_in const char *pcPath,
                         dm_in af_https_handler_fn pfHandler);
int af_https_reply(dm_in uint64 u64ReplyId, dm_in uint uiStatus, dm_in const void *pvBody,
                   dm_in uint32 u32Len);
uint16 af_https_get_port(void);
void af_https_set_test_on_free(dm_in af_https_on_free_fn pfCb);

#ifdef __cplusplus
}
#endif

#endif /* AF_HTTPS_SRV_H */
