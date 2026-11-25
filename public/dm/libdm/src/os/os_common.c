/**
 ******************************************************************************
 * @file    os_common.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   三端共用 OS 原语：无环 BFS 子孙遍历、常见目录、后缀/分隔符与冲突环境变量扫描。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "os_internal.h"
#include <dm/mem.h>

#include <stdio.h>
#include <string.h>

#define DM__OS_DIRS_MAX 8u

static char g_aacOsBinDir[DM__OS_DIRS_MAX][512];
static const char *g_apcOsBinDir[DM__OS_DIRS_MAX + 1u];
static dm_once_flag_st g_stOsBinDirOnce = DM_ONCE_FLAG_INIT;

/* ============================ 常见可执行目录与分隔符 ============================ */

static void dm__os_dir_add(dm_io uint32 *pu32Count, dm_in const char *pcBase,
                           dm_in const char *pcTail)
{
  int iN = 0;
  if ((*pu32Count >= DM__OS_DIRS_MAX) || (NULL == pcBase) || ('\0' == pcBase[0]))
  {
    return;
  }
  iN = snprintf(g_aacOsBinDir[*pu32Count], sizeof(g_aacOsBinDir[0]), "%s%s", pcBase,
                (NULL != pcTail) ? pcTail : "");
  if ((iN <= 0) || ((size_t)iN >= sizeof(g_aacOsBinDir[0])))
  {
    g_aacOsBinDir[*pu32Count][0] = '\0';
    return;
  }
  g_apcOsBinDir[*pu32Count] = g_aacOsBinDir[*pu32Count];
  (*pu32Count)++;
  return;
}

static void dm__os_env_read(dm_in const char *pcName, dm_out char *pcBuf, dm_in size_t uCap)
{
  size_t uSize = uCap;
  if ((NULL == pcBuf) || (0u == uCap))
  {
    return;
  }
  if (DM_SUCCESS != dm_os_getenv(pcName, pcBuf, &uSize))
  {
    pcBuf[0] = '\0';
  }
  return;
}

static void dm__os_bin_dirs_once(void)
{
  char acHome[512] = {0};
  uint32 u32Count = 0;
  size_t uSize = sizeof(acHome);

  if (DM_SUCCESS != dm_os_homedir(acHome, &uSize))
  {
    acHome[0] = '\0';
  }
#if defined(_WIN32)
  {
    char acProg[512] = {0};
    char acApp[512] = {0};
    char acLocalApp[512] = {0};
    dm__os_env_read("ProgramFiles", acProg, sizeof(acProg));
    dm__os_env_read("APPDATA", acApp, sizeof(acApp));
    dm__os_env_read("LOCALAPPDATA", acLocalApp, sizeof(acLocalApp));
    dm__os_dir_add(&u32Count, acProg, "\\nodejs");
    dm__os_dir_add(&u32Count, acProg, "\\Git\\cmd");
    dm__os_dir_add(&u32Count, acApp, "\\npm");
    dm__os_dir_add(&u32Count, acLocalApp, "\\Programs\\nodejs");
    dm__os_dir_add(&u32Count, acHome, "\\.local\\bin");
  }
#else
#if defined(__APPLE__)
  dm__os_dir_add(&u32Count, "/opt/homebrew/bin", NULL);
#endif
  dm__os_dir_add(&u32Count, "/usr/local/bin", NULL);
  dm__os_dir_add(&u32Count, acHome, "/.local/bin");
  (void)dm__os_env_read;
#endif
  g_apcOsBinDir[u32Count] = NULL;
  return;
}

const char *const *dm_os_bin_dirs(void)
{
  dm_call_once(&g_stOsBinDirOnce, dm__os_bin_dirs_once);
  return g_apcOsBinDir;
}

