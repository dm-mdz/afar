/**
 ******************************************************************************
 * @file    sock_addr.c
 * @author  MDM
 * @version 2.1.0
 * @brief   DM 纯 C 套接字地址解析、格式化、文本/二进制转换与多网卡枚举（DD-DM-2.1 §2.3）。
 *          显式解析 IPv6 %<zone>（数字/网卡名三端一致）、uCap == 0 入口守卫，零 dm_internal.h。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "sock_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <netioapi.h>
#else
#include <net/if.h>
#endif

/* ============================ 内部辅助 ============================ */

static bool_t dm__is_all_ascii_digits(dm_in const char *pcStr)
{
  size_t uIdx = 0;

  if ((NULL == pcStr) || ('\0' == pcStr[0]))
  {
    return DM_FALSE;
  }
  for (uIdx = 0; '\0' != pcStr[uIdx]; uIdx++)
  {
    if ((pcStr[uIdx] < '0') || (pcStr[uIdx] > '9'))
    {
      return DM_FALSE;
    }
  }
  return DM_TRUE;
}

static int dm__parse_ipv6_zone(dm_in const char *pcZone, dm_out uint32 *pu32Scope)
{
  uint64 u64Val = 0;
  size_t uIdx = 0;
  unsigned int uiIfIdx = 0;

  if ((NULL == pcZone) || ('\0' == pcZone[0]) || (NULL == pu32Scope))
  {
    return DM_INVALID;
  }
  if (DM_TRUE == dm__is_all_ascii_digits(pcZone))
  {
    for (uIdx = 0; '\0' != pcZone[uIdx]; uIdx++)
    {
      u64Val = (u64Val * 10ull) + (uint64)(pcZone[uIdx] - '0');
      if (u64Val > (uint64)UINT32_MAX)
      {
        return DM_INVALID;
      }
    }
    *pu32Scope = (uint32)u64Val;
    return DM_SUCCESS;
  }
  uiIfIdx = if_nametoindex(pcZone);
  if (0u == uiIfIdx)
  {
    return DM_INVALID;
  }
  *pu32Scope = (uint32)uiIfIdx;
  return DM_SUCCESS;
}

static bool_t dm__has_nonzero_mac(dm_in const char acPhys[6])
{
  size_t uIdx = 0;

  for (uIdx = 0; uIdx < 6u; uIdx++)
  {
    if (0 != acPhys[uIdx])
    {
      return DM_TRUE;
    }
  }
  return DM_FALSE;
}

int dm__sockaddr_to_os(dm_in const dm_sockaddr_st *pstIn, dm_out struct sockaddr_storage *pstOut,
                       dm_out int *piLen)
{
  if ((NULL == pstIn) || (NULL == pstOut) || (NULL == piLen))
  {
    return DM_INVALID;
  }
  memset(pstOut, 0, sizeof(*pstOut));
  if (DM_AF_IPV4 == pstIn->eFamily)
  {
    struct sockaddr_in *pstV4 = (struct sockaddr_in *)pstOut;
    pstV4->sin_family = AF_INET;
    pstV4->sin_port = htons(pstIn->u16Port);
    memcpy(&pstV4->sin_addr, pstIn->au8Addr, 4u);
    *piLen = (int)sizeof(struct sockaddr_in);
    return DM_SUCCESS;
  }
  if (DM_AF_IPV6 == pstIn->eFamily)
  {
    struct sockaddr_in6 *pstV6 = (struct sockaddr_in6 *)pstOut;
    pstV6->sin6_family = AF_INET6;
    pstV6->sin6_port = htons(pstIn->u16Port);
    memcpy(&pstV6->sin6_addr, pstIn->au8Addr, 16u);
    pstV6->sin6_scope_id = pstIn->u32ScopeId;
    *piLen = (int)sizeof(struct sockaddr_in6);
    return DM_SUCCESS;
  }
  return DM_INVALID;
}

