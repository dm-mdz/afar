/**
 ******************************************************************************
 * @file    af_plat_inject.m
 * @author  Afar_C
 * @version 1.0.0
 * @brief   macOS 极薄键鼠/Pencil/Unicode 注入与辅助功能/安全输入平台适配（DD v2.1 §6.2）。
 ******************************************************************************
 */

#include <dm.h>
#include "af_direct_proto.h"
#include "plat/af_plat.h"
#include "plat/af_objc_undef.h"

#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/hidsystem/IOHIDLib.h>
#import <IOKit/hidsystem/IOHIDParameter.h>

#include <string.h>

/* ============================ USB HID Page 0x07 -> macOS kVK_* ============================ */
/* 存储 (kVK_* + 1)，未定义槽位自动零初始化为 0（映射返回 -1），零 GCC 扩展与覆盖警告 */

#define AF_VK(k) ((int16_t)((k) + 1))

static const int16_t g_ai16HidToVkPlus1[256] = {
    /* A - Z (0x04..0x1D) */
    [0x04] = AF_VK(kVK_ANSI_A),
    [0x05] = AF_VK(kVK_ANSI_B),
    [0x06] = AF_VK(kVK_ANSI_C),
    [0x07] = AF_VK(kVK_ANSI_D),
    [0x08] = AF_VK(kVK_ANSI_E),
    [0x09] = AF_VK(kVK_ANSI_F),
    [0x0A] = AF_VK(kVK_ANSI_G),
    [0x0B] = AF_VK(kVK_ANSI_H),
    [0x0C] = AF_VK(kVK_ANSI_I),
    [0x0D] = AF_VK(kVK_ANSI_J),
    [0x0E] = AF_VK(kVK_ANSI_K),
    [0x0F] = AF_VK(kVK_ANSI_L),
    [0x10] = AF_VK(kVK_ANSI_M),
    [0x11] = AF_VK(kVK_ANSI_N),
    [0x12] = AF_VK(kVK_ANSI_O),
    [0x13] = AF_VK(kVK_ANSI_P),
    [0x14] = AF_VK(kVK_ANSI_Q),
    [0x15] = AF_VK(kVK_ANSI_R),
    [0x16] = AF_VK(kVK_ANSI_S),
    [0x17] = AF_VK(kVK_ANSI_T),
    [0x18] = AF_VK(kVK_ANSI_U),
    [0x19] = AF_VK(kVK_ANSI_V),
    [0x1A] = AF_VK(kVK_ANSI_W),
    [0x1B] = AF_VK(kVK_ANSI_X),
    [0x1C] = AF_VK(kVK_ANSI_Y),
    [0x1D] = AF_VK(kVK_ANSI_Z),
    /* 1 - 9, 0 (0x1E..0x27) */
    [0x1E] = AF_VK(kVK_ANSI_1),
    [0x1F] = AF_VK(kVK_ANSI_2),
    [0x20] = AF_VK(kVK_ANSI_3),
    [0x21] = AF_VK(kVK_ANSI_4),
    [0x22] = AF_VK(kVK_ANSI_5),
    [0x23] = AF_VK(kVK_ANSI_6),
    [0x24] = AF_VK(kVK_ANSI_7),
    [0x25] = AF_VK(kVK_ANSI_8),
    [0x26] = AF_VK(kVK_ANSI_9),
    [0x27] = AF_VK(kVK_ANSI_0),
    /* 控制与符号键 (0x28..0x39) */
    [0x28] = AF_VK(kVK_Return),
    [0x29] = AF_VK(kVK_Escape),
    [0x2A] = AF_VK(kVK_Delete),
    [0x2B] = AF_VK(kVK_Tab),
    [0x2C] = AF_VK(kVK_Space),
    [0x2D] = AF_VK(kVK_ANSI_Minus),
    [0x2E] = AF_VK(kVK_ANSI_Equal),
    [0x2F] = AF_VK(kVK_ANSI_LeftBracket),
    [0x30] = AF_VK(kVK_ANSI_RightBracket),
    [0x31] = AF_VK(kVK_ANSI_Backslash),
    [0x32] = AF_VK(kVK_ANSI_Backslash),
    [0x33] = AF_VK(kVK_ANSI_Semicolon),
    [0x34] = AF_VK(kVK_ANSI_Quote),
    [0x35] = AF_VK(kVK_ANSI_Grave),
    [0x36] = AF_VK(kVK_ANSI_Comma),
    [0x37] = AF_VK(kVK_ANSI_Period),
    [0x38] = AF_VK(kVK_ANSI_Slash),
    [0x39] = AF_VK(kVK_CapsLock),
    /* F1 - F12 (0x3A..0x45) */
    [0x3A] = AF_VK(kVK_F1),
    [0x3B] = AF_VK(kVK_F2),
    [0x3C] = AF_VK(kVK_F3),
    [0x3D] = AF_VK(kVK_F4),
    [0x3E] = AF_VK(kVK_F5),
    [0x3F] = AF_VK(kVK_F6),
    [0x40] = AF_VK(kVK_F7),
    [0x41] = AF_VK(kVK_F8),
    [0x42] = AF_VK(kVK_F9),
    [0x43] = AF_VK(kVK_F10),
    [0x44] = AF_VK(kVK_F11),
    [0x45] = AF_VK(kVK_F12),
    /* 导航与编辑键 (0x46..0x52) */
    [0x46] = AF_VK(kVK_F13),
    [0x47] = AF_VK(kVK_F14),
    [0x48] = AF_VK(kVK_F15),
    [0x49] = AF_VK(kVK_Help),
    [0x4A] = AF_VK(kVK_Home),
    [0x4B] = AF_VK(kVK_PageUp),
    [0x4C] = AF_VK(kVK_ForwardDelete),
    [0x4D] = AF_VK(kVK_End),
    [0x4E] = AF_VK(kVK_PageDown),
    [0x4F] = AF_VK(kVK_RightArrow),
    [0x50] = AF_VK(kVK_LeftArrow),
    [0x51] = AF_VK(kVK_DownArrow),
    [0x52] = AF_VK(kVK_UpArrow),
    /* 小键盘 (0x53..0x67) */
    [0x53] = AF_VK(kVK_ANSI_KeypadClear),
    [0x54] = AF_VK(kVK_ANSI_KeypadDivide),
    [0x55] = AF_VK(kVK_ANSI_KeypadMultiply),
    [0x56] = AF_VK(kVK_ANSI_KeypadMinus),
    [0x57] = AF_VK(kVK_ANSI_KeypadPlus),
    [0x58] = AF_VK(kVK_ANSI_KeypadEnter),
    [0x59] = AF_VK(kVK_ANSI_Keypad1),
    [0x5A] = AF_VK(kVK_ANSI_Keypad2),
    [0x5B] = AF_VK(kVK_ANSI_Keypad3),
    [0x5C] = AF_VK(kVK_ANSI_Keypad4),
    [0x5D] = AF_VK(kVK_ANSI_Keypad5),
    [0x5E] = AF_VK(kVK_ANSI_Keypad6),
    [0x5F] = AF_VK(kVK_ANSI_Keypad7),
    [0x60] = AF_VK(kVK_ANSI_Keypad8),
    [0x61] = AF_VK(kVK_ANSI_Keypad9),
    [0x62] = AF_VK(kVK_ANSI_Keypad0),
    [0x63] = AF_VK(kVK_ANSI_KeypadDecimal),
    [0x64] = AF_VK(kVK_ISO_Section),
    [0x67] = AF_VK(kVK_ANSI_KeypadEquals),
    /* F13 - F20 (0x68..0x6F) */
    [0x68] = AF_VK(kVK_F13),
    [0x69] = AF_VK(kVK_F14),
    [0x6A] = AF_VK(kVK_F15),
    [0x6B] = AF_VK(kVK_F16),
    [0x6C] = AF_VK(kVK_F17),
    [0x6D] = AF_VK(kVK_F18),
    [0x6E] = AF_VK(kVK_F19),
    [0x6F] = AF_VK(kVK_F20),
    /* 左右修饰键 (0xE0..0xE7) */
    [0xE0] = AF_VK(kVK_Control),
    [0xE1] = AF_VK(kVK_Shift),
    [0xE2] = AF_VK(kVK_Option),
    [0xE3] = AF_VK(kVK_Command),
    [0xE4] = AF_VK(kVK_RightControl),
    [0xE5] = AF_VK(kVK_RightShift),
    [0xE6] = AF_VK(kVK_RightOption),
    [0xE7] = AF_VK(kVK_RightCommand),
};

