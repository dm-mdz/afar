/**
 ******************************************************************************
 * @file    dmcmd_mcp_int.h
 * @brief   dmcmd mcp 并发异步中继内部共享结构与原语声明（DD-02 §3.4）。
 ******************************************************************************
 */
#ifndef DMCMD_MCP_INT_H
#define DMCMD_MCP_INT_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "dmcmd_internal.h"

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef _WIN32
typedef uintptr_t dmcmd_mcp_sock_t;
#define DMCMD_MCP_SOCK_BAD ((dmcmd_mcp_sock_t)(~(uintptr_t)0))
typedef CRITICAL_SECTION dmcmd_mcp_mutex_t;
typedef CONDITION_VARIABLE dmcmd_mcp_cond_t;
typedef HANDLE dmcmd_mcp_thread_t;
#else
typedef int dmcmd_mcp_sock_t;
#define DMCMD_MCP_SOCK_BAD (-1)
typedef pthread_mutex_t dmcmd_mcp_mutex_t;
typedef pthread_cond_t dmcmd_mcp_cond_t;
typedef pthread_t dmcmd_mcp_thread_t;
#endif

typedef struct dmcmd_mcp_ctx dmcmd_mcp_ctx_st;

typedef struct dmcmd_mcp_slot
{
  dmcmd_mcp_ctx_st *pstCtx;
  dmcmd_mcp_thread_t stThr;
  bool_t bActive;
  bool_t bDone;
  char *pcReqLine;
  size_t uReqLen;
} dmcmd_mcp_slot_st;

struct dmcmd_mcp_ctx
{
  char acHost[256];
  int iPort;
  char acPath[1024];
  char acAuth[2048];
  bool_t bHasAuth;
  uint32 u32IdleMs;
  dmcmd_mcp_mutex_t stOutMtx;
  dmcmd_mcp_mutex_t stPoolMtx;
  dmcmd_mcp_cond_t stPoolCond;
  uint32 u32Inflight;
  dmcmd_mcp_slot_st astSlots[DMCMD_MCP_INFLIGHT];
};

dmcmd_mcp_sock_t dmcmd_mcp_connect(dm_in const char *pcHost, dm_in int iPort, dm_in uint32 u32Ms);
void dmcmd_mcp_sock_close(dm_in dmcmd_mcp_sock_t iSock);
int dmcmd_mcp_send_all(dm_in dmcmd_mcp_sock_t iSock, dm_in const void *pvBuf, dm_in size_t uLen,
                       dm_in int iTimeoutMs);
int dmcmd_mcp_recv_idle(dm_in dmcmd_mcp_sock_t iSock, dm_out void *pvBuf, dm_in size_t uCap,
                        dm_in int iIdleMs);

void dmcmd_mcp_write_json_line(dm_io dmcmd_mcp_ctx_st *pstCtx, dm_in const char *pcData,
                               dm_in size_t uLen);
void dmcmd_mcp_reply_rpc_err(dm_io dmcmd_mcp_ctx_st *pstCtx, dm_in const char *pcReqLine,
                             dm_in size_t uReqLen, dm_in int iCode, dm_in const char *pcMsg);
void dmcmd_mcp_handle_one(dm_io dmcmd_mcp_ctx_st *pstCtx, dm_in const char *pcReq,
                          dm_in size_t uReqLen);

#ifdef __cplusplus
}
#endif

#endif /* DMCMD_MCP_INT_H */
