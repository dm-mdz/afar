/**
 ******************************************************************************
 * @file    module.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   模块生命周期（DD-01）：dm_main_ex、阶段、收尾、拆除、信号、标准输入、
 *          数据目录锁、线程池、线程检查。
 *          不管：总队列本身（runtime/queue.c）、连接（net/）、定时器（runtime/timer.c）。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "dm"
#include <dm.h>
#include <dm/module.h>
#include <dm/check.h>
#include <dm/limits.h>
#include <dm/log.h>
#include <dm/event.h>
#include "../dm_internal.h"

#include <signal.h> /* SIGINT / SIGTERM / SIGPIPE */
#include <stdlib.h> /* _exit（Windows） */
#include <stdio.h>  /* snprintf / fflush */
#include <string.h> /* memset / memcpy / strlen */
#ifdef _WIN32
#include <windows.h>
#include <io.h> /* _write */
#else
#include <unistd.h> /* read / close / _exit */
#endif

#ifdef _WIN32
#define dm__getpid() ((long)GetCurrentProcessId())
#else
#define dm__getpid() ((long)getpid())
#endif

#define DM__DRAIN_HOLD_MAX 64u /* 收尾时同时拿着的号最多几个 */
#ifdef _WIN32
#define DM__DRAIN_MS_DEFAULT 2500u
#else
#define DM__DRAIN_MS_DEFAULT 5000u
#endif

struct dm_drain
{
  uint uiMod; /* 第几个模块 */
};

typedef struct dm__hold
{
  bool_t bUsed;
  uint uiMod;
} dm__hold_st;

/* ---- 阶段与模块 ---- */
static dm__phase_et g_ePhase = DM__PHASE_NONE;
static const dm_module_st *g_pastMod = NULL;
static uint g_uiModCount = 0;
static uint g_uiModInited = 0; /* pfInit 成功了几个（按清单顺序） */
static int g_iExitCode = 0;
static int g_iLastErr = DM_SUCCESS;
static bool_t g_bQuitting = DM_FALSE;
static char g_acBoot[17];     /* 启动号：每次启动换（16 个十六进制字符） */
static char g_acRunPath[600]; /* 写了运行文件就记着它，退出时删；空 = 没写 */
static uint32 g_u32DrainMs = DM__DRAIN_MS_DEFAULT;

/* ---- 收尾的号 ---- */
static dm__hold_st g_astHold[DM__DRAIN_HOLD_MAX];
static uint g_uiHoldCount = 0;

/* ---- 句柄 ---- */
static dm_timer_st g_stDrainTimer;    /* 收尾最多等多久 */
static dm_timer_st g_stWorkWaitTimer; /* 拆除时最多等在跑的 work 多久 */
static bool_t g_bDrainTimerUp = DM_FALSE;
static bool_t g_bWorkWaitTimerUp = DM_FALSE;
static dm_signal_st g_stSigInt;
static dm_signal_st g_stSigTerm;
static bool_t g_bSigIntUp = DM_FALSE;
static bool_t g_bSigTermUp = DM_FALSE;
#ifdef _WIN32
static dm_signal_st g_stSigHup; /* 控制台关闭事件，libuv 报成 SIGHUP */
static bool_t g_bSigHupUp = DM_FALSE;
#endif
static uint g_uiSignalCount = 0;

typedef union dm__stdin
{
  dm_pipe_st stPipe;
  dm_tty_st stTty;
} dm__stdin_ut;

static dm__stdin_ut g_uStdin;
static bool_t g_bStdinUp = DM_FALSE;
static char g_acStdinBuf[1024];

/* ============================ 阶段与扩展钩子 ============================ */

dm__phase_et dm__phase(void)
{
  return g_ePhase;
}

#define DM__EXT_HOOKS_MAX 8u

static const dm_ext_hooks_st *g_apstExt[DM__EXT_HOOKS_MAX];
static uint g_uiExtCount = 0;
static const dm_ext_hooks_st *g_pstCfgExt = NULL;
static uint g_uiExtInited = 0;
static bool_t g_bCfgExtInited = DM_FALSE;

int dm_ext_register(in const dm_ext_hooks_st *pstHooks)
{
  uint uiIdx = 0;

  if (NULL == pstHooks)
  {
    return DM_INVALID;
  }
  for (uiIdx = 0; uiIdx < g_uiExtCount; uiIdx++)
  {
    if (g_apstExt[uiIdx] == pstHooks)
    {
      return DM_SUCCESS;
    }
  }
  if (g_uiExtCount >= DM__EXT_HOOKS_MAX)
  {
    return DM_RANGE;
  }
  g_apstExt[g_uiExtCount++] = pstHooks;
  return DM_SUCCESS;
}

void dm_ext_clear(void)
{
  g_uiExtCount = 0;
  g_uiExtInited = 0;
  g_bCfgExtInited = DM_FALSE;
  g_pstCfgExt = NULL;
  return;
}

static bool_t dm__ext_has_cfg_extra(void)
{
  uint uiIdx = 0;

  if (NULL == g_pstCfgExt)
  {
    return DM_FALSE;
  }
  for (uiIdx = 0; uiIdx < g_uiExtCount; uiIdx++)
  {
    if (g_apstExt[uiIdx] == g_pstCfgExt)
    {
      return DM_FALSE;
    }
  }
  return DM_TRUE;
}

static int dm__ext_before_init(in const dm_cfg_st *pstCfg)
{
  uint uiIdx = 0;
  int iErr = DM_SUCCESS;

  g_uiExtInited = 0;
  g_bCfgExtInited = DM_FALSE;
  for (uiIdx = 0; uiIdx < g_uiExtCount; uiIdx++)
  {
    if (NULL != g_apstExt[uiIdx]->pfBeforeInit)
    {
      iErr = g_apstExt[uiIdx]->pfBeforeInit(pstCfg);
      if (DM_SUCCESS != iErr)
      {
        return iErr;
      }
    }
    g_uiExtInited = uiIdx + 1u;
  }
  if (dm__ext_has_cfg_extra())
  {
    if (NULL != g_pstCfgExt->pfBeforeInit)
    {
      iErr = g_pstCfgExt->pfBeforeInit(pstCfg);
      if (DM_SUCCESS != iErr)
      {
        return iErr;
      }
    }
    g_bCfgExtInited = DM_TRUE;
  }
  return DM_SUCCESS;
}

