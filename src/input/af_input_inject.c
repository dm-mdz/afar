/**
 ******************************************************************************
 * @file    af_input_inject.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   T_inject 注入线程、1024 槽无锁 SPSC 环、保序合并屏障、坐标映射、修饰键与中文分片。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "input/af_input_int.h"

#include <limits.h>
#include <math.h>
#include <string.h>

/* ============================ 坐标与边界辅助 ============================ */

static double af__clamp_d(dm_in double dVal, dm_in double dLo, dm_in double dHi)
{
  if (dVal < dLo)
  {
    return dLo;
  }
  if (dVal > dHi)
  {
    return dHi;
  }
  return dVal;
}

static void af__bounds_max(dm_in const af_rect_f_st *pstB, dm_out double *pdMaxX,
                           dm_out double *pdMaxY)
{
  double dSpanX = (pstB->dW > 1.0) ? (pstB->dW - 1.0) : 0.0;
  double dSpanY = (pstB->dH > 1.0) ? (pstB->dH - 1.0) : 0.0;

  *pdMaxX = pstB->dX + dSpanX;
  *pdMaxY = pstB->dY + dSpanY;
}

static void af__inj_apply_bounds(dm_io af_inj_ctx_st *pstInj, dm_in uint32 u32DispId,
                                 dm_in bool_t bCenter)
{
  af_rect_f_st stNew;
  double dMaxX = 0.0;
  double dMaxY = 0.0;

  memset(&stNew, 0, sizeof(stNew));
  if ((0u == u32DispId) || (!pstInj->stOps.pfGetBounds(u32DispId, &stNew)))
  {
    return;
  }
  if ((!isfinite(stNew.dX)) || (!isfinite(stNew.dY)) || (!isfinite(stNew.dW)) ||
      (!isfinite(stNew.dH)) || (stNew.dW <= 0.0) || (stNew.dH <= 0.0))
  {
    return;
  }
  pstInj->stVdBounds = stNew;
  af__bounds_max(&pstInj->stVdBounds, &dMaxX, &dMaxY);
  if (bCenter)
  {
    pstInj->dCurX = pstInj->stVdBounds.dX + ((dMaxX - pstInj->stVdBounds.dX) * 0.5);
    pstInj->dCurY = pstInj->stVdBounds.dY + ((dMaxY - pstInj->stVdBounds.dY) * 0.5);
    return;
  }
  pstInj->dCurX = af__clamp_d(pstInj->dCurX, pstInj->stVdBounds.dX, dMaxX);
  pstInj->dCurY = af__clamp_d(pstInj->dCurY, pstInj->stVdBounds.dY, dMaxY);
}

static void af__inj_map_abs(dm_io af_inj_ctx_st *pstInj, dm_in float fNx, dm_in float fNy)
{
  double dNx = isfinite(fNx) ? af__clamp_d((double)fNx, 0.0, 1.0) : 0.0;
  double dNy = isfinite(fNy) ? af__clamp_d((double)fNy, 0.0, 1.0) : 0.0;
  double dMaxX = 0.0;
  double dMaxY = 0.0;
  double dX = 0.0;
  double dY = 0.0;

  af__bounds_max(&pstInj->stVdBounds, &dMaxX, &dMaxY);
  dX = pstInj->stVdBounds.dX + (dNx * (dMaxX - pstInj->stVdBounds.dX));
  dY = pstInj->stVdBounds.dY + (dNy * (dMaxY - pstInj->stVdBounds.dY));
  pstInj->dCurX = af__clamp_d(dX, pstInj->stVdBounds.dX, dMaxX);
  pstInj->dCurY = af__clamp_d(dY, pstInj->stVdBounds.dY, dMaxY);
}

static uint8 af__inj_active_drag_btn(dm_in uint8 u8Mask, dm_out uint8 *pu8Kind)
{
  if (0u != (u8Mask & 0x01u))
  {
    *pu8Kind = 3u;
    return 0u;
  }
  if (0u != (u8Mask & 0x02u))
  {
    *pu8Kind = 3u;
    return 1u;
  }
  if (0u != (u8Mask & 0x04u))
  {
    *pu8Kind = 3u;
    return 2u;
  }
  *pu8Kind = 0u;
  return 0u;
}