int dm__sockaddr_from_os(dm_in const struct sockaddr *pstIn, dm_out dm_sockaddr_st *pstOut)
{
  if ((NULL == pstIn) || (NULL == pstOut))
  {
    return DM_INVALID;
  }
  memset(pstOut, 0, sizeof(*pstOut));
  if (AF_INET == pstIn->sa_family)
  {
    const struct sockaddr_in *pstV4 = (const struct sockaddr_in *)pstIn;
    pstOut->eFamily = DM_AF_IPV4;
    pstOut->u16Port = ntohs(pstV4->sin_port);
    memcpy(pstOut->au8Addr, &pstV4->sin_addr, 4u);
    return DM_SUCCESS;
  }
  if (AF_INET6 == pstIn->sa_family)
  {
    const struct sockaddr_in6 *pstV6 = (const struct sockaddr_in6 *)pstIn;
    pstOut->eFamily = DM_AF_IPV6;
    pstOut->u16Port = ntohs(pstV6->sin6_port);
    memcpy(pstOut->au8Addr, &pstV6->sin6_addr, 16u);
    pstOut->u32ScopeId = pstV6->sin6_scope_id;
    return DM_SUCCESS;
  }
  return DM_INVALID;
}

bool_t dm__sockaddr_parse_port_str(dm_in const char *pcService, dm_out uint16 *pu16Port)
{
  uint64 u64Val = 0;
  size_t uIdx = 0;

  if (NULL == pu16Port)
  {
    return DM_FALSE;
  }
  if ((NULL == pcService) || ('\0' == pcService[0]))
  {
    *pu16Port = 0u;
    return DM_TRUE;
  }
  if (DM_TRUE != dm__is_all_ascii_digits(pcService))
  {
    return DM_FALSE;
  }
  for (uIdx = 0; '\0' != pcService[uIdx]; uIdx++)
  {
    u64Val = (u64Val * 10ull) + (uint64)(pcService[uIdx] - '0');
    if (u64Val > 65535ull)
    {
      return DM_FALSE;
    }
  }
  *pu16Port = (uint16)u64Val;
  return DM_TRUE;
}

/* ============================ 公开地址与网卡 API ============================ */

int dm_inet_pton(dm_in dm_af_et eFamily, dm_in const char *pcSrc, dm_out uint8 au8Dst[16])
{
  uint8 au8Tmp[16] = {0};
  int iErr = 0;

  if ((NULL == pcSrc) || ('\0' == pcSrc[0]) || (NULL == au8Dst))
  {
    return DM_INVALID;
  }
  if (DM_AF_IPV4 == eFamily)
  {
    iErr = dm_uv_inet_pton(AF_INET, pcSrc, au8Tmp);
    if (0 != iErr)
    {
      return DM_INVALID;
    }
    memset(au8Dst, 0, 16u);
    memcpy(au8Dst, au8Tmp, 4u);
    return DM_SUCCESS;
  }
  if (DM_AF_IPV6 == eFamily)
  {
    iErr = dm_uv_inet_pton(AF_INET6, pcSrc, au8Tmp);
    if (0 != iErr)
    {
      return DM_INVALID;
    }
    memcpy(au8Dst, au8Tmp, 16u);
    return DM_SUCCESS;
  }
  return DM_INVALID;
}

int dm_inet_ntop(dm_in dm_af_et eFamily, dm_in const uint8 au8Src[16], dm_out char *pcDst,
                 dm_in size_t uCap)
{
  int iAf = 0;
  int iErr = 0;

  /* AUD-V21-R2-P1-04：入口前置校验 uCap == 0 -> DM_INVALID */
  if ((NULL == au8Src) || (NULL == pcDst) || (0u == uCap))
  {
    return DM_INVALID;
  }
  if (DM_AF_IPV4 == eFamily)
  {
    iAf = AF_INET;
  }
  else if (DM_AF_IPV6 == eFamily)
  {
    iAf = AF_INET6;
  }
  else
  {
    return DM_INVALID;
  }
  iErr = dm_uv_inet_ntop(iAf, au8Src, pcDst, uCap);
  return dm__uv_err_map(iErr);
}

