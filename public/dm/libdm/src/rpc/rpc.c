/**
 ******************************************************************************
 * @file    rpc.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   接口表、应答凭据与内置 RPC 接口（DD-01）。
 *          不管：连接和帧（link_proto.c）；推送在 push.c，反向调用在 call.c。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "rpc"
#include <dm.h>
#include <dm/rpc.h>
#include <dm/json.h>
#include <dm/hash.h>
#include <dm/list.h>
#include <dm/log.h>
#include <dm/limits.h>

#include <ctype.h>  /* tolower */
#include <string.h> /* memset / strcmp / strstr / memcmp */

#include "../dm_internal.h"
#include "../transport/conn.h" /* dm__link_frame_max */

#define DM__IFACE_BUCKETS 64u

static dm_hash_table_st g_stIfaceTable; /* linkID -> dm_iface_st（拷贝） */

/* ============================ 接口表 ============================ */

static int dm__iface_add(in const dm_iface_st *pstIface)
{
  dm_iface_st *pstCopy = NULL;

  if ((NULL == g_stIfaceTable.pstBuckets) &&
      (DM_SUCCESS != dm_hash_init(&g_stIfaceTable, DM__IFACE_BUCKETS, NULL)))
  {
    return DM_ALLOC;
  }
  pstCopy = (dm_iface_st *)dm_malloc_tag(sizeof(dm_iface_st), DM_MEM_TAG_RPC);
  if (NULL == pstCopy)
  {
    return DM_ALLOC;
  }
  *pstCopy = *pstIface;
  if (DM_SUCCESS != dm_hash_add(&g_stIfaceTable, pstIface->uiLinkID, pstCopy))
  {
    dm_free_tag(pstCopy);
    return DM_ALLOC;
  }
  return DM_SUCCESS;
}

static const dm_iface_st *dm__iface_find(in uint uiLinkID)
{
  if (NULL == g_stIfaceTable.pstBuckets)
  {
    return NULL;
  }
  return (const dm_iface_st *)dm_hash_find(&g_stIfaceTable, uiLinkID);
}

/* 挨个看接口表（接口不多）；pfEach 返回真就停 */
static void dm__iface_each(in bool_t (*pfEach)(in const dm_iface_st *pstOne, in void *pvArg),
                           in void *pvArg)
{
  uint32 u32Idx = 0;
  dm_list_node_st *pstNode = NULL;

  if (NULL == g_stIfaceTable.pstBuckets)
  {
    return;
  }
  for (u32Idx = 0; u32Idx < g_stIfaceTable.size; u32Idx++)
  {
    DM_LIST_FOREACH(&g_stIfaceTable.pstBuckets[u32Idx], pstNode)
    {
      if (pfEach((const dm_iface_st *)DM_HASH_NODE_ENTRY(pstNode)->value, pvArg))
      {
        return;
      }
    }
  }
  return;
}

typedef struct dm__name_hit
{
  const char *pcName;
  bool_t bHit;
} dm__name_hit_st;

static bool_t dm__iface_name_cmp(in const dm_iface_st *pstOne, in void *pvArg)
{
  dm__name_hit_st *pstHit = (dm__name_hit_st *)pvArg;
  pstHit->bHit = (NULL != pstOne->pcName) && (0 == strcmp(pstOne->pcName, pstHit->pcName));
  return pstHit->bHit;
}

/* 名字重了没有 */
static bool_t dm__iface_name_taken(in const char *pcName)
{
  dm__name_hit_st stHit = {0};
  stHit.pcName = pcName;
  stHit.bHit = DM_FALSE;
  dm__iface_each(dm__iface_name_cmp, &stHit);
  return stHit.bHit;
}

int dm_iface_register(in const dm_iface_st *pstIface)
{
  DM__CHECK_LOOP();
  if ((DM__PHASE_INIT != dm__phase()) && (DM__PHASE_NONE != dm__phase()))
  {
    DM_LOG_ERROR("dm_iface_register outside init");
    return DM_INVALID;
  }
  if ((NULL == pstIface) || (NULL == pstIface->pcName))
  {
    return DM_INVALID;
  }
  if (NULL == dm__link_lookup(pstIface->uiLinkID))
  {
    return DM_LINK_ERR_NOT_FOUND;
  }
  if ((NULL != dm__iface_find(pstIface->uiLinkID)) || dm__iface_name_taken(pstIface->pcName))
  {
    return DM_EXISTS;
  }
  return dm__iface_add(pstIface);
}

const char *dm__iface_name(in uint uiLinkID)
{
  const dm_iface_st *pstIface = dm__iface_find(uiLinkID);
  return (NULL != pstIface) ? pstIface->pcName : NULL;
}

