/**
 ******************************************************************************
 * @file    push.c
 * @author  MDM
 * @version 0.0.0.1
 * @brief   可靠推送、断线续传与流式推送（DD-01）。
 *
 * 推送：64 位序号从 1 递增；整帧拷一份进环形缓冲（按条数、字节数两个上限，满了丢最老的），
 *       再发给握手时说了要收这一类的连接。重连带（启动号, 最后序号）的，续得上就补发。
 * 流式：不带序号、不进缓冲。同一 (类, 键) 的片接在一个小缓冲后面，有数据时才开一个
 *       DM_STREAM_MS 的一次性定时器，到点发给收这一类的连接；连接积压超过 DM_STREAM_HIGH
 *       的这一拍不给它发，丢掉、不补。
 * 不管：连接和帧（link_proto.c）。
 ******************************************************************************
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define DM_LOG_MOD "push"
#include <dm.h>
#include <dm/rpc.h>
#include <dm/frame.h>
#include <dm/hash.h>
#include <dm/list.h>
#include <dm/log.h>
#include <dm/limits.h>

#include <string.h> /* memcpy / strcmp */

#include "../dm_internal.h"
#include "../transport/conn.h" /* dm__link_frame_max */

#define DM__STREAM_KEEP (64u * 1024u) /* 流式一个键的缓冲发完以后最多留这么大 */

#define DM__TOPIC_MAX 256u /* 推送的类最多登记几个 */

typedef struct dm__topic
{
  uint uiTopic;
  const char *pcName;
} dm__topic_st;

/* 环形缓冲里的一条：整帧（帧头 + 18 字节前缀 + 值），补发时原样写出去 */
typedef struct dm__push_ent
{
  uint64 u64Seq;
  uint uiTopic;
  uint32 u32Len;
  uint8 *pu8Frame;
} dm__push_ent_st;

/* 流式：一个 (类, 键) 攒着的片 */
typedef struct dm__stream
{
  dm_list_node_st stNode;
  uint uiTopic;
  uint64 u64Key;
  uint8 *pu8Data;
  uint32 u32Len;
  uint32 u32Cap;
} dm__stream_st;

static dm__topic_st g_astTopic[DM__TOPIC_MAX];
static uint g_uiTopicCount = 0;
static uint64 g_u64PushSeq = 0;
static dm__push_ent_st *g_astPushRing = NULL; /* DM_PUSH_COUNT 格，第一次推的时候分配 */
static uint32 g_u32RingHead = 0;              /* 最老那条在哪一格 */
static uint32 g_u32RingCount = 0;
static uint64 g_u64RingBytes = 0;
static dm_list_st g_stStreams;
static bool_t g_bStreamsUp = DM_FALSE;
static dm_timer_st g_stStreamTimer;
static bool_t g_bStreamTimerUp = DM_FALSE;

/* ============================ 类 ============================ */

static const dm__topic_st *dm__push_topic_find(in uint uiTopic)
{
  uint uiIdx = 0;
  for (uiIdx = 0; uiIdx < g_uiTopicCount; uiIdx++)
  {
    if (g_astTopic[uiIdx].uiTopic == uiTopic)
    {
      return &g_astTopic[uiIdx];
    }
  }
  return NULL;
}

int dm_push_topic(in uint uiTopic, in const char *pcName)
{
  uint uiIdx = 0;

  DM__CHECK_LOOP();
  if ((DM__PHASE_INIT != dm__phase()) && (DM__PHASE_NONE != dm__phase()))
  {
    DM_LOG_ERROR("dm_push_topic outside init");
    return DM_INVALID;
  }
  if ((NULL == pcName) || (uiTopic > 0xFFFFu))
  {
    return DM_INVALID;
  }
  for (uiIdx = 0; uiIdx < g_uiTopicCount; uiIdx++)
  {
    if ((g_astTopic[uiIdx].uiTopic == uiTopic) || (0 == strcmp(g_astTopic[uiIdx].pcName, pcName)))
    {
      return DM_EXISTS;
    }
  }
  if (g_uiTopicCount >= DM__TOPIC_MAX)
  {
    return DM_RANGE;
  }
  g_astTopic[g_uiTopicCount].uiTopic = uiTopic;
  g_astTopic[g_uiTopicCount].pcName = pcName;
  g_uiTopicCount++;
  return DM_SUCCESS;
}

