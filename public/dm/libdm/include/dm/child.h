/**
 ******************************************************************************
 * @file    child.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   子进程托管（详设 2.6）。只在主线程调。
 *
 *          输出交给 uiOutLinkID（来源 DM_SRC_CHILD_OUT，LINES 一行一条、不含 \n；CHUNKS、PTY
 * 读到多少 交多少）；结束交给 uiExitLinkID（来源 DM_SRC_CHILD_EXIT，负载是 dm_child_exit_st
 * 后面跟着 标准错误的最后一段），只交一次，并且在全部输出之后。u64Tag 原样放进这两类消息。
 *          子进程的编号在它的结束消息交出去以后就作废，之后对它调下面任何一个都回 DM_CONN_GONE。
 *
 *          Mac、Linux：子进程自成一个会话和进程组，整树结束给整组发 SIGTERM，DM_KILL_GRACE_MS 后发
 *          SIGKILL。伪终端经小程序 dm-pty 拉（和本程序同一个目录，或 dm_cfg.pcPtyHelper 指定）。
 *          Windows：放进自己的作业对象，整树结束关作业；伪终端通过独立 ConPTY 适配层
 *          （child_conpty_win.c，DD-01 / AUD-09）实现，不可用或被禁用时自动回退普通管道。
 *          拉起、结束各往数据目录的 children.reg 追加一行；下次启动按它清残留（DD-01）。
 ******************************************************************************
 */
#ifndef DM_CHILD_H
#define DM_CHILD_H

#include <dm.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum dm_child_io
{
  DM_CHILD_LINES = 0, /* 三条管道，标准输出按行切 */
  DM_CHILD_CHUNKS,    /* 三条管道，标准输出按块原样交 */
  DM_CHILD_PTY        /* 伪终端，按块原样交（终端的 shell） */
} dm_child_io_et;

typedef struct dm_child_spec
{
  const char *pcFile;      /* 程序路径（调用方已经找好） */
  char **ppcArgv;          /* 以 NULL 结尾 */
  char **ppcEnv;           /* 完整的环境变量表，以 NULL 结尾（白名单由调用方做好）；NULL = 空环境。
                            * Windows 上 libuv 会补上表里没有的这几个（从核心自己的环境抄）：
                            * HOMEDRIVE HOMEPATH LOGONSERVER PATH SYSTEMDRIVE SYSTEMROOT TEMP
                            * USERDOMAIN USERNAME USERPROFILE WINDIR，不然很多程序起不来。
                            * 不想让子进程看到的，在表里显式给一个值 */
  const char *pcCwd;       /* NULL = 核心的当前目录 */
  uint8 u8Io;              /* dm_child_io_et */
  uint16 u16Cols, u16Rows; /* 只有 PTY 用；0 = 80x24 */
  uint32 u32MaxLine;       /* 只有 LINES 用：一行最长；0 = DM_LINE_MAX（64MB） */
  uint32 u32ErrKeep;       /* 标准错误留最后多少字节，随结束消息交；0 = 64KB */
  uint uiOutLinkID;        /* 输出交给谁。两个编号没登记过当场回 DM_LINK_ERR_NOT_FOUND，不拉起 */
  uint uiExitLinkID;       /* 结束交给谁 */
  uint64 u64Tag;           /* 原样放进这两类消息的 u64Tag */
  const char *pcOwner;     /* 写进登记文件的「谁拉的」，比如 "proc"；NULL = "-" */
} dm_child_spec_st;

typedef struct dm_child_exit /* DM_SRC_CHILD_EXIT 的负载 */
{
  int64 i64Status;  /* 退出码 */
  int iSignal;      /* 被哪个信号结束；0 = 正常退出 */
  int iReason;      /* DM_SUCCESS；DM_TIMEOUT（退出后 DM_CHILD_EOF_MS 还没读到尽头，DM 关了管道）；
                       DM_IO（读管道出了不是「读到尽头」的错，DM 关了管道） */
  uint32 u32ErrLen; /* 后面跟着标准错误的最后一段 */
} dm_child_exit_st;

/* 拉起；*pu32Child 带出编号。收尾、拆除阶段回 DM_SHUTTING_DOWN */
DM_API int dm_child_spawn(dm_in const dm_child_spec_st *pstSpec, dm_out uint32 *pu32Child);
/* DM 拷一份进写队列，按顺序写；队列超过 DM_CHILD_WRITE_MAX 回 DM_BUSY（已经在队列里的不丢） */
DM_API int dm_child_write(dm_in uint32 u32Child, dm_in const void *pvData, dm_in uint32 u32Len);
/* 关它的输入（写完队列里的再关）；PTY 是发一个 Ctrl-D */
DM_API int dm_child_close_stdin(dm_in uint32 u32Child);
/* 只有 PTY；别的回 DM_INVALID */
DM_API int dm_child_resize(dm_in uint32 u32Child, dm_in uint16 u16Cols, dm_in uint16 u16Rows);
DM_API int dm_child_pause(dm_in uint32 u32Child); /* 暂停读它的输出（背压） */
DM_API int dm_child_resume(dm_in uint32 u32Child);
/* 先关输入，限时等它自己退，到点整树结束 */
DM_API int dm_child_stop(dm_in uint32 u32Child, dm_in uint32 u32GraceMs);
DM_API int dm_child_kill_tree(dm_in uint32 u32Child); /* 立即整树结束 */
DM_API int dm_child_pid(dm_in uint32 u32Child, dm_out int *piPid);

struct dm_ext_hooks;
DM_API const struct dm_ext_hooks *dm_child_hooks(void);

#ifdef __cplusplus
}
#endif

#endif /* DM_CHILD_H */
