/**
 ******************************************************************************
 * @file    list.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   DM 侵入式双向循环链表容器（O(1) 头尾增删与节点摘除）。
 ******************************************************************************
 */
#ifndef DM_LIST_H
#define DM_LIST_H

#include <stddef.h>
#include <dm/type.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t)&(((TYPE *)0)->MEMBER))
#endif

/**
 * dm_container_of - cast a member of a structure out to the containing structure
 * @ptr:    the pointer to the member.
 * @type:   the type of the container struct this is embedded in.
 * @member: the name of the member within the struct.
 */
#define dm_container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

#define DM_LIST_ENTRY(ptr, type, member) dm_container_of(ptr, type, member)

/**
 ******************************************************************************
 * @brief   DM Headed Circular List
 * @author  MDM
 * @version 0.0.0.1
 ******************************************************************************
 * @note    ---------     ---------     ---------     ---------     ---------
 *         :  head  :    :  node1 :    :  node2 :    :  node3 :    :  noden :
 *      -> : first  : -> :  next  : -> :  next  : -> :  next  : -> :  next  :->
 *      :  :  tial  : <- :  prev  : <- :  prev  : <- :  prev  : <- :  prev  : :
 *      :   ---------     ---------     ---------     ---------     --------- :
 *      +<------:-------------------------------------------------------^-----+
 *              v------------------------------------------------------>:
 *
 ******************************************************************************
 */
typedef struct dm_list_node
{
  struct dm_list_node *pstPrev;
  struct dm_list_node *pstNext;
} dm_list_node_st;

typedef struct dm_list_head
{
  dm_list_node_st stHead;
} dm_list_st;

static inline void dm_list_init(in dm_list_st *pstList);
static inline void dm_list_node_init(in dm_list_node_st *pstNode);
static inline bool_t dm_list_is_empty(in const dm_list_st *pstList);
static inline void dm_list_add_after(in dm_list_node_st *pstPrev, in dm_list_node_st *pstInst);
static inline void dm_list_add_before(in dm_list_node_st *pstNext, in dm_list_node_st *pstInst);
static inline void dm_list_del_node(in const dm_list_node_st *pstNode);
static inline void dm_list_add_head(in dm_list_st *pstList, in dm_list_node_st *pstNode);
static inline dm_list_node_st *dm_list_del_head(in const dm_list_st *pstList);
static inline void dm_list_add_tail(in dm_list_st *pstList, in dm_list_node_st *pstNode);
static inline dm_list_node_st *dm_list_del_tail(in const dm_list_st *pstList);
static inline dm_list_node_st *dm_list_first(in const dm_list_st *pstList);
static inline dm_list_node_st *dm_list_last(in const dm_list_st *pstList);
static inline bool_t dm_list_is_end(in const dm_list_st *pstList,
                                    in const dm_list_node_st *pstNode);
static inline void dm_list_move(in dm_list_st *pstDstList, in dm_list_st *pstSrcList);
static inline dm_list_node_st *dm_list_prev(in const dm_list_node_st *pstNode);
static inline dm_list_node_st *dm_list_next(in const dm_list_node_st *pstNode);
static inline void dm_list_free_all(in dm_list_st *pstList, in void (*pfFree)(void *));

#define DM_LIST_FOREACH(pstList, pstNode)                                                          \
  for ((pstNode) = (pstList)->stHead.pstNext; ((pstNode) != &((pstList)->stHead));                 \
       (pstNode) = dm_list_next(pstNode))

#define DM_LIST_FOREACH_SAFE(pstList, pstNode, pstNextNode)                                        \
  for ((pstNode) = (pstList)->stHead.pstNext, (pstNextNode) = dm_list_next(pstNode);               \
       ((pstNode) != &((pstList)->stHead));                                                        \
       (pstNode) = (pstNextNode), (pstNextNode) = dm_list_next(pstNextNode))

#define DM_LIST_FOREACH_REVERSE(pstList, pstNode)                                                  \
  for ((pstNode) = dm_list_last(pstList); (true != dm_list_is_end(pstList, pstNode));              \
       (pstNode) = dm_list_prev(pstNode))

#define DM_LIST_FOREACH_REVERSE_SAFE(pstList, pstNode, pstPrev)                                    \
  for ((pstNode) = dm_list_last(pstList), (pstPrev) = dm_list_prev(pstNode);                       \
       (true != dm_list_is_end(pstList, pstNode));                                                 \
       (pstNode) = (pstPrev), (pstPrev) = dm_list_prev(pstPrev))

#define DM_LIST_FIRST_ENTRY(pstList, type, member)                                                 \
  (DM_LIST_ENTRY(dm_list_first(pstList), type, member))

#define DM_LIST_LAST_ENTRY(pstList, type, member)                                                  \
  (DM_LIST_ENTRY(dm_list_last(pstList), type, member))

#define DM_LIST_NEXT_ENTRY(pstList, type, member)                                                  \
  (DM_LIST_ENTRY(dm_list_first(pstList), type, member))

#define DM_LIST_FOREACH_ENTRY(pstList, pstNode)                                                    \
  for ((pstNode) = (pstList)->stHead.pstNext; ((pstNode) != &((pstList)->stHead));                 \
       (pstNode) = dm_list_next(pstNode))

