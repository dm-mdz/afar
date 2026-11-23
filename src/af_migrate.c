/**
 ******************************************************************************
 * @file    af_migrate.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   老版本 Rust 0.4.12 无损幂等迁移实现（DD-05 §4，严禁污染宿主机）。
 ******************************************************************************
 */

#define _DARWIN_C_SOURCE
#define DM_LOG_MOD "migrate"

#include "af_migrate.h"
#include "af_store.h"
#include "plat/af_plat.h"

#include <dm/log.h>
#include <dm/mem.h>
#include <yyjson.h>

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static af_migrate_ops_st g_stMigOps;

void af_migrate_set_ops(in const af_migrate_ops_st *pstOps)
{
  if (NULL == pstOps)
  {
    memset(&g_stMigOps, 0, sizeof(g_stMigOps));
  }
  else
  {
    g_stMigOps = *pstOps;
  }
}

static bool_t af__file_exists(in const char *pcPath)
{
  struct stat stSb;
  return (0 == stat(pcPath, &stSb)) ? DM_TRUE : DM_FALSE;
}

static char *af__read_all(in const char *pcPath, out size_t *puLen)
{
  FILE *pf = fopen(pcPath, "rb");
  long lSz = 0;
  char *pcBuf = NULL;

  *puLen = 0u;
  if (NULL == pf) { return NULL; }
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

/* ============================ 1. config.toml -> config.json ============================ */

static const char *af__skip_ws(in const char *pcP)
{
  while (('\0' != *pcP) && ((' ' == *pcP) || ('\t' == *pcP) || ('\r' == *pcP)))
  {
    pcP++;
  }
  return pcP;
}

static bool_t af__parse_toml_str(in const char *pcVal, out char *pcOut, in size_t uCap)
{
  char cQ = '\0';
  size_t uPos = 0u;
  const char *pcP = af__skip_ws(pcVal);

  if (('"' != *pcP) && ('\'' != *pcP))
  {
    return DM_FALSE;
  }
  cQ = *pcP++;
  while (('\0' != *pcP) && (cQ != *pcP))
  {
    char cCh = *pcP++;
    if (('\\' == cCh) && ('"' == cQ) && ('\0' != *pcP))
    {
      cCh = *pcP++;
    }
    if ((uPos + 1u) < uCap)
    {
      pcOut[uPos++] = cCh;
    }
  }
  if (cQ != *pcP)
  {
    return DM_FALSE;
  }
  pcOut[uPos] = '\0';
  return DM_TRUE;
}

static bool_t af__parse_u32_pair(in const char *pcS, out const char **ppcEnd, out uint32 *pu32A,
                                 out uint32 *pu32B)
{
  char *pcE1 = NULL;
  char *pcE2 = NULL;
  unsigned long ulA = 0ul;
  unsigned long ulB = 0ul;
  const char *pcP = pcS;

  while (('\0' != *pcP) && ('[' != *pcP)) { pcP++; }
  if ('[' != *pcP) { return DM_FALSE; }
  pcP++;
  while (('\0' != *pcP) && (0 != isspace((unsigned char)*pcP))) { pcP++; }
  if (0 == isdigit((unsigned char)*pcP)) { return DM_FALSE; }
  errno = 0;
  ulA = strtoul(pcP, &pcE1, 10);
  if ((ERANGE == errno) || (ulA > UINT32_MAX)) { return DM_FALSE; }
  pcP = pcE1;
  while (('\0' != *pcP) && (0 != isspace((unsigned char)*pcP))) { pcP++; }
  if (',' != *pcP) { return DM_FALSE; }
  pcP++;
  while (('\0' != *pcP) && (0 != isspace((unsigned char)*pcP))) { pcP++; }
  if (0 == isdigit((unsigned char)*pcP)) { return DM_FALSE; }
  errno = 0;
  ulB = strtoul(pcP, &pcE2, 10);
  if ((ERANGE == errno) || (ulB > UINT32_MAX)) { return DM_FALSE; }
  pcP = pcE2;
  while (('\0' != *pcP) && (0 != isspace((unsigned char)*pcP))) { pcP++; }
  if (']' != *pcP) { return DM_FALSE; }
  *pu32A = (uint32)ulA;
  *pu32B = (uint32)ulB;
  if (NULL != ppcEnd) { *ppcEnd = pcP + 1; }
  return DM_TRUE;
}

static void af__parse_extra_sizes(in const char *pcVal, io af_config_st *pstCfg)
{
  const char *pcP = strchr(pcVal, '[');
  if (NULL == pcP) { return; }
  pcP++;
  while (('\0' != *pcP) && (']' != *pcP) && (pstCfg->u32ExtraCnt < 16u))
  {
    if ('[' == *pcP)
    {
      uint32 u32W = 0u;
      uint32 u32H = 0u;
      const char *pcNext = NULL;
      if (!af__parse_u32_pair(pcP, &pcNext, &u32W, &u32H))
      {
        break;
      }
      if ((u32W >= 640u) && (u32W <= 7680u) && (u32H >= 480u) && (u32H <= 4320u)
          && (0u == (u32W % 2u)) && (0u == (u32H % 2u)))
      {
        pstCfg->aau32Extra[pstCfg->u32ExtraCnt][0] = u32W;
        pstCfg->aau32Extra[pstCfg->u32ExtraCnt][1] = u32H;
        pstCfg->u32ExtraCnt++;
      }
      pcP = pcNext;
    }
    else
    {
      pcP++;
    }
  }
}

static void af__apply_toml_kv(in const char *pcKey, in const char *pcVal, io af_config_st *pstCfg)
{
  char acBuf[128];

  if (0 == strcmp(pcKey, "ipad_name"))
  {
    if (af__parse_toml_str(pcVal, acBuf, sizeof(acBuf)))
    {
      snprintf(pstCfg->acIpadName, sizeof(pstCfg->acIpadName), "%s", acBuf);
    }
  }
  else if (0 == strcmp(pcKey, "scale"))
  {
    if (af__parse_toml_str(pcVal, acBuf, sizeof(acBuf))
        && ((0 == strcmp(acBuf, "auto")) || (0 == strcmp(acBuf, "larger"))
            || (0 == strcmp(acBuf, "default")) || (0 == strcmp(acBuf, "more"))))
    {
      snprintf(pstCfg->acScale, sizeof(pstCfg->acScale), "%s", acBuf);
    }
  }
  else if (0 == strcmp(pcKey, "auto_connect"))
  {
    if (0 == strncmp(pcVal, "true", 4u)) { pstCfg->bAutoConnect = DM_TRUE; }
    else if (0 == strncmp(pcVal, "false", 5u)) { pstCfg->bAutoConnect = DM_FALSE; }
  }
  else if (0 == strcmp(pcKey, "mirror"))
  {
    if (0 == strncmp(pcVal, "true", 4u)) { pstCfg->bMirror = DM_TRUE; }
    else if (0 == strncmp(pcVal, "false", 5u)) { pstCfg->bMirror = DM_FALSE; }
  }
  else if (0 == strcmp(pcKey, "port"))
  {
    unsigned long ulP = strtoul(pcVal, NULL, 10);
    if ((ulP >= 1024ul) && (ulP <= 65535ul))
    {
      pstCfg->u16Port = (uint16)ulP;
    }
  }
  else if (0 == strcmp(pcKey, "ipad_pixels"))
  {
    uint32 u32W = 0u;
    uint32 u32H = 0u;
    if (af__parse_u32_pair(pcVal, NULL, &u32W, &u32H) && (u32W >= 400u) && (u32W <= 8000u)
        && (u32H >= 400u) && (u32H <= 8000u))
    {
      pstCfg->au32IpadPixels[0] = u32W;
      pstCfg->au32IpadPixels[1] = u32H;
    }
  }
  else if (0 == strcmp(pcKey, "extra_sizes"))
  {
    af__parse_extra_sizes(pcVal, pstCfg);
  }
}

static void af__parse_config_toml_text(in const char *pcText, out af_config_st *pstCfg)
{
  const char *pcP = pcText;

  memset(pstCfg, 0, sizeof(*pstCfg));
  snprintf(pstCfg->acScale, sizeof(pstCfg->acScale), "auto");
  pstCfg->bAutoConnect = DM_TRUE;
  pstCfg->bMirror = DM_TRUE;
  pstCfg->au32IpadPixels[0] = 2360u;
  pstCfg->au32IpadPixels[1] = 1640u;
  pstCfg->u16Port = 47911u;

  while ('\0' != *pcP)
  {
    const char *pcLine = af__skip_ws(pcP);
    const char *pcNl = strchr(pcLine, '\n');
    const char *pcEq = NULL;
    char acKey[64];
    size_t uKLen = 0u;

    if (('\0' == *pcLine) || ('\n' == *pcLine) || ('#' == *pcLine) || ('[' == *pcLine))
    {
      if (NULL == pcNl) { break; }
      pcP = pcNl + 1;
      continue;
    }
    pcEq = strchr(pcLine, '=');
    if ((NULL == pcEq) || ((NULL != pcNl) && (pcEq > pcNl)))
    {
      DM_LOG_WARN("ignoring malformed config.toml line");
      if (NULL == pcNl) { break; }
      pcP = pcNl + 1;
      continue;
    }
    {
      const char *pcKe = pcEq;
      while ((pcKe > pcLine) && ((' ' == pcKe[-1]) || ('\t' == pcKe[-1]))) { pcKe--; }
      uKLen = (size_t)(pcKe - pcLine);
      if ((uKLen > 0u) && (uKLen < sizeof(acKey)))
      {
        const char *pcVal = af__skip_ws(pcEq + 1);
        memcpy(acKey, pcLine, uKLen);
        acKey[uKLen] = '\0';
        af__apply_toml_kv(acKey, pcVal, pstCfg);
      }
    }
    if (NULL == pcNl) { break; }
    pcP = pcNl + 1;
  }
}

static int af__write_config_json(in const char *pcPath, in const af_config_st *pstCfg)
{
  yyjson_mut_doc *pstDoc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *pstRoot = NULL;
  yyjson_mut_val *pstPx = NULL;
  yyjson_mut_val *pstEx = NULL;
  char *pcJson = NULL;
  size_t uLen = 0u;
  uint32 i = 0u;
  int iRc = DM_SUCCESS;

  if (NULL == pstDoc) { return DM_ALLOC; }
  pstRoot = yyjson_mut_obj(pstDoc);
  yyjson_mut_doc_set_root(pstDoc, pstRoot);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "ipad_name", pstCfg->acIpadName);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "auto_connect", pstCfg->bAutoConnect ? true : false);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "scale", pstCfg->acScale);
  pstPx = yyjson_mut_arr(pstDoc);
  yyjson_mut_arr_add_uint(pstDoc, pstPx, pstCfg->au32IpadPixels[0]);
  yyjson_mut_arr_add_uint(pstDoc, pstPx, pstCfg->au32IpadPixels[1]);
  yyjson_mut_obj_add_val(pstDoc, pstRoot, "ipad_pixels", pstPx);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "mirror", pstCfg->bMirror ? true : false);
  yyjson_mut_obj_add_uint(pstDoc, pstRoot, "port", pstCfg->u16Port);
  pstEx = yyjson_mut_arr(pstDoc);
  for (i = 0u; i < pstCfg->u32ExtraCnt; i++)
  {
    yyjson_mut_val *pstPair = yyjson_mut_arr(pstDoc);
    yyjson_mut_arr_add_uint(pstDoc, pstPair, pstCfg->aau32Extra[i][0]);
    yyjson_mut_arr_add_uint(pstDoc, pstPair, pstCfg->aau32Extra[i][1]);
    yyjson_mut_arr_add_val(pstEx, pstPair);
  }
  yyjson_mut_obj_add_val(pstDoc, pstRoot, "extra_sizes", pstEx);
  pcJson = yyjson_mut_write(pstDoc, YYJSON_WRITE_PRETTY, &uLen);
  yyjson_mut_doc_free(pstDoc);
  if (NULL == pcJson) { return DM_ALLOC; }
  iRc = af_store_write_atomic(pcPath, pcJson, uLen);
  free(pcJson);
  return iRc;
}

