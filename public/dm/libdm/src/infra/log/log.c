/**
 ******************************************************************************
 * @file    log.c
 * @author  MDM
 * @version 0.0.0.2
 * @brief   日志实现：时间到毫秒、线程号、模块名、打码、最近一批、轮换。
 *          不管：日志往哪个目录写、起始级别（由 dm_main_ex 按 dm_cfg 调 dm_log_init）。
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 MDM.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include <dm.h>
#include <dm/log.h>
#include <dm/limits.h>

#include <stdatomic.h> /* g_eLogLevel 原子级别过滤（W1-AUD-08） */
#include <stdio.h>     /* FILE / fopen / vsnprintf */
#include <string.h>    /* memcpy / strlen / strrchr */
#include <stdarg.h>    /* va_list */
#include <time.h>      /* localtime / strftime */
#include <errno.h>     /* errno */
#include <ctype.h>     /* tolower / isalnum */
#include "../../uv_adapter.h"

/* ============================ 内部状态 ============================ */
#define DM_LOG_DIR_MAX 512
#define DM_LOG_PATH_MAX 1200
#define DM_LOG_BODY_MAX 2048          /* 正文上限，超了截掉 */
#define DM_LOG_LINE_MAX 2400          /* 一整行上限（正文 + 行头） */
#define DM_LOG_BUF_SIZE (64u * 1024u) /* 文件流缓冲（关掉逐行刷盘时才起作用） */

static FILE *g_pstLogFp = NULL;
static char g_acLogDir[DM_LOG_DIR_MAX] = {0};
static char g_acLogBase[128] = {0};
static char g_acLogPath[DM_LOG_PATH_MAX] = {0};
static _Atomic int g_eLogLevel = (int)DM_LOG_LEVEL_INFO;
static int g_iLogConsole = 1;
static int g_iLogFlush = 1;
static uint64 g_u64LogMaxSize = 0; /* 字节，0 = 不轮换 */
static int g_iLogMaxFiles = 0;
static uint64 g_u64LogCurSize = 0;
static int g_iLogInited = 0;

/* 时间戳按秒缓存：同一秒内复用，避开每次 localtime 的时区查表 */
static time_t g_tLogLastSec = (time_t)0;
static char g_acLogTs[32] = {0};

/* 线程号：每个线程第一次写日志时领一个，记在线程局部变量里；调 dm_log_init 的线程是 0 */
static _Thread_local int g_iLogTid = -1;
static int g_iLogNextTid = 1;

/* 最近一批：定长环，热路径不分配内存 */
static char g_aacLogRecent[DM_LOG_RECENT][DM_LOG_RECENT_LINE];
static uint32 g_u32LogRecentHead = 0;  /* 下一行写到哪一格 */
static uint32 g_u32LogRecentCount = 0; /* 已经存了几行 */
static uint64 g_u64LogRecentSeq = 0;   /* 单调递增序号，供 OOM 逐行兜底不漏不错位（W1-AUD-07） */

/* 主线程（T00）写日志的开销：等锁、写 */
static dm_log_stat_st g_stLogStat;

/* ============================ 跨线程锁 ============================
 * 走适配层的 dm_mutex_*，不写 #ifdef _WIN32 / pthread。
 * 锁随进程存活，不销毁 —— 免去「close 之后再 init」的悬垂竞态。 */
static dm_mutex_t g_stLogLock;
static dm_once_t g_stLogLockOnce = DM_ONCE_INIT;

static void dm__log_lock_init(void)
{
  dm_mutex_init(&g_stLogLock);
  return;
}

/* 多个线程可能同时写第一条日志：用 dm_once 保证锁只初始化一次 */
static void dm__log_lock(void)
{
  dm_once(&g_stLogLockOnce, dm__log_lock_init);
  dm_mutex_lock(&g_stLogLock);
  return;
}

static void dm__log_unlock(void)
{
  dm_mutex_unlock(&g_stLogLock);
  return;
}

/* ============================ 行头 ============================ */

