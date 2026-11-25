/**
 ******************************************************************************
 * @file    frame.h
 * @brief   帧头契约（单一来源，详设 2.5）：核心（link）和客户端库（dmc）共用。
 *          二进制口和 WebSocket 用同一个帧头。只依赖 <dm/type.h>，零依赖的 dmc 也能包含。
 *
 *  网络序大端，16 字节：
 *  0        2      3      4           8           12          16
 *  | linkID | type | flag | reqID u32 | status i32 | len u32 | 负载 len 字节
 *  type：1 请求  2 应答  3 推送
 *  flag：bit0 = 负载是 JSON（dmcmd 打印时用）；bit1 = 流式推送（这时推送前缀里的序号是 0）
 *  reqID：请求和应答对上用；推送为 0。双方各自从 1 往上编，方向不同不冲突
 *  status：只在应答里有意义，DM_SUCCESS 或错误码
 *  len：负载长度；上限由核心定（默认 16MB）
 *
 *  推送的负载前面加 18 字节：序号 u64 + 类 u16 + 键 u64，后面才是值（JSON）。
 ******************************************************************************
 */
#ifndef DM_FRAME_H
#define DM_FRAME_H

#include <dm/type.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_FRAME_HEAD 16u
#define DM_FRAME_T_REQ 1u
#define DM_FRAME_T_REP 2u
#define DM_FRAME_T_PUSH 3u
#define DM_FRAME_F_JSON 0x01u
#define DM_FRAME_F_STREAM 0x02u
#define DM_FRAME_F_SENSITIVE 0x04u /* 敏感载荷：WS/TCP/qbuf/JSON 释放前全链路 dm_secure_zero */
#define DM_FRAME_MAX_DEFAULT (16u * 1024u * 1024u)
#define DM_FRAME_PUSH_HEAD 18u /* 推送负载前缀：序号 8 + 类 2 + 键 8 */

/* DM 自带的接口（详设 2.5.3） */
#define DM_LINKID_HELLO 0x0001u         /* 握手 */
#define DM_LINKID_IFACE_LIST 0x0002u    /* 接口清单 */
#define DM_LINKID_LOG_RECENT 0x0003u    /* 最近的日志 */
#define DM_LINKID_STAT 0x0004u          /* 统计 */
#define DM_LINKID_PING 0x0005u          /* 心跳 */
#define DM_LINKID_LOG_LEVEL 0x0006u     /* 改日志级别（仅 CLI） */
#define DM_LINKID_DIAG_SNAPSHOT 0x0007u /* 诊断快照（仅 CLI/DIAG） */

typedef struct dm_frame_head
{
  uint16 u16LinkID;
  uint8 u8Type;
  uint8 u8Flag;
  uint32 u32ReqID;
  int32 i32Status;
  uint32 u32Len;
} dm_frame_head_st;

/* 大端读写：手写移位，不引 htons/ntohs，保持纯 C11 */
static inline void dm_frame_wr16(dm_out uint8 *pu8, dm_in uint32 u32Val)
{
  pu8[0] = (uint8)((u32Val >> 8) & 0xFFu);
  pu8[1] = (uint8)(u32Val & 0xFFu);
  return;
}

static inline uint32 dm_frame_rd16(dm_in const uint8 *pu8)
{
  return ((uint32)pu8[0] << 8) | (uint32)pu8[1];
}

static inline void dm_frame_wr32(dm_out uint8 *pu8, dm_in uint32 u32Val)
{
  pu8[0] = (uint8)(u32Val >> 24);
  pu8[1] = (uint8)(u32Val >> 16);
  pu8[2] = (uint8)(u32Val >> 8);
  pu8[3] = (uint8)u32Val;
  return;
}

static inline uint32 dm_frame_rd32(dm_in const uint8 *pu8)
{
  return ((uint32)pu8[0] << 24) | ((uint32)pu8[1] << 16) | ((uint32)pu8[2] << 8) | (uint32)pu8[3];
}

static inline void dm_frame_wr64(dm_out uint8 *pu8, dm_in uint64 u64Val)
{
  dm_frame_wr32(pu8, (uint32)(u64Val >> 32));
  dm_frame_wr32(pu8 + 4, (uint32)u64Val);
  return;
}

static inline uint64 dm_frame_rd64(dm_in const uint8 *pu8)
{
  return ((uint64)dm_frame_rd32(pu8) << 32) | (uint64)dm_frame_rd32(pu8 + 4);
}

/* 写帧头到 pu8Out（要有 DM_FRAME_HEAD 字节） */
static inline void dm_frame_put(dm_out uint8 *pu8Out, dm_in const dm_frame_head_st *pstHead)
{
  dm_frame_wr16(pu8Out, pstHead->u16LinkID);
  pu8Out[2] = pstHead->u8Type;
  pu8Out[3] = pstHead->u8Flag;
  dm_frame_wr32(pu8Out + 4, pstHead->u32ReqID);
  dm_frame_wr32(pu8Out + 8, (uint32)pstHead->i32Status);
  dm_frame_wr32(pu8Out + 12, pstHead->u32Len);
  return;
}

/* 从 pu8In（至少 DM_FRAME_HEAD 字节）读帧头 */
static inline void dm_frame_get(dm_in const uint8 *pu8In, dm_out dm_frame_head_st *pstHead)
{
  pstHead->u16LinkID = (uint16)dm_frame_rd16(pu8In);
  pstHead->u8Type = pu8In[2];
  pstHead->u8Flag = pu8In[3];
  pstHead->u32ReqID = dm_frame_rd32(pu8In + 4);
  pstHead->i32Status = (int32)dm_frame_rd32(pu8In + 8);
  pstHead->u32Len = dm_frame_rd32(pu8In + 12);
  return;
}

#ifdef __cplusplus
}
#endif

#endif /* DM_FRAME_H */