static void dm__ext_after_init(void)
{
  uint uiIdx = 0;

  for (uiIdx = 0; uiIdx < g_uiExtInited; uiIdx++)
  {
    if (NULL != g_apstExt[uiIdx]->pfAfterInit)
    {
      g_apstExt[uiIdx]->pfAfterInit();
    }
  }
  if (g_bCfgExtInited && (NULL != g_pstCfgExt) && (NULL != g_pstCfgExt->pfAfterInit))
  {
    g_pstCfgExt->pfAfterInit();
  }
  return;
}

static void dm__ext_on_drain(void)
{
  uint uiIdx = 0;

  if (g_bCfgExtInited && (NULL != g_pstCfgExt) && (NULL != g_pstCfgExt->pfOnDrain))
  {
    g_pstCfgExt->pfOnDrain();
  }
  for (uiIdx = g_uiExtInited; uiIdx > 0u; uiIdx--)
  {
    if (NULL != g_apstExt[uiIdx - 1u]->pfOnDrain)
    {
      g_apstExt[uiIdx - 1u]->pfOnDrain();
    }
  }
  return;
}

static void dm__ext_on_teardown(void)
{
  uint uiIdx = 0;

  if (g_bCfgExtInited && (NULL != g_pstCfgExt) && (NULL != g_pstCfgExt->pfOnTeardown))
  {
    g_pstCfgExt->pfOnTeardown();
  }
  for (uiIdx = g_uiExtInited; uiIdx > 0u; uiIdx--)
  {
    if (NULL != g_apstExt[uiIdx - 1u]->pfOnTeardown)
    {
      g_apstExt[uiIdx - 1u]->pfOnTeardown();
    }
  }
  return;
}

static void dm__ext_on_end(void)
{
  uint uiIdx = 0;

  if (g_bCfgExtInited && (NULL != g_pstCfgExt) && (NULL != g_pstCfgExt->pfOnEnd))
  {
    g_pstCfgExt->pfOnEnd();
  }
  for (uiIdx = g_uiExtInited; uiIdx > 0u; uiIdx--)
  {
    if (NULL != g_apstExt[uiIdx - 1u]->pfOnEnd)
    {
      g_apstExt[uiIdx - 1u]->pfOnEnd();
    }
  }
  g_uiExtInited = 0;
  g_bCfgExtInited = DM_FALSE;
  return;
}

void dm__ext_after_work(in uint32 u32StatKey)
{
  uint uiIdx = 0;

  for (uiIdx = 0; uiIdx < g_uiExtInited; uiIdx++)
  {
    if (NULL != g_apstExt[uiIdx]->pfAfterWork)
    {
      g_apstExt[uiIdx]->pfAfterWork(u32StatKey);
    }
  }
  if (g_bCfgExtInited && (NULL != g_pstCfgExt) && (NULL != g_pstCfgExt->pfAfterWork))
  {
    g_pstCfgExt->pfAfterWork(u32StatKey);
  }
  return;
}

void dm__ext_on_thread_exit(void)
{
  uint uiIdx = 0;

  for (uiIdx = 0; uiIdx < g_uiExtInited; uiIdx++)
  {
    if (NULL != g_apstExt[uiIdx]->pfOnThreadExit)
    {
      g_apstExt[uiIdx]->pfOnThreadExit();
    }
  }
  if (g_bCfgExtInited && (NULL != g_pstCfgExt) && (NULL != g_pstCfgExt->pfOnThreadExit))
  {
    g_pstCfgExt->pfOnThreadExit();
  }
  return;
}

static void dm__ext_get_ports(out int *piLink, out int *piHttp)
{
  uint uiIdx = 0;
  int iLink = 0;
  int iHttp = 0;

  for (uiIdx = 0; uiIdx < g_uiExtInited; uiIdx++)
  {
    if (NULL != g_apstExt[uiIdx]->pfGetPorts)
    {
      g_apstExt[uiIdx]->pfGetPorts(&iLink, &iHttp);
    }
  }
  if (g_bCfgExtInited && (NULL != g_pstCfgExt) && (NULL != g_pstCfgExt->pfGetPorts))
  {
    g_pstCfgExt->pfGetPorts(&iLink, &iHttp);
  }
  *piLink = iLink;
  *piHttp = iHttp;
  return;
}

uint32 dm__ext_child_count(void)
{
  uint uiIdx = 0;
  uint32 u32Count = 0;

  for (uiIdx = 0; uiIdx < g_uiExtInited; uiIdx++)
  {
    if (NULL != g_apstExt[uiIdx]->pfChildCount)
    {
      u32Count = g_apstExt[uiIdx]->pfChildCount();
    }
  }
  if (g_bCfgExtInited && (NULL != g_pstCfgExt) && (NULL != g_pstCfgExt->pfChildCount))
  {
    u32Count = g_pstCfgExt->pfChildCount();
  }
  return u32Count;
}

int dm__ext_reply_take(in const void *pvMsg, out void *pvReply)
{
  uint uiIdx = 0;

  for (uiIdx = 0; uiIdx < g_uiExtInited; uiIdx++)
  {
    if (NULL != g_apstExt[uiIdx]->pfReplyTake)
    {
      return g_apstExt[uiIdx]->pfReplyTake(pvMsg, pvReply);
    }
  }
  if (g_bCfgExtInited && (NULL != g_pstCfgExt) && (NULL != g_pstCfgExt->pfReplyTake))
  {
    return g_pstCfgExt->pfReplyTake(pvMsg, pvReply);
  }
  return DM_NOT_FOUND;
}

