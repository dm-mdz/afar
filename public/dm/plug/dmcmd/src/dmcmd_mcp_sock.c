/**
 ******************************************************************************
 * @file    dmcmd_mcp_sock.c
 * @brief   dmcmd mcp 跨平台非阻塞 TCP Socket 原语（连接超时、带超时写满、空闲超时读）。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "dmcmd_mcp_int.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <limits.h>
#include <string.h>

void dmcmd_mcp_sock_close(in dmcmd_mcp_sock_t iSock)
{
  if (DMCMD_MCP_SOCK_BAD == iSock)
  {
    return;
  }
#ifdef _WIN32
  (void)closesocket((SOCKET)iSock);
#else
  (void)close(iSock);
#endif
  return;
}

static void dmcmd__mcp_set_timeouts(in dmcmd_mcp_sock_t iSock, in int iMs)
{
  int iUse = (iMs > 0) ? iMs : 5000;
#ifdef _WIN32
  DWORD dwMs = (DWORD)iUse;
  (void)setsockopt((SOCKET)iSock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&dwMs, sizeof(dwMs));
  (void)setsockopt((SOCKET)iSock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&dwMs, sizeof(dwMs));
#else
  struct timeval stTv;
  stTv.tv_sec = iUse / 1000;
  stTv.tv_usec = (iUse % 1000) * 1000;
  (void)setsockopt(iSock, SOL_SOCKET, SO_RCVTIMEO, &stTv, sizeof(stTv));
  (void)setsockopt(iSock, SOL_SOCKET, SO_SNDTIMEO, &stTv, sizeof(stTv));
#endif
  return;
}

static int dmcmd__mcp_wait_sock(in dmcmd_mcp_sock_t iSock, in bool_t bWrite, in int iMs)
{
  uint64 u64End = (iMs > 0) ? (dmc_now_ms() + (uint64)iMs) : 0u;

  for (;;)
  {
    fd_set stSet;
    struct timeval stTv;
    int iRet = 0;
    int iRem = iMs;

    if (iMs > 0)
    {
      uint64 u64Now = dmc_now_ms();
      if (u64Now >= u64End)
      {
        return 0;
      }
      iRem = (int)(u64End - u64Now);
    }
    FD_ZERO(&stSet);
#ifdef _WIN32
    FD_SET((SOCKET)iSock, &stSet);
#else
    if ((iSock < 0) || (iSock >= FD_SETSIZE))
    {
      return -1;
    }
    FD_SET(iSock, &stSet);
#endif
    if (iRem >= 0)
    {
      stTv.tv_sec = iRem / 1000;
      stTv.tv_usec = (iRem % 1000) * 1000;
    }
#ifdef _WIN32
    {
      fd_set stExc;
      FD_ZERO(&stExc);
      FD_SET((SOCKET)iSock, &stExc);
      iRet = select(0, bWrite ? NULL : &stSet, bWrite ? &stSet : NULL, bWrite ? &stExc : NULL,
                    (iRem < 0) ? NULL : &stTv);
      if ((iRet > 0) && bWrite && FD_ISSET((SOCKET)iSock, &stExc))
      {
        return -1;
      }
      return (iRet > 0) ? 1 : ((0 == iRet) ? 0 : -1);
    }
#else
    iRet = select(iSock + 1, bWrite ? NULL : &stSet, bWrite ? &stSet : NULL, NULL,
                  (iRem < 0) ? NULL : &stTv);
    if ((iRet < 0) && (EINTR == errno))
    {
      if (0 == iMs)
      {
        return 0;
      }
      continue;
    }
    return (iRet > 0) ? 1 : ((0 == iRet) ? 0 : -1);
#endif
  }
}

static dmcmd_mcp_sock_t dmcmd__mcp_connect_one(in const struct addrinfo *pstAi, in int iMs)
{
  dmcmd_mcp_sock_t iSock = DMCMD_MCP_SOCK_BAD;
  int iErr = 0;
  int iOne = 1;
#ifdef _WIN32
  int iLen = (int)sizeof(iErr);
  u_long ulNonBlk = 1u;
#else
  socklen_t iLen = (socklen_t)sizeof(iErr);
  int iFl = 0;
#endif

  iSock = (dmcmd_mcp_sock_t)socket(pstAi->ai_family, pstAi->ai_socktype, pstAi->ai_protocol);
  if (DMCMD_MCP_SOCK_BAD == iSock)
  {
    return DMCMD_MCP_SOCK_BAD;
  }
#ifdef _WIN32
  (void)ioctlsocket((SOCKET)iSock, FIONBIO, &ulNonBlk);
#else
  iFl = fcntl(iSock, F_GETFL, 0);
  if (iFl >= 0)
  {
    (void)fcntl(iSock, F_SETFL, iFl | O_NONBLOCK);
  }
#endif
  if (0 != connect(iSock, pstAi->ai_addr, (int)pstAi->ai_addrlen))
  {
#ifdef _WIN32
    int iWsa = WSAGetLastError();
    if ((WSAEWOULDBLOCK != iWsa) && (WSAEINPROGRESS != iWsa))
    {
      dmcmd_mcp_sock_close(iSock);
      return DMCMD_MCP_SOCK_BAD;
    }
#else
    if ((EINPROGRESS != errno) && (EWOULDBLOCK != errno))
    {
      dmcmd_mcp_sock_close(iSock);
      return DMCMD_MCP_SOCK_BAD;
    }
#endif
    if ((1 != dmcmd__mcp_wait_sock(iSock, DM_TRUE, iMs)) ||
        (0 != getsockopt(iSock, SOL_SOCKET, SO_ERROR, (char *)&iErr, &iLen)) || (0 != iErr))
    {
      dmcmd_mcp_sock_close(iSock);
      return DMCMD_MCP_SOCK_BAD;
    }
  }
  dmcmd__mcp_set_timeouts(iSock, iMs);
  (void)setsockopt(iSock, IPPROTO_TCP, TCP_NODELAY, (const char *)&iOne, sizeof(iOne));
#ifdef SO_NOSIGPIPE
  (void)setsockopt(iSock, SOL_SOCKET, SO_NOSIGPIPE, &iOne, sizeof(iOne));
#endif
  return iSock;
}

dmcmd_mcp_sock_t dmcmd_mcp_connect(in const char *pcHost, in int iPort, in uint32 u32Ms)
{
  char acPort[16];
  struct addrinfo stHints;
  struct addrinfo *pstRes = NULL;
  struct addrinfo *pstCur = NULL;
  dmcmd_mcp_sock_t iSock = DMCMD_MCP_SOCK_BAD;
  int iTotalMs = (u32Ms > (uint32)INT_MAX) ? INT_MAX : (int)((0u != u32Ms) ? u32Ms : 3000u);
  uint64 u64End = dmc_now_ms() + (uint64)iTotalMs;

  snprintf(acPort, sizeof(acPort), "%d", iPort);
  memset(&stHints, 0, sizeof(stHints));
  stHints.ai_family = AF_UNSPEC;
  stHints.ai_socktype = SOCK_STREAM;
  stHints.ai_protocol = IPPROTO_TCP;
  if (0 != getaddrinfo(pcHost, acPort, &stHints, &pstRes))
  {
    return DMCMD_MCP_SOCK_BAD;
  }
  for (pstCur = pstRes; NULL != pstCur; pstCur = pstCur->ai_next)
  {
    uint64 u64Now = dmc_now_ms();
    int iRem = (u64Now < u64End) ? (int)(u64End - u64Now) : 1;
    iSock = dmcmd__mcp_connect_one(pstCur, iRem);
    if (DMCMD_MCP_SOCK_BAD != iSock)
    {
      break;
    }
    if (dmc_now_ms() >= u64End)
    {
      break;
    }
  }
  freeaddrinfo(pstRes);
  return iSock;
}

int dmcmd_mcp_send_all(in dmcmd_mcp_sock_t iSock, in const void *pvBuf, in size_t uLen,
                       in int iTimeoutMs)
{
  const char *pc = (const char *)pvBuf;
  int iLim = (iTimeoutMs > 0) ? iTimeoutMs : 5000;
  uint64 u64End = dmc_now_ms() + (uint64)iLim;
#ifdef MSG_NOSIGNAL
  int iFlags = MSG_NOSIGNAL;
#else
  int iFlags = 0;
#endif
  while (uLen > 0u)
  {
    uint64 u64Now = dmc_now_ms();
    int iRem = 0;
    size_t uChunk = (uLen > (size_t)0x40000000u) ? (size_t)0x40000000u : uLen;
    if (u64Now >= u64End)
    {
      return -1;
    }
    iRem = (int)(u64End - u64Now);
    if (1 != dmcmd__mcp_wait_sock(iSock, DM_TRUE, iRem))
    {
      return -1;
    }
#ifdef _WIN32
    {
      int iN = send((SOCKET)iSock, pc, (int)uChunk, iFlags);
      if (iN < 0)
      {
        if (WSAEWOULDBLOCK == WSAGetLastError())
        {
          continue;
        }
        return -1;
      }
      if (0 == iN)
      {
        return -1;
      }
      pc += iN;
      uLen -= (size_t)iN;
    }
#else
    {
      ssize_t iN = send(iSock, pc, uChunk, iFlags);
      if (iN < 0)
      {
        if ((EINTR == errno) || (EAGAIN == errno) || (EWOULDBLOCK == errno))
        {
          continue;
        }
        return -1;
      }
      if (0 == iN)
      {
        return -1;
      }
      pc += iN;
      uLen -= (size_t)iN;
    }
#endif
  }
  return 0;
}

int dmcmd_mcp_recv_idle(in dmcmd_mcp_sock_t iSock, out void *pvBuf, in size_t uCap, in int iIdleMs)
{
  uint64 u64End = (iIdleMs > 0) ? (dmc_now_ms() + (uint64)iIdleMs) : 0u;
  size_t uWant = (uCap > (size_t)0x40000000u) ? (size_t)0x40000000u : uCap;

  for (;;)
  {
    int iRem = iIdleMs;
    int iReady = 0;
    if (iIdleMs > 0)
    {
      uint64 u64Now = dmc_now_ms();
      if (u64Now >= u64End)
      {
        return -2;
      }
      iRem = (int)(u64End - u64Now);
    }
    iReady = dmcmd__mcp_wait_sock(iSock, DM_FALSE, iRem);
    if (0 == iReady)
    {
      return -2;
    }
    if (iReady < 0)
    {
      return -1;
    }
#ifdef _WIN32
    {
      int iN = recv((SOCKET)iSock, (char *)pvBuf, (int)uWant, 0);
      if (iN >= 0)
      {
        return iN;
      }
      if (WSAEWOULDBLOCK == WSAGetLastError())
      {
        continue;
      }
      return -1;
    }
#else
    {
      ssize_t iN = recv(iSock, pvBuf, uWant, 0);
      if (iN >= 0)
      {
        return (int)iN;
      }
      if ((EINTR == errno) || (EAGAIN == errno) || (EWOULDBLOCK == errno))
      {
        continue;
      }
      return -1;
    }
#endif
  }
}

#ifdef __cplusplus
}
#endif
