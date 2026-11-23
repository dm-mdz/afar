/**
 ******************************************************************************
 * @file    dm.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   DM 全局公共入口头（适配并隔离底层 libuv）。
 *
 * 应用代码只需包含 <dm.h>、使用 dm_* 名字，从不直接接触 libuv。
 * libuv 作为独立、未修改的内置依赖（public/core/libuv）存在。
 * libuv 适配层完全收敛在内部头文件 src/uv_adapter.h（及 src/runtime/adapter.c）中，
 * 因此升级 libuv 只需要重新运行 tool/gen_dm_adapters.js。
 *
 * dm 自身版本（dm_version / dm_version_string）定义在 dm/version.h，与内置的
 * libuv 版本相互独立；libuv 版本通过 dm_uv_version / dm_uv_version_string 获取。
 ******************************************************************************
 */
#ifndef DM_H
#define DM_H

#include <dm/version.h>
#include <dm/type.h>
#include <dm/errno.h>

#include <stdio.h>  /* DMASSERT 写标准错误 */
#include <stdlib.h> /* DMASSERT 用 abort */
#ifndef _WIN32
#include <sys/types.h> /* pid_t */
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/* NULL:C++ 下为 0(可隐式转任意指针),C 下为 (void*)0,保证两语言均可使用 */
#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void *)0)
#endif
#endif

// Assertion control
typedef enum
{
  DMFW_ASSERT_DISABLED = 0, /* 关闭断言 */
  DMFW_ASSERT_ENABLED = 1   /* 启用断言 */
} DMFW_ASSERT_STATUS;

/* DMFW_ASSERT:1=启用、0=关闭(默认启用,可用 -DDMFW_ASSERT=0 覆盖)。
 * 注意:必须是宏(0/1),不能用枚举常量——预处理器的 #if 不认枚举。 */
#ifndef DMFW_ASSERT
#define DMFW_ASSERT 1
#endif

/*
 * DMASSERT:断言。
 *  - Debug 构建(未定义 NDEBUG,CMake 的 Debug 类型)默认启用;
 *  - Release 构建(CMake 自动定义 NDEBUG)默认禁用,断言代码不编译、不占符号;
 *  - 也可用 -DDMFW_ASSERT=0 强制禁用。
 * do-while 包装可安全用于 if/else。失败时写标准错误而不是日志:马上就 abort,日志可能来不及。
 */
