/**
 ******************************************************************************
 * @file    os.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   三端统一通用 OS 原语（DD-01 §4，FW-04）。
 ******************************************************************************
 */
#ifndef DM_OS_H
#define DM_OS_H

#include <dm.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_OS_PROC_NAME_MAX 64u
#define DM_OS_PROC_CMD_MAX 512u
#define DM_OS_PROC_MAX 512u
#define DM_OS_ENV_NAME_MAX 128u
#define DM_OS_ENV_SRC_MAX 64u

typedef struct dm_os_proc
{
  int iPid;
  int iPpid;
  uint64 u64StartMs;
  char acName[DM_OS_PROC_NAME_MAX];
  char acCmd[DM_OS_PROC_CMD_MAX];
} dm_os_proc_st;

typedef struct dm_os_env_conflict
{
  char acName[DM_OS_ENV_NAME_MAX];
  char acSource[DM_OS_ENV_SRC_MAX];
} dm_os_env_conflict_st;

DM_API int dm_os_proc_start_ms(in int iPid, out uint64 *pu64StartMs);
DM_API int dm_os_proc_descendants(in int iRootPid, out dm_os_proc_st *pastOut, in uint32 u32Cap,
                                  out uint32 *pu32Count);
DM_API int dm_os_proc_kill_tree(in int iPid);

DM_API const char *const *dm_os_bin_dirs(void);
DM_API const char *const *dm_os_exe_suffixes(void);
DM_API bool_t dm_os_is_exec(in const char *pcPathUtf8);
DM_API char dm_os_path_list_sep(void);
DM_API char dm_os_dir_sep(void);
DM_API bool_t dm_os_env_nocase(void);

/* 冲突环境变量扫描（AUD-07）：检查给定变量名列表在当前进程环境中是否存在非空定义 */
DM_API int dm_os_env_conflicts(in const char *const *ppcNames, in uint32 u32NameCount,
                               out dm_os_env_conflict_st *pastOut, in uint32 u32Cap,
                               out uint32 *pu32Count);

/* 免管理员权限目录联接（AUD-07）：
 * Windows 优先 NTFS Junction（DeviceIoControl FSCTL_SET_REPARSE_POINT，零 1314 提权错），
 * 非 NTFS 卷失败时自动递归拷贝兜底（*pbUsedCopy = DM_TRUE）；macOS/Linux 使用 symlink。 */
DM_API int dm_os_dir_link(in const char *pcLinkPathUtf8, in const char *pcTargetPathUtf8,
                          out bool_t *pbUsedCopy);
DM_API int dm_os_dir_unlink(in const char *pcLinkPathUtf8);

#ifdef __cplusplus
}
#endif

#endif /* DM_OS_H */
