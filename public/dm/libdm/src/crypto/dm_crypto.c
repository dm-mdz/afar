/**
 ******************************************************************************
 * @file    dm_crypto.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   libdm_crypto 密码学外观实现（DD-18 §1.2，MEMO-06b）：
 *          - RFC 8032 SHA-512 Ed25519 严格调用 Monocypher crypto_ed25519_*；
 *          - X25519 / HKDF-SHA256 / ChaCha20-Poly1305 / SHA-256 / CSPRNG 基于 Mbed TLS 3.6.8；
 *          - 严格零 dm_malloc_tag / dm_free_tag / libdm_core 依赖，全栈/调用方缓冲 +
 *            mbedtls_platform_zeroize 擦零，支持移动端原生 FFI 独立静态链接。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include <dm/crypto.h>
#include <dm/errno.h>

#include <monocypher-ed25519.h>

#include <mbedtls/chachapoly.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>

#include <string.h> /* memcpy / memset */

#define DM__CRYPTO_OK 0
#define DM__CRYPTO_INFO_PREFIX_MAX 128u
#define DM__CRYPTO_INFO_BUF_CAP (DM__CRYPTO_INFO_PREFIX_MAX + 64u)

/* DD-18 §5.3.2 默认 24B 域分隔前缀（含末尾 '\0'） */
static const uint8 g_au8DmCryptoDefaultInfo[24] = {
    0x42u, 0x4Fu, 0x54u, 0x46u, 0x4Cu, 0x4Fu, 0x57u, 0x2Du, 0x53u, 0x45u, 0x43u, 0x2Du,
    0x45u, 0x4Eu, 0x56u, 0x45u, 0x4Cu, 0x4Fu, 0x50u, 0x45u, 0x2Du, 0x56u, 0x31u, 0x00u,
};

/* ============================ 内部辅助 ============================ */

static bool_t dm__crypto_is_all_zero(dm_in const uint8 *pu8Buf, dm_in size_t uLen)
{
  size_t uIdx = 0u;
  uint8 u8Acc = 0u;

  for (uIdx = 0u; uIdx < uLen; uIdx++)
  {
    u8Acc |= pu8Buf[uIdx];
  }
  return (0u == u8Acc) ? DM_TRUE : DM_FALSE;
}

static int dm__crypto_drbg_init(dm_out mbedtls_entropy_context *pstEnt,
                                dm_out mbedtls_ctr_drbg_context *pstDrbg)
{
  static const unsigned char s_au8Pers[] = "dm_crypto_drbg_v1";
  int iRc = 0;

  mbedtls_entropy_init(pstEnt);
  mbedtls_ctr_drbg_init(pstDrbg);
  iRc = mbedtls_ctr_drbg_seed(pstDrbg, mbedtls_entropy_func, pstEnt, s_au8Pers,
                              sizeof(s_au8Pers) - 1u);
  if (0 != iRc)
  {
    mbedtls_ctr_drbg_free(pstDrbg);
    mbedtls_entropy_free(pstEnt);
    return DM__FAILD;
  }
  return DM__CRYPTO_OK;
}

static void dm__crypto_drbg_fini(dm_io mbedtls_entropy_context *pstEnt,
                                 dm_io mbedtls_ctr_drbg_context *pstDrbg)
{
  mbedtls_ctr_drbg_free(pstDrbg);
  mbedtls_entropy_free(pstEnt);
  return;
}

/* RFC 7748 Curve25519 标量钳位 */
static void dm__crypto_x25519_clamp(dm_io uint8 au8Scalar[32])
{
  au8Scalar[0] &= 248u;
  au8Scalar[31] &= 127u;
  au8Scalar[31] |= 64u;
  return;
}

/* 判定小端 32B 坐标（已清最高位）是否 >= p = 2^255 - 19（拒绝非规范 p/p+1 等低阶编码） */
static bool_t dm__crypto_x25519_ge_p(dm_in const uint8 au8U[32])
{
  size_t uIdx = 0u;

  if (au8U[31] != 0x7Fu)
  {
    return (au8U[31] > 0x7Fu) ? DM_TRUE : DM_FALSE;
  }
  for (uIdx = 30u; uIdx >= 1u; uIdx--)
  {
    if (au8U[uIdx] != 0xFFu)
    {
      return DM_FALSE;
    }
  }
  return (au8U[0] >= 0xEDu) ? DM_TRUE : DM_FALSE;
}