int dm__push_topic_id(in const char *pcName)
{
  uint uiIdx = 0;
  for (uiIdx = 0; uiIdx < g_uiTopicCount; uiIdx++)
  {
    if (0 == strcmp(g_astTopic[uiIdx].pcName, pcName))
    {
      return (int)g_astTopic[uiIdx].uiTopic;
    }
  }
  return -1;
}

/* ============================ 推送 ============================ */

static uint8 *dm__push_frame(in uint uiTopic, in uint64 u64Seq, in uint64 u64Key, in uint8 u8Flag,
                             in const void *pv, in uint32 u32Len, out uint32 *pu32Total)
{
  dm_frame_head_st stHead = {0};
  uint32 u32Total = DM_FRAME_HEAD + DM_FRAME_PUSH_HEAD + u32Len;
  uint8 *pu8 = (uint8 *)dm_malloc_tag(u32Total, DM_MEM_TAG_PUSH);

  if (NULL == pu8)
  {
    return NULL;
  }
  memset(&stHead, 0, sizeof(stHead));
  stHead.u16LinkID = (uint16)uiTopic;
  stHead.u8Type = DM_FRAME_T_PUSH;
  stHead.u8Flag = u8Flag;
  stHead.u32Len = DM_FRAME_PUSH_HEAD + u32Len;
  dm_frame_put(pu8, &stHead);
  dm_frame_wr64(pu8 + DM_FRAME_HEAD, u64Seq);
  dm_frame_wr16(pu8 + DM_FRAME_HEAD + 8u, (uint32)uiTopic);
  dm_frame_wr64(pu8 + DM_FRAME_HEAD + 10u, u64Key);
  if (u32Len > 0u)
  {
    memcpy(pu8 + DM_FRAME_HEAD + DM_FRAME_PUSH_HEAD, pv, u32Len);
  }
  *pu32Total = u32Total;
  return pu8;
}

static void dm__push_drop_oldest(void)
{
  dm__push_ent_st *pstOld = &g_astPushRing[g_u32RingHead];
  g_u64RingBytes -= pstOld->u32Len;
  dm_free_tag(pstOld->pu8Frame);
  memset(pstOld, 0, sizeof(*pstOld));
  g_u32RingHead = (g_u32RingHead + 1u) % DM_PUSH_COUNT;
  g_u32RingCount--;
  return;
}

int dm_push(in uint uiTopic, in uint64 u64Key, in const void *pvVal, in uint32 u32Len)
{
  dm__push_ent_st *pstEnt = NULL;
  uint8 *pu8Frame = NULL;
  uint32 u32Total = 0;

  DM__CHECK_LOOP();
  if ((NULL == pvVal) && (u32Len > 0u))
  {
    return DM_INVALID;
  }
  if (dm__phase() >= DM__PHASE_TEARDOWN)
  {
    return DM_SHUTTING_DOWN;
  }
  if (NULL == dm__push_topic_find(uiTopic))
  {
    return DM_NOT_FOUND; /* 序号不动 */
  }
  if ((uint64)u32Len + DM_FRAME_PUSH_HEAD > dm__link_frame_max())
  {
    return DM_TOO_BIG; /* 超出单帧上限；不推进序号 */
  }
  if (NULL == g_astPushRing)
  {
    g_astPushRing =
        (dm__push_ent_st *)dm_calloc_tag(DM_PUSH_COUNT, sizeof(dm__push_ent_st), DM_MEM_TAG_PUSH);
    if (NULL == g_astPushRing)
    {
      return DM_ALLOC;
    }
  }
  pu8Frame = dm__push_frame(uiTopic, g_u64PushSeq + 1u, u64Key,
                            (u32Len > 0u) ? DM_FRAME_F_JSON : 0u, pvVal, u32Len, &u32Total);
  if (NULL == pu8Frame)
  {
    return DM_ALLOC;
  }
  g_u64PushSeq++;
  /* 先腾地方：条数、字节两个上限，先到哪个算哪个；单条比上限还大的也留它自己一条 */
  while ((g_u32RingCount > 0u) &&
         ((g_u32RingCount >= DM_PUSH_COUNT) || (g_u64RingBytes + u32Total > (uint64)DM_PUSH_BYTES)))
  {
    dm__push_drop_oldest();
  }
  pstEnt = &g_astPushRing[(g_u32RingHead + g_u32RingCount) % DM_PUSH_COUNT];
  pstEnt->u64Seq = g_u64PushSeq;
  pstEnt->uiTopic = uiTopic;
  pstEnt->u32Len = u32Total;
  pstEnt->pu8Frame = pu8Frame;
  g_u32RingCount++;
  g_u64RingBytes += u32Total;
  dm__conn_push_fanout(uiTopic, pu8Frame, u32Total, DM_FALSE);
  return DM_SUCCESS;
}

