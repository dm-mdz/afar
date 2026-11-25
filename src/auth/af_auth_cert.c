/**
 ******************************************************************************
 * @file    af_auth_cert.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   ECDSA P-256 TLS 证书加载（兼容 PKCS#8 与 SEC1 DER）、首次自签与指纹计算。
 ******************************************************************************
 */

#define _DARWIN_C_SOURCE
#define DM_LOG_MOD "auth"

#include "af_auth_internal.h"
#include <dm/log.h>

#include <mbedtls/ecp.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define AF_CERT_FILE_MAX (64u * 1024u)

static int af__cert_read_file(dm_in const char *pcPath, dm_out uint8 **ppu8Out,
                              dm_out size_t *puLen)
{
  FILE *pf = NULL;
  long lSz = 0;
  uint8 *pu8Buf = NULL;

  *ppu8Out = NULL;
  *puLen = 0u;
  pf = fopen(pcPath, "rb");
  if (NULL == pf)
  {
    return (ENOENT == errno) ? DM_NOT_FOUND : DM_IO;
  }
  if ((0 != fseek(pf, 0, SEEK_END)) || ((lSz = ftell(pf)) <= 0) || ((size_t)lSz > AF_CERT_FILE_MAX))
  {
    fclose(pf);
    return DM_INVALID;
  }
  rewind(pf);
  pu8Buf = (uint8 *)dm_malloc_tag((size_t)lSz, DM_MEM_TAG_SECRET);
  if (NULL == pu8Buf)
  {
    fclose(pf);
    return DM_ALLOC;
  }
  if ((size_t)lSz != fread(pu8Buf, 1u, (size_t)lSz, pf))
  {
    dm_secure_zero(pu8Buf, (size_t)lSz);
    dm_free_tag(pu8Buf);
    fclose(pf);
    return DM_IO;
  }
  fclose(pf);
  *ppu8Out = pu8Buf;
  *puLen = (size_t)lSz;
  return DM_SUCCESS;
}

static int af__cert_verify_pair(dm_in const uint8 *pu8Cert, dm_in size_t uCertLen,
                                dm_in const uint8 *pu8Key, dm_in size_t uKeyLen,
                                dm_out uint8 au8Fp[32])
{
  mbedtls_entropy_context stEnt;
  mbedtls_ctr_drbg_context stDrbg;
  mbedtls_x509_crt stCrt;
  mbedtls_pk_context stKey;
  int iRc = DM_INVALID;

  mbedtls_entropy_init(&stEnt);
  mbedtls_ctr_drbg_init(&stDrbg);
  mbedtls_x509_crt_init(&stCrt);
  mbedtls_pk_init(&stKey);

  if ((0 == mbedtls_ctr_drbg_seed(&stDrbg, mbedtls_entropy_func, &stEnt,
                                  (const unsigned char *)"af_cert_v", 9u)) &&
      (0 == mbedtls_x509_crt_parse_der(&stCrt, pu8Cert, uCertLen)) &&
      (0 ==
       mbedtls_pk_parse_key(&stKey, pu8Key, uKeyLen, NULL, 0u, mbedtls_ctr_drbg_random, &stDrbg)) &&
      (0 == mbedtls_pk_check_pair(&stCrt.pk, &stKey, mbedtls_ctr_drbg_random, &stDrbg)) &&
      (0 == mbedtls_sha256(pu8Cert, uCertLen, au8Fp, 0)))
  {
    iRc = DM_SUCCESS;
  }

  mbedtls_pk_free(&stKey);
  mbedtls_x509_crt_free(&stCrt);
  mbedtls_ctr_drbg_free(&stDrbg);
  mbedtls_entropy_free(&stEnt);
  return iRc;
}

