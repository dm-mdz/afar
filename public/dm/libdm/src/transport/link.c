/**
 ******************************************************************************
 * @file    link.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   网络传输与 RPC 组件生命周期装配（libdm_net.a）：
 *          dm_link_init / dm_link_fini / dm_net_hooks。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "dm"
#include <dm.h>
#include <dm/link.h>
#include <dm/module.h>
#include <dm/rpc.h>
#include <string.h>
#include "../dm_internal.h"

_Static_assert(sizeof(dm_reply_st) <= sizeof(((dm_stateless_job_st *)0)->au64Reply),
               "dm_reply_st exceeds dm_stateless_job_st.au64Reply capacity");

static bool_t g_bNetUp = DM_FALSE;
static bool_t g_bNetTableOwned = DM_FALSE;

/* ============================ 生命周期装配 ============================ */

int dm_link_init(void)
{
  int iErrCode = DM_SUCCESS;

  if (g_bNetUp)
  {
    return DM_SUCCESS;
  }
  if (!dm__link_table_ready())
  {
    iErrCode = dm__link_table_init();
    if (DM_SUCCESS != iErrCode)
    {
      return iErrCode;
    }
    g_bNetTableOwned = DM_TRUE;
  }
  iErrCode = dm__rpc_builtin_init();
  if (DM_SUCCESS != iErrCode)
  {
    if (g_bNetTableOwned)
    {
      dm__link_table_destroy();
      g_bNetTableOwned = DM_FALSE;
    }
    return iErrCode;
  }
  dm__stat_set_name_cb(dm__iface_name);
  iErrCode = dm__proto_init();
  if (DM_SUCCESS != iErrCode)
  {
    dm__stat_set_name_cb(NULL);
    dm__rpc_destroy();
    if (g_bNetTableOwned)
    {
      dm__link_table_destroy();
      g_bNetTableOwned = DM_FALSE;
    }
    return iErrCode;
  }
  g_bNetUp = DM_TRUE;
  return DM_SUCCESS;
}

void dm_link_fini(void)
{
  if (g_bNetUp)
  {
    dm__proto_fini();
    g_bNetUp = DM_FALSE;
  }
  if (g_bNetTableOwned || (DM__PHASE_NONE == dm__phase()))
  {
    dm__http_routes_clear();
    dm__call_destroy();
    dm__push_destroy();
    dm__rpc_destroy();
    dm__stat_set_name_cb(NULL);
  }
  if (g_bNetTableOwned)
  {
    dm__link_table_destroy();
    g_bNetTableOwned = DM_FALSE;
  }
  return;
}

/* ============================ 扩展钩子表 ============================ */

static int dm__net_hook_before_init(dm_in const dm_cfg_st *pstCfg)
{
  if (NULL != pstCfg)
  {
    dm__link_config(pstCfg);
  }
  return dm_link_init();
}

static void dm__net_hook_on_drain(void)
{
  dm__link_stop_accept();
  return;
}

static void dm__net_hook_on_teardown(void)
{
  dm_link_fini();
  return;
}

static void dm__net_hook_on_end(void)
{
  dm_link_fini();
  dm__http_routes_clear();
  dm__call_destroy();
  dm__push_destroy();
  dm__rpc_destroy();
  dm__stat_set_name_cb(NULL);
  return;
}

static void dm__net_hook_get_ports(dm_out int *piLinkPort, dm_out int *piHttpPort)
{
  if (NULL != piLinkPort)
  {
    *piLinkPort = dm__link_port();
  }
  if (NULL != piHttpPort)
  {
    *piHttpPort = dm__http_port();
  }
  return;
}

static int dm__net_hook_reply_take(dm_in const void *pvMsg, dm_out void *pvReply)
{
  dm_reply_st stReply = {0};
  int iErr = DM_SUCCESS;

  if (NULL == pvReply)
  {
    return DM_INVALID;
  }
  iErr = dm_reply_take((const dm_link_msg_st *)pvMsg, &stReply);
  if (DM_SUCCESS == iErr)
  {
    memcpy(pvReply, &stReply, sizeof(stReply));
  }
  return iErr;
}

static int dm__net_hook_reply_send(dm_in const void *pvReply, dm_in int iStatus,
                                   dm_in const void *pvBody, dm_in uint32 u32Len)
{
  dm_reply_st stReply = {0};

  if (NULL == pvReply)
  {
    return DM_INVALID;
  }
  memcpy(&stReply, pvReply, sizeof(stReply));
  return dm_reply_send(&stReply, iStatus, pvBody, u32Len);
}

static const dm_ext_hooks_st g_stNetExtHooks = {
    dm__net_hook_before_init,
    NULL,
    dm__net_hook_on_drain,
    dm__net_hook_on_teardown,
    dm__net_hook_on_end,
    NULL,
    NULL,
    dm__net_hook_get_ports,
    NULL,
    dm__net_hook_reply_take,
    dm__net_hook_reply_send,
};

const dm_ext_hooks_st *dm_net_hooks(void)
{
  return &g_stNetExtHooks;
}

#ifdef __cplusplus
}
#endif
