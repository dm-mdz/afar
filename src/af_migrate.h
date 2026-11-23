/**
 ******************************************************************************
 * @file    af_migrate.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   老版本 Rust 0.4.12 无损幂等迁移接口（config.toml / state.json / devices.json / 旧钥匙串）。
 ******************************************************************************
 */
#ifndef AF_MIGRATE_H
#define AF_MIGRATE_H

#include <dm/type.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 可选测试注入钩子（NULL 成员回退真实 af_plat_keychain_*） */
typedef struct af_migrate_ops
{
  bool_t (*pfKeychainHasLegacyPw)(void);
  void (*pfKeychainDelLegacyPw)(void);
} af_migrate_ops_st;

void af_migrate_set_ops(in const af_migrate_ops_st *pstOps);
int af_migrate_run(in const char *pcDataDir);

#ifdef __cplusplus
}
#endif

#endif /* AF_MIGRATE_H */
