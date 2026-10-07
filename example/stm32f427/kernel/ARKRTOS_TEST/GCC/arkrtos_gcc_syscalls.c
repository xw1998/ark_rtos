/*
 * Ark RTOS — GCC/Make 工程专用的 C 库桩（仅 GCC 侧需要，Keil 侧不用）
 * ===================================================================
 * 背景：
 *   newlib-nano 的 stdio 初始化链（findfp -> _malloc_r -> _sbrk_r -> _sbrk）
 *   会把 `_sbrk` 拉进镜像。libnosys.a 里的 `_sbrk` **没有任何边界检查**
 *   （反汇编确认：第一次调用用 `end` 初始化堆指针，之后只做 heap_end += incr
 *   并返回旧值），一旦真的被调用就会一路向上长、踩穿栈与其它数据。
 *
 *   内核本身不提供 C 库堆（见链接脚本 _Min_Heap_Size = 0），也不提供文件
 *   系统系统调用。这里就地给出一个**恒定失败**的 `_sbrk`：本文件里的强符号
 *   会先于 libnosys.a 的 sbrk.o 满足 `_sbrk_r`，于是归档成员不会被拉入，
 *   既消掉 `undefined reference to 'end'`，也保证任何 malloc 都返回 NULL，
 *   与 Keil 侧「无堆」语义一致。
 *
 * 维护：本文件是 GCC 工程独有，不要登记进 Keil 工程。
 */
#include <errno.h>
#include <stddef.h>

void *_sbrk(ptrdiff_t incr)
{
    (void)incr;
    errno = ENOMEM;
    return (void *)-1;
}
