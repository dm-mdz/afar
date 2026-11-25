/**
 ******************************************************************************
 * @file    af_store.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   基础层唯一 config.json / state.json 内存与原子落盘实现（DD-01 §2.2）。
 ******************************************************************************
 */

#define _DARWIN_C_SOURCE
#define DM_LOG_MOD "store"

#include "af_store.h"
#include <dm.h>
#include <dm/json.h>
#include <dm/log.h>
#include <dm/pool.h>

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_acDataDir[512];
static af_config_st g_stCfg;
static af_state_st g_stState;
static _Atomic uint32 g_u32TmpSeq = 0u;

static void af__store_defaults(void)
{
  memset(&g_stCfg, 0, sizeof(g_stCfg));
  snprintf(g_stCfg.acScale, sizeof(g_stCfg.acScale), "auto");
  g_stCfg.bAutoConnect = DM_TRUE;
  g_stCfg.bMirror = DM_TRUE;
  g_stCfg.au32IpadPixels[0] = 2360u;
  g_stCfg.au32IpadPixels[1] = 1640u;
  g_stCfg.u16Port = 47911u;

  memset(&g_stState, 0, sizeof(g_stState));
  snprintf(g_stState.acScale, sizeof(g_stState.acScale), "default");
  snprintf(g_stState.acPlan, sizeof(g_stState.acPlan), "auto");
  g_stState.bKeyboardAuto = DM_TRUE;
}

static void af__copy_str(dm_out char *pcDst, dm_in size_t uCap, dm_in yyjson_val *pstV)
{
  const char *pcS = yyjson_get_str(pstV);
  if ((NULL != pcS) && (uCap > 0u))
  {
    snprintf(pcDst, uCap, "%s", pcS);
  }
}

static char *af__read_file(dm_in const char *pcPath, dm_out size_t *puLen)
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