static bool_t af__migrate_config_toml(in const char *pcDataDir, out bool_t *pbAutoConnect)
{
  char acToml[600];
  char acJson[600];
  char acBak[600];
  size_t uLen = 0u;
  char *pcText = NULL;
  af_config_st stCfg;

  *pbAutoConnect = DM_TRUE;
  snprintf(acToml, sizeof(acToml), "%s/config.toml", pcDataDir);
  snprintf(acJson, sizeof(acJson), "%s/config.json", pcDataDir);
  snprintf(acBak, sizeof(acBak), "%s/config.toml.bak", pcDataDir);

  if (af__file_exists(acJson) || (!af__file_exists(acToml)))
  {
    return DM_FALSE;
  }
  pcText = af__read_all(acToml, &uLen);
  if (NULL == pcText)
  {
    return DM_FALSE;
  }
  af__parse_config_toml_text(pcText, &stCfg);
  dm_free_tag(pcText);
  *pbAutoConnect = stCfg.bAutoConnect;
  stCfg.bAutoConnect = DM_TRUE;
  if (DM_SUCCESS != af__write_config_json(acJson, &stCfg))
  {
    DM_LOG_WARN("failed to write migrated config.json");
    return DM_FALSE;
  }
  if (!af__file_exists(acBak))
  {
    (void)rename(acToml, acBak);
  }
  return DM_TRUE;
}