#undef AF_VK

/* ============================ 权限、显示器边界与安全输入 ============================ */

bool_t af_plat_perm_accessibility(void)
{
  return AXIsProcessTrusted() ? DM_TRUE : DM_FALSE;
}

void af_plat_request_screen_capture(void)
{
  (void)CGRequestScreenCaptureAccess();
}

void af_plat_request_accessibility(void)
{
  const void *apKeys[1] = {kAXTrustedCheckOptionPrompt};
  const void *apVals[1] = {kCFBooleanTrue};
  CFDictionaryRef pOpts = CFDictionaryCreate(kCFAllocatorDefault, apKeys, apVals, 1,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
  if (NULL != pOpts)
  {
    (void)AXIsProcessTrustedWithOptions(pOpts);
    CFRelease(pOpts);
  }
}

bool_t af_plat_disp_bounds(dm_in uint32 u32DispId, dm_out af_rect_f_st *pstOut)
{
  CGRect stRect;

  if ((0u == u32DispId) || (NULL == pstOut))
  {
    return DM_FALSE;
  }
  stRect = CGDisplayBounds((CGDirectDisplayID)u32DispId);
  if (CGRectIsEmpty(stRect))
  {
    return DM_FALSE;
  }
  pstOut->dX = (double)stRect.origin.x;
  pstOut->dY = (double)stRect.origin.y;
  pstOut->dW = (double)stRect.size.width;
  pstOut->dH = (double)stRect.size.height;
  return DM_TRUE;
}

bool_t af_plat_is_secure_input(void)
{
  return IsSecureEventInputEnabled() ? DM_TRUE : DM_FALSE;
}

int af_plat_hid_to_vk(dm_in uint16 u16HidUsage)
{
  int16_t i16Val = 0;

  if (u16HidUsage >= 256u)
  {
    return -1;
  }
  i16Val = g_ai16HidToVkPlus1[u16HidUsage];
  return (i16Val > 0) ? (int)(i16Val - 1) : -1;
}

/* ============================ 鼠标与滚轮注入 ============================ */

static CGEventType af__mouse_event_type(dm_in uint8 u8Kind, dm_in uint8 u8Btn,
                                        dm_out CGMouseButton *peBtn)
{
  *peBtn = (1u == u8Btn) ? kCGMouseButtonRight
                         : ((2u == u8Btn) ? kCGMouseButtonCenter : kCGMouseButtonLeft);
  if (1u == u8Kind)
  {
    return (1u == u8Btn) ? kCGEventRightMouseDown
                         : ((2u == u8Btn) ? kCGEventOtherMouseDown : kCGEventLeftMouseDown);
  }
  if (2u == u8Kind)
  {
    return (1u == u8Btn) ? kCGEventRightMouseUp
                         : ((2u == u8Btn) ? kCGEventOtherMouseUp : kCGEventLeftMouseUp);
  }
  if (3u == u8Kind)
  {
    return (1u == u8Btn) ? kCGEventRightMouseDragged
                         : ((2u == u8Btn) ? kCGEventOtherMouseDragged : kCGEventLeftMouseDragged);
  }
  return kCGEventMouseMoved;
}

void af_plat_inject_mouse(dm_in const af_inject_mouse_st *pstEv)
{
  CGMouseButton eBtn = kCGMouseButtonLeft;
  CGEventType eType = kCGEventMouseMoved;
  CGEventRef pEv = NULL;

  if (NULL == pstEv)
  {
    return;
  }
  eType = af__mouse_event_type(pstEv->u8Kind, pstEv->u8Btn, &eBtn);
  pEv = CGEventCreateMouseEvent(NULL, eType, CGPointMake(pstEv->dX, pstEv->dY), eBtn);
  if (NULL == pEv)
  {
    return;
  }
  if ((0 != pstEv->i32DeltaX) || (0 != pstEv->i32DeltaY))
  {
    CGEventSetIntegerValueField(pEv, kCGMouseEventDeltaX, (int64_t)pstEv->i32DeltaX);
    CGEventSetIntegerValueField(pEv, kCGMouseEventDeltaY, (int64_t)pstEv->i32DeltaY);
  }
  CGEventSetFlags(pEv, (CGEventFlags)pstEv->u64Flags);
  CGEventPost(kCGHIDEventTap, pEv);
  CFRelease(pEv);
}

void af_plat_inject_scroll(dm_in int32 i32Dx, dm_in int32 i32Dy, dm_in uint64 u64Flags)
{
  CGEventRef pEv = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitPixel, 2u, i32Dy, i32Dx);

  if (NULL == pEv)
  {
    return;
  }
  CGEventSetIntegerValueField(pEv, kCGScrollWheelEventIsContinuous, 1);
  CGEventSetFlags(pEv, (CGEventFlags)u64Flags);
  CGEventPost(kCGHIDEventTap, pEv);
  CFRelease(pEv);
}

