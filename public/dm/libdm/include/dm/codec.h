/**
 ******************************************************************************
 * @file    codec.h
 * @brief   dm 字节流编解码器 —— 常见类型与自定义结构体的封装/解封装。
 *
 * 定位:无状态字节流编解码。每个函数把"一个值"写到缓冲(encode)或从缓冲解出
 * (decode),返回写/读字节数;缓冲内一律为网络序大端。字符串按含结尾 NUL 的
 * 方式编解(自包含,方便顺序拼装)。业务结构体经 dm_codec_register
 * 按名字注册 dm_encode_cb / dm_decode_cb,再按名字调用 dm_codec_encode /
 * dm_codec_decode 封解。NULL 参数一律返回 0。
 *
 * 注意术语:本模块的"方案(schema)"只描述"字节怎么解成结构体",与"业务归谁处理"无关。
 ******************************************************************************
 */
#ifndef DM_CODEC_H
#define DM_CODEC_H

#include <stddef.h>
#include <dm.h>
#include <dm/type.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ---- 基本类型编码:value → 缓冲,返回写字节数(NULL 返回 0) ---- */
uint dm_encode_uint8(dm_in uint8 u8Value, dm_out void *pvbuffer);
uint dm_encode_int8(dm_in int8 iValue, dm_out void *pvbuffer);
uint dm_encode_uint16(dm_in uint16 u16Value, dm_out void *pvbuffer);
uint dm_encode_int16(dm_in int16 i16Value, dm_out void *pvbuffer);
uint dm_encode_uint32(dm_in uint32 u32Value, dm_out void *pvbuffer);
uint dm_encode_int32(dm_in int32 i32Value, dm_out void *pvbuffer);
uint dm_encode_uint64(dm_in uint64 u64Value, dm_out void *pvbuffer);
uint dm_encode_int64(dm_in int64 i64Value, dm_out void *pvbuffer);
uint dm_encode_float(dm_in float flValue, dm_out void *pvbuffer);
uint dm_encode_double(dm_in double dValue, dm_out void *pvbuffer);
uint dm_encode_bool(dm_in bool_t bValue, dm_out void *pvbuffer);
uint dm_encode_uchar(dm_in uchar ucValue, dm_out void *pvbuffer);
uint dm_encode_char(dm_in char cValue, dm_out void *pvbuffer);
uint dm_encode_string(dm_in const char *pcStr, dm_out void *pvbuffer); /* 含结尾 NUL */

/* ---- 基本类型解码:缓冲 → 值,返回读字节数(NULL 返回 0) ---- */
uint dm_decode_uint8(dm_in const void *pvbuffer, dm_out uint8 *pu8Value);
uint dm_decode_int8(dm_in const void *pvbuffer, dm_out int8 *piValue);
uint dm_decode_uint16(dm_in const void *pvbuffer, dm_out uint16 *pu16Value);
uint dm_decode_int16(dm_in const void *pvbuffer, dm_out int16 *pi16Value);
uint dm_decode_uint32(dm_in const void *pvbuffer, dm_out uint32 *pu32Value);
uint dm_decode_int32(dm_in const void *pvbuffer, dm_out int32 *pi32Value);
uint dm_decode_uint64(dm_in const void *pvbuffer, dm_out uint64 *pu64Value);
uint dm_decode_int64(dm_in const void *pvbuffer, dm_out int64 *pi64Value);
uint dm_decode_float(dm_in const void *pvbuffer, dm_out float *pflValue);
uint dm_decode_double(dm_in const void *pvbuffer, dm_out double *pdValue);
uint dm_decode_bool(dm_in const void *pvbuffer, dm_out bool_t *pbValue);
uint dm_decode_uchar(dm_in const void *pvbuffer, dm_out uchar *pucValue);
uint dm_decode_char(dm_in const void *pvbuffer, dm_out char *pcValue);
uint dm_decode_string(dm_in const void *pvbuffer,
                      dm_out const char **ppcStr); /* 读至 NUL,返回含 NUL 字节数 */

/* ---- 结构体封解函数数组(按编解码方案枚举做下标) ---- */
/* 编解码方案列表宏:单一来源,同时生成枚举值、名称字符串表与"名称→方案"查找。 */
#define DM_CODEC_SCHEMA_MAP(XX)                                                                    \
  XX(MAIN, "main")                                                                                 \
  XX(NET, "net")                                                                                   \
  XX(LINK, "link")                                                                                 \
  XX(SESSION, "session")                                                                           \
  XX(MSG, "msg")                                                                                   \
  XX(STATE, "state")                                                                               \
  XX(PLUG_STATS, "plug_stats")

#define DM_CODEC_SCHEMA_COUNT 256 /* 槽位总容量(框架方案 + 预留扩展) */

/* clang-format off */
typedef enum dm_codec_schema
{
#define DM_CODEC_SCHEMA_XX(n, s) DM_CODEC_##n,
  DM_CODEC_SCHEMA_MAP(DM_CODEC_SCHEMA_XX)
#undef DM_CODEC_SCHEMA_XX
  DM_CODEC_COUNT, /* 框架方案数(由 MAP 自动) */
} dm_codec_schema_et;
/* clang-format on */

/* 结构体封解回调:encode 把 pucData 封进 pvbuffer;decode 从 pvbuffer 解出 pucData。
 * 返回封解字节数或错误码;由业务实现,经方案注册挂接。 */
typedef int (*dm_encode_cb)(dm_in uchar *pucData, dm_in uint uiDataLen, dm_out void *pvbuffer);
typedef int (*dm_decode_cb)(dm_in const void *pvbuffer, dm_in uint uiBufferLen,
                            dm_out uchar *pucData);

typedef struct dm_codec_fn
{
  dm_encode_cb pfEncode; /* 可 NULL,表示该方案未注册编码 */
  dm_decode_cb pfDecode; /* 可 NULL,表示该方案未注册解码 */
} dm_codec_fn_st;

/* 按名字注册一个方案的封解函数(可覆盖;传 NULL 表示该方向不注册)。
 * 名字未命中(不在方案表)返回 DM_INVALID。 */
int dm_codec_register(dm_in const char *pcName, dm_in dm_encode_cb pfEncode,
                      dm_in dm_decode_cb pfDecode);

/* 按名字封/解结构体:自动调用该方案注册的封解函数;
 * 名字未命中返回 DM_INVALID;该方向未注册返回 DM_NOT_FOUND。 */
int dm_codec_encode(dm_in const char *pcName, dm_in uchar *pucData, dm_in uint uiDataLen,
                    dm_out void *pvbuffer);
int dm_codec_decode(dm_in const char *pcName, dm_in const void *pvbuffer, dm_in uint uiBufferLen,
                    dm_out uchar *pucData);

#ifdef __cplusplus
}
#endif

#endif /* DM_CODEC_H */