static inline void dm_list_init(in dm_list_st *pstList)
{
  pstList->stHead.pstNext = &pstList->stHead;
  pstList->stHead.pstPrev = &pstList->stHead;
  return;
}

static inline void dm_list_node_init(in dm_list_node_st *pstNode)
{
  pstNode->pstNext = (dm_list_node_st *)NULL;
  pstNode->pstPrev = (dm_list_node_st *)NULL;
  return;
}

static inline bool_t dm_list_is_empty(in const dm_list_st *pstList)
{
  return (pstList->stHead.pstNext == &pstList->stHead);
}

static inline void dm_list_add_after(in dm_list_node_st *pstPrev, in dm_list_node_st *pstInst)
{
  pstInst->pstPrev = pstPrev;
  pstInst->pstNext = pstPrev->pstNext;
  pstPrev->pstNext = pstInst;
  pstInst->pstNext->pstPrev = pstInst;
  return;
}

static inline void dm_list_add_before(in dm_list_node_st *pstNext, in dm_list_node_st *pstInst)
{
  pstInst->pstPrev = pstNext->pstPrev;
  pstInst->pstNext = pstNext;
  pstNext->pstPrev->pstNext = pstInst;
  pstNext->pstPrev = pstInst;
  return;
}

static inline void dm_list_del_node(in const dm_list_node_st *pstNode)
{
  pstNode->pstPrev->pstNext = pstNode->pstNext;
  pstNode->pstNext->pstPrev = pstNode->pstPrev;
  return;
}

static inline void dm_list_add_head(in dm_list_st *pstList, in dm_list_node_st *pstNode)
{
  dm_list_add_after(&pstList->stHead, pstNode);
  return;
}

static inline dm_list_node_st *dm_list_del_head(in const dm_list_st *pstList)
{
  dm_list_node_st *pstNode = dm_list_first(pstList);

  if (dm_list_is_end(pstList, pstNode))
  {
    pstNode = (dm_list_node_st *)NULL;
  }
  else
  {
    dm_list_del_node(pstNode);
  }
  return pstNode;
}

static inline void dm_list_add_tail(in dm_list_st *pstList, in dm_list_node_st *pstNode)
{
  dm_list_add_before(&pstList->stHead, pstNode);
  return;
}

static inline dm_list_node_st *dm_list_del_tail(in const dm_list_st *pstList)
{
  dm_list_node_st *pstNode = dm_list_last(pstList);

  if (dm_list_is_end(pstList, pstNode))
  {
    pstNode = (dm_list_node_st *)NULL;
  }
  else
  {
    dm_list_del_node(pstNode);
  }
  return pstNode;
}

static inline dm_list_node_st *dm_list_first(in const dm_list_st *pstList)
{
  dm_list_node_st *pstNode = pstList->stHead.pstNext;
  if (pstNode == &(pstList->stHead))
  {
    return (dm_list_node_st *)NULL;
  }
  return pstNode;
}

static inline dm_list_node_st *dm_list_last(in const dm_list_st *pstList)
{
  dm_list_node_st *pstNode = pstList->stHead.pstPrev;
  if (pstNode == &(pstList->stHead))
  {
    return (dm_list_node_st *)NULL;
  }
  return pstNode;
}

static inline bool_t dm_list_is_end(in const dm_list_st *pstList, in const dm_list_node_st *pstNode)
{
  if (dm_list_is_empty(pstList))
  {
    return true;
  }
  if (NULL == pstNode)
  {
    return true;
  }

  return (pstNode == &(pstList->stHead));
}

static inline dm_list_node_st *dm_list_prev(in const dm_list_node_st *pstNode)
{
  return (pstNode->pstPrev);
}

static inline dm_list_node_st *dm_list_next(in const dm_list_node_st *pstNode)
{
  return (pstNode->pstNext);
}

static inline void dm_list_move(in dm_list_st *pstDstList, in dm_list_st *pstSrcList)
{
  if (!dm_list_is_empty(pstSrcList))
  {
    if (dm_list_is_empty(pstDstList))
    {
      // If destination list is empty, just copy the pointers
      pstDstList->stHead.pstNext = pstSrcList->stHead.pstNext;
      pstDstList->stHead.pstPrev = pstSrcList->stHead.pstPrev;
      pstSrcList->stHead.pstNext->pstPrev = &pstDstList->stHead;
      pstSrcList->stHead.pstPrev->pstNext = &pstDstList->stHead;
    }
    else
    {
      // If destination list is not empty, insert source list nodes at the end
      pstSrcList->stHead.pstNext->pstPrev = pstDstList->stHead.pstPrev;
      pstDstList->stHead.pstPrev->pstNext = pstSrcList->stHead.pstNext;
      pstDstList->stHead.pstPrev = pstSrcList->stHead.pstPrev;
      pstSrcList->stHead.pstPrev->pstNext = &pstDstList->stHead;
    }
    dm_list_init(pstSrcList);
  }
  return;
}

static inline void dm_list_free_all(in dm_list_st *pstList, in void (*pfFree)(void *))
{
  if (!dm_list_is_empty(pstList))
  {
    dm_list_node_st *pstNode;
    dm_list_node_st *pstNextNode;

    DM_LIST_FOREACH_SAFE(pstList, pstNode, pstNextNode)
    {
      pfFree(pstNode);
    }
  }

  dm_list_init(pstList);
  return;
}

#ifdef __cplusplus
}
#endif

#endif // DM_LIST_H
