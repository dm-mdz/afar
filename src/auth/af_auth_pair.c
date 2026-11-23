/**
 ******************************************************************************
 * @file    af_auth_pair.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   10 分钟配对窗、5 次指数锁定、4 待定槽与 mbedTLS EC-JPAKE v:2 两步流实现。
 ******************************************************************************
 */

#define _DARWIN_C_SOURCE
#define DM_LOG_MOD "auth"

#include "af_auth_internal.h"
#include <dm/json.h>
#include <dm/log.h>

#include <mbedtls/base64.h>
#include <mbedtls/md.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#define AF_JPAKE_BUF_MAX 512u
#define AF_JPAKE_B64_MAX 768u

uint32 af_auth_lock_minutes(in uint32 u32Rounds)
{
  uint32 u32Shift = 0u;
  uint32 u32Min = 0u;

  if (0u == u32Rounds)
  {
    return 0u;
  }
  u32Shift = ((u32Rounds - 1u) < 6u) ? (u32Rounds - 1u) : 6u;
  u32Min = 1u << u32Shift;
  return (u32Min > 60u) ? 60u : u32Min;
}

static void af__slot_clear(io af_pair_slot_st *pstSlot)
{
  if (pstSlot->bUsed)
  {
    mbedtls_ecjpake_free(&pstSlot->stJpake);
    dm_secure_zero(pstSlot->acPairId, sizeof(pstSlot->acPairId));
    dm_secure_zero(pstSlot->acDevName, sizeof(pstSlot->acDevName));
    pstSlot->u64ExpireMs = 0u;
    pstSlot->bUsed = DM_FALSE;
  }
}

static void af__slots_clear_all(void)
{
  uint32 i = 0u;

  for (i = 0u; i < AF_PAIR_PENDING_MAX; i++)
  {
    af__slot_clear(&g_stAfAuth.astSlots[i]);
  }
}

static void af__slots_prune(in uint64 u64Now)
{
  uint32 i = 0u;

  for (i = 0u; i < AF_PAIR_PENDING_MAX; i++)
  {
    if (g_stAfAuth.astSlots[i].bUsed && (u64Now >= g_stAfAuth.astSlots[i].u64ExpireMs))
    {
      af__slot_clear(&g_stAfAuth.astSlots[i]);
    }
  }
}

static void af__gen_code6(out char acOut[7], in const char *pcAvoid)
{
  uint32 u32Val = 0u;
  int iTry = 0;

  for (iTry = 0; iTry < 64; iTry++)
  {
    uint8 au8R[4];
    if (DM_SUCCESS != af_auth_rng_bytes(au8R, sizeof(au8R)))
    {
      break;
    }
    u32Val = ((uint32)au8R[0] << 24u) | ((uint32)au8R[1] << 16u) | ((uint32)au8R[2] << 8u)
             | (uint32)au8R[3];
    dm_secure_zero(au8R, sizeof(au8R));
    if (u32Val >= 4294000000u)
    {
      continue;
    }
    snprintf(acOut, 7u, "%06u", (unsigned)(u32Val % 1000000u));
    if ((NULL == pcAvoid) || (0 != strcmp(acOut, pcAvoid)))
    {
      return;
    }
  }
  snprintf(acOut, 7u, "%06u", (unsigned)((u32Val + 1u) % 1000000u));
}

static int af__gen_uuid_v4(out char acUuid[37])
{
  uint8 au8B[16];

  memset(au8B, 0, sizeof(au8B));
  if (DM_SUCCESS != af_auth_rng_bytes(au8B, sizeof(au8B)))
  {
    return DM_FAILD;
  }
  au8B[6] = (uint8)((au8B[6] & 0x0Fu) | 0x40u);
  au8B[8] = (uint8)((au8B[8] & 0x3Fu) | 0x80u);
  snprintf(acUuid, 37u,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           au8B[0], au8B[1], au8B[2], au8B[3], au8B[4], au8B[5], au8B[6], au8B[7], au8B[8],
           au8B[9], au8B[10], au8B[11], au8B[12], au8B[13], au8B[14], au8B[15]);
  dm_secure_zero(au8B, sizeof(au8B));
  return DM_SUCCESS;
}