/* Mbed TLS 3.6.8 X25519 ECDH：计算 out = scalar * peer_pub（小端 32B），拒绝低阶点/全零结果 */
static int dm__crypto_x25519_shared(dm_in const uint8 au8Priv[32], dm_in const uint8 au8PeerPub[32],
                                    dm_out uint8 au8Out[32])
{
  mbedtls_entropy_context stEnt;
  mbedtls_ctr_drbg_context stDrbg;
  mbedtls_ecp_group stGrp;
  mbedtls_mpi stD;
  mbedtls_mpi stZ;
  mbedtls_ecp_point stQ;
  uint8 au8PrivClamp[32] = {0};
  uint8 au8PeerMask[32] = {0};
  bool_t bDrbgUp = DM_FALSE;
  int iErrCode = DM__FAILD;

  mbedtls_ecp_group_init(&stGrp);
  mbedtls_mpi_init(&stD);
  mbedtls_mpi_init(&stZ);
  mbedtls_ecp_point_init(&stQ);

  if (DM__CRYPTO_OK != dm__crypto_drbg_init(&stEnt, &stDrbg))
  {
    goto cleanup;
  }
  bDrbgUp = DM_TRUE;

  if (0 != mbedtls_ecp_group_load(&stGrp, MBEDTLS_ECP_DP_CURVE25519))
  {
    goto cleanup;
  }
  memcpy(au8PrivClamp, au8Priv, 32u);
  dm__crypto_x25519_clamp(au8PrivClamp);
  if (0 != mbedtls_mpi_read_binary_le(&stD, au8PrivClamp, 32u))
  {
    goto cleanup;
  }

  memcpy(au8PeerMask, au8PeerPub, 32u);
  au8PeerMask[31] &= 0x7Fu;
  if (dm__crypto_x25519_ge_p(au8PeerMask) || dm__crypto_is_all_zero(au8PeerMask, 32u))
  {
    iErrCode = DM__INVALID;
    goto cleanup;
  }
  if (0 != mbedtls_ecp_point_read_binary(&stGrp, &stQ, au8PeerMask, 32u))
  {
    iErrCode = DM__INVALID;
    goto cleanup;
  }
  if (0 != mbedtls_ecdh_compute_shared(&stGrp, &stZ, &stQ, &stD, mbedtls_ctr_drbg_random, &stDrbg))
  {
    iErrCode = DM__INVALID;
    goto cleanup;
  }
  if (0 != mbedtls_mpi_write_binary_le(&stZ, au8Out, 32u))
  {
    goto cleanup;
  }
  if (dm__crypto_is_all_zero(au8Out, 32u))
  {
    mbedtls_platform_zeroize(au8Out, 32u);
    iErrCode = DM__INVALID;
    goto cleanup;
  }
  iErrCode = DM__CRYPTO_OK;

cleanup:
  mbedtls_platform_zeroize(au8PrivClamp, sizeof(au8PrivClamp));
  mbedtls_platform_zeroize(au8PeerMask, sizeof(au8PeerMask));
  mbedtls_ecp_point_free(&stQ);
  mbedtls_mpi_free(&stZ);
  mbedtls_mpi_free(&stD);
  mbedtls_ecp_group_free(&stGrp);
  if (bDrbgUp)
  {
    dm__crypto_drbg_fini(&stEnt, &stDrbg);
  }
  return iErrCode;
}

static int dm__crypto_derive_key(dm_in const uint8 au8Shared[32], dm_in const uint8 *pu8Salt,
                                 dm_in size_t uSaltLen, dm_in const uint8 au8EphemPub[32],
                                 dm_in const uint8 au8RecipientPub[32],
                                 dm_in const uint8 *pu8Prefix, dm_in size_t uPrefixLen,
                                 dm_out uint8 au8KeyOut[32])
{
  const mbedtls_md_info_t *pstMd = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  uint8 au8Info[DM__CRYPTO_INFO_BUF_CAP] = {0};
  const uint8 *pu8UsePrefix = g_au8DmCryptoDefaultInfo;
  size_t uUsePrefixLen = sizeof(g_au8DmCryptoDefaultInfo);
  size_t uInfoLen = 0u;
  int iRc = 0;

  if (NULL == pstMd)
  {
    return DM__FAILD;
  }
  if (NULL != pu8Prefix)
  {
    if (uPrefixLen > DM__CRYPTO_INFO_PREFIX_MAX)
    {
      return DM__TOO_BIG;
    }
    pu8UsePrefix = pu8Prefix;
    uUsePrefixLen = uPrefixLen;
  }
  if (uUsePrefixLen > 0u)
  {
    memcpy(au8Info, pu8UsePrefix, uUsePrefixLen);
  }
  memcpy(au8Info + uUsePrefixLen, au8EphemPub, 32u);
  memcpy(au8Info + uUsePrefixLen + 32u, au8RecipientPub, 32u);
  uInfoLen = uUsePrefixLen + 64u;

  iRc = mbedtls_hkdf(pstMd, pu8Salt, uSaltLen, au8Shared, 32u, au8Info, uInfoLen, au8KeyOut, 32u);
  mbedtls_platform_zeroize(au8Info, sizeof(au8Info));
  return (0 == iRc) ? DM__CRYPTO_OK : DM__FAILD;
}

