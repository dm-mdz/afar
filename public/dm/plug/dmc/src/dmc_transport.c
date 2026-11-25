/**
 ******************************************************************************
 * @file    dmc_transport.c
 * @brief   dmc 上层可插拔传输适配表（DD-02 §2.4，PLUG-01/03）：
 *          一期落地 local:// 与 tcp://（BSD Socket / Winsock，非阻塞 + select 读写超时）；
 *          ws:// 与 serial:// 登记在统一 vtable 中，显式返回 DMC_ERR_UNSUPPORTED 随时可插拔替换。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif

#include <dmc_client.h>

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DMC__TR_MAX 16u
#define DMC__SCHEME_MAX 32u
#define DMC__WRITE_TIMEOUT_MS 15000

#ifdef _WIN32
typedef uintptr_t dmc__sock_t;
#define DMC__SOCK_BAD ((dmc__sock_t)(~(uintptr_t)0))
static SRWLOCK g_stDmcTrLock = SRWLOCK_INIT;
static INIT_ONCE g_stDmcWsaOnce = INIT_ONCE_STATIC_INIT;
static int g_iDmcWsaErr = 0;
static BOOL CALLBACK dmc__wsa_once_cb(PINIT_ONCE pstOnce, PVOID pvParam, PVOID *ppvCtx)
{
  WSADATA stWsa;
  (void)pstOnce;
  (void)pvParam;
  (void)ppvCtx;
  g_iDmcWsaErr = (0 == WSAStartup(MAKEWORD(2, 2), &stWsa)) ? 0 : -1;
  return TRUE;
}
static void dmc__tr_lock(void)
{
  AcquireSRWLockExclusive(&g_stDmcTrLock);
}
static void dmc__tr_unlock(void)
{
  ReleaseSRWLockExclusive(&g_stDmcTrLock);
}
#else
typedef int dmc__sock_t;
#define DMC__SOCK_BAD (-1)
static pthread_mutex_t g_stDmcTrLock = PTHREAD_MUTEX_INITIALIZER;
static void dmc__tr_lock(void)
{
  (void)pthread_mutex_lock(&g_stDmcTrLock);
}
static void dmc__tr_unlock(void)
{
  (void)pthread_mutex_unlock(&g_stDmcTrLock);
}
#endif

typedef struct dmc__tcp_handle
{
  dmc__sock_t iSock;
  int iWriteTimeoutMs;
} dmc__tcp_handle_st;

typedef struct dmc__tr_entry
{
  char acScheme[DMC__SCHEME_MAX];
  dmc_transport_vtbl_st stVtbl;
} dmc__tr_entry_st;

static dmc__tr_entry_st g_astDmcTr[DMC__TR_MAX];
static uint g_uiDmcTrCount = 0u;
static bool_t g_bDmcTrInited = DM_FALSE;

/* ============================ 工具函数 ============================ */

static unsigned char dmc__ascii_lower(dm_in unsigned char u8Ch)
{
  return ((u8Ch >= 'A') && (u8Ch <= 'Z')) ? (unsigned char)(u8Ch - 'A' + 'a') : u8Ch;
}

static bool_t dmc__scheme_eq(dm_in const char *pcA, dm_in const char *pcB)
{
  size_t uIdx = 0u;
  if ((NULL == pcA) || (NULL == pcB))
  {
    return DM_FALSE;
  }
  while (('\0' != pcA[uIdx]) && ('\0' != pcB[uIdx]))
  {
    if (dmc__ascii_lower((unsigned char)pcA[uIdx]) != dmc__ascii_lower((unsigned char)pcB[uIdx]))
    {
      return DM_FALSE;
    }
    uIdx++;
  }
  return (pcA[uIdx] == pcB[uIdx]) ? DM_TRUE : DM_FALSE;
}

static int dmc__clamp_ms(dm_in uint32 u32Ms)
{
  return (u32Ms > (uint32)INT_MAX) ? INT_MAX : (int)u32Ms;
}

