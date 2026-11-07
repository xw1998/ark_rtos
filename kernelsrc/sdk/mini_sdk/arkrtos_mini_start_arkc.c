/**
* @file arkrtos_mini_start_arkc.c
* @brief Mini SDK 启动入口 —— arkc（Ark C Compiler）版
* @details 同一个入口，三套工具链各有各的写法，原因在「谁来搬 .data / 清 .bss」：
*
*            - Keil(armasm) `arkrtos_mini_start.s` 只跳到 ARM C 库的 `__main`，
*              由它做 scatter loading 再调 main()。
*            - GCC `arkrtos_mini_start_gcc.s` 自己按链接脚本给的
*              _sidata/_sdata/_edata/_sbss/_ebss 搬一遍。
*            - 本文件给 arkc 用：arkc 不生成数据段加载表，它把「全局初值」
*              直接编成一个函数 `__arkcc_init_globals()`（MOVW/MOVT + STR 逐条
*              写初值、然后清零 .bss），所以这里要做的就是调用它。
*
*          【为什么必须调 __arkcc_init_globals】
*          arkc 只在 `Reset_Handler` 的函数序言里自动 `BL __arkcc_init_globals`。
*          小程序没有复位向量（它是被内核当任务起来的），入口叫 MINISTART，
*          因此不调它的话全局变量会是 RAM 里的残留值——这类错误在板上表现为
*          「有时对有时错」，是最难查的一种。
*
*          【没有全局变量时会发生什么】
*          arkc 在 .data 与 .bss 都为空时**不生成**该函数，此时本文件会链接失败
*          并报 `undefined symbol __arkcc_init_globals`。这是有意的：宁可链接
*          期报错，也不要运行期带着未初始化的全局跑。工程里如果确实没有全局，
*          在任一源文件里放一个带非零初值的锚点变量即可。
*
*          【入口偏移的约定】
*          打包工具把本文件作为第一个链接输入，因此 MINISTART 落在负载偏移 0，
*          镜像头的 entry_offset 写 0。打包时会校验负载前两条指令是本函数固定
*          的序言（PUSH {r5, r6, r7, lr}，机器码 0xB5E0），不符就拒绝出镜像——不猜入口。
*
* @note 只给 arkc 用，不要登记进 Keil / GCC 工程（会与各自的启动文件重复定义
*       MINISTART / main）。
*/

/* 由 arkc 生成（见上）；没有 .data/.bss 时不存在，届时本文件链接失败即为预期。 */
extern void __arkcc_init_globals(void);

/* 工程自己定义的标准 C 入口（与 Linux 上一模一样）。arkc 档不提供 main()：
 * 没定义就在链接期报 `undefined symbol "main"`，不给"空程序也能跑"的假象。 */
extern void arkrtos_thread_exit(void);
extern int main(void);

void MINISTART(void)
{
    __arkcc_init_globals();

    /* main() 是标准入口：它 return 就是程序结束，由这里收尾。 */
    (void)main();
    arkrtos_thread_exit();

    /* thread_exit() 不会返回；真回来了就停在这里：从这里 return 会回到
     * 内核给的栈顶，那是未定义行为。 */
    for (;;)
    {
    }
}
