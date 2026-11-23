/**
 ******************************************************************************
 * @file    af_display_algo.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   af_display 纯 C 算法：像素规整、3 档尺寸与 iPad mini 公式、vd_modes、4 类显示器识别。
 ******************************************************************************
 */

#include "af_display.h"
#include "../af_store.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define AF_DEFAULT_PX_W 2360u
#define AF_DEFAULT_PX_H 1640u

static uint32 af__round_even(in double dVal)
{
  double dHalf = round(dVal / 2.0);
  if (dHalf < 1.0)
  {
    return 2u;
  }
  return (uint32)(dHalf * 2.0);
}

bool_t af_scale_normalize_screen(in uint32 u32InW, in uint32 u32InH, out uint32 *pu32Long,
                                 out uint32 *pu32Short)
{
  uint32 u32A = (u32InW >= u32InH) ? u32InW : u32InH;
  uint32 u32B = (u32InW >= u32InH) ? u32InH : u32InW;

  if ((u32A < 1500u) || (u32A > 4000u) || (u32B < 1000u) || (u32B > 3000u))
  {
    return DM_FALSE;
  }
  if (((uint64)u32B * 10ULL < (uint64)u32A * 6ULL) || ((uint64)u32B * 10ULL > (uint64)u32A * 8ULL))
  {
    return DM_FALSE;
  }
  if (NULL != pu32Long)
  {
    *pu32Long = u32A;
  }
  if (NULL != pu32Short)
  {
    *pu32Short = u32B;
  }
  return DM_TRUE;
}

int af_scale_key_index(in const char *pcKey)
{
  if (NULL == pcKey)
  {
    return -1;
  }
  if (0 == strcmp(pcKey, "larger"))
  {
    return 0;
  }
  if (0 == strcmp(pcKey, "default"))
  {
    return 1;
  }
  if (0 == strcmp(pcKey, "more"))
  {
    return 2;
  }
  return -1;
}

void af_scale_presets(in uint32 u32PxW, in uint32 u32PxH,
                      out af_scale_preset_st astOut[AF_SCALE_COUNT])
{
  uint32 u32A = AF_DEFAULT_PX_W;
  uint32 u32B = AF_DEFAULT_PX_H;
  uint32 u32BaseW = 0u;
  uint32 u32BaseH = 0u;
  double dRatio = 0.0;
  uint32 u32Lw = 0u;
  uint32 u32Mw = 0u;

  if (NULL == astOut)
  {
    return;
  }
  if (!af_scale_normalize_screen(u32PxW, u32PxH, &u32A, &u32B))
  {
    u32A = AF_DEFAULT_PX_W;
    u32B = AF_DEFAULT_PX_H;
  }
  /* 默认档严格 px/2 不取偶（iPad mini 2266x1488 -> 1133x744） */
  u32BaseW = u32A / 2u;
  u32BaseH = u32B / 2u;
  dRatio = (double)u32B / (double)u32A;

  u32Lw = af__round_even((double)u32BaseW * 0.87);
  u32Mw = af__round_even((double)u32BaseW * 1.16);

  astOut[0].pcKey = "larger";
  astOut[0].pcLabel = "更大文字";
  astOut[0].u32W = u32Lw;
  astOut[0].u32H = af__round_even((double)u32Lw * dRatio);

  astOut[1].pcKey = "default";
  astOut[1].pcLabel = "默认";
  astOut[1].u32W = u32BaseW;
  astOut[1].u32H = u32BaseH;

  astOut[2].pcKey = "more";
  astOut[2].pcLabel = "更多空间";
  astOut[2].u32W = u32Mw;
  astOut[2].u32H = af__round_even((double)u32Mw * dRatio);
}

static void af__vd_push_unique(io af_vd_mode_req_st *pastOut, in uint32 u32Cap,
                               io uint32 *pu32Cnt, in uint32 u32W, in uint32 u32H)
{
  uint32 i = 0u;

  if ((0u == u32W) || (0u == u32H) || (*pu32Cnt >= u32Cap))
  {
    return;
  }
  for (i = 0u; i < *pu32Cnt; i++)
  {
    if ((pastOut[i].u32W == u32W) && (pastOut[i].u32H == u32H))
    {
      return;
    }
  }
  pastOut[*pu32Cnt].u32W = u32W;
  pastOut[*pu32Cnt].u32H = u32H;
  (*pu32Cnt)++;
}

