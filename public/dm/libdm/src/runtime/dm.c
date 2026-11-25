/**
 ******************************************************************************
 * @file    dm.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   DM 框架自有核心 API 实现（DD-01）：版本、分配器包装、dm_call_once、
 *          环境变量拷贝、错误码注册与查表、带标签内存分配桥接。
 *          注：uv_* 适配层主体在自动生成的 adapter.c 中，不在此维护。
 ******************************************************************************
 */

#define DM_LOG_MOD "dm"
#include <dm.h>
#include <dm/link.h>
#include <dm/mem.h>
#include <stdatomic.h>
#include <string.h>
#include "../dm_internal.h"
#include <dm/log.h>

#include <stdarg.h> /* dm_loop_configure 用 va_list */
#include <stdio.h>
#include <stdlib.h> /* dm_replace_allocator 用 NULL */

typedef struct
{
  dm_malloc_func local_malloc;
  dm_realloc_func local_realloc;
  dm_calloc_func local_calloc;
  dm_free_func local_free;
} dm__allocator_st;

static dm__allocator_st dm__allocator = {
    malloc,
    realloc,
    calloc,
    free,
};

/* 按版本宏拼出版本字符串,如 "0.1.0-dev"。 */
static void dm__version_build(char *buf, size_t len)
{
  if (DM_VERSION_IS_RELEASE)
  {
    snprintf(buf, len, "%u.%u.%u", DM_VERSION_MAJOR, DM_VERSION_MINOR, DM_VERSION_PATCH);
  }
  else
  {
    snprintf(buf, len, "%u.%u.%u-%s", DM_VERSION_MAJOR, DM_VERSION_MINOR, DM_VERSION_PATCH,
             DM_VERSION_SUFFIX);
  }
  return;
}

unsigned int dm_version(void)
{
  return DM_VERSION_HEX;
}

unsigned int dm_version_hex(void)
{
  return DM_VERSION_HEX;
}

const char *dm_version_string(void)
{
  static char s_acBuf[64] = {0};
  dm__version_build(s_acBuf, sizeof(s_acBuf));
  return s_acBuf;
}

/* ---- 保留的 11 个既有公共 dm.h 基础函数（其中 dm_os_environ/dm_os_free_environ 在下方） ---- */

unsigned int dm_uv_version(void)
{
  return uv_version();
}

const char *dm_uv_version_string(void)
{
  return uv_version_string();
}

const char *dm_strerror(dm_in int iErr)
{
  static _Thread_local char s_acBuf[96] = {0};
  return uv_strerror_r(iErr, s_acBuf, sizeof(s_acBuf));
}

uint64 dm_hrtime(void)
{
  return uv_hrtime();
}

void dm_sleep(dm_in unsigned int uiMsec)
{
  uv_sleep(uiMsec);
  return;
}

dm_pid_t dm_os_getpid(void)
{
  return uv_os_getpid();
}

dm_pid_t dm_os_getppid(void)
{
#ifdef _WIN32
  /* 确保 libuv 初始化 pNtQueryInformationProcess（W1 Win 修复，不可遗漏） */
  (void)uv_hrtime();
#endif
  return uv_os_getppid();
}

int dm_os_getenv(dm_in const char *pcName, dm_out char *pcBuffer, dm_io size_t *puSize)
{
  return uv_os_getenv(pcName, pcBuffer, puSize);
}

int dm_os_homedir(dm_out char *pcBuffer, dm_io size_t *puSize)
{
  return uv_os_homedir(pcBuffer, puSize);
}