/* 校验外部连接角色是否有权调用指定 uiLinkID（DD-01） */
int dm__iface_allow(in uint uiLinkID, in uint32 u32Role)
{
  const dm_iface_st *pstIface = dm__iface_find(uiLinkID);

  if (NULL == pstIface)
  {
    return DM_LINK_ERR_NOT_FOUND; /* 只登记了三段的，和编号不存在一样 */
  }
  return (0u != (pstIface->u32Roles & u32Role)) ? DM_SUCCESS : DM_PERM;
}

static void dm__iface_free(in void *pvValue)
{
  dm_free_tag(pvValue);
  return;
}

void dm__rpc_destroy(void)
{
  dm_hash_destroy(&g_stIfaceTable, dm__iface_free);
  memset(&g_stIfaceTable, 0, sizeof(g_stIfaceTable));
  return;
}

/* ============================ 应答 ============================ */

bool_t dm_reply_equal(in const dm_reply_st *pstA, in const dm_reply_st *pstB)
{
  if ((NULL == pstA) || (NULL == pstB))
  {
    return DM_FALSE;
  }
  return (0 == memcmp(pstA, pstB, sizeof(*pstA))) ? DM_TRUE : DM_FALSE;
}

int dm_reply_take(in const dm_link_msg_st *pstMsg, out dm_reply_st *pstReply)
{
  const dm__reply_ctx_st *pstCtx = NULL;

  if ((NULL == pstMsg) || (NULL == pstReply))
  {
    return DM_INVALID;
  }
  memset(pstReply, 0, sizeof(*pstReply));
  pstCtx = (const dm__reply_ctx_st *)pstMsg->pvPriv;
  if ((NULL == pstCtx) || (0u == pstCtx->u32ConnID) || (0u == pstCtx->u32ReqID))
  {
    return DM_NOT_FOUND;
  }
  pstReply->au32Priv[0] = pstCtx->u32ConnID;
  pstReply->au32Priv[1] = pstCtx->u32ReqID;
  pstReply->au32Priv[2] = ((uint32)(pstMsg->u8Flags & DM_FRAME_F_SENSITIVE) << 24) |
                          ((uint32)pstCtx->u8Src << 16) | (pstMsg->uiLinkID & 0xFFFFu);
  pstReply->au32Priv[3] = DM__REPLY_MAGIC;
  return DM_SUCCESS;
}

int dm_reply_send(in const dm_reply_st *pstReply, in int iStatus, in const void *pvBody,
                  in uint32 u32Len)
{
  DM__CHECK_LOOP();
  if ((NULL == pstReply) || (DM__REPLY_MAGIC != pstReply->au32Priv[3]) ||
      ((NULL == pvBody) && (u32Len > 0u)))
  {
    return DM_INVALID;
  }
  if (DM_SRC_HTTP == (uint8)((pstReply->au32Priv[2] >> 16) & 0xFFu))
  {
    return DM_INVALID; /* HTTP 来的请求用 dm_http_reply 答 */
  }
  if (u32Len > dm__link_frame_max())
  {
    return DM_TOO_BIG; /* 超出单帧上限；尚未应答，调用方可改发错误应答 */
  }
  return dm__conn_answer_ex(pstReply->au32Priv[0], pstReply->au32Priv[2] & 0xFFFFu,
                            pstReply->au32Priv[1], iStatus, (uint8)(pstReply->au32Priv[2] >> 24),
                            pvBody, u32Len);
}

/* ============================ DM 内置接口 ============================ */

/* 回一段 JSON（拼好的文本）；超帧上限时兜底回 DM_TOO_BIG 避免 pending 挂死 */
static void dm__builtin_answer(in dm_link_msg_st *pstMsg, in int iStatus, in const char *pcJson,
                               in size_t uLen)
{
  dm_reply_st stReply = {0};
  if (DM_SUCCESS == dm_reply_take(pstMsg, &stReply))
  {
    int iSendErr = dm_reply_send(&stReply, iStatus, pcJson, (NULL != pcJson) ? (uint32)uLen : 0u);
    if (DM_TOO_BIG == iSendErr)
    {
      (void)dm_reply_send(&stReply, DM_TOO_BIG, NULL, 0u);
    }
  }
  return;
}

static void dm__builtin_answer_mut(in dm_link_msg_st *pstMsg, in yyjson_mut_doc *pstDoc)
{
  size_t uLen = 0;
  char *pcText = dm_json_write_mut(pstDoc, &uLen);
  dm__builtin_answer(pstMsg, (NULL != pcText) ? DM_SUCCESS : DM_ALLOC, pcText,
                     (NULL != pcText) ? uLen : 0u);
  dm_json_free_str(pcText);
  return;
}

