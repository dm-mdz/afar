/**
 ******************************************************************************
 * @file    crypto.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   DM 统一密码学外观（DD-18 §1.2）：
 *          RFC 8032 SHA-512 Ed25519（Monocypher）+ X25519 / HKDF-SHA256 /
 *          ChaCha20-Poly1305 / SHA-256 / CSPRNG（Mbed TLS 3.6.8）。
 *          零 libuv / libdm_core 依赖，可独立静态链接至移动端原生库。
 ******************************************************************************
 */
#ifndef DM_CRYPTO_H
#define DM_CRYPTO_H

#include <dm/type.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_CRYPTO_ED25519_SEED_LEN 32u
#define DM_CRYPTO_ED25519_PUB_LEN 32u
#define DM_CRYPTO_ED25519_PRIV_LEN 64u /* seed(32) || pub(32) */
#define DM_CRYPTO_ED25519_SIG_LEN 64u
#define DM_CRYPTO_X25519_KEY_LEN 32u
#define DM_CRYPTO_AEAD_NONCE_LEN 12u
#define DM_CRYPTO_AEAD_TAG_LEN 16u
#define DM_CRYPTO_SHA256_LEN 32u

typedef struct dm_x25519_seal_req
{
  const uint8 *pu8RecipientPub; /* 32B 目标节点 X25519 公钥 */
  const uint8 *pu8Salt;         /* HKDF salt（如 id || '\0' || target_id） */
  size_t uSaltLen;
  const uint8 *pu8Aad; /* AEAD AAD */
  size_t uAadLen;
  const uint8 *pu8Plain; /* 待封明文（调用方封完立刻擦零） */
  size_t uPlainLen;
  const uint8 *pu8InfoPrefix; /* 可选 HKDF info 前缀（NULL 使用默认 24B 域前缀） */
  size_t uInfoPrefixLen;
} dm_x25519_seal_req_st;

typedef struct dm_x25519_envelope
{
  uint8 au8EphemPub[32];
  uint8 au8Nonce[12];
  uint8 au8Tag[16];
  uint8 *pu8Cipher; /* 调用方提供缓冲（长度 >= uPlainLen） */
  size_t uCipherCap;
  size_t uCipherLen;
} dm_x25519_envelope_st;

typedef struct dm_x25519_open_req
{
  const uint8 *pu8RecipientPriv; /* 32B 本机 X25519 私钥 */
  const uint8 *pu8RecipientPub;  /* 32B 本机 X25519 公钥（绑定 HKDF info） */
  const uint8 *pu8EphemPub;      /* 32B 临时公钥 */
  const uint8 *pu8Nonce;         /* 12B AEAD Nonce */
  const uint8 *pu8Tag;           /* 16B Poly1305 Tag */
  const uint8 *pu8Salt;
  size_t uSaltLen;
  const uint8 *pu8Aad;
  size_t uAadLen;
  const uint8 *pu8Cipher;
  size_t uCipherLen;
  const uint8 *pu8InfoPrefix; /* 可选 HKDF info 前缀（NULL 使用默认 24B 域前缀） */
  size_t uInfoPrefixLen;
} dm_x25519_open_req_st;

DM_API int dm_crypto_random(dm_out uint8 *pu8Out, dm_in size_t uLen);
DM_API int dm_crypto_sha256(dm_in const void *pvIn, dm_in size_t uLen, dm_out uint8 au8Out[32]);
DM_API bool_t dm_crypto_const_eq(dm_in const void *pvA, dm_in const void *pvB, dm_in size_t uLen);

DM_API int dm_crypto_ed25519_from_seed(dm_in const uint8 au8Seed[32], dm_out uint8 au8Priv[64],
                                       dm_out uint8 au8Pub[32]);
DM_API int dm_crypto_ed25519_keygen(dm_out uint8 au8Priv[64], dm_out uint8 au8Pub[32]);
DM_API int dm_crypto_ed25519_sign(dm_in const uint8 au8Priv[64], dm_in const uint8 *pu8Msg,
                                  dm_in size_t uMsgLen, dm_out uint8 au8Sig[64]);
DM_API int dm_crypto_ed25519_verify(dm_in const uint8 au8Pub[32], dm_in const uint8 *pu8Msg,
                                    dm_in size_t uMsgLen, dm_in const uint8 au8Sig[64]);

DM_API int dm_crypto_x25519_keygen(dm_out uint8 au8Priv[32], dm_out uint8 au8Pub[32]);
DM_API int dm_crypto_x25519_seal(dm_in const dm_x25519_seal_req_st *pstReq,
                                 dm_io dm_x25519_envelope_st *pstEnv);
DM_API int dm_crypto_x25519_open(dm_in const dm_x25519_open_req_st *pstReq, dm_out uint8 *pu8Plain,
                                 dm_in size_t uPlainCap, dm_out size_t *puPlainLen);

#ifdef __cplusplus
}
#endif

#endif /* DM_CRYPTO_H */
