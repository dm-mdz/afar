/**
 ******************************************************************************
 * @file    fs_ext.c
 * @author  MDM
 * @version 2.1.0
 * @brief   DM 跨平台文件系统扩展实现（DD-DM-2.1 §2.5）：
 *          stat/lstat、realpath（Win UNC/正斜杠规范化）、link（Win CreateHardLinkW 精确映射）、
 *          mkdir/mkdirs（Win 盘符与 UNC 根跳过）、unlink/rmdir/rename、scandir 与 UTF 编解码。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include <dm/fs.h>
#include <dm/mem.h>
#include "../../uv_adapter.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#endif

extern int dm__uv_err_map(dm_in int iUvErr);

static bool_t dm__fs_is_sep(dm_in char cCh)
{
  return (('/' == cCh) || ('\\' == cCh)) ? DM_TRUE : DM_FALSE;
}

static void dm__fs_fill_stat(dm_in const dm_uv_stat_st *pstUv, dm_out dm_fs_stat_st *pstOut)
{
  uint64 u64Mode = pstUv->st_mode;

  memset(pstOut, 0, sizeof(*pstOut));
  pstOut->bIsFile = ((u64Mode & S_IFMT) == S_IFREG) ? DM_TRUE : DM_FALSE;
  pstOut->bIsDir = ((u64Mode & S_IFMT) == S_IFDIR) ? DM_TRUE : DM_FALSE;
#ifdef S_IFLNK
  pstOut->bIsSymlink = ((u64Mode & S_IFMT) == S_IFLNK) ? DM_TRUE : DM_FALSE;
#else
  pstOut->bIsSymlink = ((u64Mode & S_IFMT) == 0120000u) ? DM_TRUE : DM_FALSE;
#endif
  pstOut->u32Mode = (uint32)(u64Mode & 07777u);
  pstOut->u64Size = (uint64)pstUv->st_size;
  pstOut->i64MtimeMs =
      ((int64)pstUv->st_mtim.tv_sec * 1000ll) + ((int64)pstUv->st_mtim.tv_nsec / 1000000ll);
  pstOut->i64MtimeNs =
      ((int64)pstUv->st_mtim.tv_sec * 1000000000ll) + (int64)pstUv->st_mtim.tv_nsec;
  pstOut->i64AtimeMs =
      ((int64)pstUv->st_atim.tv_sec * 1000ll) + ((int64)pstUv->st_atim.tv_nsec / 1000000ll);
  pstOut->i64CtimeMs =
      ((int64)pstUv->st_ctim.tv_sec * 1000ll) + ((int64)pstUv->st_ctim.tv_nsec / 1000000ll);
  return;
}

static int dm__fs_stat_impl(dm_in const char *pcPathUtf8, dm_in bool_t bFollow,
                            dm_out dm_fs_stat_st *pstOut)
{
  dm_uv_fs_st stReq;
  int iErr = 0;

  if ((NULL == pcPathUtf8) || ('\0' == pcPathUtf8[0]) || (NULL == pstOut))
  {
    return DM_INVALID;
  }
  memset(&stReq, 0, sizeof(stReq));
  iErr = (DM_TRUE == bFollow) ? dm_uv_fs_stat(NULL, &stReq, pcPathUtf8, NULL)
                              : dm_uv_fs_lstat(NULL, &stReq, pcPathUtf8, NULL);
  if (0 != iErr)
  {
    dm_uv_fs_req_cleanup(&stReq);
    return dm__uv_err_map(iErr);
  }
  dm__fs_fill_stat(dm_uv_fs_get_statbuf(&stReq), pstOut);
  dm_uv_fs_req_cleanup(&stReq);
  return DM_SUCCESS;
}

int dm_fs_stat(dm_in const char *pcPathUtf8, dm_out dm_fs_stat_st *pstOut)
{
  return dm__fs_stat_impl(pcPathUtf8, DM_TRUE, pstOut);
}

int dm_fs_lstat(dm_in const char *pcPathUtf8, dm_out dm_fs_stat_st *pstOut)
{
  return dm__fs_stat_impl(pcPathUtf8, DM_FALSE, pstOut);
}

#ifdef _WIN32
static bool_t dm__fs_prefix_ci_unc(dm_in const char *pcPath)
{
  return (('\\' == pcPath[0]) && ('\\' == pcPath[1]) && ('?' == pcPath[2]) && ('\\' == pcPath[3]) &&
          (('U' == pcPath[4]) || ('u' == pcPath[4])) &&
          (('N' == pcPath[5]) || ('n' == pcPath[5])) &&
          (('C' == pcPath[6]) || ('c' == pcPath[6])) && ('\\' == pcPath[7]))
             ? DM_TRUE
             : DM_FALSE;
}

static size_t dm__fs_win_norm_path(dm_in const char *pcRaw, dm_out char *pcDst)
{
  size_t uOut = 0;
  const char *pcSrc = pcRaw;

  if (DM_TRUE == dm__fs_prefix_ci_unc(pcRaw))
  {
    pcDst[uOut++] = '/';
    pcDst[uOut++] = '/';
    pcSrc = pcRaw + 8;
  }
  else if (0 == strncmp(pcRaw, "\\\\?\\", 4u))
  {
    pcSrc = pcRaw + 4;
  }
  else if (('\\' == pcRaw[0]) && ('\\' == pcRaw[1]))
  {
    pcDst[uOut++] = '/';
    pcDst[uOut++] = '/';
    pcSrc = pcRaw + 2;
  }
  while ('\0' != *pcSrc)
  {
    pcDst[uOut++] = ('\\' == *pcSrc) ? '/' : *pcSrc;
    pcSrc++;
  }
  pcDst[uOut] = '\0';
  return uOut;
}
#endif

int dm_fs_realpath(dm_in const char *pcPathUtf8, dm_out char *pcBuf, dm_io size_t *puSize)
{
  dm_uv_fs_st stReq;
  const char *pcRaw = NULL;
  const char *pcFinal = NULL;
  char acStack[4096] = {0};
  char *pcHeap = NULL;
  size_t uLen = 0;
  int iErr = 0;

  if ((NULL == pcPathUtf8) || ('\0' == pcPathUtf8[0]) || (NULL == pcBuf) || (NULL == puSize) ||
      (0u == *puSize))
  {
    return DM_INVALID;
  }
  memset(&stReq, 0, sizeof(stReq));
  iErr = dm_uv_fs_realpath(NULL, &stReq, pcPathUtf8, NULL);
  if (0 != iErr)
  {
    dm_uv_fs_req_cleanup(&stReq);
    return dm__uv_err_map(iErr);
  }
  pcRaw = (const char *)dm_uv_fs_get_ptr(&stReq);
  if (NULL == pcRaw)
  {
    dm_uv_fs_req_cleanup(&stReq);
    return DM_IO;
  }
#ifdef _WIN32
  {
    size_t uRawLen = strlen(pcRaw);
    char *pcNorm = acStack;
    if ((uRawLen + 4u) > sizeof(acStack))
    {
      pcHeap = (char *)dm_malloc_tag(uRawLen + 4u, DM_MEM_TAG_FS);
      if (NULL == pcHeap)
      {
        dm_uv_fs_req_cleanup(&stReq);
        return DM_ALLOC;
      }
      pcNorm = pcHeap;
    }
    uLen = dm__fs_win_norm_path(pcRaw, pcNorm);
    pcFinal = pcNorm;
  }
#else
  (void)acStack;
  pcFinal = pcRaw;
  uLen = strlen(pcFinal);
#endif
  if (*puSize <= uLen)
  {
    *puSize = uLen + 1u;
    dm_free_tag(pcHeap);
    dm_uv_fs_req_cleanup(&stReq);
    return DM_TOO_BIG;
  }
  memcpy(pcBuf, pcFinal, uLen + 1u);
  *puSize = uLen;
  dm_free_tag(pcHeap);
  dm_uv_fs_req_cleanup(&stReq);
  return DM_SUCCESS;
}

#ifdef _WIN32
static int dm__fs_win_link_err(dm_in DWORD dwErr, dm_in const uint16 *pu16Src)
{
  switch (dwErr)
  {
  case ERROR_NOT_SAME_DEVICE:
    return DM_EXDEV;
  case ERROR_INVALID_FUNCTION:
  case ERROR_NOT_SUPPORTED:
  case ERROR_TOO_MANY_LINKS:
    return DM_UNSUPPORTED;
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
    return DM_NOT_FOUND;
  case ERROR_ALREADY_EXISTS:
  case ERROR_FILE_EXISTS:
    return DM_EXISTS;
  case ERROR_ACCESS_DENIED:
  {
    DWORD dwAttr = GetFileAttributesW((LPCWSTR)pu16Src);
    if ((INVALID_FILE_ATTRIBUTES != dwAttr) && (0u != (dwAttr & FILE_ATTRIBUTE_DIRECTORY)))
    {
      return DM_IS_DIR;
    }
    return DM_PERM;
  }
  case ERROR_INVALID_NAME:
  case ERROR_INVALID_PARAMETER:
    return DM_INVALID;
  default:
    return DM_IO;
  }
}
#endif

int dm_fs_link(dm_in const char *pcSrcUtf8, dm_in const char *pcDstUtf8)
{
  if ((NULL == pcSrcUtf8) || ('\0' == pcSrcUtf8[0]) || (NULL == pcDstUtf8) ||
      ('\0' == pcDstUtf8[0]))
  {
    return DM_INVALID;
  }
#ifdef _WIN32
  {
    uint16 *pu16Src = NULL;
    uint16 *pu16Dst = NULL;
    DWORD dwAttr = INVALID_FILE_ATTRIBUTES;
    int iErr = DM_SUCCESS;

    iErr = dm_utf8_to_utf16_alloc(pcSrcUtf8, &pu16Src, NULL);
    if (DM_SUCCESS != iErr)
    {
      return iErr;
    }
    iErr = dm_utf8_to_utf16_alloc(pcDstUtf8, &pu16Dst, NULL);
    if (DM_SUCCESS != iErr)
    {
      dm_free_tag(pu16Src);
      return iErr;
    }
    dwAttr = GetFileAttributesW((LPCWSTR)pu16Src);
    if ((INVALID_FILE_ATTRIBUTES != dwAttr) && (0u != (dwAttr & FILE_ATTRIBUTE_DIRECTORY)))
    {
      dm_free_tag(pu16Dst);
      dm_free_tag(pu16Src);
      return DM_IS_DIR;
    }
    if (!CreateHardLinkW((LPCWSTR)pu16Dst, (LPCWSTR)pu16Src, NULL))
    {
      iErr = dm__fs_win_link_err(GetLastError(), pu16Src);
    }
    dm_free_tag(pu16Dst);
    dm_free_tag(pu16Src);
    return iErr;
  }
#else
  {
    dm_uv_fs_st stReq;
    int iErr = 0;

    memset(&stReq, 0, sizeof(stReq));
    iErr = dm_uv_fs_link(NULL, &stReq, pcSrcUtf8, pcDstUtf8, NULL);
    dm_uv_fs_req_cleanup(&stReq);
    return dm__uv_err_map(iErr);
  }
#endif
}

int dm_fs_mkdir(dm_in const char *pcDirUtf8, dm_in uint32 u32Mode)
{
  dm_uv_fs_st stReq;
  int iMode = (0u == u32Mode) ? 0755 : (int)u32Mode;
  int iErr = 0;

  if ((NULL == pcDirUtf8) || ('\0' == pcDirUtf8[0]))
  {
    return DM_INVALID;
  }
  memset(&stReq, 0, sizeof(stReq));
  iErr = dm_uv_fs_mkdir(NULL, &stReq, pcDirUtf8, iMode, NULL);
  dm_uv_fs_req_cleanup(&stReq);
  return dm__uv_err_map(iErr);
}

static size_t dm__fs_mkdirs_root_skip(dm_in const char *pcPath, dm_in size_t uLen)
{
#ifdef _WIN32
  if ((uLen >= 2u) && isalpha((unsigned char)pcPath[0]) && (':' == pcPath[1]))
  {
    return ((uLen >= 3u) && (DM_TRUE == dm__fs_is_sep(pcPath[2]))) ? 3u : 2u;
  }
  if ((uLen >= 2u) && (DM_TRUE == dm__fs_is_sep(pcPath[0])) &&
      (DM_TRUE == dm__fs_is_sep(pcPath[1])))
  {
    size_t uPos = 2u;
    int iSeg = 0;
    while ((uPos < uLen) && (iSeg < 2))
    {
      while ((uPos < uLen) && (DM_TRUE != dm__fs_is_sep(pcPath[uPos])))
      {
        uPos++;
      }
      while ((uPos < uLen) && (DM_TRUE == dm__fs_is_sep(pcPath[uPos])))
      {
        uPos++;
      }
      iSeg++;
    }
    return uPos;
  }
  return (DM_TRUE == dm__fs_is_sep(pcPath[0])) ? 1u : 0u;
#else
  (void)uLen;
  return ('/' == pcPath[0]) ? 1u : 0u;
#endif
}

static int dm__fs_mkdir_one(dm_in const char *pcDir, dm_in int iMode)
{
  dm_uv_fs_st stReq;
  dm_fs_stat_st stStat;
  int iErr = 0;

  memset(&stReq, 0, sizeof(stReq));
  iErr = dm_uv_fs_mkdir(NULL, &stReq, pcDir, iMode, NULL);
  dm_uv_fs_req_cleanup(&stReq);
  if (0 == iErr)
  {
    return DM_SUCCESS;
  }
  if (DM_SUCCESS == dm_fs_stat(pcDir, &stStat))
  {
    return (DM_TRUE == stStat.bIsDir) ? DM_SUCCESS : DM_NOT_DIR;
  }
  return dm__uv_err_map(iErr);
}

int dm_fs_mkdirs(dm_in const char *pcDirUtf8, dm_in uint32 u32Mode)
{
  char acStack[1024] = {0};
  char *pcHeap = NULL;
  char *pcWork = acStack;
  size_t uLen = 0;
  size_t uRoot = 0;
  size_t uIdx = 0;
  int iMode = (0u == u32Mode) ? 0755 : (int)u32Mode;
  int iErr = DM_SUCCESS;

  if ((NULL == pcDirUtf8) || ('\0' == pcDirUtf8[0]))
  {
    return DM_INVALID;
  }
  uLen = strlen(pcDirUtf8);
  if ((uLen + 1u) > sizeof(acStack))
  {
    pcHeap = (char *)dm_malloc_tag(uLen + 1u, DM_MEM_TAG_FS);
    if (NULL == pcHeap)
    {
      return DM_ALLOC;
    }
    pcWork = pcHeap;
  }
  memcpy(pcWork, pcDirUtf8, uLen + 1u);
  uRoot = dm__fs_mkdirs_root_skip(pcWork, uLen);
  while ((uLen > uRoot) && (DM_TRUE == dm__fs_is_sep(pcWork[uLen - 1u])))
  {
    pcWork[--uLen] = '\0';
  }
  if (uLen <= uRoot)
  {
    dm_fs_stat_st stStat;
    iErr = dm_fs_stat(pcWork, &stStat);
    dm_free_tag(pcHeap);
    if (DM_SUCCESS != iErr)
    {
      return iErr;
    }
    return (DM_TRUE == stStat.bIsDir) ? DM_SUCCESS : DM_NOT_DIR;
  }
  for (uIdx = uRoot; uIdx <= uLen; uIdx++)
  {
    char cCh = pcWork[uIdx];
    if ((DM_TRUE != dm__fs_is_sep(cCh)) && ('\0' != cCh))
    {
      continue;
    }
    if ((uIdx == uRoot) || ((uIdx > 0u) && (DM_TRUE == dm__fs_is_sep(pcWork[uIdx - 1u]))))
    {
      continue;
    }
    pcWork[uIdx] = '\0';
    iErr = dm__fs_mkdir_one(pcWork, iMode);
    pcWork[uIdx] = cCh;
    if (DM_SUCCESS != iErr)
    {
      break;
    }
  }
  dm_free_tag(pcHeap);
  return iErr;
}

int dm_fs_unlink(dm_in const char *pcPathUtf8)
{
  dm_uv_fs_st stReq;
  int iErr = 0;

  if ((NULL == pcPathUtf8) || ('\0' == pcPathUtf8[0]))
  {
    return DM_INVALID;
  }
  memset(&stReq, 0, sizeof(stReq));
  iErr = dm_uv_fs_unlink(NULL, &stReq, pcPathUtf8, NULL);
  dm_uv_fs_req_cleanup(&stReq);
  return dm__uv_err_map(iErr);
}

int dm_fs_rmdir(dm_in const char *pcDirUtf8)
{
  dm_uv_fs_st stReq;
  int iErr = 0;

  if ((NULL == pcDirUtf8) || ('\0' == pcDirUtf8[0]))
  {
    return DM_INVALID;
  }
  memset(&stReq, 0, sizeof(stReq));
  iErr = dm_uv_fs_rmdir(NULL, &stReq, pcDirUtf8, NULL);
  dm_uv_fs_req_cleanup(&stReq);
  return dm__uv_err_map(iErr);
}

int dm_fs_rename(dm_in const char *pcFromUtf8, dm_in const char *pcToUtf8)
{
  dm_uv_fs_st stReq;
  int iRet = 0;

  if ((NULL == pcFromUtf8) || ('\0' == pcFromUtf8[0]) || (NULL == pcToUtf8) ||
      ('\0' == pcToUtf8[0]))
  {
    return DM_INVALID;
  }
#ifdef _WIN32
  {
    int iTry = 0;
    unsigned int uiWaitMs = 2u;
    for (iTry = 0; iTry < 6; iTry++)
    {
      dm_fs_stat_st stDst;
      memset(&stReq, 0, sizeof(stReq));
      iRet = dm_uv_fs_rename(NULL, &stReq, pcFromUtf8, pcToUtf8, NULL);
      dm_uv_fs_req_cleanup(&stReq);
      if (0 == iRet)
      {
        return DM_SUCCESS;
      }
      if (((DM_UV_EACCES != iRet) && (DM_UV_EPERM != iRet) && (DM_UV_EBUSY != iRet)) ||
          ((DM_SUCCESS == dm_fs_stat(pcToUtf8, &stDst)) && (DM_TRUE == stDst.bIsDir)))
      {
        break;
      }
      if (iTry + 1 < 6)
      {
        dm_sleep(uiWaitMs);
        uiWaitMs *= 2u;
      }
    }
    return dm__uv_err_map(iRet);
  }
#else
  memset(&stReq, 0, sizeof(stReq));
  iRet = dm_uv_fs_rename(NULL, &stReq, pcFromUtf8, pcToUtf8, NULL);
  dm_uv_fs_req_cleanup(&stReq);
  return dm__uv_err_map(iRet);
#endif
}

static dm_fs_dirent_type_et dm__fs_dirent_type(dm_in dm_uv_dirent_st stEnt)
{
  switch (stEnt.type)
  {
  case DM_UV_DIRENT_FILE:
    return DM_FS_ENT_FILE;
  case DM_UV_DIRENT_DIR:
    return DM_FS_ENT_DIR;
  case DM_UV_DIRENT_LINK:
    return DM_FS_ENT_LINK;
  case DM_UV_DIRENT_UNKNOWN:
    return DM_FS_ENT_UNKNOWN;
  default:
    return DM_FS_ENT_OTHER;
  }
}

int dm_fs_scandir(dm_in const char *pcDirUtf8, dm_out dm_fs_dirent_st **ppastOut,
                  dm_out uint32 *pu32Count)
{
  dm_uv_fs_st stReq;
  dm_uv_dirent_st stEnt;
  dm_fs_dirent_st *pastList = NULL;
  uint32 u32Filled = 0;
  int iRes = 0;

  if ((NULL == ppastOut) || (NULL == pu32Count))
  {
    return DM_INVALID;
  }
  *ppastOut = NULL;
  *pu32Count = 0u;
  if ((NULL == pcDirUtf8) || ('\0' == pcDirUtf8[0]))
  {
    return DM_INVALID;
  }
  memset(&stReq, 0, sizeof(stReq));
  iRes = dm_uv_fs_scandir(NULL, &stReq, pcDirUtf8, 0, NULL);
  if (iRes < 0)
  {
    dm_uv_fs_req_cleanup(&stReq);
    return dm__uv_err_map(iRes);
  }
  if (0 == iRes)
  {
    dm_uv_fs_req_cleanup(&stReq);
    return DM_SUCCESS;
  }
  pastList = (dm_fs_dirent_st *)dm_calloc_tag((size_t)iRes, sizeof(dm_fs_dirent_st), DM_MEM_TAG_FS);
  if (NULL == pastList)
  {
    dm_uv_fs_req_cleanup(&stReq);
    return DM_ALLOC;
  }
  while (DM_UV_EOF != dm_uv_fs_scandir_next(&stReq, &stEnt))
  {
    size_t uNameLen = 0;
    char *pcCopy = NULL;
    if ((NULL == stEnt.name) || (0 == strcmp(stEnt.name, ".")) || (0 == strcmp(stEnt.name, "..")))
    {
      continue;
    }
    if (u32Filled >= (uint32)iRes)
    {
      break;
    }
    uNameLen = strlen(stEnt.name);
    pcCopy = (char *)dm_malloc_tag(uNameLen + 1u, DM_MEM_TAG_FS);
    if (NULL == pcCopy)
    {
      dm_fs_scandir_free(pastList, u32Filled);
      dm_uv_fs_req_cleanup(&stReq);
      return DM_ALLOC;
    }
    memcpy(pcCopy, stEnt.name, uNameLen + 1u);
    pastList[u32Filled].pcName = pcCopy;
    pastList[u32Filled].eType = dm__fs_dirent_type(stEnt);
    u32Filled++;
  }
  dm_uv_fs_req_cleanup(&stReq);
  if (0u == u32Filled)
  {
    dm_free_tag(pastList);
    return DM_SUCCESS;
  }
  *ppastOut = pastList;
  *pu32Count = u32Filled;
  return DM_SUCCESS;
}

void dm_fs_scandir_free(dm_io dm_fs_dirent_st *pastList, dm_in uint32 u32Count)
{
  uint32 u32Idx = 0;

  if (NULL == pastList)
  {
    return;
  }
  for (u32Idx = 0u; u32Idx < u32Count; u32Idx++)
  {
    dm_free_tag(pastList[u32Idx].pcName);
  }
  dm_free_tag(pastList);
  return;
}

/* ============================ UTF-8(WTF-8) <-> UTF-16 转换 ============================ */

