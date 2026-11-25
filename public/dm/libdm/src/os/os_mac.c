/**
 ******************************************************************************
 * @file    os_mac.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   macOS 通用 OS 原语实现（DD-01 §4，libproc + KERN_PROCARGS2 + 无环 BFS）。
 ******************************************************************************
 */
#ifdef __APPLE__

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "os"
#include "os_internal.h"
#include <dm/check.h>
#include <dm/mem.h>

#include <dirent.h>
#include <errno.h>
#include <libproc.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* ============================ 进程查询与进程树终止 ============================ */

static int dm__os_mac_bsdinfo(dm_in int iPid, dm_out struct proc_bsdinfo *pstInfo)
{
  int iRet = proc_pidinfo(iPid, PROC_PIDTBSDINFO, 0, pstInfo, (int)sizeof(*pstInfo));
  if (iRet == (int)sizeof(*pstInfo))
  {
    return (SZOMB == pstInfo->pbi_status) ? DM_NOT_FOUND : DM_SUCCESS;
  }
  if ((EPERM == errno) || (EACCES == errno))
  {
    return DM_PERM;
  }
  return DM_NOT_FOUND;
}

static void dm__os_mac_cmdline(dm_in int iPid, dm_out char *pcOut, dm_in size_t uCap)
{
  int aiMibArg[2] = {CTL_KERN, KERN_ARGMAX};
  int aiMib[3] = {CTL_KERN, KERN_PROCARGS2, iPid};
  int iArgMax = 0;
  size_t uArgSz = sizeof(iArgMax);
  size_t uLen = 65536u;
  char *pcBuf = NULL;
  int iArgc = 0;
  size_t uPos = 0u;
  size_t uWritten = 0u;

  if ((NULL == pcOut) || (0u == uCap))
  {
    return;
  }
  pcOut[0] = '\0';
  if ((0 == sysctl(aiMibArg, 2, &iArgMax, &uArgSz, NULL, 0)) && (iArgMax > 0))
  {
    uLen = (size_t)iArgMax;
    if (uLen > 262144u)
    {
      uLen = 262144u;
    }
  }
  pcBuf = (char *)dm_malloc_tag(uLen, DM_MEM_TAG_OS);
  if (NULL == pcBuf)
  {
    return;
  }
  if ((0 != sysctl(aiMib, 3, pcBuf, &uLen, NULL, 0)) || (uLen < sizeof(int)))
  {
    dm_free_tag(pcBuf);
    return;
  }
  memcpy(&iArgc, pcBuf, sizeof(iArgc));
  uPos = sizeof(iArgc);
  /* 跳过 exec_path 与后续 NUL 填充 */
  while ((uPos < uLen) && ('\0' != pcBuf[uPos]))
  {
    uPos++;
  }
  while ((uPos < uLen) && ('\0' == pcBuf[uPos]))
  {
    uPos++;
  }
  while ((iArgc > 0) && (uPos < uLen) && (uWritten + 2u < uCap))
  {
    size_t uArgLen = strnlen(pcBuf + uPos, uLen - uPos);
    size_t uCopy = uArgLen;
    if ((uWritten > 0u) && (uWritten + 1u < uCap))
    {
      pcOut[uWritten++] = ' ';
    }
    if (uCopy > (uCap - 1u - uWritten))
    {
      uCopy = uCap - 1u - uWritten;
    }
    memcpy(pcOut + uWritten, pcBuf + uPos, uCopy);
    uWritten += uCopy;
    uPos += uArgLen + 1u;
    iArgc--;
  }
  pcOut[uWritten] = '\0';
  dm_free_tag(pcBuf);
  return;
}

int dm_os_proc_start_ms(dm_in int iPid, dm_out uint64 *pu64StartMs)
{
  struct proc_bsdinfo stInfo = {0};
  int iRc = DM_SUCCESS;

  DM_ASSERT_SLOW();
  if ((iPid <= 0) || (NULL == pu64StartMs))
  {
    return DM_INVALID;
  }
  *pu64StartMs = 0u;
  iRc = dm__os_mac_bsdinfo(iPid, &stInfo);
  if (DM_SUCCESS != iRc)
  {
    return iRc;
  }
  *pu64StartMs =
      ((uint64)stInfo.pbi_start_tvsec * 1000ULL) + ((uint64)stInfo.pbi_start_tvusec / 1000ULL);
  return DM_SUCCESS;
}

