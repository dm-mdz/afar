/**
 ******************************************************************************
 * @file    os_info.c
 * @author  MDM
 * @version 2.1.0
 * @brief   DM 跨平台 OS 信息与时钟实现（DD-DM-2.1 §2.4）：
 *          dm_exepath / dm_os_tmpdir / dm_os_gethostname / dm_os_uname /
 *          dm_cwd / dm_chdir / dm_os_setenv / dm_os_unsetenv /
 *          dm_available_parallelism / dm_resident_set_memory /
 *          dm_get_total_memory / dm_get_free_memory / dm_clock_gettime。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include <dm/os.h>
#include <dm/mem.h>
#include "../uv_adapter.h"

#include <stdio.h>
#include <string.h>

extern int dm__uv_err_map(dm_in int iUvErr);

static int dm__os_copy_str(dm_in const char *pcSrc, dm_in size_t uLen, dm_out char *pcBuf,
                           dm_io size_t *puSize)
{
  if (*puSize <= uLen)
  {
    *puSize = uLen + 1u;
    return DM_TOO_BIG;
  }
  memcpy(pcBuf, pcSrc, uLen + 1u);
  *puSize = uLen;
  return DM_SUCCESS;
}

int dm_exepath(dm_out char *pcBuf, dm_io size_t *puSize)
{
  char acTmp[4096] = {0};
  size_t uGot = sizeof(acTmp);
  int iErr = 0;

  if ((NULL == pcBuf) || (NULL == puSize) || (0u == *puSize))
  {
    return DM_INVALID;
  }
  /* AUD-V21-P0-05：先读入内部大缓冲，彻底根除 libuv 三端静默截断 */
  iErr = dm_uv_exepath(acTmp, &uGot);
  if ((0 == iErr) && (uGot < (sizeof(acTmp) - 1u)))
  {
    acTmp[uGot] = '\0';
    return dm__os_copy_str(acTmp, uGot, pcBuf, puSize);
  }
  if ((0 == iErr) || (DM_UV_ENOBUFS == iErr))
  {
    size_t uBigCap = 32768u;
    char *pcBig = (char *)dm_malloc_tag(uBigCap, DM_MEM_TAG_OS);
    int iRet = DM_SUCCESS;
    if (NULL == pcBig)
    {
      return DM_ALLOC;
    }
    iErr = dm_uv_exepath(pcBig, &uBigCap);
    if (0 != iErr)
    {
      dm_free_tag(pcBig);
      return dm__uv_err_map(iErr);
    }
    pcBig[uBigCap] = '\0';
    iRet = dm__os_copy_str(pcBig, uBigCap, pcBuf, puSize);
    dm_free_tag(pcBig);
    return iRet;
  }
  return dm__uv_err_map(iErr);
}

int dm_os_tmpdir(dm_out char *pcBuf, dm_io size_t *puSize)
{
  char acTmp[4096] = {0};
  size_t uGot = sizeof(acTmp);
  int iErr = 0;

  if ((NULL == pcBuf) || (NULL == puSize) || (0u == *puSize))
  {
    return DM_INVALID;
  }
  /* AUD-V21-R2-P1-03：先查内部大缓冲完成 macOS 去尾斜杠后再按精确长度比对容量 */
  iErr = dm_uv_os_tmpdir(acTmp, &uGot);
  if (DM_UV_ENOBUFS == iErr)
  {
    size_t uAlloc = (uGot > 4096u) ? uGot : 32768u;
    char *pcBig = (char *)dm_malloc_tag(uAlloc, DM_MEM_TAG_OS);
    int iRet = DM_SUCCESS;
    if (NULL == pcBig)
    {
      return DM_ALLOC;
    }
    iErr = dm_uv_os_tmpdir(pcBig, &uAlloc);
    if (0 != iErr)
    {
      dm_free_tag(pcBig);
      return dm__uv_err_map(iErr);
    }
    pcBig[uAlloc] = '\0';
    iRet = dm__os_copy_str(pcBig, uAlloc, pcBuf, puSize);
    dm_free_tag(pcBig);
    return iRet;
  }
  if (0 != iErr)
  {
    return dm__uv_err_map(iErr);
  }
  acTmp[uGot] = '\0';
  return dm__os_copy_str(acTmp, uGot, pcBuf, puSize);
}

int dm_os_gethostname(dm_out char *pcBuf, dm_io size_t *puSize)
{
  char acTmp[512] = {0};
  size_t uGot = sizeof(acTmp);
  int iErr = 0;

  if ((NULL == pcBuf) || (NULL == puSize) || (0u == *puSize))
  {
    return DM_INVALID;
  }
  iErr = dm_uv_os_gethostname(acTmp, &uGot);
  if (DM_UV_ENOBUFS == iErr)
  {
    size_t uAlloc = (uGot > sizeof(acTmp)) ? uGot : 4096u;
    char *pcBig = (char *)dm_malloc_tag(uAlloc, DM_MEM_TAG_OS);
    int iRet = DM_SUCCESS;
    if (NULL == pcBig)
    {
      return DM_ALLOC;
    }
    iErr = dm_uv_os_gethostname(pcBig, &uAlloc);
    if (0 != iErr)
    {
      dm_free_tag(pcBig);
      return dm__uv_err_map(iErr);
    }
    pcBig[uAlloc] = '\0';
    iRet = dm__os_copy_str(pcBig, uAlloc, pcBuf, puSize);
    dm_free_tag(pcBig);
    return iRet;
  }
  if (0 != iErr)
  {
    return dm__uv_err_map(iErr);
  }
  acTmp[uGot] = '\0';
  return dm__os_copy_str(acTmp, uGot, pcBuf, puSize);
}