int af_store_write_atomic(dm_in const char *pcPath, dm_in const void *pvData, dm_in size_t uLen)
{
  char acTmp[640];
  int iFd = -1;
  size_t uOff = 0u;
  const uint8 *pu8 = (const uint8 *)pvData;
  int iN = 0;

  if ((NULL == pcPath) || ((uLen > 0u) && (NULL == pvData)))
  {
    return DM_INVALID;
  }
  uint32 u32Seq = atomic_fetch_add(&g_u32TmpSeq, 1u) + 1u;
  iN = snprintf(acTmp, sizeof(acTmp), "%s.tmp.%d.%u", pcPath, (int)getpid(), u32Seq);
  if ((iN <= 0) || ((size_t)iN >= sizeof(acTmp)))
  {
    return DM_TOO_BIG;
  }
  iFd = open(acTmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (iFd < 0)
  {
    return DM_IO;
  }
  while (uOff < uLen)
  {
    ssize_t iW = write(iFd, pu8 + uOff, uLen - uOff);
    if (iW < 0)
    {
      if (EINTR == errno) { continue; }
      close(iFd);
      (void)unlink(acTmp);
      return DM_IO;
    }
    uOff += (size_t)iW;
  }
  if ((0 != fsync(iFd)) || (0 != fchmod(iFd, 0600)))
  {
    close(iFd);
    (void)unlink(acTmp);
    return DM_IO;
  }
  if (0 != close(iFd))
  {
    (void)unlink(acTmp);
    return DM_IO;
  }
  if (0 != rename(acTmp, pcPath))
  {
    (void)unlink(acTmp);
    return DM_IO;
  }
  return DM_SUCCESS;
}

static void af__parse_config_json(dm_in const char *pcPath)
{
  size_t uLen = 0u;
  char *pcText = af__read_file(pcPath, &uLen);
  yyjson_doc *pstDoc = NULL;
  yyjson_val *pstRoot = NULL;
  yyjson_val *pstV = NULL;

  if (NULL == pcText)
  {
    return;
  }
  pstDoc = dm_json_read(pcText, uLen, 0u, NULL);
  dm_free_tag(pcText);
  if ((NULL == pstDoc) || (!yyjson_is_obj(yyjson_doc_get_root(pstDoc))))
  {
    DM_LOG_WARN("corrupt config.json at %s, falling back to defaults", pcPath);
    if (NULL != pstDoc) { yyjson_doc_free(pstDoc); }
    return;
  }
  pstRoot = yyjson_doc_get_root(pstDoc);
  af__copy_str(g_stCfg.acIpadName, sizeof(g_stCfg.acIpadName), yyjson_obj_get(pstRoot, "ipad_name"));
  af__copy_str(g_stCfg.acScale, sizeof(g_stCfg.acScale), yyjson_obj_get(pstRoot, "scale"));
  if (yyjson_is_bool((pstV = yyjson_obj_get(pstRoot, "auto_connect"))))
  {
    g_stCfg.bAutoConnect = yyjson_get_bool(pstV) ? DM_TRUE : DM_FALSE;
  }
  if (yyjson_is_bool((pstV = yyjson_obj_get(pstRoot, "mirror"))))
  {
    g_stCfg.bMirror = yyjson_get_bool(pstV) ? DM_TRUE : DM_FALSE;
  }
  if (yyjson_is_uint((pstV = yyjson_obj_get(pstRoot, "port"))))
  {
    uint64 u64P = yyjson_get_uint(pstV);
    if ((u64P >= 1024u) && (u64P <= 65535u))
    {
      g_stCfg.u16Port = (uint16)u64P;
    }
  }
  pstV = yyjson_obj_get(pstRoot, "ipad_pixels");
  if (yyjson_is_arr(pstV) && (2u == yyjson_arr_size(pstV)))
  {
    yyjson_val *pstW = yyjson_arr_get(pstV, 0u);
    yyjson_val *pstH = yyjson_arr_get(pstV, 1u);
    if (yyjson_is_uint(pstW) && yyjson_is_uint(pstH) && (yyjson_get_uint(pstW) > 0u)
        && (yyjson_get_uint(pstH) > 0u))
    {
      g_stCfg.au32IpadPixels[0] = (uint32)yyjson_get_uint(pstW);
      g_stCfg.au32IpadPixels[1] = (uint32)yyjson_get_uint(pstH);
    }
  }
  pstV = yyjson_obj_get(pstRoot, "extra_sizes");
  if (yyjson_is_arr(pstV))
  {
    size_t uIdx = 0u;
    size_t uMax = 0u;
    yyjson_val *pstItem = NULL;
    yyjson_arr_foreach(pstV, uIdx, uMax, pstItem)
    {
      if ((g_stCfg.u32ExtraCnt < 16u) && yyjson_is_arr(pstItem) && (2u == yyjson_arr_size(pstItem)))
      {
        yyjson_val *pstEw = yyjson_arr_get(pstItem, 0u);
        yyjson_val *pstEh = yyjson_arr_get(pstItem, 1u);
        if (yyjson_is_uint(pstEw) && yyjson_is_uint(pstEh) && (yyjson_get_uint(pstEw) > 0u)
            && (yyjson_get_uint(pstEh) > 0u))
        {
          g_stCfg.aau32Extra[g_stCfg.u32ExtraCnt][0] = (uint32)yyjson_get_uint(pstEw);
          g_stCfg.aau32Extra[g_stCfg.u32ExtraCnt][1] = (uint32)yyjson_get_uint(pstEh);
          g_stCfg.u32ExtraCnt++;
        }
      }
    }
  }
  yyjson_doc_free(pstDoc);
}

static void af__parse_one_ipad_sizes(dm_in yyjson_val *pstKey, dm_in yyjson_val *pstVal)
{
  af_ipad_sizes_entry_st *pstE = NULL;
  uint32 k = 0u;

  if ((g_stState.u32SizesCnt >= AF_IPAD_MAP_MAX) || (!yyjson_is_arr(pstVal))
      || (3u != yyjson_arr_size(pstVal)))
  {
    return;
  }
  pstE = &g_stState.astSizes[g_stState.u32SizesCnt++];
  af__copy_str(pstE->acName, sizeof(pstE->acName), pstKey);
  for (k = 0u; k < 3u; k++)
  {
    yyjson_val *pstPair = yyjson_arr_get(pstVal, k);
    if (yyjson_is_arr(pstPair) && (2u == yyjson_arr_size(pstPair)))
    {
      pstE->aau32Sz[k][0] = (uint32)yyjson_get_uint(yyjson_arr_get(pstPair, 0u));
      pstE->aau32Sz[k][1] = (uint32)yyjson_get_uint(yyjson_arr_get(pstPair, 1u));
    }
  }
}

static void af__parse_state_maps(dm_in yyjson_val *pstRoot)
{
  yyjson_val *pstObj = NULL;
  yyjson_val *pstKey = NULL;
  yyjson_val *pstVal = NULL;
  size_t uIdx = 0u;
  size_t uMax = 0u;

  pstObj = yyjson_obj_get(pstRoot, "ipad_screens");
  if (yyjson_is_obj(pstObj))
  {
    yyjson_obj_foreach(pstObj, uIdx, uMax, pstKey, pstVal)
    {
      if ((g_stState.u32ScreenCnt < AF_IPAD_MAP_MAX) && yyjson_is_arr(pstVal)
          && (2u == yyjson_arr_size(pstVal)))
      {
        af_ipad_screen_entry_st *pstE = &g_stState.astScreens[g_stState.u32ScreenCnt++];
        af__copy_str(pstE->acName, sizeof(pstE->acName), pstKey);
        pstE->au32Px[0] = (uint32)yyjson_get_uint(yyjson_arr_get(pstVal, 0u));
        pstE->au32Px[1] = (uint32)yyjson_get_uint(yyjson_arr_get(pstVal, 1u));
      }
    }
  }
  pstObj = yyjson_obj_get(pstRoot, "ipad_sizes");
  if (yyjson_is_obj(pstObj))
  {
    yyjson_obj_foreach(pstObj, uIdx, uMax, pstKey, pstVal)
    {
      af__parse_one_ipad_sizes(pstKey, pstVal);
    }
  }
  pstObj = yyjson_obj_get(pstRoot, "ipad_scales");
  if (yyjson_is_obj(pstObj))
  {
    yyjson_obj_foreach(pstObj, uIdx, uMax, pstKey, pstVal)
    {
      if ((g_stState.u32ScaleCnt < AF_IPAD_MAP_MAX) && yyjson_is_str(pstVal))
      {
        af_ipad_scale_entry_st *pstE = &g_stState.astScales[g_stState.u32ScaleCnt++];
        af__copy_str(pstE->acName, sizeof(pstE->acName), pstKey);
        af__copy_str(pstE->acScale, sizeof(pstE->acScale), pstVal);
      }
    }
  }
  pstObj = yyjson_obj_get(pstRoot, "display_modes");
  if (yyjson_is_obj(pstObj))
  {
    yyjson_obj_foreach(pstObj, uIdx, uMax, pstKey, pstVal)
    {
      if ((g_stState.u32DispModeCnt < AF_DISP_MODES_SAVE) && yyjson_is_arr(pstVal)
          && (5u == yyjson_arr_size(pstVal)))
      {
        af_disp_mode_entry_st *pstE = &g_stState.astDispModes[g_stState.u32DispModeCnt++];
        uint32 k = 0u;
        af__copy_str(pstE->acKey, sizeof(pstE->acKey), pstKey);
        for (k = 0u; k < 5u; k++)
        {
          pstE->au32Mode[k] = (uint32)yyjson_get_uint(yyjson_arr_get(pstVal, k));
        }
      }
    }
  }
}

static void af__parse_state_json(dm_in const char *pcPath)
{
  size_t uLen = 0u;
  char *pcText = af__read_file(pcPath, &uLen);
  yyjson_doc *pstDoc = NULL;
  yyjson_val *pstRoot = NULL;
  yyjson_val *pstV = NULL;

  if (NULL == pcText)
  {
    return;
  }
  pstDoc = dm_json_read(pcText, uLen, 0u, NULL);
  dm_free_tag(pcText);
  if ((NULL == pstDoc) || (!yyjson_is_obj(yyjson_doc_get_root(pstDoc))))
  {
    DM_LOG_WARN("corrupt state.json at %s, falling back to defaults", pcPath);
    if (NULL != pstDoc) { yyjson_doc_free(pstDoc); }
    return;
  }
  pstRoot = yyjson_doc_get_root(pstDoc);
  af__copy_str(g_stState.acName, sizeof(g_stState.acName), yyjson_obj_get(pstRoot, "name"));
  af__copy_str(g_stState.acScale, sizeof(g_stState.acScale), yyjson_obj_get(pstRoot, "scale"));
  af__copy_str(g_stState.acLastIpad, sizeof(g_stState.acLastIpad), yyjson_obj_get(pstRoot, "last_ipad"));
  af__copy_str(g_stState.acPlan, sizeof(g_stState.acPlan), yyjson_obj_get(pstRoot, "plan"));

  if (yyjson_is_obj((pstV = yyjson_obj_get(pstRoot, "keyboard"))))
  {
    g_stState.bHasKeyboard = DM_TRUE;
    af__copy_str(g_stState.stKeyboard.acAddr, sizeof(g_stState.stKeyboard.acAddr),
                 yyjson_obj_get(pstV, "address"));
    af__copy_str(g_stState.stKeyboard.acName, sizeof(g_stState.stKeyboard.acName),
                 yyjson_obj_get(pstV, "name"));
  }
  if (yyjson_is_bool((pstV = yyjson_obj_get(pstRoot, "keyboard_auto"))))
  {
    g_stState.bKeyboardAuto = yyjson_get_bool(pstV) ? DM_TRUE : DM_FALSE;
  }
  if (yyjson_is_bool((pstV = yyjson_obj_get(pstRoot, "keyboard_taken"))))
  {
    g_stState.bKeyboardTaken = yyjson_get_bool(pstV) ? DM_TRUE : DM_FALSE;
  }
  if (yyjson_is_bool((pstV = yyjson_obj_get(pstRoot, "user_off"))))
  {
    g_stState.bUserOff = yyjson_get_bool(pstV) ? DM_TRUE : DM_FALSE;
  }
  if (yyjson_is_bool((pstV = yyjson_obj_get(pstRoot, "no_boot_connect"))))
  {
    g_stState.bNoBootConnect = yyjson_get_bool(pstV) ? DM_TRUE : DM_FALSE;
  }
  if (yyjson_is_bool((pstV = yyjson_obj_get(pstRoot, "setup_done"))))
  {
    g_stState.bSetupDone = yyjson_get_bool(pstV) ? DM_TRUE : DM_FALSE;
  }
  if (yyjson_is_bool((pstV = yyjson_obj_get(pstRoot, "anti_unplug"))))
  {
    g_stState.bAntiUnplug = yyjson_get_bool(pstV) ? DM_TRUE : DM_FALSE;
  }
  if (yyjson_is_arr((pstV = yyjson_obj_get(pstRoot, "desk_mode"))) && (4u == yyjson_arr_size(pstV)))
  {
    uint32 k = 0u;
    g_stState.bHasDeskMode = DM_TRUE;
    for (k = 0u; k < 4u; k++)
    {
      g_stState.au32DeskMode[k] = (uint32)yyjson_get_uint(yyjson_arr_get(pstV, k));
    }
  }
  if (yyjson_is_obj((pstV = yyjson_obj_get(pstRoot, "orig_screensaver"))))
  {
    yyjson_val *pstS = yyjson_obj_get(pstV, "saved");
    yyjson_val *pstK = yyjson_obj_get(pstV, "known");
    yyjson_val *pstSec = yyjson_obj_get(pstV, "sec");
    g_stState.stOrigSs.bSaved = yyjson_is_bool(pstS) && yyjson_get_bool(pstS) ? DM_TRUE : DM_FALSE;
    g_stState.stOrigSs.bKnown = yyjson_is_bool(pstK) && yyjson_get_bool(pstK) ? DM_TRUE : DM_FALSE;
    g_stState.stOrigSs.iSec = yyjson_is_int(pstSec) ? (int)yyjson_get_int(pstSec) : 0;
  }
  af__parse_state_maps(pstRoot);
  yyjson_doc_free(pstDoc);
}

static void af__serialize_state_maps(dm_io yyjson_mut_doc *pstDoc, dm_io yyjson_mut_val *pstRoot,
                                     dm_in const af_state_st *pstSt)
{
  yyjson_mut_val *pstObj = NULL;
  uint32 u32N = 0u;
  uint32 i = 0u;

  pstObj = yyjson_mut_obj(pstDoc);
  u32N = (pstSt->u32ScreenCnt < AF_IPAD_MAP_MAX) ? pstSt->u32ScreenCnt : AF_IPAD_MAP_MAX;
  for (i = 0u; i < u32N; i++)
  {
    yyjson_mut_val *pstArr = yyjson_mut_arr(pstDoc);
    yyjson_mut_arr_add_uint(pstDoc, pstArr, pstSt->astScreens[i].au32Px[0]);
    yyjson_mut_arr_add_uint(pstDoc, pstArr, pstSt->astScreens[i].au32Px[1]);
    yyjson_mut_obj_add_val(pstDoc, pstObj, pstSt->astScreens[i].acName, pstArr);
  }
  yyjson_mut_obj_add_val(pstDoc, pstRoot, "ipad_screens", pstObj);

  pstObj = yyjson_mut_obj(pstDoc);
  u32N = (pstSt->u32SizesCnt < AF_IPAD_MAP_MAX) ? pstSt->u32SizesCnt : AF_IPAD_MAP_MAX;
  for (i = 0u; i < u32N; i++)
  {
    yyjson_mut_val *pstTri = yyjson_mut_arr(pstDoc);
    uint32 k = 0u;
    for (k = 0u; k < 3u; k++)
    {
      yyjson_mut_val *pstP = yyjson_mut_arr(pstDoc);
      yyjson_mut_arr_add_uint(pstDoc, pstP, pstSt->astSizes[i].aau32Sz[k][0]);
      yyjson_mut_arr_add_uint(pstDoc, pstP, pstSt->astSizes[i].aau32Sz[k][1]);
      yyjson_mut_arr_add_val(pstTri, pstP);
    }
    yyjson_mut_obj_add_val(pstDoc, pstObj, pstSt->astSizes[i].acName, pstTri);
  }
  yyjson_mut_obj_add_val(pstDoc, pstRoot, "ipad_sizes", pstObj);

  pstObj = yyjson_mut_obj(pstDoc);
  u32N = (pstSt->u32ScaleCnt < AF_IPAD_MAP_MAX) ? pstSt->u32ScaleCnt : AF_IPAD_MAP_MAX;
  for (i = 0u; i < u32N; i++)
  {
    yyjson_mut_obj_add_str(pstDoc, pstObj, pstSt->astScales[i].acName, pstSt->astScales[i].acScale);
  }
  yyjson_mut_obj_add_val(pstDoc, pstRoot, "ipad_scales", pstObj);

  pstObj = yyjson_mut_obj(pstDoc);
  u32N = (pstSt->u32DispModeCnt < AF_DISP_MODES_SAVE) ? pstSt->u32DispModeCnt : AF_DISP_MODES_SAVE;
  for (i = 0u; i < u32N; i++)
  {
    yyjson_mut_val *pstArr = yyjson_mut_arr(pstDoc);
    uint32 k = 0u;
    for (k = 0u; k < 5u; k++)
    {
      yyjson_mut_arr_add_uint(pstDoc, pstArr, pstSt->astDispModes[i].au32Mode[k]);
    }
    yyjson_mut_obj_add_val(pstDoc, pstObj, pstSt->astDispModes[i].acKey, pstArr);
  }
  yyjson_mut_obj_add_val(pstDoc, pstRoot, "display_modes", pstObj);
}

static char *af__serialize_state(dm_in const af_state_st *pstSt, dm_out size_t *puLen)
{
  yyjson_mut_doc *pstDoc = yyjson_mut_doc_new(dm_json_alc());
  yyjson_mut_val *pstRoot = NULL;
  char *pcJson = NULL;

  *puLen = 0u;
  if (NULL == pstDoc) { return NULL; }
  pstRoot = yyjson_mut_obj(pstDoc);
  yyjson_mut_doc_set_root(pstDoc, pstRoot);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "name", pstSt->acName);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "scale", pstSt->acScale);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "last_ipad", pstSt->acLastIpad);
  if (pstSt->bHasKeyboard)
  {
    yyjson_mut_val *pstK = yyjson_mut_obj(pstDoc);
    yyjson_mut_obj_add_str(pstDoc, pstK, "address", pstSt->stKeyboard.acAddr);
    yyjson_mut_obj_add_str(pstDoc, pstK, "name", pstSt->stKeyboard.acName);
    yyjson_mut_obj_add_val(pstDoc, pstRoot, "keyboard", pstK);
  }
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "keyboard_auto", pstSt->bKeyboardAuto ? true : false);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "keyboard_taken", pstSt->bKeyboardTaken ? true : false);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "user_off", pstSt->bUserOff ? true : false);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "no_boot_connect", pstSt->bNoBootConnect ? true : false);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "setup_done", pstSt->bSetupDone ? true : false);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "plan", pstSt->acPlan);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "anti_unplug", pstSt->bAntiUnplug ? true : false);
  if (pstSt->bHasDeskMode)
  {
    yyjson_mut_val *pstD = yyjson_mut_arr(pstDoc);
    uint32 k = 0u;
    for (k = 0u; k < 4u; k++)
    {
      yyjson_mut_arr_add_uint(pstDoc, pstD, pstSt->au32DeskMode[k]);
    }
    yyjson_mut_obj_add_val(pstDoc, pstRoot, "desk_mode", pstD);
  }
  if (pstSt->stOrigSs.bSaved)
  {
    yyjson_mut_val *pstSs = yyjson_mut_obj(pstDoc);
    yyjson_mut_obj_add_bool(pstDoc, pstSs, "saved", true);
    yyjson_mut_obj_add_bool(pstDoc, pstSs, "known", pstSt->stOrigSs.bKnown ? true : false);
    yyjson_mut_obj_add_int(pstDoc, pstSs, "sec", pstSt->stOrigSs.iSec);
    yyjson_mut_obj_add_val(pstDoc, pstRoot, "orig_screensaver", pstSs);
  }
  af__serialize_state_maps(pstDoc, pstRoot, pstSt);
  pcJson = yyjson_mut_write_opts(pstDoc, YYJSON_WRITE_PRETTY, dm_json_alc(), puLen, NULL);
  yyjson_mut_doc_free(pstDoc);
  return pcJson;
}