/* ============================ 键盘、修饰键与 Unicode 注入 ============================ */

static uint64 af__hid_mod_mask(dm_in uint16 u16Hid)
{
  if ((AF_HID_KEY_LCTRL == u16Hid) || (AF_HID_KEY_RCTRL == u16Hid))
  {
    return (uint64)kCGEventFlagMaskControl;
  }
  if ((AF_HID_KEY_LSHIFT == u16Hid) || (AF_HID_KEY_RSHIFT == u16Hid))
  {
    return (uint64)kCGEventFlagMaskShift;
  }
  if ((AF_HID_KEY_LALT == u16Hid) || (AF_HID_KEY_RALT == u16Hid))
  {
    return (uint64)kCGEventFlagMaskAlternate;
  }
  if ((AF_HID_KEY_LGUI == u16Hid) || (AF_HID_KEY_RGUI == u16Hid))
  {
    return (uint64)kCGEventFlagMaskCommand;
  }
  return 0u;
}

static bool_t af__toggle_capslock_hid(void)
{
  io_service_t uSvc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                  IOServiceMatching(kIOHIDSystemClass));
  io_connect_t uConn = 0;
  bool bHidCur = false;
  bool bCgCur = (0 != (CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState) &
                       kCGEventFlagMaskAlphaShift));
  bool bCur = bCgCur;
  bool bNext = !bCur;

  if (MACH_PORT_NULL != uSvc)
  {
    if (KERN_SUCCESS == IOServiceOpen(uSvc, mach_task_self(), kIOHIDParamConnectType, &uConn))
    {
      if (KERN_SUCCESS == IOHIDGetModifierLockState(uConn, kIOHIDCapsLockState, &bHidCur))
      {
        bCur = bHidCur || bCgCur;
        bNext = !bCur;
      }
      (void)IOHIDSetModifierLockState(uConn, kIOHIDCapsLockState, bNext);
      IOServiceClose(uConn);
    }
    IOObjectRelease(uSvc);
  }
  return bNext ? DM_TRUE : DM_FALSE;
}

