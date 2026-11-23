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
int af_auth_rng_bytes(out uint8 *pu8Out, in size_t uLen);
int af_auth_rng_cb(io void *pvCtx, out unsigned char *pucOut, in size_t uLen);
void af_auth_hex_encode(in const uint8 *pu8In, in size_t uLen, out char *pcOut);
void af_auth_trunc_utf8_chars(in const char *pcSrc, out char *pcDst, in size_t uDstCap,
                              in size_t uMaxChars);
void af_auth_publish_ev(in af_auth_ev_kind_et eKind, in const char *pcDevId,
                        in const char *pcDevName, in const char *pcSidecarName);
void af_auth_inflight_inc(void);
void af_auth_inflight_dec(void);

/* af_auth_store.c */
int af_auth_store_load(in const char *pcDataDir);
int af_auth_store_save_async(in af_store_saved_cb pfOnDone, io void *pvCtx);
int af_auth_store_find_by_id(in const char *pcDevId);
bool_t af_auth_store_remove_by_id(in const char *pcDevId, out af_device_st *pstRemoved);

/* af_auth_pair.c */
void af_auth_pair_refresh(void);
void af_auth_pair_open(void);
void af_auth_pair_close(in bool_t bKeepLastPaired);
void af_auth_pair_on_start(in const dm_https_msg_st *pstMsg);
void af_auth_pair_on_finish(in const dm_https_msg_st *pstMsg);

#ifdef __cplusplus
}
#endif

#endif /* AF_AUTH_INTERNAL_H */