/* ============================ 公开基础原语 ============================ */

int dm_crypto_random(dm_out uint8 *pu8Out, dm_in size_t uLen)
{
  mbedtls_entropy_context stEnt;
  mbedtls_ctr_drbg_context stDrbg;
  size_t uOff = 0u;
  int iErrCode = DM__FAILD;

  if ((NULL == pu8Out) && (uLen > 0u))
  {
    return DM__INVALID;
  }
  if (0u == uLen)
  {
    return DM__CRYPTO_OK;
  }
  if (DM__CRYPTO_OK != dm__crypto_drbg_init(&stEnt, &stDrbg))
  {
    return DM__FAILD;
  }
  while (uOff < uLen)
  {
    size_t uRem = uLen - uOff;
    size_t uStep = (uRem > MBEDTLS_CTR_DRBG_MAX_REQUEST) ? MBEDTLS_CTR_DRBG_MAX_REQUEST : uRem;
    if (0 != mbedtls_ctr_drbg_random(&stDrbg, pu8Out + uOff, uStep))
    {
      mbedtls_platform_zeroize(pu8Out, uLen);
      goto cleanup;
    }
    uOff += uStep;
  }
  iErrCode = DM__CRYPTO_OK;

cleanup:
  dm__crypto_drbg_fini(&stEnt, &stDrbg);
  return iErrCode;
}

int dm_crypto_sha256(dm_in const void *pvIn, dm_in size_t uLen, dm_out uint8 au8Out[32])
{
  static const uint8 s_u8Empty = 0u;
  const unsigned char *pu8In = (NULL != pvIn) ? (const unsigned char *)pvIn : &s_u8Empty;

  if ((NULL == au8Out) || ((NULL == pvIn) && (uLen > 0u)))
  {
    return DM__INVALID;
  }
  return (0 == mbedtls_sha256(pu8In, uLen, au8Out, 0)) ? DM__CRYPTO_OK : DM__FAILD;
}

bool_t dm_crypto_const_eq(dm_in const void *pvA, dm_in const void *pvB, dm_in size_t uLen)
{
  const volatile uint8 *pu8A = (const volatile uint8 *)pvA;
  const volatile uint8 *pu8B = (const volatile uint8 *)pvB;
  size_t uIdx = 0u;
  uint8 u8Diff = 0u;

  if (0u == uLen)
  {
    return DM_TRUE;
  }
  if ((NULL == pvA) || (NULL == pvB))
  {
    return DM_FALSE;
  }
  for (uIdx = 0u; uIdx < uLen; uIdx++)
  {
    u8Diff |= (uint8)(pu8A[uIdx] ^ pu8B[uIdx]);
  }
  return (0u == u8Diff) ? DM_TRUE : DM_FALSE;
}

/* ============================ RFC 8032 Ed25519 ============================ */

int dm_crypto_ed25519_from_seed(dm_in const uint8 au8Seed[32], dm_out uint8 au8Priv[64],
                                dm_out uint8 au8Pub[32])
{
  uint8 au8SeedCopy[32] = {0};

  if ((NULL == au8Seed) || (NULL == au8Priv) || (NULL == au8Pub))
  {
    return DM__INVALID;
  }
  memcpy(au8SeedCopy, au8Seed, 32u);
  crypto_ed25519_key_pair(au8Priv, au8Pub, au8SeedCopy);
  mbedtls_platform_zeroize(au8SeedCopy, sizeof(au8SeedCopy));
  return DM__CRYPTO_OK;
}