/* ---- 统一 libuv 错误码 -> dm_errno_et 映射（DD-DM-2.1 §1.4，含 P1-01/02） ---- */
int dm__uv_err_map(dm_in int iUvErr)
{
  if (0 == iUvErr)
  {
    return DM_SUCCESS;
  }
  switch (iUvErr)
  {
  case UV_EINVAL:
  case UV_EBADF:
  case UV_EAFNOSUPPORT:
  case UV_EPROTOTYPE:
  case UV_EDESTADDRREQ:
    return DM_INVALID;
  case UV_ENOMEM:
  case UV_EAI_MEMORY:
    return DM_ALLOC;
  case UV_ENOENT:
  case UV_ESRCH:
  case UV_ENXIO:
    return DM_NOT_FOUND;
  case UV_EEXIST:
    return DM_EXISTS;
  case UV_EBUSY:
  case UV_ETXTBSY:
    return DM_BUSY;
  case UV_EAGAIN:
#if defined(UV_EWOULDBLOCK) && (UV_EWOULDBLOCK != UV_EAGAIN)
  case UV_EWOULDBLOCK:
#endif
    return DM_AGAIN;
  case UV_ETIMEDOUT:
    return DM_TIMEOUT;
  case UV_EACCES:
  case UV_EPERM:
  case UV_EROFS:
    return DM_PERM;
  case UV_ERANGE:
    return DM_RANGE;
  case UV_ENOBUFS:
  case UV_ENOSPC:
  case UV_E2BIG:
  case UV_EMSGSIZE:
  case UV_ENAMETOOLONG:
  case UV_EOVERFLOW:
    return DM_TOO_BIG;
  case UV_ENOSYS:
  case UV_ENOTSUP:
  case UV_EPROTONOSUPPORT:
  case UV_ESOCKTNOSUPPORT:
  case UV_EMLINK:
    return DM_UNSUPPORTED;
  case UV_EXDEV:
    return DM_EXDEV;
  case UV_EOF:
    return DM_EOF;
  case UV_ECANCELED:
  case UV_EAI_CANCELED:
    return DM_CANCELED;
  case UV_EADDRINUSE:
    return DM_ADDR_IN_USE;
  case UV_ECONNREFUSED:
    return DM_CONN_REFUSED;
  case UV_ECONNRESET:
  case UV_ECONNABORTED:
  case UV_EPIPE:
  case UV_ESHUTDOWN:
  case UV_ENOTCONN:
    return DM_CONN_GONE;
  case UV_EAI_ADDRFAMILY:
  case UV_EAI_AGAIN:
  case UV_EAI_BADFLAGS:
  case UV_EAI_BADHINTS:
  case UV_EAI_FAIL:
  case UV_EAI_FAMILY:
  case UV_EAI_NODATA:
  case UV_EAI_NONAME:
  case UV_EAI_OVERFLOW:
  case UV_EAI_PROTOCOL:
  case UV_EAI_SERVICE:
  case UV_EAI_SOCKTYPE:
    return DM_DNS_FAIL;
  case UV_ENOTDIR:
    return DM_NOT_DIR;
  case UV_EISDIR:
    return DM_IS_DIR;
  default:
    return DM_IO;
  }
}

/*
 * dm_loop_configure —— 手工适配 libuv 唯一的变参函数 uv_loop_configure。
 * C 无法前向 `...`,故读取可变参数后按选项分发:
 *   DM_LOOP_BLOCK_SIGNAL        —— 读 1 个 int(信号号)
 *   DM_METRICS_IDLE_TIME        —— 无变参
 *   DM_LOOP_USE_IO_URING_SQPOLL —— 无变参(Linux)
 */
int dm_uv_loop_configure(dm_uv_loop_st *loop, dm_uv_loop_option_et option, ...)
{
  va_list ap;
  int signal = 0;

  va_start(ap, option);
  if (DM_UV_LOOP_BLOCK_SIGNAL == option)
  {
    signal = va_arg(ap, int);
  }
  va_end(ap);

  if (DM_UV_LOOP_BLOCK_SIGNAL == option)
  {
    return uv_loop_configure(loop, option, signal);
  }
  return uv_loop_configure(loop, option);
}

/* 下面 5 个照 libuv 的 uv__malloc 一族写的：大小为 0 不分配；释放不改 errno */
void *dm_malloc(size_t uSize)
{
  if (0u == uSize)
  {
    return NULL;
  }
  return dm__allocator.local_malloc(uSize);
}

