/**
* @file arkrtos_modld.h
* @brief VxWorks 式可下载模块：把 ET_REL 目标文件在板上现场链接再跑起来
* @details 与 arkrtos_elf.h 是**两种应用模型**，不是同一件事的两步：
*
*          |            | Linux 式（exec）        | VxWorks 式（ld）           |
*          |------------|-------------------------|----------------------------|
*          | 输入       | ET_EXEC 可执行文件      | ET_REL 目标文件（.o）      |
*          | 地址       | 链接期定死（-Ttext）    | 装载时决定（任意空槽）     |
*          | 外部引用   | 程序自负（静态可执行）  | 对内核对导出表，按名字解析 |
*          | 特权级     | PLV3（隔离）            | PLV0（内核态，共享地址空间）|
*          | 生态       | 标准工具链链出来的程序  | 内核的插件（驱动/诊断）    |
*
*          **这一档等于放弃 PLV3 隔离**：模块必须在内核态才能直接按名字调内核
*          函数，这是 VxWorks 5.x 的老模型，不是 Linux 的进程模型。开关默认关，
*          打开的那一行要自己认这个取舍。
*
*          支持的重定位类型（LoongArch64，GCC 8.3 只发 SOP 栈机形式；
*          类型号按 LoongArch ELF ABI 表，且都用汇编器实测核对过指令字）：
*            22 PUSH_PCREL / 23 PUSH_ABSOLUTE / 25 PUSH_GPREL / 29 PUSH_PLT_PCREL
*            30 ASSERT / 31 NOT / 32 SUB / 33 SL / 34 SR / 35 ADD
*            40 POP_32_S_10_12（addi 的 12 位立即数，bits[21:10]）
*            42 POP_32_S_10_16_S2（B16 分支，offs>>2 在 bits[25:10]）
*            43 POP_32_S_5_20（pcalau12i 的 20 位，bits[24:5]）
*            45 POP_32_S_0_10_10_16_S2（B/BL 26 位：bits[25:10] 放低 16 位、
*               bits[9:0] 放高 10 位——字段是交叉的，写成连续 26 位会跳错地方）
*            46 POP_32_U / 1 R_LARCH_32 / 2 R_LARCH_64
*          其余类型一律**报错并点出类型号**，不猜编码——猜错会得到一个能跑
*          但算错的模块，比装载失败难查得多。特别注意 44
*          （POP_32_S_0_5_10_16_S2）的字段布局没能核实，所以故意不认它。
*
* @note 开关：ARKRTOS_USE_MODLOAD（默认 0）。关闭时本文件只剩宏，
*       整块零体积零符号。打开时要求板级给出 ARKRTOS_MOD_AREA_SIZE（窗口地址由链接器给池子），
*       缺一个是编译期 #error。
*
* @warning **调用 arkrtos_mod_load() 的任务要给足栈**：装载一次要读 ELF、解析节、
*          拷段、逐条做重定位、逐个查符号，示例实测峰值 3080 B（LoongArch64，
*          modhello.o / 165 条重定位）。按 8 KB 给，别卡在 2 KB；
*          也别让它和别的任务的栈在链接布局上贴在一起（溢出会打穿别人的保存帧，
*          症状是「模块跑完一会儿才 FATAL」）。shell 里敲 ld 时用的是 shell 任务栈。
* @author xw
*/

#ifndef __ARKRTOS_MODLD_H__
#define __ARKRTOS_MODLD_H__

#include "arkrtos_types.h"

#if (ARKRTOS_USE_MODLOAD == 1)