static int32 af__clamp_i32(dm_in int64 i64Val)
{
  if (i64Val > (int64)INT32_MAX)
  {
    return INT32_MAX;
  }
  if (i64Val < (int64)INT32_MIN)
  {
    return INT32_MIN;
  }
  return (int32)i64Val;
}

/* ============================ 修饰键与键盘状态机 ============================ */

static bool_t af__is_mod_hid(dm_in uint16 u16Hid)
{
  return ((u16Hid >= AF_HID_KEY_LCTRL) && (u16Hid <= AF_HID_KEY_RGUI)) ? DM_TRUE : DM_FALSE;
}

static uint64 af__compute_mod_flags(dm_in const af_inj_ctx_st *pstInj)
{
  uint64 u64F = pstInj->u64ModFlags & AF_MOD_FLAG_ALPHA_SHIFT;

  if ((0u != pstInj->au8HeldKeys[AF_HID_KEY_LCTRL]) ||
      (0u != pstInj->au8HeldKeys[AF_HID_KEY_RCTRL]))
  {
    u64F |= AF_MOD_FLAG_CONTROL;
  }
  if ((0u != pstInj->au8HeldKeys[AF_HID_KEY_LSHIFT]) ||
      (0u != pstInj->au8HeldKeys[AF_HID_KEY_RSHIFT]))
  {
    u64F |= AF_MOD_FLAG_SHIFT;
  }
  if ((0u != pstInj->au8HeldKeys[AF_HID_KEY_LALT]) || (0u != pstInj->au8HeldKeys[AF_HID_KEY_RALT]))
  {
    u64F |= AF_MOD_FLAG_ALTERNATE;
  }
  if ((0u != pstInj->au8HeldKeys[AF_HID_KEY_LGUI]) || (0u != pstInj->au8HeldKeys[AF_HID_KEY_RGUI]))
  {
    u64F |= AF_MOD_FLAG_COMMAND;
  }
  return u64F;
}

static void af__inj_handle_key(dm_io af_inj_ctx_st *pstInj, dm_in uint16 u16Hid, dm_in bool_t bDown)
{
  if ((u16Hid >= 256u) || (af_plat_hid_to_vk(u16Hid) < 0))
  {
    return;
  }
  if (af__is_mod_hid(u16Hid))
  {
    uint64 u64New = 0u;
    pstInj->au8HeldKeys[u16Hid] = bDown ? 1u : 0u;
    u64New = af__compute_mod_flags(pstInj);
    if (u64New != pstInj->u64ModFlags)
    {
      pstInj->stOps.pfInjectKey(u16Hid, bDown, &pstInj->u64ModFlags);
      pstInj->u64ModFlags = u64New;
    }
    return;
  }
  pstInj->au8HeldKeys[u16Hid] = bDown ? 1u : 0u;
  pstInj->stOps.pfInjectKey(u16Hid, bDown, &pstInj->u64ModFlags);
}

/* ============================ 事件分发与安全分片 ============================ */

static void af__inj_flush_rel(dm_io af_inj_ctx_st *pstInj, dm_io bool_t *pbHasMove,
                              dm_io int64 *pi64Dx, dm_io int64 *pi64Dy)
{
  af_inject_mouse_st stEv;
  double dMaxX = 0.0;
  double dMaxY = 0.0;

  if (!(*pbHasMove))
  {
    return;
  }
  af__bounds_max(&pstInj->stVdBounds, &dMaxX, &dMaxY);
  pstInj->dCurX = af__clamp_d(pstInj->dCurX + (double)(*pi64Dx), pstInj->stVdBounds.dX, dMaxX);
  pstInj->dCurY = af__clamp_d(pstInj->dCurY + (double)(*pi64Dy), pstInj->stVdBounds.dY, dMaxY);

  memset(&stEv, 0, sizeof(stEv));
  stEv.dX = pstInj->dCurX;
  stEv.dY = pstInj->dCurY;
  stEv.i32DeltaX = af__clamp_i32(*pi64Dx);
  stEv.i32DeltaY = af__clamp_i32(*pi64Dy);
  stEv.u8Btn = af__inj_active_drag_btn(pstInj->u8BtnMask, &stEv.u8Kind);
  stEv.u64Flags = pstInj->u64ModFlags;
  pstInj->stOps.pfInjectMouse(&stEv);

  *pbHasMove = DM_FALSE;
  *pi64Dx = 0;
  *pi64Dy = 0;
}

