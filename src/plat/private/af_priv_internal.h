/**
 ******************************************************************************
 * @file    af_priv_internal.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   src/plat/ 内部私有 API 适配声明（仅限 src/plat/ 内使用）。
 ******************************************************************************
 */
#ifndef AF_PRIV_INTERNAL_H
#define AF_PRIV_INTERNAL_H

#include "plat/af_plat.h"

#ifdef __cplusplus
extern "C"
{
#endif

int af_priv_cgs_set_mode(dm_in uint32 u32DispId, dm_in const af_disp_mode_st *pstWant);

#ifdef __cplusplus
}
#endif

#endif /* AF_PRIV_INTERNAL_H */
