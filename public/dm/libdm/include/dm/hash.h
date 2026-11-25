/**
 ******************************************************************************
 * @file    hash.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   DM 链地址法哈希表（拉链桶采用双向循环链表 dm_list_st）。
 *
 *          -------------------
 *          | 0 |-> node1->node2
 *          -------------------
 *          | 1 |-> node3
 *          -------------------
 *          | 2 |-> NULL
 *          -------------------
 ******************************************************************************
 */
#ifndef DM_HASH_H
#define DM_HASH_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h> /* 提供 size_t 类型定义 */
#include <stdlib.h>
#include <dm/type.h>
#include <dm.h>
#include <dm/list.h>

/**
 * @brief 哈希表节点结构
 */
typedef struct dm_hash_node
{
  dm_list_node_st stNode; /* 链表节点 */
  uint32 key;             /* 键值 */
  void *value;            /* 值指针 */
} dm_hash_node_st;

/**
 * @brief 哈希表结构
 */
typedef struct dm_hash_table
{
  dm_list_st *pstBuckets;              /* 桶数组，每个桶是一个链表 */
  uint32 size;                         /* 桶数量 */
  uint32 count;                        /* 节点数量 */
  uint32 (*pfHashFunc)(uint32 u32Key); /* 哈希函数指针 */
} dm_hash_table_st;

/**
 * @brief 从节点指针获取哈希节点指针
 * @param pstNode 链表节点指针
 * @return 哈希节点指针
 */
#define DM_HASH_NODE_ENTRY(pstNode) dm_container_of(pstNode, dm_hash_node_st, stNode)

/**
 * @brief 默认哈希函数(内部)
 * @param u32Key 输入键值
 * @return 哈希值
 */
static inline uint32 dm__hash_default_func(dm_in uint32 u32Key)
{
  /* Jenkins One-At-A-Time Hash */
  u32Key = (u32Key + 0x7ed55d16) + (u32Key << 12);
  u32Key = (u32Key ^ 0xc761c23c) ^ (u32Key >> 19);
  u32Key = (u32Key + 0x165667b1) + (u32Key << 5);
  u32Key = (u32Key + 0xd3a2646c) ^ (u32Key << 9);
  u32Key = (u32Key + 0xfd7046c5) + (u32Key << 3);
  u32Key = (u32Key ^ 0xb55a4f09) ^ (u32Key >> 16);
  return u32Key;
}

/**
 * @brief 初始化哈希表
 * @param pstTable 哈希表指针
 * @param u32Size 桶数量
 * @param pfHashFunc 自定义哈希函数(可为NULL，使用默认函数)
 * @return DM_SUCCESS成功，其他失败
 */
static inline int dm_hash_init(dm_in dm_hash_table_st *pstTable, dm_in uint32 u32Size,
                               dm_in uint32 (*pfHashFunc)(uint32 u32Key))
{
  if (NULL == pstTable || 0 == u32Size)
  {
    return DM_INVALID;
  }

  /* 分配桶数组 */
  pstTable->pstBuckets = (dm_list_st *)dm_malloc_tag(sizeof(dm_list_st) * u32Size, DM_MEM_TAG_HASH);
  if (NULL == pstTable->pstBuckets)
  {
    return DM_ALLOC;
  }

  /* 初始化每个桶 */
  uint32 i;
  for (i = 0; i < u32Size; i++)
  {
    dm_list_init(&pstTable->pstBuckets[i]);
  }

  pstTable->size = u32Size;
  pstTable->count = 0;
  pstTable->pfHashFunc = (NULL != pfHashFunc) ? pfHashFunc : dm__hash_default_func;

  return DM_SUCCESS;
}

/**
 * @brief 计算键值的哈希值(内部)
 * @param pstTable 哈希表指针
 * @param u32Key 键值
 * @return 哈希值
 */
static inline uint32 dm__hash_value(dm_in const dm_hash_table_st *pstTable, dm_in uint32 u32Key)
{
  if (NULL == pstTable || NULL == pstTable->pfHashFunc)
  {
    return dm__hash_default_func(u32Key);
  }
  return pstTable->pfHashFunc(u32Key);
}

/**
 * @brief 计算键值的桶索引(内部)
 * @param pstTable 哈希表指针
 * @param u32Key 键值
 * @return 桶索引
 */
static inline uint32 dm__hash_index(dm_in const dm_hash_table_st *pstTable, dm_in uint32 u32Key)
{
  if (NULL == pstTable || 0 == pstTable->size)
  {
    return 0;
  }
  return dm__hash_value(pstTable, u32Key) % pstTable->size;
}