int af_store_load(dm_in const char *pcDataDir)
{
  char acCfg[600];
  char acState[600];

  if ((NULL == pcDataDir) || ('\0' == pcDataDir[0]))
  {
    return DM_INVALID;
  }
  af__store_defaults();
  snprintf(g_acDataDir, sizeof(g_acDataDir), "%s", pcDataDir);
  snprintf(acCfg, sizeof(acCfg), "%s/config.json", pcDataDir);
  snprintf(acState, sizeof(acState), "%s/state.json", pcDataDir);
  af__parse_config_json(acCfg);
  af__parse_state_json(acState);
  return DM_SUCCESS;
}

int af_store_save_sync(dm_in const char *pcDataDir)
{
  char acPath[600];
  const char *pcDir = ((NULL != pcDataDir) && ('\0' != pcDataDir[0])) ? pcDataDir : g_acDataDir;
  size_t uLen = 0u;
  char *pcJson = NULL;
  int iErr = DM_SUCCESS;

  if ((NULL == pcDir) || ('\0' == pcDir[0]))
  {
    return DM_INVALID;
  }
  pcJson = af__serialize_state(&g_stState, &uLen);
  if (NULL == pcJson)
  {
    return DM_ALLOC;
  }
  snprintf(acPath, sizeof(acPath), "%s/state.json", pcDir);
  iErr = af_store_write_atomic(acPath, pcJson, uLen);
  dm_json_free_str(pcJson);
  return iErr;
}