/* 返回码：0 成功，负值失败。名字经 arkrtos_mod_error_name() 换成人话。 */
#define ARKRTOS_MOD_OK              (0)
#define ARKRTOS_MOD_ERR_PARAM       (-1)    /* 空路径 / 空 argv 元素 / 参数越界 */
#define ARKRTOS_MOD_ERR_FS          (-2)    /* VFS 打不开、读不全、文件超过槽容量 */
#define ARKRTOS_MOD_ERR_FORMAT      (-3)    /* 不是 ELF64 ET_REL / 节头表坏 */
#define ARKRTOS_MOD_ERR_ARCH        (-4)    /* e_machine 不是本架构 */
#define ARKRTOS_MOD_ERR_LAYOUT      (-5)    /* 节放不进槽 / 越界 */
#define ARKRTOS_MOD_ERR_SYMBOL      (-6)    /* 外部符号不在导出表里（会点名） */
#define ARKRTOS_MOD_ERR_RELOC       (-7)    /* 不认识的重定位类型（会点类型号） */
#define ARKRTOS_MOD_ERR_NOENTRY     (-8)    /* 找不到入口符号 */
#define ARKRTOS_MOD_ERR_NOMEM       (-9)    /* 槽位满 / 任务表满 */
#define ARKRTOS_MOD_ERR_BUSY        (-10)   /* 该槽里的模块还在跑，不能卸 */
#define ARKRTOS_MOD_ERR_SIGN        (-11)   /* 缺尾部签名 tag / tag 不匹配（EXEC_SIGN 档） */

/** @brief 一次装载的可读结果（全部是实测值，不是推算值） */
typedef struct {
    char   name[24];        /**< 模块名（取自文件名，去掉目录与后缀） */
    uint32 base;            /**< 模块在 RAM 里的起始地址 */
    uint32 size;            /**< 实际占用字节数（含 GOT） */
    uint32 entry;           /**< 入口地址（定位后的绝对地址） */
    uint32 text_bytes;      /**< 从 .o 里搬进来的字节数（不含 BSS/GOT） */
    uint32 relocs;          /**< 应用成功的重定位条数 */
    uint32 imports;         /**< 解析成功的外部符号引用次数 */
    int32  task;            /**< 起跑的任务号；0 = 已装未跑（或已结束） */
    int32  state;           /**< 1 = 驻留；0 = 空槽（列表里不出现） */
} arkrtos_mod_info_t;

/**
* @brief 装载一个 ET_REL 模块并起跑它的入口
* @param path  VFS 路径（如 "/mnt/ram/modhello.o"）
* @param argc  传给 module_main 的参数个数（含 argv[0]）；0 表示自动补 1 个
* @param argv  参数数组；0 时按 argc=0 处理
* @return 0 = 已装并起跑；负值 = ARKRTOS_MOD_ERR_*
* @note 起跑后立刻返回（不等待模块结束）。要等结束由调用者按 task 号看。
*/
int32 arkrtos_mod_load(const char *path, int32 argc, char *argv[]);

/** @brief 当前驻留的模块个数 */
int32 arkrtos_mod_count(void);

/** @brief 取第 index 个驻留模块（index 是「第几个」，不是槽号） @return 0/-1 */
int32 arkrtos_mod_info(int32 index, arkrtos_mod_info_t *p_out);

/** @brief 卸掉一个模块（按装载顺序的下标） @return 0 / ARKRTOS_MOD_ERR_* */
int32 arkrtos_mod_unload(int32 index);

/** @brief 返回码的短名（只用于打印，不参与判定） */
const char *arkrtos_mod_error_name(int32 rc);

/**
* @brief 最近一次装载失败缺失的外部符号名
* @return 符号名；空串表示上次失败不是符号问题
* @note 只在 ARKRTOS_MOD_ERR_SYMBOL 之后有意义，其余情况返回空串。
*/
const char *arkrtos_mod_missing_symbol(void);

/**
* @brief 最近一次装载失败遇到的重定位类型号
* @return LoongArch 重定位类型号；0 表示上次失败不是重定位问题
*/
uint32 arkrtos_mod_bad_reloc(void);

#else  /* ARKRTOS_USE_MODLOAD == 0 */

/* 关闭档：调用点不必再套 #if，且每个入口都回答「没有这件事」。 */
#define arkrtos_mod_load(path, argc, argv)      ((int32)(-1))
#define arkrtos_mod_count()                    ((int32)0)
#define arkrtos_mod_info(index, out)           ((int32)(-1))
#define arkrtos_mod_unload(index)              ((int32)(-1))
#define arkrtos_mod_error_name(rc)             ("module loader not compiled in")
#define arkrtos_mod_missing_symbol()           ("")
#define arkrtos_mod_bad_reloc()                ((uint32)0)

#endif /* ARKRTOS_USE_MODLOAD */

#endif /* __ARKRTOS_MODLD_H__ */
