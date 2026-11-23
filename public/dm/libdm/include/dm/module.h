/**
 ******************************************************************************
 * @file    module.h
 * @author  MDM
 * @version 0.0.0.2
 * @brief   模块治理与生命周期（DD-00 §2.1 / DD-01 §2.1）：
 *          u16ModID 派生宏、dm_module_st、dm_ext_hooks_st、dm_cfg_st、dm_main_ex。
 ******************************************************************************
 */
#ifndef DM_MODULE_H
#define DM_MODULE_H

#include <dm.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ---- 全局模块号与自动派生规则（DD-00 §2.1，AUD-10） ---- */
#define DM_MOD_ID_APP_MIN 16u
#define DM_MOD_ID_APP_MAX 93u

/* linkID（16 bit）：高 8 位为模块号，低 8 位为模块内子号（0x00 保留） */
#define DM_MOD_LINKID(mod_id, sub_id)                                                              \
  ((uint16)(((uint16)(mod_id) << 8) | ((uint16)(sub_id) & 0xFFu)))

/* 内存标签（0 ~ 1023）：0~399 归 libdm 内核，400~1023 归应用模块（每模块 8 个子标签） */
#define DM_MEM_TAG_APP_BASE 400u
#define DM_MOD_MEMTAG(mod_id, sub_idx)                                                             \
  ((int)(DM_MEM_TAG_APP_BASE + (((uint)(mod_id) - 16u) * 8u) + ((uint)(sub_idx) & 0x07u)))

/* 应用模块错误码：-(200000 + (mod_id - 16) * 1000 + sub_err) */
#define DM_MOD_ERR(mod_id, sub_err) (-(200000 + (((int)(mod_id) - 16) * 1000) + (int)(sub_err)))

/* 模块错误码文案表条目（数组以 {0, NULL} 结尾） */
typedef struct dm_err_entry
{
  int iCode;
  const char *pcMsg;
} dm_err_entry_st;

/* 句柄不透明：只在 pfDrain 里交给 dm_drain_hold，别存起来 */
typedef struct dm_drain dm_drain_st;

typedef struct dm_module
{
  uint16 u16ModID;                           /* 唯一模块号：应用模块 16..93 */
  const char *pcName;                        /* 日志与诊断名，如 "bf_proc" */
  int (*pfInit)(void);                       /* 初始化：登记处理项、接口、定时器、事件 */
  void (*pfDrain)(io dm_drain_st *pstDrain); /* 优雅收尾（可 NULL） */
  void (*pfFini)(void);                      /* 倒序清理（可 NULL） */
  const dm_err_entry_st *pastErrMap;         /* 本模块错误码文案表（{0, NULL} 结尾，可 NULL） */
} dm_module_st;

typedef struct dm_role
{
  uint32 u32Bit;       /* 这个角色的位。接口登记时写明哪些位能调 */
  const char *pcName;  /* 握手时报的名字，例："ui" */
  const char *pcToken; /* 这个角色的口令；应用在调 dm_main_ex 之前备好 */
  bool_t bInRunFile;   /* 口令要不要写进运行文件 */
} dm_role_st;

struct dm_cfg;
struct dm_https_cfg; /* 前向声明：libdm_core 零反向依赖 dm/https.h */

/* 可选组件生命周期钩子（解除 libdm_core 对 net/db/child 的强链接依赖，AUD-02 / AUD-01） */
typedef struct dm_ext_hooks
{
  int (*pfBeforeInit)(in const struct dm_cfg *pstCfg);
  void (*pfAfterInit)(void);
  void (*pfOnDrain)(void);
  void (*pfOnTeardown)(void);
  void (*pfOnEnd)(void);
  void (*pfAfterWork)(in uint32 u32StatKey);
  void (*pfOnThreadExit)(void);
  void (*pfGetPorts)(out int *piLinkPort, out int *piHttpPort);
  uint32 (*pfChildCount)(void);
  int (*pfReplyTake)(in const void *pvMsg, out void *pvReply);
  int (*pfReplySend)(in const void *pvReply, in int iStatus, in const void *pvBody,
                     in uint32 u32Len);
} dm_ext_hooks_st;