static char dm__log_level_char(in dm_log_level_et eLevel)
{
  switch (eLevel)
  {
  case DM_LOG_LEVEL_DEBUG:
    return 'D';
  case DM_LOG_LEVEL_INFO:
    return 'I';
  case DM_LOG_LEVEL_WARN:
    return 'W';
  case DM_LOG_LEVEL_ERROR:
    return 'E';
  default:
    return '?';
  }
}

/* 取「年-月-日 时:分:秒」，同一秒复用。毫秒由 pu32Ms 带出。调用点必须已持锁 */
static const char *dm__log_timestamp(out uint32 *pu32Ms)
{
  dm_timeval64_st stTv = {0};
  time_t tNow = (time_t)0;
  struct tm *pstTm = NULL;

  memset(&stTv, 0, sizeof(stTv));
  if (DM_SUCCESS != dm_gettimeofday(&stTv))
  {
    stTv.tv_sec = (int64_t)time(NULL);
    stTv.tv_usec = 0;
  }
  *pu32Ms = (uint32)(stTv.tv_usec / 1000);
  tNow = (time_t)stTv.tv_sec;
  if (tNow == g_tLogLastSec)
  {
    return g_acLogTs;
  }
  pstTm = localtime(&tNow);
  if (NULL != pstTm)
  {
    strftime(g_acLogTs, sizeof(g_acLogTs), "%Y-%m-%d %H:%M:%S", pstTm);
    g_tLogLastSec = tNow;
  }
  return g_acLogTs;
}

/* 当前线程的线程号；第一次调时领一个。调用点必须已持锁 */
static int dm__log_tid(void)
{
  if (g_iLogTid < 0)
  {
    g_iLogTid = g_iLogNextTid;
    g_iLogNextTid++;
  }
  return g_iLogTid;
}

/* 只留文件名，去掉路径 */
static const char *dm__log_short_file(in const char *pcFile)
{
  const char *pcSlash = NULL;
  const char *pcBack = NULL;

  if (NULL == pcFile)
  {
    return "?";
  }
  pcSlash = strrchr(pcFile, '/');
  pcBack = strrchr(pcFile, '\\');
  if (pcBack > pcSlash)
  {
    pcSlash = pcBack;
  }
  return (NULL != pcSlash) ? (pcSlash + 1) : pcFile;
}

/* ============================ 打码 ============================
 * 这些字样后面跟着的值换成 ***（不分大小写）。值的范围：
 *   跳过分隔符 「 」「:」「=」和引号；最后一个分隔符是引号的，值一直到配对的引号；
 *   否则到空白、逗号、分号、& 为止。authorization 后面的「Bearer 」一起跳过。
 * 只是兜底：敏感值本来就不该写进日志。 */
static const char *const g_apcLogSecretWord[] = {
    "authorization", "password", "api_key", "secret", "bearer", "cookie", "token",
};

/* pcAt 处是不是 pcWord（不分大小写）；是就返回它的长度，不是返回 0 */
static size_t dm__log_word_at(in const char *pcAt, in const char *pcWord)
{
  size_t uIdx = 0;
  for (uIdx = 0; '\0' != pcWord[uIdx]; uIdx++)
  {
    if (tolower((unsigned char)pcAt[uIdx]) != pcWord[uIdx])
    {
      return 0;
    }
  }
  return uIdx;
}

static int dm__log_is_sep(in char c)
{
  return (' ' == c) || ('\t' == c) || (':' == c) || ('=' == c) || ('"' == c) || ('\'' == c);
}

static int dm__log_is_value_end(in char c)
{
  return ('\0' == c) || (' ' == c) || ('\t' == c) || (',' == c) || (';' == c) || ('&' == c) ||
         ('"' == c) || ('\'' == c) || ('\r' == c) || ('\n' == c) || ('}' == c) || (')' == c);
}

/* 往 pcOut 里追加 uLen 个字节，满了就不再写；返回新的长度 */
static size_t dm__log_put(io char *pcOut, in size_t uCap, in size_t uPos, in const char *pc,
                          in size_t uLen)
{
  size_t uRoom = (uPos + 1u < uCap) ? (uCap - 1u - uPos) : 0u;
  if (uLen > uRoom)
  {
    uLen = uRoom;
  }
  memcpy(pcOut + uPos, pc, uLen);
  return uPos + uLen;
}

