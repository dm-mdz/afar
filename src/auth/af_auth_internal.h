/**
 ******************************************************************************
 * @file    af_auth_internal.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   服务层 af_auth 内部共享数据结构与子模块声明（仅 src/auth/ 内包含）。
 ******************************************************************************
 */
#ifndef AF_AUTH_INTERNAL_H
#define AF_AUTH_INTERNAL_H

#include "af_auth.h"
#include "../af_modules.h"
#include "../af_store.h"
#include "../plat/af_plat.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecjpake.h>
#include <mbedtls/entropy.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct af_pair_slot
{
  bool_t bUsed;
  char acPairId[33];
  char acDevName[65];
  uint64 u64ExpireMs;
  mbedtls_ecjpake_context stJpake;
} af_pair_slot_st;

typedef struct af_auth_ctx
{
  char acDataDir[512];
  uint8 au8OwnCertFp[32];
  bool_t bHasCertFp;

  af_device_st astDevs[AF_DEV_MAX];
  uint32 u32DevCnt;

  bool_t bPairOpen;
  char acCode[7];
  uint64 u64WinUntilMs;
  uint32 u32Attempts;
  uint32 u32LockRounds;
  uint64 u64LockedUntilMs;
  char acLastPaired[65];
  af_pair_slot_st astSlots[AF_PAIR_PENDING_MAX];

  uint32 u32PwFails;
  uint64 u64PwLockedUntilMs;

  bool_t bRngReady;
  mbedtls_entropy_context stEntropy;
  mbedtls_ctr_drbg_context stDrbg;

  uint32 u32InFlight;
  int iDrainHold;
  bool_t bDraining;

  af_auth_clock_fn pfClock;
  af_auth_od_verify_fn pfOdVerify;
} af_auth_ctx_st;

extern af_auth_ctx_st g_stAfAuth;

/* 公共辅助（af_auth_mod.c / af_auth_store.c / af_auth_pair.c） */
uint64 af_auth_now_ms(void);
int af_auth_rng_init(void);
void af_auth_rng_fini(void);
int af_auth_rng_bytes(dm_out uint8 *pu8Out, dm_in size_t uLen);
int af_auth_rng_cb(dm_io void *pvCtx, dm_out unsigned char *pucOut, dm_in size_t uLen);
void af_auth_hex_encode(dm_in const uint8 *pu8In, dm_in size_t uLen, dm_out char *pcOut);
void af_auth_trunc_utf8_chars(dm_in const char *pcSrc, dm_out char *pcDst, dm_in size_t uDstCap,
                              dm_in size_t uMaxChars);
void af_auth_publish_ev(dm_in af_auth_ev_kind_et eKind, dm_in const char *pcDevId,
                        dm_in const char *pcDevName, dm_in const char *pcSidecarName);
void af_auth_inflight_inc(void);
void af_auth_inflight_dec(void);

/* af_auth_store.c */
int af_auth_store_load(dm_in const char *pcDataDir);
int af_auth_store_save_async(dm_in af_store_saved_cb pfOnDone, dm_io void *pvCtx);
int af_auth_store_find_by_id(dm_in const char *pcDevId);
bool_t af_auth_store_remove_by_id(dm_in const char *pcDevId, dm_out af_device_st *pstRemoved);

/* af_auth_pair.c */
void af_auth_pair_refresh(void);
void af_auth_pair_open(void);
void af_auth_pair_close(dm_in bool_t bKeepLastPaired);
void af_auth_pair_on_start(dm_in const af_https_msg_st *pstMsg);
void af_auth_pair_on_finish(dm_in const af_https_msg_st *pstMsg);

#ifdef __cplusplus
}
#endif

#endif /* AF_AUTH_INTERNAL_H */
