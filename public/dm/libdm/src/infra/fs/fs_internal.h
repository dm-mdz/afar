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

void dm__mkdirs(in const char *pcPath);
int dm__lock_take(in const char *pcDir, in const char *pcLockName);
void dm__lock_release(void);
int dm__fs_replace(in const char *pcFrom, in const char *pcTo);
char *dm__file_read_all(in const char *pcPath, in size_t uMax, out size_t *puLen);
int dm__file_write_atomic(in const char *pcPath, in const char *pcText, in size_t uLen);

#ifdef __cplusplus
}
#endif

#endif /* DM_FS_INTERNAL_H */
