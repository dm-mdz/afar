/**
 ******************************************************************************
 * @file    secret.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   系统密钥库与可注入 Mock 适配（DD-01 §5，FW-05）。
 ******************************************************************************
 */
#ifndef DM_SECRET_H
#define DM_SECRET_H

#include <dm.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct dm_secret_provider
{
  int (*pfPut)(dm_in const char *pcService, dm_in const char *pcAccount,
               dm_in const uint8 *pu8Secret, dm_in uint32 u32Len, dm_in void *pvCtx);
  int (*pfGet)(dm_in const char *pcService, dm_in const char *pcAccount, dm_out uint8 *pu8Buf,
               dm_in uint32 u32Cap, dm_out uint32 *pu32OutLen, dm_in void *pvCtx);
  int (*pfDel)(dm_in const char *pcService, dm_in const char *pcAccount, dm_in void *pvCtx);
  bool_t (*pfExists)(dm_in const char *pcService, dm_in const char *pcAccount, dm_in void *pvCtx);
  void *pvCtx;
} dm_secret_provider_st;

DM_API void dm_secret_set_provider(dm_in const dm_secret_provider_st *pstProvider);

/* 设置本地加密密钥库存储目录（Win DPAPI 密文文件 / Linux 无头加密文件），传 NULL 恢复默认目录 */
DM_API void dm_secret_set_dir(dm_in const char *pcDirUtf8);

DM_API int dm_secret_put(dm_in const char *pcService, dm_in const char *pcAccount,
                         dm_in const uint8 *pu8Secret, dm_in uint32 u32Len);
DM_API int dm_secret_get(dm_in const char *pcService, dm_in const char *pcAccount,
                         dm_out uint8 *pu8Buf, dm_in uint32 u32Cap, dm_out uint32 *pu32OutLen);
DM_API int dm_secret_del(dm_in const char *pcService, dm_in const char *pcAccount);
DM_API bool_t dm_secret_exists(dm_in const char *pcService, dm_in const char *pcAccount);

#ifdef __cplusplus
}
#endif

#endif /* DM_SECRET_H */