void dm_free(void *pvPtr)
{
  int iSavedErrno = 0;

  /* libuv 假定 free() 不改 errno：系统的分配器守这个约定，换上的分配器不一定 */
  iSavedErrno = errno;
  dm__allocator.local_free(pvPtr);
  errno = iSavedErrno;
  return;
}

void *dm_calloc(size_t uCount, size_t uSize)
{
  return dm__allocator.local_calloc(uCount, uSize);
}

void *dm_realloc(void *pvPtr, size_t uSize)
{
  if (uSize > 0u)
  {
    return dm__allocator.local_realloc(pvPtr, uSize);
  }
  dm_free(pvPtr);
  return NULL;
}

/* 和 dm_realloc 一样，只是失败时把原来那块也放掉 */
void *dm_reallocf(void *pvPtr, size_t uSize)
{
  void *pvNew = dm_realloc(pvPtr, uSize);

  if ((NULL == pvNew) && (uSize > 0u))
  {
    dm_free(pvPtr);
  }
  return pvNew;
}

int dm_replace_allocator(dm_malloc_func malloc_func, dm_realloc_func realloc_func,
                         dm_calloc_func calloc_func, dm_free_func free_func)
{
  if ((NULL == malloc_func) || (NULL == realloc_func) || (NULL == calloc_func) ||
      (NULL == free_func))
  {
    return DM_INVALID;
  }

  dm__allocator.local_malloc = malloc_func;
  dm__allocator.local_realloc = realloc_func;
  dm__allocator.local_calloc = calloc_func;
  dm__allocator.local_free = free_func;

  /* 同一套分配器也交给 libuv,整个进程的堆分配口径一致 */
  return uv_replace_allocator(malloc_func, realloc_func, calloc_func, free_func);
}

/* ---- 一次性初始化（零 <uv.h> 泄露，标准 C11 _Atomic int，无严格别名 UB） ---- */
void dm_call_once(dm_io dm_once_flag_st *pstFlag, dm_in void (*pfFn)(void))
{
  int iExpected = 0;

  if ((NULL == pstFlag) || (NULL == pfFn))
  {
    return;
  }
  if (2 == atomic_load_explicit(&pstFlag->iState, memory_order_acquire))
  {
    return;
  }
  iExpected = 0;
  if (atomic_compare_exchange_strong_explicit(&pstFlag->iState, &iExpected, 1, memory_order_acq_rel,
                                              memory_order_acquire))
  {
    pfFn();
    atomic_store_explicit(&pstFlag->iState, 2, memory_order_release);
    return;
  }
  while (2 != atomic_load_explicit(&pstFlag->iState, memory_order_acquire))
  {
    dm_sleep(1u); /* 避免 Windows Sleep(0) 优先级反转活锁与 POSIX 空转 */
  }
  return;
}

