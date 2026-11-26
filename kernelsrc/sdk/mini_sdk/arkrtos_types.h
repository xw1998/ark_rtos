/**
* @brief Ark RTOS 小程序基础类型定义
* @details 与 App / 驱动 SDK 的同名文件内容一致：不含任何 MCU 头文件、
*          不含内核内部头文件。小程序只需包含 arkrtos.h 即可调用全部 OS 接口。
*/

#ifndef __ARKRTOS_TYPES_H__
#define __ARKRTOS_TYPES_H__

typedef unsigned long  uint32;
typedef signed   long  int32;
typedef unsigned short uint16;
typedef signed   short int16;
typedef unsigned char  uint8;
typedef signed   char  int8;

/* 指针宽度整数：与 kernelsrc/include/arkrtos_types.h 的同名块逐字一致
 * （sdk-stdio-host 门禁会核）。ulog 的 %p 要用它，而 App/驱动/小程序只能
 * 看到 SDK 侧的这份精简 types.h，所以这 10 行必须在每个副本里都有一份。 */
#if (defined(__SIZEOF_POINTER__) && (__SIZEOF_POINTER__ > 4)) || \
    defined(__LP64__) || defined(_LP64) || \
    (defined(__SIZEOF_LONG__) && (__SIZEOF_LONG__ > 4)) || \
    defined(_WIN64)
typedef unsigned long long arkrtos_uptr_t;
#ifndef ARKRTOS_PTR64
#define ARKRTOS_PTR64 1
#endif
#else
typedef unsigned int arkrtos_uptr_t;
#ifndef ARKRTOS_PTR64
#define ARKRTOS_PTR64 0
#endif
#endif

/* Toolchain-independent weak-symbol attribute (AC5 vs AC6/GCC/Clang). */
#if defined(__CC_ARM)
#define ARKRTOS_WEAK __weak
#else
#define ARKRTOS_WEAK __attribute__((weak))
#endif

#endif
