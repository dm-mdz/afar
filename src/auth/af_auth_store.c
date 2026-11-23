/**
 ******************************************************************************
 * @file    af_auth_store.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   devices.json 读写（DM_LANE_DB 原子落盘）、SHA-256 令牌哈希与常数时间比对。
 ******************************************************************************
 */

#define _DARWIN_C_SOURCE
#define DM_LOG_MOD "auth"

#include "af_auth_internal.h"
#include <dm/json.h>
#include <dm/log.h>
#include <mbedtls/sha256.h>

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic uint64 g_u64CtEqSteps = 0u;

void af_auth_hex_encode(in const uint8 *pu8In, in size_t uLen, out char *pcOut)
{
  static const char acHex[] = "0123456789abcdef";
  size_t i = 0u;

  for (i = 0u; i < uLen; i++)
  {
    pcOut[i * 2u] = acHex[(pu8In[i] >> 4u) & 0x0Fu];
    pcOut[(i * 2u) + 1u] = acHex[pu8In[i] & 0x0Fu];
  }
  pcOut[uLen * 2u] = '\0';
}

bool_t af_auth_ct_eq(in const uint8 *pu8A, in const uint8 *pu8B, in size_t uLen)
{
  volatile uint8 u8Diff = 0u;
  uint64 u64Steps = 0u;
  size_t i = 0u;

  if (0u == uLen)
  {
    atomic_store_explicit(&g_u64CtEqSteps, 0u, memory_order_relaxed);
    return DM_TRUE;
  }
  if ((NULL == pu8A) || (NULL == pu8B))
  {
    atomic_store_explicit(&g_u64CtEqSteps, 0u, memory_order_relaxed);
    return DM_FALSE;
  }
  for (i = 0u; i < uLen; i++)
  {
    u8Diff = (uint8)(u8Diff | (uint8)(pu8A[i] ^ pu8B[i]));
    u64Steps++;
  }
  atomic_store_explicit(&g_u64CtEqSteps, u64Steps, memory_order_relaxed);
  return (0u == u8Diff) ? DM_TRUE : DM_FALSE;
}

uint64 af_auth_ct_eq_last_steps(void)
{
  return atomic_load_explicit(&g_u64CtEqSteps, memory_order_relaxed);
}

void af_auth_hash_token(in const char *pcTok, in size_t uLen, out char acHexOut[65])
{
  uint8 au8Digest[32];

  if (NULL == acHexOut)
  {
    return;
  }
  acHexOut[0] = '\0';
  if (NULL == pcTok)
  {
    return;
  }
  if (0 == mbedtls_sha256((const unsigned char *)pcTok, uLen, au8Digest, 0))
  {
    af_auth_hex_encode(au8Digest, sizeof(au8Digest), acHexOut);
  }
  dm_secure_zero(au8Digest, sizeof(au8Digest));
}

static bool_t af__is_hex64(in const char *pcStr)
{
  size_t i = 0u;

  if ((NULL == pcStr) || (64u != strlen(pcStr)))
  {
    return DM_FALSE;
  }
  for (i = 0u; i < 64u; i++)
  {
    if (0 == isxdigit((unsigned char)pcStr[i]))
    {
      return DM_FALSE;
    }
  }
  return DM_TRUE;
}

static char *af__read_text_file(in const char *pcPath, out size_t *puLen)
{
  FILE *pf = fopen(pcPath, "rb");
  long lSz = 0;
  char *pcBuf = NULL;

  *puLen = 0u;
  if (NULL == pf)
  {
    return NULL;
  }
  if ((0 != fseek(pf, 0, SEEK_END)) || ((lSz = ftell(pf)) < 0) || (lSz > (1024L * 1024L)))
  {
    fclose(pf);
    return NULL;
  }
  rewind(pf);
  pcBuf = (char *)dm_malloc_tag((size_t)lSz + 1u, DM_MEM_TAG_JSON);
  if (NULL == pcBuf)
  {
    fclose(pf);
    return NULL;
  }
  if ((lSz > 0) && ((size_t)lSz != fread(pcBuf, 1u, (size_t)lSz, pf)))
  {
    dm_free_tag(pcBuf);
    fclose(pf);
    return NULL;
  }
  pcBuf[lSz] = '\0';
  fclose(pf);
  *puLen = (size_t)lSz;
  return pcBuf;
}

