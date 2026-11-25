/**
 ******************************************************************************
 * @file    codec.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   DM 字节流编解码器实现（见 dm/codec.h）：
 *          无状态基本类型大端封解（纯移位不依赖 htonl/htons）与结构体方案注册表。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include <dm/codec.h>

#include <limits.h>
#include <string.h>

/* ============================ 基本类型编码（值 → 大端字节） ============================ */

uint dm_encode_uint8(dm_in uint8 u8Value, dm_out void *pvbuffer)
{
  DMASSERT(NULL != pvbuffer);

  if (NULL == pvbuffer)
  {
    return 0;
  }
  *(uint8 *)pvbuffer = u8Value;
  return 1;
}

uint dm_encode_int8(dm_in int8 iValue, dm_out void *pvbuffer)
{
  DMASSERT(NULL != pvbuffer);

  if (NULL == pvbuffer)
  {
    return 0;
  }
  *(int8 *)pvbuffer = iValue;
  return 1;
}

uint dm_encode_uint16(dm_in uint16 u16Value, dm_out void *pvbuffer)
{
  uint8 *pDst = (uint8 *)pvbuffer;

  DMASSERT(NULL != pvbuffer);

  if (NULL == pvbuffer)
  {
    return 0;
  }
  pDst[0] = (uint8)(u16Value >> 8); /* 大端:高字节在前 */
  pDst[1] = (uint8)u16Value;
  return 2;
}

uint dm_encode_int16(dm_in int16 i16Value, dm_out void *pvbuffer)
{
  return dm_encode_uint16((uint16)i16Value, pvbuffer); /* 强转同宽无符号,位模式不变 */
}

uint dm_encode_uint32(dm_in uint32 u32Value, dm_out void *pvbuffer)
{
  uint8 *pDst = (uint8 *)pvbuffer;

  DMASSERT(NULL != pvbuffer);

  if (NULL == pvbuffer)
  {
    return 0;
  }
  pDst[0] = (uint8)(u32Value >> 24);
  pDst[1] = (uint8)(u32Value >> 16);
  pDst[2] = (uint8)(u32Value >> 8);
  pDst[3] = (uint8)u32Value;
  return 4;
}

uint dm_encode_int32(dm_in int32 i32Value, dm_out void *pvbuffer)
{
  return dm_encode_uint32((uint32)i32Value, pvbuffer);
}

uint dm_encode_uint64(dm_in uint64 u64Value, dm_out void *pvbuffer)
{
  uint8 *pDst = (uint8 *)pvbuffer;
  int i = 0;

  DMASSERT(NULL != pvbuffer);

  if (NULL == pvbuffer)
  {
    return 0;
  }
  for (i = 0; i < 8; i++)
  {
    pDst[i] = (uint8)(u64Value >> ((7 - i) * 8)); /* 大端 */
  }
  return 8;
}

uint dm_encode_int64(dm_in int64 i64Value, dm_out void *pvbuffer)
{
  return dm_encode_uint64((uint64)i64Value, pvbuffer);
}

/* 浮点:位模式经同宽无符号整封解,不动位,保字节序正确。 */
uint dm_encode_float(dm_in float flValue, dm_out void *pvbuffer)
{
  uint32 uiBits = 0u;

  DMASSERT(NULL != pvbuffer);

  if (NULL == pvbuffer)
  {
    return 0;
  }
  memcpy(&uiBits, &flValue, sizeof(uiBits));
  return dm_encode_uint32(uiBits, pvbuffer);
}

uint dm_encode_double(dm_in double dValue, dm_out void *pvbuffer)
{
  uint64 uiBits = 0u;

  DMASSERT(NULL != pvbuffer);

  if (NULL == pvbuffer)
  {
    return 0;
  }
  memcpy(&uiBits, &dValue, sizeof(uiBits));
  return dm_encode_uint64(uiBits, pvbuffer);
}

uint dm_encode_bool(dm_in bool_t bValue, dm_out void *pvbuffer)
{
  DMASSERT(NULL != pvbuffer);

  if (NULL == pvbuffer)
  {
    return 0;
  }
  *(bool_t *)pvbuffer = bValue;
  return 1;
}

uint dm_encode_uchar(dm_in uchar ucValue, dm_out void *pvbuffer)
{
  DMASSERT(NULL != pvbuffer);

  if (NULL == pvbuffer)
  {
    return 0;
  }
  *(uchar *)pvbuffer = ucValue;
  return 1;
}

uint dm_encode_char(dm_in char cValue, dm_out void *pvbuffer)
{
  DMASSERT(NULL != pvbuffer);

  if (NULL == pvbuffer)
  {
    return 0;
  }
  *(char *)pvbuffer = cValue;
  return 1;
}