int dm_crypto_ed25519_keygen(dm_out uint8 au8Priv[64], dm_out uint8 au8Pub[32])
{
  uint8 au8Seed[32] = {0};
  int iErrCode = DM__FAILD;

  if ((NULL == au8Priv) || (NULL == au8Pub))
  {
    return DM__INVALID;
  }
  iErrCode = dm_crypto_random(au8Seed, sizeof(au8Seed));
  if (DM__CRYPTO_OK == iErrCode)
  {
    crypto_ed25519_key_pair(au8Priv, au8Pub, au8Seed);
  }
  mbedtls_platform_zeroize(au8Seed, sizeof(au8Seed));
  return iErrCode;
}

int dm_crypto_ed25519_sign(dm_in const uint8 au8Priv[64], dm_in const uint8 *pu8Msg,
                           dm_in size_t uMsgLen, dm_out uint8 au8Sig[64])
{
  static const uint8 s_u8Empty = 0u;
  const uint8 *pu8Use = (NULL != pu8Msg) ? pu8Msg : &s_u8Empty;

  if ((NULL == au8Priv) || (NULL == au8Sig) || ((NULL == pu8Msg) && (uMsgLen > 0u)))
  {
    return DM__INVALID;
  }
  crypto_ed25519_sign(au8Sig, au8Priv, pu8Use, uMsgLen);
  return DM__CRYPTO_OK;
}

int dm_crypto_ed25519_verify(dm_in const uint8 au8Pub[32], dm_in const uint8 *pu8Msg,
                             dm_in size_t uMsgLen, dm_in const uint8 au8Sig[64])
{
  static const uint8 s_u8Empty = 0u;
  const uint8 *pu8Use = (NULL != pu8Msg) ? pu8Msg : &s_u8Empty;

  if ((NULL == au8Pub) || (NULL == au8Sig) || ((NULL == pu8Msg) && (uMsgLen > 0u)))
  {
    return DM__INVALID;
  }
  return (0 == crypto_ed25519_check(au8Sig, au8Pub, pu8Use, uMsgLen)) ? DM__CRYPTO_OK : DM__PERM;
}

/* ============================ X25519 + HKDF-SHA256 + ChaCha20-Poly1305
 * ============================ */

int dm_crypto_x25519_keygen(dm_out uint8 au8Priv[32], dm_out uint8 au8Pub[32])
{
  mbedtls_entropy_context stEnt;
  mbedtls_ctr_drbg_context stDrbg;
  mbedtls_ecp_group stGrp;
  mbedtls_mpi stD;
  mbedtls_ecp_point stQ;
  size_t uPubLen = 0u;
  bool_t bDrbgUp = DM_FALSE;
  int iErrCode = DM__FAILD;

  if ((NULL == au8Priv) || (NULL == au8Pub))
  {
    return DM__INVALID;
  }
  mbedtls_ecp_group_init(&stGrp);
  mbedtls_mpi_init(&stD);
  mbedtls_ecp_point_init(&stQ);

  if (DM__CRYPTO_OK != dm__crypto_drbg_init(&stEnt, &stDrbg))
  {
    goto cleanup;
  }
  bDrbgUp = DM_TRUE;
  if (0 != mbedtls_ecp_group_load(&stGrp, MBEDTLS_ECP_DP_CURVE25519))
  {
    goto cleanup;
  }
  if (0 != mbedtls_ecdh_gen_public(&stGrp, &stD, &stQ, mbedtls_ctr_drbg_random, &stDrbg))
  {
    goto cleanup;
  }
  if ((0 != mbedtls_mpi_write_binary_le(&stD, au8Priv, 32u)) ||
      (0 != mbedtls_ecp_point_write_binary(&stGrp, &stQ, MBEDTLS_ECP_PF_UNCOMPRESSED, &uPubLen,
                                           au8Pub, 32u)) ||
      (32u != uPubLen))
  {
    goto cleanup;
  }
  iErrCode = DM__CRYPTO_OK;

cleanup:
  if (DM__CRYPTO_OK != iErrCode)
  {
    mbedtls_platform_zeroize(au8Priv, 32u);
    mbedtls_platform_zeroize(au8Pub, 32u);
  }
  mbedtls_ecp_point_free(&stQ);
  mbedtls_mpi_free(&stD);
  mbedtls_ecp_group_free(&stGrp);
  if (bDrbgUp)
  {
    dm__crypto_drbg_fini(&stEnt, &stDrbg);
  }
  return iErrCode;
}