/* dm.ping：心跳 */
static int dm__builtin_ping(in void *pvArg)
{
  dm__builtin_answer((dm_link_msg_st *)pvArg, DM_SUCCESS, NULL, 0u);
  return DM_SUCCESS;
}

/* dm.iface.list：列出全部接口，每项带 allowed（当前连接角色是否允许调用）。无权项仍列于清单，
 * 实际调用时由核心返回 DM_PERM；客户端帮助仅展示允许项 */
typedef struct dm__list_ctx
{
  yyjson_mut_doc *pstDoc;
  yyjson_mut_val *pstArr;
  uint32 u32Role;
  bool_t bOom;
} dm__list_ctx_st;

static bool_t dm__builtin_list_one(in const dm_iface_st *pstOne, in void *pvArg)
{
  dm__list_ctx_st *pstCtx = (dm__list_ctx_st *)pvArg;
  yyjson_mut_val *pstObj = NULL;
  bool bOk = true;

  if (pstOne->uiLinkID <= 0x00FFu)
  {
    return DM_FALSE; /* DM 自己的不列（dmcmd 有固定命令） */
  }
  pstObj = yyjson_mut_arr_add_obj(pstCtx->pstDoc, pstCtx->pstArr);
  if (NULL == pstObj)
  {
    pstCtx->bOom = DM_TRUE;
    return DM_TRUE;
  }
  bOk = bOk && yyjson_mut_obj_add_uint(pstCtx->pstDoc, pstObj, "id", pstOne->uiLinkID);
  bOk = bOk && yyjson_mut_obj_add_str(pstCtx->pstDoc, pstObj, "name", pstOne->pcName);
  bOk = bOk && yyjson_mut_obj_add_str(pstCtx->pstDoc, pstObj, "desc",
                                      (NULL != pstOne->pcDesc) ? pstOne->pcDesc : "");
  bOk = bOk && yyjson_mut_obj_add_str(pstCtx->pstDoc, pstObj, "args",
                                      (NULL != pstOne->pcArgs) ? pstOne->pcArgs : "");
  bOk = bOk && yyjson_mut_obj_add_uint(pstCtx->pstDoc, pstObj, "flags", pstOne->u32Flags);
  bOk = bOk && yyjson_mut_obj_add_bool(pstCtx->pstDoc, pstObj, "mutate",
                                       0u != (pstOne->u32Flags & DM_IFACE_MUTATE));
  bOk = bOk && yyjson_mut_obj_add_bool(pstCtx->pstDoc, pstObj, "advanced",
                                       0u != (pstOne->u32Flags & DM_IFACE_ADVANCED));
  bOk = bOk && yyjson_mut_obj_add_bool(pstCtx->pstDoc, pstObj, "diag",
                                       0u != (pstOne->u32Flags & DM_IFACE_DIAG));
  bOk = bOk && yyjson_mut_obj_add_bool(pstCtx->pstDoc, pstObj, "test_only",
                                       0u != (pstOne->u32Flags & DM_IFACE_TEST_ONLY));
  bOk = bOk && yyjson_mut_obj_add_bool(pstCtx->pstDoc, pstObj, "stateless",
                                       0u != (pstOne->u32Flags & DM_IFACE_STATELESS));
  bOk = bOk && yyjson_mut_obj_add_bool(pstCtx->pstDoc, pstObj, "allowed",
                                       0u != (pstOne->u32Roles & pstCtx->u32Role));
  if (!bOk)
  {
    pstCtx->bOom = DM_TRUE;
    return DM_TRUE;
  }
  return DM_FALSE;
}

static int dm__builtin_iface_list(in void *pvArg)
{
  dm_link_msg_st *pstMsg = (dm_link_msg_st *)pvArg;
  dm__list_ctx_st stCtx = {0};
  yyjson_mut_val *pstRoot = NULL;

  stCtx.pstDoc = yyjson_mut_doc_new(dm_json_alc());
  if (NULL == stCtx.pstDoc)
  {
    dm__builtin_answer(pstMsg, DM_ALLOC, NULL, 0u);
    return DM_SUCCESS;
  }
  stCtx.pstArr = yyjson_mut_arr(stCtx.pstDoc);
  stCtx.u32Role = pstMsg->u32Role;
  pstRoot = yyjson_mut_obj(stCtx.pstDoc);
  if ((NULL == pstRoot) || (NULL == stCtx.pstArr) ||
      !yyjson_mut_obj_add_val(stCtx.pstDoc, pstRoot, "ifaces", stCtx.pstArr))
  {
    yyjson_mut_doc_free(stCtx.pstDoc);
    dm__builtin_answer(pstMsg, DM_ALLOC, NULL, 0u);
    return DM_SUCCESS;
  }
  yyjson_mut_doc_set_root(stCtx.pstDoc, pstRoot);
  dm__iface_each(dm__builtin_list_one, &stCtx);
  if (stCtx.bOom)
  {
    yyjson_mut_doc_free(stCtx.pstDoc);
    dm__builtin_answer(pstMsg, DM_ALLOC, NULL, 0u);
    return DM_SUCCESS;
  }
  dm__builtin_answer_mut(pstMsg, stCtx.pstDoc);
  yyjson_mut_doc_free(stCtx.pstDoc);
  return DM_SUCCESS;
}

