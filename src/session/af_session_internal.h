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
#include "../kbd/af_kbd.h"
#include "../sidecar/af_sidecar.h"

#include <dm.h>
#include <dm/event.h>
#include <dm/https.h>
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
#define AF_SES_TAG_JOB        DM_MOD_MEMTAG(AF_MOD_SESSION, 0u)
#define AF_SES_TAG_MENU       DM_MOD_MEMTAG(AF_MOD_SESSION, 1u)
#define AF_SES_TAG_SAVE       DM_MOD_MEMTAG(AF_MOD_SESSION, 2u)
#define AF_SES_JSON_MAX_DEPTH 64u
#define AF_SES_RESTART_MS     60000u

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
void af_ses_get_mac_name(out char *pcOut, in size_t uCap);
void af_ses_push_menu(void);
void af_ses_front_begin(in const char *pcName);
void af_ses_front_stop(void);
void af_ses_drain_check(void);
void af_ses_disconnect_core(in af_store_saved_cb pfOnSaved, io void *pvCtx);
int af_ses_start_restart_job(in bool_t bReopen);
int af_ses_http_register(void);
void af_ses_save_inflight_inc(void);
void af_ses_save_inflight_dec(void);

#ifdef __cplusplus
}
#endif

#endif /* AF_SESSION_INTERNAL_H */