/* ============================ TCP / Loopback 底层 ============================ */

static int dmc__net_init(void)
{
#ifdef _WIN32
  if (!InitOnceExecuteOnce(&g_stDmcWsaOnce, dmc__wsa_once_cb, NULL, NULL))
  {
    return -1;
  }
  return g_iDmcWsaErr;
#else
  return 0;
#endif
}

static void dmc__sock_close(dm_in dmc__sock_t iSock)
{
  if (DMC__SOCK_BAD == iSock)
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

static void dmc__sock_set_nonblock(dm_in dmc__sock_t iSock)
{
#ifdef _WIN32
  u_long ulMode = 1u;
  (void)ioctlsocket((SOCKET)iSock, FIONBIO, &ulMode);
#else
  int iFlags = fcntl(iSock, F_GETFL, 0);
  if (iFlags >= 0)
  {
    (void)fcntl(iSock, F_SETFL, iFlags | O_NONBLOCK);
  }
#endif
  return;
}

static void dmc__sock_set_timeouts(dm_in dmc__sock_t iSock, dm_in int iMs)
{
#ifdef _WIN32
  DWORD dwMs = (DWORD)((iMs > 0) ? iMs : 5000);
  (void)setsockopt((SOCKET)iSock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&dwMs, sizeof(dwMs));
  (void)setsockopt((SOCKET)iSock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&dwMs, sizeof(dwMs));
#else
  struct timeval stTv;
  int iUse = (iMs > 0) ? iMs : 5000;
  stTv.tv_sec = iUse / 1000;
  stTv.tv_usec = (iUse % 1000) * 1000;
  (void)setsockopt(iSock, SOL_SOCKET, SO_RCVTIMEO, &stTv, sizeof(stTv));
  (void)setsockopt(iSock, SOL_SOCKET, SO_SNDTIMEO, &stTv, sizeof(stTv));
#endif
  return;
}

/* 等可读或可写（支持 POSIX EINTR 余量续等）：回 1 就绪，0 超时，-1 错误 */
static int dmc__sock_wait(dm_in dmc__sock_t iSock, dm_in bool_t bWrite, dm_in int iMs)
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

/* 解析 "scheme://host:port" 或 "scheme://[ipv6]:port"，端口严检纯数字 1..65535 */
static bool_t dmc__parse_host_port(dm_in const char *pcUri, dm_out char *pcHost,
                                   dm_in size_t uHostCap, dm_out char *pcPort,
                                   dm_in size_t uPortCap)
{
  const char *pcSep = NULL;
  const char *pcAuth = NULL;
  const char *pcColon = NULL;
  const char *pcEnd = NULL;
  size_t uHostLen = 0u;
  size_t uPortLen = 0u;
  size_t uIdx = 0u;
  unsigned long ulPort = 0ul;

  if (NULL == pcUri)
  {
    return DM_FALSE;
  }
  pcSep = strstr(pcUri, "://");
  pcAuth = (NULL != pcSep) ? (pcSep + 3) : pcUri;
  pcEnd = pcAuth;
  while (('\0' != *pcEnd) && ('/' != *pcEnd) && ('?' != *pcEnd) && ('#' != *pcEnd))
  {
    pcEnd++;
  }
  if ('[' == *pcAuth)
  {
    const char *pcClose = (const char *)memchr(pcAuth, ']', (size_t)(pcEnd - pcAuth));
    if ((NULL == pcClose) || (pcClose + 1 >= pcEnd) || (':' != pcClose[1]))
    {
      return DM_FALSE;
    }
    uHostLen = (size_t)(pcClose - (pcAuth + 1));
    pcColon = pcClose + 1;
    pcAuth++;
  }
  else
  {
    const char *pcP = pcEnd;
    while (pcP > pcAuth)
    {
      pcP--;
      if (':' == *pcP)
      {
        pcColon = pcP;
        break;
      }
    }
    if (NULL == pcColon)
    {
      return DM_FALSE;
    }
    uHostLen = (size_t)(pcColon - pcAuth);
  }
  uPortLen = (size_t)(pcEnd - (pcColon + 1));
  if ((0u == uHostLen) || (uHostLen >= uHostCap) || (0u == uPortLen) || (uPortLen >= uPortCap))
  {
    return DM_FALSE;
  }
  for (uIdx = 0u; uIdx < uPortLen; uIdx++)
  {
    unsigned char u8Ch = (unsigned char)pcColon[1u + uIdx];
    if (!isdigit(u8Ch))
    {
      return DM_FALSE;
    }
    ulPort = ulPort * 10ul + (unsigned long)(u8Ch - '0');
    if (ulPort > 65535ul)
    {
      return DM_FALSE;
    }
  }
  if (0ul == ulPort)
  {
    return DM_FALSE;
  }
  memcpy(pcHost, pcAuth, uHostLen);
  pcHost[uHostLen] = '\0';
  memcpy(pcPort, pcColon + 1, uPortLen);
  pcPort[uPortLen] = '\0';
  return DM_TRUE;
}

static dmc__sock_t dmc__tcp_connect_one(dm_in const struct addrinfo *pstAi, dm_in int iTimeoutMs)
{
  dmc__sock_t iSock = DMC__SOCK_BAD;
  int iErr = 0;
  int iOne = 1;
#ifdef _WIN32
  int iLen = (int)sizeof(iErr);
#else
  socklen_t iLen = (socklen_t)sizeof(iErr);
#endif

  iSock = (dmc__sock_t)socket(pstAi->ai_family, pstAi->ai_socktype, pstAi->ai_protocol);
  if (DMC__SOCK_BAD == iSock)
  {
    return DMC__SOCK_BAD;
  }
  dmc__sock_set_nonblock(iSock);
  if (0 != connect(iSock, pstAi->ai_addr, (int)pstAi->ai_addrlen))
  {
#ifdef _WIN32
    int iWsa = WSAGetLastError();
    if ((WSAEWOULDBLOCK != iWsa) && (WSAEINPROGRESS != iWsa))
    {
      dmc__sock_close(iSock);
      return DMC__SOCK_BAD;
    }
#else
    if ((EINPROGRESS != errno) && (EWOULDBLOCK != errno))
    {
      dmc__sock_close(iSock);
      return DMC__SOCK_BAD;
    }
#endif
    if (1 != dmc__sock_wait(iSock, DM_TRUE, iTimeoutMs))
    {
      dmc__sock_close(iSock);
      return DMC__SOCK_BAD;
    }
    if ((0 != getsockopt(iSock, SOL_SOCKET, SO_ERROR, (char *)&iErr, &iLen)) || (0 != iErr))
    {
      dmc__sock_close(iSock);
      return DMC__SOCK_BAD;
    }
  }
  /* 保持非阻塞 + SO_RCVTIMEO/SO_SNDTIMEO，配合 select 彻底杜绝虚假就绪或对端卡死 */
  dmc__sock_set_timeouts(iSock, iTimeoutMs);
  (void)setsockopt(iSock, IPPROTO_TCP, TCP_NODELAY, (const char *)&iOne, sizeof(iOne));
#ifdef SO_NOSIGPIPE
  (void)setsockopt(iSock, SOL_SOCKET, SO_NOSIGPIPE, &iOne, sizeof(iOne));
#endif
  return iSock;
}

static int dmc__tcp_open(dm_in const char *pcUri, dm_in uint32 u32TimeoutMs,
                         dm_out void **ppvHandle)
{
  char acHost[256];
  char acPort[16];
  struct addrinfo stHints;
  struct addrinfo *pstRes = NULL;
  struct addrinfo *pstCur = NULL;
  dmc__sock_t iSock = DMC__SOCK_BAD;
  dmc__tcp_handle_st *pstHandle = NULL;
  int iTotalMs = dmc__clamp_ms((0u != u32TimeoutMs) ? u32TimeoutMs : 5000u);
  uint64 u64End = dmc_now_ms() + (uint64)iTotalMs;

  if ((NULL == pcUri) || (NULL == ppvHandle))
  {
    return DMC_ERR_ARG;
  }
  *ppvHandle = NULL;
  if (0 != dmc__net_init())
  {
    return DMC_ERR_NOT_CON;
  }
  if (!dmc__parse_host_port(pcUri, acHost, sizeof(acHost), acPort, sizeof(acPort)))
  {
    return DMC_ERR_ARG;
  }
  memset(&stHints, 0, sizeof(stHints));
  stHints.ai_family = AF_UNSPEC;
  stHints.ai_socktype = SOCK_STREAM;
  stHints.ai_protocol = IPPROTO_TCP;
  if (0 != getaddrinfo(acHost, acPort, &stHints, &pstRes))
  {
    return DMC_ERR_NOT_CON;
  }
  for (pstCur = pstRes; NULL != pstCur; pstCur = pstCur->ai_next)
  {
    uint64 u64Now = dmc_now_ms();
    int iRem = (u64Now < u64End) ? (int)(u64End - u64Now) : 1;
    iSock = dmc__tcp_connect_one(pstCur, iRem);
    if (DMC__SOCK_BAD != iSock)
    {
      break;
    }
    if (dmc_now_ms() >= u64End)
    {
      break;
    }
  }
  freeaddrinfo(pstRes);
  if (DMC__SOCK_BAD == iSock)
  {
    return DMC_ERR_NOT_CON;
  }
  pstHandle = (dmc__tcp_handle_st *)malloc(sizeof(dmc__tcp_handle_st));
  if (NULL == pstHandle)
  {
    dmc__sock_close(iSock);
    return DMC_ERR_QUEUE;
  }
  pstHandle->iSock = iSock;
  pstHandle->iWriteTimeoutMs = (iTotalMs > 0) ? iTotalMs : DMC__WRITE_TIMEOUT_MS;
  *ppvHandle = pstHandle;
  return DMC_OK;
}

static int dmc__tcp_read(dm_io void *pvHandle, dm_out void *pvBuf, dm_in size_t uCap,
                         dm_in int iTimeoutMs)
{
  dmc__tcp_handle_st *pstHandle = (dmc__tcp_handle_st *)pvHandle;
  uint64 u64End = (iTimeoutMs > 0) ? (dmc_now_ms() + (uint64)iTimeoutMs) : 0u;
  size_t uWant = (uCap > (size_t)0x40000000u) ? (size_t)0x40000000u : uCap;

  if ((NULL == pstHandle) || (NULL == pvBuf) || (0u == uCap))
  {
    return -1;
  }
  for (;;)
  {
    int iRem = iTimeoutMs;
    int iReady = 0;
    if (iTimeoutMs > 0)
    {
      uint64 u64Now = dmc_now_ms();
      if (u64Now >= u64End)
      {
        return -2;
      }
      iRem = (int)(u64End - u64Now);
    }
    iReady = dmc__sock_wait(pstHandle->iSock, DM_FALSE, iRem);
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
      int iN = recv((SOCKET)pstHandle->iSock, (char *)pvBuf, (int)uWant, 0);
      if (iN >= 0)
      {
        return iN;
      }
      if (WSAEWOULDBLOCK == WSAGetLastError())
      {
        if (0 == iTimeoutMs)
        {
          return -2;
        }
        continue;
      }
      return -1;
    }
#else
    {
      ssize_t iN = recv(pstHandle->iSock, pvBuf, uWant, 0);
      if (iN >= 0)
      {
        return (int)iN;
      }
      if ((EINTR == errno) || (EAGAIN == errno) || (EWOULDBLOCK == errno))
      {
        if (0 == iTimeoutMs)
        {
          return -2;
        }
        continue;
      }
      return -1;
    }
#endif
  }
}