static void af__parse_device_item(in yyjson_val *pstItem)
{
  const char *pcId = NULL;
  const char *pcName = NULL;
  const char *pcHash = NULL;
  const char *pcSc = NULL;
  yyjson_val *pstV = NULL;
  af_device_st *pstDev = NULL;

  if ((g_stAfAuth.u32DevCnt >= AF_DEV_MAX) || (!yyjson_is_obj(pstItem)))
  {
    return;
  }
  pcId = yyjson_get_str(yyjson_obj_get(pstItem, "id"));
  pcHash = yyjson_get_str(yyjson_obj_get(pstItem, "token_hash"));
  if ((NULL == pcId) || ('\0' == pcId[0]) || (!af__is_hex64(pcHash)))
  {
    return;
  }
  pstDev = &g_stAfAuth.astDevs[g_stAfAuth.u32DevCnt++];
  memset(pstDev, 0, sizeof(*pstDev));
  snprintf(pstDev->acId, sizeof(pstDev->acId), "%s", pcId);
  pcName = yyjson_get_str(yyjson_obj_get(pstItem, "name"));
  snprintf(pstDev->acName, sizeof(pstDev->acName), "%s", (NULL != pcName) ? pcName : "iPad");
  snprintf(pstDev->acTokenHash, sizeof(pstDev->acTokenHash), "%s", pcHash);
  pcSc = yyjson_get_str(yyjson_obj_get(pstItem, "sidecar_name"));
  if (NULL != pcSc)
  {
    snprintf(pstDev->acSidecarName, sizeof(pstDev->acSidecarName), "%s", pcSc);
  }
  pstV = yyjson_obj_get(pstItem, "paired_at");
  if (yyjson_is_uint(pstV))
  {
    pstDev->u64PairedAt = yyjson_get_uint(pstV);
  }
  pstV = yyjson_obj_get(pstItem, "password_saved");
  if (yyjson_is_bool(pstV))
  {
    pstDev->bPasswordSaved = yyjson_get_bool(pstV) ? DM_TRUE : DM_FALSE;
  }
}

int af_auth_store_load(in const char *pcDataDir)
{
  char acPath[600];
  size_t uLen = 0u;
  char *pcText = NULL;
  yyjson_doc *pstDoc = NULL;
  yyjson_val *pstRoot = NULL;
  yyjson_val *pstArr = NULL;
  yyjson_val *pstItem = NULL;
  size_t uIdx = 0u;
  size_t uMax = 0u;

  memset(g_stAfAuth.astDevs, 0, sizeof(g_stAfAuth.astDevs));
  g_stAfAuth.u32DevCnt = 0u;
  if ((NULL == pcDataDir) || ('\0' == pcDataDir[0]))
  {
    return DM_INVALID;
  }
  snprintf(acPath, sizeof(acPath), "%s/devices.json", pcDataDir);
  pcText = af__read_text_file(acPath, &uLen);
  if (NULL == pcText)
  {
    return DM_SUCCESS;
  }
  pstDoc = yyjson_read(pcText, uLen, 0u);
  dm_free_tag(pcText);
  if (NULL == pstDoc)
  {
    DM_LOG_WARN("corrupt devices.json at %s, starting empty", acPath);
    return DM_SUCCESS;
  }
  pstRoot = yyjson_doc_get_root(pstDoc);
  pstArr = yyjson_is_arr(pstRoot) ? pstRoot : yyjson_obj_get(pstRoot, "devices");
  if (!yyjson_is_arr(pstArr))
  {
    DM_LOG_WARN("invalid devices.json root at %s, starting empty", acPath);
    yyjson_doc_free(pstDoc);
    return DM_SUCCESS;
  }
  yyjson_arr_foreach(pstArr, uIdx, uMax, pstItem)
  {
    af__parse_device_item(pstItem);
  }
  yyjson_doc_free(pstDoc);
  return DM_SUCCESS;
}

