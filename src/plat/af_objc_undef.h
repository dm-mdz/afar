/**
 ******************************************************************************
 * @file    af_objc_undef.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   解除 dm/type.h 定义的 in/out/io 空宏，避免与 ObjC for (id x in arr) 冲突。
 ******************************************************************************
 */
#ifndef AF_OBJC_UNDEF_H
#define AF_OBJC_UNDEF_H

#ifdef __OBJC__
#undef in
#undef out
#undef io
#endif

#endif /* AF_OBJC_UNDEF_H */
