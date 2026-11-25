/**
 ******************************************************************************
 * @file    sock_internal.h
 * @author  MDM
 * @version 2.1.0
 * @brief   DM 网络原语内部转换契约（仅供 sock_{addr,dns,tcp,udp}.c 复用，零 dm_internal.h /
 *          llhttp / yyjson 耦合）。
 ******************************************************************************
 */
#ifndef DM_SOCK_INTERNAL_H
#define DM_SOCK_INTERNAL_H

#include <dm/mem.h>
#include <dm/sock.h>
#include "../runtime/loop_internal.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* 纯 C dm_sockaddr_st -> OS sockaddr_storage（IPv4/IPv6；无效族返 DM_INVALID） */
int dm__sockaddr_to_os(dm_in const dm_sockaddr_st *pstIn, dm_out struct sockaddr_storage *pstOut,
                       dm_out int *piLen);

/* OS sockaddr -> 纯 C dm_sockaddr_st（仅支持 AF_INET / AF_INET6，输出先清零） */
int dm__sockaddr_from_os(dm_in const struct sockaddr *pstIn, dm_out dm_sockaddr_st *pstOut);

/* 解析纯数字端口串（NULL 或 "" -> 0 返回 DM_TRUE；"0".."65535" -> 对应值返 DM_TRUE；其余返
 * DM_FALSE） */
bool_t dm__sockaddr_parse_port_str(dm_in const char *pcService, dm_out uint16 *pu16Port);

#ifdef __cplusplus
}
#endif

#endif /* DM_SOCK_INTERNAL_H */