static char *af__serialize_devices(out size_t *puLen)
{
  yyjson_mut_doc *pstDoc = yyjson_mut_doc_new(dm_json_alc());
  yyjson_mut_val *pstRoot = NULL;
  yyjson_mut_val *pstArr = NULL;
  char *pcJson = NULL;
  uint32 i = 0u;

  *puLen = 0u;
  if (NULL == pstDoc)
  {
    return NULL;
  }
  pstRoot = yyjson_mut_obj(pstDoc);
  pstArr = yyjson_mut_arr(pstDoc);
  yyjson_mut_doc_set_root(pstDoc, pstRoot);
  for (i = 0u; i < g_stAfAuth.u32DevCnt; i++)
  {
    const af_device_st *pstD = &g_stAfAuth.astDevs[i];
    yyjson_mut_val *pstObj = yyjson_mut_obj(pstDoc);
    yyjson_mut_obj_add_str(pstDoc, pstObj, "id", pstD->acId);
    yyjson_mut_obj_add_str(pstDoc, pstObj, "name", pstD->acName);
    yyjson_mut_obj_add_str(pstDoc, pstObj, "token_hash", pstD->acTokenHash);
    if ('\0' != pstD->acSidecarName[0])
    {
      yyjson_mut_obj_add_str(pstDoc, pstObj, "sidecar_name", pstD->acSidecarName);
    }
    else
    {
      yyjson_mut_obj_add_null(pstDoc, pstObj, "sidecar_name");
    }
    yyjson_mut_obj_add_uint(pstDoc, pstObj, "paired_at", pstD->u64PairedAt);
    yyjson_mut_obj_add_bool(pstDoc, pstObj, "password_saved",
                            pstD->bPasswordSaved ? true : false);
    yyjson_mut_arr_add_val(pstArr, pstObj);
  }
  yyjson_mut_obj_add_val(pstDoc, pstRoot, "devices", pstArr);
  pcJson = dm_json_write_mut(pstDoc, puLen);
  yyjson_mut_doc_free(pstDoc);
  return pcJson;
}

typedef struct af__auth_save_job
{
  char acPath[600];
  char *pcJson;
  size_t uLen;
  int iErr;
  af_store_saved_cb pfOnDone;
  void *pvCtx;
} af__auth_save_job_st;

static void af__auth_save_work(io void *pvArg)
{
  af__auth_save_job_st *pstJob = (af__auth_save_job_st *)pvArg;
  pstJob->iErr = af_store_write_atomic(pstJob->acPath, pstJob->pcJson, pstJob->uLen);
}

static void af__auth_save_after(io void *pvArg, in int iStatus)
{
  af__auth_save_job_st *pstJob = (af__auth_save_job_st *)pvArg;
  int iErr = (0 != iStatus) ? iStatus : pstJob->iErr;
  af_store_saved_cb pfCb = pstJob->pfOnDone;
  void *pvCtx = pstJob->pvCtx;

  dm_json_free_str(pstJob->pcJson);
  dm_free_tag(pstJob);
  af_auth_inflight_dec();
  if (NULL != pfCb)
  {
    pfCb(iErr, pvCtx);
  }
}

