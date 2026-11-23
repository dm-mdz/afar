/**
 ******************************************************************************
 * @file    dmc_client_int.h
 * @brief   dmc 上层会话 SDK 内部共享结构与辅助声明。
 ******************************************************************************
 */
#ifndef DMC_CLIENT_INT_H
#define DMC_CLIENT_INT_H

#include <dmc_client.h>
#include <dm/errno.h>
#include <yyjson.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DMC__PUSH_TOPIC_MAX 32u

struct dmc_client
{
  char acTarget[512];
  char acDataDir[512];
  char acToken[256];
  char acRole[64];
  char acPeerID[64];
  char acScheme[32];
  bool_t bHasToken;
  bool_t bHasRole;
  uint32 u32ConnectTimeoutMs;
  uint32 u32DefaultTimeoutMs;
  uint32 u32PingIntervalMs;
  bool_t bAutoReconnect;
  bool_t bWantPush;
  bool_t bHasSinceSeq;
  uint64 u64LastSeq;
  char acBoot[32];
  int iHttpPort;
  char aacPushTopics[DMC__PUSH_TOPIC_MAX][64];
  uint uiPushTopicCount;
  void (*pfOnPush)(in void *pvUser, in const dmc_push_st *pstPush);
  void (*pfOnCall)(in void *pvUser, in uint16 u16Func, in uint32 u32ReqID, in const uint8 *pu8Body,
                   in uint32 u32Len);
  void *pvUser;

  dmc_transport_vtbl_st stVtbl;
  bool_t bHasVtbl;
  void *pvHandle;
  dmc_st *pstDmc;
  uint64 u64LastPingMs;
  uint64 u64LastReconnMs;
  char *pcIfacesJson;
  yyjson_doc *pstIfacesDoc;
};

bool_t dmc__transport_copy(in const char *pcScheme, out dmc_transport_vtbl_st *pstOut);
void dmc__zero_mem(out void *pvBuf, in size_t uLen);
void dmc__copy_str(out char *pcDst, in size_t uCap, in const char *pcSrc);
int dmc__read_run_file(in const char *pcDataDir, in const char *pcWantRole, out int *piLink,
                       out int *piHttp, out char *pcBoot, in size_t uBootCap, out char *pcRole,
                       in size_t uRoleCap, out char *pcToken, in size_t uTokenCap);
void dmc__cli_drop_link(io dmc_client_st *pstCli);
void dmc__cli_clear_ifaces(io dmc_client_st *pstCli);
int dmc__cli_pump_once(io dmc_client_st *pstCli, in int iWaitMs);
int dmc__cli_call_raw(io dmc_client_st *pstCli, in uint16 u16LinkID, in uint8 u8Flag,
                      in const void *pvBody, in uint32 u32Len, in uint32 u32TimeoutMs,
                      out dmc_resp_st *pstResp);
int dmc__cli_connect_once(io dmc_client_st *pstCli);

#ifdef __cplusplus
}
#endif

#endif /* DMC_CLIENT_INT_H */
