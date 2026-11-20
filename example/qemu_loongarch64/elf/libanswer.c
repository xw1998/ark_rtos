/**
* @file libanswer.c
* @brief 动态链接夹具里的"共享库"（QEMU LoongArch64）
* @details 它被 tools/elf/dynfixture.py 封装成一个带 .dynsym / .rela 的
*          ET_DYN 镜像——本机工具链的 ld 2.31.1 不支持 -shared，所以动态
*          元数据由 Python 构建器按清单生成。这里只放"要被重定位的东西"：
*
*            g_base        导出的数据符号（值 100）
*            g_ptr         一个指针槽，靠 R_LARCH_RELATIVE 在装载时被写成
*                          &g_base（链接期的值是 0，不是链接器算出来的）
*            abs_ptr       一个指针槽，靠 R_LARCH_64 在装载时被写成 &g_base
*            got_answer    一个调用槽，靠 R_LARCH_JUMP_SLOT 被写成
*                          answer_add 的运行期地址
*            lib_g_addr()  用 PC 相对寻址现算 &g_base，作为"真值"与上面
*                          三条重定位的结果对照：只有重定位真的发生过，
*                          它们才会相等
*
* @note 这里没有任何 libc 调用；volatile 是为了不让 -O2 把槽读成常量。
* @author xw
*/

volatile long g_base = 100;
volatile long *g_ptr = 0;
long *abs_ptr = 0;
long got_answer = 0;

long answer_add(long x)
{
    return x + g_base;
}

long *lib_g_addr(void)
{
    return (long *)&g_base;
}
