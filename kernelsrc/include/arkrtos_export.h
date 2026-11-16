/**
* @file arkrtos_export.h
* @brief 内核符号导出表：可下载模块在装载时按名字解析外部引用
* @details 这张表回答的是「模块能按名字调用哪些内核函数」。
*          它是**白名单**，不是「所有带导出符号的内核函数」：
*          没进表的符号，模块里的引用会在装载阶段被拒并点出名字，
*          而不是留到运行期跳到一个 0 地址。
*
*          与 VxWorks 的对照要写清楚：VxWorks 的 sysSymTbl 由链接器把
*          全量符号塞进去，规模大、查表慢；这里用一张常量数组
*          `{ 名字, 地址 }`，不改链接脚本、不加自定义段，
*          代价是「要用什么就得在这里登记一行」——这条代价是有意的：
*          每多导出一个人，就多一个人能在内核态改内核状态。
*
* @note 开关：ARKRTOS_USE_MODLOAD（默认 0）。关闭时本文件只剩宏，
*       整块零体积零符号。
* @author xw
*/

#ifndef __ARKRTOS_EXPORT_H__
#define __ARKRTOS_EXPORT_H__

#include "arkrtos_types.h"

#if (ARKRTOS_USE_MODLOAD == 1)

/** @brief 表项：名字 + 地址。地址用 unsigned long，64 位端口上不截断。 */
typedef struct {
    const char   *name;     /**< 模块里写的外部符号名（大小写敏感） */
    unsigned long addr;     /**< 装载时写进 GOT / 重定位的值 */
} arkrtos_export_t;

/**
* @brief 按名字查一个导出符号
* @param name  符号名（不是路径，不是 "&name"）
* @param p_addr 收地址；找到时写入
* @return 0 = 找到；<0 = 没有这个名字
*/
int32 arkrtos_export_lookup(const char *name, unsigned long *p_addr);

/**
* @brief 取整张表（只读）
* @param p_count 收表项个数；可为 0
* @return 表首地址；关闭档返回 0
* @note `lkup` 就是照着这张表打印的，不另存一份清单。
*/
const arkrtos_export_t *arkrtos_export_table(uint32 *p_count);

#else  /* ARKRTOS_USE_MODLOAD == 0 */

/* 关闭档：调用点不必套 #if，且每个入口都回答「没有这件事」。 */
#define arkrtos_export_lookup(name, p_addr)     ((int32)(-1))
#define arkrtos_export_table(p_count)           ((const arkrtos_export_t *)0)

#endif /* ARKRTOS_USE_MODLOAD */

#endif /* __ARKRTOS_EXPORT_H__ */
