# /mnt/nor/va/makefile -- 标准 C 的变参函数，作为 Ark RTOS 小程序编译
#
# 这个工程里只有标准 C（<stdio.h> + <stdarg.h> + va_start/va_arg）——没有一句
# Ark RTOS 的东西，在 Linux 上就是 `cc va.c && ./a.out`。在这里是：
#
#   cd /mnt/nor/va
#   make
#   mini run va.svcm
#
# ---- 这份文件只认 ark make 的四个键 ----
# TARGET / OUT / SRCS / INCS（还有 DEFS）。没有规则、没有依赖、没有时间戳：
# 板上没有增量构建，每次都是全量四遍差分打包。
#
# ---- SRCS 的第一份必须是启动桩 ----
# 镜像的入口偏移是相对负载起点算的，启动桩的 MINISTART 必须落在偏移 0。
# 打包时会校验负载前两条指令是启动桩固定的序言（0xB5E0），不是就拒绝出镜像。
#
# ---- 为什么写绝对路径 ----
# 当前目录是 /mnt/nor/hello，而 SDK 的三份源在 /mnt/nor 下（上一级）。
# ark make 的一行值只有 96 字节，所以能用绝对路径就直接写绝对路径，避免
# ".." 这种要靠路径解析去猜的写法；三份源加起来 44 字节，够。
#
# ---- 为什么没有 mini_main.c ----
# arkc 档的入口是标准 C 的 int main(void)：启动桩（start.c）直接调它，它
# return 之后由桩调 arkrtos_thread_exit() 收尾。SDK 里那份「main() 调
# MiniMain()」的骨架是 Keil / GCC 档的写法，arkc 档塞进来会直接 #error。
TARGET = va
OUT    = va.svcm
SRCS   = /mnt/nor/start.c /mnt/nor/rt.c va.c
INCS   = /mnt/nor