size_t dm_utf16_len_as_utf8(dm_in const uint16 *pu16Src, dm_in int64 i64SrcUnits)
{
  ssize_t iSrcUnits = 0;

  if ((NULL == pu16Src) || (i64SrcUnits > (int64)SSIZE_MAX))
  {
    return 0u;
  }
  iSrcUnits = (i64SrcUnits < 0) ? (ssize_t)-1 : (ssize_t)i64SrcUnits;
  return dm_uv_utf16_length_as_wtf8(pu16Src, iSrcUnits);
}

int dm_utf16_to_utf8(dm_in const uint16 *pu16Src, dm_in int64 i64SrcUnits, dm_out char *pcOut,
                     dm_io size_t *puOutBytes)
{
  ssize_t iSrcUnits = 0;
  size_t uNeed = 0;
  size_t uWrite = 0;
  char *pcDst = NULL;
  int iErr = 0;

  if ((NULL == pu16Src) || (NULL == pcOut) || (NULL == puOutBytes) || (0u == *puOutBytes) ||
      (i64SrcUnits > (int64)SSIZE_MAX))
  {
    return DM_INVALID;
  }
  iSrcUnits = (i64SrcUnits < 0) ? (ssize_t)-1 : (ssize_t)i64SrcUnits;
  /* AUD-V21-R2-P1-01：先精确求长，避免小缓冲触发多字节虚报与半截损坏写入 */
  uNeed = dm_uv_utf16_length_as_wtf8(pu16Src, iSrcUnits);
  if (*puOutBytes <= uNeed)
  {
    *puOutBytes = uNeed + 1u;
    return DM_TOO_BIG;
  }
  pcDst = pcOut;
  uWrite = uNeed;
  iErr = dm_uv_utf16_to_wtf8(pu16Src, iSrcUnits, &pcDst, &uWrite);
  if (0 != iErr)
  {
    return dm__uv_err_map(iErr);
  }
  pcOut[uNeed] = '\0';
  *puOutBytes = uNeed;
  return DM_SUCCESS;
}

