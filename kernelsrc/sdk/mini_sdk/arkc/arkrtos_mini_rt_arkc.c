/**
* @file arkrtos_mini_rt_arkc.c
* @brief 小程序 SDK 的 arkc 运行时：POSIX 控制台写出 + 线程退出
* @details 这个小程序为什么需要它，而 Keil / GCC 的小程序不需要：
*
*          Keil / GCC 的小程序把 `sdk/app_sdk/arkrtos_oslib.c` 链接进来，
*          那份门面把内核的每一个服务都封成 SVC 调用。arkc 编得动它（实测
*          17 KB 对象），但小程序是**整份负载搬进 RAM 执行**的，把一个用不到
*          的服务门面整块塞进 8 KB 代码块里，剩下的空间就不够写程序了。
*
*          所以这里只保留小程序真正需要的两个服务，SVC 号与 oslib 逐位相同
*          （不是新 ABI，只是同一张表上的两条）：
*
*            0x1A 子命令 3  arkrtos_shell_print(const char *msg)   控制台
*            0x1B 子命令 3  arkrtos_thread_exit(void)              线程退出
*
*          对照点：`kernelsrc/sdk/app_sdk/arkrtos_oslib.c` 是这两条的唯一
*          权威实现，本文件只是它们在 arkc + 小程序场景下的最小副本。两边
*          的 SVC 号若有一天不一致，两个文件都会在同一处被改到（这里和那里
*          都引用了同样的服务号）。
*
*          【为什么 write 要拷进缓冲】
*          SVC 0x1A 的控制台接口收的是 NUL 结尾的字符串，而 POSIX 的
*          write(fd, buf, len) 允许任意缓冲、任意长度。这里按需分块拷贝并
*          补 '\0'，所以调用方可以传非字符串的内存。已知限制写在下面。
*
* @note 只给 arkc 用。Keil / GCC 的小程序请用 app_sdk 的 oslib。
*/

/* 与 oslib 相同的两个服务，写成 arkc 认得的 __svc 形式。 */
typedef unsigned int u32;

int __svc(0x1A) arkrtos_call_shell_svc(u32 *p);
int __svc(0x1B) arkrtos_call_thread_ctrl(u32 *p);

/* 一次拷多少字节进缓冲。控制台是给人看的，96 字节够一行；大块输出会被
 * 拆成多次 SVC，每次都是完整的提示输出，不会与别的任务交错到半个字。 */
#define MINI_RT_CHUNK   96

static char g_chunk[MINI_RT_CHUNK];

/**
* @brief POSIX write：fd 1/2 写控制台，其余返回 -1
* @return 实际交给内核的字节数（= len），失败 -1
* @note 已知限制：控制台接口按 NUL 结尾字符串输出，所以负载里出现 0 字节时
*       只有它之前的部分会出来。往控制台打二进制本来就没有意义，这里不假装
*       支持（不转义、不拆分），如实记在文档里。
*/
int write(int fd, const void *buf, unsigned int len)
{
    const char  *p    = (const char *)buf;
    unsigned int done = 0u;
    u32          q[3];

    if ((fd != 1) && (fd != 2))
    {
        return -1;
    }
    if (buf == (const void *)0)
    {
        return -1;
    }

    while (done < len)
    {
        unsigned int n = len - done;
        unsigned int i;

        if (n > (MINI_RT_CHUNK - 1u))
        {
            n = MINI_RT_CHUNK - 1u;
        }
        for (i = 0u; i < n; i++)
        {
            g_chunk[i] = p[done + i];
        }
        g_chunk[n] = 0;

        q[0] = 3u; q[1] = (u32)g_chunk; q[2] = 0u;
        (void)arkrtos_call_shell_svc(q);

        done += n;
    }

    return (int)len;
}

/**
* @brief POSIX _exit：小程序没有进程，交还自己占的两块 RAM
* @note 与 app_sdk 的 arkrtos_thread_exit() 是同一条 SVC。MiniMain() 正常
*       返回时由 SDK 的 main() 调用它，程序里显式调用也合法（提前结束）。
*/
void _exit(int code)
{
    u32 q[3];

    (void)code;
    q[0] = 3u; q[1] = 0u; q[2] = 0u;
    (void)arkrtos_call_thread_ctrl(q);

    /* 内核不会再调度这个任务；真回来了就停住，不往前跑到别人的代码上。 */
    for (;;)
    {
    }
}

/**
* @brief App SDK 的退出接口，供 sdk/mini_sdk/arkrtos_mini_main.c 使用
* @note 那个文件按 SDK 约定调 arkrtos_thread_exit()。Keil / GCC 的小程序由
*       oslib 提供它；arkc 的小程序由本文件提供，语义完全相同。
*/
void arkrtos_thread_exit(void)
{
    _exit(0);
}