/* dm.log.recent：{"limit"|"n":100,"mod":"...","level":"debug|info|warn|error","grep":"..."}
 * -> {"lines":[...]} */
typedef struct dm__recent_ctx
{
  yyjson_mut_doc *pstDoc;
  yyjson_mut_val *pstArr;
  yyjson_mut_val **ppstRing;
  uint32 u32Limit;
  uint32 u32Head;
  uint32 u32Count;
  const char *pcMod;
  size_t uModLen;
  const char *pcGrep;
  int iMinLevel; /* -1 表示不过滤；0=D,1=I,2=W,3=E */
  bool_t bOom;
} dm__recent_ctx_st;

static int dm__log_parse_level(in const char *pcLevel)
{
  char acLow[16] = {0};
  size_t uIdx = 0u;

  if ((NULL == pcLevel) || ('\0' == pcLevel[0]))
  {
    return -1;
  }
  for (uIdx = 0u; ('\0' != pcLevel[uIdx]) && (uIdx + 1u < sizeof(acLow)); uIdx++)
  {
    acLow[uIdx] = (char)tolower((unsigned char)pcLevel[uIdx]);
  }
  if ('\0' != pcLevel[uIdx])
  {
    return -2;
  }
  acLow[uIdx] = '\0';
  if ((0 == strcmp(acLow, "debug")) || (0 == strcmp(acLow, "d")))
  {
    return (int)DM_LOG_LEVEL_DEBUG;
  }
  if ((0 == strcmp(acLow, "info")) || (0 == strcmp(acLow, "i")))
  {
    return (int)DM_LOG_LEVEL_INFO;
  }
  if ((0 == strcmp(acLow, "warn")) || (0 == strcmp(acLow, "w")))
  {
    return (int)DM_LOG_LEVEL_WARN;
  }
  if ((0 == strcmp(acLow, "error")) || (0 == strcmp(acLow, "e")))
  {
    return (int)DM_LOG_LEVEL_ERROR;
  }
  return -2;
}

static int dm__log_char_level(in char cLevel)
{
  switch (cLevel)
  {
  case 'D':
    return (int)DM_LOG_LEVEL_DEBUG;
  case 'I':
    return (int)DM_LOG_LEVEL_INFO;
  case 'W':
    return (int)DM_LOG_LEVEL_WARN;
  case 'E':
    return (int)DM_LOG_LEVEL_ERROR;
  default:
    return -1;
  }
}

/* 解析日志行 "YYYY-MM-DD HH:MM:SS.mmm <L> T<nn> <mod>  <file:line>  <msg>" 做过滤 */
static bool_t dm__recent_match(in const char *pcLine, in const dm__recent_ctx_st *pstCtx)
{
  const char *pc = pcLine;
  const char *pcTok = NULL;
  size_t uTokLen = 0u;
  uint uiTok = 0u;
  int iLineLevel = -1;
  const char *pcLineMod = NULL;
  size_t uLineModLen = 0u;

  if ((pstCtx->iMinLevel >= 0) || (NULL != pstCtx->pcMod))
  {
    for (uiTok = 0u; uiTok <= 4u; uiTok++)
    {
      while (' ' == *pc)
      {
        pc++;
      }
      if ('\0' == *pc)
      {
        return DM_FALSE;
      }
      pcTok = pc;
      while (('\0' != *pc) && (' ' != *pc))
      {
        pc++;
      }
      uTokLen = (size_t)(pc - pcTok);
      if (2u == uiTok)
      {
        iLineLevel = (1u == uTokLen) ? dm__log_char_level(pcTok[0]) : -1;
      }
      else if (4u == uiTok)
      {
        pcLineMod = pcTok;
        uLineModLen = uTokLen;
      }
    }
    if ((pstCtx->iMinLevel >= 0) && ((iLineLevel < 0) || (iLineLevel < pstCtx->iMinLevel)))
    {
      return DM_FALSE;
    }
    if ((NULL != pstCtx->pcMod) &&
        ((uLineModLen != pstCtx->uModLen) || (0 != memcmp(pcLineMod, pstCtx->pcMod, uLineModLen))))
    {
      return DM_FALSE;
    }
  }
  if ((NULL != pstCtx->pcGrep) && (NULL == strstr(pcLine, pstCtx->pcGrep)))
  {
    return DM_FALSE;
  }
  return DM_TRUE;
}