/**
 * @brief 创建哈希节点(内部)
 * @param u32Key 键值
 * @param pvValue 值指针
 * @return 哈希节点指针，失败返回NULL
 */
static inline dm_hash_node_st *dm__hash_create_node(dm_in uint32 u32Key, dm_in void *pvValue)
{
  dm_hash_node_st *pstNode =
      (dm_hash_node_st *)dm_malloc_tag(sizeof(dm_hash_node_st), DM_MEM_TAG_HASH);
  if (NULL == pstNode)
  {
    return NULL;
  }

  dm_list_node_init(&pstNode->stNode);
  pstNode->key = u32Key;
  pstNode->value = pvValue;

  return pstNode;
}

/**
 * @brief 向哈希表中添加节点(内部)
 * @param pstTable 哈希表指针
 * @param pstNode 哈希节点指针
 * @return DM_SUCCESS成功，其他失败
 */
static inline int dm__hash_add_node(dm_in dm_hash_table_st *pstTable,
                                    dm_in dm_hash_node_st *pstNode)
{
  if (NULL == pstTable || NULL == pstNode || NULL == pstTable->pstBuckets)
  {
    return DM_INVALID;
  }

  /* 计算桶索引 */
  uint32 u32Index = dm__hash_index(pstTable, pstNode->key);

  /* 添加节点到桶的头部 */
  dm_list_add_head(&pstTable->pstBuckets[u32Index], &pstNode->stNode);
  pstTable->count++;

  return DM_SUCCESS;
}

/**
 * @brief 向哈希表中添加键值对
 * @param pstTable 哈希表指针
 * @param u32Key 键值
 * @param pvValue 值指针
 * @return DM_SUCCESS成功，其他失败
 */
static inline int dm_hash_add(dm_in dm_hash_table_st *pstTable, dm_in uint32 u32Key,
                              dm_in void *pvValue)
{
  dm_hash_node_st *pstNode = dm__hash_create_node(u32Key, pvValue);
  if (NULL == pstNode)
  {
    return DM_ALLOC;
  }

  int iErrCode = dm__hash_add_node(pstTable, pstNode);
  if (DM_SUCCESS != iErrCode)
  {
    dm_free_tag(pstNode);
  }

  return iErrCode;
}

/**
 * @brief 从哈希表中查找节点(内部)
 * @param pstTable 哈希表指针
 * @param u32Key 键值
 * @return 哈希节点指针，未找到返回NULL
 */
static inline dm_hash_node_st *dm__hash_find_node(dm_in const dm_hash_table_st *pstTable,
                                                  dm_in uint32 u32Key)
{
  if (NULL == pstTable || NULL == pstTable->pstBuckets)
  {
    return NULL;
  }

  /* 计算桶索引 */
  uint32 u32Index = dm__hash_index(pstTable, u32Key);

  /* 在桶中查找节点 */
  dm_list_node_st *pstListNode;
  DM_LIST_FOREACH(&pstTable->pstBuckets[u32Index], pstListNode)
  {
    dm_hash_node_st *pstNode = DM_HASH_NODE_ENTRY(pstListNode);
    if (pstNode->key == u32Key)
    {
      return pstNode;
    }
  }

  return NULL;
}

/**
 * @brief 从哈希表中查找值
 * @param pstTable 哈希表指针
 * @param u32Key 键值
 * @return 值指针，未找到返回NULL
 */
static inline void *dm_hash_find(dm_in const dm_hash_table_st *pstTable, dm_in uint32 u32Key)
{
  dm_hash_node_st *pstNode = dm__hash_find_node(pstTable, u32Key);
  if (NULL == pstNode)
  {
    return NULL;
  }
  return pstNode->value;
}

/**
 * @brief 从哈希表中删除节点(内部)
 * @param pstTable 哈希表指针
 * @param pstNode 哈希节点指针
 * @return DM_SUCCESS成功，其他失败
 */
static inline int dm__hash_del_node(dm_in dm_hash_table_st *pstTable,
                                    dm_in dm_hash_node_st *pstNode)
{
  if (NULL == pstTable || NULL == pstNode || NULL == pstTable->pstBuckets)
  {
    return DM_INVALID;
  }

  /* 从链表中删除节点 */
  dm_list_del_node(&pstNode->stNode);
  pstTable->count--;

  return DM_SUCCESS;
}