static void af__inj_handle_mouse_btn(dm_io af_inj_ctx_st *pstInj, dm_in const af_inj_item_st *pstIt)
{
  af_inject_mouse_st stEv;
  uint8 u8Btn = pstIt->u.stBtn.u8Btn;

  if (u8Btn > 2u)
  {
    return;
  }
  if (pstIt->u.stBtn.bHasAbs)
  {
    af__inj_map_abs(pstInj, pstIt->u.stBtn.fNx, pstIt->u.stBtn.fNy);
  }
  if (pstIt->u.stBtn.bDown)
  {
    pstInj->u8BtnMask = (uint8)(pstInj->u8BtnMask | (uint8)(1u << u8Btn));
  }
  else
  {
    pstInj->u8BtnMask = (uint8)(pstInj->u8BtnMask & (uint8)(~(1u << u8Btn)));
  }

  memset(&stEv, 0, sizeof(stEv));
  stEv.dX = pstInj->dCurX;
  stEv.dY = pstInj->dCurY;
  stEv.u8Kind = pstIt->u.stBtn.bDown ? 1u : 2u;
  stEv.u8Btn = u8Btn;
  stEv.u64Flags = pstInj->u64ModFlags;
  pstInj->stOps.pfInjectMouse(&stEv);
}

static void af__inj_handle_pencil(dm_io af_inj_ctx_st *pstInj, dm_in const af_inj_item_st *pstIt)
{
  af_inject_pencil_st stEv;
  uint8 u8Ph = pstIt->u.stPencil.u8Phase;

  if (u8Ph > AF_PENCIL_CANCEL)
  {
    return;
  }
  af__inj_map_abs(pstInj, pstIt->u.stPencil.fNx, pstIt->u.stPencil.fNy);
  memset(&stEv, 0, sizeof(stEv));
  stEv.dX = pstInj->dCurX;
  stEv.dY = pstInj->dCurY;
  stEv.fPressure = isfinite(pstIt->u.stPencil.fPressure)
                       ? (float)af__clamp_d((double)pstIt->u.stPencil.fPressure, 0.0, 1.0)
                       : 0.0f;
  stEv.fTiltX = isfinite(pstIt->u.stPencil.fTiltX)
                    ? (float)af__clamp_d((double)pstIt->u.stPencil.fTiltX, -1.0, 1.0)
                    : 0.0f;
  stEv.fTiltY = isfinite(pstIt->u.stPencil.fTiltY)
                    ? (float)af__clamp_d((double)pstIt->u.stPencil.fTiltY, -1.0, 1.0)
                    : 0.0f;
  stEv.u8Phase = u8Ph;
  stEv.u64Flags = pstInj->u64ModFlags;

  if ((AF_PENCIL_UP == u8Ph) || (AF_PENCIL_CANCEL == u8Ph))
  {
    if ((!pstInj->bPencilDown) && (!pstInj->bPencilProx))
    {
      return;
    }
    stEv.u8Phase = pstInj->bPencilDown ? u8Ph : AF_PENCIL_HOVER;
    stEv.bExitProx = pstInj->bPencilProx;
    pstInj->bPencilDown = DM_FALSE;
    pstInj->bPencilProx = DM_FALSE;
  }
  else
  {
    if (!pstInj->bPencilProx)
    {
      stEv.bEnterProx = DM_TRUE;
      pstInj->bPencilProx = DM_TRUE;
    }
    if (AF_PENCIL_DOWN == u8Ph)
    {
      pstInj->bPencilDown = DM_TRUE;
    }
  }
  pstInj->stOps.pfInjectPencil(&stEv);
}