int dm_utf16_to_utf8_alloc(dm_in const uint16 *pu16Src, dm_in int64 i64SrcUnits,
                           dm_out char **ppcOut, dm_out size_t *puOutBytes)
{
  ssize_t iSrcUnits = 0;
  size_t uNeed = 0;
  size_t uWrite = 0;
  char *pcBuf = NULL;
  char *pcDst = NULL;
  int iErr = 0;

  if (NULL == ppcOut)
  {
    return DM_INVALID;
  }
  *ppcOut = NULL;
  if (NULL != puOutBytes)
  {
    *puOutBytes = 0u;
  }
  if ((NULL == pu16Src) || (i64SrcUnits > (int64)SSIZE_MAX))
  {
    return DM_INVALID;
  }
  iSrcUnits = (i64SrcUnits < 0) ? (ssize_t)-1 : (ssize_t)i64SrcUnits;
  uNeed = dm_uv_utf16_length_as_wtf8(pu16Src, iSrcUnits);
  /* AUD-V21-P0-08：显式按 DM_MEM_TAG_FS 分配，严禁交由 libuv 走无标签 uv__malloc */
  pcBuf = (char *)dm_malloc_tag(uNeed + 1u, DM_MEM_TAG_FS);
  if (NULL == pcBuf)
  {
    return DM_ALLOC;
  }
  pcDst = pcBuf;
  uWrite = uNeed;
  iErr = dm_uv_utf16_to_wtf8(pu16Src, iSrcUnits, &pcDst, &uWrite);
  if (0 != iErr)
  {
    dm_free_tag(pcBuf);
    return dm__uv_err_map(iErr);
  }
  pcBuf[uNeed] = '\0';
  *ppcOut = pcBuf;
  if (NULL != puOutBytes)
  {
    *puOutBytes = uNeed;
  }
  return DM_SUCCESS;
}

