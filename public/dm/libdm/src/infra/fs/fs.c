/**
 ******************************************************************************
 * @file    fs.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   文件系统与安全随机基础设施（从 module.c 拆出，零向上依赖）：
 *          递归建目录、数据目录独占锁、原子写文件、跨平台替换、安全口令与 stdin 逐字节读行。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "dm"
#include <dm.h>
#include <dm/module.h>
#include <dm/log.h>
#include "../../uv_adapter.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "fs_internal.h"

#ifdef _WIN32
#define dm__fs_getpid() ((long)GetCurrentProcessId())
#else
#define dm__fs_getpid() ((long)getpid())
#endif

#ifdef _WIN32
static HANDLE g_hLock = INVALID_HANDLE_VALUE;
#else
static int g_iLockFd = -1;
#endif

/* ============================ 目录与数据目录独占锁 ============================ */

/* 递归建目录；已存在不算错 */
void dm__mkdirs(in const char *pcPath)
{
  char acTmp[512] = {0};
  size_t uLen = 0;
  size_t uIdx = 0;
  char cSaved = '\0';
  dm_fs_st stReq = {0};

  uLen = strlen(pcPath);
  if ((0u == uLen) || (uLen >= sizeof(acTmp)))
  {
    return;
  }
  memcpy(acTmp, pcPath, uLen + 1u);
  for (uIdx = 1; uIdx <= uLen; uIdx++)
  {
    if (('/' == acTmp[uIdx]) || ('\\' == acTmp[uIdx]) || ('\0' == acTmp[uIdx]))
    {
      cSaved = acTmp[uIdx];
      acTmp[uIdx] = '\0';
      memset(&stReq, 0, sizeof(stReq));
      (void)dm_fs_mkdir(NULL, &stReq, acTmp, 0700, NULL); /* 已存在会失败，不用管 */
      dm_fs_req_cleanup(&stReq);
      acTmp[uIdx] = cSaved;
    }
  }
  return;
}

/* 拿数据目录的独占锁。拿不到说明同一个目录已经有一个核心在跑 */
int dm__lock_take(in const char *pcDir, in const char *pcLockName)
{
  char acPath[600] = {0};
  const char *pcName = ((NULL != pcLockName) && ('\0' != pcLockName[0])) ? pcLockName : "dm.lock";
  int iPathLen = 0;

  iPathLen = snprintf(acPath, sizeof(acPath), "%s/%s", pcDir, pcName);
  if ((iPathLen < 0) || ((size_t)iPathLen >= sizeof(acPath)))
  {
    return DM_TOO_BIG;
  }
#ifdef _WIN32
  {
    WCHAR awcPath[600] = {0};
    if (0 == MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, acPath, -1, awcPath, 600))
    {
      return DM_IO; /* 路径按 UTF-8 解释 */
    }
    g_hLock = CreateFileW(awcPath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
  }
  if (INVALID_HANDLE_VALUE == g_hLock)
  {
    return (ERROR_SHARING_VIOLATION == GetLastError()) ? DM_BUSY : DM_IO;
  }
#else
  g_iLockFd = open(acPath, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (g_iLockFd < 0)
  {
    return DM_IO;
  }
  if (0 != flock(g_iLockFd, LOCK_EX | LOCK_NB))
  {
    close(g_iLockFd);
    g_iLockFd = -1;
    return DM_BUSY;
  }
#endif
  return DM_SUCCESS;
}

void dm__lock_release(void)
{
#ifdef _WIN32
  if (INVALID_HANDLE_VALUE != g_hLock)
  {
    CloseHandle(g_hLock);
    g_hLock = INVALID_HANDLE_VALUE;
  }
#else
  if (g_iLockFd >= 0)
  {
    close(g_iLockFd); /* 关掉文件锁就放了 */
    g_iLockFd = -1;
  }
#endif
  return;
}

/* ============================ 标准输入读行与安全口令 ============================ */

int dm_stdin_line(out char *pcBuf, in uint uiSize)
{
  uint uiLen = 0;
  char c = '\0';
  long lGot = 0;

  if ((NULL == pcBuf) || (0u == uiSize))
  {
    return DM_INVALID;
  }
  for (;;)
  {
#ifdef _WIN32
    lGot = (long)_read(0, &c, 1);
#else
    lGot = (long)read(0, &c, 1);
#endif
    if (lGot <= 0)
    {
      if (0u == uiLen)
      {
        pcBuf[0] = '\0';
        return DM_IO;
      }
      break;
    }
    if ('\n' == c)
    {
      break;
    }
    if ('\r' == c)
    {
      continue;
    }
    if (uiLen + 1u >= uiSize)
    {
      pcBuf[uiLen] = '\0';
      return DM_OVERFLOW;
    }
    pcBuf[uiLen++] = c;
  }
  pcBuf[uiLen] = '\0';
  return (int)uiLen;
}

int dm_token_make(out char *pcBuf, in uint uiSize)
{
  static const char acHex[] = "0123456789abcdef";
  uint8 au8Rand[32] = {0};
  uint uiIdx = 0;
  int iErrCode = DM_SUCCESS;

  if ((NULL == pcBuf) || (uiSize < sizeof(au8Rand) * 2u + 1u))
  {
    return DM_OVERFLOW;
  }
  iErrCode = dm_random(NULL, NULL, au8Rand, sizeof(au8Rand), 0, NULL); /* 同步、系统的安全随机数 */
  if (DM_SUCCESS != iErrCode)
  {
    return iErrCode;
  }
  for (uiIdx = 0; uiIdx < sizeof(au8Rand); uiIdx++)
  {
    pcBuf[uiIdx * 2u] = acHex[au8Rand[uiIdx] >> 4];
    pcBuf[uiIdx * 2u + 1u] = acHex[au8Rand[uiIdx] & 0x0Fu];
  }
  pcBuf[sizeof(au8Rand) * 2u] = '\0';
  return DM_SUCCESS;
}

/* ============================ 跨平台替换与原子写文件 ============================ */

/* 文件路径一律按 UTF-8：走适配层同步文件调用（Windows 内部转宽字符） */
#ifdef _WIN32
static bool_t dm__win_path_is_dir(in const char *pcPath)
{
  WCHAR awcPath[1024] = {0};
  DWORD dwAttr = INVALID_FILE_ATTRIBUTES;

  if (0 == MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pcPath, -1, awcPath, 1024))
  {
    return DM_FALSE;
  }
  dwAttr = GetFileAttributesW(awcPath);
  return ((INVALID_FILE_ATTRIBUTES != dwAttr) && (0u != (dwAttr & FILE_ATTRIBUTE_DIRECTORY)))
             ? DM_TRUE
             : DM_FALSE;
}
#endif