/* ---- 环境变量枚举（零严格别名 UB：经本地 uv_env_item_t 拷贝进单块 dm_malloc_tag 内存） ---- */
int dm_os_environ(dm_out dm_env_item_st **ppstEnvItems, dm_out int *piCount)
{
  uv_env_item_t *pstUv = NULL;
  int iCount = 0;
  int iErr = 0;
  int iIdx = 0;
  size_t uStrBytes = 0;
  size_t uArrBytes = 0;
  uint8 *pu8Buf = NULL;
  dm_env_item_st *pstOut = NULL;
  char *pcCursor = NULL;

  if ((NULL == ppstEnvItems) || (NULL == piCount))
  {
    return DM_INVALID;
  }
  *ppstEnvItems = NULL;
  *piCount = 0;
  iErr = uv_os_environ(&pstUv, &iCount);
  if (0 != iErr)
  {
    return iErr;
  }
  if (iCount <= 0)
  {
    uv_os_free_environ(pstUv, iCount);
    return DM_SUCCESS;
  }
  uArrBytes = (size_t)iCount * sizeof(dm_env_item_st);
  for (iIdx = 0; iIdx < iCount; iIdx++)
  {
    size_t uNameLen = (NULL != pstUv[iIdx].name) ? (strlen(pstUv[iIdx].name) + 1u) : 1u;
    size_t uValLen = (NULL != pstUv[iIdx].value) ? (strlen(pstUv[iIdx].value) + 1u) : 1u;
    if ((uNameLen > SIZE_MAX - uStrBytes) || (uValLen > SIZE_MAX - uStrBytes - uNameLen))
    {
      uv_os_free_environ(pstUv, iCount);
      return DM_ALLOC;
    }
    uStrBytes += uNameLen + uValLen;
  }
  if (uStrBytes > SIZE_MAX - uArrBytes)
  {
    uv_os_free_environ(pstUv, iCount);
    return DM_ALLOC;
  }
  pu8Buf = (uint8 *)dm_malloc_tag(uArrBytes + uStrBytes, DM_MEM_TAG_UV_OTHER);
  if (NULL == pu8Buf)
  {
    uv_os_free_environ(pstUv, iCount);
    return DM_ALLOC;
  }
  pstOut = (dm_env_item_st *)pu8Buf;
  pcCursor = (char *)(pu8Buf + uArrBytes);
  for (iIdx = 0; iIdx < iCount; iIdx++)
  {
    const char *pcName = (NULL != pstUv[iIdx].name) ? pstUv[iIdx].name : "";
    const char *pcVal = (NULL != pstUv[iIdx].value) ? pstUv[iIdx].value : "";
    size_t uNameLen = strlen(pcName) + 1u;
    size_t uValLen = strlen(pcVal) + 1u;
    pstOut[iIdx].name = pcCursor;
    memcpy(pcCursor, pcName, uNameLen);
    pcCursor += uNameLen;
    pstOut[iIdx].value = pcCursor;
    memcpy(pcCursor, pcVal, uValLen);
    pcCursor += uValLen;
  }
  uv_os_free_environ(pstUv, iCount);
  *ppstEnvItems = pstOut;
  *piCount = iCount;
  return DM_SUCCESS;
}

void dm_os_free_environ(dm_io dm_env_item_st *pstEnvItems, dm_in int iCount)
{
  (void)iCount;
  dm_free_tag(pstEnvItems);
  return;
}

/* ---- 错误码说明与跨表查重（线程安全：受 g_stErrnoLock 保护，消除 TSan 竞态） ---- */
#define DM__ERRNO_REG_MAX 32u
#define DM__MOD_ERR_REG_MAX 96u
#define DM__MOD_ERR_SPAN 1000u

typedef struct dm__errno_reg
{
  int iBase;                      /* 段首(负数) */
  uint uiCount;                   /* 段里几个 */
  const char *(*pfStr)(int iErr); /* 查说明,查不到返回 NULL */
} dm__errno_reg_st;

typedef struct dm__mod_err_reg
{
  uint16 u16ModID;
  const dm_err_entry_st *pastMap;
} dm__mod_err_reg_st;

static dm_once_flag_st g_stErrnoLockOnce = DM_ONCE_FLAG_INIT;
static dm_uv_mutex_t g_stErrnoLock;
static dm__errno_reg_st g_astErrnoReg[DM__ERRNO_REG_MAX];
static uint g_uiErrnoRegCount = 0;
static dm__mod_err_reg_st g_astModErrReg[DM__MOD_ERR_REG_MAX];
static uint g_uiModErrRegCount = 0;

static void dm__errno_lock_init(void)
{
  (void)dm_uv_mutex_init(&g_stErrnoLock);
  return;
}

static void dm__errno_lock(void)
{
  dm_call_once(&g_stErrnoLockOnce, dm__errno_lock_init);
  dm_uv_mutex_lock(&g_stErrnoLock);
  return;
}

static void dm__errno_unlock(void)
{
  dm_uv_mutex_unlock(&g_stErrnoLock);
  return;
}

/* [iBaseA - uiCountA + 1, iBaseA] 和 [iBaseB - uiCountB + 1, iBaseB] 有没有交集 */
static int dm__errno_overlap(dm_in int iBaseA, dm_in uint uiCountA, dm_in int iBaseB,
                             dm_in uint uiCountB)
{
  long long llLowA = (long long)iBaseA - (long long)uiCountA + 1;
  long long llLowB = (long long)iBaseB - (long long)uiCountB + 1;
  return (llLowA <= (long long)iBaseB) && (llLowB <= (long long)iBaseA);
}