/* 把 pcIn 打码后写进 pcOut（容量 uCap，结尾补 '\0'）；返回写了多长 */
static size_t dm__log_mask_secrets(in const char *pcIn, out char *pcOut, in size_t uCap)
{
  const char *pc = pcIn;
  size_t uPos = 0;
  size_t uWord = 0;
  size_t uIdx = 0;
  const char *pcSep = NULL;
  const char *pcVal = NULL;
  char cQuote = '\0';

  while ('\0' != *pc)
  {
    /* 网址里的账号密码：scheme://user:pass@host 的 user:pass 换成 ***（令牌常这么带） */
    if ((':' == pc[0]) && ('/' == pc[1]) && ('/' == pc[2]))
    {
      const char *pcAt = pc + 3;
      while (('\0' != *pcAt) && (NULL == strchr("/@?# \t\"'\r\n", *pcAt)))
      {
        pcAt++;
      }
      uPos = dm__log_put(pcOut, uCap, uPos, pc, 3u);
      pc += 3;
      if (('@' == *pcAt) && (pcAt > pc))
      {
        uPos = dm__log_put(pcOut, uCap, uPos, "***", 3u);
        pc = pcAt;
      }
      continue;
    }
    uWord = 0;
    for (uIdx = 0; uIdx < ARRAY_SIZE(g_apcLogSecretWord); uIdx++)
    {
      uWord = dm__log_word_at(pc, g_apcLogSecretWord[uIdx]);
      if (0u != uWord)
      {
        break;
      }
    }
    if (0u == uWord)
    {
      uPos = dm__log_put(pcOut, uCap, uPos, pc, 1u);
      pc++;
      continue;
    }

    /* 字样本身原样留着 */
    uPos = dm__log_put(pcOut, uCap, uPos, pc, uWord);
    pc += uWord;

    /* 分隔符原样留着；记下最后一个是不是引号 */
    pcSep = pc;
    cQuote = '\0';
    while (dm__log_is_sep(*pc))
    {
      cQuote = (('"' == *pc) || ('\'' == *pc)) ? *pc : '\0';
      pc++;
    }
    if (pc == pcSep)
    {
      continue; /* 字样后面直接跟着别的字母（比如 tokens），不算 */
    }
    uPos = dm__log_put(pcOut, uCap, uPos, pcSep, (size_t)(pc - pcSep));

    /* Authorization: Bearer xxx 里的 Bearer 和后面的空格也留着 */
    uWord = dm__log_word_at(pc, "bearer");
    if ((0u != uWord) && (' ' == pc[uWord]) && ('\0' == cQuote))
    {
      uPos = dm__log_put(pcOut, uCap, uPos, pc, uWord + 1u);
      pc += uWord + 1u;
    }

    pcVal = pc;
    if ('\0' != cQuote)
    {
      while (('\0' != *pc) && (cQuote != *pc))
      {
        pc++;
      }
    }
    else
    {
      while (!dm__log_is_value_end(*pc))
      {
        pc++;
      }
    }
    if (pc != pcVal)
    {
      uPos = dm__log_put(pcOut, uCap, uPos, "***", 3u);
    }
  }
  pcOut[uPos] = '\0';
  return uPos;
}

/* ============================ 最近一批 ============================ */

/* 存一行（去掉行尾换行，超长截掉）。调用点必须已持锁 */
static void dm__log_recent_put(in const char *pcLine, in size_t uLen)
{
  char *pcSlot = g_aacLogRecent[g_u32LogRecentHead];

  if ((uLen > 0u) && ('\n' == pcLine[uLen - 1u]))
  {
    uLen--;
  }
  if (uLen >= DM_LOG_RECENT_LINE)
  {
    uLen = DM_LOG_RECENT_LINE - 1u;
  }
  memcpy(pcSlot, pcLine, uLen);
  pcSlot[uLen] = '\0';
  g_u32LogRecentHead = (g_u32LogRecentHead + 1u) % DM_LOG_RECENT;
  if (g_u32LogRecentCount < DM_LOG_RECENT)
  {
    g_u32LogRecentCount++;
  }
  g_u64LogRecentSeq++;
  return;
}

