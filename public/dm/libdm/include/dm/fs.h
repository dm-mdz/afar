/**
 ******************************************************************************
 * @file    fs.h
 * @author  MDM
 * @version 2.1.0
 * @brief   DM 跨平台文件系统原语：原子落盘、目录变更监控、常规文件操作与 UTF-8/UTF-16 转换。
 *          全量路径采用 UTF-8（Windows 内部自动转 UTF-16 宽字符），零底层头泄露。
 ******************************************************************************
 */
#ifndef DM_FS_H
#define DM_FS_H

#include <dm.h>
#include <dm/loop.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ============================ 原子写文件（从 module.h 规范归位） ============================ */

/* 原子写文件（0600，同目录唯一临时文件 + 写满循环 + fsync + 原子替换 + 任意失败清理 .tmp） */
DM_API int dm_fs_write_atomic(dm_in const char *pcPathUtf8, dm_in const void *pvData,
                              dm_in size_t uLen);

/* ============================ 跨平台常规文件与目录操作 ============================ */

typedef struct dm_fs_stat
{
  bool_t bIsFile;    /* 常规文件 */
  bool_t bIsDir;     /* 目录 */
  bool_t bIsSymlink; /* 符号链接（仅 dm_fs_lstat 时可为 DM_TRUE） */
  uint32 u32Mode;    /* POSIX 权限位（如 0644/0755） */
  uint64 u64Size;    /* 字节大小 */
  int64 i64MtimeMs;  /* 修改时间（Unix 纪元毫秒） */
  int64 i64MtimeNs;  /* 修改时间（Unix 纪元纳秒，满足高频增量变更检测精度） */
  int64 i64AtimeMs;  /* 访问时间（Unix 纪元毫秒） */
  int64 i64CtimeMs;  /* 元数据变更时间（Unix 纪元毫秒） */
} dm_fs_stat_st;

typedef enum dm_fs_dirent_type
{
  DM_FS_ENT_UNKNOWN = 0,
  DM_FS_ENT_FILE = 1,
  DM_FS_ENT_DIR = 2,
  DM_FS_ENT_LINK = 3,
  DM_FS_ENT_OTHER = 4
} dm_fs_dirent_type_et;

typedef struct dm_fs_dirent
{
  char *pcName; /* 条目名（UTF-8，不含父目录路径，不含 "." 与 ".."） */
  dm_fs_dirent_type_et eType;
} dm_fs_dirent_st;

/* 查询文件/目录元数据：dm_fs_stat 跟随符号链接，dm_fs_lstat 不跟随。
 * 不存在回 DM_NOT_FOUND，无权限回 DM_PERM。 */
DM_API int dm_fs_stat(dm_in const char *pcPathUtf8, dm_out dm_fs_stat_st *pstOut);
DM_API int dm_fs_lstat(dm_in const char *pcPathUtf8, dm_out dm_fs_stat_st *pstOut);

/* 规范化绝对路径（解析 ..、符号链接与 Windows NTFS 联接/UNC 前缀）。
 * Windows 统一剥离 \\?\ 前缀、将 \\?\UNC\srv\share 与 \\srv\share 规范为 //srv/share，
 * 并将全部反斜杠 '\' 归一为 '/'（三端统一 UTF-8 正斜杠绝对路径）；严格遵循统一缓冲契约。 */
DM_API int dm_fs_realpath(dm_in const char *pcPathUtf8, dm_out char *pcBuf, dm_io size_t *puSize);

/* 创建硬链接（pcDstUtf8 指向 pcSrcUtf8）。
 * 错误码严格区分：
 *   - 跨卷/跨文件系统：返回 DM_EXDEV；
 *   - 目标文件系统不支持硬链接（如 FAT32/exFAT）或超链接上限：返回 DM_UNSUPPORTED；
 *   - 源不存在：DM_NOT_FOUND；目标已存在：DM_EXISTS；无权限：DM_PERM。 */
DM_API int dm_fs_link(dm_in const char *pcSrcUtf8, dm_in const char *pcDstUtf8);

/* 创建单级目录（u32Mode 在 POSIX 生效，0 默认 0755；已存在返回 DM_EXISTS） */
DM_API int dm_fs_mkdir(dm_in const char *pcDirUtf8, dm_in uint32 u32Mode);

/* 递归创建多级目录（已存在且为目录返回 DM_SUCCESS；路径分量是常规文件返回 DM_NOT_DIR/DM_IO；
 * Windows 自动跳过盘符根 X:/、X:\ 与 UNC 共享前缀 //host/share/、\\host\share\ 并容忍尾部斜杠） */
DM_API int dm_fs_mkdirs(dm_in const char *pcDirUtf8, dm_in uint32 u32Mode);

/* 删除文件或空目录；不存在回 DM_NOT_FOUND */
DM_API int dm_fs_unlink(dm_in const char *pcPathUtf8);
DM_API int dm_fs_rmdir(dm_in const char *pcDirUtf8);

/* 重命名/替换（目标文件存在时原子覆盖；Windows 内置短退避重试防杀软/索引句柄短暂占用） */
DM_API int dm_fs_rename(dm_in const char *pcFromUtf8, dm_in const char *pcToUtf8);

/* 枚举目录子项（过滤 "."/".."；内存按 DM_MEM_TAG_FS 分配，须配对 dm_fs_scandir_free 释放） */
DM_API int dm_fs_scandir(dm_in const char *pcDirUtf8, dm_out dm_fs_dirent_st **ppastOut,
                         dm_out uint32 *pu32Count);
