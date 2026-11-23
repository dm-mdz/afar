/**
 ******************************************************************************
 * @file    af_setup_scripts.c
 * @author  Afar_C
 * @version 1.0.0
 * @brief   af_setup 纯 C 工具：sh_quote、AppleScript 转义、一键/SSH 脚本、parse_fail 与设置 URL。
 ******************************************************************************
 */

#define _DARWIN_C_SOURCE
#include "af_setup.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

uint32 af_setup_calc_todo(in const af_setup_view_st *pstV)
{
  uint32 u32Todo = 0u;

  if (NULL == pstV)
  {
    return 0u;
  }
  if (!pstV->bAgent) { u32Todo++; }
  if (!pstV->bFw) { u32Todo++; }
  if (!pstV->bPower) { u32Todo++; }
  if (0 == strcmp(pstV->acPlan, "fv"))
  {
    if (!pstV->bRemoteLogin) { u32Todo++; }
  }
  else
  {
    if (pstV->bFileVault) { u32Todo++; }
    if (!pstV->bAutoLogin) { u32Todo++; }
  }
  return u32Todo;
}

int af_setup_sh_quote(in const char *pcStr, out char *pcBuf, in size_t uCap)
{
  size_t uPos = 0u;
  const char *pcP = NULL;

  if ((NULL == pcStr) || (NULL == pcBuf) || (uCap < 3u))
  {
    return DM_INVALID;
  }
  pcBuf[uPos++] = '\'';
  for (pcP = pcStr; '\0' != *pcP; pcP++)
  {
    if ('\'' == *pcP)
    {
      if ((uPos + 4u) >= uCap)
      {
        return DM_TOO_BIG;
      }
      pcBuf[uPos++] = '\'';
      pcBuf[uPos++] = '\\';
      pcBuf[uPos++] = '\'';
      pcBuf[uPos++] = '\'';
    }
    else
    {
      if ((uPos + 1u) >= uCap)
      {
        return DM_TOO_BIG;
      }
      pcBuf[uPos++] = *pcP;
    }
  }
  if ((uPos + 1u) >= uCap)
  {
    return DM_TOO_BIG;
  }
  pcBuf[uPos++] = '\'';
  pcBuf[uPos] = '\0';
  return DM_SUCCESS;
}

int af_setup_applescript_str(in const char *pcStr, out char *pcBuf, in size_t uCap)
{
  size_t uPos = 0u;
  const char *pcP = NULL;

  if ((NULL == pcStr) || (NULL == pcBuf) || (uCap < 3u))
  {
    return DM_INVALID;
  }
  pcBuf[uPos++] = '"';
  for (pcP = pcStr; '\0' != *pcP; pcP++)
  {
    char cEsc = '\0';
    if ('\\' == *pcP) { cEsc = '\\'; }
    else if ('"' == *pcP) { cEsc = '"'; }
    else if ('\n' == *pcP) { cEsc = 'n'; }
    else if ('\r' == *pcP) { cEsc = 'r'; }
    else if ('\t' == *pcP) { cEsc = 't'; }
    if ('\0' != cEsc)
    {
      if ((uPos + 2u) >= uCap) { return DM_TOO_BIG; }
      pcBuf[uPos++] = '\\';
      pcBuf[uPos++] = cEsc;
    }
    else
    {
      if ((uPos + 1u) >= uCap) { return DM_TOO_BIG; }
      pcBuf[uPos++] = *pcP;
    }
  }
  if ((uPos + 1u) >= uCap)
  {
    return DM_TOO_BIG;
  }
  pcBuf[uPos++] = '"';
  pcBuf[uPos] = '\0';
  return DM_SUCCESS;
}

int af_setup_build_one_script(in const char *pcExePath, out char *pcBuf, in size_t uCap)
{
  char acQuoted[1536];
  int iN = 0;

  if ((NULL == pcExePath) || ('\0' == pcExePath[0]) || (NULL == pcBuf) || (0u == uCap))
  {
    return DM_INVALID;
  }
  if (DM_SUCCESS != af_setup_sh_quote(pcExePath, acQuoted, sizeof(acQuoted)))
  {
    return DM_TOO_BIG;
  }
  iN = snprintf(
      pcBuf, uCap,
      "FAIL=\"\"\n"
      "/usr/libexec/ApplicationFirewall/socketfilterfw --add %s --unblockapp %s >/dev/null 2>&1"
      " || FAIL=\"${FAIL:+$FAIL、}防火墙\"\n"
      "/usr/bin/pmset -a autorestart 1 >/dev/null 2>&1"
      " || FAIL=\"${FAIL:+$FAIL、}断电后自动开机\"\n"
      "if [ -f /Library/LaunchDaemons/com.afar.restart.plist ]; then\n"
      "  /bin/launchctl bootout system/com.afar.restart >/dev/null 2>&1\n"
      "  /bin/rm -f /Library/LaunchDaemons/com.afar.restart.plist\n"
      "fi\n"
      "echo \"AFAR-FAIL: ${FAIL}\"\n",
      acQuoted, acQuoted);
  if ((iN <= 0) || ((size_t)iN >= uCap))
  {
    return DM_TOO_BIG;
  }
  return DM_SUCCESS;
}

