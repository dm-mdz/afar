/**
 ******************************************************************************
 * @file    os_linux.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   Linux 通用 OS 原语实现（DD-01 §4，/proc/<pid>/stat + btime + 无环 BFS）。
 ******************************************************************************
 */
#ifdef __linux__

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
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ============================ 进程查询与进程树终止 ============================ */

static uint64 dm__os_linux_btime_sec(void)
{
  FILE *pstFp = fopen("/proc/stat", "r");
  char acLine[256] = {0};
  uint64 u64Btime = 0u;

  if (NULL == pstFp)
  {
    return 0u;
  }
  while (NULL != fgets(acLine, (int)sizeof(acLine), pstFp))
  {
    if (0 == strncmp(acLine, "btime ", 6u))
    {
      u64Btime = (uint64)strtoull(acLine + 6u, NULL, 10);
      break;
    }
  }
  fclose(pstFp);
  return u64Btime;
}

static int dm__os_linux_read_stat(dm_in int iPid, dm_in uint64 u64BtimeSec, dm_in long lClkTck,
                                  dm_out dm__os_node_st *pstNode)
{
  char acPath[64] = {0};
  char acBuf[1024] = {0};
  FILE *pstFp = NULL;
  size_t uRead = 0u;
  char *pcOpen = NULL;
  char *pcClose = NULL;
  char *pcCur = NULL;
  char cState = '\0';
  int iPpid = 0;
  uint64 u64StartTicks = 0u;
  int iField = 0;

  memset(pstNode, 0, sizeof(*pstNode));
  pstNode->iPid = iPid;
  (void)snprintf(acPath, sizeof(acPath), "/proc/%d/stat", iPid);
  pstFp = fopen(acPath, "r");
  if (NULL == pstFp)
  {
    return ((EACCES == errno) || (EPERM == errno)) ? DM_PERM : DM_NOT_FOUND;
  }
  uRead = fread(acBuf, 1u, sizeof(acBuf) - 1u, pstFp);
  fclose(pstFp);
  acBuf[uRead] = '\0';

  pcOpen = strchr(acBuf, '(');
  pcClose = strrchr(acBuf, ')');
  if ((NULL == pcOpen) || (NULL == pcClose) || (pcClose <= pcOpen))
  {
    return DM_IO;
  }
  {
    size_t uNameLen = (size_t)(pcClose - pcOpen - 1);
    if (uNameLen >= sizeof(pstNode->acName))
    {
      uNameLen = sizeof(pstNode->acName) - 1u;
    }
    memcpy(pstNode->acName, pcOpen + 1, uNameLen);
    pstNode->acName[uNameLen] = '\0';
  }
  /* 字段 3 是 state，字段 4 是 ppid，字段 22 是 starttime (相对 ')' 后第 1..20 个空格分隔字段) */
  pcCur = pcClose + 1;
  for (iField = 3; (iField <= 22) && ('\0' != *pcCur); iField++)
  {
    while (' ' == *pcCur)
    {
      pcCur++;
    }
    if (3 == iField)
    {
      cState = *pcCur;
    }
    else if (4 == iField)
    {
      iPpid = (int)strtol(pcCur, NULL, 10);
    }
    else if (22 == iField)
    {
      u64StartTicks = (uint64)strtoull(pcCur, NULL, 10);
    }
    while (('\0' != *pcCur) && (' ' != *pcCur))
    {
      pcCur++;
    }
  }
  if (iField <= 22)
  {
    return DM_IO;
  }
  if (('Z' == cState) || ('X' == cState))
  {
    return DM_NOT_FOUND;
  }
  pstNode->iPpid = iPpid;
  if (lClkTck > 0)
  {
    pstNode->u64StartMs = (u64BtimeSec * 1000ULL) + ((u64StartTicks * 1000ULL) / (uint64)lClkTck);
  }
  return DM_SUCCESS;
}

static void dm__os_linux_cmdline(dm_in int iPid, dm_out char *pcOut, dm_in size_t uCap)
{
  char acPath[64] = {0};
  FILE *pstFp = NULL;
  size_t uRead = 0u;
  size_t uIdx = 0u;

  if ((NULL == pcOut) || (0u == uCap))
  {
    return;
  }
  pcOut[0] = '\0';
  (void)snprintf(acPath, sizeof(acPath), "/proc/%d/cmdline", iPid);
  pstFp = fopen(acPath, "rb");
  if (NULL == pstFp)
  {
    return;
  }
  uRead = fread(pcOut, 1u, uCap - 1u, pstFp);
  fclose(pstFp);
  while ((uRead > 0u) && ('\0' == pcOut[uRead - 1u]))
  {
    uRead--;
  }
  for (uIdx = 0u; uIdx < uRead; uIdx++)
  {
    if ('\0' == pcOut[uIdx])
    {
      pcOut[uIdx] = ' ';
    }
  }
  pcOut[uRead] = '\0';
  return;
}