int dm__ext_reply_send(in const void *pvReply, in int iStatus, in const void *pvBody,
                       in uint32 u32Len)
{
  uint uiIdx = 0;

  for (uiIdx = 0; uiIdx < g_uiExtInited; uiIdx++)
  {
    if (NULL != g_apstExt[uiIdx]->pfReplySend)
    {
      return g_apstExt[uiIdx]->pfReplySend(pvReply, iStatus, pvBody, u32Len);
    }
  }
  if (g_bCfgExtInited && (NULL != g_pstCfgExt) && (NULL != g_pstCfgExt->pfReplySend))
  {
    return g_pstCfgExt->pfReplySend(pvReply, iStatus, pvBody, u32Len);
  }
  return DM_NOT_FOUND;
}

/* ============================ 拆除 ============================ */

static void dm__close_if(in dm_handle_st *pstHandle)
{
  if (!dm_is_closing(pstHandle))
  {
    dm_close(pstHandle, NULL);
  }
  return;
}

static void dm__signals_close(void)
{
  if (g_bSigIntUp)
  {
    dm__close_if((dm_handle_st *)&g_stSigInt);
    g_bSigIntUp = DM_FALSE;
  }
  if (g_bSigTermUp)
  {
    dm__close_if((dm_handle_st *)&g_stSigTerm);
    g_bSigTermUp = DM_FALSE;
  }
#ifdef _WIN32
  if (g_bSigHupUp)
  {
    dm__close_if((dm_handle_st *)&g_stSigHup);
    g_bSigHupUp = DM_FALSE;
  }
#endif
  return;
}

static void dm__stdin_close(void)
{
  if (g_bStdinUp)
  {
    dm__close_if((dm_handle_st *)&g_uStdin);
    g_bStdinUp = DM_FALSE;
  }
  return;
}

/* 拆除时还有 work 在跑、等满了也没回来：记 ERROR、刷日志，_exit(2)。不调 fini、不返回 */
static void dm__on_work_wait(in dm_timer_st *pstTimer)
{
  (void)pstTimer;
  if (0u == dm__work_inflight())
  {
    return;
  }
  DM_LOG_ERROR("%u work(s) still running %u ms after teardown, forcing exit",
               (unsigned)dm__work_inflight(), (unsigned)DM_WORK_WAIT_MS);
  dm__run_file_remove();
  dm_log_close();
  fflush(stdout);
  fflush(stderr);
  _exit(2);
}

static void dm__teardown(void)
{
  if (g_ePhase >= DM__PHASE_TEARDOWN)
  {
    return;
  }
  g_ePhase = DM__PHASE_TEARDOWN;
  DM_LOG_INFO("teardown");
  (void)dm_timer_stop(&g_stDrainTimer);
  dm__timer_cancel_all();
  dm__ext_on_teardown(); /* 强杀残留子进程、关监听与连接 */
  dm__run_file_remove();
  dm__queue_stop_async();   /* 停止接受跨线程 dm_queue_post_async */
  dm__work_cancel_queued(); /* 排着队还没开始的 work：after 收到「被取消」 */
  dm__signals_close();
  dm__stdin_close();
  if (dm__work_inflight() > 0u)
  {
    (void)dm_timer_start(&g_stWorkWaitTimer, dm__on_work_wait, DM_WORK_WAIT_MS, 0);
  }
  return;
}

static void dm__run_teardown(io dm__qitem_st *pstItem)
{
  (void)pstItem;
  dm__teardown();
  return;
}

/* ============================ 收尾 ============================ */

static void dm__on_drain_timeout(in dm_timer_st *pstTimer)
{
  uint uiIdx = 0;
  (void)pstTimer;

  for (uiIdx = 0; uiIdx < DM__DRAIN_HOLD_MAX; uiIdx++)
  {
    if (g_astHold[uiIdx].bUsed)
    {
      DM_LOG_WARN("module %s did not release drain hold %u within %u ms",
                  g_pastMod[g_astHold[uiIdx].uiMod].pcName, uiIdx + 1u, (unsigned)g_u32DrainMs);
    }
  }
  dm__teardown();
  return;
}

static void dm__run_drain(io dm__qitem_st *pstItem)
{
  dm_drain_st stDrain = {0};
  uint uiIdx = 0;
  (void)pstItem;

  if (DM__PHASE_RUN != g_ePhase)
  {
    return;
  }
  g_ePhase = DM__PHASE_DRAIN;
  DM_LOG_INFO("drain start");
  dm__ext_on_drain();
  for (uiIdx = g_uiModInited; uiIdx > 0u; uiIdx--)
  {
    if (NULL != g_pastMod[uiIdx - 1u].pfDrain)
    {
      stDrain.uiMod = uiIdx - 1u;
      g_pastMod[uiIdx - 1u].pfDrain(&stDrain);
    }
  }
  if (DM__PHASE_DRAIN != g_ePhase)
  {
    return; /* 某个 pfDrain 里第二个信号来了之类，已经进了拆除 */
  }
  if (0u == g_uiHoldCount)
  {
    dm__teardown();
    return;
  }
  (void)dm_timer_start(&g_stDrainTimer, dm__on_drain_timeout, g_u32DrainMs, 0);
  return;
}

int dm_drain_hold(io dm_drain_st *pstDrain)
{
  uint uiIdx = 0;

  DM__CHECK_LOOP();
  if ((NULL == pstDrain) || (DM__PHASE_DRAIN != g_ePhase))
  {
    return DM_LINK_ERR_STATE;
  }
  for (uiIdx = 0; uiIdx < DM__DRAIN_HOLD_MAX; uiIdx++)
  {
    if (!g_astHold[uiIdx].bUsed)
    {
      g_astHold[uiIdx].bUsed = DM_TRUE;
      g_astHold[uiIdx].uiMod = pstDrain->uiMod;
      g_uiHoldCount++;
      return (int)(uiIdx + 1u);
    }
  }
  return DM_RANGE;
}