int dm_push_peer(in const char *pcPeerID, in uint uiTopic, in uint64 u64Key, in const void *pvVal,
                 in uint32 u32Len)
{
  uint8 *pu8Frame = NULL;
  uint32 u32Total = 0u;
  int iErrCode = DM_SUCCESS;

  DM__CHECK_LOOP();
  if ((NULL == pcPeerID) || ('\0' == pcPeerID[0]) || ((NULL == pvVal) && (u32Len > 0u)))
  {
    return DM_INVALID;
  }
  if (dm__phase() >= DM__PHASE_TEARDOWN)
  {
    return DM_SHUTTING_DOWN;
  }
  if (NULL == dm__push_topic_find(uiTopic))
  {
    return DM_NOT_FOUND;
  }
  if ((uint64)u32Len + DM_FRAME_PUSH_HEAD > dm__link_frame_max())
  {
    return DM_TOO_BIG;
  }
  if (!dm__conn_peer_exists(pcPeerID))
  {
    return DM_CONN_GONE;
  }
  pu8Frame = dm__push_frame(uiTopic, 0u, u64Key, (u32Len > 0u) ? DM_FRAME_F_JSON : 0u, pvVal,
                            u32Len, &u32Total);
  if (NULL == pu8Frame)
  {
    return DM_ALLOC;
  }
  iErrCode = dm__conn_push_peer(pcPeerID, uiTopic, pu8Frame, u32Total);
  dm_free_tag(pu8Frame);
  return iErrCode;
}

uint64 dm_push_seq(void)
{
  if (!dm_in_loop()) /* 仅允许主循环线程调用：跨线程回 0 并记 ERROR */
  {
    dm__wrong_thread(__func__);
    return 0u;
  }
  return g_u64PushSeq;
}

uint64 dm__push_ring_bytes(void)
{
  return g_u64RingBytes;
}

uint32 dm__push_ring_count(void)
{
  return g_u32RingCount;
}

/* 握手续传判定（DD-01）：返回 "ok" / "full" / "none" */
const char *dm__push_resume(in const char *pcBoot, in bool_t bHasSeq, in uint64 u64Seen)
{
  uint64 u64Oldest = 0;

  if (NULL == pcBoot)
  {
    return "none";
  }
  if (!bHasSeq || (0 != strcmp(pcBoot, dm__boot_hex())) || (u64Seen > g_u64PushSeq))
  {
    return "full";
  }
  if (u64Seen == g_u64PushSeq)
  {
    return "ok"; /* 没落下 */
  }
  if (0u == g_u32RingCount)
  {
    return "full";
  }
  u64Oldest = g_astPushRing[g_u32RingHead].u64Seq;
  return (u64Oldest <= u64Seen + 1u) ? "ok" : "full";
}

/* 续上了：把 u64Seen 之后的补给这条连接（它要收的那几类） */
void dm__push_replay(in uint32 u32ConnID, in uint64 u64Seen)
{
  uint32 u32Idx = 0;
  for (u32Idx = 0; u32Idx < g_u32RingCount; u32Idx++)
  {
    const dm__push_ent_st *pstEnt = &g_astPushRing[(g_u32RingHead + u32Idx) % DM_PUSH_COUNT];
    if (pstEnt->u64Seq > u64Seen)
    {
      if (DM_SUCCESS !=
          dm__conn_push_one(u32ConnID, pstEnt->uiTopic, pstEnt->pu8Frame, pstEnt->u32Len))
      {
        return; /* 连接没了（比如补发时积压太多被断开） */
      }
    }
  }
  return;
}

