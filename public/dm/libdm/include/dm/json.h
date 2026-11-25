/**
 ******************************************************************************
 * @file    json.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   JSON（详设 2.16）：直接用 yyjson，DM 只补三样——
 *          ① yyjson 用 DM 的内存分配（内存标签 207）；② 读的时候有大小上限；
 *          ③ 几个常用的取值函数（按 JSON Pointer 路径取字符串、整数，取不到给默认值）。
 *          要改一段 JSON 时：读进来转成可改的文档（yyjson_doc_mut_copy），只改认识的字段，
 *          别的原样留着再写出去（AR-23）。
 ******************************************************************************
 */
#ifndef DM_JSON_H
#define DM_JSON_H

#include <dm.h>
#include <yyjson.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 给 yyjson 的分配器：所有 yyjson_read_opts / yyjson_mut_doc_new / yyjson_write_opts 都传它 */
DM_API const yyjson_alc *dm_json_alc(void);

/* 敏感 JSON 专用分配器（AUD-06）：realloc/free 前对完整数据块执行 dm_secure_zero */
DM_API const yyjson_alc *dm_json_alc_sensitive(void);

/* 读一段 JSON（不改原文）。超过 uMax 字节回 NULL 并在 piErr 里给 DM_TOO_BIG；
 * 格式不对回 NULL、DM_INVALID。用完 yyjson_doc_free */
DM_API yyjson_doc *dm_json_read(dm_in const char *pcText, dm_in size_t uLen, dm_in size_t uMax,
                                dm_out int *piErr);

/* 敏感 JSON 解析（AUD-06）：底层绑定 dm_json_alc_sensitive，调用 yyjson_doc_free 时自动全量擦零 */
DM_API yyjson_doc *dm_json_read_sensitive(dm_in const char *pcText, dm_in size_t uLen,
                                          dm_in size_t uMax, dm_out int *piErr);

/* 按路径取值；路径是 JSON Pointer，例 "/role"、"/push/0"。取不到或类型不对给默认值 */
DM_API const char *dm_json_str(dm_in yyjson_val *pstRoot, dm_in const char *pcPath,
                               dm_in const char *pcDef);
DM_API int64 dm_json_int(dm_in yyjson_val *pstRoot, dm_in const char *pcPath, dm_in int64 i64Def);
DM_API bool_t dm_json_bool(dm_in yyjson_val *pstRoot, dm_in const char *pcPath, dm_in bool_t bDef);

/* 写出来（紧凑格式），返回的字符串用 dm_json_free_str 释放；puLen 带出长度 */
DM_API char *dm_json_write_mut(dm_in const yyjson_mut_doc *pstDoc, dm_out size_t *puLen);
DM_API void dm_json_free_str(dm_in char *pcText);

/* 敏感 JSON 序列化与释放（AUD-06）：走 dm_json_alc_sensitive，realloc/free 自动全量擦零 */
DM_API char *dm_json_write_mut_sensitive(dm_in const yyjson_mut_doc *pstDoc, dm_out size_t *puLen);
DM_API void dm_json_free_str_sensitive(dm_io char *pcText);

#ifdef __cplusplus
}
#endif

#endif /* DM_JSON_H */
