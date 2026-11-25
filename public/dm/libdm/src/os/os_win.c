/**
 ******************************************************************************
 * @file    os_win.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   Windows 通用 OS 原语实现（DD-01 §4，FIX-01/05/06，AUD-07）：
 *          - dm_os_proc_start_ms：精确区分 DM_PERM / DM_NOT_FOUND 与 STILL_ACTIVE；
 *          - dm_os_proc_descendants：单次 Toolhelp 快照 + 无环 BFS；
 *          - dm_os_proc_kill_tree：自底向上强杀整树，透传 DM_PERM；
 *          - dm_os_dir_link / unlink：免管理员权限 NTFS Junction + 非 NTFS 拷贝兜底。
 ******************************************************************************
 */
#ifdef _WIN32

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "os"
#include "os_internal.h"
#include <dm/check.h>
#include <dm/log.h>
#include <dm/mem.h>

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <winioctl.h>
#include <tlhelp32.h>

/* FILETIME (100ns since 1601-01-01) -> UNIX ms (since 1970-01-01) */
#define DM__WIN_EPOCH_DIFF_100NS 116444736000000000ULL

/* MinGW 头文件可能未暴露内核 REPARSE_DATA_BUFFER 结构，本地精确定义 MountPoint 分支 */
typedef struct dm__win_reparse_mount
{
  ULONG ReparseTag;
  USHORT ReparseDataLength;
  USHORT Reserved;
  USHORT SubstituteNameOffset;
  USHORT SubstituteNameLength;
  USHORT PrintNameOffset;
  USHORT PrintNameLength;
  WCHAR PathBuffer[1];
} dm__win_reparse_mount_st;

/* ============================ 编码转换与进程查询 ============================ */

static bool_t dm__os_utf8_to_wide(dm_in const char *pcUtf8, dm_out WCHAR *pwcOut, dm_in int iCap)
{
  if ((NULL == pcUtf8) || ('\0' == pcUtf8[0]) || (NULL == pwcOut) || (iCap <= 1))
  {
    return DM_FALSE;
  }
  return (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pcUtf8, -1, pwcOut, iCap) > 0)
             ? DM_TRUE
             : DM_FALSE;
}

static void dm__os_wide_to_utf8(dm_in const WCHAR *pwcIn, dm_out char *pcOut, dm_in int iCap)
{
  if ((NULL == pcOut) || (iCap <= 0))
  {
    return;
  }
  pcOut[0] = '\0';
  if ((NULL == pwcIn) || (L'\0' == pwcIn[0]))
  {
    return;
  }
  if (0 == WideCharToMultiByte(CP_UTF8, 0, pwcIn, -1, pcOut, iCap, NULL, NULL))
  {
    pcOut[0] = '\0';
  }
  return;
}

static uint64 dm__os_filetime_to_ms(dm_in const FILETIME *pstFt)
{
  uint64 u64Val = ((uint64)pstFt->dwHighDateTime << 32) | (uint64)pstFt->dwLowDateTime;
  if (u64Val < DM__WIN_EPOCH_DIFF_100NS)
  {
    return 0u;
  }
  return (u64Val - DM__WIN_EPOCH_DIFF_100NS) / 10000ULL;
}