/* ============================ 流式 ============================ */

static dm__stream_st *dm__stream_find(in uint uiTopic, in uint64 u64Key)
{
  dm_list_node_st *pstNode = NULL;
  if (!g_bStreamsUp)
  {
    return NULL;
  }
  DM_LIST_FOREACH(&g_stStreams, pstNode)
  {
    dm__stream_st *pstOne = DM_LIST_ENTRY(pstNode, dm__stream_st, stNode);
    if ((pstOne->uiTopic == uiTopic) && (pstOne->u64Key == u64Key))
    {
      return pstOne;
    }
  }
  return NULL;
}

static void dm__stream_flush(io dm__stream_st *pstOne)
{
  uint8 *pu8Frame = NULL;
  uint32 u32Total = 0;

  if (0u == pstOne->u32Len)
  {
    return;
  }
  pu8Frame = dm__push_frame(pstOne->uiTopic, 0u, pstOne->u64Key, DM_FRAME_F_STREAM, pstOne->pu8Data,
                            pstOne->u32Len, &u32Total);
  pstOne->u32Len = 0;
  if (pstOne->u32Cap > DM__STREAM_KEEP) /* 临时扩容的流式缓冲发送后立即归还，避免长期驻留 */
  {
    dm_free_tag(pstOne->pu8Data);
    pstOne->pu8Data = NULL;
    pstOne->u32Cap = 0;
  }
  if (NULL == pu8Frame)
  {
    return;
  }
  dm__conn_push_fanout(pstOne->uiTopic, pu8Frame, u32Total, DM_TRUE);
  dm_free_tag(pu8Frame);
  return;
}

static void dm__stream_free(io dm__stream_st *pstOne)
{
  dm_list_del_node(&pstOne->stNode);
  dm_free_tag(pstOne->pu8Data);
  dm_free_tag(pstOne);
  return;
}

static void dm__stream_on_timer(in dm_timer_st *pstTimer)
{
  dm_list_node_st *pstNode = NULL;
  (void)pstTimer;
  DM_LIST_FOREACH(&g_stStreams, pstNode)
  {
    dm__stream_flush(DM_LIST_ENTRY(pstNode, dm__stream_st, stNode));
  }
  return; /* 一次性的：下一片来了再开 */
}

