/**
 ******************************************************************************
 * @file    mbedtls_user_config.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   mbedTLS 构建期附加配置（不修改上游 public/core/mbedtls/ 源码）：
 *          - 关闭 MBEDTLS_SELF_TEST，消除 ecp.c 全局 mul_count++ 并发无锁写竞争；
 *          - POSIX 平台开启 MBEDTLS_THREADING_C + MBEDTLS_THREADING_PTHREAD 线程安全层。
 ******************************************************************************
 */
#ifndef BF_MBEDTLS_USER_CONFIG_H
#define BF_MBEDTLS_USER_CONFIG_H

#undef MBEDTLS_SELF_TEST

#ifndef _WIN32
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
#endif

#endif /* BF_MBEDTLS_USER_CONFIG_H */