typedef struct dm_cfg
{
  const char *pcLogDir;    /* 日志目录；NULL = 只打到标准错误 */
  const char *pcLogName;   /* 日志文件名前缀；NULL = "dm" */
  uint8 u8LogLevel;        /* 起始级别，dm_log_level_et；默认 INFO */
  bool_t bLogLevelSet;     /* u8LogLevel 是不是有意设的；假 = 用 INFO */
  uint16 u16LogMaxMb;      /* 单个文件多大轮换；0 = 10 */
  uint16 u16LogKeep;       /* 留几个旧文件；0 = 5 */
  const char *pcDataDir;   /* 数据目录（锁/端口/运行/子进程登记文件）；NULL = 都不要 */
  const char *pcDbPath;    /* 数据库文件；NULL = 不开库 */
  const char *pcPtyHelper; /* dm-pty 路径；NULL = 同目录查找 */
  uint uiWorkThreads;      /* 线程池线程数（兼容旧配置）；0 = 默认 8，最多 64 */
  uint32 u32FrameMax;      /* 单帧上限；0 = 16MB */
  uint32 u32QueueMax;      /* 总队列外部入队上限；0 = 64MB */
  uint32 u32DrainMs;       /* 收尾最多等多久；0 = 5000（Windows 2500） */
  bool_t bExitOnStdinEof;  /* 标准输入断了就退出 */
  bool_t bReadyLine;       /* 起来以后往标准输出写一行就绪 */
  const dm_role_st *pastRole;
  uint uiRoleCount;
  const char *const *ppcOrigin;
  uint16 u16PoolMin;               /* dm_pool 最小线程数（W3），0 = 默认 */
  uint16 u16PoolMax;               /* dm_pool 最大线程数（W3），0 = 默认 */
  uint32 u32PoolIdleMs;            /* dm_pool 空闲缩容超时（W3），0 = 默认 */
  const dm_ext_hooks_st *pstHooks; /* 可选组件单组钩子（可与 dm_ext_register 组合使用） */
  const char *pcLockName;          /* 数据目录锁文件名；NULL = "dm.lock"（Afar 设 "agent.lock"） */
  bool_t bDisableLocalHttp;        /* DM_TRUE = 不监听 127.0.0.1 明文 HTTP 口 */
  const struct dm_https_cfg *pstHttps; /* 非 NULL 时由 dm_https_hooks() 启用局域网 TLS HTTPS */
} dm_cfg_st;

/* 注册可选组件钩子（幂等去重，供 libdm_net / libdm_db / libdm_child 装配，零环依赖） */
DM_API int dm_ext_register(in const dm_ext_hooks_st *pstHooks);

/* 清空已注册的可选组件钩子（测试隔离用） */
DM_API void dm_ext_clear(void);

DM_API int dm_main_ex(in int argc, in char **argv, in const dm_cfg_st *pstCfg,
                      in const dm_module_st *pastMod, in uint uiCount);

DM_API int dm_main(in int argc, in char **argv);

DM_API int dm_main_last_err(void);

DM_API void dm_quit(in int iCode);

DM_API int dm_stdin_line(out char *pcBuf, in uint uiSize);

DM_API int dm_token_make(out char *pcBuf, in uint uiSize);

DM_API int dm_drain_hold(io dm_drain_st *pstDrain);

DM_API void dm_drain_release(in int iHold);

/* 原子写文件（0600，唯一临时文件 + 写满循环 + fsync + 原子替换 + 失败清理，路径 UTF-8） */
DM_API int dm_fs_write_atomic(in const char *pcPath, in const void *pvData, in size_t uLen);

#ifdef __cplusplus
}
#endif

#endif /* DM_MODULE_H */