int dm__fs_replace(in const char *pcFrom, in const char *pcTo)
{
  dm_fs_st stReq = {0};
  int iRet = 0;
#ifdef _WIN32
  int iTry = 0;
  unsigned int uiWaitMs = 2u;
  for (iTry = 0; iTry < 6; iTry++)
  {
    memset(&stReq, 0, sizeof(stReq));
    iRet = dm_fs_rename(NULL, &stReq, pcFrom, pcTo, NULL); /* 目标已有就替换 */
    dm_fs_req_cleanup(&stReq);
    if (0 == iRet)
    {
      return DM_SUCCESS;
    }
    if (((DM_EACCES != iRet) && (DM_EPERM != iRet) && (DM_EBUSY != iRet)) ||
        dm__win_path_is_dir(pcTo))
    {
      break;
    }
    if (iTry + 1 < 6)
    {
      dm_sleep(uiWaitMs);
      uiWaitMs *= 2u;
    }
  }
  return DM_IO;
#else
  memset(&stReq, 0, sizeof(stReq));
  iRet = dm_fs_rename(NULL, &stReq, pcFrom, pcTo, NULL); /* 目标已有就替换 */
  dm_fs_req_cleanup(&stReq);
  return (0 == iRet) ? DM_SUCCESS : DM_IO;
#endif
}

char *dm__file_read_all(in const char *pcPath, in size_t uMax, out size_t *puLen)
{
  dm_fs_st stReq = {0};
  dm_file iFd = -1;
  char *pcText = NULL;
  size_t uLen = 0;
  size_t uCap = 0;

  *puLen = 0;
  memset(&stReq, 0, sizeof(stReq));
  iFd = (dm_file)dm_fs_open(dm_default_loop(), &stReq, pcPath, DM_FS_O_RDONLY, 0, NULL);
  dm_fs_req_cleanup(&stReq);
  if (iFd < 0)
  {
    return NULL;
  }
  for (;;)
  {
    dm_buf_st stBuf = {0};
    int iGot = 0;
    if (uLen >= uMax)
    {
      break; /* 读满上限：后面的不要 */
    }
    if (uLen + 1u >= uCap)
    {
      size_t uNew = (0u != uCap) ? uCap * 2u : 4096u;
      char *pcNew = NULL;
      uNew = (uNew > uMax + 1u) ? (uMax + 1u) : uNew;
      pcNew = (NULL == pcText) ? (char *)dm_malloc_tag(uNew, DM_MEM_TAG_UV_FS)
                               : (char *)dm_realloc_tag(pcText, uNew);
      if (NULL == pcNew)
      {
        break; /* 没内存：只要读到的 */
      }
      pcText = pcNew;
      uCap = uNew;
    }
    stBuf = dm_buf_init(pcText + uLen, (unsigned int)(uCap - 1u - uLen));
    memset(&stReq, 0, sizeof(stReq));
    iGot = dm_fs_read(dm_default_loop(), &stReq, iFd, &stBuf, 1u, -1, NULL);
    dm_fs_req_cleanup(&stReq);
    if (iGot <= 0)
    {
      break;
    }
    uLen += (size_t)iGot;
  }
  memset(&stReq, 0, sizeof(stReq));
  (void)dm_fs_close(dm_default_loop(), &stReq, iFd, NULL);
  dm_fs_req_cleanup(&stReq);
  if (NULL != pcText)
  {
    pcText[uLen] = '\0';
  }
  *puLen = uLen;
  return pcText;
}

