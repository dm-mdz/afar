/**
 ******************************************************************************
 * @file    dmc_transport_serial.c
 * @brief   PLUG-04 串口 / 不可靠字节流跨平台传输适配器（DD-20 §2.3）：
 *          支持 serial://<dev>?baud=...&databits=...&parity=...&stopbits=...&flow=...&frame_max=...
 *          内嵌 COBS + Magic + CRC16 帧同步、TX 整帧拼包与 RX 多帧 FIFO 环队列；
 *          Windows 采用 OVERLAPPED + SetCommTimeouts + CancelIoEx + 阻塞 GetOverlappedResult；
 *          macOS 严守 tcsetattr 先于 IOSSIOSPEED；Linux 非标波特率隔离至 dmc_serial_linux_baud.c。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#endif
#endif

#include <dmc_client.h>
#include <dmc_frame_sync.h>

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DMC__SER_DEV_CAP 260u
#define DMC__SER_FRAME_MIN 256u
#define DMC__SER_FRAME_MAX_CAP 262144u
#define DMC__SER_BAUD_MAX 12000000u
#define DMC__SER_WRITE_TIMEOUT_MS 15000

#ifdef __linux__
extern int dmc__serial_linux_set_baud(dm_in int iFd, dm_in uint32 u32Baud);
#endif

typedef struct dmc__serial_cfg
{
  char acDev[DMC__SER_DEV_CAP];
  uint32 u32Baud;
  uint8 u8DataBits;
  char cParity;
  uint8 u8StopBits;
  bool_t bRtsCts;
  uint32 u32FrameMax;
} dmc__serial_cfg_st;

typedef void *(*dmc__serial_realloc_fn)(dm_io void *pvPtr, dm_in size_t uSize);

static dmc__serial_realloc_fn g_pfDmcSerRealloc = NULL;

void dmc__serial_set_realloc_fn(dm_in dmc__serial_realloc_fn pfFn)
{
  g_pfDmcSerRealloc = pfFn;
  return;
}

static void *dmc__ser_realloc(dm_io void *pvPtr, dm_in size_t uSize)
{
  if (NULL != g_pfDmcSerRealloc)
  {
    return g_pfDmcSerRealloc(pvPtr, uSize);
  }
  return realloc(pvPtr, uSize);
}

typedef struct dmc__serial_handle
{
#ifdef _WIN32
  HANDLE hPort;
  HANDLE hReadEv;
  HANDLE hWriteEv;
  HANDLE hCancelEv;
  SRWLOCK stIoLock;
  CONDITION_VARIABLE stIoCv;
  bool_t bIsPipe;
#else
  int iFd;
  int aiWakePipe[2];
  pthread_mutex_t stIoLock;
  pthread_cond_t stIoCv;
  bool_t bSyncInited;
#endif
  bool_t bClosing;
  uint32 u32InflightIo;
  int iWriteTimeoutMs;
  uint32 u32FrameMax;
  dmc_sync_dec_st stDec;
  dmc_sync_stats_st stStats;
  uint8 *pu8DecWork;
  uint8 *pu8TxBuf;
  size_t uTxCap;
  size_t uTxLen;
  size_t uTxNeed;
  uint8 *pu8EncBuf;
  size_t uEncCap;
  uint8 *pu8Fifo;
  size_t uFifoCap;
  size_t uFifoMaxCap;
  size_t uFifoHead;
  size_t uFifoCount;
  bool_t bFifoErr;
} dmc__serial_handle_st;

static bool_t dmc__ser_sync_init(dm_io dmc__serial_handle_st *pstH)
{
#ifdef _WIN32
  InitializeSRWLock(&pstH->stIoLock);
  InitializeConditionVariable(&pstH->stIoCv);
  return DM_TRUE;
#else
  if (0 != pthread_mutex_init(&pstH->stIoLock, NULL))
  {
    return DM_FALSE;
  }
  if (0 != pthread_cond_init(&pstH->stIoCv, NULL))
  {
    (void)pthread_mutex_destroy(&pstH->stIoLock);
    return DM_FALSE;
  }
  pstH->bSyncInited = DM_TRUE;
  return DM_TRUE;
#endif
}

static void dmc__ser_sync_fini(dm_io dmc__serial_handle_st *pstH)
{
#ifdef _WIN32
  (void)pstH;
#else
  if (pstH->bSyncInited)
  {
    (void)pthread_cond_destroy(&pstH->stIoCv);
    (void)pthread_mutex_destroy(&pstH->stIoLock);
    pstH->bSyncInited = DM_FALSE;
  }
#endif
  return;
}

static bool_t dmc__ser_io_enter(dm_io dmc__serial_handle_st *pstH)
{
  bool_t bOk = DM_FALSE;
#ifdef _WIN32
  AcquireSRWLockExclusive(&pstH->stIoLock);
  if (!pstH->bClosing)
  {
    pstH->u32InflightIo++;
    bOk = DM_TRUE;
  }
  ReleaseSRWLockExclusive(&pstH->stIoLock);
#else
  (void)pthread_mutex_lock(&pstH->stIoLock);
  if (!pstH->bClosing)
  {
    pstH->u32InflightIo++;
    bOk = DM_TRUE;
  }
  (void)pthread_mutex_unlock(&pstH->stIoLock);
#endif
  return bOk;
}

static void dmc__ser_io_leave(dm_io dmc__serial_handle_st *pstH)
{
#ifdef _WIN32
  AcquireSRWLockExclusive(&pstH->stIoLock);
  if (pstH->u32InflightIo > 0u)
  {
    pstH->u32InflightIo--;
  }
  if (pstH->bClosing && (0u == pstH->u32InflightIo))
  {
    WakeAllConditionVariable(&pstH->stIoCv);
  }
  ReleaseSRWLockExclusive(&pstH->stIoLock);
#else
  (void)pthread_mutex_lock(&pstH->stIoLock);
  if (pstH->u32InflightIo > 0u)
  {
    pstH->u32InflightIo--;
  }
  if (pstH->bClosing && (0u == pstH->u32InflightIo))
  {
    (void)pthread_cond_broadcast(&pstH->stIoCv);
  }
  (void)pthread_mutex_unlock(&pstH->stIoLock);
#endif
  return;
}

static bool_t dmc__ser_is_closing(dm_io dmc__serial_handle_st *pstH)
{
  bool_t bClosing = DM_FALSE;
#ifdef _WIN32
  AcquireSRWLockShared(&pstH->stIoLock);
  bClosing = pstH->bClosing;
  ReleaseSRWLockShared(&pstH->stIoLock);
#else
  (void)pthread_mutex_lock(&pstH->stIoLock);
  bClosing = pstH->bClosing;
  (void)pthread_mutex_unlock(&pstH->stIoLock);
#endif
  return bClosing;
}

static void dmc__ser_close_begin(dm_io dmc__serial_handle_st *pstH)
{
#ifdef _WIN32
  AcquireSRWLockExclusive(&pstH->stIoLock);
  pstH->bClosing = DM_TRUE;
  if (NULL != pstH->hCancelEv)
  {
    (void)SetEvent(pstH->hCancelEv);
  }
  if (INVALID_HANDLE_VALUE != pstH->hPort)
  {
    (void)CancelIoEx(pstH->hPort, NULL);
  }
  while (pstH->u32InflightIo > 0u)
  {
    (void)SleepConditionVariableSRW(&pstH->stIoCv, &pstH->stIoLock, 50u, 0u);
    if (INVALID_HANDLE_VALUE != pstH->hPort)
    {
      (void)CancelIoEx(pstH->hPort, NULL);
    }
  }
  ReleaseSRWLockExclusive(&pstH->stIoLock);
#else
  (void)pthread_mutex_lock(&pstH->stIoLock);
  pstH->bClosing = DM_TRUE;
  if (pstH->aiWakePipe[1] >= 0)
  {
    uint8 u8Wake = 1u;
    (void)write(pstH->aiWakePipe[1], &u8Wake, 1u);
  }
  while (pstH->u32InflightIo > 0u)
  {
    (void)pthread_cond_wait(&pstH->stIoCv, &pstH->stIoLock);
  }
  (void)pthread_mutex_unlock(&pstH->stIoLock);
#endif
  return;
}

/* ============================ URI 解析与参数校验 ============================ */