static int af__hmac_confirm(in const uint8 *pu8Key, in size_t uKeyLen, in const char *pcLabel,
                            in const uint8 au8Fp[32], out uint8 au8Out[32])
{
  const mbedtls_md_info_t *pstInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t stMd;
  int iRc = DM_FAILD;

  if (NULL == pstInfo)
  {
    return DM_FAILD;
  }
  mbedtls_md_init(&stMd);
  if ((0 == mbedtls_md_setup(&stMd, pstInfo, 1))
      && (0 == mbedtls_md_hmac_starts(&stMd, pu8Key, uKeyLen))
      && (0 == mbedtls_md_hmac_update(&stMd, (const unsigned char *)pcLabel, strlen(pcLabel)))
      && (0 == mbedtls_md_hmac_update(&stMd, au8Fp, 32u))
      && (0 == mbedtls_md_hmac_finish(&stMd, au8Out)))
  {
    iRc = DM_SUCCESS;
  }
  mbedtls_md_free(&stMd);
  return iRc;
}

void af_auth_pair_close(in bool_t bKeepLastPaired)
{
  af__slots_clear_all();
  dm_secure_zero(g_stAfAuth.acCode, sizeof(g_stAfAuth.acCode));
  g_stAfAuth.bPairOpen = DM_FALSE;
  g_stAfAuth.u64WinUntilMs = 0u;
  g_stAfAuth.u32Attempts = 0u;
  g_stAfAuth.u32LockRounds = 0u;
  g_stAfAuth.u64LockedUntilMs = 0u;
  if (!bKeepLastPaired)
  {
    g_stAfAuth.acLastPaired[0] = '\0';
  }
}

void af_auth_pair_refresh(void)
{
  uint64 u64Now = af_auth_now_ms();

  af__slots_prune(u64Now);
  if (g_stAfAuth.bPairOpen && (u64Now >= g_stAfAuth.u64WinUntilMs))
  {
    af_auth_pair_close(DM_FALSE);
    return;
  }
  if ((0u != g_stAfAuth.u64LockedUntilMs) && (u64Now >= g_stAfAuth.u64LockedUntilMs))
  {
    g_stAfAuth.u64LockedUntilMs = 0u;
  }
}

void af_auth_pair_open(void)
{
  uint64 u64Now = af_auth_now_ms();

  af_auth_pair_refresh();
  if (g_stAfAuth.bPairOpen)
  {
    if ((0u == g_stAfAuth.u64LockedUntilMs) || (u64Now >= g_stAfAuth.u64LockedUntilMs))
    {
      g_stAfAuth.u64WinUntilMs = u64Now + AF_PAIR_WINDOW_MS;
    }
    return;
  }
  g_stAfAuth.acLastPaired[0] = '\0';
  g_stAfAuth.u32Attempts = 0u;
  g_stAfAuth.u32LockRounds = 0u;
  g_stAfAuth.u64LockedUntilMs = 0u;
  af__gen_code6(g_stAfAuth.acCode, NULL);
  g_stAfAuth.bPairOpen = DM_TRUE;
  g_stAfAuth.u64WinUntilMs = u64Now + AF_PAIR_WINDOW_MS;
}

static void af__trigger_pair_lock(in uint64 u64Now)
{
  char acOld[7];
  uint32 u32Min = 0u;

  memcpy(acOld, g_stAfAuth.acCode, sizeof(acOld));
  g_stAfAuth.u32LockRounds++;
  u32Min = af_auth_lock_minutes(g_stAfAuth.u32LockRounds);
  g_stAfAuth.u64LockedUntilMs = u64Now + ((uint64)u32Min * 60000ULL);
  g_stAfAuth.u32Attempts = 0u;
  af__slots_clear_all();
  af__gen_code6(g_stAfAuth.acCode, acOld);
  dm_secure_zero(acOld, sizeof(acOld));
  af_auth_publish_ev(AF_AUTH_EV_PAIR_WIN, NULL, NULL, NULL);
}

static af_pair_slot_st *af__alloc_slot(void)
{
  uint32 i = 0u;

  for (i = 0u; i < AF_PAIR_PENDING_MAX; i++)
  {
    if (!g_stAfAuth.astSlots[i].bUsed)
    {
      return &g_stAfAuth.astSlots[i];
    }
  }
  return NULL;
}