static int dm__os_win_proc_info(dm_in int iPid, dm_out uint64 *pu64StartMs, dm_out char *pcCmd,
                                dm_in int iCmdCap)
{
  HANDLE hProc = NULL;
  DWORD dwCode = 0;
  FILETIME stCreate = {0};
  FILETIME stExit = {0};
  FILETIME stKernel = {0};
  FILETIME stUser = {0};

  if (NULL != pu64StartMs)
  {
    *pu64StartMs = 0u;
  }
  if ((NULL != pcCmd) && (iCmdCap > 0))
  {
    pcCmd[0] = '\0';
  }
  if (iPid <= 0)
  {
    return DM_INVALID;
  }
  /* FIX-01 + 259 退出码防误判：优先 SYNCHRONIZE + WaitForSingleObject(0)，无权时退化 STILL_ACTIVE
   */
  hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, (DWORD)iPid);
  if (NULL != hProc)
  {
    if (WAIT_TIMEOUT != WaitForSingleObject(hProc, 0u))
    {
      CloseHandle(hProc);
      return DM_NOT_FOUND;
    }
  }
  else
  {
    DWORD dwErr = GetLastError();
    if (ERROR_ACCESS_DENIED != dwErr)
    {
      return DM_NOT_FOUND;
    }
    hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)iPid);
    if (NULL == hProc)
    {
      return (ERROR_ACCESS_DENIED == GetLastError()) ? DM_PERM : DM_NOT_FOUND;
    }
    if (!GetExitCodeProcess(hProc, &dwCode) || (STILL_ACTIVE != dwCode))
    {
      CloseHandle(hProc);
      return DM_NOT_FOUND;
    }
  }
  if (!GetProcessTimes(hProc, &stCreate, &stExit, &stKernel, &stUser))
  {
    DWORD dwErr = GetLastError();
    CloseHandle(hProc);
    return (ERROR_ACCESS_DENIED == dwErr) ? DM_PERM : DM_IO;
  }
  if (NULL != pu64StartMs)
  {
    *pu64StartMs = dm__os_filetime_to_ms(&stCreate);
  }
  if ((NULL != pcCmd) && (iCmdCap > 1))
  {
    WCHAR awcPath[1024] = {0};
    DWORD dwLen = (DWORD)ARRAY_SIZE(awcPath);
    if (QueryFullProcessImageNameW(hProc, 0, awcPath, &dwLen))
    {
      dm__os_wide_to_utf8(awcPath, pcCmd, iCmdCap);
    }
  }
  CloseHandle(hProc);
  return DM_SUCCESS;
}

static int dm__os_win_resolve_start(dm_in int iPid, dm_out uint64 *pu64StartMs, dm_io void *pvUser)
{
  (void)pvUser;
  return dm__os_win_proc_info(iPid, pu64StartMs, NULL, 0);
}

int dm_os_proc_start_ms(dm_in int iPid, dm_out uint64 *pu64StartMs)
{
  DM_ASSERT_SLOW();
  if ((iPid <= 0) || (NULL == pu64StartMs))
  {
    return DM_INVALID;
  }
  return dm__os_win_proc_info(iPid, pu64StartMs, NULL, 0);
}