int dm_os_proc_descendants(dm_in int iRootPid, dm_out dm_os_proc_st *pastOut, dm_in uint32 u32Cap,
                           dm_out uint32 *pu32Count)
{
  int iBytes = proc_listallpids(NULL, 0);
  int iSlots = 0;
  pid_t *paiPid = NULL;
  dm__os_node_st *pastAll = NULL;
  uint32 *pau32Idx = NULL;
  uint32 u32All = 0u;
  uint32 u32Found = 0u;
  int iGot = 0;
  int iIdx = 0;

  DM_ASSERT_SLOW();
  if ((iRootPid <= 0) || (NULL == pu32Count) || ((u32Cap > 0u) && (NULL == pastOut)))
  {
    return DM_INVALID;
  }
  *pu32Count = 0u;
  if (iBytes <= 0)
  {
    return DM_IO;
  }
  iSlots = iBytes + 64;
  paiPid = (pid_t *)dm_calloc_tag((size_t)iSlots, sizeof(pid_t), DM_MEM_TAG_OS);
  pastAll = (dm__os_node_st *)dm_calloc_tag((size_t)iSlots, sizeof(dm__os_node_st), DM_MEM_TAG_OS);
  if ((NULL == paiPid) || (NULL == pastAll))
  {
    dm_free_tag(paiPid);
    dm_free_tag(pastAll);
    return DM_ALLOC;
  }
  iGot = proc_listallpids(paiPid, iSlots * (int)sizeof(pid_t));
  if (iGot < 0)
  {
    dm_free_tag(paiPid);
    dm_free_tag(pastAll);
    return DM_IO;
  }
  if (iGot > iSlots)
  {
    iGot = iSlots;
  }
  for (iIdx = 0; iIdx < iGot; iIdx++)
  {
    struct proc_bsdinfo stInfo = {0};
    if ((paiPid[iIdx] <= 0) || (DM_SUCCESS != dm__os_mac_bsdinfo((int)paiPid[iIdx], &stInfo)))
    {
      continue;
    }
    pastAll[u32All].iPid = (int)paiPid[iIdx];
    pastAll[u32All].iPpid = (int)stInfo.pbi_ppid;
    pastAll[u32All].u64StartMs =
        ((uint64)stInfo.pbi_start_tvsec * 1000ULL) + ((uint64)stInfo.pbi_start_tvusec / 1000ULL);
    (void)snprintf(pastAll[u32All].acName, sizeof(pastAll[u32All].acName), "%s", stInfo.pbi_comm);
    u32All++;
  }
  dm_free_tag(paiPid);

  if (0u == u32Cap)
  {
    dm_free_tag(pastAll);
    return DM_SUCCESS;
  }
  pau32Idx = (uint32 *)dm_malloc_tag((size_t)u32Cap * sizeof(uint32), DM_MEM_TAG_OS);
  if (NULL == pau32Idx)
  {
    dm_free_tag(pastAll);
    return DM_ALLOC;
  }
  u32Found = dm__os_walk_descendants(pastAll, u32All, iRootPid, pau32Idx, u32Cap);
  for (u32All = 0u; u32All < u32Found; u32All++)
  {
    const dm__os_node_st *pstNode = &pastAll[pau32Idx[u32All]];
    dm_os_proc_st *pstOut = &pastOut[u32All];
    memset(pstOut, 0, sizeof(*pstOut));
    pstOut->iPid = pstNode->iPid;
    pstOut->iPpid = pstNode->iPpid;
    pstOut->u64StartMs = pstNode->u64StartMs;
    (void)snprintf(pstOut->acName, sizeof(pstOut->acName), "%s", pstNode->acName);
    dm__os_mac_cmdline(pstNode->iPid, pstOut->acCmd, sizeof(pstOut->acCmd));
  }
  *pu32Count = u32Found;
  dm_free_tag(pau32Idx);
  dm_free_tag(pastAll);
  return DM_SUCCESS;
}

static int dm__os_mac_kill_one(dm_in int iPid)
{
  if (0 == kill((pid_t)iPid, SIGKILL))
  {
    return DM_SUCCESS;
  }
  return (EPERM == errno) ? DM_PERM : DM_NOT_FOUND;
}