static void af__vd_add_pair(io af_vd_mode_req_st *pastOut, in uint32 u32Cap, io uint32 *pu32Cnt,
                            in uint32 u32W, in uint32 u32H, in uint32 u32Style,
                            io uint32 *pu32MaxPw, io uint32 *pu32MaxPh)
{
  if ((0u == u32W) || (0u == u32H))
  {
    return;
  }
  if ((u32W * 2u) > *pu32MaxPw)
  {
    *pu32MaxPw = u32W * 2u;
  }
  if ((u32H * 2u) > *pu32MaxPh)
  {
    *pu32MaxPh = u32H * 2u;
  }
  if (0u == u32Style)
  {
    af__vd_push_unique(pastOut, u32Cap, pu32Cnt, u32W * 2u, u32H * 2u);
    af__vd_push_unique(pastOut, u32Cap, pu32Cnt, u32W, u32H);
  }
  else
  {
    af__vd_push_unique(pastOut, u32Cap, pu32Cnt, u32W, u32H);
  }
}

uint32 af_vd_build_modes(in const af_scale_preset_st astPresets[AF_SCALE_COUNT],
                         in const char *pcWantKey, in const uint32 aau32Extra[][2],
                         in uint32 u32ExtraCnt, in uint32 u32Style,
                         out af_vd_mode_req_st *pastOut, in uint32 u32Cap, out uint32 *pu32MaxPw,
                         out uint32 *pu32MaxPh)
{
  int iWant = af_scale_key_index(pcWantKey);
  uint32 u32WantIdx = (iWant >= 0) ? (uint32)iWant : 1u;
  uint32 u32Cnt = 0u;
  uint32 u32MaxPw = 0u;
  uint32 u32MaxPh = 0u;
  uint32 i = 0u;

  if ((NULL == astPresets) || (NULL == pastOut) || (0u == u32Cap))
  {
    return 0u;
  }
  /* 目标档位必须排首位 */
  af__vd_add_pair(pastOut, u32Cap, &u32Cnt, astPresets[u32WantIdx].u32W,
                  astPresets[u32WantIdx].u32H, u32Style, &u32MaxPw, &u32MaxPh);
  for (i = 0u; i < AF_SCALE_COUNT; i++)
  {
    if (i == u32WantIdx)
    {
      continue;
    }
    af__vd_add_pair(pastOut, u32Cap, &u32Cnt, astPresets[i].u32W, astPresets[i].u32H, u32Style,
                    &u32MaxPw, &u32MaxPh);
  }
  if (NULL != aau32Extra)
  {
    for (i = 0u; i < u32ExtraCnt; i++)
    {
      af__vd_add_pair(pastOut, u32Cap, &u32Cnt, aau32Extra[i][0], aau32Extra[i][1], u32Style,
                      &u32MaxPw, &u32MaxPh);
    }
  }
  if (NULL != pu32MaxPw)
  {
    *pu32MaxPw = u32MaxPw;
  }
  if (NULL != pu32MaxPh)
  {
    *pu32MaxPh = u32MaxPh;
  }
  return u32Cnt;
}

af_disp_kind_et af_disp_classify(in bool_t bIsOurs, in uint32 u32Vendor, in uint32 u32Model)
{
  if (bIsOurs)
  {
    return AF_DISP_OURS;
  }
  if ((0x6161706Cu == u32Vendor) && (0x69506164u == u32Model))
  {
    return AF_DISP_SIDECAR;
  }
  if (((0x756E6B6Eu == u32Vendor) && (0x76697274u == u32Model))
      || ((0x4D50u == u32Vendor) && (0x0001u == u32Model))
      || ((0x6161706Cu == u32Vendor) && (0x69506164u != u32Model)) || (0u == u32Vendor))
  {
    return AF_DISP_OTHER;
  }
  return AF_DISP_PHYSICAL;
}

const char *af_disp_store_target_scale(in const char *pcName)
{
  const af_state_st *pstS = af_store_state();
  uint32 i = 0u;

  if ((NULL != pcName) && ('\0' != pcName[0]))
  {
    for (i = 0u; i < pstS->u32ScaleCnt; i++)
    {
      if ((0 == strcmp(pstS->astScales[i].acName, pcName))
          && (af_scale_key_index(pstS->astScales[i].acScale) >= 0))
      {
        return pstS->astScales[i].acScale;
      }
    }
  }
  return (af_scale_key_index(pstS->acScale) >= 0) ? pstS->acScale : "default";
}

