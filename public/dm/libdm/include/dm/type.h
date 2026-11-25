/**
 ******************************************************************************
 * @file    type.h
 * @author  MDM
 * @version 2.1.0
 * @brief   DM 框架跨平台基础类型、参数方向标注（dm_in/dm_out/dm_io）与可见性宏定义。
 ******************************************************************************
 */
#ifndef DM_TYPE_H
#define DM_TYPE_H

#include <stddef.h> /* size_t、NULL */
#include <stdint.h> /* 定宽整数类型 int8_t/uint32_t 等,保证跨平台宽度一致 */
#ifndef __cplusplus
#include <stdbool.h> /* C99 bool/true/false;C++ 下 bool 为原生关键字 */
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/* DM_API —— 可见性宏：控制 Windows 下 DLL 符号的导入/导出装饰符 */
#ifndef DM_API
#ifdef _WIN32
#if defined(BUILDING_DM_SHARED)
#define DM_API __declspec(dllexport)
#elif defined(USING_DM_SHARED)
#define DM_API __declspec(dllimport)
#else
#define DM_API
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define DM_API __attribute__((visibility("default")))
#else
#define DM_API
#endif
#endif

/* Input/Output direction macros (空宏，仅作参数方向标注；C++20 STL 安全，绝不定义裸 in/out/io) */
#define dm_in
#define dm_out
#define dm_io

/* DZFree Code Style Type(通用类型,宽度随平台,勿用于跨平台数据布局) */
typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef signed long long longlong;
typedef unsigned long long ulonglong;
typedef unsigned char bool_t; /* 布尔,1 字节(见下方静态断言) */

#define DM_TRUE 1
#define DM_FALSE 0

/* Standard integer types —— 基于 stdint 定宽类型,跨平台宽度一致 */
typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;

/* 平台无关跨进程/Peer 原始流描述符（零 <uv.h>/<winsock2.h> 泄露，独立于内部 dm_os_fd_t） */
typedef intptr_t dm_peer_fd_t;
#define DM_PEER_FD_INVALID ((dm_peer_fd_t)(-1))

/* 编译期校验:定宽类型在目标系统/编译器上的真实宽度(保证跨 OS 数据布局一致) */
#ifdef __cplusplus
#define DM_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#else
#define DM_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif

DM_STATIC_ASSERT(sizeof(int8) == 1, "int8   must be 1 byte");
DM_STATIC_ASSERT(sizeof(int16) == 2, "int16  must be 2 bytes");
DM_STATIC_ASSERT(sizeof(int32) == 4, "int32  must be 4 bytes");
DM_STATIC_ASSERT(sizeof(int64) == 8, "int64  must be 8 bytes");
DM_STATIC_ASSERT(sizeof(uint8) == 1, "uint8  must be 1 byte");
DM_STATIC_ASSERT(sizeof(uint16) == 2, "uint16 must be 2 bytes");
DM_STATIC_ASSERT(sizeof(uint32) == 4, "uint32 must be 4 bytes");
DM_STATIC_ASSERT(sizeof(uint64) == 8, "uint64 must be 8 bytes");
DM_STATIC_ASSERT(sizeof(bool_t) == 1, "bool_t must be 1 byte");

#undef DM_STATIC_ASSERT

#ifdef __cplusplus
}
#endif

#endif // DM_TYPE_H