static int af__pair_start_crypto(io af_pair_slot_st *pstSlot, in const uint8 *pu8CR1,
                                 in size_t uCR1Len, out char acSR1B64[AF_JPAKE_B64_MAX],
                                 out char acSR2B64[AF_JPAKE_B64_MAX])
{
  uint8 au8SR1[AF_JPAKE_BUF_MAX];
  uint8 au8SR2[AF_JPAKE_BUF_MAX];
  size_t uSR1Len = 0u;
  size_t uSR2Len = 0u;
  size_t uB64Len = 0u;
  int iRc = DM_INVALID;

  mbedtls_ecjpake_init(&pstSlot->stJpake);
  if ((0 == mbedtls_ecjpake_setup(&pstSlot->stJpake, MBEDTLS_ECJPAKE_SERVER, MBEDTLS_MD_SHA256,
                                  MBEDTLS_ECP_DP_SECP256R1, (const unsigned char *)g_stAfAuth.acCode,
                                  6u))
      && (0 == mbedtls_ecjpake_read_round_one(&pstSlot->stJpake, pu8CR1, uCR1Len))
      && (0 == mbedtls_ecjpake_write_round_one(&pstSlot->stJpake, au8SR1, sizeof(au8SR1), &uSR1Len,
                                               af_auth_rng_cb, NULL))
      && (0 == mbedtls_ecjpake_write_round_two(&pstSlot->stJpake, au8SR2, sizeof(au8SR2), &uSR2Len,
                                               af_auth_rng_cb, NULL))
      && (0 == mbedtls_base64_encode((unsigned char *)acSR1B64, AF_JPAKE_B64_MAX, &uB64Len, au8SR1,
                                     uSR1Len))
      && (0 == mbedtls_base64_encode((unsigned char *)acSR2B64, AF_JPAKE_B64_MAX, &uB64Len, au8SR2,
                                     uSR2Len)))
  {
    iRc = DM_SUCCESS;
  }
  else
  {
    mbedtls_ecjpake_free(&pstSlot->stJpake);
  }
  dm_secure_zero(au8SR1, sizeof(au8SR1));
  dm_secure_zero(au8SR2, sizeof(au8SR2));
  return iRc;
}

static void af__pair_start_reply(in uint64 u64ReplyId, in const char *pcPairId,
                                 in const char *pcSR1B64, in const char *pcSR2B64)
{
  yyjson_mut_doc *pstDoc = yyjson_mut_doc_new(dm_json_alc());
  yyjson_mut_val *pstRoot = NULL;
  char *pcJson = NULL;
  size_t uLen = 0u;

  if (NULL == pstDoc)
  {
    af_https_send_err(u64ReplyId, 500u, "internal", "内存不足");
    return;
  }
  pstRoot = yyjson_mut_obj(pstDoc);
  yyjson_mut_doc_set_root(pstDoc, pstRoot);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "pair_id", pcPairId);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "s_r1", pcSR1B64);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "s_r2", pcSR2B64);
  pcJson = dm_json_write_mut(pstDoc, &uLen);
  yyjson_mut_doc_free(pstDoc);
  if (NULL != pcJson)
  {
    (void)dm_https_reply(u64ReplyId, 200u, pcJson, (uint32)uLen);
    dm_json_free_str(pcJson);
  }
  else
  {
    af_https_send_err(u64ReplyId, 500u, "internal", "序列化失败");
  }
}