void af_disp_store_ipad_pixels(in const char *pcName, out uint32 *pu32W, out uint32 *pu32H)
{
  const af_config_st *pstCfg = af_store_cfg();
  const af_state_st *pstSt = af_store_state();
  uint32 i = 0u;

  *pu32W = pstCfg->au32IpadPixels[0];
  *pu32H = pstCfg->au32IpadPixels[1];
  if ((NULL == pcName) || ('\0' == pcName[0]))
  {
    return;
  }
  for (i = 0u; i < pstSt->u32ScreenCnt; i++)
  {
    if (0 == strcmp(pstSt->astScreens[i].acName, pcName))
    {
      *pu32W = pstSt->astScreens[i].au32Px[0];
      *pu32H = pstSt->astScreens[i].au32Px[1];
      return;
    }
  }
}

void af_disp_store_resolve_presets(in const char *pcName,
                                   out af_scale_preset_st astOut[AF_SCALE_COUNT])
{
  const af_state_st *pstS = af_store_state();
  uint32 u32PxW = 0u;
  uint32 u32PxH = 0u;
  uint32 i = 0u;

  af_disp_store_ipad_pixels(pcName, &u32PxW, &u32PxH);
  if ((NULL != pcName) && ('\0' != pcName[0]))
  {
    for (i = 0u; i < pstS->u32SizesCnt; i++)
    {
      if ((0 == strcmp(pstS->astSizes[i].acName, pcName))
          && (pstS->astSizes[i].aau32Sz[0][0] > 0u) && (pstS->astSizes[i].aau32Sz[1][0] > 0u)
          && (pstS->astSizes[i].aau32Sz[2][0] > 0u))
      {
        af_scale_presets(u32PxW, u32PxH, astOut);
        astOut[0].u32W = pstS->astSizes[i].aau32Sz[0][0];
        astOut[0].u32H = pstS->astSizes[i].aau32Sz[0][1];
        astOut[1].u32W = pstS->astSizes[i].aau32Sz[1][0];
        astOut[1].u32H = pstS->astSizes[i].aau32Sz[1][1];
        astOut[2].u32W = pstS->astSizes[i].aau32Sz[2][0];
        astOut[2].u32H = pstS->astSizes[i].aau32Sz[2][1];
        return;
      }
    }
  }
  af_scale_presets(u32PxW, u32PxH, astOut);
}

void af_disp_store_save_sizes(in const char *pcName,
                              in const af_scale_preset_st astP[AF_SCALE_COUNT])
{
  af_state_st *pstS = af_store_state_mut();
  af_ipad_sizes_entry_st *pstE = NULL;
  uint32 i = 0u;

  if ((NULL == pcName) || ('\0' == pcName[0]))
  {
    return;
  }
  for (i = 0u; i < pstS->u32SizesCnt; i++)
  {
    if (0 == strcmp(pstS->astSizes[i].acName, pcName))
    {
      pstE = &pstS->astSizes[i];
      break;
    }
  }
  if ((NULL == pstE) && (pstS->u32SizesCnt < AF_IPAD_MAP_MAX))
  {
    pstE = &pstS->astSizes[pstS->u32SizesCnt++];
    snprintf(pstE->acName, sizeof(pstE->acName), "%s", pcName);
  }
  if (NULL != pstE)
  {
    for (i = 0u; i < AF_SCALE_COUNT; i++)
    {
      pstE->aau32Sz[i][0] = astP[i].u32W;
      pstE->aau32Sz[i][1] = astP[i].u32H;
    }
    (void)af_store_save_async(NULL, NULL);
  }
}

void af_disp_store_save_scale(in const char *pcName, in const char *pcKey)
{
  af_state_st *pstS = af_store_state_mut();
  uint32 i = 0u;

  snprintf(pstS->acScale, sizeof(pstS->acScale), "%s", pcKey);
  if ((NULL != pcName) && ('\0' != pcName[0]))
  {
    af_ipad_scale_entry_st *pstE = NULL;
    for (i = 0u; i < pstS->u32ScaleCnt; i++)
    {
      if (0 == strcmp(pstS->astScales[i].acName, pcName))
      {
        pstE = &pstS->astScales[i];
        break;
      }
    }
    if ((NULL == pstE) && (pstS->u32ScaleCnt < AF_IPAD_MAP_MAX))
    {
      pstE = &pstS->astScales[pstS->u32ScaleCnt++];
      snprintf(pstE->acName, sizeof(pstE->acName), "%s", pcName);
    }
    if (NULL != pstE)
    {
      snprintf(pstE->acScale, sizeof(pstE->acScale), "%s", pcKey);
    }
  }
  (void)af_store_save_async(NULL, NULL);
}