int af_setup_build_ssh_script(out char *pcBuf, in size_t uCap)
{
  static const char acScript[] =
      "/bin/launchctl enable system/com.openssh.sshd\n"
      "/bin/launchctl bootstrap system /System/Library/LaunchDaemons/ssh.plist >/dev/null 2>&1\n"
      "/bin/sleep 2\n"
      "if /usr/bin/nc -z -G 2 127.0.0.1 22 >/dev/null 2>&1; then "
      "echo \"AFAR-FAIL:\"; else "
      "echo \"AFAR-FAIL: 远程登录（请到 系统设置 › 通用 › 共享 里打开）\"; fi\n";
  size_t uLen = sizeof(acScript) - 1u;

  if ((NULL == pcBuf) || (0u == uCap))
  {
    return DM_INVALID;
  }
  if (uLen >= uCap)
  {
    return DM_TOO_BIG;
  }
  memcpy(pcBuf, acScript, uLen + 1u);
  return DM_SUCCESS;
}

bool_t af_setup_parse_fail(in const char *pcOut, out char *pcErrOut, in size_t uErrCap)
{
  const char *pcTag = NULL;
  const char *pcStart = NULL;
  const char *pcEnd = NULL;
  size_t uLen = 0u;

  if ((NULL != pcErrOut) && (uErrCap > 0u))
  {
    pcErrOut[0] = '\0';
  }
  if (NULL == pcOut)
  {
    return DM_FALSE;
  }
  pcTag = strstr(pcOut, "AFAR-FAIL:");
  if (NULL == pcTag)
  {
    return DM_FALSE;
  }
  pcStart = pcTag + 10;
  pcEnd = pcStart;
  while (('\0' != *pcEnd) && ('\r' != *pcEnd) && ('\n' != *pcEnd))
  {
    pcEnd++;
  }
  while ((pcStart < pcEnd) && ((' ' == *pcStart) || ('\t' == *pcStart)))
  {
    pcStart++;
  }
  while ((pcEnd > pcStart) && ((' ' == pcEnd[-1]) || ('\t' == pcEnd[-1])))
  {
    pcEnd--;
  }
  uLen = (size_t)(pcEnd - pcStart);
  if (0u == uLen)
  {
    return DM_FALSE;
  }
  if ((NULL != pcErrOut) && (uErrCap > 1u))
  {
    if (uLen >= uErrCap)
    {
      uLen = uErrCap - 1u;
    }
    memcpy(pcErrOut, pcStart, uLen);
    pcErrOut[uLen] = '\0';
  }
  return DM_TRUE;
}

const char *af_setup_settings_url(in const char *pcWhat)
{
  static const struct
  {
    const char *pcKey;
    const char *pcUrl;
  } astMap[] = {
      {"bluetooth", "x-apple.systempreferences:com.apple.BluetoothSettings"},
      {"wifi", "x-apple.systempreferences:com.apple.wifi-settings-extension"},
      {"handoff", "x-apple.systempreferences:com.apple.AirDrop-Handoff-Settings.extension"},
      {"filevault",
       "x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?Privacy_FileVault"},
      {"users", "x-apple.systempreferences:com.apple.Users-Groups-Settings.extension"},
      {"sharing", "x-apple.systempreferences:com.apple.Sharing-Settings.extension"},
      {"energy", "x-apple.systempreferences:com.apple.preference.energysaver"},
      {"firewall", "x-apple.systempreferences:com.apple.Network-Settings.extension?Firewall"}
  };
  size_t i = 0u;

  if (NULL == pcWhat)
  {
    return NULL;
  }
  for (i = 0u; i < (sizeof(astMap) / sizeof(astMap[0])); i++)
  {
    if (0 == strcmp(pcWhat, astMap[i].pcKey))
    {
      return astMap[i].pcUrl;
    }
  }
  return NULL;
}

void af_setup_format_date(in uint64 u64EpochSec, in int iCurYear, out char *pcBuf, in size_t uCap)
{
  time_t tVal = (time_t)u64EpochSec;
  struct tm stTm;

  if ((NULL == pcBuf) || (0u == uCap))
  {
    return;
  }
  if ((0u == u64EpochSec) || (NULL == localtime_r(&tVal, &stTm)))
  {
    pcBuf[0] = '\0';
    return;
  }
  if ((stTm.tm_year + 1900) == iCurYear)
  {
    snprintf(pcBuf, uCap, "%d 月 %d 日", stTm.tm_mon + 1, stTm.tm_mday);
  }
  else
  {
    snprintf(pcBuf, uCap, "%d 年 %d 月 %d 日", stTm.tm_year + 1900, stTm.tm_mon + 1, stTm.tm_mday);
  }
}