int dm_push_stream(in uint uiTopic, in uint64 u64Key, in const void *pvData, in uint32 u32Len)
{
  dm__stream_st *pstOne = NULL;

  DM__CHECK_LOOP();
  if ((NULL == pvData) && (u32Len > 0u))
  {
    return DM_INVALID;
  }
  if (dm__phase() >= DM__PHASE_TEARDOWN)
  {
    return DM_SHUTTING_DOWN;
  }
  if (NULL == dm__push_topic_find(uiTopic))
  {
    return DM_NOT_FOUND;
  }
  if ((uint64)u32Len + DM_FRAME_PUSH_HEAD > dm__link_frame_max())
  {
    return DM_TOO_BIG; /* 单片数据即超出单帧上限 */
  }
  if (0u == u32Len)
  {
    return DM_SUCCESS; /* 空数据不启动合批定时器 */
  }
  if (!g_bStreamsUp)
  {
    dm_list_init(&g_stStreams);
    g_bStreamsUp = DM_TRUE;
  }
  if (!g_bStreamTimerUp)
  {
    if (DM_SUCCESS != dm_timer_init(dm_default_loop(), &g_stStreamTimer))
    {
      return DM_ALLOC;
    }
    g_bStreamTimerUp = DM_TRUE;
  }
  pstOne = dm__stream_find(uiTopic, u64Key);
  if (NULL == pstOne)
  {
    pstOne = (dm__stream_st *)dm_calloc_tag(1u, sizeof(dm__stream_st), DM_MEM_TAG_PUSH);
    if (NULL == pstOne)
    {
      return DM_ALLOC;
    }
    pstOne->uiTopic = uiTopic;
    pstOne->u64Key = u64Key;
    dm_list_add_tail(&g_stStreams, &pstOne->stNode);
  }
  if ((uint64)pstOne->u32Len + u32Len + DM_FRAME_PUSH_HEAD > dm__link_frame_max())
  {
    dm__stream_flush(pstOne); /* 攒上这一片就超过单条上限：先把攒着的发掉 */
  }
  if (pstOne->u32Len + u32Len > pstOne->u32Cap)
  {
    uint32 u32Cap = (pstOne->u32Cap < 1024u) ? 1024u : pstOne->u32Cap;
    uint8 *pu8New = NULL;
    while (u32Cap < pstOne->u32Len + u32Len)
    {
      u32Cap *= 2u;
    }
    pu8New = (NULL == pstOne->pu8Data) ? (uint8 *)dm_malloc_tag(u32Cap, DM_MEM_TAG_PUSH)
                                       : (uint8 *)dm_realloc_tag(pstOne->pu8Data, u32Cap);
    if (NULL == pu8New)
    {
      return DM_ALLOC;
    }
    pstOne->pu8Data = pu8New;
    pstOne->u32Cap = u32Cap;
  }
  if (u32Len > 0u)
  {
    memcpy(pstOne->pu8Data + pstOne->u32Len, pvData, u32Len);
    pstOne->u32Len += u32Len;
  }
  if (pstOne->u32Len >= DM_STREAM_HIGH)
  {
    dm__stream_flush(pstOne); /* 一拍里攒得太多：不等定时器 */
  }
  else if (!dm_is_active((dm_handle_st *)&g_stStreamTimer))
  {
    (void)dm_timer_start(&g_stStreamTimer, dm__stream_on_timer, DM_STREAM_MS, 0);
  }
  return DM_SUCCESS;
}

int dm_push_stream_end(in uint uiTopic, in uint64 u64Key)
{
  dm__stream_st *pstOne = NULL;
  dm_list_node_st *pstNode = NULL;
  bool_t bPending = DM_FALSE;

  DM__CHECK_LOOP();
  pstOne = dm__stream_find(uiTopic, u64Key);
  if (NULL == pstOne)
  {
    return DM_NOT_FOUND;
  }
  dm__stream_flush(pstOne);
  dm__stream_free(pstOne);
  DM_LIST_FOREACH(&g_stStreams, pstNode)
  {
    if (0u != DM_LIST_ENTRY(pstNode, dm__stream_st, stNode)->u32Len)
    {
      bPending = DM_TRUE;
    }
  }
  if (!bPending && g_bStreamTimerUp)
  {
    (void)dm_timer_stop(&g_stStreamTimer); /* 没数据就没有定时器 */
  }
  return DM_SUCCESS;
}

bool_t dm__push_stream_timer_on(void)
{
  return (g_bStreamTimerUp && dm_is_active((dm_handle_st *)&g_stStreamTimer)) ? DM_TRUE : DM_FALSE;
}

/* ============================ 收尾 ============================ */

void dm__push_destroy(void)
{
  dm_list_node_st *pstNode = NULL;
  dm_list_node_st *pstNext = NULL;

  while (g_u32RingCount > 0u)
  {
    dm__push_drop_oldest();
  }
  dm_free_tag(g_astPushRing);
  g_astPushRing = NULL;
  g_u32RingHead = 0;
  g_u64RingBytes = 0;
  g_u64PushSeq = 0;
  g_uiTopicCount = 0;
  if (g_bStreamsUp)
  {
    DM_LIST_FOREACH_SAFE(&g_stStreams, pstNode, pstNext)
    {
      dm__stream_free(DM_LIST_ENTRY(pstNode, dm__stream_st, stNode));
    }
    g_bStreamsUp = DM_FALSE;
  }
  if (g_bStreamTimerUp)
  {
    (void)dm_timer_stop(&g_stStreamTimer);
    dm_close((dm_handle_st *)&g_stStreamTimer, NULL);
    g_bStreamTimerUp = DM_FALSE;
  }
  return;
}

#ifdef __cplusplus
}
#endif