int dm_os_proc_descendants(dm_in int iRootPid, dm_out dm_os_proc_st *pastOut, dm_in uint32 u32Cap,
                           dm_out uint32 *pu32Count)
{
  HANDLE hSnap = INVALID_HANDLE_VALUE;
  PROCESSENTRY32W stPe = {0};
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

  /* 单次 Toolhelp 快照（FIX-05/06） */
  hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (INVALID_HANDLE_VALUE == hSnap)
  {
    return DM_IO;
  }
  pastAll = (dm__os_node_st *)dm_calloc_tag(u32AllCap, sizeof(dm__os_node_st), DM_MEM_TAG_OS);
  if (NULL == pastAll)
  {
    CloseHandle(hSnap);
    return DM_ALLOC;
  }

  memset(&stPe, 0, sizeof(stPe));
  stPe.dwSize = sizeof(stPe);
  if (Process32FirstW(hSnap, &stPe))
  {
    do
    {
      if (u32All >= u32AllCap)
      {
        uint32 u32NextCap = u32AllCap * 2u;
        dm__os_node_st *pastGrow =
            (dm__os_node_st *)dm_realloc_tag(pastAll, (size_t)u32NextCap * sizeof(dm__os_node_st));
        if (NULL == pastGrow)
        {
          dm_free_tag(pastAll);
          CloseHandle(hSnap);
          return DM_ALLOC;
        }
        pastAll = pastGrow;
        u32AllCap = u32NextCap;
      }
      memset(&pastAll[u32All], 0, sizeof(pastAll[u32All]));
      pastAll[u32All].iPid = (int)stPe.th32ProcessID;
      pastAll[u32All].iPpid = (int)stPe.th32ParentProcessID;
      dm__os_wide_to_utf8(stPe.szExeFile, pastAll[u32All].acName,
                          (int)sizeof(pastAll[u32All].acName));
      if ((int)stPe.th32ProcessID == iRootPid)
      {
        (void)dm__os_win_proc_info(iRootPid, &pastAll[u32All].u64StartMs, NULL, 0);
      }
      u32All++;
    } while (Process32NextW(hSnap, &stPe));
  }
  CloseHandle(hSnap);

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

  u32Found = dm__os_walk_descendants_ex(pastAll, u32All, iRootPid, pau32Idx, u32Cap,
                                        dm__os_win_resolve_start, NULL);
  for (u32Idx = 0u; u32Idx < u32Found; u32Idx++)
  {
    const dm__os_node_st *pstNode = &pastAll[pau32Idx[u32Idx]];
    dm_os_proc_st *pstOut = &pastOut[u32Idx];
    uint64 u64Fresh = 0u;
    int iInfoRc = DM_SUCCESS;
    memset(pstOut, 0, sizeof(*pstOut));
    pstOut->iPid = pstNode->iPid;
    pstOut->iPpid = pstNode->iPpid;
    pstOut->u64StartMs = pstNode->u64StartMs;
    (void)snprintf(pstOut->acName, sizeof(pstOut->acName), "%s", pstNode->acName);
    iInfoRc =
        dm__os_win_proc_info(pstNode->iPid, &u64Fresh, pstOut->acCmd, (int)sizeof(pstOut->acCmd));
    if (DM_SUCCESS == iInfoRc)
    {
      pstOut->u64StartMs = u64Fresh;
    }
    else if (DM_NOT_FOUND == iInfoRc)
    {
      pstOut->acCmd[0] = '\0';
    }
  }
  *pu32Count = u32Found;

  dm_free_tag(pau32Idx);
  dm_free_tag(pastAll);
  return DM_SUCCESS;
}

static int dm__os_win_kill_one(dm_in int iPid)
{
  HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)iPid);
  BOOL bOk = FALSE;

  if (NULL == hProc)
  {
    DWORD dwErr = GetLastError();
    return (ERROR_ACCESS_DENIED == dwErr) ? DM_PERM : DM_NOT_FOUND;
  }
  bOk = TerminateProcess(hProc, 1u);
  if (!bOk)
  {
    DWORD dwErr = GetLastError();
    CloseHandle(hProc);
    if (ERROR_ACCESS_DENIED == dwErr)
    {
      return DM_PERM;
    }
    return DM_NOT_FOUND;
  }
  CloseHandle(hProc);
  return DM_SUCCESS;
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
  /* 自底向上先杀叶子孙进程再杀父进程，避免中间层瞬间重生 */
  for (iIdx = (int)u32Count - 1; iIdx >= 0; iIdx--)
  {
    int iRc = dm__os_win_kill_one(pastDesc[iIdx].iPid);
    if ((DM_PERM == iRc) && (DM_SUCCESS == iFirstPerm))
    {
      iFirstPerm = DM_PERM;
    }
  }
  dm_free_tag(pastDesc);
  iRootRc = dm__os_win_kill_one(iPid);
  if ((DM_PERM == iRootRc) || (DM_PERM == iFirstPerm))
  {
    return DM_PERM;
  }
  return iRootRc;
}

/* ============================ 文件可执行性与目录联接 ============================ */

bool_t dm_os_is_exec(dm_in const char *pcPathUtf8)
{
  WCHAR awcPath[1024] = {0};
  DWORD dwAttr = 0;

  if (!dm__os_utf8_to_wide(pcPathUtf8, awcPath, (int)ARRAY_SIZE(awcPath)))
  {
    return DM_FALSE;
  }
  dwAttr = GetFileAttributesW(awcPath);
  return ((INVALID_FILE_ATTRIBUTES != dwAttr) && (0u == (dwAttr & FILE_ATTRIBUTE_DIRECTORY)))
             ? DM_TRUE
             : DM_FALSE;
}