/* ============================ 文件 ============================ */

/* 同步建一级目录。dm_fs_mkdir 的 req 不能为 NULL（libuv 会直接回 UV_EINVAL），
 * 要给一个真实请求体，再 cleanup 掉它内部的路径分配。 */
static void dm__log_mkdir_one(in const char *pcPath)
{
  dm_fs_st stReq = {0};
  memset(&stReq, 0, sizeof(stReq));
  (void)dm_fs_mkdir(NULL, &stReq, pcPath, 0755, NULL); /* 已存在会失败，不用管 */
  dm_fs_req_cleanup(&stReq);
  return;
}

/* 递归建目录，支持 "a"、"a/b"、"a/b/c"。已存在不算错 */
static void dm__log_make_dirs(in const char *pcPath)
{
  char acTmp[DM_LOG_DIR_MAX] = {0};
  size_t uLen = 0;
  size_t uIdx = 0;
  char cSaved = '\0';

  if ((NULL == pcPath) || ('\0' == *pcPath))
  {
    return;
  }
  uLen = strlen(pcPath);
  if (uLen >= sizeof(acTmp))
  {
    return;
  }
  memcpy(acTmp, pcPath, uLen + 1u);

  /* 去掉结尾的分隔符 */
  while ((uLen > 1u) && (('/' == acTmp[uLen - 1u]) || ('\\' == acTmp[uLen - 1u])))
  {
    acTmp[--uLen] = '\0';
  }
  for (uIdx = 1; uIdx < uLen; uIdx++)
  {
    if (('/' == acTmp[uIdx]) || ('\\' == acTmp[uIdx]))
    {
      cSaved = acTmp[uIdx];
      acTmp[uIdx] = '\0';
      dm__log_mkdir_one(acTmp);
      acTmp[uIdx] = cSaved;
    }
  }
  dm__log_mkdir_one(acTmp);
  return;
}

/* 打开当前日志文件（追加），记下它的大小 */
static int dm__log_open_current(void)
{
  if ('\0' == g_acLogPath[0])
  {
    return DM_SUCCESS;
  }
  g_pstLogFp = fopen(g_acLogPath, "ab");
  if (NULL == g_pstLogFp)
  {
    /* 日志本身打不开，只能写标准错误 */
    fprintf(stderr, "[dm] log open '%s' failed: %s\n", g_acLogPath, strerror(errno));
    return DM_IO;
  }
  /* 必须在任何流操作之前设置 */
  setvbuf(g_pstLogFp, NULL, _IOFBF, DM_LOG_BUF_SIZE);
  if (0 == fseek(g_pstLogFp, 0, SEEK_END))
  {
    g_u64LogCurSize = (uint64)ftell(g_pstLogFp);
  }
  else
  {
    g_u64LogCurSize = 0;
  }
  return DM_SUCCESS;
}

/* 轮换：base.log -> base.log.1 -> base.log.2 ... */
static void dm__log_rotate(void)
{
  char acFrom[DM_LOG_PATH_MAX + 16] = {0}; /* 路径 + ".<序号>" */
  char acTo[DM_LOG_PATH_MAX + 16] = {0};
  int iIdx = 0;

  if (NULL != g_pstLogFp)
  {
    fclose(g_pstLogFp);
    g_pstLogFp = NULL;
  }
  if (g_iLogMaxFiles > 0)
  {
    snprintf(acFrom, sizeof(acFrom), "%s.%d", g_acLogPath, g_iLogMaxFiles);
    (void)remove(acFrom); /* 最老的那个；不存在不算错 */
    for (iIdx = g_iLogMaxFiles - 1; iIdx >= 1; iIdx--)
    {
      snprintf(acFrom, sizeof(acFrom), "%s.%d", g_acLogPath, iIdx);
      snprintf(acTo, sizeof(acTo), "%s.%d", g_acLogPath, iIdx + 1);
      (void)rename(acFrom, acTo); /* 不存在不算错 */
    }
    snprintf(acTo, sizeof(acTo), "%s.1", g_acLogPath);
    (void)rename(g_acLogPath, acTo);
  }
  else
  {
    (void)remove(g_acLogPath); /* 不留旧文件，直接清空 */
  }
  g_u64LogCurSize = 0;
  (void)dm__log_open_current(); /* 打不开时已经写过标准错误，之后只写控制台 */
  return;
}

