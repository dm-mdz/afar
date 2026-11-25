/**
 ******************************************************************************
 * @file    dmc_serial_linux_baud.c
 * @brief   Linux 非标任意波特率配置（DD-20 §2.3.2，PLUG-04）：
 *          独立翻译单元隔离 <asm/termbits.h> 与 <termios.h> 的 struct termios 重定义冲突，
 *          通过 TCGETS2 / TCSETS2 + BOTHER 设置任意非标波特率（如 250000 / 500000）。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include <dm/type.h>

#ifdef __linux__
#include <asm/termbits.h>
#include <sys/ioctl.h>

int dmc__serial_linux_set_baud(dm_in int iFd, dm_in uint32 u32Baud)
{
  struct termios2 stTio2;

  if ((iFd < 0) || (0u == u32Baud))
  {
    return -1;
  }
  if (0 != ioctl(iFd, TCGETS2, &stTio2))
  {
    return -1;
  }
  stTio2.c_cflag &= (tcflag_t)(~CBAUD);
#ifdef CIBAUD
  stTio2.c_cflag &= (tcflag_t)(~CIBAUD);
#endif
  stTio2.c_cflag |= (tcflag_t)BOTHER;
  stTio2.c_ispeed = (speed_t)u32Baud;
  stTio2.c_ospeed = (speed_t)u32Baud;
  if (0 != ioctl(iFd, TCSETS2, &stTio2))
  {
    return -1;
  }
  return 0;
}
#else
int dmc__serial_linux_set_baud(dm_in int iFd, dm_in uint32 u32Baud)
{
  (void)iFd;
  (void)u32Baud;
  return -1;
}
#endif

#ifdef __cplusplus
}
#endif
