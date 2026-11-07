# /mnt/nor/makefile -- Ark RTOS 小程序 hello world
#
# 板上用法（与 Linux 一样：进目录、make、跑）：
#   cd /mnt/nor
#   make
#   mini run hello.svcm
#
# 这份文件只认 ark make 的四类键：TARGET / OUT / SRCS / INCS（还有 DEFS）。
# 没有规则、没有依赖、没有时间戳——板上没有增量构建，每次都是全量四遍差分。
#
# SRCS 的第一个必须是启动桩：镜像的 entry_offset 是相对负载起点算的，
# MINISTART 必须落在偏移 0。四个源正好是这个文件系统的对象槽上限。
#
# 路径写相对名：ark make 的一行值只有 96 字节，四条绝对路径放不下（实测报
# "value is longer than the field that holds it"）；相对名按 shell 的当前
# 目录解析，`cd /mnt/nor && make` 与 Linux 上的习惯一致。
TARGET = hello
OUT    = hello.svcm
SRCS   = start.c mini_main.c rt.c hello.c
INCS   = /mnt/nor