int dm_errno_register(dm_in int iBase, dm_in uint uiCount, dm_in const char *(*pfStr)(int iErr))
{
  uint uiIdx = 0;

  if ((iBase >= 0) || (0u == uiCount) || (NULL == pfStr))
  {
    return DM_INVALID;
  }
  if (dm__errno_overlap(iBase, uiCount, DM_ERRNO_BASE, DM_ERRNO_SPAN))
  {
    return DM_EXISTS; /* DM 自己的段 */
  }
  dm__errno_lock();
  for (uiIdx = 0; uiIdx < g_uiErrnoRegCount; uiIdx++)
  {
    if (dm__errno_overlap(iBase, uiCount, g_astErrnoReg[uiIdx].iBase, g_astErrnoReg[uiIdx].uiCount))
    {
      dm__errno_unlock();
      return DM_EXISTS;
    }
  }
  for (uiIdx = 0; uiIdx < g_uiModErrRegCount; uiIdx++)
  {
    int iModBase = DM_MOD_ERR(g_astModErrReg[uiIdx].u16ModID, 0);
    if (dm__errno_overlap(iBase, uiCount, iModBase, DM__MOD_ERR_SPAN))
    {
      dm__errno_unlock();
      return DM_EXISTS;
    }
  }
  if (g_uiErrnoRegCount >= DM__ERRNO_REG_MAX)
  {
    dm__errno_unlock();
    return DM_RANGE;
  }
  g_astErrnoReg[g_uiErrnoRegCount].iBase = iBase;
  g_astErrnoReg[g_uiErrnoRegCount].uiCount = uiCount;
  g_astErrnoReg[g_uiErrnoRegCount].pfStr = pfStr;
  g_uiErrnoRegCount++;
  dm__errno_unlock();
  return DM_SUCCESS;
}

int dm__errno_mod_register(dm_in uint16 u16ModID, dm_in const dm_err_entry_st *pastErrMap)
{
  int iHi = 0;
  int iLo = 0;
  uint uiIdx = 0;
  uint uiJ = 0;

  if ((u16ModID < DM_MOD_ID_APP_MIN) || (u16ModID > DM_MOD_ID_APP_MAX))
  {
    return DM_RANGE;
  }
  if (NULL == pastErrMap)
  {
    return DM_SUCCESS;
  }
  iHi = DM_MOD_ERR(u16ModID, 0);
  iLo = DM_MOD_ERR(u16ModID, 999);
  for (uiIdx = 0; (0 != pastErrMap[uiIdx].iCode) || (NULL != pastErrMap[uiIdx].pcMsg); uiIdx++)
  {
    int iCode = pastErrMap[uiIdx].iCode;
    if ((0 == iCode) || (NULL == pastErrMap[uiIdx].pcMsg))
    {
      return DM_INVALID;
    }
    if ((iCode > iHi) || (iCode < iLo))
    {
      return DM_RANGE;
    }
    for (uiJ = 0; uiJ < uiIdx; uiJ++)
    {
      if (pastErrMap[uiJ].iCode == iCode)
      {
        return DM_EXISTS;
      }
    }
  }
  dm__errno_lock();
  for (uiIdx = 0; uiIdx < g_uiModErrRegCount; uiIdx++)
  {
    if (g_astModErrReg[uiIdx].u16ModID == u16ModID)
    {
      dm__errno_unlock();
      return DM_EXISTS;
    }
  }
  for (uiIdx = 0; uiIdx < g_uiErrnoRegCount; uiIdx++)
  {
    if (dm__errno_overlap(iHi, DM__MOD_ERR_SPAN, g_astErrnoReg[uiIdx].iBase,
                          g_astErrnoReg[uiIdx].uiCount))
    {
      dm__errno_unlock();
      return DM_EXISTS;
    }
  }
  if (g_uiModErrRegCount >= DM__MOD_ERR_REG_MAX)
  {
    dm__errno_unlock();
    return DM_RANGE;
  }
  g_astModErrReg[g_uiModErrRegCount].u16ModID = u16ModID;
  g_astModErrReg[g_uiModErrRegCount].pastMap = pastErrMap;
  g_uiModErrRegCount++;
  dm__errno_unlock();
  return DM_SUCCESS;
}