static int af__cert_load_existing(dm_in const char *pcCertPath, dm_in const char *pcKeyPath,
                                  dm_out uint8 au8Fp[32])
{
  uint8 *pu8Cert = NULL;
  uint8 *pu8Key = NULL;
  size_t uCertLen = 0u;
  size_t uKeyLen = 0u;
  int iRcCert = af__cert_read_file(pcCertPath, &pu8Cert, &uCertLen);
  int iRcKey = af__cert_read_file(pcKeyPath, &pu8Key, &uKeyLen);
  int iRet = DM_SUCCESS;

  if ((DM_NOT_FOUND == iRcCert) || (DM_NOT_FOUND == iRcKey))
  {
    iRet = DM_NOT_FOUND;
  }
  else if ((DM_SUCCESS != iRcCert) || (DM_SUCCESS != iRcKey))
  {
    iRet = DM_INVALID;
  }
  else
  {
    iRet = af__cert_verify_pair(pu8Cert, uCertLen, pu8Key, uKeyLen, au8Fp);
  }
  if (NULL != pu8Key)
  {
    dm_secure_zero(pu8Key, uKeyLen);
    dm_free_tag(pu8Key);
  }
  if (NULL != pu8Cert)
  {
    dm_free_tag(pu8Cert);
  }
  return iRet;
}

static int af__cert_write_generated(dm_in mbedtls_pk_context *pstKey,
                                    dm_io mbedtls_ctr_drbg_context *pstDrbg,
                                    dm_in const char *pcCertPath, dm_in const char *pcKeyPath,
                                    dm_out uint8 au8Fp[32])
{
  mbedtls_x509write_cert stCrt;
  unsigned char aucSer[16];
  unsigned char aucKeyDer[512];
  unsigned char aucCrtDer[2048];
  int iKeyLen = 0;
  int iCrtLen = 0;
  int iRet = DM_FAILD;

  mbedtls_x509write_crt_init(&stCrt);
  mbedtls_x509write_crt_set_version(&stCrt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&stCrt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&stCrt, pstKey);
  mbedtls_x509write_crt_set_issuer_key(&stCrt, pstKey);

  if ((0 == mbedtls_ctr_drbg_random(pstDrbg, aucSer, sizeof(aucSer))) &&
      (0 == mbedtls_x509write_crt_set_subject_name(&stCrt, "CN=afar.local")) &&
      (0 == mbedtls_x509write_crt_set_issuer_name(&stCrt, "CN=afar.local")) &&
      (0 == mbedtls_x509write_crt_set_validity(&stCrt, "20240101000000", "20491231235959")))
  {
    aucSer[0] = (unsigned char)((aucSer[0] & 0x7Fu) | 0x01u);
    if (0 == mbedtls_x509write_crt_set_serial_raw(&stCrt, aucSer, sizeof(aucSer)))
    {
      iKeyLen = mbedtls_pk_write_key_der(pstKey, aucKeyDer, sizeof(aucKeyDer));
      iCrtLen = mbedtls_x509write_crt_der(&stCrt, aucCrtDer, sizeof(aucCrtDer),
                                          mbedtls_ctr_drbg_random, pstDrbg);
    }
  }
  if ((iKeyLen > 0) && (iCrtLen > 0))
  {
    const uint8 *pu8KeyStart = aucKeyDer + (sizeof(aucKeyDer) - (size_t)iKeyLen);
    const uint8 *pu8CrtStart = aucCrtDer + (sizeof(aucCrtDer) - (size_t)iCrtLen);
    if ((DM_SUCCESS == af_store_write_atomic(pcKeyPath, pu8KeyStart, (size_t)iKeyLen)) &&
        (DM_SUCCESS == af_store_write_atomic(pcCertPath, pu8CrtStart, (size_t)iCrtLen)) &&
        (0 == mbedtls_sha256(pu8CrtStart, (size_t)iCrtLen, au8Fp, 0)))
    {
      iRet = DM_SUCCESS;
    }
  }
  dm_secure_zero(aucKeyDer, sizeof(aucKeyDer));
  dm_secure_zero(aucSer, sizeof(aucSer));
  mbedtls_x509write_crt_free(&stCrt);
  return iRet;
}