int dm_sockaddr_ip4(dm_in const char *pcIp, dm_in uint16 u16Port, dm_out dm_sockaddr_st *pstOut)
{
  uint8 au8Tmp[16] = {0};
  int iErr = 0;

  if ((NULL == pcIp) || ('\0' == pcIp[0]) || (NULL == pstOut))
  {
    return DM_INVALID;
  }
  iErr = dm_inet_pton(DM_AF_IPV4, pcIp, au8Tmp);
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  memset(pstOut, 0, sizeof(*pstOut));
  pstOut->eFamily = DM_AF_IPV4;
  pstOut->u16Port = u16Port;
  memcpy(pstOut->au8Addr, au8Tmp, 4u);
  return DM_SUCCESS;
}

int dm_sockaddr_ip6(dm_in const char *pcIp, dm_in uint16 u16Port, dm_out dm_sockaddr_st *pstOut)
{
  uint8 au8Tmp[16] = {0};
  char acPureIp[64] = {0};
  const char *pcPct = NULL;
  uint32 u32Scope = 0u;
  size_t uIpLen = 0;
  int iErr = 0;

  if ((NULL == pcIp) || ('\0' == pcIp[0]) || (NULL == pstOut))
  {
    return DM_INVALID;
  }
  /* AUD-V21-R2-P0-01：显式解析 %<zone>，三端一致支持数字 scope 与网卡名 */
  pcPct = strchr(pcIp, '%');
  if (NULL == pcPct)
  {
    iErr = dm_inet_pton(DM_AF_IPV6, pcIp, au8Tmp);
    if (DM_SUCCESS != iErr)
    {
      return iErr;
    }
  }
  else
  {
    uIpLen = (size_t)(pcPct - pcIp);
    if ((0u == uIpLen) || (uIpLen >= sizeof(acPureIp)))
    {
      return DM_INVALID;
    }
    memcpy(acPureIp, pcIp, uIpLen);
    acPureIp[uIpLen] = '\0';
    iErr = dm_inet_pton(DM_AF_IPV6, acPureIp, au8Tmp);
    if (DM_SUCCESS != iErr)
    {
      return iErr;
    }
    iErr = dm__parse_ipv6_zone(pcPct + 1, &u32Scope);
    if (DM_SUCCESS != iErr)
    {
      return iErr;
    }
  }
  memset(pstOut, 0, sizeof(*pstOut));
  pstOut->eFamily = DM_AF_IPV6;
  pstOut->u16Port = u16Port;
  memcpy(pstOut->au8Addr, au8Tmp, 16u);
  pstOut->u32ScopeId = u32Scope;
  return DM_SUCCESS;
}

int dm_sockaddr_format(dm_in const dm_sockaddr_st *pstAddr, dm_out char *pcBuf, dm_in size_t uCap)
{
  char acIp[64] = {0};
  char acFull[DM_SOCKADDR_STR_MAX] = {0};
  int iErr = 0;
  int iLen = 0;

  /* AUD-V21-R2-P1-04：入口前置校验 uCap == 0 -> DM_INVALID */
  if ((NULL == pstAddr) || (NULL == pcBuf) || (0u == uCap))
  {
    return DM_INVALID;
  }
  iErr = dm_inet_ntop(pstAddr->eFamily, pstAddr->au8Addr, acIp, sizeof(acIp));
  if (DM_SUCCESS != iErr)
  {
    return iErr;
  }
  if (DM_AF_IPV4 == pstAddr->eFamily)
  {
    if (0u == pstAddr->u16Port)
    {
      iLen = snprintf(acFull, sizeof(acFull), "%s", acIp);
    }
    else
    {
      iLen = snprintf(acFull, sizeof(acFull), "%s:%u", acIp, (unsigned int)pstAddr->u16Port);
    }
  }
  else if (0u == pstAddr->u16Port)
  {
    if (0u == pstAddr->u32ScopeId)
    {
      iLen = snprintf(acFull, sizeof(acFull), "%s", acIp);
    }
    else
    {
      iLen = snprintf(acFull, sizeof(acFull), "%s%%%u", acIp, (unsigned int)pstAddr->u32ScopeId);
    }
  }
  else if (0u == pstAddr->u32ScopeId)
  {
    iLen = snprintf(acFull, sizeof(acFull), "[%s]:%u", acIp, (unsigned int)pstAddr->u16Port);
  }
  else
  {
    iLen = snprintf(acFull, sizeof(acFull), "[%s%%%u]:%u", acIp, (unsigned int)pstAddr->u32ScopeId,
                    (unsigned int)pstAddr->u16Port);
  }
  if ((iLen < 0) || ((size_t)iLen >= sizeof(acFull)))
  {
    return DM_TOO_BIG;
  }
  if (uCap <= (size_t)iLen)
  {
    return DM_TOO_BIG;
  }
  memcpy(pcBuf, acFull, (size_t)iLen + 1u);
  return DM_SUCCESS;
}

