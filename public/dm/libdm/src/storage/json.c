/**
 ******************************************************************************
 * @file    json.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   JSON 封装的实现，见 dm/json.h。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include <dm.h>
#include <dm/json.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h> /* memset / memcpy */

/* ============================ 标准分配器 ============================
 * 走带标签的分配，泄漏时一眼看到是 JSON。 */
static void *dm__json_malloc(void *pvCtx, size_t uSize)
{
  (void)pvCtx;
  return dm_malloc_tag(uSize, DM_MEM_TAG_JSON);
}

static void *dm__json_realloc(void *pvCtx, void *pvPtr, size_t uOld, size_t uSize)
{
  (void)pvCtx;
  (void)uOld;
  if (NULL == pvPtr)
  {
    return dm_malloc_tag(uSize, DM_MEM_TAG_JSON);
  }
  return dm_realloc_tag(pvPtr, uSize);
}

static void dm__json_free(void *pvCtx, void *pvPtr)
{
  (void)pvCtx;
  dm_free_tag(pvPtr);
  return;
}

static const yyjson_alc g_stJsonAlc = {dm__json_malloc, dm__json_realloc, dm__json_free, NULL};

const yyjson_alc *dm_json_alc(void)
{
  return &g_stJsonAlc;
}

/* ============================ 敏感 JSON 分配器（AUD-06） ============================
 * max_align_t 对齐记录长度，realloc/free 前全量 dm_secure_zero。 */
typedef union dm__json_sens_hdr
{
  size_t uSize;
  max_align_t stAlign;
} dm__json_sens_hdr_ut;

_Static_assert((sizeof(dm__json_sens_hdr_ut) % _Alignof(max_align_t)) == 0u,
               "dm__json_sens_hdr_ut must stay aligned to max_align_t");

static void *dm__json_sens_malloc(void *pvCtx, size_t uSize)
{
  dm__json_sens_hdr_ut *pstHdr = NULL;
  (void)pvCtx;
  if (uSize > (SIZE_MAX - sizeof(dm__json_sens_hdr_ut)))
  {
    return NULL;
  }
  pstHdr =
      (dm__json_sens_hdr_ut *)dm_malloc_tag(sizeof(dm__json_sens_hdr_ut) + uSize, DM_MEM_TAG_JSON);
  if (NULL == pstHdr)
  {
    return NULL;
  }
  memset(pstHdr, 0, sizeof(*pstHdr));
  pstHdr->uSize = uSize;
  return (void *)(pstHdr + 1);
}

static void *dm__json_sens_realloc(void *pvCtx, void *pvPtr, size_t uOld, size_t uSize)
{
  dm__json_sens_hdr_ut *pstOld = NULL;
  void *pvNew = NULL;
  size_t uOldSize = 0;
  size_t uCopy = 0;
  (void)uOld;

  if (NULL == pvPtr)
  {
    return dm__json_sens_malloc(pvCtx, uSize);
  }
  pstOld = ((dm__json_sens_hdr_ut *)pvPtr) - 1;
  uOldSize = pstOld->uSize;
  if (0u == uSize)
  {
    dm_secure_zero(pstOld, sizeof(dm__json_sens_hdr_ut) + uOldSize);
    dm_free_tag(pstOld);
    return NULL;
  }
  pvNew = dm__json_sens_malloc(pvCtx, uSize);
  if (NULL == pvNew)
  {
    return NULL;
  }
  uCopy = (uOldSize < uSize) ? uOldSize : uSize;
  if (uCopy > 0u)
  {
    memcpy(pvNew, pvPtr, uCopy);
  }
  dm_secure_zero(pstOld, sizeof(dm__json_sens_hdr_ut) + uOldSize);
  dm_free_tag(pstOld);
  return pvNew;
}

static void dm__json_sens_free(void *pvCtx, void *pvPtr)
{
  dm__json_sens_hdr_ut *pstHdr = NULL;
  size_t uSize = 0;
  (void)pvCtx;
  if (NULL == pvPtr)
  {
    return;
  }
  pstHdr = ((dm__json_sens_hdr_ut *)pvPtr) - 1;
  uSize = pstHdr->uSize;
  dm_secure_zero(pstHdr, sizeof(dm__json_sens_hdr_ut) + uSize);
  dm_free_tag(pstHdr);
  return;
}