void af_plat_inject_key(dm_in uint16 u16HidUsage, dm_in bool_t bDown, dm_io uint64 *pu64Flags)
{
  int iVk = af_plat_hid_to_vk(u16HidUsage);
  uint64 u64Mask = af__hid_mod_mask(u16HidUsage);
  uint64 u64Cur = (NULL != pu64Flags) ? *pu64Flags : 0u;
  CGEventRef pEv = NULL;

  if (iVk < 0)
  {
    return;
  }
  if (0x39u == u16HidUsage)
  {
    if (!bDown)
    {
      return;
    }
    bool_t bNextCaps = af__toggle_capslock_hid();
    u64Cur = bNextCaps ? (u64Cur | (uint64)kCGEventFlagMaskAlphaShift)
                       : (u64Cur & (~((uint64)kCGEventFlagMaskAlphaShift)));
    if (NULL != pu64Flags)
    {
      *pu64Flags = u64Cur;
    }
    pEv = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)kVK_CapsLock, bNextCaps ? true : false);
    if (NULL != pEv)
    {
      CGEventSetType(pEv, kCGEventFlagsChanged);
      CGEventSetFlags(pEv, (CGEventFlags)u64Cur);
      CGEventPost(kCGHIDEventTap, pEv);
      CFRelease(pEv);
    }
    return;
  }
  if (0u != u64Mask)
  {
    u64Cur = bDown ? (u64Cur | u64Mask) : (u64Cur & (~u64Mask));
    if (NULL != pu64Flags)
    {
      *pu64Flags = u64Cur;
    }
    pEv = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)iVk, bDown ? true : false);
    if (NULL == pEv)
    {
      return;
    }
    CGEventSetType(pEv, kCGEventFlagsChanged);
    CGEventSetFlags(pEv, (CGEventFlags)u64Cur);
    CGEventPost(kCGHIDEventTap, pEv);
    CFRelease(pEv);
    return;
  }
  if (0 != (CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState) &
            kCGEventFlagMaskAlphaShift))
  {
    u64Cur |= (uint64)kCGEventFlagMaskAlphaShift;
  }
  pEv = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)iVk, bDown ? true : false);
  if (NULL == pEv)
  {
    return;
  }
  CGEventSetFlags(pEv, (CGEventFlags)u64Cur);
  CGEventPost(kCGHIDEventTap, pEv);
  CFRelease(pEv);
}