/**
 * @brief 从哈希表中删除键值对
 * @param pstTable 哈希表指针
 * @param u32Key 键值
 * @param pfFree 自定义释放函数(可为NULL)
 * @return DM_SUCCESS成功，其他失败
 */
static inline int dm_hash_del(dm_in dm_hash_table_st *pstTable, dm_in uint32 u32Key,
                              dm_in void (*pfFree)(void *))
{
  dm_hash_node_st *pstNode = dm__hash_find_node(pstTable, u32Key);
  if (NULL == pstNode)
  {
    return DM_INVALID;
  }

  /* 释放值 */
  if (NULL != pfFree && NULL != pstNode->value)
  {
    pfFree(pstNode->value);
  }

  /* 从哈希表中删除节点 */
  int iErrCode = dm__hash_del_node(pstTable, pstNode);
  if (DM_SUCCESS != iErrCode)
  {
    return iErrCode;
  }

  /* 释放节点内存 */
  dm_free_tag(pstNode);

  return DM_SUCCESS;
}

/**
 * @brief 销毁哈希表
 * @param pstTable 哈希表指针
 * @param pfFree 自定义释放函数(可为NULL)
 */
static inline void dm_hash_destroy(dm_in dm_hash_table_st *pstTable, dm_in void (*pfFree)(void *))
{
  if (NULL == pstTable || NULL == pstTable->pstBuckets)
  {
    return;
  }

  dm_hash_node_st *pstNode;
  dm_list_node_st *pstListNode, *pstNextListNode;
  uint32 u32Index = 0;

  /* 遍历所有桶 */
  for (u32Index = 0; u32Index < pstTable->size; u32Index++)
  {
    dm_list_st *pstBucket = &pstTable->pstBuckets[u32Index];

    /* 安全遍历链表，避免删除节点时出现问题 */
    DM_LIST_FOREACH_SAFE(pstBucket, pstListNode, pstNextListNode)
    {
      pstNode = DM_HASH_NODE_ENTRY(pstListNode);

      /* 释放值 */
      if (NULL != pfFree && NULL != pstNode->value)
      {
        pfFree(pstNode->value);
      }

      /* 从链表中删除节点 */
      dm_list_del_node(pstListNode);

      /* 释放节点内存 */
      dm_free_tag(pstNode);
    }
  }

  /* 释放桶数组 */
  dm_free_tag(pstTable->pstBuckets);
  pstTable->pstBuckets = NULL;
  pstTable->size = 0;
  pstTable->count = 0;
  pstTable->pfHashFunc = NULL;
  return;
}

/**
 * @brief 获取哈希表中的节点数量
 * @param pstTable 哈希表指针
 * @return 节点数量
 */
static inline uint32 dm_hash_count(dm_in const dm_hash_table_st *pstTable)
{
  if (NULL == pstTable)
  {
    return 0;
  }
  return pstTable->count;
}

/**
 * @brief 检查哈希表是否为空
 * @param pstTable 哈希表指针
 * @return true为空，false不为空
 */
static inline bool_t dm_hash_is_empty(dm_in const dm_hash_table_st *pstTable)
{
  return (dm_hash_count(pstTable) == 0);
}

/**
 * @brief 遍历哈希表的宏定义
 * @param pstTable 哈希表指针
 * @param pstNode 节点指针
 * @param u32Index 桶索引变量
 * @param pstListNode 链表节点指针(临时变量)
 */
#define DM_HASH_FOREACH(pstTable, pstNode, index, pstListNode)                                     \
  for ((index) = 0; (index) < (pstTable)->size; (index)++)                                         \
    DM_LIST_FOREACH(&((pstTable)->pstBuckets[(index)]), (pstListNode))                             \
  if (NULL != ((pstNode) = DM_HASH_NODE_ENTRY(pstListNode)))

/**
 * @brief 安全遍历哈希表的宏定义
 * @param pstTable 哈希表指针
 * @param pstNode 节点指针
 * @param u32Index 桶索引变量
 * @param pstListNode 链表节点指针(临时变量)
 * @param pstNextListNode 下一个链表节点指针(临时变量)
 */
#define DM_HASH_FOREACH_SAFE(pstTable, pstNode, index, pstListNode, pstNextListNode)               \
  for ((index) = 0; (index) < (pstTable)->size; (index)++)                                         \
    DM_LIST_FOREACH_SAFE(&((pstTable)->pstBuckets[(index)]), (pstListNode), (pstNextListNode))     \
  if (NULL != ((pstNode) = DM_HASH_NODE_ENTRY(pstListNode)))

#ifdef __cplusplus
}
#endif

#endif // DM_HASH_H