static unsigned char dmc__ser_lower(dm_in unsigned char u8Ch)
{
  return ((u8Ch >= 'A') && (u8Ch <= 'Z')) ? (unsigned char)(u8Ch - 'A' + 'a') : u8Ch;
}

static bool_t dmc__ser_eq_ci_n(dm_in const char *pcA, dm_in size_t uLenA, dm_in const char *pcB)
{
  size_t uIdx = 0u;
  size_t uLenB = strlen(pcB);

  if (uLenA != uLenB)
  {
    return DM_FALSE;
  }
  for (uIdx = 0u; uIdx < uLenA; uIdx++)
  {
    if (dmc__ser_lower((unsigned char)pcA[uIdx]) != dmc__ser_lower((unsigned char)pcB[uIdx]))
    {
      return DM_FALSE;
    }
  }
  return DM_TRUE;
}

static bool_t dmc__ser_parse_u32(dm_in const char *pcVal, dm_in size_t uLen, dm_out uint32 *pu32Out)
{
  uint64 u64Val = 0u;
  size_t uIdx = 0u;

  if ((0u == uLen) || (uLen > 10u))
  {
    return DM_FALSE;
  }
  for (uIdx = 0u; uIdx < uLen; uIdx++)
  {
    unsigned char u8Ch = (unsigned char)pcVal[uIdx];
    if (!isdigit(u8Ch))
    {
      return DM_FALSE;
    }
    u64Val = (u64Val * 10u) + (uint64)(u8Ch - '0');
    if (u64Val > 0xFFFFFFFFu)
    {
      return DM_FALSE;
    }
  }
  *pu32Out = (uint32)u64Val;
  return DM_TRUE;
}

static int dmc__ser_apply_kv(dm_io dmc__serial_cfg_st *pstCfg, dm_in const char *pcKey,
                             dm_in size_t uKeyLen, dm_in const char *pcVal, dm_in size_t uValLen)
{
  uint32 u32Num = 0u;

  if ((0u == uKeyLen) || (0u == uValLen))
  {
    return DMC_ERR_ARG;
  }
  if (dmc__ser_eq_ci_n(pcKey, uKeyLen, "baud"))
  {
    if (!dmc__ser_parse_u32(pcVal, uValLen, &u32Num) || (0u == u32Num) ||
        (u32Num > DMC__SER_BAUD_MAX))
    {
      return DMC_ERR_ARG;
    }
    pstCfg->u32Baud = u32Num;
    return DMC_OK;
  }
  if (dmc__ser_eq_ci_n(pcKey, uKeyLen, "databits"))
  {
    if (!dmc__ser_parse_u32(pcVal, uValLen, &u32Num) || (u32Num < 5u) || (u32Num > 8u))
    {
      return DMC_ERR_ARG;
    }
    pstCfg->u8DataBits = (uint8)u32Num;
    return DMC_OK;
  }
  if (dmc__ser_eq_ci_n(pcKey, uKeyLen, "parity"))
  {
    char cP = '\0';
    if (1u != uValLen)
    {
      return DMC_ERR_ARG;
    }
    cP = (char)toupper((unsigned char)pcVal[0]);
    if (('N' != cP) && ('E' != cP) && ('O' != cP))
    {
      return DMC_ERR_ARG;
    }
    pstCfg->cParity = cP;
    return DMC_OK;
  }
  if (dmc__ser_eq_ci_n(pcKey, uKeyLen, "stopbits"))
  {
    if (!dmc__ser_parse_u32(pcVal, uValLen, &u32Num) || ((1u != u32Num) && (2u != u32Num)))
    {
      return DMC_ERR_ARG;
    }
    pstCfg->u8StopBits = (uint8)u32Num;
    return DMC_OK;
  }
  if (dmc__ser_eq_ci_n(pcKey, uKeyLen, "flow"))
  {
    if (dmc__ser_eq_ci_n(pcVal, uValLen, "none"))
    {
      pstCfg->bRtsCts = DM_FALSE;
      return DMC_OK;
    }
    if (dmc__ser_eq_ci_n(pcVal, uValLen, "rtscts"))
    {
      pstCfg->bRtsCts = DM_TRUE;
      return DMC_OK;
    }
    return DMC_ERR_ARG;
  }
  if (dmc__ser_eq_ci_n(pcKey, uKeyLen, "frame_max"))
  {
    if (!dmc__ser_parse_u32(pcVal, uValLen, &u32Num) || (u32Num < DMC__SER_FRAME_MIN) ||
        (u32Num > DMC__SER_FRAME_MAX_CAP))
    {
      return DMC_ERR_ARG;
    }
    pstCfg->u32FrameMax = u32Num;
    return DMC_OK;
  }
  return DMC_ERR_ARG;
}

static int dmc__ser_norm_dev(dm_in const char *pcRaw, dm_in size_t uRawLen, dm_out char *pcOut,
                             dm_in size_t uCap)
{
  if ((0u == uRawLen) || (uRawLen >= uCap))
  {
    return DMC_ERR_ARG;
  }
#ifdef _WIN32
  if ((uRawLen >= 4u) && ('\\' == pcRaw[0]) && ('\\' == pcRaw[1]) && ('.' == pcRaw[2]) &&
      ('\\' == pcRaw[3]))
  {
    memcpy(pcOut, pcRaw, uRawLen);
    pcOut[uRawLen] = '\0';
    return DMC_OK;
  }
  if ((uRawLen >= 4u) && ('/' == pcRaw[0]) && ('/' == pcRaw[1]) && ('.' == pcRaw[2]) &&
      ('/' == pcRaw[3]))
  {
    size_t uIdx = 0u;
    for (uIdx = 0u; uIdx < uRawLen; uIdx++)
    {
      pcOut[uIdx] = ('/' == pcRaw[uIdx]) ? '\\' : pcRaw[uIdx];
    }
    pcOut[uRawLen] = '\0';
    return DMC_OK;
  }
  if ((uRawLen + 4u) >= uCap)
  {
    return DMC_ERR_ARG;
  }
  memcpy(pcOut, "\\\\.\\", 4u);
  memcpy(pcOut + 4u, pcRaw, uRawLen);
  pcOut[uRawLen + 4u] = '\0';
  return DMC_OK;
#else
  memcpy(pcOut, pcRaw, uRawLen);
  pcOut[uRawLen] = '\0';
  return DMC_OK;
#endif
}