/* ============================ 2. state.json 14 键兼容与开关合并 ============================ */

static void af__add_scale_key_if_missing(io yyjson_mut_doc *pstDoc, io yyjson_mut_val *pstScales,
                                         in const char *pcIpad, in const char *pcScale)
{
  if ((NULL == pcIpad) || ('\0' == pcIpad[0]))
  {
    return;
  }
  if (NULL == yyjson_mut_obj_get(pstScales, pcIpad))
  {
    yyjson_mut_obj_add_strcpy(pstDoc, pstScales, pcIpad, pcScale);
  }
}

static bool_t af__populate_ipad_scales(io yyjson_mut_doc *pstDoc, io yyjson_mut_val *pstRoot)
{
  yyjson_mut_val *pstScaleVal = yyjson_mut_obj_get(pstRoot, "scale");
  yyjson_mut_val *pstScales = yyjson_mut_obj_get(pstRoot, "ipad_scales");
  const char *pcScale = (NULL != pstScaleVal) ? yyjson_mut_get_str(pstScaleVal) : NULL;
  bool_t bNeed = DM_FALSE;

  if ((NULL == pcScale)
      || ((0 != strcmp(pcScale, "larger")) && (0 != strcmp(pcScale, "default"))
          && (0 != strcmp(pcScale, "more"))))
  {
    return DM_FALSE;
  }
  if ((NULL != pstScales) && yyjson_mut_is_obj(pstScales) && (yyjson_mut_obj_size(pstScales) > 0u))
  {
    return DM_FALSE;
  }
  if ((NULL == pstScales) || (!yyjson_mut_is_obj(pstScales)))
  {
    pstScales = yyjson_mut_obj(pstDoc);
    yyjson_mut_obj_put(pstRoot, yyjson_mut_str(pstDoc, "ipad_scales"), pstScales);
  }
  {
    yyjson_mut_val *pstLast = yyjson_mut_obj_get(pstRoot, "last_ipad");
    const char *pcLast = (NULL != pstLast) ? yyjson_mut_get_str(pstLast) : NULL;
    if ((NULL != pcLast) && ('\0' != pcLast[0]))
    {
      af__add_scale_key_if_missing(pstDoc, pstScales, pcLast, pcScale);
      bNeed = DM_TRUE;
    }
  }
  {
    static const char *const apcMaps[2] = {"ipad_screens", "ipad_sizes"};
    int m = 0;
    for (m = 0; m < 2; m++)
    {
      yyjson_mut_val *pstMap = yyjson_mut_obj_get(pstRoot, apcMaps[m]);
      if ((NULL != pstMap) && yyjson_mut_is_obj(pstMap))
      {
        size_t uIdx = 0u;
        size_t uMax = 0u;
        yyjson_mut_val *pstK = NULL;
        yyjson_mut_val *pstV = NULL;
        yyjson_mut_obj_foreach(pstMap, uIdx, uMax, pstK, pstV)
        {
          (void)pstV;
          af__add_scale_key_if_missing(pstDoc, pstScales, yyjson_mut_get_str(pstK), pcScale);
          bNeed = DM_TRUE;
        }
      }
    }
  }
  return bNeed;
}

