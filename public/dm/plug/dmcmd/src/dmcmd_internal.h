/**
 ******************************************************************************
 * @file    dmcmd_internal.h
 * @author  MDM
 * @version 0.0.1.0
 * @brief   dmcmd 内部契约（DD-02 §3，CLI-01 ~ CLI-08，FIX-08，AUD-03/14）。
 *
 *          退出码铁律（DD-02 §3.1）：
 *            0：成功
 *            1：核心返回业务/系统错误或网络/超时错误
 *            2：本地参数错误（含超 64 参数截断拦截）
 ******************************************************************************
 */
#ifndef DMCMD_INTERNAL_H
#define DMCMD_INTERNAL_H

#include <dmc_client.h>
#include <dm/type.h>
#include <yyjson.h>

#include <stdio.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DMCMD_EXIT_OK 0
#define DMCMD_EXIT_ERR 1
#define DMCMD_EXIT_USAGE 2

#define DMCMD_MAX_ARGS 64
#define DMCMD_DEFAULT_TIMEOUT_MS 15000u
#define DMCMD_MCP_LINE_MAX (16u * 1024u * 1024u)
#define DMCMD_MCP_RESP_MAX (16u * 1024u * 1024u)
#define DMCMD_MCP_INFLIGHT 32u

typedef enum dmcmd_body_mode
{
  DMCMD_BODY_NONE = 0,
  DMCMD_BODY_JSON,
  DMCMD_BODY_FILE,
  DMCMD_BODY_STDIN
} dmcmd_body_mode_et;

typedef struct dmcmd_opt
{
  const char *pcTarget;  /* NULL / "local://" / "tcp://host:port" / ... */
  const char *pcDataDir; /* local:// 数据目录 */
  const char *pcToken;   /* 显式口令 */
  const char *pcRole;    /* 握手角色 */
  uint32 u32TimeoutMs;   /* 单次调用超时（默认 15000ms） */
  bool_t bHasTimeout;    /* 用户是否显式传入了全局 --timeout */
  bool_t bJson;          /* 全链路 JSON 信封输出 */
  bool_t bRaw;           /* 配合 --json 输出裸 JSON 数据 */
  dmcmd_body_mode_et eBodyMode;
  const char *pcBodyArg; /* --body-json 文本 或 --body-file 路径 */
} dmcmd_opt_st;

/* ============================ 参数、输出与帮助（dmcmd_args.c） ============================ */

/* 输出统一错误（--json 下打印标准信封到 stdout；否则输出可读错误到 stderr） */
void dmcmd_emit_error(dm_in bool_t bJson, dm_in int iStatus, dm_in const char *pcErr,
                      dm_in const char *pcDetail);

/* 输出成功结果（--json 下包 {"ok":true,"status":0,"data":...}；--raw 裸输出；否则人类表格） */
void dmcmd_emit_ok(dm_in bool_t bJson, dm_in bool_t bRaw, dm_in const char *pcBody,
                   dm_in size_t uLen);

/* 从 --body-json / --body-file / --body-stdin 读取并校验 JSON 请求体（调用方 free） */
int dmcmd_load_body_opt(dm_in const dmcmd_opt_st *pstOpt, dm_out char **ppcBody);

typedef struct dmcmd_args_req
{
  const dmcmd_opt_st *pstOpt; /* 全局 CLI 选项 */
  const char *pcName;         /* 接口名 */
  const char *pcSchema;       /* 接口参数 JSON Schema（可 NULL） */
  int iArgc;                  /* 参数个数 */
  char **ppcArgv;             /* 参数数组 */
} dmcmd_args_req_st;

/* 按接口 JSON Schema 将命令行参数拼成 JSON（出错时通过 dmcmd_emit_error 报错并返 DMCMD_EXIT_USAGE）
 */
int dmcmd_args_build(dm_in const dmcmd_args_req_st *pstReq, dm_out char **ppcBody);

/* 单个接口的人类可读帮助 */
void dmcmd_args_help(dm_io FILE *pstOut, dm_in const char *pcName, dm_in const char *pcDesc,
                     dm_in const char *pcSchema);

/* 读取本地运行文件获取 HTTP 端口（供 dmcmd mcp 使用）；失败返回 -1 */
int dmcmd_read_http_port(dm_in const char *pcDataDir);

/* ============================ 内置子命令与工具（dmcmd_cmd.c） ============================ */

bool_t dmcmd_parse_u64(dm_in const char *pcStr, dm_out uint64 *pu64Out);
void dmcmd_usage(dm_io FILE *pstOut);
int dmcmd_cmd_run(dm_io dmc_client_st *pstCli, dm_in const dmcmd_opt_st *pstOpt, dm_in int iArgc,
                  dm_in char **ppcArgv);
int dmcmd_cmd_watch(dm_io dmcmd_opt_st *pstOpt, dm_in int iArgc, dm_in char **ppcArgv);

/* ============================ 并发异步 MCP 中继（dmcmd_mcp.c） ============================ */

int dmcmd_run_mcp(dm_in const dmcmd_opt_st *pstOpt);

#ifdef __cplusplus
}
#endif

#endif /* DMCMD_INTERNAL_H */