int dm_os_uname(dm_out dm_utsname_st *pstOut)
{
  dm_uv_utsname_st stUv;
  int iErr = 0;

  if (NULL == pstOut)
  {
    return DM_INVALID;
  }
  memset(pstOut, 0, sizeof(*pstOut));
  memset(&stUv, 0, sizeof(stUv));
  iErr = dm_uv_os_uname(&stUv);
  if (0 != iErr)
  {
    return dm__uv_err_map(iErr);
  }
#ifdef _WIN32
  /* AUD-V21-R2-P0-02：消除 MinGW64 输出 "MINGW32_NT-10.0" 漂移，统一归一化为 "Windows_NT" */
  snprintf(pstOut->acSysname, sizeof(pstOut->acSysname), "Windows_NT");
#else
  snprintf(pstOut->acSysname, sizeof(pstOut->acSysname), "%s", stUv.sysname);
#endif
  snprintf(pstOut->acRelease, sizeof(pstOut->acRelease), "%s", stUv.release);
  snprintf(pstOut->acVersion, sizeof(pstOut->acVersion), "%s", stUv.version);
  snprintf(pstOut->acMachine, sizeof(pstOut->acMachine), "%s", stUv.machine);
  return DM_SUCCESS;
}

int dm_cwd(dm_out char *pcBuf, dm_io size_t *puSize)
{
  char acTmp[4096] = {0};
  size_t uGot = sizeof(acTmp);
  int iErr = 0;

  if ((NULL == pcBuf) || (NULL == puSize) || (0u == *puSize))
  {
    return DM_INVALID;
  }
  iErr = dm_uv_cwd(acTmp, &uGot);
  if (DM_UV_ENOBUFS == iErr)
  {
    size_t uAlloc = (uGot > sizeof(acTmp)) ? uGot : 32768u;
    char *pcBig = (char *)dm_malloc_tag(uAlloc, DM_MEM_TAG_OS);
    int iRet = DM_SUCCESS;
    if (NULL == pcBig)
    {
      return DM_ALLOC;
    }
    iErr = dm_uv_cwd(pcBig, &uAlloc);
    if (0 != iErr)
    {
      dm_free_tag(pcBig);
      return dm__uv_err_map(iErr);
    }
    pcBig[uAlloc] = '\0';
    iRet = dm__os_copy_str(pcBig, uAlloc, pcBuf, puSize);
    dm_free_tag(pcBig);
    return iRet;
  }
  if (0 != iErr)
  {
    return dm__uv_err_map(iErr);
  }
  acTmp[uGot] = '\0';
  return dm__os_copy_str(acTmp, uGot, pcBuf, puSize);
}

int dm_chdir(dm_in const char *pcDirUtf8)
{
  if ((NULL == pcDirUtf8) || ('\0' == pcDirUtf8[0]))
  {
    return DM_INVALID;
  }
  return dm__uv_err_map(dm_uv_chdir(pcDirUtf8));
}

int dm_os_setenv(dm_in const char *pcName, dm_in const char *pcValue)
{
  if ((NULL == pcName) || ('\0' == pcName[0]) || (NULL != strchr(pcName, '=')) || (NULL == pcValue))
  {
    return DM_INVALID;
  }
  return dm__uv_err_map(dm_uv_os_setenv(pcName, pcValue));
}

int dm_os_unsetenv(dm_in const char *pcName)
{
  if ((NULL == pcName) || ('\0' == pcName[0]) || (NULL != strchr(pcName, '=')))
  {
    return DM_INVALID;
  }
  return dm__uv_err_map(dm_uv_os_unsetenv(pcName));
}

uint32 dm_available_parallelism(void)
{
  unsigned int uiN = dm_uv_available_parallelism();
  return (uiN > 0u) ? (uint32)uiN : 1u;
}

int dm_resident_set_memory(dm_out size_t *puRssBytes)
{
  if (NULL == puRssBytes)
  {
    return DM_INVALID;
  }
  return dm__uv_err_map(dm_uv_resident_set_memory(puRssBytes));
}

uint64 dm_get_total_memory(void)
{
  return dm_uv_get_total_memory();
}

uint64 dm_get_free_memory(void)
{
  return dm_uv_get_free_memory();
}

int dm_clock_gettime(dm_in dm_clock_id_et eClockId, dm_out dm_timespec_st *pstOut)
{
  dm_uv_clock_id_et eUvClk = DM_UV_CLOCK_MONOTONIC;
  dm_uv_timespec64_st stTs;
  int iErr = 0;

  if (NULL == pstOut)
  {
    return DM_INVALID;
  }
  switch (eClockId)
  {
  case DM_CLOCK_MONOTONIC:
    eUvClk = DM_UV_CLOCK_MONOTONIC;
    break;
  case DM_CLOCK_REALTIME:
    eUvClk = DM_UV_CLOCK_REALTIME;
    break;
  default:
    return DM_INVALID;
  }
  memset(&stTs, 0, sizeof(stTs));
  iErr = dm_uv_clock_gettime(eUvClk, &stTs);
  if (0 != iErr)
  {
    return dm__uv_err_map(iErr);
  }
  pstOut->i64Sec = (int64)stTs.tv_sec;
  pstOut->i32Nsec = (int32)stTs.tv_nsec;
  return DM_SUCCESS;
}

#ifdef __cplusplus
}
#endif
