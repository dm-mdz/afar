/**
 ******************************************************************************
 * @file    thread.h
 * @author  MDM
 * @version 2.1.0
 * @brief   DM 跨平台线程与同步原语（不透明堆句柄，零底层头泄露）。
 *          所有句柄均经 DM_MEM_TAG_THREAD 分配并在 DM_MEM_DEBUG 下追踪泄漏。
 ******************************************************************************
 */
#ifndef DM_THREAD_H
#define DM_THREAD_H

#include <dm.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ============================ 线程生命周期与属性 ============================ */

/* 不透明线程句柄：由 dm_thread_create[_ex] 分配，由 dm_thread_join 或 dm_thread_detach 消费释放 */
typedef struct dm_thread dm_thread_st;

/* 跨平台不透明线程 ID（值类型，零堆分配，可用于任意线程含主线程；
 * Windows 存 GetCurrentThreadId()，POSIX 存 pthread_t） */
typedef struct dm_thread_id
{
  uint64 au64Opaque[2];
} dm_thread_id_st;

typedef enum dm_thread_priority
{
  DM_THREAD_PRIO_LOWEST = 0,
  DM_THREAD_PRIO_BELOW_NORMAL = 1,
  DM_THREAD_PRIO_NORMAL = 2,
  DM_THREAD_PRIO_ABOVE_NORMAL = 3,
  DM_THREAD_PRIO_HIGHEST = 4
} dm_thread_priority_et;

typedef struct dm_thread_opt
{
  size_t uStackSize;               /* 栈大小（字节）；0 = OS 默认 */
  dm_thread_priority_et ePriority; /* 目标优先级（bSetPriority=DM_TRUE 时生效） */
  bool_t bSetPriority; /* 是否设置优先级（非特权 Linux 遇 DM_PERM 记 WARN 后继续启动） */
  const char *pcName;  /* 线程调试名（UTF-8，可 NULL；Linux 最长保留 15 字节） */
} dm_thread_opt_st;

typedef void (*dm_thread_entry_fn)(dm_io void *pvArg);

/* 创建可汇合线程（内部直接委托 dm_thread_create_ex(NULL, ...)，共享启动就绪栅栏，
 * 确保返回时 dm_thread_id() 已就绪；dm_out 句柄指针统一置末位）。 */
DM_API int dm_thread_create(dm_in dm_thread_entry_fn pfEntry, dm_io void *pvArg,
                            dm_out dm_thread_st **ppstThread);

/* 带选项创建线程（栈大小、优先级、线程名均在线程入口前自动应用，解决 macOS/Linux
 * 仅限本线程命名限制； pstOpt == NULL 等价于全零默认选项；ePriority 超 [LOWEST..HIGHEST] 且
 * bSetPriority=DM_TRUE 返回 DM_INVALID）。 */
DM_API int dm_thread_create_ex(dm_in const dm_thread_opt_st *pstOpt,
                               dm_in dm_thread_entry_fn pfEntry, dm_io void *pvArg,
                               dm_out dm_thread_st **ppstThread);

/* 阻塞等待目标线程退出并释放 pstThread 句柄（所有权消费，不可重复 join/detach）。
 * 自 join 返回 DM_INVALID（且不释放句柄，仍须由其他线程 join 或 detach）；任意非目标线程可调。 */
DM_API int dm_thread_join(dm_io dm_thread_st *pstThread);

/* 分离目标线程并移交句柄释放权（线程退出后由内核自动 dm_free_tag，零竞态零泄漏）。
 * 调用后调用方不得再访问 pstThread。 */
DM_API int dm_thread_detach(dm_io dm_thread_st *pstThread);

/* 获取当前线程 ID（值返回，零堆分配，Windows 走 GetCurrentThreadId 绝不泄漏 HANDLE） */
DM_API dm_thread_id_st dm_thread_self(void);

/* 获取有效（未 join/detach）线程句柄对应的线程 ID（空指针返回全零 ID） */
DM_API dm_thread_id_st dm_thread_id(dm_in const dm_thread_st *pstThread);

/* 比较两个线程 ID 是否指向同一线程 */
DM_API bool_t dm_thread_equal(dm_in dm_thread_id_st stA, dm_in dm_thread_id_st stB);

/* 设置当前线程名（UTF-8；空指针返回 DM_INVALID；超长自动安全截断：Linux 15B，macOS 63B，Win
 * UTF-16） */
DM_API int dm_thread_setname(dm_in const char *pcName);

/* 查询线程名：pstThread == NULL 表示查询当前线程；pstThread 已退出但尚未 join 返回 DM_NOT_FOUND。
 * 严格遵循统一字符串缓冲契约（*puSize == 0 -> DM_INVALID；缓冲不足 -> DM_TOO_BIG 并回写 uLen+1）。
 */
DM_API int dm_thread_getname(dm_in const dm_thread_st *pstThread, dm_out char *pcBuf,
                             dm_io size_t *puSize);

/* 设置/读取线程优先级：pstThread == NULL 表示当前线程；pstThread 已退出但未 join 返回
 * DM_NOT_FOUND。 入口严格校验 ePriority ∈ [DM_THREAD_PRIO_LOWEST..DM_THREAD_PRIO_HIGHEST]（否则
 * DM_INVALID）。 注：Linux 非特权进程调高优先级（nice < 0）受 CAP_SYS_NICE 限制返回 DM_PERM。 */
