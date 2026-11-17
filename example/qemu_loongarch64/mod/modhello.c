/**
* @file modhello.c
* @brief VxWorks 式可下载模块示例（QEMU LoongArch64）
* @details 这不是一个可执行文件：它是一个 **ET_REL 目标文件（.o）**，里面
*          引用了三个内核函数，但**不含它们的地址**。装载器在板上做四件事：
*            1) 把 .text/.data/.rodata 摆进模块区的一个空槽；
*            2) 给每个未定义符号在导出表里找一个地址，写进 GOT；
*            3) 按 LoongArch64 的 SOP 重定位把每条 pcalau12i/ld.d 的偏移
*               算成「槽地址 - P」；
*            4) 把 module_main 当任务入口挂起来。
*
*          它要证明的三条，正好对应上面前三步：
*            - 能跑（装载器摆的位置与 PC 相对量都对）；
*            - 能按名字调内核 API（arkrtos_mod_printf / tick / delay）；
*            - 参数能从 shell 传进来（argc/argv 走的是装载器拷的静态区，
*              不是 shell 那一行还在变的行缓冲）。
*
*          链接期**没有**地址：用它证明「同一个 .o 装到哪个槽都能跑」，
*          这正是它与 hello.elf（-Ttext=0x02000000 定死）的区别。
*
* @note 模块跑在内核态（PLV0）：它能直接调内核函数，也就同样能直接改内核
*       内存。这是开关打开时认下的取舍，见 docs/MMU平台可下载模块-VxWorks式.md。
* @author xw
*/

/* 内核导出表里的三个符号。类型与 kernelsrc/include 下各头文件的原型逐字一致：
 * 类型不一致时 64 位端口上会静默截断参数，所以这里不做「差不多就行」。 */
extern int      arkrtos_mod_printf(const char *fmt, ...);
extern unsigned int arkrtos_kernel_get_tick(void);
extern void     arkrtos_task_delay_internal(unsigned int us);

/* 模块自己的数据/只读数据：这两块必须由装载器搬到槽里并按实际情况改地址，
 * 否则「能跑」只说明代码段碰巧没引用它们。 */
static int s_calls = 0;
static const char s_tag[] = "[modhello]";

int module_main(int argc, char **argv)
{
    unsigned int t0;
    unsigned int t1;
    int i;

    arkrtos_mod_printf("%s hello from a relocatable module\n", s_tag);
    arkrtos_mod_printf("%s argc=%d\n", s_tag, argc);

    for (i = 0; i < argc; i++)
    {
        arkrtos_mod_printf("%s argv[%d]=%s\n", s_tag, i,
                           (argv[i] != 0) ? argv[i] : "(null)");
    }

    /* 两次取 tick 之间真让出 CPU：读数不是 0 就说明模块能调内核的调度器，
     * 而不是自己在那儿空转。 */
    t0 = arkrtos_kernel_get_tick();
    arkrtos_task_delay_internal(20000u);    /* 单位是微秒 */
    t1 = arkrtos_kernel_get_tick();

    s_calls++;
    arkrtos_mod_printf("%s tick %u -> %u, calls=%d\n", s_tag, t0, t1, s_calls);
    arkrtos_mod_printf("%s bye\n", s_tag);

    /* 模块的返回值就是它的结论：内核在退出路径里把它转成任务退出码
     * （开档时），shell 的 wait <task id> 能读到。这里故意返回 argc
     * 而不是 0 —— 「没有码」和「码是 0」在读数上必须能分开，而 0 恰好
     * 是最容易被误当默认值的那一个。 */
    return argc;
}