static void af__inj_handle_text(dm_io af_inj_ctx_st *pstInj, dm_in const uint8 *pu8Utf8,
                                dm_in uint16 u16Len)
{
  char acUtf8[AF_DP_TEXT_MAX_BYTES + 1u];
  uint16 au16Buf[AF_DP_TEXT_MAX_BYTES + 2u];
  size_t uUnits = AF_DP_TEXT_MAX_BYTES + 2u;
  size_t uOff = 0u;
  uint16 i = 0u;

  if ((NULL == pu8Utf8) || (0u == u16Len) || (u16Len > AF_DP_TEXT_MAX_BYTES))
  {
    return;
  }
  for (i = 0u; i < u16Len; i++)
  {
    if (0u == pu8Utf8[i])
    {
      return;
    }
  }
  memcpy(acUtf8, pu8Utf8, u16Len);
  acUtf8[u16Len] = '\0';

  if ((DM_SUCCESS != dm_utf8_to_utf16(acUtf8, au16Buf, &uUnits)) || (0u == uUnits))
  {
    dm_secure_zero(acUtf8, sizeof(acUtf8));
    dm_secure_zero(au16Buf, sizeof(au16Buf));
    return;
  }

  while (uOff < uUnits)
  {
    size_t uChunk = uUnits - uOff;
    if (uChunk > AF_INPUT_UTF16_CHUNK_MAX)
    {
      uint16 u16Last = 0u;
      uChunk = AF_INPUT_UTF16_CHUNK_MAX;
      u16Last = au16Buf[uOff + uChunk - 1u];
      if ((u16Last >= 0xD800u) && (u16Last <= 0xDBFFu))
      {
        uChunk--;
      }
    }
    pstInj->stOps.pfInjectUnicode(&au16Buf[uOff], uChunk);
    uOff += uChunk;
  }
  dm_secure_zero(acUtf8, sizeof(acUtf8));
  dm_secure_zero(au16Buf, sizeof(au16Buf));
}

static void af__inj_release_all(dm_io af_inj_ctx_st *pstInj)
{
  uint32 i = 0u;
  uint8 u8Btn = 0u;
  bool_t bHadMod = (0u != pstInj->u64ModFlags) ? DM_TRUE : DM_FALSE;

  for (i = 0u; i < 256u; i++)
  {
    if ((0u != pstInj->au8HeldKeys[i]) && (!af__is_mod_hid((uint16)i)))
    {
      pstInj->au8HeldKeys[i] = 0u;
      pstInj->stOps.pfInjectKey((uint16)i, DM_FALSE, &pstInj->u64ModFlags);
    }
  }
  for (u8Btn = 0u; u8Btn < 3u; u8Btn++)
  {
    if (0u != (pstInj->u8BtnMask & (1u << u8Btn)))
    {
      af_inject_mouse_st stEv;
      pstInj->u8BtnMask = (uint8)(pstInj->u8BtnMask & (uint8)(~(1u << u8Btn)));
      memset(&stEv, 0, sizeof(stEv));
      stEv.dX = pstInj->dCurX;
      stEv.dY = pstInj->dCurY;
      stEv.u8Kind = 2u;
      stEv.u8Btn = u8Btn;
      stEv.u64Flags = pstInj->u64ModFlags;
      pstInj->stOps.pfInjectMouse(&stEv);
    }
  }
  if (pstInj->bPencilDown || pstInj->bPencilProx)
  {
    af_inject_pencil_st stP;
    memset(&stP, 0, sizeof(stP));
    stP.dX = pstInj->dCurX;
    stP.dY = pstInj->dCurY;
    stP.u8Phase = pstInj->bPencilDown ? AF_PENCIL_CANCEL : AF_PENCIL_HOVER;
    stP.bExitProx = DM_TRUE;
    stP.u64Flags = pstInj->u64ModFlags;
    pstInj->bPencilDown = DM_FALSE;
    pstInj->bPencilProx = DM_FALSE;
    pstInj->stOps.pfInjectPencil(&stP);
  }
  for (i = AF_HID_KEY_LCTRL; i <= AF_HID_KEY_RGUI; i++)
  {
    if (0u != pstInj->au8HeldKeys[i])
    {
      pstInj->au8HeldKeys[i] = 0u;
      bHadMod = DM_TRUE;
    }
  }
  if (bHadMod)
  {
    pstInj->u64ModFlags = 0u;
    pstInj->stOps.pfInjectFlags(0u);
  }
}