static _Atomic uint64 g_u64AtomicSeq = 0;

static void dm__fs_unlink_quiet(in const char *pcPath)
{
  dm_fs_st stReq = {0};
  memset(&stReq, 0, sizeof(stReq));
  (void)dm_fs_unlink(NULL, &stReq, pcPath, NULL);
  dm_fs_req_cleanup(&stReq);
  return;
}

int dm_fs_write_atomic(in const char *pcPath, in const void *pvData, in size_t uLen)
{
  char acTmp[1024] = {0};
  dm_fs_st stReq = {0};
  dm_file iFd = -1;
  uint64 u64Seq = 0;
  int iPathLen = 0;
  size_t uWritten = 0;
  int iCloseRet = 0;
  int iSyncRet = 0;
  int iErr = DM_IO;
  bool_t bOk = DM_FALSE;

  if ((NULL == pcPath) || ('\0' == pcPath[0]) || ((uLen > 0u) && (NULL == pvData)))
  {
    return DM_INVALID;
  }
  u64Seq = atomic_fetch_add(&g_u64AtomicSeq, 1u) + 1u;
  iPathLen = snprintf(acTmp, sizeof(acTmp), "%s.tmp.%ld.%llu", pcPath, dm__fs_getpid(),
                      (unsigned long long)u64Seq);
  if ((iPathLen < 0) || ((size_t)iPathLen >= sizeof(acTmp)))
  {
    return DM_TOO_BIG;
  }
  memset(&stReq, 0, sizeof(stReq));
  iFd = (dm_file)dm_fs_open(NULL, &stReq, acTmp, DM_FS_O_WRONLY | DM_FS_O_CREAT | DM_FS_O_EXCL,
                            0600, NULL);
  dm_fs_req_cleanup(&stReq);
  if (iFd < 0)
  {
    return DM_IO;
  }
  bOk = DM_TRUE;
  while (uWritten < uLen)
  {
    size_t uLeft = uLen - uWritten;
    unsigned int uiChunk = (uLeft > 0x7FFFFFFFu) ? 0x7FFFFFFFu : (unsigned int)uLeft;
    dm_buf_st stBuf = dm_buf_init((char *)((const uint8 *)pvData + uWritten), uiChunk);
    int iGot = 0;

    memset(&stReq, 0, sizeof(stReq));
    iGot = dm_fs_write(NULL, &stReq, iFd, &stBuf, 1u, -1, NULL);
    dm_fs_req_cleanup(&stReq);
    if (iGot <= 0)
    {
      bOk = DM_FALSE;
      break;
    }
    uWritten += (size_t)iGot;
  }
  if (bOk)
  {
    memset(&stReq, 0, sizeof(stReq));
    iSyncRet = dm_fs_fsync(NULL, &stReq, iFd, NULL);
    dm_fs_req_cleanup(&stReq);
    if (0 != iSyncRet)
    {
      bOk = DM_FALSE;
    }
  }
  memset(&stReq, 0, sizeof(stReq));
  iCloseRet = dm_fs_close(NULL, &stReq, iFd, NULL);
  dm_fs_req_cleanup(&stReq);
  if (0 != iCloseRet)
  {
    bOk = DM_FALSE;
  }
  if (!bOk)
  {
    dm__fs_unlink_quiet(acTmp);
    return DM_IO;
  }
  iErr = dm__fs_replace(acTmp, pcPath);
  if (DM_SUCCESS != iErr)
  {
    dm__fs_unlink_quiet(acTmp);
    return iErr;
  }
  return DM_SUCCESS;
}

int dm__file_write_atomic(in const char *pcPath, in const char *pcText, in size_t uLen)
{
  return dm_fs_write_atomic(pcPath, pcText, uLen);
}

#ifdef __cplusplus
}
#endif
