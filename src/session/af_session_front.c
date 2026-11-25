/**
 ******************************************************************************
 * @file    af_session_front.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   Front 20s 回到桌面状态机与 UTF-8 字符边界安全截断纯 C 实现（DD-05 §1）。
 ******************************************************************************
 */

#include "af_session.h"

#include <stdio.h>
#include <string.h>

void af_front_start(dm_out af_front_st *pstF, dm_in const char *pcName, dm_in uint64 u64NowMs)
{
  if (NULL == pstF)
  {
    return;
  }
  memset(pstF, 0, sizeof(*pstF));
  if ((NULL == pcName) || ('\0' == pcName[0]))
  {
    return;
  }
  pstF->bActive = DM_TRUE;
  snprintf(pstF->acName, sizeof(pstF->acName), "%s", pcName);
  pstF->u64ReconnectAtMs = u64NowMs + (uint64)AF_FRONT_GAP_MS;
  pstF->u64UntilMs = u64NowMs + (uint64)AF_FRONT_TIMEOUT_MS;
}

af_front_act_et af_front_step(dm_io af_front_st *pstF, dm_in uint64 u64NowMs,
                              dm_in bool_t bConnected, dm_in bool_t bAvailable)
{
  if ((NULL == pstF) || (!pstF->bActive))
  {
    return AF_FRONT_WAIT;
  }
  if (u64NowMs >= pstF->u64UntilMs)
  {
    pstF->bActive = DM_FALSE;
    return AF_FRONT_GIVEUP;
  }
  if (0u == pstF->u64SentAtMs)
  {
    if (u64NowMs < pstF->u64ReconnectAtMs)
    {
      return AF_FRONT_WAIT;
    }
    if (bConnected)
    {
      if (u64NowMs >= (pstF->u64ReconnectAtMs + (uint64)AF_FRONT_STUCK_MS))
      {
        pstF->bActive = DM_FALSE;
        return AF_FRONT_GIVEUP;
      }
      return AF_FRONT_WAIT;
    }
    if (!bAvailable)
    {
      return AF_FRONT_WAIT;
    }
    pstF->u64SentAtMs = (u64NowMs > 0u) ? u64NowMs : 1u;
    return AF_FRONT_CONNECT;
  }
  if (bConnected)
  {
    if (0u == pstF->u64SeenAtMs)
    {
      pstF->u64SeenAtMs = (u64NowMs > 0u) ? u64NowMs : 1u;
    }
    if (u64NowMs >= (pstF->u64SeenAtMs + (uint64)AF_FRONT_STABLE_MS))
    {
      pstF->bActive = DM_FALSE;
      return AF_FRONT_DONE;
    }
    return AF_FRONT_WAIT;
  }
  pstF->u64SeenAtMs = 0u;
  return AF_FRONT_WAIT;
}

static size_t af__utf8_seq_len(dm_in unsigned char ucLead)
{
  if (ucLead < 0x80u)
  {
    return 1u;
  }
  if ((ucLead >= 0xC2u) && (ucLead <= 0xDFu))
  {
    return 2u;
  }
  if ((ucLead >= 0xE0u) && (ucLead <= 0xEFu))
  {
    return 3u;
  }
  if ((ucLead >= 0xF0u) && (ucLead <= 0xF4u))
  {
    return 4u;
  }
  return 0u;
}

size_t af_session_trunc_utf8_bytes(dm_in const char *pcSrc, dm_out char *pcDst,
                                   dm_in size_t uDstCap, dm_in size_t uMaxBytes)
{
  size_t uSrcLen = 0u;
  size_t uLimit = 0u;
  size_t uPos = 0u;

  if ((NULL == pcDst) || (0u == uDstCap))
  {
    return 0u;
  }
  pcDst[0] = '\0';
  if (NULL == pcSrc)
  {
    return 0u;
  }
  uSrcLen = strlen(pcSrc);
  uLimit = (uMaxBytes < (uDstCap - 1u)) ? uMaxBytes : (uDstCap - 1u);

  while (uPos < uSrcLen)
  {
    size_t uStep = af__utf8_seq_len((unsigned char)pcSrc[uPos]);
    size_t k = 0u;
    bool_t bOk = DM_TRUE;

    if ((0u == uStep) || ((uPos + uStep) > uSrcLen) || ((uPos + uStep) > uLimit))
    {
      break;
    }
    for (k = 1u; k < uStep; k++)
    {
      if (0x80u != (((unsigned char)pcSrc[uPos + k]) & 0xC0u))
      {
        bOk = DM_FALSE;
        break;
      }
    }
    if (!bOk)
    {
      break;
    }
    uPos += uStep;
  }
  if (uPos > 0u)
  {
    memcpy(pcDst, pcSrc, uPos);
  }
  pcDst[uPos] = '\0';
  return uPos;
}