void dm_drain_release(in int iHold)
{
  if ((!dm_in_loop()) || (iHold <= 0) || ((uint)iHold > DM__DRAIN_HOLD_MAX) ||
      (!g_astHold[iHold - 1].bUsed))
  {
    return;
  }
  g_astHold[iHold - 1].bUsed = DM_FALSE;
  g_uiHoldCount--;
  if ((0u == g_uiHoldCount) && (DM__PHASE_DRAIN == g_ePhase))
  {
    /* 不在调用方的调用栈里直接拆，排一条：调用方可能正在用着别的东西 */
    (void)dm__queue_run(dm__run_teardown, NULL);
  }
  return;
}

void dm_quit(in int iCode)
{
  if (!dm_in_loop())
  {
    dm__wrong_thread(__func__);
    return;
  }
  if (g_bQuitting || (DM__PHASE_NONE == g_ePhase))
  {
    return;
  }
  g_bQuitting = DM_TRUE;
  g_iExitCode = iCode;
  DM_LOG_INFO("quit requested, code %d", iCode);
  (void)dm__queue_run(dm__run_drain, NULL);
  return;
}

/* ============================ 信号、标准输入 ============================ */

/* 第一次：走收尾；第二次：不等了，直接拆除 */
static void dm__on_signal(in dm_signal_st *pstSignal, in int iSignum)
{
  (void)pstSignal;
  g_uiSignalCount++;
  DM_LOG_INFO("signal %d (%u)", iSignum, g_uiSignalCount);
  if (1u == g_uiSignalCount)
  {
    dm_quit(0);
    return;
  }
  dm__teardown();
  return;
}

static int dm__signals_open(void)
{
  dm_loop_st *pstLoop = dm_default_loop();
  int iErrCode = DM_SUCCESS;

  iErrCode = dm_signal_init(pstLoop, &g_stSigInt);
  if (DM_SUCCESS != iErrCode)
  {
    return iErrCode;
  }
  g_bSigIntUp = DM_TRUE;
  iErrCode = dm_signal_init(pstLoop, &g_stSigTerm);
  if (DM_SUCCESS != iErrCode)
  {
    dm__signals_close();
    return iErrCode;
  }
  g_bSigTermUp = DM_TRUE;
#ifdef _WIN32
  iErrCode = dm_signal_init(pstLoop, &g_stSigHup);
  if (DM_SUCCESS != iErrCode)
  {
    dm__signals_close();
    return iErrCode;
  }
  g_bSigHupUp = DM_TRUE;
#endif
  (void)dm_signal_start(&g_stSigInt, dm__on_signal, SIGINT);
  (void)dm_signal_start(&g_stSigTerm, dm__on_signal, SIGTERM);
#ifdef _WIN32
  (void)dm_signal_start(&g_stSigHup, dm__on_signal, SIGHUP);
#endif
  return DM_SUCCESS;
}

static void dm__on_stdin_alloc(in dm_handle_st *pstHandle, in size_t uSuggested,
                               out dm_buf_st *pstBuf)
{
  (void)pstHandle;
  (void)uSuggested;
  pstBuf->base = g_acStdinBuf;
  pstBuf->len = sizeof(g_acStdinBuf);
  return;
}

/* 标准输入读到的内容不用（这条路只为了知道它断没断）；断了就收尾 */
static void dm__on_stdin_read(in dm_stream_st *pstStream, in ssize_t iNRead,
                              in const dm_buf_st *pstBuf)
{
  (void)pstStream;
  (void)pstBuf;
  if (iNRead < 0)
  {
    DM_LOG_INFO("stdin closed, quitting");
    dm__stdin_close();
    dm_quit(0);
  }
  return;
}

static int dm__stdin_open(void)
{
  dm_handle_type_et eType = dm_guess_handle(0);
  int iErrCode = DM_SUCCESS;

  if (DM_NAMED_PIPE == eType)
  {
    iErrCode = dm_pipe_init(dm_default_loop(), &g_uStdin.stPipe, 0);
    if (DM_SUCCESS == iErrCode)
    {
      g_bStdinUp = DM_TRUE;
      iErrCode = dm_pipe_open(&g_uStdin.stPipe, 0);
    }
  }
  else if (DM_TTY == eType)
  {
    iErrCode = dm_tty_init(dm_default_loop(), &g_uStdin.stTty, 0, 1);
    g_bStdinUp = (DM_SUCCESS == iErrCode) ? DM_TRUE : DM_FALSE;
  }
  else
  {
    DM_LOG_WARN("stdin is not a pipe or tty, cannot watch for it closing");
    return DM_SUCCESS;
  }
  if (DM_SUCCESS == iErrCode)
  {
    iErrCode = dm_read_start((dm_stream_st *)&g_uStdin, dm__on_stdin_alloc, dm__on_stdin_read);
  }
  if (DM_SUCCESS != iErrCode)
  {
    DM_LOG_WARN("cannot watch stdin: %s", dm_errno_str(iErrCode));
    dm__stdin_close();
  }
  return DM_SUCCESS;
}

/* ============================ 启动号、运行文件、就绪行 ============================ */

static void dm__boot_make(void)
{
  static const char acHex[] = "0123456789abcdef";
  uint8 au8Rand[8] = {0};
  uint uiIdx = 0;

  if (DM_SUCCESS != dm_random(NULL, NULL, au8Rand, sizeof(au8Rand), 0, NULL))
  {
    uint64 u64Now = dm_hrtime(); /* 系统随机数拿不到（极少见）：拿时间凑，只是用来分辨两次启动 */
    memcpy(au8Rand, &u64Now, sizeof(au8Rand));
  }
  for (uiIdx = 0; uiIdx < sizeof(au8Rand); uiIdx++)
  {
    g_acBoot[uiIdx * 2u] = acHex[au8Rand[uiIdx] >> 4];
    g_acBoot[uiIdx * 2u + 1u] = acHex[au8Rand[uiIdx] & 0x0Fu];
  }
  g_acBoot[16] = '\0';
  return;
}