static void af__migrate_state_json(in const char *pcDataDir, in bool_t bMigratedCfg,
                                   in bool_t bCfgAutoConnect)
{
  char acPath[600];
  size_t uLen = 0u;
  char *pcText = NULL;
  yyjson_doc *pstDoc = NULL;
  yyjson_mut_doc *pstMDoc = NULL;
  yyjson_mut_val *pstRoot = NULL;
  bool_t bDirty = DM_FALSE;

  snprintf(acPath, sizeof(acPath), "%s/state.json", pcDataDir);
  pcText = af__read_all(acPath, &uLen);
  if (NULL == pcText)
  {
    if (bMigratedCfg && (!bCfgAutoConnect))
    {
      static const char acInit[] = "{\"no_boot_connect\":true}\n";
      (void)af_store_write_atomic(acPath, acInit, sizeof(acInit) - 1u);
    }
    return;
  }
  pstDoc = yyjson_read(pcText, uLen, 0u);
  dm_free_tag(pcText);
  if ((NULL == pstDoc) || (!yyjson_is_obj(yyjson_doc_get_root(pstDoc))))
  {
    DM_LOG_WARN("corrupt state.json during migration, skipping");
    if (NULL != pstDoc) { yyjson_doc_free(pstDoc); }
    return;
  }
  pstMDoc = yyjson_doc_mut_copy(pstDoc, NULL);
  yyjson_doc_free(pstDoc);
  if (NULL == pstMDoc) { return; }
  pstRoot = yyjson_mut_doc_get_root(pstMDoc);
  if (bMigratedCfg && (!bCfgAutoConnect))
  {
    yyjson_mut_val *pstNb = yyjson_mut_obj_get(pstRoot, "no_boot_connect");
    if ((NULL == pstNb) || (!yyjson_mut_get_bool(pstNb)))
    {
      yyjson_mut_obj_put(pstRoot, yyjson_mut_str(pstMDoc, "no_boot_connect"),
                         yyjson_mut_bool(pstMDoc, true));
      bDirty = DM_TRUE;
    }
  }
  if (af__populate_ipad_scales(pstMDoc, pstRoot))
  {
    bDirty = DM_TRUE;
  }
  if (bDirty)
  {
    size_t uOutLen = 0u;
    char *pcOut = yyjson_mut_write(pstMDoc, YYJSON_WRITE_PRETTY, &uOutLen);
    if (NULL != pcOut)
    {
      (void)af_store_write_atomic(acPath, pcOut, uOutLen);
      free(pcOut);
    }
  }
  yyjson_mut_doc_free(pstMDoc);
}

