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

DM_API int dm_os_proc_start_ms(dm_in int iPid, dm_out uint64 *pu64StartMs);
DM_API int dm_os_proc_descendants(dm_in int iRootPid, dm_out dm_os_proc_st *pastOut,
                                  dm_in uint32 u32Cap, dm_out uint32 *pu32Count);
DM_API int dm_os_proc_kill_tree(dm_in int iPid);

DM_API const char *const *dm_os_bin_dirs(void);
DM_API const char *const *dm_os_exe_suffixes(void);
DM_API bool_t dm_os_is_exec(dm_in const char *pcPathUtf8);
DM_API char dm_os_path_list_sep(void);
DM_API char dm_os_dir_sep(void);
DM_API bool_t dm_os_env_nocase(void);

/* 冲突环境变量扫描（AUD-07）：检查给定变量名列表在当前进程环境中是否存在非空定义 */
DM_API int dm_os_env_conflicts(dm_in const char *const *ppcNames, dm_in uint32 u32NameCount,
                               dm_out dm_os_env_conflict_st *pastOut, dm_in uint32 u32Cap,
                               dm_out uint32 *pu32Count);

/* 免管理员权限目录联接（AUD-07）：
 * Windows 优先 NTFS Junction（DeviceIoControl FSCTL_SET_REPARSE_POINT，零 1314 提权错），
 * 非 NTFS 卷失败时自动递归拷贝兜底（*pbUsedCopy = DM_TRUE）；macOS/Linux 使用 symlink。 */
DM_API int dm_os_dir_link(dm_in const char *pcLinkPathUtf8, dm_in const char *pcTargetPathUtf8,
                          dm_out bool_t *pbUsedCopy);
DM_API int dm_os_dir_unlink(dm_in const char *pcLinkPathUtf8);

/* ============================ 跨平台 OS 信息与时钟（DM v2.1.0） ============================ */

#define DM_UTSNAME_FIELD_MAX 256u

typedef struct dm_utsname
{
  char acSysname[DM_UTSNAME_FIELD_MAX]; /* 如 "Windows_NT" / "Darwin" / "Linux" */
  char acRelease[DM_UTSNAME_FIELD_MAX]; /* 内核版本 */
  char acVersion[DM_UTSNAME_FIELD_MAX]; /* 构建版本 */
  char acMachine[DM_UTSNAME_FIELD_MAX]; /* 架构如 "x86_64" / "arm64" */
} dm_utsname_st;

typedef enum dm_clock_id
{
  DM_CLOCK_MONOTONIC = 0, /* 单调递增时钟（免疫系统改时，供超时/耗时统计） */
  DM_CLOCK_REALTIME = 1   /* Unix 纪元墙钟（UTC 1970-01-01 起） */
} dm_clock_id_et;

typedef struct dm_timespec
{
  int64 i64Sec;  /* 秒 */
  int32 i32Nsec; /* 纳秒 [0, 999999999] */
} dm_timespec_st;

/* 统一字符串输出缓冲约定：
 *   - pcBuf == NULL || puSize == NULL || *puSize == 0u -> DM_INVALID；
 *   - 成功返回 DM_SUCCESS，pcBuf 以 '\0' 结尾，*puSize 回写实际字符串长度（不含 '\0'）；
 *   - 容量不足（*puSize <= uLen）返回 DM_TOO_BIG，*puSize 回写所需总容量（含 '\0'，即 uLen+1）。 */

/* 获取当前进程可执行文件绝对路径（UTF-8；内部先读完整路径再比对容量，绝不静默截断） */
DM_API int dm_exepath(dm_out char *pcBuf, dm_io size_t *puSize);

/* 获取操作系统临时目录路径（UTF-8，不保证末尾带斜杠） */
DM_API int dm_os_tmpdir(dm_out char *pcBuf, dm_io size_t *puSize);

/* 获取本机主机名（UTF-8） */
DM_API int dm_os_gethostname(dm_out char *pcBuf, dm_io size_t *puSize);

/* 获取操作系统名称、版本与 CPU 架构 */
DM_API int dm_os_uname(dm_out dm_utsname_st *pstOut);

/* 获取/切换当前进程工作目录（UTF-8；dm_chdir(NULL/"") 返回 DM_INVALID） */
DM_API int dm_cwd(dm_out char *pcBuf, dm_io size_t *puSize);
DM_API int dm_chdir(dm_in const char *pcDirUtf8);

/* 设置/删除进程环境变量（UTF-8；非线程安全，建议仅在启动期或受锁保护时修改） */
DM_API int dm_os_setenv(dm_in const char *pcName, dm_in const char *pcValue);
DM_API int dm_os_unsetenv(dm_in const char *pcName);

/* 查询当前进程可用逻辑并行度（受 cgroup/亲和性配额约束，恒 >= 1） */
DM_API uint32 dm_available_parallelism(void);

/* 查询当前进程常驻内存集 RSS（字节） */
DM_API int dm_resident_set_memory(dm_out size_t *puRssBytes);

/* 查询宿主机物理总内存与当前空闲物理内存（字节） */
DM_API uint64 dm_get_total_memory(void);
DM_API uint64 dm_get_free_memory(void);

/* 跨平台高精度时钟（单调时钟 / UTC 墙钟，零系统结构体差异） */
DM_API int dm_clock_gettime(dm_in dm_clock_id_et eClockId, dm_out dm_timespec_st *pstOut);

#ifdef __cplusplus
}
#endif

#endif /* DM_OS_H */
