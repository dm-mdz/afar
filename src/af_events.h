/**
 ******************************************************************************
 * @file    af_events.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   基础层跨模块事件负载纯数据结构（供服务层与业务层共享，零越层/互调依赖）。
 ******************************************************************************
 */
#ifndef AF_EVENTS_H
#define AF_EVENTS_H

#include <dm/type.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum af_auth_ev_kind
{
  AF_AUTH_EV_PAIR_WIN = 1,
  AF_AUTH_EV_PAIRED = 2,
  AF_AUTH_EV_REVOKED = 3,
  AF_AUTH_EV_BINDING = 4
} af_auth_ev_kind_et;

typedef struct af_ev_auth
{
  af_auth_ev_kind_et eKind;
  char acDevId[37];
  char acDevName[65];
  char acSidecarName[65];
} af_ev_auth_st;

typedef enum af_vd_state
{
  AF_VD_PHYSICAL_ONLY = 0,
  AF_VD_HEADLESS_DESK,
  AF_VD_HEADLESS_IPAD,
  AF_VD_PHYSICAL_BACKUP
} af_vd_state_et;

typedef struct af_ev_display
{
  uint32 u32PhysCount;
  af_vd_state_et eVdState;
  bool_t bHasVd;
} af_ev_display_st;

typedef enum af_sc_state
{
  AF_SC_IDLE = 0,
  AF_SC_CONNECTING,
  AF_SC_SHOWING
} af_sc_state_et;

typedef struct af_ev_sidecar
{
  af_sc_state_et ePrev;
  af_sc_state_et eCur;
  char acShowing[65];
  bool_t bUserDisconnect;
  bool_t bNewAppear;
  char acAppeared[65];
} af_ev_sidecar_st;

typedef struct af_ev_setup_todo
{
  uint32 u32TodoCount;
} af_ev_setup_todo_st;

#ifdef __cplusplus
}
#endif

#endif /* AF_EVENTS_H */