DM_API void dm_fs_scandir_free(dm_io dm_fs_dirent_st *pastList, dm_in uint32 u32Count);

/* ============================ 文件/目录变更监控（dm_fs_event） ============================ */

typedef struct dm_fs_event dm_fs_event_st;

#define DM_FS_EVENT_F_NONE 0u
/* 递归监控子目录：
 *   - Windows (ReadDirectoryChangesW) 与 macOS (FSEvents)：原生支持；
 *   - Linux (inotify)：入口显式拦截并返回 DM_UNSUPPORTED（绝不静默忽略，AUD-V21-P0-06）。
 *   - 传入未知标志位统一返回 DM_INVALID。 */
#define DM_FS_EVENT_F_RECURSIVE (1u << 0)

#define DM_FS_EV_RENAME (1u << 0) /* 创建、删除或重命名 */
#define DM_FS_EV_CHANGE (1u << 1) /* 内容或属性变更 */

/* 监控事件回调（在 pstLoop 所属线程触发；pcFilenameUtf8 可能为 NULL，仅在回调期间有效） */
typedef void (*dm_fs_event_cb)(dm_io dm_fs_event_st *pstEv, dm_in int iErr,
                               dm_in const char *pcFilenameUtf8, dm_in uint32 u32Events,
                               dm_io void *pvUser);

DM_API int dm_fs_event_create(dm_io dm_loop_st *pstLoop, dm_out dm_fs_event_st **ppstOut);
DM_API int dm_fs_event_start(dm_io dm_fs_event_st *pstEv, dm_in const char *pcPathUtf8,
                             dm_in uint32 u32Flags, dm_in dm_fs_event_cb pfCb, dm_in void *pvUser);
DM_API int dm_fs_event_stop(dm_io dm_fs_event_st *pstEv);
/* 严格遵循统一字符串缓冲契约（*puSize == 0 -> DM_INVALID，不足 -> DM_TOO_BIG 并回写 uLen+1） */
DM_API int dm_fs_event_getpath(dm_in const dm_fs_event_st *pstEv, dm_out char *pcBuf,
                               dm_io size_t *puSize);
DM_API int dm_fs_event_close(dm_io dm_fs_event_st *pstEv, dm_in dm_close_fn pfOnClose,
                             dm_in void *pvUser);

/* ============================ UTF-8(WTF-8) <-> UTF-16 转换 ============================ */
/* 统一口径（AUD-V21-P0-08 / P1-09）：
 *   - 两个长度查询函数均不含结尾 NUL（0x00 / 0x0000）；
 *   - 两个调用方缓冲转换函数均要求 *puOut > 0（0 -> DM_INVALID），成功时回写不含 NUL 的有效单元数，
 *     缓冲不足返回 DM_TOO_BIG 并回写含 NUL 的所需总容量（len + 1u）。 */

/* 计算 UTF-16 序列转 UTF-8 所需字节数（不含 '\0'；pu16Src == NULL 返回 0；i64SrcUnits < 0 表示以
 * 0x0000 结尾） */
DM_API size_t dm_utf16_len_as_utf8(dm_in const uint16 *pu16Src, dm_in int64 i64SrcUnits);

/* 转换至调用方缓冲（*puOutBytes 入参为容量含 '\0'，0 -> DM_INVALID；成功回写不含 '\0' 的字节数；
 * 不足回 DM_TOO_BIG 并回写含 '\0' 所需容量） */
DM_API int dm_utf16_to_utf8(dm_in const uint16 *pu16Src, dm_in int64 i64SrcUnits,
                            dm_out char *pcOut, dm_io size_t *puOutBytes);

/* 转换并按 DM_MEM_TAG_FS 堆分配 NUL 结尾 UTF-8 字符串（调用方用 dm_free_tag 释放；*puOutBytes 不含
 * '\0'） */
DM_API int dm_utf16_to_utf8_alloc(dm_in const uint16 *pu16Src, dm_in int64 i64SrcUnits,
                                  dm_out char **ppcOut, dm_out size_t *puOutBytes);

/* 计算 UTF-8(WTF-8) NUL 结尾字符串转 UTF-16 所需 uint16 单元数（不含末尾 0x0000；
 * 空指针或非法 UTF-8 字节序列返回 DM_INVALID (-100001)） */
DM_API int64 dm_utf8_len_as_utf16(dm_in const char *pcSrcUtf8);

/* 转换至调用方 uint16 缓冲（*puOutUnits 入参为容量含 0x0000，0 -> DM_INVALID；
 * 成功回写不含 0x0000 的有效单元数；不足回 DM_TOO_BIG 并回写含 0x0000 所需总单元数） */
DM_API int dm_utf8_to_utf16(dm_in const char *pcSrcUtf8, dm_out uint16 *pu16Out,
                            dm_io size_t *puOutUnits);

/* 转换并按 DM_MEM_TAG_FS 堆分配 0x0000 结尾 UTF-16 串（调用方用 dm_free_tag 释放；*puOutUnits 不含
 * 0x0000） */
DM_API int dm_utf8_to_utf16_alloc(dm_in const char *pcSrcUtf8, dm_out uint16 **ppu16Out,
                                  dm_out size_t *puOutUnits);

#ifdef __cplusplus
}
#endif

#endif /* DM_FS_H */