static bool_t dm__ifaddr_is_ip(dm_in const dm_uv_interface_address_st *pstIf)
{
  int iFam = pstIf->address.address4.sin_family;
  return ((AF_INET == iFam) || (AF_INET6 == iFam)) ? DM_TRUE : DM_FALSE;
}

int dm_ifaddrs_list(dm_out dm_ifaddr_st **ppastOut, dm_out uint32 *pu32Count)
{
  dm_uv_interface_address_st *pastUv = NULL;
  dm_ifaddr_st *pastOut = NULL;
  int iUvCount = 0;
  int iIdx = 0;
  int iErr = 0;
  uint32 u32Valid = 0u;
  uint32 u32Out = 0u;

  if (NULL != ppastOut)
  {
    *ppastOut = NULL;
  }
  if (NULL != pu32Count)
  {
    *pu32Count = 0u;
  }
  if ((NULL == ppastOut) || (NULL == pu32Count))
  {
    return DM_INVALID;
  }
  iErr = dm_uv_interface_addresses(&pastUv, &iUvCount);
  if (0 != iErr)
  {
    return dm__uv_err_map(iErr);
  }
  for (iIdx = 0; iIdx < iUvCount; iIdx++)
  {
    if (DM_TRUE == dm__ifaddr_is_ip(&pastUv[iIdx]))
    {
      u32Valid++;
    }
  }
  if (0u == u32Valid)
  {
    dm_uv_free_interface_addresses(pastUv, iUvCount);
    return DM_SUCCESS;
  }
  pastOut = (dm_ifaddr_st *)dm_calloc_tag(u32Valid, sizeof(dm_ifaddr_st), DM_MEM_TAG_SOCK);
  if (NULL == pastOut)
  {
    dm_uv_free_interface_addresses(pastUv, iUvCount);
    return DM_ALLOC;
  }
  for (iIdx = 0; iIdx < iUvCount; iIdx++)
  {
    const dm_uv_interface_address_st *pstSrc = &pastUv[iIdx];
    dm_ifaddr_st *pstDst = NULL;
    if (DM_TRUE != dm__ifaddr_is_ip(pstSrc))
    {
      continue;
    }
    pstDst = &pastOut[u32Out++];
    (void)snprintf(pstDst->acName, sizeof(pstDst->acName), "%s",
                   (NULL != pstSrc->name) ? pstSrc->name : "");
    memcpy(pstDst->au8PhysAddr, pstSrc->phys_addr, 6u);
    pstDst->bHasPhysAddr = dm__has_nonzero_mac(pstSrc->phys_addr);
    pstDst->bIsInternal = (0 != pstSrc->is_internal) ? DM_TRUE : DM_FALSE;
    (void)dm__sockaddr_from_os((const struct sockaddr *)&pstSrc->address, &pstDst->stAddr);
    pstDst->stAddr.u16Port = 0u;
    (void)dm__sockaddr_from_os((const struct sockaddr *)&pstSrc->netmask, &pstDst->stNetmask);
    pstDst->stNetmask.u16Port = 0u;
  }
  dm_uv_free_interface_addresses(pastUv, iUvCount);
  *ppastOut = pastOut;
  *pu32Count = u32Out;
  return DM_SUCCESS;
}

void dm_ifaddrs_free(dm_io dm_ifaddr_st *pastList, dm_in uint32 u32Count)
{
  (void)u32Count;
  if (NULL != pastList)
  {
    dm_free_tag(pastList);
  }
  return;
}

#ifdef __cplusplus
}
#endif
