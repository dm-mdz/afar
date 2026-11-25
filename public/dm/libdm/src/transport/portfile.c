/**
 ******************************************************************************
 * @file    portfile.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   端口文件 dm.port（DD-01）：持久化上次监听端口，下次启动优先复用。
 *          格式是一行「二进制口 HTTP口」。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "link"
#include <dm.h>
#include <dm/log.h>

#include <stdio.h>  /* snprintf */
#include <stdlib.h> /* strtol */

#include "../dm_internal.h"

/* ============================ 端口文件读写 ============================ */

/* dm.port 里记着的上次的端口；读不懂（空的、乱码、不在 1~65535）当作没有，返回 0 */
int dm__port_file_read(dm_in const char *pcDataDir, dm_out int *piHttp)
{
  char acPath[600] = {0};
  char *pcText = NULL;
  size_t uLen = 0;
  long lPort = 0;
  char *pcEnd = NULL;

  *piHttp = 0;
  if (NULL == pcDataDir)
  {
    return 0;
  }
  snprintf(acPath, sizeof(acPath), "%s/dm.port", pcDataDir);
  pcText = dm__file_read_all(acPath, 64u, &uLen);
  if (NULL == pcText)
  {
    return 0;
  }
  lPort = strtol(pcText, &pcEnd, 10);
  if ((pcEnd == pcText) || (lPort < 1) || (lPort > 65535))
  {
    dm_free_tag(pcText);
    return 0; /* 第一个数都读不懂：整个文件当作没有 */
  }
  {
    char *pcEnd2 = NULL;
    long lHttp = strtol(pcEnd, &pcEnd2, 10);
    *piHttp = ((pcEnd2 != pcEnd) && (lHttp >= 1) && (lHttp <= 65535)) ? (int)lHttp : 0;
  }
  dm_free_tag(pcText);
  return (int)lPort;
}

/* 写回 dm.port（先写临时文件再改名） */
void dm__port_file_write(dm_in const char *pcDataDir, dm_in int iPort, dm_in int iHttp)
{
  char acPath[600] = {0};
  char acText[32] = {0};
  int iLen = 0;

  if (NULL == pcDataDir)
  {
    return;
  }
  snprintf(acPath, sizeof(acPath), "%s/dm.port", pcDataDir);
  iLen = snprintf(acText, sizeof(acText), "%d %d\n", iPort, iHttp); /* 二进制口、HTTP 口 */
  if (DM_SUCCESS != dm__file_write_atomic(acPath, acText, (size_t)iLen))
  {
    DM_LOG_WARN("cannot write %s", acPath);
  }
  return;
}

#ifdef __cplusplus
}
#endif
