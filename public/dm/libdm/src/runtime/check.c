/**
 ******************************************************************************
 * @file    check.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   主线程/非主线程与 init 阶段校验（从 module.c 拆出，DD-01 §1.1）。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "dm"
#include <dm.h>
#include <dm/check.h>
#include <dm/log.h>
#include "../dm_internal.h"

static dm_thread_t g_stMainThread;
static bool_t g_bMainBound = DM_FALSE;
static bool_t g_bInInit = DM_FALSE;

void dm_check_bind_main(void)
{
  g_stMainThread = dm_thread_self();
  g_bMainBound = DM_TRUE;
  return;
}

void dm__check_unbind_main(void)
{
  g_bMainBound = DM_FALSE;
  g_bInInit = DM_FALSE;
  return;
}

void dm__check_set_in_init(in bool_t bInInit)
{
  g_bInInit = bInInit;
  return;
}

bool_t dm_in_loop(void)
{
  dm_thread_t stSelf = {0};

  if (!g_bMainBound)
  {
    return DM_TRUE; /* 不经 dm_main_ex、也没自己绑过：没有主线程可比，不拦 */
  }
  stSelf = dm_thread_self();
  return (0 != dm_thread_equal(&g_stMainThread, &stSelf)) ? DM_TRUE : DM_FALSE;
}

bool_t dm_in_init(void)
{
  return g_bInInit;
}

void dm__wrong_thread(in const char *pcFunc)
{
  DM_LOG_ERROR("%s called off the loop thread", pcFunc);
  return;
}

#ifdef __cplusplus
}
#endif