int dmc__serial_parse_uri(dm_in const char *pcUri, dm_out dmc__serial_cfg_st *pstCfg)
{
  const char *pcSep = NULL;
  const char *pcBody = NULL;
  const char *pcQuery = NULL;
  size_t uDevLen = 0u;

  if ((NULL == pcUri) || (NULL == pstCfg))
  {
    return DMC_ERR_ARG;
  }
  memset(pstCfg, 0, sizeof(*pstCfg));
  pstCfg->u32Baud = 115200u;
  pstCfg->u8DataBits = 8u;
  pstCfg->cParity = 'N';
  pstCfg->u8StopBits = 1u;
  pstCfg->bRtsCts = DM_FALSE;
  pstCfg->u32FrameMax = DMC_SYNC_HOST_SERIAL_FRAME_MAX;

  pcSep = strstr(pcUri, "://");
  if (NULL != pcSep)
  {
    if (!dmc__ser_eq_ci_n(pcUri, (size_t)(pcSep - pcUri), "serial"))
    {
      return DMC_ERR_ARG;
    }
    pcBody = pcSep + 3;
  }
  else
  {
    pcBody = pcUri;
  }
  pcQuery = strchr(pcBody, '?');
  uDevLen = (NULL != pcQuery) ? (size_t)(pcQuery - pcBody) : strlen(pcBody);
  if (DMC_OK != dmc__ser_norm_dev(pcBody, uDevLen, pstCfg->acDev, sizeof(pstCfg->acDev)))
  {
    return DMC_ERR_ARG;
  }
  if (NULL == pcQuery)
  {
    return DMC_OK;
  }
  pcQuery++;
  while ('\0' != *pcQuery)
  {
    const char *pcAmp = strchr(pcQuery, '&');
    size_t uPairLen = (NULL != pcAmp) ? (size_t)(pcAmp - pcQuery) : strlen(pcQuery);
    const char *pcEq = (const char *)memchr(pcQuery, '=', uPairLen);
    if (NULL == pcEq)
    {
      return DMC_ERR_ARG;
    }
    if (DMC_OK != dmc__ser_apply_kv(pstCfg, pcQuery, (size_t)(pcEq - pcQuery), pcEq + 1,
                                    uPairLen - (size_t)(pcEq - pcQuery) - 1u))
    {
      return DMC_ERR_ARG;
    }
    if (NULL == pcAmp)
    {
      break;
    }
    pcQuery = pcAmp + 1;
  }
  return DMC_OK;
}

/* ============================ RX 多帧 FIFO 环形队列 ============================ */

static bool_t dmc__ser_fifo_grow(dm_io dmc__serial_handle_st *pstH, dm_in size_t uNeedTotal)
{
  size_t uNewCap = pstH->uFifoCap;
  uint8 *pu8New = NULL;

  if (uNeedTotal > pstH->uFifoMaxCap)
  {
    return DM_FALSE;
  }
  while (uNewCap < uNeedTotal)
  {
    if (uNewCap > (pstH->uFifoMaxCap / 2u))
    {
      uNewCap = pstH->uFifoMaxCap;
    }
    else
    {
      uNewCap *= 2u;
    }
  }
  pu8New = (uint8 *)dmc__ser_realloc(NULL, uNewCap);
  if (NULL == pu8New)
  {
    return DM_FALSE;
  }
  if (pstH->uFifoCount > 0u)
  {
    size_t uFirst = pstH->uFifoCap - pstH->uFifoHead;
    if (uFirst > pstH->uFifoCount)
    {
      uFirst = pstH->uFifoCount;
    }
    memcpy(pu8New, pstH->pu8Fifo + pstH->uFifoHead, uFirst);
    if (pstH->uFifoCount > uFirst)
    {
      memcpy(pu8New + uFirst, pstH->pu8Fifo, pstH->uFifoCount - uFirst);
    }
  }
  free(pstH->pu8Fifo);
  pstH->pu8Fifo = pu8New;
  pstH->uFifoCap = uNewCap;
  pstH->uFifoHead = 0u;
  return DM_TRUE;
}

static bool_t dmc__ser_fifo_reserve(dm_io dmc__serial_handle_st *pstH, dm_in size_t uAddLen)
{
  if (0u == uAddLen)
  {
    return DM_TRUE;
  }
  if (uAddLen > (SIZE_MAX - pstH->uFifoCount))
  {
    pstH->bFifoErr = DM_TRUE;
    return DM_FALSE;
  }
  if ((pstH->uFifoCount + uAddLen) > pstH->uFifoCap)
  {
    if (!dmc__ser_fifo_grow(pstH, pstH->uFifoCount + uAddLen))
    {
      pstH->bFifoErr = DM_TRUE;
      return DM_FALSE;
    }
  }
  return DM_TRUE;
}

static bool_t dmc__ser_fifo_push(dm_io dmc__serial_handle_st *pstH, dm_in const uint8 *pu8Data,
                                 dm_in size_t uLen)
{
  size_t uTail = 0u;
  size_t uFirst = 0u;

  if (0u == uLen)
  {
    return DM_TRUE;
  }
  if (!dmc__ser_fifo_reserve(pstH, uLen))
  {
    return DM_FALSE;
  }
  uTail = (pstH->uFifoHead + pstH->uFifoCount) % pstH->uFifoCap;
  uFirst = pstH->uFifoCap - uTail;
  if (uFirst > uLen)
  {
    uFirst = uLen;
  }
  memcpy(pstH->pu8Fifo + uTail, pu8Data, uFirst);
  if (uLen > uFirst)
  {
    memcpy(pstH->pu8Fifo, pu8Data + uFirst, uLen - uFirst);
  }
  pstH->uFifoCount += uLen;
  return DM_TRUE;
}

static size_t dmc__ser_fifo_pop(dm_io dmc__serial_handle_st *pstH, dm_out uint8 *pu8Dst,
                                dm_in size_t uCap)
{
  size_t uTake = (uCap < pstH->uFifoCount) ? uCap : pstH->uFifoCount;
  size_t uFirst = 0u;

  if (0u == uTake)
  {
    return 0u;
  }
  uFirst = pstH->uFifoCap - pstH->uFifoHead;
  if (uFirst > uTake)
  {
    uFirst = uTake;
  }
  memcpy(pu8Dst, pstH->pu8Fifo + pstH->uFifoHead, uFirst);
  if (uTake > uFirst)
  {
    memcpy(pu8Dst + uFirst, pstH->pu8Fifo, uTake - uFirst);
  }
  pstH->uFifoHead = (pstH->uFifoHead + uTake) % pstH->uFifoCap;
  pstH->uFifoCount -= uTake;
  return uTake;
}