/* ============================ 3. devices.json 与旧钥匙串迁移 ============================ */

static void af__del_legacy_keychain(void)
{
  if (NULL != g_stMigOps.pfKeychainDelLegacyPw)
  {
    g_stMigOps.pfKeychainDelLegacyPw();
  }
  else
  {
    af_plat_keychain_del_legacy_pw();
  }
}

static int af__rewrite_devices_arr(in const char *pcPath, in yyjson_val *pstArr,
                                   in bool_t bForceWrite, in bool_t bHasLegacy)
{
  yyjson_mut_doc *pstMDoc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *pstMRoot = NULL;
  yyjson_mut_val *pstMArr = NULL;
  yyjson_val *pstItem = NULL;
  size_t uIdx = 0u;
  size_t uMax = 0u;
  bool_t bNeedWrite = bForceWrite;
  int iRc = DM_SUCCESS;

  if (NULL == pstMDoc) { return DM_ALLOC; }
  pstMRoot = yyjson_mut_obj(pstMDoc);
  pstMArr = yyjson_mut_arr(pstMDoc);
  yyjson_mut_doc_set_root(pstMDoc, pstMRoot);
  yyjson_arr_foreach(pstArr, uIdx, uMax, pstItem)
  {
    yyjson_mut_val *pstCopy = yyjson_val_mut_copy(pstMDoc, pstItem);
    if ((NULL != pstCopy) && yyjson_mut_is_obj(pstCopy))
    {
      yyjson_mut_val *pstPw = yyjson_mut_obj_get(pstCopy, "password_saved");
      if (bHasLegacy)
      {
        yyjson_mut_obj_put(pstCopy, yyjson_mut_str(pstMDoc, "password_saved"),
                           yyjson_mut_bool(pstMDoc, true));
        bNeedWrite = DM_TRUE;
      }
      else if (NULL == pstPw)
      {
        yyjson_mut_obj_put(pstCopy, yyjson_mut_str(pstMDoc, "password_saved"),
                           yyjson_mut_bool(pstMDoc, false));
        bNeedWrite = DM_TRUE;
      }
      yyjson_mut_arr_add_val(pstMArr, pstCopy);
    }
  }
  yyjson_mut_obj_add_val(pstMDoc, pstMRoot, "devices", pstMArr);
  if (bNeedWrite)
  {
    size_t uOutLen = 0u;
    char *pcOut = yyjson_mut_write(pstMDoc, YYJSON_WRITE_PRETTY, &uOutLen);
    if (NULL == pcOut)
    {
      iRc = DM_ALLOC;
    }
    else
    {
      iRc = af_store_write_atomic(pcPath, pcOut, uOutLen);
      free(pcOut);
    }
  }
  yyjson_mut_doc_free(pstMDoc);
  return iRc;
}

