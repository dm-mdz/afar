/**
 ******************************************************************************
 * @file    sock.h
 * @author  MDM
 * @version 2.1.0
 * @brief   DM 通用跨平台网络原语：纯 C 地址、多网卡枚举、DNS 解析、TCP 与 UDP（单播/广播/组播）。
 *          零底层库与操作系统原生头泄露；全部异步句柄绑定显式 dm_loop_st*。
 ******************************************************************************
 */
#ifndef DM_SOCK_H
#define DM_SOCK_H

#include <dm.h>
#include <dm/loop.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ============================ 纯 C 地址与网卡结构 ============================ */

#define DM_SOCKADDR_STR_MAX 64u /* 足够容纳 "[ffff:...:ffff%4294967295]:65535\0" */
#define DM_IFNAME_MAX 64u       /* 网卡名最大缓冲长度 */
#define DM_SOCK_TX_QUEUE_MAX (16u * 1024u * 1024u) /* 默认单连接/单套接字写队列背压上限 16MB */
#define DM_UDP_PAYLOAD_MAX 65507u                  /* IPv4/UDP 最大单报载荷字节数 */

typedef enum dm_af
{
  DM_AF_UNSPEC = 0,
  DM_AF_IPV4 = 4,
  DM_AF_IPV6 = 6
} dm_af_et;

/* 平台无关纯 C 套接字地址（主机字节序端口，网络字节序 IP 原始字节）：
 * - DM_AF_IPV4：有效字节为 au8Addr[0..3]（大端 4 字节 IPv4），au8Addr[4..15] 置 0；
 * - DM_AF_IPV6：有效字节为 au8Addr[0..15]（大端 16 字节 IPv6），u32ScopeId 为链路本地 scope_id。 */
typedef struct dm_sockaddr
{
  dm_af_et eFamily;
  uint16 u16Port;    /* 主机字节序（0..65535） */
  uint8 au8Addr[16]; /* IPv4 用前 4B，IPv6 用全部 16B */
  uint32 u32ScopeId; /* IPv6 scope_id（IPv4 为 0） */
} dm_sockaddr_st;

/* 网卡地址条目（供多网卡绑定、局域网广播/组播发现使用） */
typedef struct dm_ifaddr
{
  char acName[DM_IFNAME_MAX]; /* 网卡名（UTF-8，如 "en0"、"eth0"、"Ethernet"） */
  uint8 au8PhysAddr[6];       /* MAC 地址（无物理地址时全 0） */
  bool_t bHasPhysAddr;        /* MAC 地址是否有效 */
  bool_t bIsInternal;         /* DM_TRUE = 回环接口（loopback） */
  dm_sockaddr_st stAddr;      /* IPv4 或 IPv6 接口地址（u16Port = 0） */
  dm_sockaddr_st stNetmask;   /* 对应子网掩码 */
} dm_ifaddr_st;

/* ---- 地址解析、构造与格式化（纯计算，线程安全，零堆分配） ---- */
DM_API int dm_sockaddr_ip4(dm_in const char *pcIp, dm_in uint16 u16Port,
                           dm_out dm_sockaddr_st *pstOut);

/* 支持可选 IPv6 作用域后缀（如 "fe80::1%1" 或 "fe80::1%eth0"），自动写入 u32ScopeId；
 * 空后缀（"fe80::1%"）、纯数字溢出 >UINT32_MAX 或无效网卡名返回 DM_INVALID */
DM_API int dm_sockaddr_ip6(dm_in const char *pcIp, dm_in uint16 u16Port,
                           dm_out dm_sockaddr_st *pstOut);

/* 格式化为 "1.2.3.4:8080"、"[::1]:8080" 或 "[fe80::1%1]:8080"（u16Port == 0 时仅输出纯 IP，
 * IPv6 且 u32ScopeId != 0 时附带 "%<u32ScopeId>"；pcBuf == NULL 或 uCap == 0 回 DM_INVALID；
 * 1 <= uCap <= uLen 缓冲不足回 DM_TOO_BIG） */
DM_API int dm_sockaddr_format(dm_in const dm_sockaddr_st *pstAddr, dm_out char *pcBuf,
                              dm_in size_t uCap);

/* 纯 IP 文本 <-> 二进制转换（IPv4 写 au8Dst[0..3] 并清零后 12B；IPv6 写全部 16B；
 * dm_inet_ntop 入口校验 pcDst == NULL || 0u == uCap -> DM_INVALID；1 <= uCap <= uLen 回
 * DM_TOO_BIG） */
