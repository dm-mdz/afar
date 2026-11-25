/**
 ******************************************************************************
 * @file    af_session_internal.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   af_session 模块内部共享上下文与跨文件辅助声明（仅 src/session/ 内部包含）。
 ******************************************************************************
 */
#ifndef AF_SESSION_INTERNAL_H
#define AF_SESSION_INTERNAL_H

#include "af_session.h"
#include "../af_events.h"
#include "../af_modules.h"
#include "../af_store.h"
#include "../auth/af_auth.h"
#include "../display/af_display.h"
#include "../env/af_env.h"
#include "../input/af_input.h"
#include "../kbd/af_kbd.h"
#include "../sidecar/af_sidecar.h"
#include "../stream/af_stream.h"

#include <dm.h>
#include <dm/event.h>
#include "base/af_https_srv.h"
#include <dm/json.h>
#include <dm/link.h>
#include <dm/log.h>
#include <dm/pool.h>
#include <dm/rpc.h>
#include <dm/timer.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define AF_SES_LINK_FRONT_TMR DM_MOD_LINKID(AF_MOD_SESSION, 0x04u)
#define AF_SES_LINK_PRODRAW_TMR DM_MOD_LINKID(AF_MOD_SESSION, 0x05u)
#define AF_SES_LINK_SC_WAIT_TMR DM_MOD_LINKID(AF_MOD_SESSION, 0x06u)
#define AF_SES_TAG_JOB DM_MOD_MEMTAG(AF_MOD_SESSION, 0u)
#define AF_SES_TAG_MENU DM_MOD_MEMTAG(AF_MOD_SESSION, 1u)
#define AF_SES_TAG_SAVE DM_MOD_MEMTAG(AF_MOD_SESSION, 2u)
#define AF_SES_JSON_MAX_DEPTH 64u
#define AF_SES_RESTART_MS 60000u

typedef struct af_ses_ctx
{
  af_front_st stFront;
  uint32 u32FrontTimer;
  uint32 u32TodoCount;
  bool_t bReady;
  bool_t bRestartRunning;
  char acRestartError[256];
  uint32 u32InflightSave;
  bool_t bDraining;
  int iDrainHold;
} af_ses_ctx_st;

extern af_ses_ctx_st g_stAfSes;

const af_session_ops_st *af_ses_ops(void);
uint64 af_ses_now_ms(void);
void af_ses_get_mac_name(dm_out char *pcOut, dm_in size_t uCap);
void af_ses_push_menu(void);
void af_ses_front_begin(dm_in const char *pcName);
void af_ses_front_stop(void);
void af_ses_drain_check(void);
void af_ses_disconnect_core(dm_in af_store_saved_cb pfOnSaved, dm_io void *pvCtx);
int af_ses_start_restart_job(dm_in bool_t bReopen);
int af_ses_http_register(void);
int af_ses_read_json_obj(dm_in const af_https_msg_st *pstMsg, dm_in bool_t bAllowEmpty,
                         dm_out yyjson_doc **ppstDoc);
int af_ses_resolve_connect_target(dm_in const char *pcReqName, dm_in const af_device_st *pstDev,
                                  dm_in const af_sc_snap_st *pstSc, dm_out char *pcOut,
                                  dm_in size_t uCap);
void af_ses_save_inflight_inc(void);
void af_ses_save_inflight_dec(void);

int af_ses_direct_register(void);
void af_ses_direct_init(void);
void af_ses_direct_drain(void);
void af_ses_direct_fini(void);
bool_t af_ses_direct_is_active(void);
bool_t af_ses_direct_is_switching_prodraw(void);
void af_ses_direct_get_active(dm_out af_ev_stream_st *pstOut, dm_out char *pcKeyOut,
                              dm_in size_t uKeyCap);
void af_ses_direct_stop_all(dm_in bool_t bHoldVd);
void af_ses_direct_on_revoked(dm_in const char *pcDevId);
void af_ses_direct_on_stream_ev(dm_in const af_ev_stream_st *pstEv);
void af_ses_direct_on_sc_or_disp_ev(void);

#ifdef __cplusplus
}
#endif

#endif /* AF_SESSION_INTERNAL_H */