static void dmc__ser_on_frame(dm_in void *pvUser, dm_in const dm_frame_head_st *pstHead,
                              dm_in const uint8 *pu8Body)
{
  dmc__serial_handle_st *pstH = (dmc__serial_handle_st *)pvUser;
  uint8 au8Head[DM_FRAME_HEAD];
  size_t uBodyLen = 0u;

  if ((NULL == pstH) || (NULL == pstHead) || pstH->bFifoErr)
  {
    return;
  }
  if ((pstHead->u32Len > 0u) && (NULL == pu8Body))
  {
    pstH->bFifoErr = DM_TRUE;
    return;
  }
  uBodyLen = (size_t)pstHead->u32Len;
  if (uBodyLen > (SIZE_MAX - DM_FRAME_HEAD))
  {
    pstH->bFifoErr = DM_TRUE;
    return;
  }
  /* P1-29：原子预留完整帧（16B 头 + Body），防止 Body OOM 遗留半截 16B 帧头 */
  if (!dmc__ser_fifo_reserve(pstH, DM_FRAME_HEAD + uBodyLen))
  {
    return;
  }
  dm_frame_put(au8Head, pstHead);
  (void)dmc__ser_fifo_push(pstH, au8Head, DM_FRAME_HEAD);
  if (uBodyLen > 0u)
  {
    (void)dmc__ser_fifo_push(pstH, pu8Body, uBodyLen);
  }
  return;
}

/* ============================ OS 底层串口读写实现 ============================ */

#ifdef _WIN32

static void dmc__ser_fill_win_timeouts(dm_out COMMTIMEOUTS *pstTo)
{
  memset(pstTo, 0, sizeof(*pstTo));
  pstTo->ReadIntervalTimeout = MAXDWORD;
  pstTo->ReadTotalTimeoutMultiplier = 0u;
  pstTo->ReadTotalTimeoutConstant = 0u;
  pstTo->WriteTotalTimeoutMultiplier = 0u;
  pstTo->WriteTotalTimeoutConstant = 0u;
  return;
}

static bool_t dmc__ser_win_configure(dm_in HANDLE hPort, dm_in const dmc__serial_cfg_st *pstCfg)
{
  DCB stDcb;
  COMMTIMEOUTS stTo;

  memset(&stDcb, 0, sizeof(stDcb));
  stDcb.DCBlength = (DWORD)sizeof(stDcb);
  if (!GetCommState(hPort, &stDcb))
  {
    return DM_FALSE;
  }
  stDcb.BaudRate = (DWORD)pstCfg->u32Baud;
  stDcb.ByteSize = pstCfg->u8DataBits;
  stDcb.fBinary = TRUE;
  stDcb.fOutxDsrFlow = FALSE;
  stDcb.fDtrControl = DTR_CONTROL_ENABLE;
  stDcb.fDsrSensitivity = FALSE;
  stDcb.fOutX = FALSE;
  stDcb.fInX = FALSE;
  stDcb.fOutxCtsFlow = pstCfg->bRtsCts ? TRUE : FALSE;
  stDcb.fRtsControl = pstCfg->bRtsCts ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
  if ('E' == pstCfg->cParity)
  {
    stDcb.fParity = TRUE;
    stDcb.Parity = EVENPARITY;
  }
  else if ('O' == pstCfg->cParity)
  {
    stDcb.fParity = TRUE;
    stDcb.Parity = ODDPARITY;
  }
  else
  {
    stDcb.fParity = FALSE;
    stDcb.Parity = NOPARITY;
  }
  stDcb.StopBits = (2u == pstCfg->u8StopBits) ? TWOSTOPBITS : ONESTOPBIT;
  if (!SetCommState(hPort, &stDcb))
  {
    return DM_FALSE;
  }
  dmc__ser_fill_win_timeouts(&stTo);
  if (!SetCommTimeouts(hPort, &stTo))
  {
    return DM_FALSE;
  }
  (void)SetupComm(hPort, 16384u, 16384u);
  (void)PurgeComm(hPort, PURGE_RXCLEAR | PURGE_TXCLEAR);
  (void)SetCommMask(hPort, EV_RXCHAR);
  return DM_TRUE;
}

static int dmc__ser_os_open(dm_in const dmc__serial_cfg_st *pstCfg,
                            dm_io dmc__serial_handle_st *pstH)
{
  wchar_t awcDev[DMC__SER_DEV_CAP];
  int iWideLen = 0;
  DWORD dwType = FILE_TYPE_UNKNOWN;

  pstH->hPort = INVALID_HANDLE_VALUE;
  pstH->hReadEv = NULL;
  pstH->hWriteEv = NULL;
  pstH->hCancelEv = NULL;
  pstH->bIsPipe = DM_FALSE;

  iWideLen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pstCfg->acDev, -1, awcDev,
                                 (int)(sizeof(awcDev) / sizeof(awcDev[0])));
  if (iWideLen <= 0)
  {
    return DMC_ERR_ARG;
  }
  pstH->hPort = CreateFileW(awcDev, GENERIC_READ | GENERIC_WRITE, 0u, NULL, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED, NULL);
  if (INVALID_HANDLE_VALUE == pstH->hPort)
  {
    return DMC_ERR_NOT_CON;
  }
  dwType = GetFileType(pstH->hPort);
  if (FILE_TYPE_PIPE == dwType)
  {
    pstH->bIsPipe = DM_TRUE;
  }
  else if (!dmc__ser_win_configure(pstH->hPort, pstCfg))
  {
    CloseHandle(pstH->hPort);
    pstH->hPort = INVALID_HANDLE_VALUE;
    return DMC_ERR_NOT_CON;
  }
  pstH->hReadEv = CreateEventW(NULL, TRUE, FALSE, NULL);
  pstH->hWriteEv = CreateEventW(NULL, TRUE, FALSE, NULL);
  pstH->hCancelEv = CreateEventW(NULL, TRUE, FALSE, NULL);
  if ((NULL == pstH->hReadEv) || (NULL == pstH->hWriteEv) || (NULL == pstH->hCancelEv))
  {
    if (NULL != pstH->hReadEv)
    {
      CloseHandle(pstH->hReadEv);
    }
    if (NULL != pstH->hWriteEv)
    {
      CloseHandle(pstH->hWriteEv);
    }
    if (NULL != pstH->hCancelEv)
    {
      CloseHandle(pstH->hCancelEv);
    }
    CloseHandle(pstH->hPort);
    pstH->hPort = INVALID_HANDLE_VALUE;
    pstH->hReadEv = NULL;
    pstH->hWriteEv = NULL;
    pstH->hCancelEv = NULL;
    return DMC_ERR_QUEUE;
  }
  return DMC_OK;
}