/* ============================ 对外接口 ============================ */

int dm_log_init(in const char *pcDir, in const char *pcBaseName, in dm_log_level_et eLevel,
                in int iMaxSizeMb, in int iMaxFiles)
{
  dm__log_lock();
  if (g_iLogInited)
  {
    dm__log_unlock();
    return DM_SUCCESS; /* 重复初始化直接返回成功 */
  }

  g_iLogTid = 0;          /* 调 init 的线程是 T00 */
  g_u32LogRecentHead = 0; /* 最近几行和开销统计在这里清（close 时留着，退出以后还能取） */
  g_u32LogRecentCount = 0;
  g_u64LogRecentSeq = 0;
  memset(&g_stLogStat, 0, sizeof(g_stLogStat));
  atomic_store_explicit(&g_eLogLevel, (int)eLevel, memory_order_relaxed);
  g_iLogConsole = 1;
  g_iLogMaxFiles = (iMaxFiles > 0) ? iMaxFiles : 0;
  g_u64LogMaxSize = (iMaxSizeMb > 0) ? ((uint64)iMaxSizeMb * 1024u * 1024u) : 0u;
  snprintf(g_acLogBase, sizeof(g_acLogBase), "%s",
           ((NULL != pcBaseName) && ('\0' != *pcBaseName)) ? pcBaseName : "app");

  if ((NULL != pcDir) && ('\0' != *pcDir))
  {
    snprintf(g_acLogDir, sizeof(g_acLogDir), "%s", pcDir);
    dm__log_make_dirs(g_acLogDir);
    snprintf(g_acLogPath, sizeof(g_acLogPath), "%s/%s.log", g_acLogDir, g_acLogBase);
  }
  else
  {
    g_acLogDir[0] = '\0';
    g_acLogPath[0] = '\0';
  }
  if (('\0' != g_acLogPath[0]) && (DM_SUCCESS != dm__log_open_current()))
  {
    dm__log_unlock();
    return DM_IO;
  }
  g_iLogInited = 1;
  dm__log_unlock();
  return DM_SUCCESS;
}

void dm_log_close(void)
{
  dm__log_lock();
  if (NULL != g_pstLogFp)
  {
    fflush(g_pstLogFp);
    fclose(g_pstLogFp);
    g_pstLogFp = NULL;
  }
  g_iLogInited = 0; /* 最近几行留着：退出以后还能取（测试、崩溃前的诊断）；下次 dm_log_init 才清 */
  dm__log_unlock();
  return;
}

void dm_log_set_level(in dm_log_level_et eLevel)
{
  dm__log_lock();
  atomic_store_explicit(&g_eLogLevel, (int)eLevel, memory_order_relaxed);
  dm__log_unlock();
  return;
}

dm_log_level_et dm_log_get_level(void)
{
  return (dm_log_level_et)atomic_load_explicit(&g_eLogLevel, memory_order_relaxed);
}

void dm_log_set_console(in int iEnable)
{
  dm__log_lock();
  g_iLogConsole = iEnable ? 1 : 0;
  dm__log_unlock();
  return;
}

void dm_log_set_flush(in int iEnable)
{
  dm__log_lock();
  g_iLogFlush = iEnable ? 1 : 0;
  dm__log_unlock();
  return;
}

/* 记一行的开销（只记主线程的，持锁调） */
static void dm__log_cost(in uint64 u64T0, in uint64 u64T1)
{
  uint64 u64Lock = u64T1 - u64T0;
  uint64 u64Write = dm_hrtime() - u64T1;
  if (0 != dm__log_tid())
  {
    return;
  }
  g_stLogStat.u64Lines++;
  g_stLogStat.u64LockNs += u64Lock;
  g_stLogStat.u64WriteNs += u64Write;
  if (u64Lock > g_stLogStat.u64LockMaxNs)
  {
    g_stLogStat.u64LockMaxNs = u64Lock;
  }
  if (u64Write > g_stLogStat.u64WriteMaxNs)
  {
    g_stLogStat.u64WriteMaxNs = u64Write;
  }
  return;
}

