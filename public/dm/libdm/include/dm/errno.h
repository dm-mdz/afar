/**
 ******************************************************************************
 * @file    errno.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   DM 错误码数值定义（与底层 libuv 区间严格隔离）。
 *
 * 本文件只定义 DM__xxx 数值宏；错误码清单（DM_ERRNO_MAP）、枚举 dm_errno_et 在 dm.h，
 * 查说明 dm_errno_str、登记别的模块的段 dm_errno_register 在 dm.c。
 *
 * libuv 当前错误码范围：POSIX 约 [-133,-1]（系统 errno 的负值），
 * Windows 约 [-4096,-3000]（硬编码）。dm 预留 [-115999,-100000] 区间，
 * 应用模块由 DM_MOD_ERR 从 -200000 起步，与 libuv 未来扩展绝不冲突。
 ******************************************************************************
 */
#ifndef DM_ERRNO_H
#define DM_ERRNO_H

#include <errno.h> /* 系统 errno（与 libuv 相同的集成方式） */

#define DM_ERRNO_BASE (-100000)

#define DM__FAILD (DM_ERRNO_BASE)                   /* 一般失败 */
#define DM__INVALID (DM_ERRNO_BASE - 1)             /* 参数无效 */
#define DM__ALLOC (DM_ERRNO_BASE - 2)               /* 内存分配失败 */
#define DM__NOT_FOUND (DM_ERRNO_BASE - 3)           /* 未找到 */
#define DM__EXISTS (DM_ERRNO_BASE - 4)              /* 已存在 */
#define DM__BUSY (DM_ERRNO_BASE - 5)                /* 资源忙碌 */
#define DM__IO (DM_ERRNO_BASE - 6)                  /* IO 错误 */
#define DM__TIMEOUT (DM_ERRNO_BASE - 7)             /* 超时 */
#define DM__PERM (DM_ERRNO_BASE - 8)                /* 权限不足 */
#define DM__RANGE (DM_ERRNO_BASE - 9)               /* 数值越界 */
#define DM__OVERFLOW (DM_ERRNO_BASE - 10)           /* 缓冲区溢出或数据不足 */
#define DM__LINK_ERR_ARG (DM_ERRNO_BASE - 11)       /* 参数无效 */
#define DM__LINK_ERR_STATE (DM_ERRNO_BASE - 12)     /* 状态非法 */
#define DM__LINK_ERR_NO_BEFORE (DM_ERRNO_BASE - 13) /* before 未注册 */
#define DM__LINK_ERR_NOT_FOUND (DM_ERRNO_BASE - 14) /* 查表未命中 */
#define DM__LINK_ERR_REREGIST (DM_ERRNO_BASE - 15)  /* 重复注册 */
#define DM__SHUTTING_DOWN                                                                          \
  (DM_ERRNO_BASE - 16) /* 正在退出（不叫 DM_SHUTDOWN：那是 libuv 的请求类型 UV_SHUTDOWN） */
#define DM__NOT_READY (DM_ERRNO_BASE - 17)     /* 没握手 */
#define DM__CONN_GONE (DM_ERRNO_BASE - 18)     /* 连接或子进程已经没了 */
#define DM__TOO_BIG (DM_ERRNO_BASE - 19)       /* 超过上限 */
#define DM__WRONG_THREAD (DM_ERRNO_BASE - 20)  /* 在不该调的线程上调了 */
#define DM__LOOP_DETECTED (DM_ERRNO_BASE - 21) /* 派发跳数超限（死循环熔断） */

/* DM 本体占 -100000 ~ -100999；别的模块用 dm_errno_register 登记自己的段（详设 1.4） */
#define DM_ERRNO_SPAN 1000

#endif /* DM_ERRNO_H */
