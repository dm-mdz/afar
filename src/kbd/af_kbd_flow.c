/**
 ******************************************************************************
 * @file    af_kbd_flow.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   纯 C 蓝牙键盘交接决策状态机（1:1 对应 DD-04 §3.1 与 kbflow.rs，零系统依赖）。
 ******************************************************************************
 */

#include "af_kbd.h"

#include <stdio.h>
#include <string.h>

static bool_t af__kbd_name_in_list(in const char *pcTarget, in const char *const *ppcList,
                                   in uint32 u32Cnt)
{
  uint32 i = 0u;

  if ((NULL == pcTarget) || ('\0' == pcTarget[0]) || (NULL == ppcList))
  {
    return DM_FALSE;
  }
  for (i = 0u; i < u32Cnt; i++)
  {
    if ((NULL != ppcList[i]) && (0 == strcmp(pcTarget, ppcList[i])))
    {
      return DM_TRUE;
    }
  }
  return DM_FALSE;
}

static void af__kbd_clear_arm(io af_kbd_flow_st *pstFlow)
{
  pstFlow->bHasTakeFor = DM_FALSE;
  pstFlow->acTakeFor[0] = '\0';
  pstFlow->u64TakeUntilMs = 0u;
}

void af_kbd_flow_init(out af_kbd_flow_st *pstFlow, in bool_t bShowing, in bool_t bTaken)
{
  if (NULL == pstFlow)
  {
    return;
  }
  memset(pstFlow, 0, sizeof(*pstFlow));
  pstFlow->bWasShowing = bShowing ? DM_TRUE : DM_FALSE;
  pstFlow->bTaken = bTaken ? DM_TRUE : DM_FALSE;
}

void af_kbd_flow_on_connect(io af_kbd_flow_st *pstFlow, in const char *pcTarget,
                            in bool_t bKeyboard, in uint64 u64NowMs)
{
  if (NULL == pstFlow)
  {
    return;
  }
  if (bKeyboard && (NULL != pcTarget) && ('\0' != pcTarget[0]))
  {
    pstFlow->bHasTakeFor = DM_TRUE;
    snprintf(pstFlow->acTakeFor, sizeof(pstFlow->acTakeFor), "%s", pcTarget);
    pstFlow->u64TakeUntilMs = u64NowMs + (uint64)AF_KBD_ARM_MS;
  }
  else
  {
    af__kbd_clear_arm(pstFlow);
  }
}

void af_kbd_flow_hold(io af_kbd_flow_st *pstFlow, in uint64 u64UntilMs)
{
  if (NULL == pstFlow)
  {
    return;
  }
  pstFlow->u64HoldUntilMs = u64UntilMs;
}

void af_kbd_flow_on_disconnect(io af_kbd_flow_st *pstFlow, in bool_t bShowingNow,
                               in uint64 u64NowMs)
{
  if (NULL == pstFlow)
  {
    return;
  }
  af__kbd_clear_arm(pstFlow);
  pstFlow->u64HoldUntilMs = 0u;
  pstFlow->bHeldDrop = DM_FALSE;
  pstFlow->u64GapUntilMs = bShowingNow ? (u64NowMs + (uint64)AF_KBD_GAP_MS) : 0u;
}

static af_kbd_act_et af__kbd_check_take(io af_kbd_flow_st *pstFlow,
                                        in const char *const *ppcConnected, in uint32 u32ConnCnt,
                                        in bool_t bKbOnMac, in uint64 u64NowMs,
                                        in bool_t bWasShowing, in bool_t bShowing)
{
  if (pstFlow->bHasTakeFor)
  {
    if (u64NowMs > pstFlow->u64TakeUntilMs)
    {
      af__kbd_clear_arm(pstFlow);
      return AF_KBD_ACT_NONE;
    }
    if ((u64NowMs >= pstFlow->u64GapUntilMs) &&
        af__kbd_name_in_list(pstFlow->acTakeFor, ppcConnected, u32ConnCnt))
    {
      af__kbd_clear_arm(pstFlow);
      if (!bKbOnMac)
      {
        pstFlow->bTaken = DM_TRUE;
        return AF_KBD_ACT_TAKE;
      }
      return AF_KBD_ACT_NONE;
    }
  }
  if ((!bWasShowing) && bShowing && (!pstFlow->bHasTakeFor))
  {
    return AF_KBD_ACT_CLEAR_RELEASED;
  }
  return AF_KBD_ACT_NONE;
}