const char *dm__boot_hex(void)
{
  if ('\0' == g_acBoot[0])
  {
    dm__boot_make();
  }
  return g_acBoot;
}

/* 文件路径一律按 UTF-8：都走 libuv 同步文件调用（Windows 上内部转为宽字符） */
/* {"pid":..,"boot":"..","link":..,"http":..}，运行文件再加 "tokens":{角色名:口令}（只放标了
 * bInRunFile 的） */
typedef struct dm__jbuf
{
  char *pcBuf;
  size_t uLen;
  size_t uCap;
  bool_t bOom;
} dm__jbuf_st;

static void dm__jbuf_put(io dm__jbuf_st *pstJ, in const char *pcData, in size_t uN)
{
  if (pstJ->bOom || (0u == uN))
  {
    return;
  }
  if ((uN > SIZE_MAX - 1u) || (pstJ->uLen > SIZE_MAX - 1u - uN))
  {
    pstJ->bOom = DM_TRUE;
    return;
  }
  if (pstJ->uLen + uN + 1u > pstJ->uCap)
  {
    size_t uNeed = pstJ->uLen + uN + 1u;
    size_t uNew = (0u != pstJ->uCap) ? pstJ->uCap : 256u;
    char *pcNew = NULL;
    while (uNew < uNeed)
    {
      if (uNew > SIZE_MAX / 2u)
      {
        uNew = uNeed;
        break;
      }
      uNew *= 2u;
    }
    pcNew = (NULL == pstJ->pcBuf) ? (char *)dm_malloc_tag(uNew, DM_MEM_TAG_UV_OTHER)
                                  : (char *)dm_realloc_tag(pstJ->pcBuf, uNew);
    if (NULL == pcNew)
    {
      pstJ->bOom = DM_TRUE;
      return;
    }
    pstJ->pcBuf = pcNew;
    pstJ->uCap = uNew;
  }
  memcpy(pstJ->pcBuf + pstJ->uLen, pcData, uN);
  pstJ->uLen += uN;
  pstJ->pcBuf[pstJ->uLen] = '\0';
  return;
}

static void dm__jbuf_str(io dm__jbuf_st *pstJ, in const char *pcStr)
{
  const unsigned char *pu8P = (const unsigned char *)pcStr;
  char acEsc[8] = {0};

  dm__jbuf_put(pstJ, "\"", 1u);
  for (; (NULL != pu8P) && (0u != *pu8P); pu8P++)
  {
    unsigned char uc = *pu8P;
    if ('"' == uc)
    {
      dm__jbuf_put(pstJ, "\\\"", 2u);
    }
    else if ('\\' == uc)
    {
      dm__jbuf_put(pstJ, "\\\\", 2u);
    }
    else if ('\b' == uc)
    {
      dm__jbuf_put(pstJ, "\\b", 2u);
    }
    else if ('\f' == uc)
    {
      dm__jbuf_put(pstJ, "\\f", 2u);
    }
    else if ('\n' == uc)
    {
      dm__jbuf_put(pstJ, "\\n", 2u);
    }
    else if ('\r' == uc)
    {
      dm__jbuf_put(pstJ, "\\r", 2u);
    }
    else if ('\t' == uc)
    {
      dm__jbuf_put(pstJ, "\\t", 2u);
    }
    else if (uc < 0x20u)
    {
      int iN = snprintf(acEsc, sizeof(acEsc), "\\u%04x", (unsigned)uc);
      if (iN > 0)
      {
        dm__jbuf_put(pstJ, acEsc, (size_t)iN);
      }
    }
    else
    {
      char c = (char)uc;
      dm__jbuf_put(pstJ, &c, 1u);
    }
  }
  dm__jbuf_put(pstJ, "\"", 1u);
  return;
}

static void dm__run_json_tokens(io dm__jbuf_st *pstJ, in const dm_cfg_st *pstC)
{
  bool_t bFirst = DM_TRUE;
  uint uiIdx = 0;

  dm__jbuf_put(pstJ, ",\"tokens\":{", 11u);
  for (uiIdx = 0; (NULL != pstC->pastRole) && (uiIdx < pstC->uiRoleCount); uiIdx++)
  {
    if (!pstC->pastRole[uiIdx].bInRunFile || (NULL == pstC->pastRole[uiIdx].pcName) ||
        (NULL == pstC->pastRole[uiIdx].pcToken))
    {
      continue;
    }
    if (!bFirst)
    {
      dm__jbuf_put(pstJ, ",", 1u);
    }
    bFirst = DM_FALSE;
    dm__jbuf_str(pstJ, pstC->pastRole[uiIdx].pcName);
    dm__jbuf_put(pstJ, ":", 1u);
    dm__jbuf_str(pstJ, pstC->pastRole[uiIdx].pcToken);
  }
  dm__jbuf_put(pstJ, "}", 1u);
  return;
}

static char *dm__run_json(in const dm_cfg_st *pstC, in bool_t bTokens, out size_t *puLen)
{
  dm__jbuf_st stJ = {0};
  char acHead[192] = {0};
  int iLinkPort = 0;
  int iHttpPort = 0;
  int iN = 0;

  *puLen = 0u;
  dm__ext_get_ports(&iLinkPort, &iHttpPort);
  iN = snprintf(acHead, sizeof(acHead), "{\"link\":%d,\"http\":%d,\"boot\":\"%s\",\"pid\":%ld",
                iLinkPort, iHttpPort, dm__boot_hex(), dm__getpid());
  if ((iN <= 0) || ((size_t)iN >= sizeof(acHead)))
  {
    return NULL;
  }
  dm__jbuf_put(&stJ, acHead, (size_t)iN);
  if (bTokens)
  {
    dm__run_json_tokens(&stJ, pstC);
  }
  dm__jbuf_put(&stJ, "}", 1u);
  if (stJ.bOom || (NULL == stJ.pcBuf))
  {
    dm_free_tag(stJ.pcBuf);
    return NULL;
  }
  *puLen = stJ.uLen;
  return stJ.pcBuf;
}