const char *const *dm_os_exe_suffixes(void)
{
#if defined(_WIN32)
  static const char *const s_apcSuf[] = {".exe", ".cmd", ".bat", "", NULL};
#else
  static const char *const s_apcSuf[] = {"", NULL};
#endif
  return s_apcSuf;
}

char dm_os_path_list_sep(void)
{
#if defined(_WIN32)
  return ';';
#else
  return ':';
#endif
}

char dm_os_dir_sep(void)
{
#if defined(_WIN32)
  return '\\';
#else
  return '/';
#endif
}

bool_t dm_os_env_nocase(void)
{
#if defined(_WIN32)
  return DM_TRUE;
#else
  return DM_FALSE;
#endif
}

/* ============================ 无环 BFS 子孙枚举 ============================ */

static bool_t dm__os_pid_emitted(dm_in const dm__os_node_st *pastAll, dm_in const uint32 *pau32Idx,
                                 dm_in uint32 u32Count, dm_in int iPid)
{
  uint32 u32I = 0u;
  for (u32I = 0u; u32I < u32Count; u32I++)
  {
    if (pastAll[pau32Idx[u32I]].iPid == iPid)
    {
      return DM_TRUE;
    }
  }
  return DM_FALSE;
}

/* 无环 BFS 子孙枚举（FIX-05/06）：显式 abVisited[] 防环 + PID 去重 + 启动时间戳防 PID 复用 */
uint32 dm__os_walk_descendants_ex(dm_io dm__os_node_st *pastAll, dm_in uint32 u32All,
                                  dm_in int iRootPid, dm_out uint32 *pau32Idx, dm_in uint32 u32Cap,
                                  dm_in dm__os_start_fn pfResolve, dm_io void *pvUser)
{
  bool_t *pbVisited = NULL;
  uint64 u64RootStart = 0u;
  uint32 u32Count = 0u;
  uint32 u32Head = 0u;
  uint32 u32Idx = 0u;
  bool_t bFoundRoot = DM_FALSE;

  if ((NULL == pastAll) || (0u == u32All) || (iRootPid <= 0) || (NULL == pau32Idx) ||
      (0u == u32Cap))
  {
    return 0u;
  }
  pbVisited = (bool_t *)dm_calloc_tag(u32All, sizeof(bool_t), DM_MEM_TAG_OS);
  if (NULL == pbVisited)
  {
    return 0u;
  }

  for (u32Idx = 0u; u32Idx < u32All; u32Idx++)
  {
    if (pastAll[u32Idx].iPid != iRootPid)
    {
      continue;
    }
    pbVisited[u32Idx] = DM_TRUE;
    if (bFoundRoot)
    {
      continue;
    }
    bFoundRoot = DM_TRUE;
    u64RootStart = pastAll[u32Idx].u64StartMs;
    if ((0u == u64RootStart) && (NULL != pfResolve))
    {
      uint64 u64Resolved = 0u;
      if (DM_SUCCESS == pfResolve(iRootPid, &u64Resolved, pvUser))
      {
        u64RootStart = u64Resolved;
        pastAll[u32Idx].u64StartMs = u64Resolved;
      }
    }
  }
  if (!bFoundRoot)
  {
    dm_free_tag(pbVisited);
    return 0u;
  }

  for (u32Head = 0u; u32Head <= u32Count; u32Head++)
  {
    int iParent = (0u == u32Head) ? iRootPid : pastAll[pau32Idx[u32Head - 1u]].iPid;
    uint64 u64ParentStart =
        (0u == u32Head) ? u64RootStart : pastAll[pau32Idx[u32Head - 1u]].u64StartMs;

    for (u32Idx = 0u; (u32Idx < u32All) && (u32Count < u32Cap); u32Idx++)
    {
      uint64 u64ChildStart = 0u;
      if (pbVisited[u32Idx])
      {
        continue;
      }
      if ((pastAll[u32Idx].iPpid != iParent) || (pastAll[u32Idx].iPid == iParent) ||
          (pastAll[u32Idx].iPid == iRootPid))
      {
        continue;
      }
      if (dm__os_pid_emitted(pastAll, pau32Idx, u32Count, pastAll[u32Idx].iPid))
      {
        pbVisited[u32Idx] = DM_TRUE;
        continue;
      }
      u64ChildStart = pastAll[u32Idx].u64StartMs;
      if ((0u == u64ChildStart) && (NULL != pfResolve))
      {
        int iRc = pfResolve(pastAll[u32Idx].iPid, &u64ChildStart, pvUser);
        if (DM_NOT_FOUND == iRc)
        {
          pbVisited[u32Idx] = DM_TRUE;
          continue;
        }
        if (DM_SUCCESS == iRc)
        {
          pastAll[u32Idx].u64StartMs = u64ChildStart;
        }
      }
      /* 若父子均有有效启动时间，子进程不可早于父进程（允 2ms 调度/粒度抖动） */
      if ((0u != u64ParentStart) && (0u != u64ChildStart) &&
          ((u64ChildStart + 2u) < u64ParentStart))
      {
        continue;
      }
      pbVisited[u32Idx] = DM_TRUE;
      pau32Idx[u32Count++] = u32Idx;
    }
  }

  dm_free_tag(pbVisited);
  return u32Count;
}