int64 dm_utf8_len_as_utf16(dm_in const char *pcSrcUtf8)
{
  ssize_t iNeed = 0;

  if (NULL == pcSrcUtf8)
  {
    return DM_INVALID;
  }
  iNeed = dm_uv_wtf8_length_as_utf16(pcSrcUtf8);
  if (iNeed < 1)
  {
    return DM_INVALID;
  }
  return (int64)(iNeed - 1);
}

int dm_utf8_to_utf16(dm_in const char *pcSrcUtf8, dm_out uint16 *pu16Out, dm_io size_t *puOutUnits)
{
  ssize_t iNeed = 0;

  if ((NULL == pcSrcUtf8) || (NULL == pu16Out) || (NULL == puOutUnits) || (0u == *puOutUnits))
  {
    return DM_INVALID;
  }
  iNeed = dm_uv_wtf8_length_as_utf16(pcSrcUtf8);
  if (iNeed < 1)
  {
    return DM_INVALID;
  }
  if (*puOutUnits < (size_t)iNeed)
  {
    *puOutUnits = (size_t)iNeed;
    return DM_TOO_BIG;
  }
  /* AUD-V21-P0-08：传入精确 iNeed（非 *puOutUnits），根除 Debug 下 assert(w_target_len == 0) */
  dm_uv_wtf8_to_utf16(pcSrcUtf8, pu16Out, (size_t)iNeed);
  *puOutUnits = (size_t)(iNeed - 1);
  return DM_SUCCESS;
}