static int dmc__ser_win_wait_rx(dm_io dmc__serial_handle_st *pstH, dm_in int iTimeoutMs)
{
  OVERLAPPED stOv;
  HANDLE ahWait[2];
  DWORD dwMask = 0u;
  DWORD dwWait = (iTimeoutMs < 0) ? INFINITE : (DWORD)iTimeoutMs;
  DWORD dwW = 0u;
  DWORD dwDummy = 0u;

  if (dmc__ser_is_closing(pstH))
  {
    return -1;
  }
  if (0 == iTimeoutMs)
  {
    return -2;
  }
  memset(&stOv, 0, sizeof(stOv));
  ResetEvent(pstH->hReadEv);
  stOv.hEvent = pstH->hReadEv;
  if (WaitCommEvent(pstH->hPort, &dwMask, &stOv))
  {
    return 1;
  }
  if (ERROR_IO_PENDING != GetLastError())
  {
    return -1;
  }
  ahWait[0] = pstH->hReadEv;
  ahWait[1] = pstH->hCancelEv;
  dwW = WaitForMultipleObjects(2u, ahWait, FALSE, dwWait);
  if (WAIT_OBJECT_0 == dwW)
  {
    return GetOverlappedResult(pstH->hPort, &stOv, &dwDummy, FALSE) ? 1 : -1;
  }
  (void)CancelIoEx(pstH->hPort, &stOv);
  (void)GetOverlappedResult(pstH->hPort, &stOv, &dwDummy, TRUE);
  return (WAIT_TIMEOUT == dwW) ? -2 : -1;
}

static int dmc__ser_win_read_once(dm_io dmc__serial_handle_st *pstH, dm_out uint8 *pu8Dst,
                                  dm_in DWORD dwWant, dm_in DWORD dwWait)
{
  OVERLAPPED stOv;
  HANDLE ahWait[2];
  DWORD dwGot = 0u;
  DWORD dwErr = 0u;
  DWORD dwW = 0u;

  if (dmc__ser_is_closing(pstH))
  {
    return -1;
  }
  memset(&stOv, 0, sizeof(stOv));
  ResetEvent(pstH->hReadEv);
  stOv.hEvent = pstH->hReadEv;
  if (ReadFile(pstH->hPort, pu8Dst, dwWant, &dwGot, &stOv))
  {
    if (dwGot > 0u)
    {
      return (int)dwGot;
    }
    return pstH->bIsPipe ? 0 : -2;
  }
  dwErr = GetLastError();
  if (ERROR_IO_PENDING != dwErr)
  {
    if ((ERROR_BROKEN_PIPE == dwErr) || (ERROR_PIPE_NOT_CONNECTED == dwErr) ||
        (ERROR_HANDLE_EOF == dwErr))
    {
      return 0;
    }
    return -1;
  }
  ahWait[0] = pstH->hReadEv;
  ahWait[1] = pstH->hCancelEv;
  dwW = WaitForMultipleObjects(2u, ahWait, FALSE, dwWait);
  if (WAIT_OBJECT_0 == dwW)
  {
    if (GetOverlappedResult(pstH->hPort, &stOv, &dwGot, FALSE))
    {
      return (dwGot > 0u) ? (int)dwGot : (pstH->bIsPipe ? 0 : -2);
    }
    dwErr = GetLastError();
    if ((ERROR_BROKEN_PIPE == dwErr) || (ERROR_PIPE_NOT_CONNECTED == dwErr) ||
        (ERROR_HANDLE_EOF == dwErr))
    {
      return 0;
    }
    return -1;
  }
  /* P0-15/P1-01 零栈 UAF：CancelIoEx 后必须阻塞 GetOverlappedResult(..., TRUE) 再销毁栈上 stOv */
  (void)CancelIoEx(pstH->hPort, &stOv);
  dwGot = 0u;
  if (GetOverlappedResult(pstH->hPort, &stOv, &dwGot, TRUE) && (dwGot > 0u))
  {
    return (int)dwGot;
  }
  return (WAIT_TIMEOUT == dwW) ? -2 : -1;
}

static int dmc__ser_os_read(dm_io dmc__serial_handle_st *pstH, dm_out uint8 *pu8Dst,
                            dm_in size_t uCap, dm_in int iTimeoutMs)
{
  DWORD dwWant = (uCap > 16384u) ? 16384u : (DWORD)uCap;
  DWORD dwWait = (iTimeoutMs < 0) ? INFINITE : (DWORD)iTimeoutMs;
  uint64 u64End = (iTimeoutMs > 0) ? (dmc_now_ms() + (uint64)iTimeoutMs) : 0u;
  int iRc = 0;

  iRc = dmc__ser_win_read_once(pstH, pu8Dst, dwWant, dwWait);
  if ((-2 != iRc) || pstH->bIsPipe || (0 == iTimeoutMs))
  {
    return iRc;
  }
  /* P0-15：真 COM 口传播 WaitCommEvent 致命错误 -1，且二次 ReadFile 同样走完整
   * OVERLAPPED+CancelIoEx 路径 */
  iRc = dmc__ser_win_wait_rx(pstH, iTimeoutMs);
  if (1 != iRc)
  {
    return iRc;
  }
  if (iTimeoutMs > 0)
  {
    uint64 u64Now = dmc_now_ms();
    dwWait = (u64End > u64Now) ? (DWORD)(u64End - u64Now) : 1u;
  }
  return dmc__ser_win_read_once(pstH, pu8Dst, dwWant, dwWait);
}

static int dmc__ser_os_write_all(dm_io dmc__serial_handle_st *pstH, dm_in const uint8 *pu8Src,
                                 dm_in size_t uLen)
{
  size_t uOff = 0u;
  uint64 u64End = dmc_now_ms() + (uint64)pstH->iWriteTimeoutMs;

  while (uOff < uLen)
  {
    OVERLAPPED stOv;
    HANDLE ahWait[2];
    DWORD dwChunk = ((uLen - uOff) > 16384u) ? 16384u : (DWORD)(uLen - uOff);
    DWORD dwPut = 0u;
    uint64 u64Now = dmc_now_ms();
    DWORD dwRem = 0u;

    if (dmc__ser_is_closing(pstH) || (u64Now >= u64End))
    {
      return -1;
    }
    dwRem = (DWORD)(u64End - u64Now);
    memset(&stOv, 0, sizeof(stOv));
    ResetEvent(pstH->hWriteEv);
    stOv.hEvent = pstH->hWriteEv;
    if (!WriteFile(pstH->hPort, pu8Src + uOff, dwChunk, &dwPut, &stOv))
    {
      DWORD dwW = 0u;
      if (ERROR_IO_PENDING != GetLastError())
      {
        return -1;
      }
      ahWait[0] = pstH->hWriteEv;
      ahWait[1] = pstH->hCancelEv;
      dwW = WaitForMultipleObjects(2u, ahWait, FALSE, dwRem);
      if (WAIT_OBJECT_0 != dwW)
      {
        (void)CancelIoEx(pstH->hPort, &stOv);
        (void)GetOverlappedResult(pstH->hPort, &stOv, &dwPut, TRUE);
        return -1;
      }
      if (!GetOverlappedResult(pstH->hPort, &stOv, &dwPut, FALSE))
      {
        return -1;
      }
    }
    if (0u == dwPut)
    {
      return -1;
    }
    uOff += (size_t)dwPut;
  }
  return 0;
}

static void dmc__ser_os_close(dm_io dmc__serial_handle_st *pstH)
{
  if (INVALID_HANDLE_VALUE != pstH->hPort)
  {
    (void)CancelIoEx(pstH->hPort, NULL);
    CloseHandle(pstH->hPort);
    pstH->hPort = INVALID_HANDLE_VALUE;
  }
  if (NULL != pstH->hReadEv)
  {
    CloseHandle(pstH->hReadEv);
    pstH->hReadEv = NULL;
  }
  if (NULL != pstH->hWriteEv)
  {
    CloseHandle(pstH->hWriteEv);
    pstH->hWriteEv = NULL;
  }
  if (NULL != pstH->hCancelEv)
  {
    CloseHandle(pstH->hCancelEv);
    pstH->hCancelEv = NULL;
  }
  return;
}