static int dmc__tcp_write(dm_io void *pvHandle, dm_in const void *pvBuf, dm_in size_t uLen)
{
  dmc__tcp_handle_st *pstHandle = (dmc__tcp_handle_st *)pvHandle;
  const char *pc = (const char *)pvBuf;
  int iTimeoutMs = (NULL != pstHandle && pstHandle->iWriteTimeoutMs > 0)
                       ? pstHandle->iWriteTimeoutMs
                       : DMC__WRITE_TIMEOUT_MS;
  uint64 u64End = dmc_now_ms() + (uint64)iTimeoutMs;
#ifdef MSG_NOSIGNAL
  int iFlags = MSG_NOSIGNAL;
#else
  int iFlags = 0;
#endif

  if ((NULL == pstHandle) || ((NULL == pvBuf) && (uLen > 0u)))
  {
    return -1;
  }
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
    if (1 != dmc__sock_wait(pstHandle->iSock, DM_TRUE, iRem))
    {
      return -1;
    }
#ifdef _WIN32
    {
      int iN = send((SOCKET)pstHandle->iSock, pc, (int)uChunk, iFlags);
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
      ssize_t iN = send(pstHandle->iSock, pc, uChunk, iFlags);
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

static void dmc__tcp_close(dm_io void *pvHandle)
{
  dmc__tcp_handle_st *pstHandle = (dmc__tcp_handle_st *)pvHandle;
  if (NULL == pstHandle)
  {
    return;
  }
  dmc__sock_close(pstHandle->iSock);
  free(pstHandle);
  return;
}

/* ============================ 预留未实现传输桩（ws:// / serial://） ============================
 */

static int dmc__stub_open(dm_in const char *pcUri, dm_in uint32 u32TimeoutMs,
                          dm_out void **ppvHandle)
{
  (void)pcUri;
  (void)u32TimeoutMs;
  if (NULL != ppvHandle)
  {
    *ppvHandle = NULL;
  }
  return DMC_ERR_UNSUPPORTED;
}

static int dmc__stub_read(dm_io void *pvHandle, dm_out void *pvBuf, dm_in size_t uCap,
                          dm_in int iTimeoutMs)
{
  (void)pvHandle;
  (void)pvBuf;
  (void)uCap;
  (void)iTimeoutMs;
  return -1;
}

static int dmc__stub_write(dm_io void *pvHandle, dm_in const void *pvBuf, dm_in size_t uLen)
{
  (void)pvHandle;
  (void)pvBuf;
  (void)uLen;
  return -1;
}

static void dmc__stub_close(dm_io void *pvHandle)
{
  (void)pvHandle;
  return;
}

/* ============================ 线程安全传输注册表 ============================ */

static void dmc__tr_store_locked(dm_in uint uiIdx, dm_in const dmc_transport_vtbl_st *pstVtbl)
{
  size_t uLen = strlen(pstVtbl->pcScheme);
  if (uLen >= DMC__SCHEME_MAX)
  {
    uLen = DMC__SCHEME_MAX - 1u;
  }
  memcpy(g_astDmcTr[uiIdx].acScheme, pstVtbl->pcScheme, uLen);
  g_astDmcTr[uiIdx].acScheme[uLen] = '\0';
  g_astDmcTr[uiIdx].stVtbl = *pstVtbl;
  g_astDmcTr[uiIdx].stVtbl.pcScheme = g_astDmcTr[uiIdx].acScheme;
  return;
}

extern const dmc_transport_vtbl_st *dmc__serial_vtbl(void);

static void dmc__tr_ensure_builtins_locked(void)
{
  static const dmc_transport_vtbl_st astBuiltins[] = {
      {"local", dmc__tcp_open, dmc__tcp_read, dmc__tcp_write, dmc__tcp_close},
      {"tcp", dmc__tcp_open, dmc__tcp_read, dmc__tcp_write, dmc__tcp_close},
      {"ws", dmc__stub_open, dmc__stub_read, dmc__stub_write, dmc__stub_close},
  };
  uint uiIdx = 0u;

  if (g_bDmcTrInited)
  {
    return;
  }
  g_bDmcTrInited = DM_TRUE;
  for (uiIdx = 0u; uiIdx < (uint)(sizeof(astBuiltins) / sizeof(astBuiltins[0])); uiIdx++)
  {
    dmc__tr_store_locked(g_uiDmcTrCount++, &astBuiltins[uiIdx]);
  }
  dmc__tr_store_locked(g_uiDmcTrCount++, dmc__serial_vtbl());
  return;
}

int dmc_transport_register(dm_in const dmc_transport_vtbl_st *pstVtbl)
{
  uint uiIdx = 0u;
  int iRc = DMC_OK;

  if ((NULL == pstVtbl) || (NULL == pstVtbl->pcScheme) || ('\0' == pstVtbl->pcScheme[0]) ||
      (strlen(pstVtbl->pcScheme) >= DMC__SCHEME_MAX) || (NULL == pstVtbl->pfOpen) ||
      (NULL == pstVtbl->pfRead) || (NULL == pstVtbl->pfWrite) || (NULL == pstVtbl->pfClose))
  {
    return DMC_ERR_ARG;
  }
  dmc__tr_lock();
  dmc__tr_ensure_builtins_locked();
  for (uiIdx = 0u; uiIdx < g_uiDmcTrCount; uiIdx++)
  {
    if (dmc__scheme_eq(g_astDmcTr[uiIdx].acScheme, pstVtbl->pcScheme))
    {
      dmc__tr_store_locked(uiIdx, pstVtbl);
      dmc__tr_unlock();
      return DMC_OK;
    }
  }
  if (g_uiDmcTrCount >= DMC__TR_MAX)
  {
    iRc = DMC_ERR_QUEUE;
  }
  else
  {
    dmc__tr_store_locked(g_uiDmcTrCount++, pstVtbl);
  }
  dmc__tr_unlock();
  return iRc;
}

const dmc_transport_vtbl_st *dmc_transport_find(dm_in const char *pcScheme)
{
  uint uiIdx = 0u;
  const dmc_transport_vtbl_st *pstFound = NULL;

  if ((NULL == pcScheme) || ('\0' == pcScheme[0]))
  {
    return NULL;
  }
  dmc__tr_lock();
  dmc__tr_ensure_builtins_locked();
  for (uiIdx = 0u; uiIdx < g_uiDmcTrCount; uiIdx++)
  {
    if (dmc__scheme_eq(g_astDmcTr[uiIdx].acScheme, pcScheme))
    {
      pstFound = &g_astDmcTr[uiIdx].stVtbl;
      break;
    }
  }
  dmc__tr_unlock();
  return pstFound;
}

bool_t dmc__transport_copy(dm_in const char *pcScheme, dm_out dmc_transport_vtbl_st *pstOut)
{
  uint uiIdx = 0u;
  bool_t bFound = DM_FALSE;

  if ((NULL == pcScheme) || ('\0' == pcScheme[0]) || (NULL == pstOut))
  {
    return DM_FALSE;
  }
  dmc__tr_lock();
  dmc__tr_ensure_builtins_locked();
  for (uiIdx = 0u; uiIdx < g_uiDmcTrCount; uiIdx++)
  {
    if (dmc__scheme_eq(g_astDmcTr[uiIdx].acScheme, pcScheme))
    {
      *pstOut = g_astDmcTr[uiIdx].stVtbl;
      bFound = DM_TRUE;
      break;
    }
  }
  dmc__tr_unlock();
  return bFound;
}

#ifdef __cplusplus
}
#endif