static af_pair_slot_st *af__pair_check_gate(in uint64 u64ReplyId, in uint64 u64Now)
{
  af_pair_slot_st *pstSlot = NULL;

  af_auth_pair_refresh();
  if (!g_stAfAuth.bPairOpen)
  {
    af_https_send_err(u64ReplyId, 403u, "pairing_closed", "配对窗口未打开");
    return NULL;
  }
  if ((0u != g_stAfAuth.u64LockedUntilMs) && (u64Now < g_stAfAuth.u64LockedUntilMs))
  {
    af_https_send_err(u64ReplyId, 429u, "locked", "试错次数太多，请稍后再试");
    return NULL;
  }
  if (g_stAfAuth.u32Attempts >= AF_PAIR_MAX_TRY)
  {
    af__trigger_pair_lock(u64Now);
    af_https_send_err(u64ReplyId, 429u, "locked", "试错次数太多，已换新配对码");
    return NULL;
  }
  if (g_stAfAuth.u32DevCnt >= AF_DEV_MAX)
  {
    af_https_send_err(u64ReplyId, 400u, "bad_request", "已配对设备数量已达上限");
    return NULL;
  }
  pstSlot = af__alloc_slot();
  if (NULL == pstSlot)
  {
    af_https_send_err(u64ReplyId, 429u, "locked", "配对请求过多，请稍后再试");
  }
  return pstSlot;
}

void af_auth_pair_on_start(in const dm_https_msg_st *pstMsg)
{
  int iErr = 0;
  yyjson_doc *pstDoc = NULL;
  yyjson_val *pstRoot = NULL;
  yyjson_val *pstV = NULL;
  const char *pcCR1B64 = NULL;
  const char *pcDevName = NULL;
  uint8 au8CR1[AF_JPAKE_BUF_MAX];
  size_t uCR1Len = 0u;
  uint64 u64Now = af_auth_now_ms();
  af_pair_slot_st *pstSlot = NULL;
  char acSR1B64[AF_JPAKE_B64_MAX];
  char acSR2B64[AF_JPAKE_B64_MAX];
  uint8 au8Pid[16];

  pstDoc = dm_json_read_sensitive((const char *)pstMsg->pu8Body, pstMsg->u32BodyLen, 65536u, &iErr);
  if ((NULL == pstDoc) || (!yyjson_is_obj(yyjson_doc_get_root(pstDoc))))
  {
    if (NULL != pstDoc) { yyjson_doc_free(pstDoc); }
    af_https_send_err(pstMsg->u64ReplyId, 400u, "bad_request", "请求格式不对");
    return;
  }
  pstRoot = yyjson_doc_get_root(pstDoc);
  pstV = yyjson_obj_get(pstRoot, "v");
  if ((!yyjson_is_int(pstV)) || (2 != yyjson_get_int(pstV)))
  {
    yyjson_doc_free(pstDoc);
    af_https_send_err(pstMsg->u64ReplyId, 426u, "upgrade_required",
                      "请升级 iPad 上的 Afar App 后再配对");
    return;
  }
  pstSlot = af__pair_check_gate(pstMsg->u64ReplyId, u64Now);
  if (NULL == pstSlot)
  {
    yyjson_doc_free(pstDoc);
    return;
  }
  pcCR1B64 = yyjson_get_str(yyjson_obj_get(pstRoot, "c_r1"));
  pcDevName = yyjson_get_str(yyjson_obj_get(pstRoot, "device_name"));
  if ((NULL == pcCR1B64) || ('\0' == pcCR1B64[0])
      || (0 != mbedtls_base64_decode(au8CR1, sizeof(au8CR1), &uCR1Len,
                                     (const unsigned char *)pcCR1B64, strlen(pcCR1B64)))
      || (0u == uCR1Len))
  {
    yyjson_doc_free(pstDoc);
    af_https_send_err(pstMsg->u64ReplyId, 400u, "bad_request", "无效的配对参数");
    return;
  }
  af_auth_trunc_utf8_chars(((NULL != pcDevName) && ('\0' != pcDevName[0])) ? pcDevName : "iPad",
                           pstSlot->acDevName, sizeof(pstSlot->acDevName), 64u);
  yyjson_doc_free(pstDoc);

  if (DM_SUCCESS != af__pair_start_crypto(pstSlot, au8CR1, uCR1Len, acSR1B64, acSR2B64))
  {
    dm_secure_zero(au8CR1, sizeof(au8CR1));
    af_https_send_err(pstMsg->u64ReplyId, 400u, "bad_request", "无效的配对握手");
    return;
  }
  dm_secure_zero(au8CR1, sizeof(au8CR1));
  memset(au8Pid, 0, sizeof(au8Pid));
  if (DM_SUCCESS != af_auth_rng_bytes(au8Pid, sizeof(au8Pid)))
  {
    mbedtls_ecjpake_free(&pstSlot->stJpake);
    dm_secure_zero(pstSlot->acDevName, sizeof(pstSlot->acDevName));
    af_https_send_err(pstMsg->u64ReplyId, 500u, "internal", "随机数生成失败");
    return;
  }
  g_stAfAuth.u32Attempts++;
  af_auth_hex_encode(au8Pid, sizeof(au8Pid), pstSlot->acPairId);
  dm_secure_zero(au8Pid, sizeof(au8Pid));
  pstSlot->u64ExpireMs = u64Now + AF_PAIR_PENDING_MS;
  pstSlot->bUsed = DM_TRUE;
  af__pair_start_reply(pstMsg->u64ReplyId, pstSlot->acPairId, acSR1B64, acSR2B64);
}