int dm_os_proc_kill_tree(dm_in int iPid)
{
  dm_os_proc_st *pastDesc = NULL;
  uint32 u32Count = 0u;
  int iIdx = 0;
  int iRootRc = DM_SUCCESS;
  int iFirstPerm = DM_SUCCESS;

  DM_ASSERT_SLOW();
  if (iPid <= 0)
  {
    return DM_INVALID;
  }
  pastDesc = (dm_os_proc_st *)dm_calloc_tag(DM_OS_PROC_MAX, sizeof(dm_os_proc_st), DM_MEM_TAG_OS);
  if (NULL == pastDesc)
  {
    return DM_ALLOC;
  }
  {
    int iDescRc = dm_os_proc_descendants(iPid, pastDesc, DM_OS_PROC_MAX, &u32Count);
    if (DM_ALLOC == iDescRc)
    {
      dm_free_tag(pastDesc);
      return DM_ALLOC;
    }
  }
  for (iIdx = (int)u32Count - 1; iIdx >= 0; iIdx--)
  {
    int iRc = dm__os_mac_kill_one(pastDesc[iIdx].iPid);
    if ((DM_PERM == iRc) && (DM_SUCCESS == iFirstPerm))
    {
      iFirstPerm = DM_PERM;
    }
  }
  dm_free_tag(pastDesc);
  iRootRc = dm__os_mac_kill_one(iPid);
  if ((DM_PERM == iRootRc) || (DM_PERM == iFirstPerm))
  {
    return DM_PERM;
  }
  return iRootRc;
}

/* ============================ 文件可执行性与目录链接 ============================ */

bool_t dm_os_is_exec(dm_in const char *pcPathUtf8)
{
  struct stat stSt = {0};
  return ((NULL != pcPathUtf8) && (0 == stat(pcPathUtf8, &stSt)) && S_ISREG(stSt.st_mode) &&
          (0 == access(pcPathUtf8, X_OK)))
             ? DM_TRUE
             : DM_FALSE;
}

int dm_os_dir_link(dm_in const char *pcLinkPathUtf8, dm_in const char *pcTargetPathUtf8,
                   dm_out bool_t *pbUsedCopy)
{
  struct stat stTarget = {0};
  char acAbsTarget[PATH_MAX] = {0};

  DM_ASSERT_SLOW();
  if (NULL != pbUsedCopy)
  {
    *pbUsedCopy = DM_FALSE;
  }
  if ((NULL == pcLinkPathUtf8) || ('\0' == pcLinkPathUtf8[0]) || (NULL == pcTargetPathUtf8) ||
      ('\0' == pcTargetPathUtf8[0]))
  {
    return DM_INVALID;
  }
  if ((0 != stat(pcTargetPathUtf8, &stTarget)) || !S_ISDIR(stTarget.st_mode))
  {
    return DM_NOT_FOUND;
  }
  /* POSIX symlink 按链接所在目录解析相对路径；先规整为绝对路径避免悬空软链 */
  if (NULL == realpath(pcTargetPathUtf8, acAbsTarget))
  {
    return (ENOENT == errno) ? DM_NOT_FOUND : DM_IO;
  }
  if (0 != symlink(acAbsTarget, pcLinkPathUtf8))
  {
    return (EEXIST == errno) ? DM_EXISTS : DM_IO;
  }
  return DM_SUCCESS;
}

static int dm__os_mac_rm_recursive(dm_in const char *pcPath, dm_in uint32 u32Depth)
{
  struct stat stSt = {0};
  DIR *pstDir = NULL;
  struct dirent *pstEnt = NULL;
  char acSub[1024] = {0};

  if (0 != lstat(pcPath, &stSt))
  {
    return DM_NOT_FOUND;
  }
  if (S_ISLNK(stSt.st_mode) || !S_ISDIR(stSt.st_mode))
  {
    return (0 == unlink(pcPath)) ? DM_SUCCESS : DM_IO;
  }
  if (u32Depth > 32u)
  {
    return DM_TOO_BIG;
  }
  pstDir = opendir(pcPath);
  if (NULL != pstDir)
  {
    while (NULL != (pstEnt = readdir(pstDir)))
    {
      int iN = 0;
      if ((0 == strcmp(pstEnt->d_name, ".")) || (0 == strcmp(pstEnt->d_name, "..")))
      {
        continue;
      }
      iN = snprintf(acSub, sizeof(acSub), "%s/%s", pcPath, pstEnt->d_name);
      if ((iN <= 0) || ((size_t)iN >= sizeof(acSub)))
      {
        closedir(pstDir);
        return DM_TOO_BIG;
      }
      (void)dm__os_mac_rm_recursive(acSub, u32Depth + 1u);
    }
    closedir(pstDir);
  }
  return (0 == rmdir(pcPath)) ? DM_SUCCESS : DM_IO;
}

int dm_os_dir_unlink(dm_in const char *pcLinkPathUtf8)
{
  DM_ASSERT_SLOW();
  if ((NULL == pcLinkPathUtf8) || ('\0' == pcLinkPathUtf8[0]))
  {
    return DM_INVALID;
  }
  return dm__os_mac_rm_recursive(pcLinkPathUtf8, 0u);
}

#ifdef __cplusplus
}
#endif

#endif /* __APPLE__ */