DM_API int dm_thread_setpriority(dm_in const dm_thread_st *pstThread,
                                 dm_in dm_thread_priority_et ePriority);
DM_API int dm_thread_getpriority(dm_in const dm_thread_st *pstThread,
                                 dm_out dm_thread_priority_et *pePriority);

/* ============================ 同步原语（不透明堆句柄） ============================ */

typedef struct dm_mutex dm_mutex_st;
typedef struct dm_rwlock dm_rwlock_st;
typedef struct dm_cond dm_cond_st;
typedef struct dm_sem dm_sem_st;
typedef struct dm_key dm_key_st;

/* ---- 互斥锁（默认非递归；递归锁由 dm_mutex_create_recursive 创建） ---- */
DM_API int dm_mutex_create(dm_out dm_mutex_st **ppstOut);
DM_API int dm_mutex_create_recursive(dm_out dm_mutex_st **ppstOut);
DM_API void dm_mutex_destroy(dm_io dm_mutex_st *pstMutex);
DM_API void dm_mutex_lock(dm_io dm_mutex_st *pstMutex);
/* 成功持锁回 DM_SUCCESS；锁已被占用回 DM_AGAIN（非阻塞）；空指针回 DM_INVALID */
DM_API int dm_mutex_trylock(dm_io dm_mutex_st *pstMutex);
DM_API void dm_mutex_unlock(dm_io dm_mutex_st *pstMutex);

/* ---- 读写锁 ---- */
DM_API int dm_rwlock_create(dm_out dm_rwlock_st **ppstOut);
DM_API void dm_rwlock_destroy(dm_io dm_rwlock_st *pstLock);
DM_API void dm_rwlock_rdlock(dm_io dm_rwlock_st *pstLock);
DM_API int dm_rwlock_tryrdlock(dm_io dm_rwlock_st *pstLock); /* 已被写锁占用回 DM_AGAIN */
DM_API void dm_rwlock_rdunlock(dm_io dm_rwlock_st *pstLock);
DM_API void dm_rwlock_wrlock(dm_io dm_rwlock_st *pstLock);
DM_API int dm_rwlock_trywrlock(dm_io dm_rwlock_st *pstLock); /* 已被占用回 DM_AGAIN */
DM_API void dm_rwlock_wrunlock(dm_io dm_rwlock_st *pstLock);

/* ---- 条件变量（必须配对非递归 dm_mutex_st 使用） ---- */
DM_API int dm_cond_create(dm_out dm_cond_st **ppstOut);
DM_API void dm_cond_destroy(dm_io dm_cond_st *pstCond);
DM_API void dm_cond_signal(dm_io dm_cond_st *pstCond);
DM_API void dm_cond_broadcast(dm_io dm_cond_st *pstCond);
DM_API void dm_cond_wait(dm_io dm_cond_st *pstCond, dm_io dm_mutex_st *pstMutex);
/* 相对单调时钟超时等待（单位：纳秒，基于单调时钟免疫系统校时跳变）：
 *   - 内部自动按安全分片上限 DM__COND_CHUNK_NS (~24.8天) 循环等待，三端均绝不溢出或 abort()；
 *   - Windows 下 1..999999 ns 向上取整为 1 ms（0 ns 仍为立即超时试探）；
 *   - 唤醒回 DM_SUCCESS（仍须在 while 谓词循环中防虚假唤醒）；超时回 DM_TIMEOUT；空指针回
 * DM_INVALID。 */
DM_API int dm_cond_timedwait(dm_io dm_cond_st *pstCond, dm_io dm_mutex_st *pstMutex,
                             dm_in uint64 u64TimeoutNs);
/* 毫秒便捷封装（u64TimeoutMs > UINT64_MAX/1000000 饱和至 UINT64_MAX ns 并走安全分片等待） */
DM_API int dm_cond_timedwait_ms(dm_io dm_cond_st *pstCond, dm_io dm_mutex_st *pstMutex,
                                dm_in uint64 u64TimeoutMs);

/* ---- 计数信号量 ---- */
DM_API int dm_sem_create(dm_in uint32 u32InitValue, dm_out dm_sem_st **ppstOut);
DM_API void dm_sem_destroy(dm_io dm_sem_st *pstSem);
DM_API void dm_sem_post(dm_io dm_sem_st *pstSem);
DM_API void dm_sem_wait(dm_io dm_sem_st *pstSem);
DM_API int dm_sem_trywait(dm_io dm_sem_st *pstSem); /* 计数为 0 时非阻塞返回 DM_AGAIN */

/* ---- 线程局部存储（TLS） ---- */
DM_API int dm_key_create(dm_out dm_key_st **ppstOut);
DM_API void dm_key_destroy(dm_io dm_key_st *pstKey);
DM_API void *dm_key_get(dm_in const dm_key_st *pstKey);
DM_API void dm_key_set(dm_io dm_key_st *pstKey, dm_in void *pvValue);

#ifdef __cplusplus
}
#endif

#endif /* DM_THREAD_H */