typedef struct af__pair_fin_ctx
{
  uint64 u64ReplyId;
  char acDevId[37];
  char acDevName[65];
  char acTok[65];
  uint8 au8SConfirm[32];
} af__pair_fin_ctx_st;

static void af__pair_fin_on_saved(in int iStatus, io void *pvCtx)
{
  af__pair_fin_ctx_st *pstC = (af__pair_fin_ctx_st *)pvCtx;
  char acSConfB64[64];
  size_t uB64Len = 0u;

  if (DM_SUCCESS != iStatus)
  {
    (void)af_auth_store_remove_by_id(pstC->acDevId, NULL);
    af_https_send_err(pstC->u64ReplyId, 500u, "internal", "保存设备失败");
  }
  else if (0 == mbedtls_base64_encode((unsigned char *)acSConfB64, sizeof(acSConfB64), &uB64Len,
                                      pstC->au8SConfirm, sizeof(pstC->au8SConfirm)))
  {
    yyjson_mut_doc *pstDoc = yyjson_mut_doc_new(dm_json_alc_sensitive());
    yyjson_mut_val *pstRoot = (NULL != pstDoc) ? yyjson_mut_obj(pstDoc) : NULL;
    char *pcJson = NULL;
    size_t uLen = 0u;
    if ((NULL != pstDoc) && (NULL != pstRoot))
    {
      yyjson_mut_doc_set_root(pstDoc, pstRoot);
      yyjson_mut_obj_add_str(pstDoc, pstRoot, "device_id", pstC->acDevId);
      yyjson_mut_obj_add_str(pstDoc, pstRoot, "token", pstC->acTok);
      yyjson_mut_obj_add_str(pstDoc, pstRoot, "s_confirm", acSConfB64);
      pcJson = dm_json_write_mut_sensitive(pstDoc, &uLen);
    }
    if (NULL != pstDoc) { yyjson_mut_doc_free(pstDoc); }
    if (NULL != pcJson)
    {
      (void)dm_https_reply(pstC->u64ReplyId, 200u, pcJson, (uint32)uLen);
      dm_json_free_str_sensitive(pcJson);
      af_plat_notify("新 iPad 已配对", pstC->acDevName, "pair");
      af_auth_publish_ev(AF_AUTH_EV_PAIRED, pstC->acDevId, pstC->acDevName, NULL);
    }
    else
    {
      af_https_send_err(pstC->u64ReplyId, 500u, "internal", "序列化失败");
    }
  }
  dm_secure_zero(acSConfB64, sizeof(acSConfB64));
  dm_secure_zero(pstC, sizeof(*pstC));
  dm_free_tag(pstC);
}

static af_pair_slot_st *af__find_slot(in const char *pcPairId)
{
  uint32 i = 0u;

  if ((NULL == pcPairId) || (32u != strlen(pcPairId)))
  {
    return NULL;
  }
  for (i = 0u; i < AF_PAIR_PENDING_MAX; i++)
  {
    if (g_stAfAuth.astSlots[i].bUsed
        && af_auth_ct_eq((const uint8 *)g_stAfAuth.astSlots[i].acPairId, (const uint8 *)pcPairId,
                         32u))
    {
      return &g_stAfAuth.astSlots[i];
    }
  }
  return NULL;
}

