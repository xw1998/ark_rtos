#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""F427 内核工程的补充编译环境：给「ON 档语法门禁」用。

为什么需要这个文件
------------------
共享头 kernelsrc/include/arkrtos_autoconf.h 的取值来源是 F427 内核工程
（见 docs/Kconfig配置系统.md §8）。F427 工程除了 Kconfig 开关，还靠两样
东西才能把选配编起来：

  1) 头文件搜索路径：lwIP 端口头（kernelsrc/components/lwip/port）、
     ark_softbus 组件头（kernelsrc/components/ark_softbus/include）；
  2) -DARK_BUS_* 档位宏（软总线 MCU 档，见 kernelsrc/include/arkrtos_softbus.h
     顶部注释；工程侧写在 uvprojx 的 MiscControls 里）。

门禁只拿 kernelsrc/include 这一层头去 armclang -fsyntax-only 时，这两样都
没有；共享头一按 F427 的真值打开（ARKRTOS_USE_LWIP=1 / ARKRTOS_USE_SOFTBUS=1），
报出来的就是 "lwip_port.h not found" / "ark_bus.h not found" / "档位宏未生效"。
那不是被测代码的问题，是门禁没有镜像工程环境。

这张表就是那份环境的唯一副本：F427 工程的 uvprojx（IncludePath /
MiscControls）改了，或软总线档位变了，只改这里，四道门禁一起跟上。
新增选配组件时同理——别只改门禁里的 INCLUDES。

不覆盖什么：真链接、真外设。这里只影响语法/类型检查的输入。
"""

import os

__all__ = ["ROOT", "EXTRA_INCLUDES", "SOFTBUS_TIER_DEFS"]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# F427 工程额外需要的头文件搜索路径（相对仓库根）。
EXTRA_INCLUDES = [
    "kernelsrc/components/ark_softbus/include",
    "kernelsrc/components/lwip/src/include",
    "kernelsrc/components/lwip/port",
]

# 软总线 MCU 档位宏：与 F427 工程 uvprojx 的 MiscControls 逐字一致。
# arkrtos_softbus.h 的 #error 就是拿这几个值当闸门，少一个就编不过。
SOFTBUS_TIER_DEFS = [
    "ARK_BUS_MAX_TOPICS=12",
    "ARK_BUS_MAX_DEVICES=4",
    "ARK_BUS_MSG_MAX_SIZE=128",
    "ARK_BUS_TOPIC_NAME_MAX=24",
    "ARK_BUS_QOS_MAX_SESSIONS=2",
    "ARK_BUS_RPC_MAX_SLOTS=4",
    "ARK_BUS_RPC_MAX_METHODS=4",
    "ARK_BUS_TASK_ENABLED=0",
    "ARK_BUS_COLL_ENABLED=0",
    "ARK_BUS_INFER_ENABLED=0",
    "ARK_BUS_STREAM_TX_SLOTS=1",
    "ARK_BUS_STREAM_RX_SLOTS=1",
    "ARK_BUS_STREAM_RX_MAX=256",
    "ARK_BUS_STREAM_MAX_BYTES=256",
]