static void af__inj_dispatch_item(dm_io af_inj_ctx_st *pstInj, dm_in const af_inj_item_st *pstIt)
{
  af_inject_mouse_st stM;

  switch (pstIt->eKind)
  {
  case AF_INJ_SCROLL:
    pstInj->stOps.pfInjectScroll(pstIt->u.stScroll.i32Wx, pstIt->u.stScroll.i32Wy,
                                 pstInj->u64ModFlags);
    break;
  case AF_INJ_MOUSE_ABS:
    af__inj_map_abs(pstInj, pstIt->u.stAbs.fNx, pstIt->u.stAbs.fNy);
    memset(&stM, 0, sizeof(stM));
    stM.dX = pstInj->dCurX;
    stM.dY = pstInj->dCurY;
    stM.u8Btn = af__inj_active_drag_btn(pstInj->u8BtnMask, &stM.u8Kind);
    stM.u64Flags = pstInj->u64ModFlags;
    pstInj->stOps.pfInjectMouse(&stM);
    break;
  case AF_INJ_MOUSE_BTN:
    af__inj_handle_mouse_btn(pstInj, pstIt);
    break;
  case AF_INJ_KEY:
    af__inj_handle_key(pstInj, pstIt->u.stKey.u16HidUsage, pstIt->u.stKey.bDown);
    break;
  case AF_INJ_PENCIL:
    af__inj_handle_pencil(pstInj, pstIt);
    break;
  case AF_INJ_TEXT:
    af__inj_handle_text(pstInj, pstIt->u.stText.au8Utf8, pstIt->u.stText.u16Len);
    break;
  case AF_INJ_RELEASE_ALL:
    af__inj_release_all(pstInj);
    break;
  case AF_INJ_BOUNDS_UPD:
    af__inj_apply_bounds(pstInj, pstIt->u.u32DispId, DM_FALSE);
    break;
  default:
    break;
  }
}

/* ============================ SPSC 环与消费者线程 ============================ */

static bool_t af__inj_pop(dm_io af_inj_ctx_st *pstInj, dm_out af_inj_item_st *pstOut)
{
  uint32 u32Head = atomic_load_explicit(&pstInj->u32Head, memory_order_relaxed);
  uint32 u32Tail = atomic_load_explicit(&pstInj->u32Tail, memory_order_acquire);

  if (u32Head == u32Tail)
  {
    return DM_FALSE;
  }
  *pstOut = pstInj->pastSlots[u32Head & AF_INPUT_SPSC_MASK];
  atomic_store_explicit(&pstInj->u32Head, u32Head + 1u, memory_order_release);
  return DM_TRUE;
}

static bool_t af__inj_is_empty(dm_in const af_inj_ctx_st *pstInj)
{
  uint32 u32Head = atomic_load_explicit(&pstInj->u32Head, memory_order_relaxed);
  uint32 u32Tail = atomic_load_explicit(&pstInj->u32Tail, memory_order_acquire);

  if (u32Head != u32Tail)
  {
    return DM_FALSE;
  }
  return atomic_load_explicit(&pstInj->bPendingReleaseAll, memory_order_acquire) ? DM_FALSE
                                                                                 : DM_TRUE;
}

void af__inj_drain_batch(dm_io af_inj_ctx_st *pstInj)
{
  af_inj_item_st stIt;
  bool_t bHasMove = DM_FALSE;
  int64 i64Dx = 0;
  int64 i64Dy = 0;

  while (af__inj_pop(pstInj, &stIt))
  {
    if (AF_INJ_REL_MOVE == stIt.eKind)
    {
      i64Dx += (int64)stIt.u.stRel.i32Dx;
      i64Dy += (int64)stIt.u.stRel.i32Dy;
      bHasMove = DM_TRUE;
      continue;
    }
    af__inj_flush_rel(pstInj, &bHasMove, &i64Dx, &i64Dy);
    af__inj_dispatch_item(pstInj, &stIt);
  }
  af__inj_flush_rel(pstInj, &bHasMove, &i64Dx, &i64Dy);
  if (atomic_exchange_explicit(&pstInj->bPendingReleaseAll, DM_FALSE, memory_order_acq_rel))
  {
    af__inj_release_all(pstInj);
  }
}

