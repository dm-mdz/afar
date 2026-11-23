/**
 ******************************************************************************
 * @file    af_auth.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   服务层 af_auth 横切鉴权与配对状态公共契约头（S1 冻结供 S2 并行开发）。
 ******************************************************************************
 */
#ifndef AF_AUTH_H
#define AF_AUTH_H

#include <dm/https.h>
#include <dm/pool.h>
#include <dm/rpc.h>
#include "../af_events.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define AF_DEV_MAX          64u
#define AF_PAIR_PENDING_MAX 4u
#define AF_PAIR_WINDOW_MS   (10u * 60u * 1000u)
#define AF_PAIR_PENDING_MS  (60u * 1000u)
#define AF_PAIR_MAX_TRY     5u
#define AF_PW_MAX_TRY       5u
#define AF_PW_LOCK_MS       (60u * 1000u)

typedef struct af_device
{
  char acId[37];
  char acName[65];
  char acTokenHash[65];
  char acSidecarName[65];
  uint64 u64PairedAt;
  bool_t bPasswordSaved;
} af_device_st;

typedef struct af_pair_view
{
  char acCode[7];
  uint64 u64LeftSec;
  uint64 u64LockSec;
  char acLastPaired[65];
} af_pair_view_st;

int af_auth_ensure_tls_cert(in const char *pcDataDir, out uint8 au8Fp[32]);
bool_t af_auth_check_https(in const dm_https_msg_st *pstMsg, out af_device_st *pstDevOut);
void af_https_send_ok(in uint64 u64ReplyId, in const char *pcJson);
void af_https_send_err(in uint64 u64ReplyId, in uint uiStatus, in const char *pcCode,
                       in const char *pcMsg);
void af_auth_get_pair_view(out af_pair_view_st *pstOut);
uint32 af_auth_list_devices(out af_device_st *pastOut, in uint32 u32Cap);
bool_t af_auth_other_owns_sidecar(in const char *pcSelfDevId, in const char *pcSidecarName);
int af_auth_bind_sidecar(in const char *pcDevId, in const char *pcSidecarName);
uint32 af_auth_lock_minutes(in uint32 u32Rounds);
bool_t af_auth_ct_eq(in const uint8 *pu8A, in const uint8 *pu8B, in size_t uLen);

/* ---- 增量扩展接口（向后兼容冻结契约） ---- */
extern const dm_module_st g_stAfAuthMod;
const dm_module_st *af_auth_module(void);
void af_auth_hash_token(in const char *pcTok, in size_t uLen, out char acHexOut[65]);
uint64 af_auth_ct_eq_last_steps(void);
int af_auth_set_data_dir(in const char *pcDataDir);

typedef uint64 (*af_auth_clock_fn)(void);
typedef int (*af_auth_od_verify_fn)(in const char *pcUser, in const char *pcPass);
void af_auth_set_clock_fn(in af_auth_clock_fn pfClock);
void af_auth_set_od_verify_fn(in af_auth_od_verify_fn pfVerify);

#ifdef __cplusplus
}
#endif

#endif /* AF_AUTH_H */
