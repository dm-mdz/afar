/**
 ******************************************************************************
 * @file    check.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   线程检查（详设 1.7、2.9）。
 *
 * 业务模块：快接口第一行写 DM_ASSERT_LOOP();，慢接口第一行写 DM_ASSERT_SLOW();。
 *   调试版调错线程当场 abort 并打出位置；发布版是空宏。
 * DM 自己只许主线程调的接口不用这两个宏，而是每次比一下线程号，调错回 DM_WRONG_THREAD
 *   并记 ERROR，调试版和发布版一样（见 dm_internal.h 的 DM__CHECK_LOOP）。
 ******************************************************************************
 */
#ifndef DM_CHECK_H
#define DM_CHECK_H

#include <dm.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 当前线程是不是主线程（跑事件循环的那个）。dm_main_ex 开头记下主线程 */
DM_API bool_t dm_in_loop(void);

/* 是不是还在 init 阶段（各模块的 pfInit 正在跑）：这时还没进循环，允许在主线程读库 */
DM_API bool_t dm_in_init(void);

/* 把当前线程记成主线程。dm_main_ex 自己调；不经 dm_main_ex 的白盒测试可以自己调 */
DM_API void dm_check_bind_main(void);

#if defined(NDEBUG)
#define DM_ASSERT_LOOP() ((void)0)
#define DM_ASSERT_SLOW() ((void)0)
#else
#define DM_ASSERT_LOOP() DMASSERT(dm_in_loop())
#define DM_ASSERT_SLOW() DMASSERT((!dm_in_loop()) || dm_in_init())
#endif

#ifdef __cplusplus
}
#endif

#endif /* DM_CHECK_H */