typedef struct af__save_job
{
  char acPath[600];
  char *pcJson;
  size_t uLen;
  int iErr;
  af_store_saved_cb pfOnDone;
  void *pvCtx;
} af__save_job_st;

static void af__save_work(void *pvArg)
{
  af__save_job_st *pstJob = (af__save_job_st *)pvArg;
  pstJob->iErr = af_store_write_atomic(pstJob->acPath, pstJob->pcJson, pstJob->uLen);
}

static void af__save_after(void *pvArg, int iStatus)
{
  af__save_job_st *pstJob = (af__save_job_st *)pvArg;
  int iErr = (0 != iStatus) ? iStatus : pstJob->iErr;
  af_store_saved_cb pfCb = pstJob->pfOnDone;
  void *pvCtx = pstJob->pvCtx;

  dm_json_free_str(pstJob->pcJson);
  dm_free_tag(pstJob);
  if (NULL != pfCb)
  {
    pfCb(iErr, pvCtx);
  }
}

int af_store_save_async(dm_in af_store_saved_cb pfOnDone, dm_io void *pvCtx)
{
  af__save_job_st *pstJob = NULL;
  int iErr = DM_SUCCESS;

  if ('\0' == g_acDataDir[0])
  {
    return DM_INVALID;
  }
  pstJob = (af__save_job_st *)dm_calloc_tag(1u, sizeof(*pstJob), DM_MEM_TAG_JSON);
  if (NULL == pstJob)
  {
    return DM_ALLOC;
  }
  snprintf(pstJob->acPath, sizeof(pstJob->acPath), "%s/state.json", g_acDataDir);
  pstJob->pcJson = af__serialize_state(&g_stState, &pstJob->uLen);
  if (NULL == pstJob->pcJson)
  {
    dm_free_tag(pstJob);
    return DM_ALLOC;
  }
  pstJob->pfOnDone = pfOnDone;
  pstJob->pvCtx = pvCtx;
  iErr = dm_pool_submit(DM_LANE_DB, af__save_work, af__save_after, pstJob);
  if (DM_SUCCESS != iErr)
  {
    dm_json_free_str(pstJob->pcJson);
    dm_free_tag(pstJob);
  }
  return iErr;
}