#else

#ifdef __linux__
static bool_t dmc__ser_std_baud(dm_in uint32 u32Baud, dm_out speed_t *pOut)
{
  switch (u32Baud)
  {
  case 9600u:
    *pOut = B9600;
    return DM_TRUE;
  case 19200u:
    *pOut = B19200;
    return DM_TRUE;
  case 38400u:
    *pOut = B38400;
    return DM_TRUE;
  case 57600u:
    *pOut = B57600;
    return DM_TRUE;
  case 115200u:
    *pOut = B115200;
    return DM_TRUE;
  case 230400u:
    *pOut = B230400;
    return DM_TRUE;
#ifdef B460800
  case 460800u:
    *pOut = B460800;
    return DM_TRUE;
#endif
#ifdef B921600
  case 921600u:
    *pOut = B921600;
    return DM_TRUE;
#endif
#ifdef B1000000
  case 1000000u:
    *pOut = B1000000;
    return DM_TRUE;
#endif
#ifdef B2000000
  case 2000000u:
    *pOut = B2000000;
    return DM_TRUE;
#endif
#ifdef B4000000
  case 4000000u:
    *pOut = B4000000;
    return DM_TRUE;
#endif
  default:
    return DM_FALSE;
  }
}
#endif

static bool_t dmc__ser_posix_configure(dm_in int iFd, dm_in const dmc__serial_cfg_st *pstCfg)
{
  struct termios stTio;
#ifdef __linux__
  bool_t bCustomLinuxBaud = DM_FALSE;
#endif

  if (0 != tcgetattr(iFd, &stTio))
  {
    return DM_FALSE;
  }
  cfmakeraw(&stTio);
  stTio.c_cflag &= (tcflag_t)(~(CSIZE | PARENB | PARODD | CSTOPB));
  stTio.c_cflag |= (tcflag_t)(CLOCAL | CREAD);
  if (5u == pstCfg->u8DataBits)
  {
    stTio.c_cflag |= CS5;
  }
  else if (6u == pstCfg->u8DataBits)
  {
    stTio.c_cflag |= CS6;
  }
  else if (7u == pstCfg->u8DataBits)
  {
    stTio.c_cflag |= CS7;
  }
  else
  {
    stTio.c_cflag |= CS8;
  }
  if ('E' == pstCfg->cParity)
  {
    stTio.c_cflag |= PARENB;
  }
  else if ('O' == pstCfg->cParity)
  {
    stTio.c_cflag |= (tcflag_t)(PARENB | PARODD);
  }
  if (2u == pstCfg->u8StopBits)
  {
    stTio.c_cflag |= CSTOPB;
  }
#ifdef CRTSCTS
  if (pstCfg->bRtsCts)
  {
    stTio.c_cflag |= CRTSCTS;
  }
  else
  {
    stTio.c_cflag &= (tcflag_t)(~CRTSCTS);
  }
#endif
  stTio.c_cc[VMIN] = 0u;
  stTio.c_cc[VTIME] = 0u;
#ifdef __linux__
  {
    speed_t tSpd = B115200;
    if (dmc__ser_std_baud(pstCfg->u32Baud, &tSpd))
    {
      (void)cfsetispeed(&stTio, tSpd);
      (void)cfsetospeed(&stTio, tSpd);
    }
    else
    {
      bCustomLinuxBaud = DM_TRUE;
    }
  }
#endif
  /* P1-01：macOS 必须先执行 tcsetattr 再调 IOSSIOSPEED，否则波特率会被覆盖 */
  if (0 != tcsetattr(iFd, TCSANOW, &stTio))
  {
    return DM_FALSE;
  }
#ifdef __APPLE__
  {
    speed_t ulBaud = (speed_t)pstCfg->u32Baud;
    if ((0 != ioctl(iFd, IOSSIOSPEED, &ulBaud)) && (ENOTTY != errno) && (EOPNOTSUPP != errno))
    {
      return DM_FALSE;
    }
  }
#elif defined(__linux__)
  if (bCustomLinuxBaud)
  {
    if ((0 != dmc__serial_linux_set_baud(iFd, pstCfg->u32Baud)) && (ENOTTY != errno) &&
        (EOPNOTSUPP != errno))
    {
      return DM_FALSE;
    }
  }
#endif
  (void)tcflush(iFd, TCIOFLUSH);
  return DM_TRUE;
}