/* 字符串:写字节(不含 NUL)+ 结尾 NUL,返回含 NUL 字节数。 */
uint dm_encode_string(dm_in const char *pcStr, dm_out void *pvbuffer)
{
  size_t uLen = 0u;

  DMASSERT(NULL != pcStr);
  DMASSERT(NULL != pvbuffer);

  if (NULL == pcStr || NULL == pvbuffer)
  {
    return 0;
  }
  uLen = strlen(pcStr);
  if (uLen >= (size_t)UINT_MAX) /* 含 NUL 需 +1,防回绕 */
  {
    return 0;
  }
  memcpy(pvbuffer, pcStr, uLen);
  ((char *)pvbuffer)[uLen] = '\0';
  return (uint)(uLen + 1);
}

/* ============================ 基本类型解码（缓冲 → 值） ============================ */

uint dm_decode_uint8(dm_in const void *pvbuffer, dm_out uint8 *pu8Value)
{
  DMASSERT(NULL != pvbuffer);
  DMASSERT(NULL != pu8Value);

  if (NULL == pvbuffer || NULL == pu8Value)
  {
    return 0;
  }
  *pu8Value = *(const uint8 *)pvbuffer;
  return 1;
}

uint dm_decode_int8(dm_in const void *pvbuffer, dm_out int8 *piValue)
{
  DMASSERT(NULL != pvbuffer);
  DMASSERT(NULL != piValue);

  if (NULL == pvbuffer || NULL == piValue)
  {
    return 0;
  }
  *piValue = *(const int8 *)pvbuffer;
  return 1;
}

uint dm_decode_uint16(dm_in const void *pvbuffer, dm_out uint16 *pu16Value)
{
  const uint8 *pSrc = (const uint8 *)pvbuffer;

  DMASSERT(NULL != pvbuffer);
  DMASSERT(NULL != pu16Value);

  if (NULL == pvbuffer || NULL == pu16Value)
  {
    return 0;
  }
  *pu16Value = (uint16)((uint16)pSrc[0] << 8 | pSrc[1]); /* 大端还原 */
  return 2;
}

uint dm_decode_int16(dm_in const void *pvbuffer, dm_out int16 *pi16Value)
{
  return dm_decode_uint16(pvbuffer, (uint16 *)pi16Value);
}

uint dm_decode_uint32(dm_in const void *pvbuffer, dm_out uint32 *pu32Value)
{
  const uint8 *pSrc = (const uint8 *)pvbuffer;

  DMASSERT(NULL != pvbuffer);
  DMASSERT(NULL != pu32Value);

  if (NULL == pvbuffer || NULL == pu32Value)
  {
    return 0;
  }
  *pu32Value =
      ((uint32)pSrc[0] << 24) | ((uint32)pSrc[1] << 16) | ((uint32)pSrc[2] << 8) | (uint32)pSrc[3];
  return 4;
}

uint dm_decode_int32(dm_in const void *pvbuffer, dm_out int32 *pi32Value)
{
  return dm_decode_uint32(pvbuffer, (uint32 *)pi32Value);
}

uint dm_decode_uint64(dm_in const void *pvbuffer, dm_out uint64 *pu64Value)
{
  const uint8 *pSrc = (const uint8 *)pvbuffer;
  uint64 uValue = 0;
  int i = 0;

  DMASSERT(NULL != pvbuffer);
  DMASSERT(NULL != pu64Value);

  if (NULL == pvbuffer || NULL == pu64Value)
  {
    return 0;
  }
  for (i = 0; i < 8; i++)
  {
    uValue = (uValue << 8) | pSrc[i]; /* 大端还原 */
  }
  *pu64Value = uValue;
  return 8;
}

uint dm_decode_int64(dm_in const void *pvbuffer, dm_out int64 *pi64Value)
{
  return dm_decode_uint64(pvbuffer, (uint64 *)pi64Value);
}

uint dm_decode_float(dm_in const void *pvbuffer, dm_out float *pflValue)
{
  uint32 uiBits = 0u;

  DMASSERT(NULL != pvbuffer);
  DMASSERT(NULL != pflValue);

  if (NULL == pvbuffer || NULL == pflValue)
  {
    return 0;
  }
  if (dm_decode_uint32(pvbuffer, &uiBits) != 4)
  {
    return 0;
  }
  memcpy(pflValue, &uiBits, sizeof(uiBits));
  return 4;
}

uint dm_decode_double(dm_in const void *pvbuffer, dm_out double *pdValue)
{
  uint64 uiBits = 0u;

  DMASSERT(NULL != pvbuffer);
  DMASSERT(NULL != pdValue);

  if (NULL == pvbuffer || NULL == pdValue)
  {
    return 0;
  }
  if (dm_decode_uint64(pvbuffer, &uiBits) != 8)
  {
    return 0;
  }
  memcpy(pdValue, &uiBits, sizeof(uiBits));
  return 8;
}