af_kbd_act_et af_kbd_flow_tick(io af_kbd_flow_st *pstFlow, in const char *const *ppcConnected,
                               in uint32 u32ConnCnt, in bool_t bKbOnMac, in uint64 u64NowMs)
{
  bool_t bShowing = DM_FALSE;
  bool_t bWas = DM_FALSE;
  bool_t bHolding = DM_FALSE;

  if (NULL == pstFlow)
  {
    return AF_KBD_ACT_NONE;
  }
  bShowing = (u32ConnCnt > 0u) ? DM_TRUE : DM_FALSE;
  bWas = pstFlow->bWasShowing;
  pstFlow->bWasShowing = bShowing;
  bHolding = ((pstFlow->u64HoldUntilMs > 0u) && (u64NowMs < pstFlow->u64HoldUntilMs)) ? DM_TRUE
                                                                                       : DM_FALSE;

  if (!bShowing)
  {
    pstFlow->u64GapUntilMs = 0u;
  }

  if (bWas && (!bShowing))
  {
    if (bHolding)
    {
      pstFlow->bHeldDrop = DM_TRUE;
      if (pstFlow->bHasTakeFor && (u64NowMs > pstFlow->u64TakeUntilMs))
      {
        af__kbd_clear_arm(pstFlow);
      }
      return AF_KBD_ACT_NONE;
    }
    pstFlow->bHeldDrop = DM_FALSE;
    pstFlow->u64HoldUntilMs = 0u;
    if (pstFlow->bTaken)
    {
      pstFlow->bTaken = DM_FALSE;
      return AF_KBD_ACT_RELEASE;
    }
    return AF_KBD_ACT_NONE;
  }

  if (pstFlow->bHeldDrop)
  {
    if (bShowing)
    {
      pstFlow->bHeldDrop = DM_FALSE;
      pstFlow->u64HoldUntilMs = 0u;
    }
    else if (!bHolding)
    {
      pstFlow->bHeldDrop = DM_FALSE;
      pstFlow->u64HoldUntilMs = 0u;
      if (pstFlow->bTaken)
      {
        pstFlow->bTaken = DM_FALSE;
        return AF_KBD_ACT_RELEASE;
      }
      return AF_KBD_ACT_NONE;
    }
    else
    {
      return AF_KBD_ACT_NONE;
    }
  }
  else if (!bHolding)
  {
    pstFlow->u64HoldUntilMs = 0u;
  }

  return af__kbd_check_take(pstFlow, ppcConnected, u32ConnCnt, bKbOnMac, u64NowMs, bWas, bShowing);
}

af_kbd_act_et af_kbd_flow_take(io af_kbd_flow_st *pstFlow, in bool_t bKbOnMac)
{
  if (NULL == pstFlow)
  {
    return AF_KBD_ACT_NONE;
  }
  af__kbd_clear_arm(pstFlow);
  if (bKbOnMac)
  {
    return AF_KBD_ACT_NONE;
  }
  pstFlow->bTaken = DM_TRUE;
  return AF_KBD_ACT_TAKE;
}

af_kbd_act_et af_kbd_flow_release(io af_kbd_flow_st *pstFlow)
{
  if (NULL == pstFlow)
  {
    return AF_KBD_ACT_NONE;
  }
  af__kbd_clear_arm(pstFlow);
  if (!pstFlow->bTaken)
  {
    return AF_KBD_ACT_NONE;
  }
  pstFlow->bTaken = DM_FALSE;
  return AF_KBD_ACT_RELEASE;
}

af_kbd_act_et af_kbd_flow_release_now(io af_kbd_flow_st *pstFlow)
{
  if (NULL == pstFlow)
  {
    return AF_KBD_ACT_NONE;
  }
  af__kbd_clear_arm(pstFlow);
  pstFlow->bTaken = DM_FALSE;
  return AF_KBD_ACT_RELEASE;
}
