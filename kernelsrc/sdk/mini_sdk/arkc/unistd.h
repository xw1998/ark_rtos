/**
* @file unistd.h
* @brief 小程序 SDK 的 POSIX 控制台子集（arkc 版）
* @details 这里只有一件事：让「标准 POSIX 的 hello world」原样编译。
*
*          和 `kernelsrc/sdk/posix/unistd.h` 的关系：那一份是 **App SDK**
*          的完整兼容层，`write` 会被映射到 `arkrtos_posix_write()`，后面拖着
*          文件描述符表、VFS、socket 三条链（`arkrtos_posix.c` 3600 行）。小程序
*          的整份负载要搬进 RAM 执行，装不下那一整套，所以 arkc 的小程序用
*          这一份：名字与语义都是 POSIX 的，实现只有两条 SVC（见
*          `arkrtos_mini_rt_arkc.c`）。
*
*          用哪一份由工程决定（`-I` 指到哪个目录）。两份同名头不会同时可见：
*          本目录（`mini_sdk/arkc`，这里放同名头）只给 arkc 小程序工程用。
*
*          没有的东西也列清楚，免得调用方以为「编过了就是支持」：
*            - 没有 stdio（printf/fopen）：arkc 没有 va_list，也没有 C 库；
*              要输出就用 write()。
*            - 没有 read/open/close：小程序目前拿不到 stdin，也没有文件描述符表。
*            - 没有 fork/exec：小程序自己就是一个任务。
*/

#ifndef __ARKRTOS_MINI_UNISTD_H__
#define __ARKRTOS_MINI_UNISTD_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 标准流号。POSIX 里 fd 1 = stdout、2 = stderr，这里两者都接到控制台：
 * 板子只有一个串口，假装有两个目的地才是骗人。 */
#define STDIN_FILENO    (0)
#define STDOUT_FILENO   (1)
#define STDERR_FILENO   (2)

/**
* @brief 把 buf 里的 len 个字节写到标准流
* @param fd 1 或 2（都写同一个控制台）；其它值返回 -1
* @return 交给内核的字节数（= len），失败 -1
* @note 控制台接口收 NUL 结尾字符串，负载里的 0 字节会截断这一块输出；
*       往终端打二进制不在支持范围内（见 arkrtos_mini_rt_arkc.c 的说明）。
*/
int write(int fd, const void *buf, unsigned int len);

/**
* @brief 结束本小程序，把代码块与 RAM 块交还内核
* @note 等价于 MiniMain() 正常返回；调用后不会再回到本程序。
*/
void _exit(int code);

#ifdef __cplusplus
}
#endif

#endif /* __ARKRTOS_MINI_UNISTD_H__ */
