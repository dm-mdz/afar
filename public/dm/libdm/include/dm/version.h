/**
 ******************************************************************************
 * @file    version.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   DM 库版本号定义（独立于内置 libuv 版本）。
 *
 * libuv 版本通过 dm_uv_version / dm_uv_version_string 单独获取。
 ******************************************************************************
 */
#ifndef DM_VERSION_H
#define DM_VERSION_H

#define DM_VERSION_MAJOR 0 /* 主版本 */
#define DM_VERSION_MINOR 1 /* 次版本 */
#define DM_VERSION_PATCH 0 /* 修订版本 */

/* 发布版设为 1;设为 0 时,版本字符串会追加 DM_VERSION_SUFFIX。 */
#define DM_VERSION_IS_RELEASE 0
#define DM_VERSION_SUFFIX "dev"

#define DM_VERSION_HEX ((DM_VERSION_MAJOR << 16) | (DM_VERSION_MINOR << 8) | (DM_VERSION_PATCH))

#endif /* DM_VERSION_H */