DM_API int dm_inet_pton(dm_in dm_af_et eFamily, dm_in const char *pcSrc, dm_out uint8 au8Dst[16]);
DM_API int dm_inet_ntop(dm_in dm_af_et eFamily, dm_in const uint8 au8Src[16], dm_out char *pcDst,
                        dm_in size_t uCap);

/* ---- 多网卡枚举（线程安全；结果由 DM_MEM_TAG_SOCK 分配，调用方用 dm_ifaddrs_free 释放） ---- */
DM_API int dm_ifaddrs_list(dm_out dm_ifaddr_st **ppastOut, dm_out uint32 *pu32Count);
DM_API void dm_ifaddrs_free(dm_io dm_ifaddr_st *pastList, dm_in uint32 u32Count);

/* ============================ DNS 解析（异步 + 同步） ============================ */

typedef struct dm_dns_req dm_dns_req_st;

typedef struct dm_dns_query
{
  const char *pcHost;    /* 域名或 IP 字面量（UTF-8，不可空） */
  const char *pcService; /* 端口字符串如 "443" 或服务名（可 NULL） */
  dm_af_et eFamily;      /* DM_AF_UNSPEC / DM_AF_IPV4 / DM_AF_IPV6 */
  uint32 u32TimeoutMs;   /* 超时毫秒（0 = 默认 5000ms；异步与同步均受此约束） */
} dm_dns_query_st;

/* 异步 DNS 完成回调：pastAddrs 仅在回调期间有效（借用），需要持久化请按值拷贝。
 * iErr == DM_SUCCESS 时 u32Count >= 1；取消回 DM_CANCELED；超时回 DM_TIMEOUT；失败回 DM_DNS_FAIL。
 */
typedef void (*dm_dns_cb)(dm_in int iErr, dm_in const dm_sockaddr_st *pastAddrs,
                          dm_in uint32 u32Count, dm_io void *pvUser);

/* 在指定循环上发起异步 DNS 解析（可选输出 ppstReq 供中途 dm_dns_cancel，可传 NULL）。
 * 内部使用两段式完成栅栏与单次投递保护，零 UAF 零泄漏。 */
DM_API int dm_dns_resolve_async(dm_io dm_loop_st *pstLoop, dm_in const dm_dns_query_st *pstQuery,
                                dm_in dm_dns_cb pfCb, dm_in void *pvUser,
                                dm_out dm_dns_req_st **ppstReq);

/* 取消在途异步 DNS 请求（须在原 loop 线程调用；回调触发后不得再使用 pstReq） */
DM_API int dm_dns_cancel(dm_io dm_dns_req_st *pstReq);

/* 同步 DNS 解析（可在工作线程调用，严禁在 DM 主循环热路径阻塞调用；受 u32TimeoutMs 严格约束）：
 *   - IPv4/IPv6 字面量 + 空/纯数字端口走零线程快速路径；
 *   - 域名查询采用独立引用计数分离工作线程 + dm_cond_timedwait_ms，超时立即返回 DM_TIMEOUT。 */
DM_API int dm_dns_resolve_sync(dm_in const dm_dns_query_st *pstQuery,
                               dm_out dm_sockaddr_st *pastOut, dm_in uint32 u32Cap,
                               dm_out uint32 *pu32Count);

/* ============================ 通用异步 TCP（dm_tcp） ============================ */

typedef struct dm_tcp dm_tcp_st;

#define DM_TCP_F_NONE 0u
#define DM_TCP_F_IPV6ONLY (1u << 0)

/* 连接监听、主动建连、读数据、写完成、半关闭回调 */
typedef void (*dm_tcp_accept_cb)(dm_io dm_tcp_st *pstServer, dm_in int iErr, dm_io void *pvUser);
typedef void (*dm_tcp_connect_cb)(dm_io dm_tcp_st *pstTcp, dm_in int iErr, dm_io void *pvUser);
/* iErr == DM_SUCCESS 且 uLen > 0：收到数据（pu8Data 仅借用）；iErr ==
 * DM_EOF：对端优雅关闭；其余负值：错 */
typedef void (*dm_tcp_read_cb)(dm_io dm_tcp_st *pstTcp, dm_in int iErr, dm_in const uint8 *pu8Data,
                               dm_in size_t uLen, dm_io void *pvUser);
typedef void (*dm_tcp_write_cb)(dm_io dm_tcp_st *pstTcp, dm_in int iErr, dm_io void *pvUser);
typedef void (*dm_tcp_shutdown_cb)(dm_io dm_tcp_st *pstTcp, dm_in int iErr, dm_io void *pvUser);