void dm__errno_mod_clear(void)
{
  dm__errno_lock();
  g_uiModErrRegCount = 0;
  dm__errno_unlock();
  return;
}

const char *dm_errno_str(int err)
{
  uint uiIdx = 0;
  const char *pcStr = NULL;
  const char *(*pfStr)(int iErr) = NULL;

  switch (err)
  {
  case DM_SUCCESS:
    return "success";
#define XX(code, msg)                                                                              \
  case DM_##code:                                                                                  \
    return msg;
    DM_ERRNO_MAP(XX)
#undef XX
  default:
    break;
  }
  dm__errno_lock();
  for (uiIdx = 0; uiIdx < g_uiModErrRegCount; uiIdx++)
  {
    const dm_err_entry_st *pastMap = g_astModErrReg[uiIdx].pastMap;
    uint uiJ = 0;
    for (uiJ = 0; (0 != pastMap[uiJ].iCode) && (NULL != pastMap[uiJ].pcMsg); uiJ++)
    {
      if (pastMap[uiJ].iCode == err)
      {
        pcStr = pastMap[uiJ].pcMsg;
        dm__errno_unlock();
        return pcStr;
      }
    }
  }
  for (uiIdx = 0; uiIdx < g_uiErrnoRegCount; uiIdx++)
  {
    if (dm__errno_overlap(err, 1u, g_astErrnoReg[uiIdx].iBase, g_astErrnoReg[uiIdx].uiCount))
    {
      pfStr = g_astErrnoReg[uiIdx].pfStr;
      break;
    }
  }
  dm__errno_unlock();
  if (NULL != pfStr)
  {
    pcStr = pfStr(err);
    return (NULL != pcStr) ? pcStr : "unknown error";
  }
  return dm_strerror(err);
}

/* ---- 带业务标签的分配（默认走轻量标签统计；DM_MEM_DEBUG 桥接到全量调试分配器） ---- */
#ifdef DM_MEM_DEBUG
void *dm_malloc_tag_dbg(size_t size, int tag, dm_in const char *pcFile, dm_in int iLine)
{
  return dm_mem_debug_alloc_tag(size, tag, pcFile, iLine);
}

void *dm_calloc_tag_dbg(size_t count, size_t size, int tag, dm_in const char *pcFile,
                        dm_in int iLine)
{
  return dm_mem_debug_calloc_tag(count, size, tag, pcFile, iLine);
}

void *dm_realloc_tag_dbg(void *ptr, size_t size, dm_in const char *pcFile, dm_in int iLine)
{
  return dm_mem_debug_realloc(ptr, size, pcFile, iLine);
}

void dm_free_tag_dbg(void *ptr)
{
  dm_mem_debug_free(ptr);
  return;
}

void *(dm_malloc_tag)(size_t size, int tag)
{
  return dm_mem_debug_alloc_tag(size, tag, "dm", 0);
}

void *(dm_calloc_tag)(size_t count, size_t size, int tag)
{
  return dm_mem_debug_calloc_tag(count, size, tag, "dm", 0);
}

void *(dm_realloc_tag)(void *ptr, size_t size)
{
  return dm_mem_debug_realloc(ptr, size, "dm", 0);
}

void(dm_free_tag)(void *ptr)
{
  dm_mem_debug_free(ptr);
  return;
}
#else
void *dm_malloc_tag(size_t size, int tag)
{
  return dm_mem_tag_alloc(size, tag);
}

void *dm_calloc_tag(size_t count, size_t size, int tag)
{
  return dm_mem_tag_calloc(count, size, tag);
}

void *dm_realloc_tag(void *ptr, size_t size)
{
  return dm_mem_tag_realloc(ptr, size);
}

void dm_free_tag(void *ptr)
{
  dm_mem_tag_free(ptr);
  return;
}
#endif
