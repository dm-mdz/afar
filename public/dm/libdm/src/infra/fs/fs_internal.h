/**
 ******************************************************************************
 * @file    fs_internal.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   文件系统基础设施内部接口（供 runtime / transport / storage 复用，零向上依赖）。
 ******************************************************************************
 */
#ifndef DM_FS_INTERNAL_H
#define DM_FS_INTERNAL_H

#include <dm.h>

#ifdef __cplusplus
extern "C"
{
#endif

void dm__mkdirs(dm_in const char *pcPath);
int dm__lock_take(dm_in const char *pcDir, dm_in const char *pcLockName);
void dm__lock_release(void);
int dm__fs_replace(dm_in const char *pcFrom, dm_in const char *pcTo);
char *dm__file_read_all(dm_in const char *pcPath, dm_in size_t uMax, dm_out size_t *puLen);
int dm__file_write_atomic(dm_in const char *pcPath, dm_in const char *pcText, dm_in size_t uLen);

#ifdef __cplusplus
}
#endif

#endif /* DM_FS_INTERNAL_H */