int dm_crypto_x25519_seal(dm_in const dm_x25519_seal_req_st *pstReq,
                          dm_io dm_x25519_envelope_st *pstEnv)
{
  static const uint8 s_u8Empty = 0u;
  mbedtls_chachapoly_context stCp;
  uint8 au8EphemPriv[32] = {0};
  uint8 au8EphemPub[32] = {0};
  uint8 au8Nonce[12] = {0};
  uint8 au8Shared[32] = {0};
  uint8 au8Key[32] = {0};
  const uint8 *pu8Plain = NULL;
  const uint8 *pu8Aad = NULL;
  uint8 *pu8Out = NULL;
  uint8 au8DummyOut[1] = {0};
  int iErrCode = DM__FAILD;

  if ((NULL == pstReq) || (NULL == pstEnv) || (NULL == pstReq->pu8RecipientPub) ||
      ((NULL == pstReq->pu8Plain) && (pstReq->uPlainLen > 0u)) ||
      ((NULL == pstReq->pu8Salt) && (pstReq->uSaltLen > 0u)) ||
      ((NULL == pstReq->pu8Aad) && (pstReq->uAadLen > 0u)) ||
      ((NULL == pstEnv->pu8Cipher) && (pstReq->uPlainLen > 0u)))
  {
    return DM__INVALID;
  }
  if (pstEnv->uCipherCap < pstReq->uPlainLen)
  {
    return DM__OVERFLOW;
  }

  mbedtls_chachapoly_init(&stCp);
  pstEnv->uCipherLen = 0u;

  iErrCode = dm_crypto_x25519_keygen(au8EphemPriv, au8EphemPub);
  if (DM__CRYPTO_OK != iErrCode)
  {
    goto cleanup;
  }
  iErrCode = dm_crypto_random(au8Nonce, sizeof(au8Nonce));
  if (DM__CRYPTO_OK != iErrCode)
  {
    goto cleanup;
  }
  iErrCode = dm__crypto_x25519_shared(au8EphemPriv, pstReq->pu8RecipientPub, au8Shared);
  if (DM__CRYPTO_OK != iErrCode)
  {
    goto cleanup;
  }
  iErrCode = dm__crypto_derive_key(au8Shared, pstReq->pu8Salt, pstReq->uSaltLen, au8EphemPub,
                                   pstReq->pu8RecipientPub, pstReq->pu8InfoPrefix,
                                   pstReq->uInfoPrefixLen, au8Key);
  if (DM__CRYPTO_OK != iErrCode)
  {
    goto cleanup;
  }
  if (0 != mbedtls_chachapoly_setkey(&stCp, au8Key))
  {
    iErrCode = DM__FAILD;
    goto cleanup;
  }

  pu8Plain = (NULL != pstReq->pu8Plain) ? pstReq->pu8Plain : &s_u8Empty;
  pu8Aad = (NULL != pstReq->pu8Aad) ? pstReq->pu8Aad : &s_u8Empty;
  pu8Out = (pstReq->uPlainLen > 0u) ? pstEnv->pu8Cipher : au8DummyOut;
  if (0 != mbedtls_chachapoly_encrypt_and_tag(&stCp, pstReq->uPlainLen, au8Nonce, pu8Aad,
                                              pstReq->uAadLen, pu8Plain, pu8Out, pstEnv->au8Tag))
  {
    iErrCode = DM__FAILD;
    goto cleanup;
  }
  memcpy(pstEnv->au8EphemPub, au8EphemPub, 32u);
  memcpy(pstEnv->au8Nonce, au8Nonce, 12u);
  pstEnv->uCipherLen = pstReq->uPlainLen;
  iErrCode = DM__CRYPTO_OK;

cleanup:
  if (DM__CRYPTO_OK != iErrCode)
  {
    mbedtls_platform_zeroize(pstEnv->au8EphemPub, sizeof(pstEnv->au8EphemPub));
    mbedtls_platform_zeroize(pstEnv->au8Nonce, sizeof(pstEnv->au8Nonce));
    mbedtls_platform_zeroize(pstEnv->au8Tag, sizeof(pstEnv->au8Tag));
    if ((NULL != pstEnv->pu8Cipher) && (pstReq->uPlainLen > 0u))
    {
      mbedtls_platform_zeroize(pstEnv->pu8Cipher, pstReq->uPlainLen);
    }
    pstEnv->uCipherLen = 0u;
  }
  mbedtls_chachapoly_free(&stCp);
  mbedtls_platform_zeroize(au8EphemPriv, sizeof(au8EphemPriv));
  mbedtls_platform_zeroize(au8EphemPub, sizeof(au8EphemPub));
  mbedtls_platform_zeroize(au8Nonce, sizeof(au8Nonce));
  mbedtls_platform_zeroize(au8Shared, sizeof(au8Shared));
  mbedtls_platform_zeroize(au8Key, sizeof(au8Key));
  return iErrCode;
}