DM_API int dm_tcp_create(dm_io dm_loop_st *pstLoop, dm_out dm_tcp_st **ppstOut);
DM_API int dm_tcp_bind(dm_io dm_tcp_st *pstTcp, dm_in const dm_sockaddr_st *pstAddr,
                       dm_in uint32 u32Flags);
DM_API int dm_tcp_listen(dm_io dm_tcp_st *pstTcp, dm_in int iBacklog,
                         dm_in dm_tcp_accept_cb pfOnConn, dm_in void *pvUser);
/* 在 pfOnConn 回调中接受新连接，自动创建并绑定至同一 loop，*ppstClient 归调用方所有 */
DM_API int dm_tcp_accept(dm_io dm_tcp_st *pstServer, dm_out dm_tcp_st **ppstClient);
DM_API int dm_tcp_connect(dm_io dm_tcp_st *pstTcp, dm_in const dm_sockaddr_st *pstAddr,
                          dm_in dm_tcp_connect_cb pfOnConn, dm_in void *pvUser);

DM_API int dm_tcp_nodelay(dm_io dm_tcp_st *pstTcp, dm_in bool_t bEnable);
DM_API int dm_tcp_keepalive(dm_io dm_tcp_st *pstTcp, dm_in bool_t bEnable,
                            dm_in uint32 u32DelaySec);
DM_API int dm_tcp_getsockname(dm_in const dm_tcp_st *pstTcp, dm_out dm_sockaddr_st *pstOut);
DM_API int dm_tcp_getpeername(dm_in const dm_tcp_st *pstTcp, dm_out dm_sockaddr_st *pstOut);

DM_API int dm_tcp_read_start(dm_io dm_tcp_st *pstTcp, dm_in dm_tcp_read_cb pfOnRead,
                             dm_in void *pvUser);
DM_API int dm_tcp_read_stop(dm_io dm_tcp_st *pstTcp);

/* 异步写入（边界锁死）：
 *   - uLen == 0u 或 pvData == NULL -> 返回 DM_INVALID；
 *   - 单次 uLen > DM_SOCK_TX_QUEUE_MAX -> 返回 DM_TOO_BIG；
 *   - queued + uLen > DM_SOCK_TX_QUEUE_MAX -> 返回 DM_BUSY（背压熔断）；
 *   - 已关闭中 -> 返回 DM_SHUTTING_DOWN；
 *   - 内部单块 dm_malloc_tag(sizeof(req) + uLen, DM_MEM_TAG_SOCK) 拷贝，调用方返回即可释放原缓冲。
 */
DM_API int dm_tcp_write(dm_io dm_tcp_st *pstTcp, dm_in const void *pvData, dm_in size_t uLen,
                        dm_in dm_tcp_write_cb pfOnWrite, dm_in void *pvUser);

DM_API size_t dm_tcp_write_queue_size(dm_in const dm_tcp_st *pstTcp);
DM_API int dm_tcp_shutdown(dm_io dm_tcp_st *pstTcp, dm_in dm_tcp_shutdown_cb pfOnDone,
                           dm_in void *pvUser);
DM_API int dm_tcp_close(dm_io dm_tcp_st *pstTcp, dm_in dm_close_fn pfOnClose, dm_in void *pvUser);

DM_API void dm_tcp_set_data(dm_io dm_tcp_st *pstTcp, dm_in void *pvData);
DM_API void *dm_tcp_get_data(dm_in const dm_tcp_st *pstTcp);

/* ============================ 通用异步 UDP（dm_udp） ============================ */

typedef struct dm_udp dm_udp_st;

#define DM_UDP_F_NONE 0u
#define DM_UDP_F_IPV6ONLY (1u << 0)
#define DM_UDP_F_REUSEADDR (1u << 1)

#define DM_UDP_RECV_F_PARTIAL (1u << 0) /* 接收报文因缓冲截断（>65536B） */

typedef enum dm_mcast_op
{
  DM_MCAST_LEAVE = 0,
  DM_MCAST_JOIN = 1
} dm_mcast_op_et;

/* UDP 待发数据报描述（用于 dm_udp_send 保持参数 <= 5） */
typedef struct dm_udp_dgram
{
  const void *pvData;            /* 数据报载荷（uLen == 0 时允许为 NULL） */
  size_t uLen;                   /* 字节长度（0..DM_UDP_PAYLOAD_MAX 65507） */
  const dm_sockaddr_st *pstAddr; /* 目标地址；已 dm_udp_connect 时可为 NULL */
} dm_udp_dgram_st;