static void af__inj_thread_entry(dm_io void *pvArg)
{
  af_inj_ctx_st *pstInj = (af_inj_ctx_st *)pvArg;

  for (;;)
  {
    af__inj_drain_batch(pstInj);
    dm_mutex_lock(pstInj->pstWakeMtx);
    while (af__inj_is_empty(pstInj) && (!pstInj->bStop))
    {
      dm_cond_wait(pstInj->pstWakeCond, pstInj->pstWakeMtx);
    }
    if (pstInj->bStop && af__inj_is_empty(pstInj))
    {
      dm_mutex_unlock(pstInj->pstWakeMtx);
      break;
    }
    dm_mutex_unlock(pstInj->pstWakeMtx);
  }
}

bool_t af__inj_push(dm_io af_inj_ctx_st *pstInj, dm_in const af_inj_item_st *pstItem)
{
  uint32 u32Tail = 0u;
  uint32 u32Head = 0u;

  if ((NULL == pstInj) || (NULL == pstItem) || (NULL == pstInj->pastSlots))
  {
    return DM_FALSE;
  }
  u32Tail = atomic_load_explicit(&pstInj->u32Tail, memory_order_relaxed);
  u32Head = atomic_load_explicit(&pstInj->u32Head, memory_order_acquire);
  if ((u32Tail - u32Head) >= AF_INPUT_SPSC_CAP)
  {
    if (AF_INJ_RELEASE_ALL == pstItem->eKind)
    {
      atomic_store_explicit(&pstInj->bPendingReleaseAll, DM_TRUE, memory_order_release);
      return DM_TRUE;
    }
    return DM_FALSE;
  }
  pstInj->pastSlots[u32Tail & AF_INPUT_SPSC_MASK] = *pstItem;
  atomic_store_explicit(&pstInj->u32Tail, u32Tail + 1u, memory_order_release);
  return DM_TRUE;
}

void af__inj_wake(dm_io af_inj_ctx_st *pstInj)
{
  if ((NULL == pstInj) || (NULL == pstInj->pstWakeMtx) || (NULL == pstInj->pstWakeCond))
  {
    return;
  }
  dm_mutex_lock(pstInj->pstWakeMtx);
  dm_cond_signal(pstInj->pstWakeCond);
  dm_mutex_unlock(pstInj->pstWakeMtx);
}

int af__inj_init(dm_out af_inj_ctx_st *pstInj, dm_in const af_input_ops_st *pstOps,
                 dm_in uint32 u32DispId)
{
  if ((NULL == pstInj) || (NULL == pstOps))
  {
    return DM_INVALID;
  }
  memset(pstInj, 0, sizeof(*pstInj));
  pstInj->stOps = *pstOps;
  pstInj->pastSlots =
      (af_inj_item_st *)dm_calloc_tag(AF_INPUT_SPSC_CAP, sizeof(af_inj_item_st), AF_TAG_INPUT);
  if (NULL == pstInj->pastSlots)
  {
    return DM_ALLOC;
  }
  if (DM_SUCCESS != dm_mutex_create(&pstInj->pstWakeMtx))
  {
    dm_free_tag(pstInj->pastSlots);
    pstInj->pastSlots = NULL;
    return DM_ALLOC;
  }
  if (DM_SUCCESS != dm_cond_create(&pstInj->pstWakeCond))
  {
    dm_mutex_destroy(pstInj->pstWakeMtx);
    pstInj->pstWakeMtx = NULL;
    dm_free_tag(pstInj->pastSlots);
    pstInj->pastSlots = NULL;
    return DM_ALLOC;
  }
  atomic_init(&pstInj->u32Head, 0u);
  atomic_init(&pstInj->u32Tail, 0u);
  atomic_init(&pstInj->bPendingReleaseAll, DM_FALSE);
  af__inj_apply_bounds(pstInj, u32DispId, DM_TRUE);
  return DM_SUCCESS;
}

int af__inj_start_thread(dm_io af_inj_ctx_st *pstInj)
{
  dm_thread_opt_st stOpt;

  if (NULL == pstInj)
  {
    return DM_INVALID;
  }
  memset(&stOpt, 0, sizeof(stOpt));
  stOpt.pcName = "af_inject";
  return dm_thread_create_ex(&stOpt, af__inj_thread_entry, pstInj, &pstInj->pstThread);
}