static char *af__serialize_cfg(dm_in const af_config_st *pstC, dm_out size_t *puLen)
{
  yyjson_mut_doc *pstDoc = yyjson_mut_doc_new(dm_json_alc());
  yyjson_mut_val *pstRoot = NULL;
  char *pcOut = NULL;
  uint32 i = 0u;

  if (NULL == pstDoc) { return NULL; }
  pstRoot = yyjson_mut_obj(pstDoc);
  yyjson_mut_doc_set_root(pstDoc, pstRoot);
  if ('\0' != pstC->acIpadName[0])
  {
    yyjson_mut_obj_add_str(pstDoc, pstRoot, "ipad_name", pstC->acIpadName);
  }
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "auto_connect", pstC->bAutoConnect ? true : false);
  yyjson_mut_obj_add_str(pstDoc, pstRoot, "scale", pstC->acScale);
  yyjson_mut_obj_add_bool(pstDoc, pstRoot, "mirror", pstC->bMirror ? true : false);
  yyjson_mut_obj_add_uint(pstDoc, pstRoot, "port", pstC->u16Port);
  if ((pstC->au32IpadPixels[0] > 0u) && (pstC->au32IpadPixels[1] > 0u))
  {
    yyjson_mut_val *pstPx = yyjson_mut_arr(pstDoc);
    yyjson_mut_arr_add_uint(pstDoc, pstPx, pstC->au32IpadPixels[0]);
    yyjson_mut_arr_add_uint(pstDoc, pstPx, pstC->au32IpadPixels[1]);
    yyjson_mut_obj_add_val(pstDoc, pstRoot, "ipad_pixels", pstPx);
  }
  if (pstC->u32ExtraCnt > 0u)
  {
    yyjson_mut_val *pstEx = yyjson_mut_arr(pstDoc);
    for (i = 0u; (i < pstC->u32ExtraCnt) && (i < 16u); i++)
    {
      yyjson_mut_val *pstPair = yyjson_mut_arr(pstDoc);
      yyjson_mut_arr_add_uint(pstDoc, pstPair, pstC->aau32Extra[i][0]);
      yyjson_mut_arr_add_uint(pstDoc, pstPair, pstC->aau32Extra[i][1]);
      yyjson_mut_arr_add_val(pstEx, pstPair);
    }
    yyjson_mut_obj_add_val(pstDoc, pstRoot, "extra_sizes", pstEx);
  }
  pcOut = yyjson_mut_write_opts(pstDoc, YYJSON_WRITE_PRETTY, dm_json_alc(), puLen, NULL);
  yyjson_mut_doc_free(pstDoc);
  return pcOut;
}