int af_auth_store_save_async(in af_store_saved_cb pfOnDone, io void *pvCtx)
{
  af__auth_save_job_st *pstJob = NULL;
  int iErr = DM_SUCCESS;

  if ('\0' == g_stAfAuth.acDataDir[0])
  {
    return DM_INVALID;
  }
  pstJob = (af__auth_save_job_st *)dm_calloc_tag(1u, sizeof(*pstJob), DM_MEM_TAG_JSON);
  if (NULL == pstJob)
  {
    return DM_ALLOC;
  }
  snprintf(pstJob->acPath, sizeof(pstJob->acPath), "%s/devices.json", g_stAfAuth.acDataDir);
  pstJob->pcJson = af__serialize_devices(&pstJob->uLen);
  if (NULL == pstJob->pcJson)
  {
    dm_free_tag(pstJob);
    return DM_ALLOC;
  }
  pstJob->pfOnDone = pfOnDone;
  pstJob->pvCtx = pvCtx;
  af_auth_inflight_inc();
  iErr = dm_pool_submit(DM_LANE_DB, af__auth_save_work, af__auth_save_after, pstJob);
  if (DM_SUCCESS != iErr)
  {
    af_auth_inflight_dec();
    dm_json_free_str(pstJob->pcJson);
    dm_free_tag(pstJob);
  }
  return iErr;
}

int af_auth_store_find_by_id(in const char *pcDevId)
{
  uint32 i = 0u;

  if ((NULL == pcDevId) || ('\0' == pcDevId[0]))
  {
    return -1;
  }
  for (i = 0u; i < g_stAfAuth.u32DevCnt; i++)
  {
    if (0 == strcmp(g_stAfAuth.astDevs[i].acId, pcDevId))
    {
      return (int)i;
    }
  }
  return -1;
}

bool_t af_auth_store_remove_by_id(in const char *pcDevId, out af_device_st *pstRemoved)
{
  int iIdx = af_auth_store_find_by_id(pcDevId);

  if (iIdx < 0)
  {
    return DM_FALSE;
  }
  if (NULL != pstRemoved)
  {
    *pstRemoved = g_stAfAuth.astDevs[iIdx];
  }
  if ((uint32)(iIdx + 1) < g_stAfAuth.u32DevCnt)
  {
    memmove(&g_stAfAuth.astDevs[iIdx], &g_stAfAuth.astDevs[iIdx + 1],
            (size_t)(g_stAfAuth.u32DevCnt - (uint32)iIdx - 1u) * sizeof(af_device_st));
  }
  g_stAfAuth.u32DevCnt--;
  dm_secure_zero(&g_stAfAuth.astDevs[g_stAfAuth.u32DevCnt], sizeof(af_device_st));
  return DM_TRUE;
}

bool_t af_auth_check_https(in const dm_https_msg_st *pstMsg, out af_device_st *pstDevOut)
{
  const char *pcTok = NULL;
  char acHash[65];
  int iFound = -1;
  uint32 i = 0u;

  if ((NULL == pstMsg) || (NULL == pstMsg->pcAuthHeader)
      || (0 != strncmp(pstMsg->pcAuthHeader, "Bearer ", 7u)))
  {
    if (NULL != pstMsg)
    {
      af_https_send_err(pstMsg->u64ReplyId, 401u, "unauthorized", "未配对或令牌无效");
    }
    return DM_FALSE;
  }
  pcTok = pstMsg->pcAuthHeader + 7;
  if (!af__is_hex64(pcTok))
  {
    af_https_send_err(pstMsg->u64ReplyId, 401u, "unauthorized", "未配对或令牌无效");
    return DM_FALSE;
  }
  memset(acHash, 0, sizeof(acHash));
  af_auth_hash_token(pcTok, 64u, acHash);
  if ('\0' != acHash[0])
  {
    for (i = 0u; i < g_stAfAuth.u32DevCnt; i++)
    {
      if (af_auth_ct_eq((const uint8 *)acHash, (const uint8 *)g_stAfAuth.astDevs[i].acTokenHash,
                        64u))
      {
        iFound = (int)i;
      }
    }
  }
  dm_secure_zero(acHash, sizeof(acHash));
  if (iFound < 0)
  {
    af_https_send_err(pstMsg->u64ReplyId, 401u, "unauthorized", "未配对或令牌无效");
    return DM_FALSE;
  }
  if (NULL != pstDevOut)
  {
    *pstDevOut = g_stAfAuth.astDevs[iFound];
  }
  return DM_TRUE;
}