int dm_crypto_x25519_open(dm_in const dm_x25519_open_req_st *pstReq, dm_out uint8 *pu8Plain,
                          dm_in size_t uPlainCap, dm_out size_t *puPlainLen)
{
  static const uint8 s_u8Empty = 0u;
  mbedtls_chachapoly_context stCp;
  uint8 au8Shared[32] = {0};
  uint8 au8Key[32] = {0};
  const uint8 *pu8Cipher = NULL;
  const uint8 *pu8Aad = NULL;
  uint8 *pu8Out = NULL;
  uint8 au8DummyOut[1] = {0};
  int iErrCode = DM__FAILD;

  if (NULL != puPlainLen)
  {
    *puPlainLen = 0u;
  }
  if ((NULL == pstReq) || (NULL == pstReq->pu8RecipientPriv) || (NULL == pstReq->pu8RecipientPub) ||
      (NULL == pstReq->pu8EphemPub) || (NULL == pstReq->pu8Nonce) || (NULL == pstReq->pu8Tag) ||
      ((NULL == pstReq->pu8Cipher) && (pstReq->uCipherLen > 0u)) ||
      ((NULL == pstReq->pu8Salt) && (pstReq->uSaltLen > 0u)) ||
      ((NULL == pstReq->pu8Aad) && (pstReq->uAadLen > 0u)) ||
      ((NULL == pu8Plain) && (pstReq->uCipherLen > 0u)))
  {
    if ((NULL != pu8Plain) && (uPlainCap > 0u))
    {
      mbedtls_platform_zeroize(pu8Plain, uPlainCap);
    }
    return DM__INVALID;
  }
  if (uPlainCap < pstReq->uCipherLen)
  {
    if ((NULL != pu8Plain) && (uPlainCap > 0u))
    {
      mbedtls_platform_zeroize(pu8Plain, uPlainCap);
    }
    return DM__OVERFLOW;
  }

  mbedtls_chachapoly_init(&stCp);

  iErrCode = dm__crypto_x25519_shared(pstReq->pu8RecipientPriv, pstReq->pu8EphemPub, au8Shared);
  if (DM__CRYPTO_OK != iErrCode)
  {
    goto cleanup;
  }
  iErrCode = dm__crypto_derive_key(au8Shared, pstReq->pu8Salt, pstReq->uSaltLen,
                                   pstReq->pu8EphemPub, pstReq->pu8RecipientPub,
                                   pstReq->pu8InfoPrefix, pstReq->uInfoPrefixLen, au8Key);
  if (DM__CRYPTO_OK != iErrCode)
  {
    goto cleanup;
  }
  if (0 != mbedtls_chachapoly_setkey(&stCp, au8Key))
  {
    iErrCode = DM__FAILD;
    goto cleanup;
  }

  pu8Cipher = (NULL != pstReq->pu8Cipher) ? pstReq->pu8Cipher : &s_u8Empty;
  pu8Aad = (NULL != pstReq->pu8Aad) ? pstReq->pu8Aad : &s_u8Empty;
  pu8Out = (pstReq->uCipherLen > 0u) ? pu8Plain : au8DummyOut;
  if (0 != mbedtls_chachapoly_auth_decrypt(&stCp, pstReq->uCipherLen, pstReq->pu8Nonce, pu8Aad,
                                           pstReq->uAadLen, pstReq->pu8Tag, pu8Cipher, pu8Out))
  {
    iErrCode = DM__PERM;
    goto cleanup;
  }
  if (NULL != puPlainLen)
  {
    *puPlainLen = pstReq->uCipherLen;
  }
  iErrCode = DM__CRYPTO_OK;

cleanup:
  if ((DM__CRYPTO_OK != iErrCode) && (NULL != pu8Plain) && (uPlainCap > 0u))
  {
    mbedtls_platform_zeroize(pu8Plain, uPlainCap);
  }
  mbedtls_chachapoly_free(&stCp);
  mbedtls_platform_zeroize(au8Shared, sizeof(au8Shared));
  mbedtls_platform_zeroize(au8Key, sizeof(au8Key));
  return iErrCode;
}

#ifdef __cplusplus
}
#endif