static int af__pair_parse_finish(in const dm_https_msg_st *pstMsg, out char acPairId[33],
                                 out uint8 au8CR2[AF_JPAKE_BUF_MAX], out size_t *puCR2Len,
                                 out uint8 au8CConf[32])
{
  int iErr = 0;
  yyjson_doc *pstDoc = dm_json_read_sensitive((const char *)pstMsg->pu8Body, pstMsg->u32BodyLen,
                                              65536u, &iErr);
  yyjson_val *pstRoot = NULL;
  const char *pcPid = NULL;
  const char *pcCR2 = NULL;
  const char *pcConf = NULL;
  size_t uConfLen = 0u;
  int iRc = DM_INVALID;

  if ((NULL == pstDoc) || (!yyjson_is_obj(yyjson_doc_get_root(pstDoc))))
  {
    if (NULL != pstDoc) { yyjson_doc_free(pstDoc); }
    return DM_INVALID;
  }
  pstRoot = yyjson_doc_get_root(pstDoc);
  pcPid = yyjson_get_str(yyjson_obj_get(pstRoot, "pair_id"));
  pcCR2 = yyjson_get_str(yyjson_obj_get(pstRoot, "c_r2"));
  pcConf = yyjson_get_str(yyjson_obj_get(pstRoot, "c_confirm"));
  if ((NULL != pcPid) && (32u == strlen(pcPid)) && (NULL != pcCR2) && (NULL != pcConf)
      && (0 == mbedtls_base64_decode(au8CR2, AF_JPAKE_BUF_MAX, puCR2Len,
                                     (const unsigned char *)pcCR2, strlen(pcCR2)))
      && (*puCR2Len > 0u)
      && (0 == mbedtls_base64_decode(au8CConf, 32u, &uConfLen, (const unsigned char *)pcConf,
                                     strlen(pcConf)))
      && (32u == uConfLen))
  {
    memcpy(acPairId, pcPid, 33u);
    iRc = DM_SUCCESS;
  }
  yyjson_doc_free(pstDoc);
  return iRc;
}

static void af__pair_commit_device(in uint64 u64ReplyId, in const char *pcDevName,
                                   in const uint8 au8SConfirm[32])
{
  af__pair_fin_ctx_st *pstC = NULL;
  af_device_st *pstDev = NULL;
  uint8 au8TokRaw[32];

  if (g_stAfAuth.u32DevCnt >= AF_DEV_MAX)
  {
    af_https_send_err(u64ReplyId, 400u, "bad_request", "已配对设备数量已达上限");
    return;
  }
  pstC = (af__pair_fin_ctx_st *)dm_calloc_tag(1u, sizeof(*pstC), DM_MEM_TAG_SECRET);
  if (NULL == pstC)
  {
    af_https_send_err(u64ReplyId, 500u, "internal", "内存不足");
    return;
  }
  pstC->u64ReplyId = u64ReplyId;
  memset(au8TokRaw, 0, sizeof(au8TokRaw));
  if ((DM_SUCCESS != af__gen_uuid_v4(pstC->acDevId))
      || (DM_SUCCESS != af_auth_rng_bytes(au8TokRaw, sizeof(au8TokRaw))))
  {
    dm_secure_zero(au8TokRaw, sizeof(au8TokRaw));
    dm_secure_zero(pstC, sizeof(*pstC));
    dm_free_tag(pstC);
    af_https_send_err(u64ReplyId, 500u, "internal", "随机数生成失败");
    return;
  }
  snprintf(pstC->acDevName, sizeof(pstC->acDevName), "%s", pcDevName);
  af_auth_hex_encode(au8TokRaw, sizeof(au8TokRaw), pstC->acTok);
  dm_secure_zero(au8TokRaw, sizeof(au8TokRaw));
  memcpy(pstC->au8SConfirm, au8SConfirm, 32u);

  pstDev = &g_stAfAuth.astDevs[g_stAfAuth.u32DevCnt++];
  memset(pstDev, 0, sizeof(*pstDev));
  snprintf(pstDev->acId, sizeof(pstDev->acId), "%s", pstC->acDevId);
  snprintf(pstDev->acName, sizeof(pstDev->acName), "%s", pstC->acDevName);
  af_auth_hash_token(pstC->acTok, 64u, pstDev->acTokenHash);
  pstDev->u64PairedAt = (uint64)time(NULL);
  pstDev->bPasswordSaved = DM_FALSE;

  snprintf(g_stAfAuth.acLastPaired, sizeof(g_stAfAuth.acLastPaired), "%s", pstC->acDevName);
  af_auth_pair_close(DM_TRUE);
  if (DM_SUCCESS != af_auth_store_save_async(af__pair_fin_on_saved, pstC))
  {
    (void)af_auth_store_remove_by_id(pstC->acDevId, NULL);
    dm_secure_zero(pstC, sizeof(*pstC));
    dm_free_tag(pstC);
    af_https_send_err(u64ReplyId, 500u, "internal", "保存设备失败");
  }
}