uint32 dm__os_walk_descendants(dm_in const dm__os_node_st *pastAll, dm_in uint32 u32All,
                               dm_in int iRootPid, dm_out uint32 *pau32Idx, dm_in uint32 u32Cap)
{
  /* 只读封装：pfResolve == NULL 时不会修改 pastAll */
  return dm__os_walk_descendants_ex((dm__os_node_st *)(uintptr_t)pastAll, u32All, iRootPid,
                                    pau32Idx, u32Cap, NULL, NULL);
}

/* ============================ 冲突环境变量扫描 ============================ */

int dm_os_env_conflicts(dm_in const char *const *ppcNames, dm_in uint32 u32NameCount,
                        dm_out dm_os_env_conflict_st *pastOut, dm_in uint32 u32Cap,
                        dm_out uint32 *pu32Count)
{
  uint32 u32Idx = 0u;
  uint32 u32Found = 0u;
  char acVal[64] = {0};

  if ((NULL == pu32Count) || ((u32NameCount > 0u) && (NULL == ppcNames)) ||
      ((u32Cap > 0u) && (NULL == pastOut)))
  {
    return DM_INVALID;
  }
  *pu32Count = 0u;

  for (u32Idx = 0u; u32Idx < u32NameCount; u32Idx++)
  {
    const char *pcName = ppcNames[u32Idx];
    size_t uSize = sizeof(acVal);
    int iRc = DM_NOT_FOUND;

    if ((NULL == pcName) || ('\0' == pcName[0]))
    {
      continue;
    }
    acVal[0] = '\0';
    iRc = dm_os_getenv(pcName, acVal, &uSize);
    /* DM_SUCCESS（非空）或缓冲不足（uv_os_getenv 将所需长度回写 uSize >
     * sizeof(acVal)）均表示存在冲突 */
    if (((DM_SUCCESS == iRc) && ('\0' != acVal[0])) ||
        ((DM_SUCCESS != iRc) && (uSize > sizeof(acVal))) || (DM_TOO_BIG == iRc))
    {
      if (u32Found < u32Cap)
      {
        memset(&pastOut[u32Found], 0, sizeof(pastOut[u32Found]));
        (void)snprintf(pastOut[u32Found].acName, sizeof(pastOut[u32Found].acName), "%s", pcName);
        (void)snprintf(pastOut[u32Found].acSource, sizeof(pastOut[u32Found].acSource),
                       "process_env");
      }
      u32Found++;
    }
  }
  *pu32Count = (u32Found <= u32Cap) ? u32Found : u32Cap;
  return DM_SUCCESS;
}

#ifdef __cplusplus
}
#endif
