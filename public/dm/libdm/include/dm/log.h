/**
 ******************************************************************************
 * @file    log.h
 * @author  MDM
 * @version 0.0.0.2
 * @brief   DM 的日志（详设 2.12、1.6）。
 *
 * 一行的样子：
 *   2026-11-16 10:23:45.123 I T00 link  link_proto.c:120  conn closed by peer
 *   日期 时间到毫秒、级别字母、线程号（T00 是调 dm_log_init 的线程，T01 起按先后领）、
 *   模块名、文件:行号、正文。
 *
 * 模块名：每个 .c 文件在包含本头文件之前 #define DM_LOG_MOD "net"；不定义的是 "dm"。
 * 控制台输出写标准错误（标准输出留给就绪行）。每行落盘前打码（2.12），这只是兜底，
 * 带密钥、令牌、口令的值本来就不要写进日志。内容用英文。
 ******************************************************************************
 */

#ifndef DM_LOG_H
#define DM_LOG_H

#include <dm/type.h> /* DM_API */

#ifdef __cplusplus
extern "C"
{
#endif

/* 没定义模块名的文件算 "dm" */
#ifndef DM_LOG_MOD
#define DM_LOG_MOD "dm"
#endif

/* printf 格式检查：格式串和参数对不上，编译时报（-Werror 下直接编不过） */
#if defined(__GNUC__) || defined(__clang__)
#define DM_PRINTF_FMT(iFmt, iArgs) __attribute__((format(printf, iFmt, iArgs)))
#else
#define DM_PRINTF_FMT(iFmt, iArgs)
#endif

/* 日志级别，数值越小越详细 */
typedef enum dm_log_level
{
  DM_LOG_LEVEL_DEBUG = 0,
  DM_LOG_LEVEL_INFO,
  DM_LOG_LEVEL_WARN,
  DM_LOG_LEVEL_ERROR,
  DM_LOG_LEVEL_NONE /* 关闭全部日志 */
} dm_log_level_et;

/*
 * 初始化日志。调它的线程记成 T00（dm_main_ex 在主线程调）。
 *   pcDir      : 日志目录，不存在会递归创建；NULL = 只写控制台
 *   pcBaseName : 文件名前缀，实际文件为 <前缀>.log；NULL 用 "app"
 *   eLevel     : 最低级别，低于它的丢掉
 *   iMaxSizeMb : 单个文件最大 MB，超过就轮换（base.log → .1 → .2）；<= 0 不轮换
 *   iMaxFiles  : 轮换时留几个旧文件；<= 0 不留
 * 返回 DM_SUCCESS；文件打不开回 DM_IO。重复调直接返回成功。
 */
DM_API int dm_log_init(dm_in const char *pcDir, dm_in const char *pcBaseName,
                       dm_in dm_log_level_et eLevel, dm_in int iMaxSizeMb, dm_in int iMaxFiles);

/* 关日志，刷盘。最近一批（dm_log_recent）也清空。程序退出前调一次 */
DM_API void dm_log_close(void);

/* 改级别、开关控制台、开关逐行刷盘（默认开，崩溃时不丢）。任何线程都能调 */
DM_API void dm_log_set_level(dm_in dm_log_level_et eLevel);
DM_API dm_log_level_et dm_log_get_level(void);
DM_API void dm_log_set_console(dm_in int iEnable);
DM_API void dm_log_set_flush(dm_in int iEnable);

/* 日志位置上下文（打包级别、模块名与源位置，保持函数参数 <= 5） */
typedef struct dm_log_loc
{
  dm_log_level_et eLevel; /* 日志级别 */
  const char *pcMod;      /* 模块名 */
  const char *pcFile;     /* 源文件名 */
  int iLine;              /* 源文件行号 */
} dm_log_loc_st;

/* 写一行。一般用下面的宏。任何线程都能调；没初始化时只写标准错误 */
DM_API void dm_log_write(dm_in const dm_log_loc_st *pstLoc, dm_in const char *pcFmt, ...)
    DM_PRINTF_FMT(2, 3);

/*
 * 内存里留的最近一批（最多 DM_LOG_RECENT 行，每行最多存 DM_LOG_RECENT_LINE 字节，
 * 超出的截掉）。从旧到新把最后 u32Max 行逐行交给 pfLine（行尾不带换行），
 * 返回交了几行。回调在日志锁里调，里面不许再写日志。给 dmcmd 的 dm.log.recent 用。
 */
typedef void (*dm_log_line_cb)(dm_in const char *pcLine, dm_in void *pvArg);
DM_API uint32 dm_log_recent(dm_in uint32 u32Max, dm_in dm_log_line_cb pfLine, dm_in void *pvArg);

/*
 * 只留个头：pcVal 变成「前 4 个字符…(总长度)」，例 sk-a…(51)，写进 pcBuf。
 * 不足 8 个字符的只写「…(长度)」，免得短值几乎原样露出来。返回 pcBuf。
 * 需要在日志里对一下是哪个令牌时用。
 */
DM_API const char *dm_log_mask(dm_in const char *pcVal, dm_out char *pcBuf, dm_in uint32 u32Size);

/*
 * 按日志同一套密钥字样打码（authorization、password、api_key、secret、bearer、cookie、token
 * 后面的值换成 ***；网址 scheme://账号:密码@主机 里的账号密码也换成 ***，规则见
 * log.c）。给要把别处来的文字拿给人看的地方用（比如子进程的命令行）。 pcOut 容量 uCap（含结尾的
 * '\0'，至少 1）；返回写了多长。哪个线程都能调
 */
DM_API size_t dm_log_redact(dm_in const char *pcIn, dm_out char *pcOut, dm_in size_t uCap);

/* 便捷宏：自动带上模块名、文件名和行号 */
#define DM_LOG__EMIT(lvl, ...)                                                                     \
  do                                                                                               \
  {                                                                                                \
    const dm_log_loc_st stDmLogLoc = {(lvl), DM_LOG_MOD, __FILE__, __LINE__};                      \
    dm_log_write(&stDmLogLoc, __VA_ARGS__);                                                        \
  } while (0)
#define DM_LOG_DEBUG(...) DM_LOG__EMIT(DM_LOG_LEVEL_DEBUG, __VA_ARGS__)
#define DM_LOG_INFO(...) DM_LOG__EMIT(DM_LOG_LEVEL_INFO, __VA_ARGS__)
#define DM_LOG_WARN(...) DM_LOG__EMIT(DM_LOG_LEVEL_WARN, __VA_ARGS__)
#define DM_LOG_ERROR(...) DM_LOG__EMIT(DM_LOG_LEVEL_ERROR, __VA_ARGS__)

/* 日志统计快照（切断 log.c 对 dm_internal.h 的反向依赖） */
typedef struct dm_log_stat
{
  uint64 u64Lines;      /* 写出的日志总条数 */
  uint64 u64LockNs;     /* 等日志锁累计纳秒 */
  uint64 u64LockMaxNs;  /* 单次等锁最长纳秒 */
  uint64 u64WriteNs;    /* 实际落盘累计纳秒 */
  uint64 u64WriteMaxNs; /* 单次落盘最长纳秒 */
} dm_log_stat_st;

DM_API void dm_log_stat(dm_out dm_log_stat_st *pstOut);

#ifdef __cplusplus
}
#endif

#endif /* DM_LOG_H */