void af_plat_inject_flags(dm_in uint64 u64Flags)
{
  CGEventRef pEv = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)kVK_Command, false);

  if (NULL == pEv)
  {
    return;
  }
  CGEventSetType(pEv, kCGEventFlagsChanged);
  CGEventSetFlags(pEv, (CGEventFlags)u64Flags);
  CGEventPost(kCGHIDEventTap, pEv);
  CFRelease(pEv);
}

void af_plat_inject_unicode(dm_in const uint16 *pu16Units, dm_in size_t uCount)
{
  CGEventRef pDown = NULL;
  CGEventRef pUp = NULL;

  if ((NULL == pu16Units) || (0u == uCount) || (uCount > 20u))
  {
    return;
  }
  pDown = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)0, true);
  if (NULL != pDown)
  {
    CGEventSetFlags(pDown, (CGEventFlags)0);
    CGEventKeyboardSetUnicodeString(pDown, (UniCharCount)uCount, (const UniChar *)pu16Units);
    CGEventPost(kCGHIDEventTap, pDown);
    CFRelease(pDown);
  }
  pUp = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)0, false);
  if (NULL != pUp)
  {
    CGEventSetFlags(pUp, (CGEventFlags)0);
    CGEventKeyboardSetUnicodeString(pUp, (UniCharCount)uCount, (const UniChar *)pu16Units);
    CGEventPost(kCGHIDEventTap, pUp);
    CFRelease(pUp);
  }
}

/* ============================ Apple Pencil TabletPoint 注入 ============================ */

static void af__post_tablet_prox(dm_in bool_t bEnter, dm_in uint64 u64Flags)
{
  CGEventRef pProx = CGEventCreate(NULL);

  if (NULL == pProx)
  {
    return;
  }
  CGEventSetType(pProx, kCGEventTabletProximity);
  CGEventSetIntegerValueField(pProx, kCGTabletProximityEventEnterProximity, bEnter ? 1 : 0);
  CGEventSetIntegerValueField(pProx, kCGTabletProximityEventPointerType, 1);
  CGEventSetFlags(pProx, (CGEventFlags)u64Flags);
  CGEventPost(kCGHIDEventTap, pProx);
  CFRelease(pProx);
}

void af_plat_inject_pencil(dm_in const af_inject_pencil_st *pstEv)
{
  CGEventType eType = kCGEventMouseMoved;
  CGEventRef pEv = NULL;

  if (NULL == pstEv)
  {
    return;
  }
  if ((AF_PENCIL_HOVER == pstEv->u8Phase) && pstEv->bExitProx)
  {
    af__post_tablet_prox(DM_FALSE, pstEv->u64Flags);
    return;
  }
  if (pstEv->bEnterProx)
  {
    af__post_tablet_prox(DM_TRUE, pstEv->u64Flags);
  }
  if (AF_PENCIL_DOWN == pstEv->u8Phase)
  {
    eType = kCGEventLeftMouseDown;
  }
  else if (AF_PENCIL_MOVE == pstEv->u8Phase)
  {
    eType = kCGEventLeftMouseDragged;
  }
  else if ((AF_PENCIL_UP == pstEv->u8Phase) || (AF_PENCIL_CANCEL == pstEv->u8Phase))
  {
    eType = kCGEventLeftMouseUp;
  }

  pEv = CGEventCreateMouseEvent(NULL, eType, CGPointMake(pstEv->dX, pstEv->dY), kCGMouseButtonLeft);
  if (NULL != pEv)
  {
    CGEventSetIntegerValueField(pEv, kCGMouseEventSubtype, kCGEventMouseSubtypeTabletPoint);
    CGEventSetDoubleValueField(pEv, kCGTabletEventPointPressure, (double)pstEv->fPressure);
    CGEventSetDoubleValueField(pEv, kCGTabletEventTiltX, (double)pstEv->fTiltX);
    CGEventSetDoubleValueField(pEv, kCGTabletEventTiltY, (double)pstEv->fTiltY);
    CGEventSetFlags(pEv, (CGEventFlags)pstEv->u64Flags);
    CGEventPost(kCGHIDEventTap, pEv);
    CFRelease(pEv);
  }
  if (pstEv->bExitProx)
  {
    af__post_tablet_prox(DM_FALSE, pstEv->u64Flags);
  }
}