static void dm__builtin_recent_line(in const char *pcLine, in void *pvArg)
{
  dm__recent_ctx_st *pstCtx = (dm__recent_ctx_st *)pvArg;
  yyjson_mut_val *pstStr = NULL;

  if (pstCtx->bOom || (0u == pstCtx->u32Limit))
  {
    return;
  }
  if (!dm__recent_match(pcLine, pstCtx))
  {
    return;
  }
  if (NULL == pstCtx->ppstRing)
  {
    if (!yyjson_mut_arr_add_strcpy(pstCtx->pstDoc, pstCtx->pstArr, pcLine))
    {
      pstCtx->bOom = DM_TRUE;
    }
    return;
  }
  pstStr = yyjson_mut_strcpy(pstCtx->pstDoc, pcLine);
  if (NULL == pstStr)
  {
    pstCtx->bOom = DM_TRUE;
    return;
  }
  if (pstCtx->u32Count < pstCtx->u32Limit)
  {
    pstCtx->ppstRing[(pstCtx->u32Head + pstCtx->u32Count) % pstCtx->u32Limit] = pstStr;
    pstCtx->u32Count++;
  }
  else
  {
    pstCtx->ppstRing[pstCtx->u32Head] = pstStr;
    pstCtx->u32Head = (pstCtx->u32Head + 1u) % pstCtx->u32Limit;
  }
  return;
}

/* 读取可选字符串字段并校验不含内嵌 NUL；字段缺失或为空串时 *ppcOut 置 NULL */
static int dm__recent_str_field(in yyjson_val *pstRoot, in const char *pcKey,
                                out const char **ppcOut, out size_t *puLenOut)
{
  yyjson_val *pstVal = yyjson_obj_get(pstRoot, pcKey);
  const char *pcStr = NULL;
  size_t uLen = 0u;

  *ppcOut = NULL;
  if (NULL != puLenOut)
  {
    *puLenOut = 0u;
  }
  if (NULL == pstVal)
  {
    return DM_SUCCESS;
  }
  if (!yyjson_is_str(pstVal))
  {
    return DM_INVALID;
  }
  pcStr = yyjson_get_str(pstVal);
  uLen = yyjson_get_len(pstVal);
  if (strlen(pcStr) != uLen)
  {
    return DM_INVALID;
  }
  if (uLen > 0u)
  {
    *ppcOut = pcStr;
    if (NULL != puLenOut)
    {
      *puLenOut = uLen;
    }
  }
  return DM_SUCCESS;
}

/* 解析 limit / n 字段并钳位至 [0, DM_LOG_RECENT] */
static int dm__recent_parse_limit(in yyjson_val *pstRoot, out uint32 *pu32Limit)
{
  yyjson_val *pstLimit = NULL;
  int64 i64N = 100;

  if (NULL != pstRoot)
  {
    pstLimit = yyjson_obj_get(pstRoot, "limit");
    if (NULL == pstLimit)
    {
      pstLimit = yyjson_obj_get(pstRoot, "n");
    }
  }
  if (NULL != pstLimit)
  {
    if (yyjson_is_sint(pstLimit))
    {
      i64N = yyjson_get_sint(pstLimit);
    }
    else if (yyjson_is_uint(pstLimit))
    {
      uint64 u64Val = yyjson_get_uint(pstLimit);
      i64N = (u64Val > (uint64)DM_LOG_RECENT) ? (int64)DM_LOG_RECENT : (int64)u64Val;
    }
    else
    {
      return DM_INVALID;
    }
  }
  if (i64N <= 0)
  {
    *pu32Limit = 0u;
  }
  else if ((uint64)i64N > (uint64)DM_LOG_RECENT)
  {
    *pu32Limit = DM_LOG_RECENT;
  }
  else
  {
    *pu32Limit = (uint32)i64N;
  }
  return DM_SUCCESS;
}