static void dm__run_file_write(in const dm_cfg_st *pstC)
{
  char *pcText = NULL;
  size_t uLen = 0;
  int iPathLen = 0;

  if (NULL == pstC->pcDataDir)
  {
    return;
  }
  iPathLen = snprintf(g_acRunPath, sizeof(g_acRunPath), "%s/dm.run", pstC->pcDataDir);
  if ((iPathLen < 0) || ((size_t)iPathLen >= sizeof(g_acRunPath)))
  {
    DM_LOG_WARN("dm.run path too long for data dir");
    g_acRunPath[0] = '\0';
    return;
  }
  pcText = dm__run_json(pstC, DM_TRUE, &uLen);
  if ((NULL == pcText) || (DM_SUCCESS != dm__file_write_atomic(g_acRunPath, pcText, uLen)))
  {
    DM_LOG_WARN("cannot write %s", g_acRunPath);
    g_acRunPath[0] = '\0';
  }
  dm_free_tag(pcText);
  return;
}

void dm__run_file_remove(void)
{
  if ('\0' != g_acRunPath[0])
  {
    dm_fs_st stReq = {0};
    memset(&stReq, 0, sizeof(stReq));
    (void)dm_fs_unlink(NULL, &stReq, g_acRunPath, NULL);
    dm_fs_req_cleanup(&stReq);
    g_acRunPath[0] = '\0';
  }
  return;
}

/* 就绪行：一次原始的 write，不经 stdio 缓冲 */
static void dm__ready_line(in const dm_cfg_st *pstC)
{
  char acLine[256] = {0};
  char *pcText = NULL;
  size_t uLen = 0;
  int iLen = 0;

  pcText = dm__run_json(pstC, DM_FALSE, &uLen);
  if (NULL == pcText)
  {
    return;
  }
  iLen = snprintf(acLine, sizeof(acLine), "DM-READY %s\n", pcText);
  dm_free_tag(pcText);
  if ((iLen > 0) && ((size_t)iLen < sizeof(acLine)))
  {
#ifdef _WIN32
    (void)_write(1, acLine, (unsigned)iLen);
#else
    (void)!write(1, acLine, (size_t)iLen);
#endif
  }
  return;
}

/* ============================ dm_main_ex ============================ */

static void dm__reset_state(void)
{
  g_ePhase = DM__PHASE_NONE;
  g_pastMod = NULL;
  g_uiModCount = 0;
  g_uiModInited = 0;
  g_iExitCode = 0;
  g_iLastErr = DM_SUCCESS;
  g_bQuitting = DM_FALSE;
  g_pstCfgExt = NULL;
  g_uiExtInited = 0;
  g_bCfgExtInited = DM_FALSE;
  g_acRunPath[0] = '\0';
  dm__boot_make();
  g_uiHoldCount = 0;
  g_uiSignalCount = 0;
  memset(g_astHold, 0, sizeof(g_astHold));
  return;
}

/* 倒序调已经起来的模块的 pfFini */
static void dm__fini_modules(void)
{
  uint uiIdx = 0;
  for (uiIdx = g_uiModInited; uiIdx > 0u; uiIdx--)
  {
    if (NULL != g_pastMod[uiIdx - 1u].pfFini)
    {
      g_pastMod[uiIdx - 1u].pfFini();
    }
  }
  g_uiModInited = 0;
  return;
}

/* 起到一半失败、或者正常结束：把还开着的都关掉，转一圈让关闭回调跑完 */
static void dm__close_all(void)
{
  dm__pool_shutdown(); /* 先等待所有工作线程退出并跑完 pfOnThreadExit，再关可选组件 */
  dm__ext_on_end();    /* work 都停了：按逆序关闭所有可选组件 */
  dm__signals_close();
  dm__stdin_close();
  dm__timer_cancel_all();
  dm__event_destroy();
  dm__stat_fini();
  dm__link_table_destroy();
  dm__queue_close();
  if (g_bDrainTimerUp)
  {
    dm__close_if((dm_handle_st *)&g_stDrainTimer);
    g_bDrainTimerUp = DM_FALSE;
  }
  if (g_bWorkWaitTimerUp)
  {
    dm__close_if((dm_handle_st *)&g_stWorkWaitTimer);
    g_bWorkWaitTimerUp = DM_FALSE;
  }
  (void)dm_run(dm_default_loop(), DM_RUN_DEFAULT);
  dm__pool_destroy_sync();
  return;
}

/* 起到一半失败：排着队的 work 取消，在跑的最多等 DM_WORK_WAIT_MS（到点 _exit(2)，和拆除一样） */
static void dm__works_reap(void)
{
  if (0u == dm__work_inflight())
  {
    return;
  }
  dm__work_cancel_queued();
  if (g_bWorkWaitTimerUp)
  {
    (void)dm_timer_start(&g_stWorkWaitTimer, dm__on_work_wait, DM_WORK_WAIT_MS, 0);
  }
  while (dm__work_inflight() > 0u)
  {
    (void)dm_run(dm_default_loop(), DM_RUN_ONCE);
  }
  if (g_bWorkWaitTimerUp)
  {
    (void)dm_timer_stop(&g_stWorkWaitTimer);
  }
  return;
}

static int dm__startup_fail(in int iErr, in const char *pcWhat)
{
  g_iLastErr = iErr;
  DM_LOG_ERROR("startup failed at %s: %s", pcWhat, dm_errno_str(iErr));
  dm__check_set_in_init(DM_FALSE);
  g_ePhase = DM__PHASE_END;
  dm__works_reap(); /* 模块 init 里提交的 work：先收完 after，再调 fini */
  dm__fini_modules();
  dm__close_all();
  (void)dm_loop_close(dm_default_loop());
  dm_log_close();
  dm__lock_release();
  dm__errno_mod_clear();
  g_pstCfgExt = NULL;
  dm__check_unbind_main();
  g_ePhase = DM__PHASE_NONE;
  return 1;
}