void af_auth_pair_on_finish(in const dm_https_msg_st *pstMsg)
{
  char acPairId[33];
  char acDevName[65];
  uint8 au8CR2[AF_JPAKE_BUF_MAX];
  size_t uCR2Len = 0u;
  uint8 au8CConf[32];
  uint8 au8Secret[64];
  size_t uSecLen = 0u;
  uint8 au8ExpC[32];
  uint8 au8SConf[32];
  af_pair_slot_st *pstSlot = NULL;
  int iR2Rc = 0;
  int iDerRc = 0;

  if (DM_SUCCESS != af__pair_parse_finish(pstMsg, acPairId, au8CR2, &uCR2Len, au8CConf))
  {
    af_https_send_err(pstMsg->u64ReplyId, 400u, "bad_request", "请求格式不对");
    return;
  }
  af_auth_pair_refresh();
  pstSlot = af__find_slot(acPairId);
  if (NULL == pstSlot)
  {
    dm_secure_zero(au8CR2, sizeof(au8CR2));
    af_https_send_err(pstMsg->u64ReplyId, 400u, "bad_request", "配对会话已失效");
    return;
  }
  snprintf(acDevName, sizeof(acDevName), "%s", pstSlot->acDevName);
  iR2Rc = mbedtls_ecjpake_read_round_two(&pstSlot->stJpake, au8CR2, uCR2Len);
  if (0 == iR2Rc)
  {
    iDerRc = mbedtls_ecjpake_derive_secret(&pstSlot->stJpake, au8Secret, sizeof(au8Secret),
                                           &uSecLen, af_auth_rng_cb, NULL);
  }
  /* 无论成败立即原子摘除槽位，彻底杜绝 pair_id 重放 */
  af__slot_clear(pstSlot);
  dm_secure_zero(au8CR2, sizeof(au8CR2));

  if ((0 != iR2Rc) || (0 != iDerRc) || (0u == uSecLen))
  {
    dm_secure_zero(au8Secret, sizeof(au8Secret));
    af_https_send_err(pstMsg->u64ReplyId, 400u, "bad_request", "无效的配对握手");
    return;
  }
  if ((DM_SUCCESS != af__hmac_confirm(au8Secret, uSecLen, "client", g_stAfAuth.au8OwnCertFp,
                                      au8ExpC))
      || (!af_auth_ct_eq(au8ExpC, au8CConf, 32u)))
  {
    dm_secure_zero(au8Secret, sizeof(au8Secret));
    dm_secure_zero(au8ExpC, sizeof(au8ExpC));
    af_https_send_err(pstMsg->u64ReplyId, 403u, "wrong_code", "配对码不对");
    return;
  }
  if (DM_SUCCESS != af__hmac_confirm(au8Secret, uSecLen, "server", g_stAfAuth.au8OwnCertFp,
                                     au8SConf))
  {
    dm_secure_zero(au8Secret, sizeof(au8Secret));
    dm_secure_zero(au8ExpC, sizeof(au8ExpC));
    af_https_send_err(pstMsg->u64ReplyId, 500u, "internal", "内部错误");
    return;
  }
  dm_secure_zero(au8Secret, sizeof(au8Secret));
  dm_secure_zero(au8ExpC, sizeof(au8ExpC));
  af__pair_commit_device(pstMsg->u64ReplyId, acDevName, au8SConf);
  dm_secure_zero(au8SConf, sizeof(au8SConf));
}