void af__inj_stop_and_fini(dm_io af_inj_ctx_st *pstInj)
{
  af_inj_item_st stRel;

  if (NULL == pstInj)
  {
    return;
  }
  if (NULL != pstInj->pstThread)
  {
    memset(&stRel, 0, sizeof(stRel));
    stRel.eKind = AF_INJ_RELEASE_ALL;
    (void)af__inj_push(pstInj, &stRel);
    dm_mutex_lock(pstInj->pstWakeMtx);
    pstInj->bStop = DM_TRUE;
    dm_cond_broadcast(pstInj->pstWakeCond);
    dm_mutex_unlock(pstInj->pstWakeMtx);
    (void)dm_thread_join(pstInj->pstThread);
    pstInj->pstThread = NULL;
  }
  if (NULL != pstInj->pstWakeCond)
  {
    dm_cond_destroy(pstInj->pstWakeCond);
    pstInj->pstWakeCond = NULL;
  }
  if (NULL != pstInj->pstWakeMtx)
  {
    dm_mutex_destroy(pstInj->pstWakeMtx);
    pstInj->pstWakeMtx = NULL;
  }
  if (NULL != pstInj->pastSlots)
  {
    dm_secure_zero(pstInj->pastSlots, AF_INPUT_SPSC_CAP * sizeof(af_inj_item_st));
    dm_free_tag(pstInj->pastSlots);
    pstInj->pastSlots = NULL;
  }
}

bool_t af__inj_ev_to_item(dm_in const af_dp_ev_t *pstEv, dm_out af_inj_item_st *pstOut)
{
  memset(pstOut, 0, sizeof(*pstOut));
  switch (pstEv->kind)
  {
  case AF_EV_MOUSE_ABS:
    pstOut->eKind = AF_INJ_MOUSE_ABS;
    pstOut->u.stAbs.fNx = pstEv->u.abs.nx;
    pstOut->u.stAbs.fNy = pstEv->u.abs.ny;
    return DM_TRUE;
  case AF_EV_MOUSE_BTN:
    pstOut->eKind = AF_INJ_MOUSE_BTN;
    pstOut->u.stBtn.u8Btn = pstEv->u.btn.btn;
    pstOut->u.stBtn.bDown = (0u != pstEv->u.btn.down) ? DM_TRUE : DM_FALSE;
    pstOut->u.stBtn.bHasAbs = (0u != pstEv->u.btn.has_abs) ? DM_TRUE : DM_FALSE;
    pstOut->u.stBtn.fNx = pstEv->u.btn.nx;
    pstOut->u.stBtn.fNy = pstEv->u.btn.ny;
    return DM_TRUE;
  case AF_EV_KEY:
    pstOut->eKind = AF_INJ_KEY;
    pstOut->u.stKey.u16HidUsage = pstEv->u.key.hid_usage;
    pstOut->u.stKey.bDown = (0u != pstEv->u.key.down) ? DM_TRUE : DM_FALSE;
    return DM_TRUE;
  case AF_EV_PENCIL:
    pstOut->eKind = AF_INJ_PENCIL;
    pstOut->u.stPencil.u8Phase = pstEv->u.pencil.phase;
    pstOut->u.stPencil.fNx = pstEv->u.pencil.nx;
    pstOut->u.stPencil.fNy = pstEv->u.pencil.ny;
    pstOut->u.stPencil.fPressure = pstEv->u.pencil.pressure;
    pstOut->u.stPencil.fTiltX = pstEv->u.pencil.tilt_x;
    pstOut->u.stPencil.fTiltY = pstEv->u.pencil.tilt_y;
    return DM_TRUE;
  case AF_EV_TEXT_COMMIT:
    pstOut->eKind = AF_INJ_TEXT;
    pstOut->u.stText.u16Len = pstEv->u.text.utf8_len;
    if (pstEv->u.text.utf8_len > 0u)
    {
      memcpy(pstOut->u.stText.au8Utf8, pstEv->u.text.utf8, pstEv->u.text.utf8_len);
    }
    return DM_TRUE;
  default:
    return DM_FALSE;
  }
}

#ifdef __cplusplus
}
#endif