/* 校验模块表（DD-00 §2.1 / AUD-10 / T-DM-12 / T-BND-03）：
 * u16ModID 必须在 [16, 93] 且全局唯一，pcName 非空，pastErrMap 合法并自动注册 */
static int dm__validate_modules(in const dm_module_st *pastMod, in uint uiCount)
{
  uint uiIdx = 0;
  uint uiPrev = 0;
  int iErr = DM_SUCCESS;

  dm__errno_mod_clear();
  if ((uiCount > 0u) && (NULL == pastMod))
  {
    DM_LOG_ERROR("module list is NULL with count %u", uiCount);
    return DM_INVALID;
  }
  for (uiIdx = 0; uiIdx < uiCount; uiIdx++)
  {
    if ((NULL == pastMod[uiIdx].pcName) || ('\0' == pastMod[uiIdx].pcName[0]))
    {
      DM_LOG_ERROR("module[%u] has empty pcName", uiIdx);
      dm__errno_mod_clear();
      return DM_INVALID;
    }
    if ((pastMod[uiIdx].u16ModID < DM_MOD_ID_APP_MIN) ||
        (pastMod[uiIdx].u16ModID > DM_MOD_ID_APP_MAX))
    {
      DM_LOG_ERROR("module %s u16ModID %u out of range [%u..%u]", pastMod[uiIdx].pcName,
                   (unsigned)pastMod[uiIdx].u16ModID, (unsigned)DM_MOD_ID_APP_MIN,
                   (unsigned)DM_MOD_ID_APP_MAX);
      dm__errno_mod_clear();
      return DM_RANGE;
    }
    for (uiPrev = 0; uiPrev < uiIdx; uiPrev++)
    {
      if (pastMod[uiPrev].u16ModID == pastMod[uiIdx].u16ModID)
      {
        DM_LOG_ERROR("duplicate u16ModID %u between %s and %s", (unsigned)pastMod[uiIdx].u16ModID,
                     pastMod[uiPrev].pcName, pastMod[uiIdx].pcName);
        dm__errno_mod_clear();
        return DM_EXISTS;
      }
    }
    if (NULL != pastMod[uiIdx].pastErrMap)
    {
      iErr = dm__errno_mod_register(pastMod[uiIdx].u16ModID, pastMod[uiIdx].pastErrMap);
      if (DM_SUCCESS != iErr)
      {
        DM_LOG_ERROR("module %s err map invalid: %s", pastMod[uiIdx].pcName, dm_errno_str(iErr));
        dm__errno_mod_clear();
        return iErr;
      }
    }
  }
  return DM_SUCCESS;
}

static int dm__startup_prepare(in const dm_cfg_st *pstC)
{
  int iErrCode = DM_SUCCESS;

  /* ① 忽略 SIGPIPE；记下主线程 */
#ifndef _WIN32
  (void)signal(SIGPIPE, SIG_IGN);
#endif
  dm_check_bind_main();

  /* ② 数据目录的独占锁：拿不到，别的文件一个字都不碰 */
  if (NULL != pstC->pcDataDir)
  {
    dm__mkdirs(pstC->pcDataDir);
    iErrCode = dm__lock_take(pstC->pcDataDir, pstC->pcLockName);
    if (DM_SUCCESS != iErrCode)
    {
      g_iLastErr = iErrCode;
      DM_LOG_ERROR("data dir %s is in use by another core (%s)", pstC->pcDataDir,
                   dm_errno_str(iErrCode)); /* 还没起日志，只写标准错误 */
      dm__errno_mod_clear();
      g_pstCfgExt = NULL;
      dm__check_unbind_main();
      return iErrCode;
    }
  }

  /* ③ 起日志：从这里开始所有输出都进日志 */
  iErrCode = dm_log_init(pstC->pcLogDir, (NULL != pstC->pcLogName) ? pstC->pcLogName : "dm",
                         pstC->bLogLevelSet ? (dm_log_level_et)pstC->u8LogLevel : DM_LOG_LEVEL_INFO,
                         (0u != pstC->u16LogMaxMb) ? (int)pstC->u16LogMaxMb : 10,
                         (0u != pstC->u16LogKeep) ? (int)pstC->u16LogKeep : 5);
  if (DM_SUCCESS != iErrCode)
  {
    g_iLastErr = iErrCode;
    dm__lock_release();
    dm__errno_mod_clear();
    g_pstCfgExt = NULL;
    dm__check_unbind_main();
    return iErrCode;
  }
  if (pstC->bLogLevelSet && (pstC->u8LogLevel > (uint8)DM_LOG_LEVEL_NONE))
  {
    DM_LOG_WARN("u8LogLevel %u is out of range, using INFO", (unsigned)pstC->u8LogLevel);
    dm_log_set_level(DM_LOG_LEVEL_INFO);
  }
  DM_LOG_INFO("dm %s (libuv %s) starting", dm_version_string(), dm_uv_version_string());
  return DM_SUCCESS;
}