int dm_os_proc_start_ms(dm_in int iPid, dm_out uint64 *pu64StartMs)
{
  dm__os_node_st stNode = {0};
  int iRc = DM_SUCCESS;

  DM_ASSERT_SLOW();
  if ((iPid <= 0) || (NULL == pu64StartMs))
  {
    return DM_INVALID;
  }
  *pu64StartMs = 0u;
  iRc = dm__os_linux_read_stat(iPid, dm__os_linux_btime_sec(), sysconf(_SC_CLK_TCK), &stNode);
  if (DM_SUCCESS != iRc)
  {
    return iRc;
  }
  *pu64StartMs = stNode.u64StartMs;
  return DM_SUCCESS;
}

int dm_os_proc_descendants(dm_in int iRootPid, dm_out dm_os_proc_st *pastOut, dm_in uint32 u32Cap,
                           dm_out uint32 *pu32Count)
{
  DIR *pstDir = NULL;
  struct dirent *pstEnt = NULL;
  uint64 u64Btime = 0u;
  long lClkTck = 0;
  dm__os_node_st *pastAll = NULL;
  uint32 *pau32Idx = NULL;
  uint32 u32All = 0u;
  uint32 u32AllCap = 256u;
  uint32 u32Found = 0u;
  uint32 u32Idx = 0u;

  DM_ASSERT_SLOW();
  if ((iRootPid <= 0) || (NULL == pu32Count) || ((u32Cap > 0u) && (NULL == pastOut)))
  {
    return DM_INVALID;
  }
  *pu32Count = 0u;
  pstDir = opendir("/proc");
  if (NULL == pstDir)
  {
    return DM_IO;
  }
  pastAll = (dm__os_node_st *)dm_calloc_tag(u32AllCap, sizeof(dm__os_node_st), DM_MEM_TAG_OS);
  if (NULL == pastAll)
  {
    closedir(pstDir);
    return DM_ALLOC;
  }
  u64Btime = dm__os_linux_btime_sec();
  lClkTck = sysconf(_SC_CLK_TCK);

  while (NULL != (pstEnt = readdir(pstDir)))
  {
    char *pcEnd = NULL;
    long lPid = strtol(pstEnt->d_name, &pcEnd, 10);
    dm__os_node_st stNode = {0};

    if ((lPid <= 0) || (NULL == pcEnd) || ('\0' != *pcEnd))
    {
      continue;
    }
    if (DM_SUCCESS != dm__os_linux_read_stat((int)lPid, u64Btime, lClkTck, &stNode))
    {
      continue;
    }
    if (u32All >= u32AllCap)
    {
      uint32 u32NextCap = u32AllCap * 2u;
      dm__os_node_st *pastGrow =
          (dm__os_node_st *)dm_realloc_tag(pastAll, (size_t)u32NextCap * sizeof(dm__os_node_st));
      if (NULL == pastGrow)
      {
        dm_free_tag(pastAll);
        closedir(pstDir);
        return DM_ALLOC;
      }
      pastAll = pastGrow;
      u32AllCap = u32NextCap;
    }
    pastAll[u32All++] = stNode;
  }
  closedir(pstDir);

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
  for (u32Idx = 0u; u32Idx < u32Found; u32Idx++)
  {
    const dm__os_node_st *pstNode = &pastAll[pau32Idx[u32Idx]];
    dm_os_proc_st *pstOut = &pastOut[u32Idx];
    memset(pstOut, 0, sizeof(*pstOut));
    pstOut->iPid = pstNode->iPid;
    pstOut->iPpid = pstNode->iPpid;
    pstOut->u64StartMs = pstNode->u64StartMs;
    (void)snprintf(pstOut->acName, sizeof(pstOut->acName), "%s", pstNode->acName);
    dm__os_linux_cmdline(pstNode->iPid, pstOut->acCmd, sizeof(pstOut->acCmd));
  }
  *pu32Count = u32Found;
  dm_free_tag(pau32Idx);
  dm_free_tag(pastAll);
  return DM_SUCCESS;
}

static int dm__os_linux_kill_one(dm_in int iPid)
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
    int iRc = dm__os_linux_kill_one(pastDesc[iIdx].iPid);
    if ((DM_PERM == iRc) && (DM_SUCCESS == iFirstPerm))
    {
      iFirstPerm = DM_PERM;
    }
  }
  dm_free_tag(pastDesc);
  iRootRc = dm__os_linux_kill_one(iPid);
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

static int dm__os_linux_rm_recursive(dm_in const char *pcPath, dm_in uint32 u32Depth)
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
      (void)dm__os_linux_rm_recursive(acSub, u32Depth + 1u);
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
  return dm__os_linux_rm_recursive(pcLinkPathUtf8, 0u);
}

#ifdef __cplusplus
}
#endif

#endif /* __linux__ */
