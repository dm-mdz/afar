/**
 ******************************************************************************
 * @file    mem.h
 * @brief   dm 内存调试/统计分配器。
 *
 * 分两档,用注释清晰分割:
 *   1) 轻量标签统计(常驻):dm_mem_tag_* —— 每块记业务标签 + 大小,
 *      按标签统计存活字节/块数,泄漏时先定位到"哪个模块"。开销小(~慢 2 倍)。
 *   2) 全量内存定位(仅 `python build.py mem`,即定义 DM_MEM_DEBUG):
 *      dm_mem_debug_* —— backtrace 调用栈 + 越界哨兵 + 毒药,精确到文件:行。
 *      未定义 DM_MEM_DEBUG 时,全量函数不参与编译。
 *
 * 业务代码统一用 DM_MEM_* 宏,按 DM_MEM_DEBUG 自动选档:
 *   - 默认(未定义 DM_MEM_DEBUG):DM_MEM_ALLOC → dm_mem_tag_alloc(轻量)
 *   - mem 编译(定义 DM_MEM_DEBUG):DM_MEM_ALLOC → dm_mem_debug_alloc(全量)
 *
 * 用法:
 *   1. 常驻轻量:直接 DM_MEM_ALLOC / DM_MEM_ALLOC_TAG(sz,"net") / DM_MEM_FREE;
 *      退出前 dm_mem_tag_dump() 看按模块的泄漏统计。
 *   2. 全量定位:python build.py mem 编译后同样用 DM_MEM_* 宏,再看 dm_mem_debug_dump()。
 ******************************************************************************
 */
#ifndef DM_MEM_H
#define DM_MEM_H

#include <stddef.h>
#include <dm.h> /* in/out 宏与定宽类型;先于本头使用(避免 uv.h 的 in/out 字段被宏干扰) */

#ifdef __cplusplus
extern "C"
{
#endif

/* ==========================================================================
 * 第 1 档:轻量标签统计(常驻编译)
 * 每块数据前带一个块头 {标签 ID, 大小}(64 位下共 16 字节);
 * 按标签 ID 用定长数组 + 原子计数,无锁,泄漏时定位到业务模块。
 * ========================================================================== */

/* 初始化标签统计表(无锁版无需初始化,保留以兼容旧调用) */
void dm_mem_tag_init(void);

/* 带标签分配/释放(标签用 dm.h 的 DM_MEM_TAG_* 固定 ID;free 从块头读回 ID 自动扣减) */
void *dm_mem_tag_alloc(in size_t uSize, in int iTag);
void *dm_mem_tag_calloc(in size_t uCount, in size_t uSize, in int iTag);
void *dm_mem_tag_realloc(io void *pvPtr, in size_t uSize);
void dm_mem_tag_free(io void *pvPtr);

/* 打印按标签分组的存活统计(泄漏报表:哪个模块还占着多少) */
void dm_mem_tag_dump(void);

/* 当前存活块数 */
size_t dm_mem_tag_count(void);

/* 查询单个标签当前存活块数与字节数（debug 与 DM_MEM_DEBUG 均有效，供 T-MEM-01 快照比对） */
DM_API void dm_mem_tag_stat(in int iTag, out uint64 *pu64Blocks, out uint64 *pu64Bytes);

/* ==========================================================================
 * 第 2 档:全量内存定位(仅 DM_MEM_DEBUG 编译;无 mem 参数时不参与编译)
 * backtrace 调用栈 + 越界哨兵 + 毒药填充 + 记录表,精确到文件:行。
 * ========================================================================== */
#ifdef DM_MEM_DEBUG

typedef void (*dm_mem_free_hook_fn)(in const void *pvPtr, in size_t uSize);

void dm_mem_debug_init(void);
void *dm_mem_debug_alloc(in size_t uSize, in const char *pcFile, in int iLine);
void *dm_mem_debug_calloc(in size_t uCount, in size_t uSize, in const char *pcFile, in int iLine);
void *dm_mem_debug_calloc_tag(in size_t uCount, in size_t uSize, in int iTag, in const char *pcFile,
                              in int iLine);
void *dm_mem_debug_realloc(io void *pvPtr, in size_t uSize, in const char *pcFile, in int iLine);
void dm_mem_debug_free(io void *pvPtr);
void *dm_mem_debug_alloc_tag(in size_t uSize, in int iTag, in const char *pcFile, in int iLine);
void dm_mem_debug_dump(void);
size_t dm_mem_debug_count(void);
/* 释放前钩子（T-DM-11 等验证敏感缓冲释放前已擦零）与护栏越界命中计数（T-DM-06） */
DM_API void dm_mem_debug_set_free_hook(in dm_mem_free_hook_fn pfHook);
DM_API uint64 dm_mem_debug_guard_hits(void);
DM_API void dm_mem_debug_reset_guard_hits(void);

#endif /* DM_MEM_DEBUG */

/* ==========================================================================
 * 业务侧宏:按 DM_MEM_DEBUG 自动选档(带 __FILE__/__LINE__ 的是全量档)
 * ========================================================================== */
#ifdef DM_MEM_DEBUG
#define DM_MEM_ALLOC(sz) dm_mem_debug_alloc((sz), __FILE__, __LINE__)
#define DM_MEM_ALLOC_TAG(sz, tag) dm_mem_debug_alloc_tag((sz), (tag), __FILE__, __LINE__)
#define DM_MEM_CALLOC(n, s) dm_mem_debug_calloc((n), (s), __FILE__, __LINE__)
#define DM_MEM_REALLOC(p, s) dm_mem_debug_realloc((p), (s), __FILE__, __LINE__)
#define DM_MEM_FREE(p) dm_mem_debug_free(p)
#else
#define DM_MEM_ALLOC(sz) dm_mem_tag_alloc((sz), DM_MEM_TAG_UV_OTHER)
#define DM_MEM_ALLOC_TAG(sz, tag) dm_mem_tag_alloc((sz), (tag))
#define DM_MEM_CALLOC(n, s) dm_mem_tag_calloc((n), (s), DM_MEM_TAG_UV_OTHER)
#define DM_MEM_REALLOC(p, s) dm_mem_tag_realloc((p), (s))
#define DM_MEM_FREE(p) dm_mem_tag_free(p)
#endif

#ifdef __cplusplus
}
#endif

#endif /* DM_MEM_H */