void dm_log_write(in dm_log_level_et eLevel, in const char *pcMod, in const char *pcFile,
                  in int iLine, in const char *pcFmt, ...)
{
  char acRaw[DM_LOG_BODY_MAX] = {0};
  char acBody[DM_LOG_BODY_MAX] = {0};
  char acLine[DM_LOG_LINE_MAX] = {0};
  const char *pcTs = NULL;
  uint32 u32Ms = 0;
  va_list ap;
  int iRaw = 0;
  int iLen = 0;
  uint64 u64T0 = 0;
  uint64 u64T1 = 0;

  /* 级别过滤（原子无锁读，TSAN 干净） */
  if (((int)eLevel < atomic_load_explicit(&g_eLogLevel, memory_order_relaxed)) ||
      (eLevel >= DM_LOG_LEVEL_NONE))
  {
    return;
  }

  /* 正文先在锁外格式化、打码：最费时，只用栈上缓冲，几个线程可以同时做 */
  va_start(ap, pcFmt);
  iRaw = vsnprintf(acRaw, sizeof(acRaw), pcFmt, ap);
  va_end(ap);
  if (iRaw < 0)
  {
    return;
  }
  (void)dm__log_mask_secrets(acRaw, acBody, sizeof(acBody));

  u64T0 = dm_hrtime();
  dm__log_lock();
  u64T1 = dm_hrtime();
  pcTs = dm__log_timestamp(&u32Ms);
  iLen = snprintf(acLine, sizeof(acLine), "%s.%03u %c T%02d %-5s %s:%d  %s\n", pcTs,
                  (unsigned)u32Ms, dm__log_level_char(eLevel), dm__log_tid(),
                  (NULL != pcMod) ? pcMod : "dm", dm__log_short_file(pcFile), iLine, acBody);
  if (iLen < 0)
  {
    dm__log_unlock();
    return;
  }
  /* 截断时 snprintf 返回的是「本该多长」，要夹到实际长度，否则轮换会提前 */
  if ((size_t)iLen >= sizeof(acLine))
  {
    iLen = (int)sizeof(acLine) - 1;
    acLine[iLen - 1] = '\n';
  }

  /* 没初始化：只写标准错误 */
  if (!g_iLogInited)
  {
    fwrite(acLine, 1u, (size_t)iLen, stderr);
    fflush(stderr);
    dm__log_unlock();
    return;
  }

  dm__log_recent_put(acLine, (size_t)iLen);
  if (g_iLogConsole)
  {
    fwrite(acLine, 1u, (size_t)iLen, stderr);
    if (g_iLogFlush)
    {
      fflush(stderr);
    }
  }
  if (NULL != g_pstLogFp)
  {
    if ((g_u64LogMaxSize > 0u) && (g_u64LogCurSize + (uint64)iLen > g_u64LogMaxSize))
    {
      dm__log_rotate();
    }
    if (NULL != g_pstLogFp)
    {
      fwrite(acLine, 1u, (size_t)iLen, g_pstLogFp);
      g_u64LogCurSize += (uint64)iLen;
      if (g_iLogFlush)
      {
        fflush(g_pstLogFp);
      }
    }
  }
  dm__log_cost(u64T0, u64T1);
  dm__log_unlock();
  return;
}

void dm_log_stat(out dm_log_stat_st *pstOut)
{
  if (NULL == pstOut)
  {
    return;
  }
  dm__log_lock();
  *pstOut = g_stLogStat;
  dm__log_unlock();
  return;
}

