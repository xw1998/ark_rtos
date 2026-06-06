#ifndef __ARK_TYPES_H__
#define __ARK_TYPES_H__

#include <stdint.h>
#include <stddef.h>

typedef signed   char                   ark_int8_t; 
typedef signed   short                  ark_int16_t; 
typedef signed   long                   ark_int32_t; 
typedef unsigned char                   ark_uint8_t; 
typedef unsigned short                  ark_uint16_t; 
typedef unsigned long                   ark_uint32_t; 
typedef int                             ark_bool_t; 

/* 32bit CPU */ 
typedef long                            ark_base_t; 
typedef unsigned long                   ark_ubase_t; 

typedef ark_base_t                      ark_err_t; 
typedef ark_uint32_t                    ark_time_t; 
typedef ark_uint32_t                    ark_tick_t; 
typedef ark_base_t                      ark_flag_t; 
typedef ark_ubase_t                     ark_size_t; 
typedef ark_ubase_t                     ark_dev_t; 
typedef ark_base_t                      ark_off_t; 

/* 布尔数据类型重定义 */ 
#define ARK_TRUE                         1 
#define ARK_FALSE                        0

#ifdef __CC_ARM
		#define ark_inline                   static __inline
		#define ALIGN(n)                    __attribute__((aligned(n)))

#elif defined (__IAR_SYSTEMS_ICC__)
    #define ark_inline                   static inline
		#define ALIGN(n)                    PRAGMA(data_alignment=n)
		
#elif defined (__GNUC__)
    #define ark_inline                   static __inline
		#define ALIGN(n)                    __attribute__((aligned(n)))
#else
    #error not supported tool chain			
#endif

#define ARK_ALIGN(size, align)           (((size) + (align) - 1) & ~((align) - 1))
#define ARK_ALIGN_DOWN(size, align)      ((size) & ~((align) - 1))
		
#define ARK_NULL                         (0)

enum ark_err_t
{
    ARK_ERR_OK = 0,//成功
    ARK_ERR_INVALID = 1,//无效参数
    ARK_ERR_MEMORY = 2,//内存不足
    ARK_ERR_TIMEOUT = 3,//超时
    ARK_ERR_TASK = 4,//任务不存在
    ARK_ERR_STACK = 5,//栈不足
    ARK_ERR_TASK_EXIST = 6,//任务已存在
};

#endif
