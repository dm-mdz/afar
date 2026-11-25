/**
 ******************************************************************************
 * @file    af_input_int.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   服务层 af_input（ModID 25）内部共享结构、无锁 SPSC 环与注入线程接口。
 ******************************************************************************
 */
#ifndef AF_INPUT_INT_H
#define AF_INPUT_INT_H

#include <dm.h>
#include <dm/crypto.h>
#include <dm/event.h>
#include <dm/fs.h>
#include <dm/link.h>
#include <dm/log.h>
#include <dm/loop.h>
#include <dm/mem.h>
#include <dm/module.h>
#include <dm/sock.h>
#include <dm/thread.h>
#include <dm/timer.h>

#include "af_direct_proto.h"
#include "af_events.h"
#include "af_modules.h"
#include "input/af_input.h"

#include <mbedtls/chachapoly.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 常量定义（与 DD v2.1 §4.2 / §6.2 对齐） */
#define AF_INPUT_SPSC_CAP 1024u
#define AF_INPUT_SPSC_MASK (AF_INPUT_SPSC_CAP - 1u)
#define AF_INPUT_WD_DEFAULT_MS 1500u
#define AF_INPUT_SEC_DEFAULT_MS 500u
#define AF_INPUT_IDR_THROTTLE_NS 200000000ULL
#define AF_INPUT_UTF16_CHUNK_MAX 20u
#define AF_INPUT_UDP_MIN_LEN (AF_DP_UDP_OUTER_LEN + AF_DP_AEAD_TAG_LEN + AF_DP_IN_HDR_LEN)

/* macOS CGEventFlags 公开 ABI 常量（零 Apple 头依赖，纯 C 位掩码） */
#define AF_MOD_FLAG_ALPHA_SHIFT 0x00010000ULL
#define AF_MOD_FLAG_SHIFT 0x00020000ULL
#define AF_MOD_FLAG_CONTROL 0x00040000ULL
#define AF_MOD_FLAG_ALTERNATE 0x00080000ULL
#define AF_MOD_FLAG_COMMAND 0x00100000ULL

typedef enum af_inj_kind
{
  AF_INJ_REL_MOVE = 1,
  AF_INJ_SCROLL = 2,
  AF_INJ_MOUSE_ABS = 3,
  AF_INJ_MOUSE_BTN = 4,
  AF_INJ_KEY = 5,
  AF_INJ_PENCIL = 6,
  AF_INJ_TEXT = 7,
  AF_INJ_RELEASE_ALL = 8,
  AF_INJ_BOUNDS_UPD = 9
} af_inj_kind_et;

typedef struct af_inj_item
{
  af_inj_kind_et eKind;
  union
  {
    struct
    {
      int32 i32Dx;
      int32 i32Dy;
    } stRel;
    struct
    {
      int32 i32Wx;
      int32 i32Wy;
    } stScroll;
    struct
    {
      float fNx;
      float fNy;
    } stAbs;
    struct
    {
      uint8 u8Btn;
      bool_t bDown;
      bool_t bHasAbs;
      float fNx;
      float fNy;
    } stBtn;
    struct
    {
      uint16 u16HidUsage;
      bool_t bDown;
    } stKey;
    struct
    {
      uint8 u8Phase;
      float fNx;
      float fNy;
      float fPressure;
      float fTiltX;
      float fTiltY;
    } stPencil;
    struct
    {
      uint16 u16Len;
      uint8 au8Utf8[AF_DP_TEXT_MAX_BYTES];
    } stText;
    uint32 u32DispId;
  } u;
} af_inj_item_st;

typedef struct af_inj_ctx
{
  af_inj_item_st *pastSlots;
  _Atomic uint32 u32Head;
  _Atomic uint32 u32Tail;
  _Atomic bool_t bPendingReleaseAll;
  dm_mutex_st *pstWakeMtx;
  dm_cond_st *pstWakeCond;
  bool_t bStop;
  dm_thread_st *pstThread;

  /* 仅由 T_inject 消费者独占读写（零锁竞争） */
  af_input_ops_st stOps;
  af_rect_f_st stVdBounds;
  double dCurX;
  double dCurY;
  uint8 au8HeldKeys[256];
  uint8 u8BtnMask;
  bool_t bPencilDown;
  bool_t bPencilProx;
  uint64 u64ModFlags;
} af_inj_ctx_st;

/* 注入子系统内部接口（供 af_input_mod.c 及单测确定性 SPSC 合并检验使用） */
int af__inj_init(dm_out af_inj_ctx_st *pstInj, dm_in const af_input_ops_st *pstOps,
                 dm_in uint32 u32DispId);
int af__inj_start_thread(dm_io af_inj_ctx_st *pstInj);
bool_t af__inj_push(dm_io af_inj_ctx_st *pstInj, dm_in const af_inj_item_st *pstItem);
void af__inj_wake(dm_io af_inj_ctx_st *pstInj);
void af__inj_drain_batch(dm_io af_inj_ctx_st *pstInj);
void af__inj_stop_and_fini(dm_io af_inj_ctx_st *pstInj);
bool_t af__inj_ev_to_item(dm_in const af_dp_ev_t *pstEv, dm_out af_inj_item_st *pstOut);

/* 内部诊断观测（供 L1 单测校验 T_core 看门狗回调计数与会话清理状态） */
uint32 af__input_wd_ev_count(void);
void af__input_reset_wd_ev_count(void);

#ifdef __cplusplus
}
#endif

#endif /* AF_INPUT_INT_H */