/* 解析 dm.log.recent 请求体并填充过滤上下文；调用方负责释放 *ppstReq */
static int dm__recent_parse_req(in const dm_link_msg_st *pstMsg, out yyjson_doc **ppstReq,
                                io dm__recent_ctx_st *pstCtx, out uint32 *pu32Limit,
                                out bool_t *pbFilter)
{
  yyjson_doc *pstReq = NULL;
  yyjson_val *pstRoot = NULL;
  const char *pcLvlVal = NULL;
  int iErrCode = DM_SUCCESS;

  *ppstReq = NULL;
  *pbFilter = DM_FALSE;
  if (pstMsg->u32Len > 0u)
  {
    int iReadErr = DM_INVALID;
    pstReq = dm_json_read((const char *)pstMsg->pu8Data, pstMsg->u32Len, 4096u, &iReadErr);
    if (NULL == pstReq)
    {
      return (DM_SUCCESS != iReadErr) ? iReadErr : DM_INVALID;
    }
    pstRoot = yyjson_doc_get_root(pstReq);
    if (!yyjson_is_obj(pstRoot))
    {
      yyjson_doc_free(pstReq);
      return DM_INVALID;
    }
  }
  iErrCode = dm__recent_parse_limit(pstRoot, pu32Limit);
  if ((DM_SUCCESS == iErrCode) && (NULL != pstRoot))
  {
    iErrCode = dm__recent_str_field(pstRoot, "mod", &pstCtx->pcMod, &pstCtx->uModLen);
  }
  if ((DM_SUCCESS == iErrCode) && (NULL != pstRoot))
  {
    iErrCode = dm__recent_str_field(pstRoot, "level", &pcLvlVal, NULL);
  }
  if ((DM_SUCCESS == iErrCode) && (NULL != pcLvlVal))
  {
    pstCtx->iMinLevel = dm__log_parse_level(pcLvlVal);
    if (pstCtx->iMinLevel < 0)
    {
      iErrCode = DM_INVALID;
    }
  }
  if ((DM_SUCCESS == iErrCode) && (NULL != pstRoot))
  {
    iErrCode = dm__recent_str_field(pstRoot, "grep", &pstCtx->pcGrep, NULL);
  }
  if (DM_SUCCESS != iErrCode)
  {
    yyjson_doc_free(pstReq);
    return iErrCode;
  }
  if ((NULL != pstCtx->pcMod) || (pstCtx->iMinLevel >= 0) || (NULL != pstCtx->pcGrep))
  {
    *pbFilter = DM_TRUE;
  }
  *ppstReq = pstReq;
  return DM_SUCCESS;
}

/* 收集最近日志行至 pstCtx->pstArr；有过滤条件时先用环形数组保底最近 u32Limit 条 */
static int dm__recent_collect(io dm__recent_ctx_st *pstCtx, in uint32 u32Limit, in bool_t bFilter)
{
  uint32 u32Idx = 0u;

  if (0u == u32Limit)
  {
    return DM_SUCCESS;
  }
  if (!bFilter)
  {
    (void)dm_log_recent(u32Limit, dm__builtin_recent_line, pstCtx);
    return pstCtx->bOom ? DM_ALLOC : DM_SUCCESS;
  }
  pstCtx->ppstRing =
      (yyjson_mut_val **)dm_calloc_tag(u32Limit, sizeof(yyjson_mut_val *), DM_MEM_TAG_RPC);
  if (NULL == pstCtx->ppstRing)
  {
    return DM_ALLOC;
  }
  (void)dm_log_recent(DM_LOG_RECENT, dm__builtin_recent_line, pstCtx);
  for (u32Idx = 0u; !pstCtx->bOom && (u32Idx < pstCtx->u32Count); u32Idx++)
  {
    yyjson_mut_val *pstItem = pstCtx->ppstRing[(pstCtx->u32Head + u32Idx) % pstCtx->u32Limit];
    if (!yyjson_mut_arr_append(pstCtx->pstArr, pstItem))
    {
      pstCtx->bOom = DM_TRUE;
    }
  }
  dm_free_tag(pstCtx->ppstRing);
  pstCtx->ppstRing = NULL;
  return pstCtx->bOom ? DM_ALLOC : DM_SUCCESS;
}