static int dm__startup_core(in const dm_cfg_st *pstC, out const char **ppcFailStep)
{
  int iErrCode = DM_SUCCESS;

  /* ④ 总队列、统计、内部定时器与登记表（微内核自包含，零可选库依赖） */
  dm__queue_set_max(pstC->u32QueueMax);
  iErrCode = dm__queue_init();
  if (DM_SUCCESS != iErrCode)
  {
    *ppcFailStep = "queue";
    return iErrCode;
  }
  iErrCode = dm__pool_init(pstC);
  if (DM_SUCCESS != iErrCode)
  {
    *ppcFailStep = "pool";
    return iErrCode;
  }
  iErrCode = dm__stat_init();
  if (DM_SUCCESS != iErrCode)
  {
    *ppcFailStep = "stat";
    return iErrCode;
  }
  iErrCode = dm_timer_init(dm_default_loop(), &g_stDrainTimer);
  if (DM_SUCCESS != iErrCode)
  {
    *ppcFailStep = "timers";
    return iErrCode;
  }
  g_bDrainTimerUp = DM_TRUE;
  dm_unref((dm_handle_st *)&g_stDrainTimer); /* 这两个不该让循环活着 */
  iErrCode = dm_timer_init(dm_default_loop(), &g_stWorkWaitTimer);
  if (DM_SUCCESS != iErrCode)
  {
    *ppcFailStep = "timers";
    return iErrCode;
  }
  g_bWorkWaitTimerUp = DM_TRUE;
  dm_unref((dm_handle_st *)&g_stWorkWaitTimer);
  iErrCode = dm__link_table_init();
  if (DM_SUCCESS != iErrCode)
  {
    *ppcFailStep = "link table";
    return iErrCode;
  }

  /* ⑤ 可选组件启动前钩子（db / child / net 等按注册顺序装配） */
  iErrCode = dm__ext_before_init(pstC);
  if (DM_SUCCESS != iErrCode)
  {
    *ppcFailStep = "ext";
    return iErrCode;
  }
  return DM_SUCCESS;
}

static int dm__startup_modules(in const dm_module_st *pastMod)
{
  uint uiIdx = 0;
  int iErrCode = DM_SUCCESS;

  /* ⑥ 按清单顺序调各模块的 pfInit */
  g_ePhase = DM__PHASE_INIT;
  dm__check_set_in_init(DM_TRUE);
  for (uiIdx = 0; uiIdx < g_uiModCount; uiIdx++)
  {
    if (NULL == pastMod[uiIdx].pfInit)
    {
      g_uiModInited++;
      continue;
    }
    iErrCode = pastMod[uiIdx].pfInit();
    if (DM_SUCCESS != iErrCode)
    {
      DM_LOG_ERROR("module %s init failed: %s", pastMod[uiIdx].pcName, dm_errno_str(iErrCode));
      return iErrCode;
    }
    g_uiModInited++;
  }
  dm__check_set_in_init(DM_FALSE);
  dm__ext_after_init(); /* init 后收尾（如关闭主线程 DB 连接） */
  return DM_SUCCESS;
}

static void dm__shutdown_core(void)
{
  int iLoopErr = DM_SUCCESS;

  /* 结束：倒序 pfFini，关掉剩下的，关日志 */
  dm__check_set_in_init(DM_FALSE);
  g_ePhase = DM__PHASE_END;
  dm__fini_modules();
  dm__close_all();
  iLoopErr = dm_loop_close(dm_default_loop());
  if (DM_SUCCESS != iLoopErr)
  {
    DM_LOG_ERROR("dm_loop_close failed: %s", dm_errno_str(iLoopErr));
  }
  dm__run_file_remove();
  DM_LOG_INFO("exit code %d", g_iExitCode);
  dm_log_close();
  dm__lock_release();
  dm__errno_mod_clear();
  g_pstCfgExt = NULL;
  dm__check_unbind_main();
  g_ePhase = DM__PHASE_NONE;
  return;
}

int dm_main_ex(in int argc, in char **argv, in const dm_cfg_st *pstCfg,
               in const dm_module_st *pastMod, in uint uiCount)
{
  static const dm_cfg_st s_stZero = {0};
  const dm_cfg_st *pstC = (NULL != pstCfg) ? pstCfg : &s_stZero;
  const char *pcFailStep = "core";
  int iErrCode = DM_SUCCESS;
  int iLoopErr = DM_SUCCESS;
  (void)argc;
  (void)argv;

  dm__reset_state();
  iErrCode = dm__validate_modules(pastMod, uiCount);
  if (DM_SUCCESS != iErrCode)
  {
    g_iLastErr = iErrCode;
    return 1;
  }
  g_pastMod = pastMod;
  g_uiModCount = (NULL != pastMod) ? uiCount : 0u;
  g_pstCfgExt = pstC->pstHooks;
  g_u32DrainMs = (0u != pstC->u32DrainMs) ? pstC->u32DrainMs : DM__DRAIN_MS_DEFAULT;

  iErrCode = dm__startup_prepare(pstC);
  if (DM_SUCCESS != iErrCode)
  {
    return 1;
  }
  iErrCode = dm__startup_core(pstC, &pcFailStep);
  if (DM_SUCCESS != iErrCode)
  {
    return dm__startup_fail(iErrCode, pcFailStep);
  }
  iErrCode = dm__startup_modules(pastMod);
  if (DM_SUCCESS != iErrCode)
  {
    return dm__startup_fail(iErrCode, "module init");
  }

  /* ⑦ 信号、标准输入 */
  iErrCode = dm__signals_open();
  if (DM_SUCCESS != iErrCode)
  {
    return dm__startup_fail(iErrCode, "signals");
  }
  if (pstC->bExitOnStdinEof)
  {
    (void)dm__stdin_open();
  }

  /* ⑧ 就绪事件：向订阅者投递 DM_EV_READY；写运行文件、就绪行；进入主循环 */
  g_ePhase = DM__PHASE_RUN;
  (void)dm_event_publish(DM_EV_READY, NULL, 0u);
  dm__run_file_write(pstC);
  if (pstC->bReadyLine)
  {
    dm__ready_line(pstC);
  }
  DM_LOG_INFO("running, %u module(s)", g_uiModInited);
  iLoopErr = dm_run(dm_default_loop(), DM_RUN_DEFAULT);
  if (DM_SUCCESS != iLoopErr)
  {
    DM_LOG_ERROR("dm_run failed: %s", dm_errno_str(iLoopErr));
  }

  dm__shutdown_core();
  return g_iExitCode;
}

int dm_main(int argc, char **argv)
{
  return dm_main_ex(argc, argv, NULL, NULL, 0u);
}

int dm_main_last_err(void)
{
  return g_iLastErr;
}

#ifdef __cplusplus
}
#endif