static int af__cert_generate_new(dm_in const char *pcCertPath, dm_in const char *pcKeyPath,
                                 dm_out uint8 au8Fp[32])
{
  mbedtls_entropy_context stEnt;
  mbedtls_ctr_drbg_context stDrbg;
  mbedtls_pk_context stKey;
  int iRet = DM_FAILD;

  mbedtls_entropy_init(&stEnt);
  mbedtls_ctr_drbg_init(&stDrbg);
  mbedtls_pk_init(&stKey);
  if ((0 == mbedtls_ctr_drbg_seed(&stDrbg, mbedtls_entropy_func, &stEnt,
                                  (const unsigned char *)"af_cert_g", 9u)) &&
      (0 == mbedtls_pk_setup(&stKey, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) &&
      (0 == mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(stKey),
                                mbedtls_ctr_drbg_random, &stDrbg)))
  {
    iRet = af__cert_write_generated(&stKey, &stDrbg, pcCertPath, pcKeyPath, au8Fp);
  }
  mbedtls_pk_free(&stKey);
  mbedtls_ctr_drbg_free(&stDrbg);
  mbedtls_entropy_free(&stEnt);
  return iRet;
}

int af_auth_ensure_tls_cert(dm_in const char *pcDataDir, dm_out uint8 au8Fp[32])
{
  char acCertPath[600];
  char acKeyPath[600];
  uint8 au8LocalFp[32];
  int iRet = DM_SUCCESS;

  if ((NULL == pcDataDir) || ('\0' == pcDataDir[0]))
  {
    return DM_INVALID;
  }
  if (PSA_SUCCESS != psa_crypto_init())
  {
    return DM_FAILD;
  }
  (void)mkdir(pcDataDir, 0700);
  snprintf(acCertPath, sizeof(acCertPath), "%s/tls-cert.der", pcDataDir);
  snprintf(acKeyPath, sizeof(acKeyPath), "%s/tls-key.der", pcDataDir);

  iRet = af__cert_load_existing(acCertPath, acKeyPath, au8LocalFp);
  if (DM_NOT_FOUND == iRet)
  {
    iRet = af__cert_generate_new(acCertPath, acKeyPath, au8LocalFp);
  }
  if (DM_SUCCESS != iRet)
  {
    DM_LOG_ERROR("failed to ensure TLS cert in %s (err=%d)", pcDataDir, iRet);
    return iRet;
  }
  snprintf(g_stAfAuth.acDataDir, sizeof(g_stAfAuth.acDataDir), "%s", pcDataDir);
  memcpy(g_stAfAuth.au8OwnCertFp, au8LocalFp, sizeof(au8LocalFp));
  g_stAfAuth.bHasCertFp = DM_TRUE;
  if (NULL != au8Fp)
  {
    memcpy(au8Fp, au8LocalFp, sizeof(au8LocalFp));
  }
  return DM_SUCCESS;
}

bool_t af_auth_get_cert_sha256(dm_out char acHexOut[65])
{
  static const char sc_acHex[17] = "0123456789abcdef";
  size_t uIdx = 0u;

  if (NULL == acHexOut)
  {
    return DM_FALSE;
  }
  if (!g_stAfAuth.bHasCertFp)
  {
    acHexOut[0] = '\0';
    return DM_FALSE;
  }
  for (uIdx = 0u; uIdx < 32u; uIdx++)
  {
    acHexOut[uIdx * 2u] = sc_acHex[(g_stAfAuth.au8OwnCertFp[uIdx] >> 4u) & 0x0Fu];
    acHexOut[uIdx * 2u + 1u] = sc_acHex[g_stAfAuth.au8OwnCertFp[uIdx] & 0x0Fu];
  }
  acHexOut[64] = '\0';
  return DM_TRUE;
}