static const yyjson_alc g_stJsonSensAlc = {dm__json_sens_malloc, dm__json_sens_realloc,
                                           dm__json_sens_free, NULL};

const yyjson_alc *dm_json_alc_sensitive(void)
{
  return &g_stJsonSensAlc;
}

/* ============================ 解析、取值与序列化 ============================ */

static yyjson_doc *dm__json_read_with(dm_in const char *pcText, dm_in size_t uLen,
                                      dm_in size_t uMax, dm_in const yyjson_alc *pstAlc,
                                      dm_out int *piErr)
{
  yyjson_doc *pstDoc = NULL;
  yyjson_read_err stErr = {0};

  if (NULL != piErr)
  {
    *piErr = DM_SUCCESS;
  }
  if ((NULL == pcText) || ((0u != uMax) && (uLen > uMax)))
  {
    if (NULL != piErr)
    {
      *piErr = (NULL == pcText) ? DM_INVALID : DM_TOO_BIG;
    }
    return NULL;
  }
  memset(&stErr, 0, sizeof(stErr));
  pstDoc = yyjson_read_opts((char *)pcText, uLen, YYJSON_READ_NOFLAG, pstAlc, &stErr);
  if ((NULL == pstDoc) && (NULL != piErr))
  {
    *piErr = (YYJSON_READ_ERROR_MEMORY_ALLOCATION == stErr.code) ? DM_ALLOC : DM_INVALID;
  }
  return pstDoc;
}

yyjson_doc *dm_json_read(dm_in const char *pcText, dm_in size_t uLen, dm_in size_t uMax,
                         dm_out int *piErr)
{
  return dm__json_read_with(pcText, uLen, uMax, &g_stJsonAlc, piErr);
}

yyjson_doc *dm_json_read_sensitive(dm_in const char *pcText, dm_in size_t uLen, dm_in size_t uMax,
                                   dm_out int *piErr)
{
  return dm__json_read_with(pcText, uLen, uMax, &g_stJsonSensAlc, piErr);
}

const char *dm_json_str(dm_in yyjson_val *pstRoot, dm_in const char *pcPath,
                        dm_in const char *pcDef)
{
  yyjson_val *pstVal = (NULL != pstRoot) ? yyjson_ptr_get(pstRoot, pcPath) : NULL;
  return yyjson_is_str(pstVal) ? yyjson_get_str(pstVal) : pcDef;
}

int64 dm_json_int(dm_in yyjson_val *pstRoot, dm_in const char *pcPath, dm_in int64 i64Def)
{
  yyjson_val *pstVal = (NULL != pstRoot) ? yyjson_ptr_get(pstRoot, pcPath) : NULL;
  return yyjson_is_int(pstVal) ? (int64)yyjson_get_sint(pstVal) : i64Def;
}

bool_t dm_json_bool(dm_in yyjson_val *pstRoot, dm_in const char *pcPath, dm_in bool_t bDef)
{
  yyjson_val *pstVal = (NULL != pstRoot) ? yyjson_ptr_get(pstRoot, pcPath) : NULL;
  if (!yyjson_is_bool(pstVal))
  {
    return bDef;
  }
  return yyjson_get_bool(pstVal) ? DM_TRUE : DM_FALSE;
}

char *dm_json_write_mut(dm_in const yyjson_mut_doc *pstDoc, dm_out size_t *puLen)
{
  return yyjson_mut_write_opts(pstDoc, YYJSON_WRITE_NOFLAG, &g_stJsonAlc, puLen, NULL);
}

void dm_json_free_str(dm_in char *pcText)
{
  dm_free_tag(pcText);
  return;
}

char *dm_json_write_mut_sensitive(dm_in const yyjson_mut_doc *pstDoc, dm_out size_t *puLen)
{
  return yyjson_mut_write_opts(pstDoc, YYJSON_WRITE_NOFLAG, &g_stJsonSensAlc, puLen, NULL);
}

void dm_json_free_str_sensitive(dm_io char *pcText)
{
  dm__json_sens_free(NULL, pcText);
  return;
}

#ifdef __cplusplus
}
#endif
