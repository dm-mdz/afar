/**
 ******************************************************************************
 * @file    db.h
 * @author  MDM
 * @version 0.0.0.1
 * @brief   数据库（详设 2.15）：SQLite 封装。
 *
 *          一个线程一条连接：每个线程第一次调 dm_db_get 时打开，以后一直复用。所以一个 work
 *          里调到的几个慢接口拿到的是同一条连接，可以放进同一个事务。事务只由处理项的 work 开，
 *          慢接口自己不开，只在传进来的连接上读写。一个事务里不联网、不等别的东西。
 *          处理项的 work 返回时还在事务里：DM 回滚并记 ERROR（兜底）。直接用 dm_queue_work
 *          投到线程池的活不管，事务自己收尾。
 *
 *          dm_db_get 只许在 work 线程和 init 阶段调；进了事件循环以后在主线程调：
 *          调试版断言失败，发布版返回 NULL 并记 ERROR。没配 pcDbPath 时返回 NULL。
 *
 *          打开时设：WAL；synchronous=NORMAL（进程被强杀不丢已提交的；整机掉电可能丢最后几笔，
 *          库不会坏）；外键开；忙等 5 秒。
 ******************************************************************************
 */
#ifndef DM_DB_H
#define DM_DB_H

#include <dm.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct dm_db dm_db_st;     /* 一条连接，不透明 */
typedef struct dm_stmt dm_stmt_st; /* 一条缓存着的语句，不透明 */

#define DM_DB_ROW 1 /* dm_db_step：有一行可读（DM_SUCCESS = 走完了） */

/* 当前线程的连接 */
DM_API dm_db_st *dm_db_get(void);

/* 要写的事务：BEGIN IMMEDIATE，一开始就拿写锁。已经在事务里：调试版断言失败，发布版回 DM_BUSY */
DM_API int dm_db_begin(dm_in dm_db_st *pstDb);
/* 只读的事务：几条查询要看到同一时刻的数据时用。不拿写锁，不挡别人写 */
DM_API int dm_db_begin_ro(dm_in dm_db_st *pstDb);
/* 提交；失败时已经回滚 */
DM_API int dm_db_commit(dm_in dm_db_st *pstDb);
DM_API void dm_db_rollback(dm_in dm_db_st *pstDb);
/* 跑一段 SQL（可以几条，用分号隔开），不取结果 */
DM_API int dm_db_exec(dm_in dm_db_st *pstDb, dm_in const char *pcSql);

/* 按 SQL 文本缓存：同一条连接上同一段文本拿到同一个语句，拿到时已经 reset、参数已清。
 * 所以一条语句还在逐行读（dm_db_step）时，别再 prep 同一段文本：拿到的是同一个语句，外面那层
 * 会被 reset。要嵌套查同一张表，把 SQL 写得不一样（比如换个别名）*/
DM_API int dm_db_prep(dm_in dm_db_st *pstDb, dm_in const char *pcSql, dm_out dm_stmt_st **ppstStmt);

/* 绑参数：下标从 1 起，和 SQLite 一样。文本 iLen < 0 表示到 '\0' 为止；都会拷一份 */
DM_API int dm_db_bind_int(dm_in dm_stmt_st *pstStmt, dm_in int iIdx, dm_in int iVal);
DM_API int dm_db_bind_int64(dm_in dm_stmt_st *pstStmt, dm_in int iIdx, dm_in int64 i64Val);
DM_API int dm_db_bind_text(dm_in dm_stmt_st *pstStmt, dm_in int iIdx, dm_in const char *pcText,
                           dm_in int iLen);
DM_API int dm_db_bind_blob(dm_in dm_stmt_st *pstStmt, dm_in int iIdx, dm_in const void *pvData,
                           dm_in uint32 u32Len);
DM_API int dm_db_bind_null(dm_in dm_stmt_st *pstStmt, dm_in int iIdx);

/* 走一步：DM_DB_ROW 有一行；DM_SUCCESS 走完了；负数出错 */
DM_API int dm_db_step(dm_in dm_stmt_st *pstStmt);

/* 读列：下标从 0 起。文本、二进制的指针到下一次 step / reset 之前有效；
 * 空值：col_text 回 NULL，col_blob 回 NULL 且长度 0，整数回 0 */
DM_API int dm_db_col_int(dm_in dm_stmt_st *pstStmt, dm_in int iCol);
DM_API int64 dm_db_col_int64(dm_in dm_stmt_st *pstStmt, dm_in int iCol);
DM_API const char *dm_db_col_text(dm_in dm_stmt_st *pstStmt, dm_in int iCol);
DM_API const void *dm_db_col_blob(dm_in dm_stmt_st *pstStmt, dm_in int iCol,
                                  dm_out uint32 *pu32Len);

/* 用完一条语句（还要留在缓存里）：reset 并清参数 */
DM_API void dm_db_reset(dm_in dm_stmt_st *pstStmt);

/*
 * 表结构升级：模块在 init 里调。ppcSteps 是一串建表、改表的 SQL，第 i 步升到版本 i+1。
 * DM 在 dm_schema(module, version) 里记每个模块升到第几步，只跑没跑过的，每一步一个事务；
 * 某一步失败：这一步整个不生效，版本停在上一步，返回错误（模块的 init 据此失败）。
 */
DM_API int dm_db_migrate(dm_in dm_db_st *pstDb, dm_in const char *pcModule,
                         dm_in const char *const *ppcSteps, dm_in uint uiCount);

/* 在线备份到 pcToPath（先写同目录的临时文件，写完改名替换）。别的线程可以照常写 */
DM_API int dm_db_backup(dm_in dm_db_st *pstDb, dm_in const char *pcToPath);

struct dm_ext_hooks;
DM_API const struct dm_ext_hooks *dm_db_hooks(void);

#ifdef __cplusplus
}
#endif

#endif /* DM_DB_H */