static int dm__builtin_log_recent(in void *pvArg)
{
  dm_link_msg_st *pstMsg = (dm_link_msg_st *)pvArg;
  yyjson_doc *pstReq = NULL;
  uint32 u32Limit = 100u;
  bool_t bFilter = DM_FALSE;
  dm__recent_ctx_st stCtx = {0};
  yyjson_mut_val *pstRootOut = NULL;
  int iErrCode = DM_SUCCESS;

  stCtx.iMinLevel = -1;
  iErrCode = dm__recent_parse_req(pstMsg, &pstReq, &stCtx, &u32Limit, &bFilter);
  if (DM_SUCCESS != iErrCode)
  {
    dm__builtin_answer(pstMsg, iErrCode, NULL, 0u);
    return DM_SUCCESS;
  }
  stCtx.pstDoc = yyjson_mut_doc_new(dm_json_alc());
  if (NULL == stCtx.pstDoc)
  {
    yyjson_doc_free(pstReq);
    dm__builtin_answer(pstMsg, DM_ALLOC, NULL, 0u);
    return DM_SUCCESS;
  }
  pstRootOut = yyjson_mut_obj(stCtx.pstDoc);
  stCtx.pstArr = yyjson_mut_arr(stCtx.pstDoc);
  stCtx.u32Limit = u32Limit;
  if ((NULL == pstRootOut) || (NULL == stCtx.pstArr) ||
      !yyjson_mut_obj_add_val(stCtx.pstDoc, pstRootOut, "lines", stCtx.pstArr))
  {
    yyjson_mut_doc_free(stCtx.pstDoc);
    yyjson_doc_free(pstReq);
    dm__builtin_answer(pstMsg, DM_ALLOC, NULL, 0u);
    return DM_SUCCESS;
  }
  yyjson_mut_doc_set_root(stCtx.pstDoc, pstRootOut);
  iErrCode = dm__recent_collect(&stCtx, u32Limit, bFilter);
  yyjson_doc_free(pstReq);
  if (DM_SUCCESS != iErrCode)
  {
    yyjson_mut_doc_free(stCtx.pstDoc);
    dm__builtin_answer(pstMsg, iErrCode, NULL, 0u);
    return DM_SUCCESS;
  }
  dm__builtin_answer_mut(pstMsg, stCtx.pstDoc);
  yyjson_mut_doc_free(stCtx.pstDoc);
  return DM_SUCCESS;
}

typedef struct dm__log_level_opts
{
  bool_t bSetLevel;
  dm_log_level_et eNewLevel;
  bool_t bSetConsole;
  int iNewConsole;
} dm__log_level_opts_st;

static int dm__log_level_match(in const char *pcLevel, in const char *const *ppcNames,
                               in uint uiCount, out dm_log_level_et *peLevel)
{
  uint uiIdx = 0u;

  for (uiIdx = 0u; uiIdx < uiCount; uiIdx++)
  {
    if (0 == strcmp(pcLevel, ppcNames[uiIdx]))
    {
      *peLevel = (dm_log_level_et)uiIdx;
      return DM_SUCCESS;
    }
  }
  return DM_INVALID;
}

static int dm__log_level_parse_req(in const dm_link_msg_st *pstMsg, in const char *const *ppcNames,
                                   in uint uiCount, out dm__log_level_opts_st *pstOpts)
{
  yyjson_doc *pstReq = NULL;
  yyjson_val *pstRoot = NULL;
  yyjson_val *pstLevel = NULL;
  yyjson_val *pstConsole = NULL;
  int iReadErr = DM_INVALID;
  int iErrCode = DM_SUCCESS;

  memset(pstOpts, 0, sizeof(*pstOpts));
  pstOpts->eNewLevel = DM_LOG_LEVEL_INFO;
  if (0u == pstMsg->u32Len)
  {
    return DM_SUCCESS;
  }
  pstReq = dm_json_read((const char *)pstMsg->pu8Data, pstMsg->u32Len, 4096u, &iReadErr);
  if (NULL == pstReq)
  {
    return (DM_SUCCESS != iReadErr) ? iReadErr : DM_INVALID;
  }
  pstRoot = yyjson_doc_get_root(pstReq);
  if (!yyjson_is_obj(pstRoot))
  {
    yyjson_doc_free(pstReq);
    return DM_INVALID;
  }
  pstLevel = yyjson_obj_get(pstRoot, "level");
  if (NULL != pstLevel)
  {
    const char *pcLevel = NULL;
    size_t uLevelLen = 0u;
    if (!yyjson_is_str(pstLevel))
    {
      iErrCode = DM_INVALID;
    }
    else
    {
      pcLevel = yyjson_get_str(pstLevel);
      uLevelLen = yyjson_get_len(pstLevel);
      iErrCode = (strlen(pcLevel) == uLevelLen)
                     ? dm__log_level_match(pcLevel, ppcNames, uiCount, &pstOpts->eNewLevel)
                     : DM_INVALID;
      pstOpts->bSetLevel = (DM_SUCCESS == iErrCode) ? DM_TRUE : DM_FALSE;
    }
  }
  pstConsole = (DM_SUCCESS == iErrCode) ? yyjson_obj_get(pstRoot, "console") : NULL;
  if (NULL != pstConsole)
  {
    if (!yyjson_is_bool(pstConsole))
    {
      iErrCode = DM_INVALID;
    }
    else
    {
      pstOpts->bSetConsole = DM_TRUE;
      pstOpts->iNewConsole = yyjson_get_bool(pstConsole) ? 1 : 0;
    }
  }
  yyjson_doc_free(pstReq);
  return iErrCode;
}

