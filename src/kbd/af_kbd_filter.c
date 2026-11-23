/**
 ******************************************************************************
 * @file    af_kbd_filter.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   蓝牙外设大类过滤与苹果非键盘设备名过滤（DD-04 §3.3）。
 ******************************************************************************
 */

#include "af_kbd.h"

#include <ctype.h>
#include <string.h>

#define AF_BT_MAJOR_PERIPHERAL 0x05u

static bool_t af__contains_ci(in const char *pcHaystack, in const char *pcNeedle)
{
  size_t uHlen = 0u;
  size_t uNlen = 0u;
  size_t i = 0u;
  size_t j = 0u;

  if ((NULL == pcHaystack) || (NULL == pcNeedle))
  {
    return DM_FALSE;
  }
  uHlen = strlen(pcHaystack);
  uNlen = strlen(pcNeedle);
  if ((0u == uNlen) || (uNlen > uHlen))
  {
    return DM_FALSE;
  }
  for (i = 0u; i <= (uHlen - uNlen); i++)
  {
    for (j = 0u; j < uNlen; j++)
    {
      unsigned char ucH = (unsigned char)pcHaystack[i + j];
      unsigned char ucN = (unsigned char)pcNeedle[j];
      if (tolower(ucH) != tolower(ucN))
      {
        break;
      }
    }
    if (j == uNlen)
    {
      return DM_TRUE;
    }
  }
  return DM_FALSE;
}

bool_t af_kbd_is_apple_non_kb(in const char *pcName)
{
  static const char *const apcAllow[] = {
      "keyboard", "keys", "mouse", "trackpad", "keypad", "numpad",
      "\xe9\x94\xae\xe7\x9b\x98",             /* 键盘 */
      "\xe9\xbc\xa0\xe6\xa0\x87",             /* 鼠标 */
      "\xe8\xa7\xa6\xe6\x8e\xa7\xe6\x9d\xbf"  /* 触控板 */
  };
  static const char *const apcAppleNonKb[] = {
      "ipad", "iphone", "ipod", "airpods", "airtag", "apple watch",
      "apple tv", "apple pencil", "homepod", "macbook", "imac",
      "mac mini", "mac studio", "mac pro", "beats", "vision pro"
  };
  size_t i = 0u;

  if ((NULL == pcName) || ('\0' == pcName[0]))
  {
    return DM_FALSE;
  }
  for (i = 0u; i < (sizeof(apcAllow) / sizeof(apcAllow[0])); i++)
  {
    if (af__contains_ci(pcName, apcAllow[i]))
    {
      return DM_FALSE;
    }
  }
  for (i = 0u; i < (sizeof(apcAppleNonKb) / sizeof(apcAppleNonKb[0])); i++)
  {
    if (af__contains_ci(pcName, apcAppleNonKb[i]))
    {
      return DM_TRUE;
    }
  }
  return DM_FALSE;
}

bool_t af_kbd_device_allowed(in const af_bt_device_st *pstDev)
{
  if (NULL == pstDev)
  {
    return DM_FALSE;
  }
  if ((AF_BT_MAJOR_PERIPHERAL != pstDev->u32Major) && (0u != pstDev->u32Major))
  {
    return DM_FALSE;
  }
  if ((0u == pstDev->u32Major) && af_kbd_is_apple_non_kb(pstDev->acName))
  {
    return DM_FALSE;
  }
  return DM_TRUE;
}
