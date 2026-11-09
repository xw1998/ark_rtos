# /mnt/nor/tls/makefile -- `__thread` 在小程序上的实测工程
#
#   cd /mnt/nor/tls
#   make
#   mini run tls.svcm
#
# 这份工程没有额外的运行时依赖：__thread 在 ARM 后端上落成普通全局变量，
# 初值由启动桩调用的 __arkcc_init_globals() 写入。
TARGET = tls
OUT    = tls.svcm
SRCS   = /mnt/nor/start.c /mnt/nor/rt.c tls.c
INCS   = /mnt/nor