int af_store_save_cfg_async(dm_in af_store_saved_cb pfOnDone, dm_io void *pvCtx)
{
  af__save_job_st *pstJob = NULL;
  int iErr = DM_SUCCESS;

  if ('\0' == g_acDataDir[0])
  {
    return DM_INVALID;
  }
  pstJob = (af__save_job_st *)dm_calloc_tag(1u, sizeof(*pstJob), DM_MEM_TAG_JSON);
  if (NULL == pstJob)
  {
    return DM_ALLOC;
  }
  snprintf(pstJob->acPath, sizeof(pstJob->acPath), "%s/config.json", g_acDataDir);
  pstJob->pcJson = af__serialize_cfg(&g_stCfg, &pstJob->uLen);
  if (NULL == pstJob->pcJson)
  {
    dm_free_tag(pstJob);
    return DM_ALLOC;
  }
  pstJob->pfOnDone = pfOnDone;
  pstJob->pvCtx = pvCtx;
  iErr = dm_pool_submit(DM_LANE_DB, af__save_work, af__save_after, pstJob);
  if (DM_SUCCESS != iErr)
  {
    dm_json_free_str(pstJob->pcJson);
    dm_free_tag(pstJob);
  }
  return iErr;
}

const af_config_st *af_store_cfg(void) { return &g_stCfg; }
af_config_st *af_store_cfg_mut(void) { return &g_stCfg; }
af_state_st *af_store_state_mut(void) { return &g_stState; }
const af_state_st *af_store_state(void) { return &g_stState; }
