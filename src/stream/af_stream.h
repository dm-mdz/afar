/**
 ******************************************************************************
 * @file    af_stream.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   服务层 af_stream（ModID 24）公共契约：日常模式视频会话、UDP AEAD + 交错 XOR FEC 推流与编码编排。
 ******************************************************************************
 */
#ifndef AF_STREAM_H
#define AF_STREAM_H

#include <dm/module.h>
#include <dm/type.h>
#include "../af_events.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct af_stream_start_req
{
  char acDevId[37];
  char acDevName[65];
  uint32 u32DispId;
  uint16 u16EncW;
  uint16 u16EncH;
  uint16 u16Fps;
  bool_t bChroma444;
  uint8 au8SidBe[8];
  uint8 au8VidAuthKey[32]; /* k_vid_auth */
} af_stream_start_req_st;

typedef struct af_stream_start_res
{
  uint16 u16VideoPort;
  bool_t bEffChroma444;
} af_stream_start_res_st;

/* 编码帧递交回调（由采集/编码层向 T_video 2 槽邮箱投递，载荷归接收方 dm_free_tag 释放） */
typedef struct af_stream_enc_frame
{
  uint64 u64Gen;
  uint32 u32Flags;
  uint64 u64CaptureUs;
  uint8 *pu8Data;
  uint32 u32Len;
} af_stream_enc_frame_st;

typedef void (*af_stream_frame_cb)(dm_in const af_stream_enc_frame_st *pstFrame, dm_io void *pvUser);
typedef void (*af_stream_stop_cb)(dm_in uint64 u64Gen, dm_in int iReason, dm_io void *pvUser);

typedef struct af_stream_cap_cfg
{
  uint64 u64Gen;
  uint32 u32DispId;
  uint16 u16EncW;
  uint16 u16EncH;
  uint16 u16Fps;
  bool_t bChroma444;
  af_stream_frame_cb pfOnFrame;
  af_stream_stop_cb pfOnStopped;
  void *pvUser;
} af_stream_cap_cfg_st;

/* 测试注入表（NULL 成员回退真实 af_plat_stream_* 实现，供 L1 无屏幕无权限单测） */
typedef struct af_stream_ops
{
  int (*pfCapStart)(dm_in const af_stream_cap_cfg_st *pstCfg, dm_out bool_t *pbEff444);
  void (*pfCapStopAsync)(dm_in uint64 u64Gen, dm_in af_stream_stop_cb pfOnDone, dm_io void *pvUser);
  void (*pfCapStopSync)(void);
  void (*pfCapForceIdr)(dm_in uint64 u64Gen);
  void (*pfCapSwitchDisp)(dm_in uint64 u64Gen, dm_in uint32 u32NewDispId);
  void (*pfCapSetBitrate)(dm_in uint64 u64Gen, dm_in uint32 u32AvgBps);
  bool_t (*pfSupports444)(void);
  uint32 u32AuthTimeoutMs; /* 0 = 默认 10000ms */
} af_stream_ops_st;

extern const dm_module_st g_stAfStreamMod;
const dm_module_st *af_stream_module(void);

void af_stream_set_ops(dm_in const af_stream_ops_st *pstOps);

int af_stream_start(dm_in const af_stream_start_req_st *pstReq,
                    dm_out af_stream_start_res_st *pstRes);
void af_stream_force_stop(void);
void af_stream_get_snapshot(dm_out af_ev_stream_st *pstOut);
bool_t af_stream_is_hevc444_supported(void);

#ifdef __cplusplus
}
#endif

#endif /* AF_STREAM_H */