static void af__migrate_devices_and_keychain(in const char *pcDataDir)
{
  bool_t bHasLegacy = (NULL != g_stMigOps.pfKeychainHasLegacyPw)
                          ? g_stMigOps.pfKeychainHasLegacyPw()
                          : af_plat_keychain_has_legacy_pw();
  bool_t bCanDelKeychain = DM_TRUE;
  char acPath[600];
  size_t uLen = 0u;
  char *pcText = NULL;

  snprintf(acPath, sizeof(acPath), "%s/devices.json", pcDataDir);
  pcText = af__read_all(acPath, &uLen);
  if (NULL != pcText)
  {
    yyjson_doc *pstDoc = yyjson_read(pcText, uLen, 0u);
    dm_free_tag(pcText);
    if (NULL == pstDoc)
    {
      DM_LOG_WARN("corrupt devices.json during migration, skipping");
      bCanDelKeychain = DM_FALSE;
    }
    else
    {
      yyjson_val *pstRoot = yyjson_doc_get_root(pstDoc);
      bool_t bWasArr = yyjson_is_arr(pstRoot) ? DM_TRUE : DM_FALSE;
      yyjson_val *pstArr = bWasArr ? pstRoot : yyjson_obj_get(pstRoot, "devices");
      if ((NULL != pstArr) && yyjson_is_arr(pstArr))
      {
        if (DM_SUCCESS != af__rewrite_devices_arr(acPath, pstArr, bWasArr, bHasLegacy))
        {
          bCanDelKeychain = DM_FALSE;
        }
      }
      yyjson_doc_free(pstDoc);
    }
  }
  if (bHasLegacy && bCanDelKeychain)
  {
    af__del_legacy_keychain();
  }
}

int af_migrate_run(in const char *pcDataDir)
{
  bool_t bAutoConn = DM_TRUE;
  bool_t bMigCfg = DM_FALSE;

  if ((NULL == pcDataDir) || ('\0' == pcDataDir[0]))
  {
    return DM_INVALID;
  }
  bMigCfg = af__migrate_config_toml(pcDataDir, &bAutoConn);
  af__migrate_state_json(pcDataDir, bMigCfg, bAutoConn);
  af__migrate_devices_and_keychain(pcDataDir);
  return DM_SUCCESS;
}