/* 收到 UDP 数据报信息（仅在回调期间有效） */
typedef struct dm_udp_rx
{
  const uint8 *pu8Data;
  size_t uLen;
  dm_sockaddr_st stPeer; /* 发送方地址 */
  uint32 u32Flags;       /* DM_UDP_RECV_F_* */
} dm_udp_rx_st;

typedef void (*dm_udp_send_cb)(dm_io dm_udp_st *pstUdp, dm_in int iErr, dm_io void *pvUser);
typedef void (*dm_udp_recv_cb)(dm_io dm_udp_st *pstUdp, dm_in int iErr,
                               dm_in const dm_udp_rx_st *pstRx, dm_io void *pvUser);

DM_API int dm_udp_create(dm_io dm_loop_st *pstLoop, dm_out dm_udp_st **ppstOut);
DM_API int dm_udp_bind(dm_io dm_udp_st *pstUdp, dm_in const dm_sockaddr_st *pstAddr,
                       dm_in uint32 u32Flags);
DM_API int dm_udp_connect(dm_io dm_udp_st *pstUdp, dm_in const dm_sockaddr_st *pstAddr);
DM_API int dm_udp_getsockname(dm_in const dm_udp_st *pstUdp, dm_out dm_sockaddr_st *pstOut);
DM_API int dm_udp_getpeername(dm_in const dm_udp_st *pstUdp, dm_out dm_sockaddr_st *pstOut);

/* 单播/广播/组播选项 */
DM_API int dm_udp_set_broadcast(dm_io dm_udp_st *pstUdp, dm_in bool_t bEnable);
DM_API int dm_udp_set_ttl(dm_io dm_udp_st *pstUdp, dm_in int iTtl); /* 1..255 */
DM_API int dm_udp_set_multicast_loop(dm_io dm_udp_st *pstUdp, dm_in bool_t bEnable);
DM_API int dm_udp_set_multicast_ttl(dm_io dm_udp_st *pstUdp, dm_in int iTtl); /* 0..255 */
DM_API int dm_udp_set_multicast_interface(dm_io dm_udp_st *pstUdp, dm_in const char *pcIfaceAddr);
DM_API int dm_udp_set_membership(dm_io dm_udp_st *pstUdp, dm_in const char *pcMcastAddr,
                                 dm_in const char *pcIfaceAddr, dm_in dm_mcast_op_et eOp);

/* 非阻塞尝试立即发送：
 *   - uLen > DM_UDP_PAYLOAD_MAX (65507) -> 入口直返 DM_TOO_BIG；
 *   - uLen > 0 && pvData == NULL -> DM_INVALID；uLen == 0 允许发送合法 0 字节数据报（pvData 可
 * NULL）；
 *   - 内核缓冲满回 DM_AGAIN（此时可改调 dm_udp_send）；已关闭回 DM_SHUTTING_DOWN。 */
DM_API int dm_udp_try_send(dm_io dm_udp_st *pstUdp, dm_in const void *pvData, dm_in size_t uLen,
                           dm_in const dm_sockaddr_st *pstAddr);

/* 异步发送：入口同样校验 uLen > 65507 -> DM_TOO_BIG、uLen == 0 允许（单块
 * dm_malloc_tag(sizeof(req)+uLen) 绝不触发 dm_malloc(0) 返回 NULL）；受 DM_SOCK_TX_QUEUE_MAX
 * 背压约束（超限回 DM_BUSY）；pfOnSend 可 NULL。 */
DM_API int dm_udp_send(dm_io dm_udp_st *pstUdp, dm_in const dm_udp_dgram_st *pstDgram,
                       dm_in dm_udp_send_cb pfOnSend, dm_in void *pvUser);

DM_API int dm_udp_recv_start(dm_io dm_udp_st *pstUdp, dm_in dm_udp_recv_cb pfOnRecv,
                             dm_in void *pvUser);
DM_API int dm_udp_recv_stop(dm_io dm_udp_st *pstUdp);
DM_API size_t dm_udp_send_queue_size(dm_in const dm_udp_st *pstUdp);
DM_API int dm_udp_close(dm_io dm_udp_st *pstUdp, dm_in dm_close_fn pfOnClose, dm_in void *pvUser);

DM_API void dm_udp_set_data(dm_io dm_udp_st *pstUdp, dm_in void *pvData);
DM_API void *dm_udp_get_data(dm_in const dm_udp_st *pstUdp);

#ifdef __cplusplus
}
#endif

#endif /* DM_SOCK_H */