/* dm.log.level：{"level":"debug|info|warn|error|none","console":true|false}，两个都可以不给；
 * 先全量校验再落地变更，失败走统一错误 JSON 信封（DD-01） */
static int dm__builtin_log_level(in void *pvArg)
{
  static const char *const apcName[] = {"debug", "info", "warn", "error", "none"};
  dm_link_msg_st *pstMsg = (dm_link_msg_st *)pvArg;
  dm__log_level_opts_st stOpts = {0};
  char acOut[64] = {0};
  uint uiIdx = 0u;
  int iErrCode = DM_SUCCESS;

  iErrCode = dm__log_level_parse_req(pstMsg, apcName, (uint)ARRAY_SIZE(apcName), &stOpts);
  if (DM_SUCCESS != iErrCode)
  {
    dm__builtin_answer(pstMsg, iErrCode, NULL, 0u);
    return DM_SUCCESS;
  }
  if (stOpts.bSetLevel)
  {
    dm_log_set_level(stOpts.eNewLevel);
  }
  if (stOpts.bSetConsole)
  {
    dm_log_set_console(stOpts.iNewConsole);
  }
  uiIdx = (uint)dm_log_get_level();
  snprintf(acOut, sizeof(acOut), "{\"level\":\"%s\"}",
           (uiIdx < ARRAY_SIZE(apcName)) ? apcName[uiIdx] : "info");
  dm__builtin_answer(pstMsg, DM_SUCCESS, acOut, strlen(acOut));
  return DM_SUCCESS;
}

/* 登记表一建好就登记（不走「只在 init」那道检查：DM 自己的） */
int dm__rpc_builtin_init(void)
{
  static const dm_iface_st astBuiltin[] = {
      {DM_LINKID_IFACE_LIST, "dm.iface.list", DM_ROLE_ALL, 0u,
       "list the interfaces this connection may call", NULL},
      {DM_LINKID_LOG_RECENT, "dm.log.recent", DM_ROLE_ALL, 0u, "the most recent log lines",
       "{\"type\":\"object\",\"properties\":{\"limit\":{\"type\":\"integer\"},"
       "\"n\":{\"type\":\"integer\"},\"mod\":{\"type\":\"string\"},"
       "\"level\":{\"type\":\"string\",\"description\":\"debug|info|warn|error\"},"
       "\"grep\":{\"type\":\"string\"}}}"},
      {DM_LINKID_STAT, "dm.stat", DM_ROLE_ALL, 0u,
       "counters: handlers, loop, thread pool, memory, connections, log", NULL},
      {DM_LINKID_PING, "dm.ping", DM_ROLE_ALL, 0u, "heartbeat", NULL},
      {DM_LINKID_LOG_LEVEL, "dm.log.level", DM_ROLE_CLI, DM_IFACE_MUTATE,
       "change the log level or console output",
       "{\"type\":\"object\",\"properties\":{\"level\":{\"type\":\"string\",\"description\":"
       "\"debug|info|warn|error|none\"},\"console\":{\"type\":\"boolean\"}}}"},
      {DM_LINKID_DIAG_SNAPSHOT, "dm.diag.snapshot", DM_ROLE_CLI | DM_ROLE_DIAG, DM_IFACE_DIAG,
       "runtime diagnostic snapshot: conns, queue, lanes, timers, loop, push", NULL},
  };
  static const dm_link_before_cb apfBefore[] = {
      dm__builtin_iface_list, dm__builtin_log_recent, dm__stat_builtin,
      dm__builtin_ping,       dm__builtin_log_level,  dm__diag_snapshot_builtin,
  };
  uint uiIdx = 0;
  int iErrCode = DM_SUCCESS;

  for (uiIdx = 0; uiIdx < ARRAY_SIZE(astBuiltin); uiIdx++)
  {
    iErrCode = dm__link_register(astBuiltin[uiIdx].uiLinkID, apfBefore[uiIdx], NULL, NULL);
    if (DM_SUCCESS == iErrCode)
    {
      iErrCode = dm__iface_add(&astBuiltin[uiIdx]);
    }
    if (DM_SUCCESS != iErrCode)
    {
      return iErrCode;
    }
  }
  return DM_SUCCESS;
}

#ifdef __cplusplus
}
#endif
