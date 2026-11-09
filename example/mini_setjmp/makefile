# /mnt/nor/sj/makefile -- 标准 C 的 setjmp/longjmp，作为 Ark RTOS 小程序编译
#
#   cd /mnt/nor/sj
#   make
#   mini run sj.svcm
#
# ---- SRCS 的第四份是汇编 ----
# setjmp/longjmp 必须是"被调用者直接进入"的那两段指令（理由见 sj.c 与
# /mnt/nor/setjmp.s），所以它们是汇编源，单独作为一份 SRCS。板上 cc 认 .s /
# .asm 后缀，四遍差分打包对汇编对象与 C 对象一视同仁。
#
# 顺序：启动桩（start.c）必须第一（入口偏移 0 的约定），其余随意。
TARGET = sj
OUT    = sj.svcm
SRCS   = /mnt/nor/start.c /mnt/nor/rt.c /mnt/nor/setjmp.s sj.c
INCS   = /mnt/nor