uint dm_decode_bool(dm_in const void *pvbuffer, dm_out bool_t *pbValue)
{
  DMASSERT(NULL != pvbuffer);
  DMASSERT(NULL != pbValue);

  if (NULL == pvbuffer || NULL == pbValue)
  {
    return 0;
  }
  *pbValue = *(const bool_t *)pvbuffer;
  return 1;
}

uint dm_decode_uchar(dm_in const void *pvbuffer, dm_out uchar *pucValue)
{
  DMASSERT(NULL != pvbuffer);
  DMASSERT(NULL != pucValue);

  if (NULL == pvbuffer || NULL == pucValue)
  {
    return 0;
  }
  *pucValue = *(const uchar *)pvbuffer;
  return 1;
}

uint dm_decode_char(dm_in const void *pvbuffer, dm_out char *pcValue)
{
  DMASSERT(NULL != pvbuffer);
  DMASSERT(NULL != pcValue);

  if (NULL == pvbuffer || NULL == pcValue)
  {
    return 0;
  }
  *pcValue = *(const char *)pvbuffer;
  return 1;
}

/* 字符串:读到结尾 NUL,返回含 NUL 字节数;ppcStr 指向缓冲内字符串起始。 */
uint dm_decode_string(dm_in const void *pvbuffer, dm_out const char **ppcStr)
{
  const char *pcStr = NULL;

  DMASSERT(NULL != pvbuffer);
  DMASSERT(NULL != ppcStr);

  if (NULL == pvbuffer || NULL == ppcStr)
  {
    return 0;
  }
  pcStr = (const char *)pvbuffer;
  *ppcStr = pcStr;
  return (uint)(strlen(pcStr) + 1);
}

/* ============================ 结构体封解方案注册表 ============================ */

static dm_codec_fn_st g_aCodecFn[DM_CODEC_SCHEMA_COUNT];

/* 框架方案名称表(由 DM_CODEC_SCHEMA_MAP 生成) */
static const char *const g_ascSchemaName[DM_CODEC_COUNT] = {
#define DM_CODEC_SCHEMA_XX(n, s) [DM_CODEC_##n] = (s),
    DM_CODEC_SCHEMA_MAP(DM_CODEC_SCHEMA_XX)
#undef DM_CODEC_SCHEMA_XX
};

/* 名称 → 方案枚举(线性查找框架方案;未命中返回 DM_INVALID)。内部使用。 */
static int dm__codec_schema_from_name(dm_in const char *pcName)
{
  int e = 0;

  if (NULL == pcName)
  {
    return DM_INVALID;
  }
  for (e = 0; e < DM_CODEC_COUNT; e++)
  {
    if (strcmp(pcName, g_ascSchemaName[e]) == 0)
    {
      return e;
    }
  }
  return DM_INVALID;
}

/* 按名字注册方案的封解函数(可覆盖;传 NULL 表示该方向不注册)。 */
int dm_codec_register(dm_in const char *pcName, dm_encode_cb pfEncode, dm_decode_cb pfDecode)
{
  dm_codec_schema_et eSchema = (dm_codec_schema_et)dm__codec_schema_from_name(pcName);

  if (eSchema < 0 || eSchema >= DM_CODEC_SCHEMA_COUNT)
  {
    return DM_INVALID;
  }
  g_aCodecFn[eSchema].pfEncode = pfEncode;
  g_aCodecFn[eSchema].pfDecode = pfDecode;
  return DM_SUCCESS;
}

/* 按名字封结构体:找到注册的 encode 回调并执行。 */
int dm_codec_encode(dm_in const char *pcName, dm_in uchar *pucData, dm_in uint uiDataLen,
                    dm_out void *pvbuffer)
{
  dm_codec_schema_et eSchema = (dm_codec_schema_et)dm__codec_schema_from_name(pcName);

  if (eSchema < 0 || eSchema >= DM_CODEC_SCHEMA_COUNT)
  {
    return DM_INVALID;
  }
  if (NULL == g_aCodecFn[eSchema].pfEncode)
  {
    return DM_NOT_FOUND;
  }
  return g_aCodecFn[eSchema].pfEncode(pucData, uiDataLen, pvbuffer);
}

/* 按名字解结构体:找到注册的 decode 回调并执行。 */
int dm_codec_decode(dm_in const char *pcName, dm_in const void *pvbuffer, dm_in uint uiBufferLen,
                    dm_out uchar *pucData)
{
  dm_codec_schema_et eSchema = (dm_codec_schema_et)dm__codec_schema_from_name(pcName);

  if (eSchema < 0 || eSchema >= DM_CODEC_SCHEMA_COUNT)
  {
    return DM_INVALID;
  }
  if (NULL == g_aCodecFn[eSchema].pfDecode)
  {
    return DM_NOT_FOUND;
  }
  return g_aCodecFn[eSchema].pfDecode(pvbuffer, uiBufferLen, pucData);
}

#ifdef __cplusplus
}
#endif