int dm_utf8_to_utf16_alloc(dm_in const char *pcSrcUtf8, dm_out uint16 **ppu16Out,
                           dm_out size_t *puOutUnits)
{
  ssize_t iNeed = 0;
  uint16 *pu16Buf = NULL;

  if (NULL == ppu16Out)
  {
    return DM_INVALID;
  }
  *ppu16Out = NULL;
  if (NULL != puOutUnits)
  {
    *puOutUnits = 0u;
  }
  if (NULL == pcSrcUtf8)
  {
    return DM_INVALID;
  }
  iNeed = dm_uv_wtf8_length_as_utf16(pcSrcUtf8);
  if (iNeed < 1)
  {
    return DM_INVALID;
  }
  if ((size_t)iNeed > (SIZE_MAX / sizeof(uint16)))
  {
    return DM_ALLOC;
  }
  pu16Buf = (uint16 *)dm_malloc_tag((size_t)iNeed * sizeof(uint16), DM_MEM_TAG_FS);
  if (NULL == pu16Buf)
  {
    return DM_ALLOC;
  }
  dm_uv_wtf8_to_utf16(pcSrcUtf8, pu16Buf, (size_t)iNeed);
  *ppu16Out = pu16Buf;
  if (NULL != puOutUnits)
  {
    *puOutUnits = (size_t)(iNeed - 1);
  }
  return DM_SUCCESS;
}

#ifdef __cplusplus
}
#endif