static int dmc__ser_os_open(dm_in const dmc__serial_cfg_st *pstCfg,
                            dm_io dmc__serial_handle_st *pstH)
{
  int iFd = -1;

  pstH->iFd = -1;
  pstH->aiWakePipe[0] = -1;
  pstH->aiWakePipe[1] = -1;
  if (0 != pipe(pstH->aiWakePipe))
  {
    pstH->aiWakePipe[0] = -1;
    pstH->aiWakePipe[1] = -1;
    return DMC_ERR_QUEUE;
  }
  (void)fcntl(pstH->aiWakePipe[0], F_SETFL, O_NONBLOCK);
  (void)fcntl(pstH->aiWakePipe[1], F_SETFL, O_NONBLOCK);
  (void)fcntl(pstH->aiWakePipe[0], F_SETFD, FD_CLOEXEC);
  (void)fcntl(pstH->aiWakePipe[1], F_SETFD, FD_CLOEXEC);

  iFd = open(pstCfg->acDev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (iFd < 0)
  {
    (void)close(pstH->aiWakePipe[0]);
    (void)close(pstH->aiWakePipe[1]);
    pstH->aiWakePipe[0] = -1;
    pstH->aiWakePipe[1] = -1;
    return DMC_ERR_NOT_CON;
  }
  if (isatty(iFd))
  {
#ifdef TIOCEXCL
    (void)ioctl(iFd, TIOCEXCL);
#endif
    if (!dmc__ser_posix_configure(iFd, pstCfg))
    {
#ifdef TIOCNXCL
      (void)ioctl(iFd, TIOCNXCL);
#endif
      (void)close(iFd);
      (void)close(pstH->aiWakePipe[0]);
      (void)close(pstH->aiWakePipe[1]);
      pstH->aiWakePipe[0] = -1;
      pstH->aiWakePipe[1] = -1;
      return DMC_ERR_NOT_CON;
    }
  }
  pstH->iFd = iFd;
  return DMC_OK;
}

static int dmc__ser_os_read(dm_io dmc__serial_handle_st *pstH, dm_out uint8 *pu8Dst,
                            dm_in size_t uCap, dm_in int iTimeoutMs)
{
  uint64 u64End = (iTimeoutMs > 0) ? (dmc_now_ms() + (uint64)iTimeoutMs) : 0u;

  for (;;)
  {
    struct pollfd astPfd[2];
    int iRem = iTimeoutMs;
    int iRet = 0;

    if (dmc__ser_is_closing(pstH))
    {
      return -1;
    }
    if (iTimeoutMs > 0)
    {
      uint64 u64Now = dmc_now_ms();
      if (u64Now >= u64End)
      {
        return -2;
      }
      iRem = (int)(u64End - u64Now);
    }
    astPfd[0].fd = pstH->iFd;
    astPfd[0].events = POLLIN;
    astPfd[0].revents = 0;
    astPfd[1].fd = pstH->aiWakePipe[0];
    astPfd[1].events = POLLIN;
    astPfd[1].revents = 0;
    iRet = poll(astPfd, 2u, iRem);
    if (0 == iRet)
    {
      return -2;
    }
    if (iRet < 0)
    {
      if (EINTR == errno)
      {
        if (0 == iTimeoutMs)
        {
          return -2;
        }
        continue;
      }
      return -1;
    }
    if ((0 != (astPfd[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) ||
        dmc__ser_is_closing(pstH))
    {
      return -1;
    }
    if (0 != (astPfd[0].revents & POLLIN))
    {
      ssize_t iN = read(pstH->iFd, pu8Dst, uCap);
      if (iN > 0)
      {
        return (int)iN;
      }
      if (0 == iN)
      {
        return 0;
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
    if (0 != (astPfd[0].revents & (POLLERR | POLLNVAL)))
    {
      return -1;
    }
    if (0 != (astPfd[0].revents & POLLHUP))
    {
      return 0;
    }
  }
}

static int dmc__ser_os_write_all(dm_io dmc__serial_handle_st *pstH, dm_in const uint8 *pu8Src,
                                 dm_in size_t uLen)
{
  size_t uOff = 0u;
  uint64 u64End = dmc_now_ms() + (uint64)pstH->iWriteTimeoutMs;

  while (uOff < uLen)
  {
    struct pollfd astPfd[2];
    uint64 u64Now = dmc_now_ms();
    int iRem = 0;
    int iRet = 0;
    ssize_t iN = 0;

    if (dmc__ser_is_closing(pstH) || (u64Now >= u64End))
    {
      return -1;
    }
    iRem = (int)(u64End - u64Now);
    astPfd[0].fd = pstH->iFd;
    astPfd[0].events = POLLOUT;
    astPfd[0].revents = 0;
    astPfd[1].fd = pstH->aiWakePipe[0];
    astPfd[1].events = POLLIN;
    astPfd[1].revents = 0;
    iRet = poll(astPfd, 2u, iRem);
    if ((iRet < 0) && (EINTR == errno))
    {
      continue;
    }
    if (iRet <= 0)
    {
      return -1;
    }
    if ((0 != (astPfd[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) ||
        dmc__ser_is_closing(pstH))
    {
      return -1;
    }
    if (0 != (astPfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)))
    {
      return -1;
    }
    iN = write(pstH->iFd, pu8Src + uOff, uLen - uOff);
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
    uOff += (size_t)iN;
  }
  return 0;
}

static void dmc__ser_os_close(dm_io dmc__serial_handle_st *pstH)
{
  if (pstH->iFd >= 0)
  {
#ifdef TIOCNXCL
    (void)ioctl(pstH->iFd, TIOCNXCL);
#endif
    (void)close(pstH->iFd);
    pstH->iFd = -1;
  }
  if (pstH->aiWakePipe[0] >= 0)
  {
    (void)close(pstH->aiWakePipe[0]);
    pstH->aiWakePipe[0] = -1;
  }
  if (pstH->aiWakePipe[1] >= 0)
  {
    (void)close(pstH->aiWakePipe[1]);
    pstH->aiWakePipe[1] = -1;
  }
  return;
}

#endif

/* ============================ vtable 公开适配接口 ============================ */

static void dmc__serial_free_bufs(dm_io dmc__serial_handle_st *pstH)
{
  free(pstH->pu8DecWork);
  free(pstH->pu8TxBuf);
  free(pstH->pu8EncBuf);
  free(pstH->pu8Fifo);
  pstH->pu8DecWork = NULL;
  pstH->pu8TxBuf = NULL;
  pstH->pu8EncBuf = NULL;
  pstH->pu8Fifo = NULL;
  return;
}

static int dmc__serial_open(dm_in const char *pcUri, dm_in uint32 u32TimeoutMs,
                            dm_out void **ppvHandle)
{
  dmc__serial_cfg_st stCfg;
  dmc__serial_handle_st *pstH = NULL;
  size_t uEncMax = 0u;
  size_t uFrameTotal = 0u;
  int iRc = DMC_OK;

  if ((NULL == pcUri) || (NULL == ppvHandle))
  {
    return DMC_ERR_ARG;
  }
  *ppvHandle = NULL;
  iRc = dmc__serial_parse_uri(pcUri, &stCfg);
  if (DMC_OK != iRc)
  {
    return iRc;
  }
  uEncMax = dmc_sync_max_encoded_len(stCfg.u32FrameMax);
  uFrameTotal = DM_FRAME_HEAD + (size_t)stCfg.u32FrameMax;
  if (0u == uEncMax)
  {
    return DMC_ERR_ARG;
  }
  pstH = (dmc__serial_handle_st *)calloc(1u, sizeof(*pstH));
  if (NULL == pstH)
  {
    return DMC_ERR_QUEUE;
  }
  if (!dmc__ser_sync_init(pstH))
  {
    free(pstH);
    return DMC_ERR_QUEUE;
  }
  pstH->u32FrameMax = stCfg.u32FrameMax;
  if (0u == u32TimeoutMs)
  {
    pstH->iWriteTimeoutMs = DMC__SER_WRITE_TIMEOUT_MS;
  }
  else if (u32TimeoutMs > (uint32)INT_MAX)
  {
    pstH->iWriteTimeoutMs = INT_MAX;
  }
  else
  {
    pstH->iWriteTimeoutMs = (int)u32TimeoutMs;
  }
  pstH->uTxCap = uFrameTotal;
  pstH->uEncCap = uEncMax;
  pstH->uFifoCap = uFrameTotal * 2u;
  pstH->uFifoMaxCap = uFrameTotal * 4u;
  pstH->pu8DecWork = (uint8 *)malloc(uEncMax);
  pstH->pu8TxBuf = (uint8 *)malloc(pstH->uTxCap);
  pstH->pu8EncBuf = (uint8 *)malloc(pstH->uEncCap);
  pstH->pu8Fifo = (uint8 *)dmc__ser_realloc(NULL, pstH->uFifoCap);
  if ((NULL == pstH->pu8DecWork) || (NULL == pstH->pu8TxBuf) || (NULL == pstH->pu8EncBuf) ||
      (NULL == pstH->pu8Fifo))
  {
    dmc__ser_sync_fini(pstH);
    dmc__serial_free_bufs(pstH);
    free(pstH);
    return DMC_ERR_QUEUE;
  }
  dmc_sync_dec_init(&pstH->stDec, pstH->pu8DecWork, uEncMax, stCfg.u32FrameMax);
  iRc = dmc__ser_os_open(&stCfg, pstH);
  if (DMC_OK != iRc)
  {
    dmc__ser_sync_fini(pstH);
    dmc__serial_free_bufs(pstH);
    free(pstH);
    return iRc;
  }
  *ppvHandle = pstH;
  return DMC_OK;
}

static int dmc__serial_read_locked(dm_io dmc__serial_handle_st *pstH, dm_out void *pvBuf,
                                   dm_in size_t uCap, dm_in int iTimeoutMs)
{
  uint64 u64End = (iTimeoutMs > 0) ? (dmc_now_ms() + (uint64)iTimeoutMs) : 0u;
  dmc_sync_cbs_st stCbs;
  uint8 au8Raw[4096];

  /* P1-29：先判 bFifoErr 再吐数据，杜绝错误后返截断残帧 */
  if (pstH->bFifoErr)
  {
    return -1;
  }
  if (pstH->uFifoCount > 0u)
  {
    return (int)dmc__ser_fifo_pop(pstH, (uint8 *)pvBuf, uCap);
  }
  stCbs.pfOnFrame = dmc__ser_on_frame;
  stCbs.pfOnErr = NULL;
  stCbs.pvUser = pstH;

  for (;;)
  {
    int iRem = iTimeoutMs;
    int iRawN = 0;

    if (dmc__ser_is_closing(pstH))
    {
      return -1;
    }
    if (iTimeoutMs > 0)
    {
      uint64 u64Now = dmc_now_ms();
      if (u64Now >= u64End)
      {
        return -2;
      }
      iRem = (int)(u64End - u64Now);
    }
    iRawN = dmc__ser_os_read(pstH, au8Raw, sizeof(au8Raw), iRem);
    if (iRawN <= 0)
    {
      return iRawN;
    }
    (void)dmc_sync_dec_feed_ex(&pstH->stDec, au8Raw, (size_t)iRawN, &stCbs, &pstH->stStats);
    if (pstH->bFifoErr)
    {
      return -1;
    }
    if (pstH->uFifoCount > 0u)
    {
      return (int)dmc__ser_fifo_pop(pstH, (uint8 *)pvBuf, uCap);
    }
    if (0 == iTimeoutMs)
    {
      return -2;
    }
  }
}

static int dmc__serial_read(dm_io void *pvHandle, dm_out void *pvBuf, dm_in size_t uCap,
                            dm_in int iTimeoutMs)
{
  dmc__serial_handle_st *pstH = (dmc__serial_handle_st *)pvHandle;
  int iRc = -1;

  if ((NULL == pstH) || (NULL == pvBuf) || (0u == uCap))
  {
    return -1;
  }
  if (!dmc__ser_io_enter(pstH))
  {
    return -1;
  }
  iRc = dmc__serial_read_locked(pstH, pvBuf, uCap, iTimeoutMs);
  dmc__ser_io_leave(pstH);
  return iRc;
}

static int dmc__serial_flush_tx_frame(dm_io dmc__serial_handle_st *pstH)
{
  dm_frame_head_st stHead;
  size_t uEncLen = 0u;
  const void *pvBody = NULL;

  dm_frame_get(pstH->pu8TxBuf, &stHead);
  pvBody = (stHead.u32Len > 0u) ? (const void *)(pstH->pu8TxBuf + DM_FRAME_HEAD) : NULL;
  if (DMC_OK != dmc_sync_encode(&stHead, pvBody, pstH->pu8EncBuf, pstH->uEncCap, &uEncLen))
  {
    return -1;
  }
  return dmc__ser_os_write_all(pstH, pstH->pu8EncBuf, uEncLen);
}

static int dmc__serial_write(dm_io void *pvHandle, dm_in const void *pvBuf, dm_in size_t uLen)
{
  dmc__serial_handle_st *pstH = (dmc__serial_handle_st *)pvHandle;
  const uint8 *pu8In = (const uint8 *)pvBuf;
  size_t uOff = 0u;
  int iRet = 0;

  if ((NULL == pstH) || ((NULL == pvBuf) && (uLen > 0u)))
  {
    return -1;
  }
  if (!dmc__ser_io_enter(pstH))
  {
    return -1;
  }
  while (uOff < uLen)
  {
    size_t uTarget = (0u == pstH->uTxNeed) ? DM_FRAME_HEAD : pstH->uTxNeed;
    size_t uWant = uTarget - pstH->uTxLen;
    size_t uTake = ((uLen - uOff) < uWant) ? (uLen - uOff) : uWant;

    memcpy(pstH->pu8TxBuf + pstH->uTxLen, pu8In + uOff, uTake);
    pstH->uTxLen += uTake;
    uOff += uTake;

    if ((0u == pstH->uTxNeed) && (pstH->uTxLen >= DM_FRAME_HEAD))
    {
      dm_frame_head_st stHead;
      dm_frame_get(pstH->pu8TxBuf, &stHead);
      if (stHead.u32Len > pstH->u32FrameMax)
      {
        pstH->uTxLen = 0u;
        pstH->uTxNeed = 0u;
        iRet = -1;
        break;
      }
      pstH->uTxNeed = DM_FRAME_HEAD + (size_t)stHead.u32Len;
    }
    if ((pstH->uTxNeed > 0u) && (pstH->uTxLen == pstH->uTxNeed))
    {
      int iRc = dmc__serial_flush_tx_frame(pstH);
      pstH->uTxLen = 0u;
      pstH->uTxNeed = 0u;
      if (0 != iRc)
      {
        iRet = -1;
        break;
      }
    }
  }
  dmc__ser_io_leave(pstH);
  return iRet;
}

static void dmc__serial_close(dm_io void *pvHandle)
{
  dmc__serial_handle_st *pstH = (dmc__serial_handle_st *)pvHandle;

  if (NULL == pstH)
  {
    return;
  }
  dmc__ser_close_begin(pstH);
  dmc__ser_os_close(pstH);
  dmc__ser_sync_fini(pstH);
  dmc__serial_free_bufs(pstH);
  free(pstH);
  return;
}

void dmc__serial_get_stats(dm_in const void *pvHandle, dm_out dmc_sync_stats_st *pstOut)
{
  const dmc__serial_handle_st *pstH = (const dmc__serial_handle_st *)pvHandle;

  if (NULL == pstOut)
  {
    return;
  }
  memset(pstOut, 0, sizeof(*pstOut));
  if (NULL != pstH)
  {
    *pstOut = pstH->stStats;
  }
  return;
}

#ifdef _WIN32
void dmc__serial_get_expected_timeouts(dm_out uint32 *pu32Interval, dm_out uint32 *pu32Mult,
                                       dm_out uint32 *pu32Const)
{
  COMMTIMEOUTS stTo;
  dmc__ser_fill_win_timeouts(&stTo);
  if (NULL != pu32Interval)
  {
    *pu32Interval = (uint32)stTo.ReadIntervalTimeout;
  }
  if (NULL != pu32Mult)
  {
    *pu32Mult = (uint32)stTo.ReadTotalTimeoutMultiplier;
  }
  if (NULL != pu32Const)
  {
    *pu32Const = (uint32)stTo.ReadTotalTimeoutConstant;
  }
  return;
}
#endif

const dmc_transport_vtbl_st *dmc__serial_vtbl(void)
{
  static const dmc_transport_vtbl_st s_stSerialVtbl = {
      "serial", dmc__serial_open, dmc__serial_read, dmc__serial_write, dmc__serial_close,
  };
  return &s_stSerialVtbl;
}

#ifdef __cplusplus
}
#endif
