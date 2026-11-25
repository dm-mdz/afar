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
  void (*pfOnPush)(dm_in void *pvUser, dm_in const dmc_push_st *pstPush);
  void (*pfOnCall)(dm_in void *pvUser, dm_in uint16 u16Func, dm_in uint32 u32ReqID,
                   dm_in const uint8 *pu8Body, dm_in uint32 u32Len);
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

bool_t dmc__transport_copy(dm_in const char *pcScheme, dm_out dmc_transport_vtbl_st *pstOut);
void dmc__zero_mem(dm_out void *pvBuf, dm_in size_t uLen);
void dmc__copy_str(dm_out char *pcDst, dm_in size_t uCap, dm_in const char *pcSrc);
typedef struct dmc__run_info
{
  int iLinkPort;
  int iHttpPort;
  char acBoot[64];
  char acRole[64];
  char acToken[256];
} dmc__run_info_st;

int dmc__read_run_file(dm_in const char *pcDataDir, dm_in const char *pcWantRole,
                       dm_out dmc__run_info_st *pstOut);
void dmc__cli_drop_link(dm_io dmc_client_st *pstCli);
void dmc__cli_clear_ifaces(dm_io dmc_client_st *pstCli);
int dmc__cli_pump_once(dm_io dmc_client_st *pstCli, dm_in int iWaitMs);
int dmc__cli_call_raw(dm_io dmc_client_st *pstCli, dm_in const dmc_call_id_req_st *pstReq,
                      dm_out dmc_resp_st *pstResp);
int dmc__cli_connect_once(dm_io dmc_client_st *pstCli);

#ifdef __cplusplus
}
#endif

#endif /* DMC_CLIENT_INT_H */