#if defined(NDEBUG) || (DMFW_ASSERT == 0)
#define DMASSERT(cond) ((void)0)
#else
#define DMASSERT(cond)                                                                             \
  do                                                                                               \
  {                                                                                                \
    if (!(cond))                                                                                   \
    {                                                                                              \
      fprintf(stderr, "DM assertion failed: %s, file %s, line %d\n", #cond, __FILE__, __LINE__);   \
      abort();                                                                                     \
    }                                                                                              \
  } while (0)
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ARRAY_END(a) ((a) + ARRAY_SIZE(a))

/* 和 libuv 测试里的定义逐字一样（空白也一样），两处同时被包含时不算重复定义 */
/* clang-format off */
#define container_of(ptr, type, member) \
  ((type *) ((char *) (ptr) - offsetof(type, member)))
/* clang-format on */

/* ===================================================================== */
/* 错误码 —— 与 libuv 相同的划分:数值在 dm/errno.h,清单/枚举/查询在主头  */
/* 文件(本文件)。标准错误码(DM_E2BIG = UV_E2BIG …)复用 libuv。          */
/* ===================================================================== */

/* 成功(与 libuv 一致:0 表示成功) */
#define DM_SUCCESS 0

/*
 * dm 错误码映射宏,参考 libuv 的 UV_ERRNO_MAP。
 * 每行一条 XX(code, "说明");枚举与字符串查询都由它生成,新增错误码只需加一行。
 * 同步修改errno.h 中 DM__xxx 宏定义,保证数值不冲突。
 */
#define DM_ERRNO_MAP(XX)                                                                           \
  XX(FAILD, "failed")                                                                              \
  XX(INVALID, "invalid argument")                                                                  \
  XX(ALLOC, "memory allocation failed")                                                            \
  XX(NOT_FOUND, "not found")                                                                       \
  XX(EXISTS, "already exists")                                                                     \
  XX(BUSY, "resource busy")                                                                        \
  XX(IO, "i/o error")                                                                              \
  XX(TIMEOUT, "operation timed out")                                                               \
  XX(PERM, "permission denied")                                                                    \
  XX(RANGE, "value out of range")                                                                  \
  XX(OVERFLOW, "buffer overflow or truncated data")                                                \
  XX(LINK_ERR_ARG, "link invalid argument")                                                        \
  XX(LINK_ERR_STATE, "link invalid state")                                                         \
  XX(LINK_ERR_NO_BEFORE, "link before not registered")                                             \
  XX(LINK_ERR_NOT_FOUND, "link not found")                                                         \
  XX(LINK_ERR_REREGIST, "link re-registered")                                                      \
  XX(SHUTTING_DOWN, "shutting down")                                                               \
  XX(NOT_READY, "not ready (no handshake yet)")                                                    \
  XX(CONN_GONE, "connection or child process is gone")                                             \
  XX(TOO_BIG, "over the size limit")                                                               \
  XX(WRONG_THREAD, "called on the wrong thread")                                                   \
  XX(LOOP_DETECTED, "dispatch loop detected")

/* 枚举:由 DM_ERRNO_MAP 生成(第二条参数 _ 在枚举中忽略) */
typedef enum
{
#define XX(code, _) DM_##code = DM__##code,
  DM_ERRNO_MAP(XX)
#undef XX
} dm_errno_et;

/*
 * 错误码 -> 说明字符串（统一入口，英文）。先查 DM 自己的表，再查别的模块登记的段，
 * 都不是就当 libuv 的标准错误码（DM_E2BIG = UV_E2BIG …）。打印错误一律用它。
 */
DM_API const char *dm_errno_str(in int iErr);

/*
 * 登记一个模块的错误码段（详设 1.4）：iBase 是段首（负数，比如 -200000），
 * 段是 iBase ~ iBase - uiCount + 1；pfStr 查这一段里的说明，查不到返回 NULL。
 * 只在模块 init 里调（主线程）。段和 DM 自己的、已登记的重叠回 DM_EXISTS；参数不对回 DM_INVALID；
 * 登记满了回 DM_RANGE。
 */
DM_API int dm_errno_register(in int iBase, in uint uiCount, in const char *(*pfStr)(int iErr));

/* ==========================================================================
 * dm —— link 声明位置
 * ========================================================================== */
/* 处理器(注册表条目:按 LINKID 统一索引 before/work/after) */
typedef int (*dm_link_before_cb)(void *pvArg);
typedef void (*dm_link_work_cb)(void *pvArg);
typedef void (*dm_link_after_cb)(void *pvArg, int iStatus);

typedef struct dm_link_handler
{
  uint uiLinkID;
  dm_link_before_cb pfBefore;
  dm_link_work_cb pfWork;
  dm_link_after_cb pfAfter;
  uint8 u8Lane;
} dm_link_handler_st;

/* ==========================================================================
 * dm —— linkID 声明位置
 * 对应位置在link.h,dm_linkid_et
 * DM_LINKID_会自动拼接前缀，使用 DM_LINKID_XXX 直接使用即可
 * ========================================================================== */

#define DM_LINKID_MAP(XX)                                                                          \
  XX(FIRST, "first linkID")                                                                        \
  XX(MAX, "max linkID")

/* ===================================================================== */
/* dm —— 自有 API(不属于 libuv 适配部分)                                 */
/* ===================================================================== */

/* dm 库版本号,打包成整数(major<<16 | minor<<8 | patch)。 */
DM_API unsigned int dm_version(void);

/* dm 库版本字符串,例如 "0.1.0-dev"。 */
DM_API const char *dm_version_string(void);

/* dm 库版本号(与 dm_version 相同的打包整数)。 */
DM_API unsigned int dm_version_hex(void);

/* 入口 dm_main_ex / dm_main、dm_quit 等在 <dm/module.h>（详设 2.2） */

/* ---- 平台无关进程 ID 与环境变量条目（零 <uv.h> 泄露） ---- */
#ifdef _WIN32
typedef int dm_pid_t;
#else
typedef pid_t dm_pid_t;
#endif

typedef struct dm_env_item
{
  char *name;
  char *value;
} dm_env_item_st;

/* 一次性初始化标记（零 <uv.h> 泄露） */
typedef struct dm_once_flag
{
#ifndef __cplusplus
  _Atomic int iState;
#else
  int iState;
#endif
} dm_once_flag_st;

#define DM_ONCE_FLAG_INIT {0}

DM_API void dm_call_once(io dm_once_flag_st *pstFlag, in void (*pfFn)(void));

/* ---- 基础运行期与 OS 辅助原语（由 libdm_core 实现，零 <uv.h> 泄露） ---- */
DM_API unsigned int dm_uv_version(void);
DM_API const char *dm_uv_version_string(void);
DM_API const char *dm_strerror(in int iErr);
DM_API uint64 dm_hrtime(void);
DM_API void dm_sleep(in unsigned int uiMsec);
DM_API dm_pid_t dm_os_getpid(void);
DM_API dm_pid_t dm_os_getppid(void);
DM_API int dm_os_getenv(in const char *pcName, out char *pcBuffer, io size_t *puSize);
DM_API int dm_os_homedir(out char *pcBuffer, io size_t *puSize);
DM_API int dm_os_environ(out dm_env_item_st **ppstEnvItems, out int *piCount);
DM_API void dm_os_free_environ(io dm_env_item_st *pstEnvItems, in int iCount);

typedef void *(*dm_malloc_func)(size_t size);
typedef void *(*dm_realloc_func)(void *ptr, size_t size);
typedef void *(*dm_calloc_func)(size_t count, size_t size);
typedef void (*dm_free_func)(void *ptr);

DM_API int dm_replace_allocator(in dm_malloc_func pfMalloc, in dm_realloc_func pfRealloc,
                                in dm_calloc_func pfCalloc, in dm_free_func pfFree);

/* 内存分配/释放统一走 dm_replace_allocator 登记的实现(默认系统分配器)。
 * 非 libuv 代码请一律使用这些接口,以便自定义分配器生效。 */
DM_API void *dm_malloc(in size_t uSize);
DM_API void *dm_calloc(in size_t uCount, in size_t uSize);
DM_API void *dm_realloc(io void *pvPtr, in size_t uSize);
DM_API void *dm_reallocf(io void *pvPtr, in size_t uSize);
DM_API void dm_free(io void *pvPtr);

/* 防编译器优化消除的安全擦零（AUD-06：敏感帧/密码/令牌释放前必须调用） */
DM_API void dm_secure_zero(out void *pvBuf, in size_t uLen);

/* 二进制口的端口（只听 127.0.0.1，详设 2.3）。dm_set_listen_port 给测试用：设过就用它（0 =
 * 让系统挑）， 优先于数据目录里的 dm.port；要在 dm_main_ex 之前调。dm_get_listen_port
 * 取实际绑上的端口 */
DM_API int dm_get_listen_port(void);
DM_API void dm_set_listen_port(in uint16 u16Port);

/* ==========================================================================
 * 业务内存标签 ID(固定 ID,供 dm_malloc_tag / DM_MEM_ALLOC_TAG 使用)。
 * 说明:
 *   - ID 固定且不重复;轻量标签统计按 ID 用定长数组 + 原子计数,无锁。
 *   - 分段:0~99 libuv 整体;100~199 link 层;200~499 各层预留。
 *   - 新增标签:先在对应预留区补 #define,再在 mem.c 的名称表 g_apcTagName
 *     同步加一行名称。
 * ========================================================================== */

/* ---- libuv 整体(0~99) ---- */
#define DM_MEM_TAG_UV_LOOP 0   /* 事件循环 */
#define DM_MEM_TAG_UV_TIMER 1  /* 定时器 */
#define DM_MEM_TAG_UV_ASYNC 2  /* 跨线程唤醒 */
#define DM_MEM_TAG_UV_TCP 3    /* TCP */
#define DM_MEM_TAG_UV_PIPE 4   /* 管道 */
#define DM_MEM_TAG_UV_FS 5     /* 文件系统 */
#define DM_MEM_TAG_UV_WORK 6   /* 线程池 work */
#define DM_MEM_TAG_UV_DNS 7    /* 域名解析 */
#define DM_MEM_TAG_UV_SIGNAL 8 /* 信号 */
#define DM_MEM_TAG_UV_OTHER 9  /* libuv 其他 / 兜底 */

/* ---- link 层(100~199) ---- */
#define DM_MEM_TAG_LINK_CALLBACK 100 /* 回调函数 */
#define DM_MEM_TAG_LINK_CONN 101     /* 连接 */
#define DM_MEM_TAG_LINK_SESSION 102  /* 会话 */
#define DM_MEM_TAG_LINK_FRAME 103    /* 帧/消息 */
#define DM_MEM_TAG_LINK_TX 104       /* 发送缓冲 */
#define DM_MEM_TAG_LINK_RX 105       /* 接收缓冲 */

/* ---- DM 新加的件(200~299,详设 1.5) ---- */
#define DM_MEM_TAG_TIMER 200 /* 定时器 */
#define DM_MEM_TAG_CHILD 201 /* 子进程 */
#define DM_MEM_TAG_HTTP 202  /* HTTP */
#define DM_MEM_TAG_WS 203    /* WebSocket */
#define DM_MEM_TAG_RPC 204   /* 接口和应答 */
#define DM_MEM_TAG_PUSH 205  /* 推送缓冲 */
#define DM_MEM_TAG_DB 206    /* 数据库 */
#define DM_MEM_TAG_JSON 207  /* JSON */
#define DM_MEM_TAG_LOG 208   /* 日志 */
#define DM_MEM_TAG_QUEUE 209 /* 总队列 */
#define DM_MEM_TAG_EVENT 210 /* 事件订阅 */
#define DM_MEM_TAG_HASH 211  /* 哈希表的桶和节点（dm/hash.h） */
#define DM_MEM_TAG_OS 212    /* 平台 OS 原语（dm/os.h） */
#define DM_MEM_TAG_POOL 213
#define DM_MEM_TAG_SECRET 214
#define DM_MEM_TAG_HTTPC 215

/* ---- 300~399 预留;400~499 给用 DM 的应用各模块(怎么分由应用自己定) ---- */

/* 标签槽总数(定长数组容量,须大于所有已用 ID) */
#define DM_MEM_TAG_COUNT 1024

/* 带业务标签的分配接口(按模块统计泄漏)。注意:tag 分配的块必须用
 * dm_free_tag 释放(计数一致);dm_realloc_tag 保留原块标签。 */
DM_API void *dm_malloc_tag(in size_t uSize, in int iTag);
DM_API void *dm_calloc_tag(in size_t uCount, in size_t uSize, in int iTag);
DM_API void *dm_realloc_tag(io void *pvPtr, in size_t uSize);
DM_API void dm_free_tag(io void *pvPtr);

#ifdef DM_MEM_DEBUG
DM_API void *dm_malloc_tag_dbg(in size_t uSize, in int iTag, in const char *pcFile, in int iLine);
DM_API void *dm_calloc_tag_dbg(in size_t uCount, in size_t uSize, in int iTag,
                               in const char *pcFile, in int iLine);
DM_API void *dm_realloc_tag_dbg(io void *pvPtr, in size_t uSize, in const char *pcFile,
                                in int iLine);
DM_API void dm_free_tag_dbg(io void *pvPtr);

#define dm_malloc_tag(size, tag) dm_malloc_tag_dbg((size), (tag), __FILE__, __LINE__)
#define dm_calloc_tag(count, size, tag)                                                            \
  dm_calloc_tag_dbg((count), (size), (tag), __FILE__, __LINE__)
#define dm_realloc_tag(ptr, size) dm_realloc_tag_dbg((ptr), (size), __FILE__, __LINE__)
#define dm_free_tag(ptr) dm_free_tag_dbg((ptr))
#endif

/* 给一个标签起名字，dmcmd、泄漏报告里显示名字而不是编号。模块 init 里调（详设 1.5） */
DM_API void dm_mem_tag_name(in int iTag, in const char *pcName);

#ifdef __cplusplus
}
#endif

#endif /* DM_H */