uint32 dm_log_recent(in uint32 u32Max, in dm_log_line_cb pfLine, in void *pvArg)
{
  uint32 u32Alloc = 0;
  uint32 u32Count = 0;
  uint32 u32Idx = 0;
  uint32 u32Slot = 0;
  char *pcSnap = NULL;

  if ((NULL == pfLine) || (0u == u32Max))
  {
    return 0;
  }
  dm__log_lock();
  u32Alloc = (u32Max < g_u32LogRecentCount) ? u32Max : g_u32LogRecentCount;
  dm__log_unlock();
  if (0u == u32Alloc)
  {
    return 0;
  }
  pcSnap = (char *)dm_malloc((size_t)u32Alloc * DM_LOG_RECENT_LINE);
  if (NULL != pcSnap)
  {
    dm__log_lock();
    u32Count = (u32Alloc < g_u32LogRecentCount) ? u32Alloc : g_u32LogRecentCount;
    u32Slot = (g_u32LogRecentHead + DM_LOG_RECENT - u32Count) % DM_LOG_RECENT;
    for (u32Idx = 0; u32Idx < u32Count; u32Idx++)
    {
      memcpy(pcSnap + (size_t)u32Idx * DM_LOG_RECENT_LINE, g_aacLogRecent[u32Slot],
             DM_LOG_RECENT_LINE);
      u32Slot = (u32Slot + 1u) % DM_LOG_RECENT;
    }
    dm__log_unlock();
    for (u32Idx = 0; u32Idx < u32Count; u32Idx++)
    {
      pfLine(pcSnap + (size_t)u32Idx * DM_LOG_RECENT_LINE, pvArg);
    }
    dm_free(pcSnap);
    return u32Count;
  }
  /* OOM 兜底：用单调序号锁住快照区间 [u64Start, u64End)，逐行释锁回调不漏不错位（W1-AUD-07） */
  uint64 u64End = 0;
  uint64 u64Start = 0;
  uint64 u64Seq = 0;
  uint32 u32Delivered = 0;

  dm__log_lock();
  u32Count = (u32Alloc < g_u32LogRecentCount) ? u32Alloc : g_u32LogRecentCount;
  u64End = g_u64LogRecentSeq;
  u64Start = u64End - (uint64)u32Count;
  dm__log_unlock();

  for (u64Seq = u64Start; u64Seq < u64End; u64Seq++)
  {
    char acOne[DM_LOG_RECENT_LINE] = {0};
    uint64 u64Oldest = 0;

    dm__log_lock();
    u64Oldest = g_u64LogRecentSeq - (uint64)g_u32LogRecentCount;
    if (u64Seq < u64Oldest)
    {
      u64Seq = u64Oldest;
      if (u64Seq >= u64End)
      {
        dm__log_unlock();
        break;
      }
    }
    if (u64Seq >= g_u64LogRecentSeq)
    {
      dm__log_unlock();
      break;
    }
    u32Slot = (uint32)(u64Seq % (uint64)DM_LOG_RECENT);
    memcpy(acOne, g_aacLogRecent[u32Slot], sizeof(acOne));
    dm__log_unlock();
    pfLine(acOne, pvArg);
    u32Delivered++;
  }
  return u32Delivered;
}

size_t dm_log_redact(in const char *pcIn, out char *pcOut, in size_t uCap)
{
  if ((NULL == pcOut) || (0u == uCap))
  {
    return 0;
  }
  if (NULL == pcIn)
  {
    pcOut[0] = '\0';
    return 0;
  }
  return dm__log_mask_secrets(pcIn, pcOut, uCap);
}

const char *dm_log_mask(in const char *pcVal, out char *pcBuf, in uint32 u32Size)
{
  size_t uLen = 0;

  if ((NULL == pcBuf) || (0u == u32Size))
  {
    return pcBuf;
  }
  uLen = (NULL != pcVal) ? strlen(pcVal) : 0u;
  if (uLen >= 8u)
  {
    snprintf(pcBuf, u32Size, "%.4s\xE2\x80\xA6(%u)", pcVal, (unsigned)uLen); /* … */
  }
  else
  {
    snprintf(pcBuf, u32Size, "\xE2\x80\xA6(%u)", (unsigned)uLen);
  }
  return pcBuf;
}

void dm__log_stat(out dm_log_stat_st *pstOut)
{
  dm_log_stat(pstOut);
  return;
}

#ifdef __cplusplus
}
#endif