static bool_t dm__os_win_is_subpath(dm_in const WCHAR *pwcChild, dm_in const WCHAR *pwcParent)
{
  size_t uParLen = wcslen(pwcParent);
  while ((uParLen > 0u) &&
         ((L'\\' == pwcParent[uParLen - 1u]) || (L'/' == pwcParent[uParLen - 1u])))
  {
    uParLen--;
  }
  if (0u == uParLen)
  {
    return DM_FALSE;
  }
  if (0 != _wcsnicmp(pwcChild, pwcParent, uParLen))
  {
    return DM_FALSE;
  }
  return ((L'\0' == pwcChild[uParLen]) || (L'\\' == pwcChild[uParLen]) ||
          (L'/' == pwcChild[uParLen]))
             ? DM_TRUE
             : DM_FALSE;
}

/* 递归目录拷贝（非 NTFS 卷创建 Junction 失败时的安全兜底） */
static int dm__os_win_copy_dir(dm_in const WCHAR *pwcSrc, dm_in const WCHAR *pwcDst,
                               dm_in uint32 u32Depth)
{
  WIN32_FIND_DATAW stFd = {0};
  HANDLE hFind = INVALID_HANDLE_VALUE;
  WCHAR awcPattern[1024] = {0};
  WCHAR awcSubSrc[1024] = {0};
  WCHAR awcSubDst[1024] = {0};
  int iN = 0;

  if (u32Depth > 32u)
  {
    return DM_TOO_BIG;
  }
  if (!CreateDirectoryW(pwcDst, NULL) && (ERROR_ALREADY_EXISTS != GetLastError()))
  {
    return DM_IO;
  }
  iN = _snwprintf(awcPattern, ARRAY_SIZE(awcPattern), L"%ls\\*", pwcSrc);
  if ((iN <= 0) || ((size_t)iN >= ARRAY_SIZE(awcPattern)))
  {
    return DM_TOO_BIG;
  }
  hFind = FindFirstFileW(awcPattern, &stFd);
  if (INVALID_HANDLE_VALUE == hFind)
  {
    return DM_SUCCESS;
  }
  do
  {
    int iNSrc = 0;
    int iNDst = 0;
    if ((0 == wcscmp(stFd.cFileName, L".")) || (0 == wcscmp(stFd.cFileName, L"..")))
    {
      continue;
    }
    iNSrc = _snwprintf(awcSubSrc, ARRAY_SIZE(awcSubSrc), L"%ls\\%ls", pwcSrc, stFd.cFileName);
    iNDst = _snwprintf(awcSubDst, ARRAY_SIZE(awcSubDst), L"%ls\\%ls", pwcDst, stFd.cFileName);
    if ((iNSrc <= 0) || ((size_t)iNSrc >= ARRAY_SIZE(awcSubSrc)) || (iNDst <= 0) ||
        ((size_t)iNDst >= ARRAY_SIZE(awcSubDst)))
    {
      FindClose(hFind);
      return DM_TOO_BIG;
    }
    if (0u != (stFd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    {
      continue; /* 拷贝兜底不跟随符号/联接防环 */
    }
    if (0u != (stFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
      int iRc = dm__os_win_copy_dir(awcSubSrc, awcSubDst, u32Depth + 1u);
      if (DM_SUCCESS != iRc)
      {
        FindClose(hFind);
        return iRc;
      }
    }
    else if (!CopyFileW(awcSubSrc, awcSubDst, FALSE))
    {
      FindClose(hFind);
      return DM_IO;
    }
  } while (FindNextFileW(hFind, &stFd));
  FindClose(hFind);
  return DM_SUCCESS;
}

static int dm__os_win_rm_dir_recursive(dm_in const WCHAR *pwcDir, dm_in uint32 u32Depth)
{
  DWORD dwAttr = GetFileAttributesW(pwcDir);
  WIN32_FIND_DATAW stFd = {0};
  HANDLE hFind = INVALID_HANDLE_VALUE;
  WCHAR awcPattern[1024] = {0};
  WCHAR awcSub[1024] = {0};
  int iN = 0;

  if (INVALID_FILE_ATTRIBUTES == dwAttr)
  {
    return DM_NOT_FOUND;
  }
  /* 重解析点：目录联接只删联接点本身，文件符号链接用 DeleteFileW，绝不递归目标目录！ */
  if (0u != (dwAttr & FILE_ATTRIBUTE_REPARSE_POINT))
  {
    if (0u != (dwAttr & FILE_ATTRIBUTE_DIRECTORY))
    {
      return RemoveDirectoryW(pwcDir) ? DM_SUCCESS : DM_IO;
    }
    return DeleteFileW(pwcDir) ? DM_SUCCESS : DM_IO;
  }
  if (0u == (dwAttr & FILE_ATTRIBUTE_DIRECTORY))
  {
    return DeleteFileW(pwcDir) ? DM_SUCCESS : DM_IO;
  }
  if (u32Depth > 32u)
  {
    return DM_TOO_BIG;
  }
  iN = _snwprintf(awcPattern, ARRAY_SIZE(awcPattern), L"%ls\\*", pwcDir);
  if ((iN <= 0) || ((size_t)iN >= ARRAY_SIZE(awcPattern)))
  {
    return DM_TOO_BIG;
  }
  hFind = FindFirstFileW(awcPattern, &stFd);
  if (INVALID_HANDLE_VALUE != hFind)
  {
    do
    {
      int iNSub = 0;
      if ((0 == wcscmp(stFd.cFileName, L".")) || (0 == wcscmp(stFd.cFileName, L"..")))
      {
        continue;
      }
      iNSub = _snwprintf(awcSub, ARRAY_SIZE(awcSub), L"%ls\\%ls", pwcDir, stFd.cFileName);
      if ((iNSub <= 0) || ((size_t)iNSub >= ARRAY_SIZE(awcSub)))
      {
        FindClose(hFind);
        return DM_TOO_BIG;
      }
      (void)dm__os_win_rm_dir_recursive(awcSub, u32Depth + 1u);
    } while (FindNextFileW(hFind, &stFd));
    FindClose(hFind);
  }
  return RemoveDirectoryW(pwcDir) ? DM_SUCCESS : DM_IO;
}

int dm_os_dir_link(dm_in const char *pcLinkPathUtf8, dm_in const char *pcTargetPathUtf8,
                   dm_out bool_t *pbUsedCopy)
{
  WCHAR awcLink[1024] = {0};
  WCHAR awcLinkFull[1024] = {0};
  WCHAR awcTargetIn[1024] = {0};
  WCHAR awcTargetFull[1024] = {0};
  WCHAR awcSubName[1040] = {0};
  DWORD dwTargetAttr = 0;
  DWORD dwFullLen = 0;
  DWORD dwLinkFullLen = 0;
  HANDLE hDir = INVALID_HANDLE_VALUE;
  _Alignas(dm__win_reparse_mount_st) uint8 au8Buf[4096] = {0};
  dm__win_reparse_mount_st *pstRep = (dm__win_reparse_mount_st *)au8Buf;
  size_t uSubChars = 0;
  size_t uPrintChars = 0;
  size_t uDataBytes = 0;
  DWORD dwReturned = 0;
  int iNSub = 0;
  int iCopyRc = DM_SUCCESS;

  DM_ASSERT_SLOW();
  if (NULL != pbUsedCopy)
  {
    *pbUsedCopy = DM_FALSE;
  }
  if (!dm__os_utf8_to_wide(pcLinkPathUtf8, awcLink, (int)ARRAY_SIZE(awcLink)) ||
      !dm__os_utf8_to_wide(pcTargetPathUtf8, awcTargetIn, (int)ARRAY_SIZE(awcTargetIn)))
  {
    return DM_INVALID;
  }
  dwFullLen = GetFullPathNameW(awcTargetIn, (DWORD)ARRAY_SIZE(awcTargetFull), awcTargetFull, NULL);
  dwLinkFullLen = GetFullPathNameW(awcLink, (DWORD)ARRAY_SIZE(awcLinkFull), awcLinkFull, NULL);
  if ((0u == dwFullLen) || (dwFullLen >= (DWORD)ARRAY_SIZE(awcTargetFull)) ||
      (0u == dwLinkFullLen) || (dwLinkFullLen >= (DWORD)ARRAY_SIZE(awcLinkFull)))
  {
    return DM_INVALID;
  }
  dwTargetAttr = GetFileAttributesW(awcTargetFull);
  if ((INVALID_FILE_ATTRIBUTES == dwTargetAttr) ||
      (0u == (dwTargetAttr & FILE_ATTRIBUTE_DIRECTORY)))
  {
    return DM_NOT_FOUND;
  }
  if (dm__os_win_is_subpath(awcLinkFull, awcTargetFull))
  {
    return DM_INVALID; /* 禁止链接落在目标目录内部造成自包含递归 */
  }
  if (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(awcLinkFull))
  {
    return DM_EXISTS;
  }
  if (!CreateDirectoryW(awcLinkFull, NULL))
  {
    return DM_IO;
  }

  /* 尝试免管理员权限的 NTFS Mount Point (Junction)，绝不调 CreateSymbolicLinkW (1314) */
  iNSub = _snwprintf(awcSubName, ARRAY_SIZE(awcSubName), L"\\??\\%ls", awcTargetFull);
  if ((iNSub > 0) && ((size_t)iNSub < ARRAY_SIZE(awcSubName)))
  {
    uSubChars = wcslen(awcSubName);
    uPrintChars = wcslen(awcTargetFull);
    uDataBytes = 8u + (uSubChars + 1u + uPrintChars + 1u) * sizeof(WCHAR);
    if ((uDataBytes + 8u) <= sizeof(au8Buf))
    {
      hDir = CreateFileW(awcLinkFull, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
      if (INVALID_HANDLE_VALUE != hDir)
      {
        BOOL bOk = FALSE;
        memset(au8Buf, 0, sizeof(au8Buf));
        pstRep->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
        pstRep->ReparseDataLength = (USHORT)uDataBytes;
        pstRep->SubstituteNameOffset = 0u;
        pstRep->SubstituteNameLength = (USHORT)(uSubChars * sizeof(WCHAR));
        pstRep->PrintNameOffset = (USHORT)((uSubChars + 1u) * sizeof(WCHAR));
        pstRep->PrintNameLength = (USHORT)(uPrintChars * sizeof(WCHAR));
        memcpy(pstRep->PathBuffer, awcSubName, (uSubChars + 1u) * sizeof(WCHAR));
        memcpy(pstRep->PathBuffer + uSubChars + 1u, awcTargetFull,
               (uPrintChars + 1u) * sizeof(WCHAR));
        bOk = DeviceIoControl(hDir, FSCTL_SET_REPARSE_POINT, pstRep, (DWORD)(8u + uDataBytes), NULL,
                              0, &dwReturned, NULL);
        CloseHandle(hDir);
        if (bOk)
        {
          return DM_SUCCESS;
        }
      }
    }
  }

  /* 非 NTFS 等卷不支持 Junction 时自动退化为递归目录拷贝兜底；失败则回滚不留半截目录 */
  if (NULL != pbUsedCopy)
  {
    *pbUsedCopy = DM_TRUE;
  }
  iCopyRc = dm__os_win_copy_dir(awcTargetFull, awcLinkFull, 0u);
  if (DM_SUCCESS != iCopyRc)
  {
    (void)dm__os_win_rm_dir_recursive(awcLinkFull, 0u);
  }
  return iCopyRc;
}

int dm_os_dir_unlink(dm_in const char *pcLinkPathUtf8)
{
  WCHAR awcLink[1024] = {0};

  DM_ASSERT_SLOW();
  if (!dm__os_utf8_to_wide(pcLinkPathUtf8, awcLink, (int)ARRAY_SIZE(awcLink)))
  {
    return DM_INVALID;
  }
  return dm__os_win_rm_dir_recursive(awcLink, 0u);
}

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
