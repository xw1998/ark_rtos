#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""宿主用例的行覆盖率门禁：把"没测过的行"变成可数的数字。

为什么需要这一项
----------------
ci_gate 里那一排 *-host 检查只回答"用例过了没有"。用例过了但只走到模块的
三成代码，与走了八成代码，在这套门禁里长得一模一样。覆盖率把这件事变成一行
可比的数字，并且可以设一条"不许低于当前值"的下限——回归时掉下去就红。

这台机器上没有 cppcheck / gcov 的板端工具链也没关系：宿主用例本来就是用
MinGW gcc 编的，gcc 自带 --coverage + gcov，不需要额外装东西。

口径（写死，避免误读）
----------------------
* 只量 kernelsrc/src 里**被宿主用例链接的那一个 .c**，不量测试文件本身，也不量
  整仓。所以本门禁的数字**不代表内核整体覆盖率**——内核绝大多数的行（调度、
  SVC、驱动、shell）在宿主上根本没有被链接。
* 行覆盖率 = gcov 报的 Lines executed 那一行，分子分母都由 gcov 给，本文不重算。
* 门槛是"不低于基线"，不是"达到某个行业标准"。基线由 --update 从实测写入
  （见 tools/coverage_baseline.json），加代码导致覆盖率下降时会红，需要人工
  判断是"该补用例"还是"该改基线"，不许默默调低。
* 阻塞 / 上板路径在宿主上走不到，所以覆盖率天然有天花板。这是设计事实，不是
  缺陷——别把它读成"这模块测好了"。

用法
----
    python tools/coverage_gate.py                # 量一遍并和基线比
    python tools/coverage_gate.py --update       # 用实测值重写基线
    python tools/coverage_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe

退出码 0 = 全过；1 = 有用例失败 / 编不出来 / 覆盖率低于基线。
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
from tempdir_util import TempDir  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BASELINE = os.path.join(HERE, "coverage_baseline.json")

GCC_FALLBACKS = [
    "gcc",
    r"D:\mingw\MinGW\bin\gcc.exe",
    r"C:\mingw64\bin\gcc.exe",
    os.environ.get("ARKRTOS_HOST_CC", ""),
]

COMMON_INC = [
    "-Ikernelsrc/include",
    "-Ikernelsrc/components/mdk_trace",
    "-Iboard/stm32f427",
    "-Iconfig",
]

# F427 工程那一套补充环境（lwIP / 软总线的头与档位宏）。原样借
# tools/arkrtos_f427_env.py，不在这里抄第二份：那份表改了，四道门禁一起跟上。
from arkrtos_f427_env import EXTRA_INCLUDES, SOFTBUS_TIER_DEFS

F427_INCS = [
    "-Ikernelsrc/shell",
    "-Ikernelsrc/shell/ark_shell",
    "-Ikernelsrc/fs",
    "-Ikernelsrc/fs/littlefs",
    "-Ikernelsrc/components/ark_vfs/include",
    "-Ikernelsrc/components/arkc/include",
    "-Ikernelsrc/components/arkmake/include",
    "-Ikernelsrc/sdk",
    "-Iconfig/stm32f427",
    "-Iexample/stm32f427/kernel/ARKRTOS_TEST/Core/Inc",
    "-Iexample/stm32f427/kernel/ARKRTOS_TEST/Drivers/STM32F4xx_HAL_Driver/Inc",
    "-Iexample/stm32f427/kernel/ARKRTOS_TEST/Drivers/CMSIS/Device/ST/STM32F4xx/Include",
    "-Iexample/stm32f427/kernel/ARKRTOS_TEST/Drivers/CMSIS/Core/Include",
] + ["-I" + p for p in EXTRA_INCLUDES]

F427_DEFS = [
    '-DARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
    "-DSTM32F427xx",
    "-DUSE_HAL_DRIVER",
] + ["-D" + d for d in SOFTBUS_TIER_DEFS]

# 签名链的 32 字节密钥（0x00..0x1F）。与 test_sign_host.c /
# test_payload_sign_host.c 里的 static 表一致，改一处要三处一起改。
SIGN_KEY_DEF = "-DARKRTOS_IMAGE_SIGN_KEY={" + ",".join(
    "0x%02X" % b for b in range(32)) + "}"

# 关掉块层时它上层的三个消费者也必须一起关（features.h 有 #error 守着，
# 这不是本门禁的额外要求，而是仓库自己的依赖规则）。
BLK_OFF_DEFS = [
    "-DARKRTOS_USE_BLK=0",
    "-DARKRTOS_USE_FS=0",
    "-DARKRTOS_USE_VFS=0",
    "-DARKRTOS_USE_VFS_LFS=0",
    "-DARKRTOS_USE_MINIAPP=0",
    "-DARKRTOS_USE_PROCFS=0",
]

# (名字, 测试源, 被量的内核源, 额外宏, 成功标记[, 可选 dict])
# 宏与标记逐条对齐 tools/ci_gate.py 里对应的 *-host 检查，改那边要同步这里。
#
# 可选 dict 的两个键：
#   incs —— 这个用例额外要的头文件搜索路径（F427 工程那套）
#   argv —— 传给用例的实参（例如签名用例把镜像写到哪里）
#   markers —— 覆盖掉第 5 项，要求多条输出同时出现才算过
#   ldflags —— 链接期参数（如 -Wl,--wrap=名字），只影响链接
#
# 同一个 .c 出现在多个用例里（blk 的开档 / 关档）时，百分比按**行号并集**算：
# 两档编出来的是不同的代码（#else 那一半只在关档时存在），而覆盖率问的是
# "这行代码有没有被执行过"。所以每档交出自己的"执行行集 / 编入行集"，
# main() 按模块求并集再除。单档百分比会以 case 行打印出来，只作参考不参与判。
#
# 为什么 sign 用的是 test_payload_sign_host.c 而不是 test_sign_host.c：
# 后者把 MAC 打到 stdout 交给 tools/sign_host_gate.py 与打包器比对，自己
# 不做判定；前者自带手算 tag 对拍 + 篡改必拒 + 流式/一次性一致，宿主上
# 失败会自己返回非 0。覆盖面也更宽（多跑了流式入口）。
# 软总线宿主用例要链**真的组件**：被测的是内核接入层（arkrtos_softbus.c），
# 但它一调就是 ark_bus_* 整条链（core / frame / qos / rpc / stream /
# discovery / transport_loopback / port 胶水）。这些源要一起编进来参与链接，
# 但覆盖率只记接入层那一个 .c —— 组件是同步进来的第三方副本，量它的行数
# 说明不了本仓库的测试质量。
SOFTBUS_INCS = ["-I" + p for p in EXTRA_INCLUDES] + [
    "-Ikernelsrc/shell",
    "-Ikernelsrc/shell/ark_shell",
    "-Ikernelsrc/components/ark_softbus/src",
    "-Ikernelsrc/components/ark_softbus/port",
]

SOFTBUS_DEFS = ["-DARKRTOS_USE_SOFTBUS=1"] + ["-D" + x for x in SOFTBUS_TIER_DEFS]

SOFTBUS_LINK = (
    [os.path.join("kernelsrc/components/ark_softbus/src", n)
     for n in sorted(os.listdir(os.path.join(ROOT,
                                             "kernelsrc/components/ark_softbus/src")))
     if n.startswith("ark_bus_") and n.endswith(".c")]
    + ["kernelsrc/components/ark_softbus/src/transport/transport_loopback.c",
       "kernelsrc/components/ark_softbus/port/ark_bus_hal_arkrtos.c"]
)

# 板级配置由 -D + -include 显式带进来：coverage_gate 的 COMMON_INC 里那份
# 是 stm32f427 的搜索路径，而 Linux ABI 依赖 qemu_loongarch64 的开关组合
# （USE_PAGING / USE_LINUX_ABI / USE_ELF 同时打开）。
BOARD_QEMU_LA64_DEFS = [
    '-DARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
]

CASES = [
    ("utest", "tools/utest_host_selftest.c",
     ["kernelsrc/src/arkrtos_utest.c"], [], "HOST-SELFTEST OK"),
    ("sbuf", "kernelsrc/tests/test_sbuf_host.c",
     ["kernelsrc/src/arkrtos_sbuf.c"], ["-DARKRTOS_USE_SBUF=1"], "SBUF-HOST OK"),
    ("mempool", "kernelsrc/tests/test_mempool_host.c",
     ["kernelsrc/src/arkrtos_mempool.c"], ["-DARKRTOS_USE_MEMPOOL=1"],
     "MEMPOOL-HOST OK"),
    ("mq", "kernelsrc/tests/test_mq_host.c",
     ["kernelsrc/src/arkrtos_mq.c"],
     ["-DARKRTOS_USE_MQ=1", "-DARKRTOS_MQ_HOST_TEST"], "MQ-HOST OK"),
    ("sync", "kernelsrc/tests/test_sync_host.c",
     ["kernelsrc/src/arkrtos_sync.c"],
     ["-DARKRTOS_USE_MTX_RECURSIVE=1", "-DARKRTOS_SYNC_HOST_TEST"], "SYNC-HOST OK"),
    ("loader", "kernelsrc/tests/test_loader_host.c",
     ["kernelsrc/src/arkrtos_loader.c"],
     ["-fno-asynchronous-unwind-tables",
      "-DARKRTOS_USE_IMAGE_CRC_RECHECK=1"], "LOADER-HOST OK"),
    ("fifo", "kernelsrc/tests/test_fifo_host.c",
     ["kernelsrc/src/arkrtos_fifo.c"],
     ["-fno-asynchronous-unwind-tables"], "FIFO-HOST OK"),
    ("fault", "kernelsrc/tests/test_fault_host.c",
     ["kernelsrc/src/arkrtos_fault.c"],
     ["-fno-asynchronous-unwind-tables"], "FAULT-HOST OK"),
    ("eset", "kernelsrc/tests/test_eset_host.c",
     ["kernelsrc/src/arkrtos_eset.c"],
     ["-DARKRTOS_USE_ESET=1", "-DARKRTOS_ESET_HOST_TEST"], "ESET-HOST OK"),
    ("waitmulti", "kernelsrc/tests/test_waitmulti_host.c",
     ["kernelsrc/src/arkrtos_waitmulti.c"],
     ["-DARKRTOS_USE_WAITMULTI=1"], "WM-HOST OK"),
    # --- 2026-11-21：把已有宿主用例但没进覆盖率账的模块补上 -------------
    ("smp", "kernelsrc/tests/test_smp_host.c",
     ["kernelsrc/src/arkrtos_smp.c"],
     ["-DARKRTOS_USE_SMP=1", "-DARKRTOS_CPU_NUM=4"] + F427_DEFS,
     "SMP-HOST OK", {"incs": F427_INCS}),
    ("sign", "kernelsrc/tests/test_payload_sign_host.c",
     ["kernelsrc/src/arkrtos_sign.c"],
     ["-DARKRTOS_USE_IMAGE_SIGN=1", "-DARKRTOS_USE_EXEC_SIGN=1",
      "-Wno-variably-modified", SIGN_KEY_DEF],
     ["STREAM_MATCH 1", "WRONG_TAG_REFUSED 1"]),
    # 镜像侧入口（sign_image / stream_* / verify）由 test_sign_host.c 走：
    # 它把镜像写到 argv[1]，所以这里给一个相对文件名（工作目录就是临时目录）。
    # 与上面的 sign 用例按行号并集合并到同一个模块（arkrtos_sign）。
    ("sign-image", "kernelsrc/tests/test_sign_host.c",
     ["kernelsrc/src/arkrtos_sign.c"],
     ["-DARKRTOS_USE_IMAGE_SIGN=1", "-Wno-variably-modified", SIGN_KEY_DEF],
     ["MAC ", "IMAGE "], {"argv": ["image.bin"]}),
    ("tickless", "kernelsrc/tests/test_tickless_host.c",
     ["kernelsrc/src/arkrtos_tickless.c"],
     ["-DARKRTOS_USE_TICKLESS=1", "-DARKRTOS_USE_TIMER=1"],
     "TICKLESS-HOST OK", {"incs": F427_INCS}),
    # --- 2026-11-21 新增：块设备注册表（此前没有任何宿主用例）---------
    ("blk", "kernelsrc/tests/test_blk_host.c",
     ["kernelsrc/src/arkrtos_blk.c"],
     ["-DARKRTOS_USE_BLK=1"], "BLK-HOST OK"),
    ("blk-off", "kernelsrc/tests/test_blk_host.c",
     ["kernelsrc/src/arkrtos_blk.c"],
     BLK_OFF_DEFS, "BLK-HOST OFF OK"),
    # --- 2026-11-21 新增：设备框架（此前没有任何宿主用例）---------------
    # HW_SAFE 开/关是两段代码（所有权台账 claim/release），按行号并集合并。
    ("dev", "kernelsrc/tests/test_dev_host.c",
     ["kernelsrc/src/arkrtos_dev.c"],
     ["-DARKRTOS_HW_SAFE=1"], "DEV-HOST OK"),
    ("dev-nosafe", "kernelsrc/tests/test_dev_host.c",
     ["kernelsrc/src/arkrtos_dev.c"],
     ["-DARKRTOS_HW_SAFE=0"], "DEV-HOST NOSAFE OK"),
    # --- 2026-11-21 新增：事件对象（WAITMULTI 开/关是两段代码）-------
    ("event", "kernelsrc/tests/test_event_host.c",
     ["kernelsrc/src/arkrtos_event.c"],
     ["-DARKRTOS_USE_WAITMULTI=0"], "EVENT-HOST OK"),
    ("event-multi", "kernelsrc/tests/test_event_host.c",
     ["kernelsrc/src/arkrtos_event.c"],
     ["-DARKRTOS_USE_WAITMULTI=1"], "EVENT-HOST MULTI OK"),
    # --- 2026-11-21 新增：软定时器（USE_TIMER 开/关 + TICKLESS 是第三段代码）--
    ("timer", "kernelsrc/tests/test_timer_host.c",
     ["kernelsrc/src/arkrtos_timer.c"],
     ["-DARKRTOS_USE_TIMER=1", "-DARKRTOS_TIMER_HOST_TEST"], "TIMER-HOST OK"),
    ("timer-tickless", "kernelsrc/tests/test_timer_host.c",
     ["kernelsrc/src/arkrtos_timer.c"],
     ["-DARKRTOS_USE_TIMER=1", "-DARKRTOS_USE_TICKLESS=1",
      "-DARKRTOS_TIMER_HOST_TEST"], "TIMER-HOST TL OK"),
    ("timer-off", "kernelsrc/tests/test_timer_host.c",
     ["kernelsrc/src/arkrtos_timer.c"],
     ["-DARKRTOS_USE_TIMER=0"], "TIMER-HOST OFF OK"),
    # --- 2026-11-21 新增：任务配置与任务表（STACK_USAGE 开/关 +
    # STACK_GUARD 是第二段代码，按行号并集合并）-----------------------
    # 槽位复用 / recover_pending 跳过 / 表满 / 三道参数闸门是同一个函数里的
    # 不同分支，成功路径必须走一遍才看得见 TCB 填得对不对。
    ("cfg", "kernelsrc/tests/test_cfg_host.c",
     ["kernelsrc/src/arkrtos_cfg.c"], [], "CFG-HOST OK"),
    ("cfg-guard", "kernelsrc/tests/test_cfg_host.c",
     ["kernelsrc/src/arkrtos_cfg.c"], ["-DARKRTOS_USE_STACK_GUARD=1"],
     "CFG-HOST GUARD OK"),
    ("cfg-nousage", "kernelsrc/tests/test_cfg_host.c",
     ["kernelsrc/src/arkrtos_cfg.c"], ["-DARKRTOS_USE_STACK_USAGE=0"],
     "CFG-HOST NOUSAGE OK"),
    # --- 2026-11-21 新增：内核符号导出表（MODLOAD 开/关是两段代码）-------
    # 关档时 exports.c 是空目标（零体积零符号），只有开档能走打印与查表。
    ("export", "kernelsrc/tests/test_export_host.c",
     ["kernelsrc/src/arkrtos_exports.c"],
     ["-DARKRTOS_USE_MODLOAD=1"], "EXPORT-HOST OK"),
    ("export-off", "kernelsrc/tests/test_export_host.c",
     ["kernelsrc/src/arkrtos_exports.c"],
     ["-DARKRTOS_USE_MODLOAD=0"], "EXPORT-HOST OFF OK"),
    ("init", "kernelsrc/tests/test_init_host.c",
     ["kernelsrc/src/arkrtos_init.c"], [], "INIT-HOST OK"),
    ("init-on", "kernelsrc/tests/test_init_host.c",
     ["kernelsrc/src/arkrtos_init.c"],
     ["-DARKRTOS_USE_SBUF=1", "-DARKRTOS_USE_ESET=1",
      "-DARKRTOS_USE_SMP=1"], "INIT-HOST ON OK"),
    # --- 2026-11-21 新增：分级日志 / 控制台锁 / dmesg 环 ----------------
    # USE_LOG 与 USE_LOG_RING 是两层 #if：#else 是「恒拒绝」桩、环的 #else 是
    # 四个答 0 的函数，所以三档缺一档就有代码进不了行号并集。
    # 默认档把环压到 64 字节，否则回卷要写满 4096 字节日志才看得见。
    ("log", "kernelsrc/tests/test_log_host.c",
     ["kernelsrc/src/arkrtos_log.c"],
     ["-DARKRTOS_LOG_RING_SIZE=64"], "LOG-HOST OK"),
    ("log-noring", "kernelsrc/tests/test_log_host.c",
     ["kernelsrc/src/arkrtos_log.c"],
     ["-DARKRTOS_USE_LOG_RING=0"], "LOG-HOST NORING OK"),
    ("log-off", "kernelsrc/tests/test_log_host.c",
     ["kernelsrc/src/arkrtos_log.c"],
     ["-DARKRTOS_USE_LOG=0"], "LOG-HOST OFF OK"),
    ("ptable", "kernelsrc/tests/test_ptable_host.c",
     ["kernelsrc/src/arkrtos_ptable.c"], [], "PTABLE-HOST OK"),
    # --- 2026-11-22 新增：设备端布局解析（配置区记录 + 固定槽表 + 发布）---
    ("layout", "kernelsrc/tests/test_layout_host.c",
     ["kernelsrc/src/arkrtos_layout.c"], [], "LAYOUT-HOST OK"),
    # --- 2026-11-22 新增：跨复位崩溃日志（USE_CRASH_LOG 开/关是两段代码，
    # 关档连 journal 全局量都不存在）。宿主编译时把日志宏编掉
    # （-DARKRTOS_LOG_LEVEL=0），该模块只依赖 ptable / tick / 临界区三个符号。
    ("crash", "kernelsrc/tests/test_crash_host.c",
     ["kernelsrc/src/arkrtos_crash.c"],
     ["-DARKRTOS_LOG_LEVEL=0"], "CRASH-HOST OK  "),
    ("crash-off", "kernelsrc/tests/test_crash_host.c",
     ["kernelsrc/src/arkrtos_crash.c"],
     ["-DARKRTOS_LOG_LEVEL=0", "-DARKRTOS_USE_CRASH_LOG=0"],
     "CRASH-HOST OFF OK"),
    # --- 2026-11-22 新增：硬件安全态 / 遗言（HW_SAFE 开/关是两段代码）---
    ("hwsafe", "kernelsrc/tests/test_hwsafe_host.c",
     ["kernelsrc/src/arkrtos_hwsafe.c"],
     ["-DARKRTOS_HW_SAFE=1"], "HWSAFE-HOST OK"),
    ("hwsafe-off", "kernelsrc/tests/test_hwsafe_host.c",
     ["kernelsrc/src/arkrtos_hwsafe.c"],
     ["-DARKRTOS_HW_SAFE=0"], "HWSAFE-HOST OFF OK"),
    # --- 2026-11-22 新增：内核自检（USE_AUDIT 开/关是两段代码）---
    # ON 档链接真实的 ptable.c：audit 读的就是那张表，不替它做桩。
    # ptable 的分母在这一档只走到一个入口，
    # 但并集只加不减，ptable 自己那条 case 的覆盖不会被拉低。
    ("audit", "kernelsrc/tests/test_audit_host.c",
     ["kernelsrc/src/arkrtos_audit.c", "kernelsrc/src/arkrtos_ptable.c"],
     ["-DARKRTOS_USE_AUDIT=1"], "AUDIT-HOST OK"),
    ("audit-off", "kernelsrc/tests/test_audit_host.c",
     ["kernelsrc/src/arkrtos_audit.c"],
     ["-DARKRTOS_USE_AUDIT=0"], "AUDIT-HOST OFF OK"),
    # --- 2026-11-22 新增：MPU 窗口构建（USE_MPU 开/关是两段代码）---
    # 关档是空目标，覆盖率只由开档供给。
    ("mpu", "kernelsrc/tests/test_mpu_host.c",
     ["kernelsrc/src/arkrtos_mpu.c"],
     ["-DARKRTOS_USE_MPU=1"], "MPU-HOST OK"),
    ("mpu-off", "kernelsrc/tests/test_mpu_host.c",
     ["kernelsrc/src/arkrtos_mpu.c"],
     ["-DARKRTOS_USE_MPU=0"], "MPU-HOST OFF OK"),
    # --- 2026-11-22 新增：看门狗门控与心跳契约 ---
    # 装狗/不装狗是同一个宏的两个取值（同一份源码编出两段决策），关档又是另一段
    # #else；三条并集才把两个分支都盖住。
    ("guard", "kernelsrc/tests/test_guard_host.c",
     ["kernelsrc/src/arkrtos_guard.c"],
     ["-DARKRTOS_WDG_ENABLE=1"], "GUARD-HOST OK"),
    ("guard-nowdg", "kernelsrc/tests/test_guard_host.c",
     ["kernelsrc/src/arkrtos_guard.c"],
     ["-DARKRTOS_WDG_ENABLE=0"], "GUARD-HOST NOWDG OK"),
    ("guard-off", "kernelsrc/tests/test_guard_host.c",
     ["kernelsrc/src/arkrtos_guard.c"],
     ["-DARKRTOS_USE_KERNEL_GUARD=0"], "GUARD-HOST OFF OK"),
    # --- 2026-11-22 新增：实时性度量（RTSTAT 开/关是两段代码）---
    # 关档是空目标（零符号），覆盖率只由开档供给；开档带着宿主预算钩子
    # ARKRTOS_RT_HOST_TEST，没有它辅助任务体那个 for(;;) 会把门禁挂死。
    ("rt", "kernelsrc/tests/test_rt_host.c",
     ["kernelsrc/src/arkrtos_rt.c"],
     ["-DARKRTOS_USE_RTSTAT=1", "-DARKRTOS_RT_HOST_TEST=1"], "RT-HOST OK"),
    ("rt-off", "kernelsrc/tests/test_rt_host.c",
     ["kernelsrc/src/arkrtos_rt.c"],
     ["-DARKRTOS_USE_RTSTAT=0"], "RT-HOST OFF OK"),
    # --- 2026-11-22 新增：安全基线自检（12 项判据）。四档并集才够：
    # sec.c 每一项几乎都有 #if/#else 两种写法（开关在/不在），单档最多 99%。
    # -ffunction-sections 是宿主的必需项：MinGW 在 -O0 下会把带（弱）符号的
    # 函数放进调用者的 COMDAT 组，模块自带的 weak arkrtos_port_flash_protection
    # 会被捆进 main 的组里，调用点解析到错误的入口（实测直接段错误）。
    ("sec", "kernelsrc/tests/test_sec_host.c",
     ["kernelsrc/src/arkrtos_sec.c"],
     ["-ffunction-sections", "-DARKRTOS_USE_SECCHECK=1",
      "-DARKRTOS_USE_IMAGE_CRC_RECHECK=1", "-DARKRTOS_USE_IMAGE_SIGN=1",
      "-DARKRTOS_HW_SAFE=1", "-DARKRTOS_WDG_ENABLE=1",
      "-DARKRTOS_USE_RTSTAT=1", SIGN_KEY_DEF], "SEC-HOST OK  "),
    # 全关档：也把弱端口让出来（SEC_HOST_NO_PORT），由模块自带的弱符号兜底，
    # 走的是 rc != 0 的那条 SKIP。
    ("sec-min", "kernelsrc/tests/test_sec_host.c",
     ["kernelsrc/src/arkrtos_sec.c"],
     ["-ffunction-sections", "-DARKRTOS_USE_SECCHECK=1",
      "-DARKRTOS_USE_MPU=0", "-DARKRTOS_USE_PRIV=0",
      "-DARKRTOS_USE_STACK_CHECK=0", "-DARKRTOS_USE_FAULT_RECOVER=0",
      "-DARKRTOS_USE_CRASH_LOG=0", "-DARKRTOS_USE_AUDIT=0",
      "-DARKRTOS_USE_IMAGE_SIGN=0", "-DARKRTOS_USE_IMAGE_CRC_RECHECK=0",
      "-DARKRTOS_HW_SAFE=0", "-DARKRTOS_USE_KERNEL_GUARD=0",
      "-DARKRTOS_WDG_ENABLE=0", "-DARKRTOS_USE_RTSTAT=0",
      "-DSEC_HOST_NO_PORT", "-DSEC_HOST_EXPECT_MIN"],
     "SEC-HOST MIN OK  "),
    # 页表后端：LA64 没有 ARM MPU，隔离由分页 + PLV 提供（iso=2）。
    # 看门狗这一档不咬（WDG_ENABLE=0），所以 guard.watchdog 报 FINDING。
    ("sec-page", "kernelsrc/tests/test_sec_host.c",
     ["kernelsrc/src/arkrtos_sec.c"],
     ["-ffunction-sections", "-DARKRTOS_USE_SECCHECK=1",
      "-DARKRTOS_ARCH_CORE=0x0302", "-DARKRTOS_USE_MPU=0",
      "-DARKRTOS_USE_PRIV=1", "-DARKRTOS_USE_PAGING=1",
      "-DARKRTOS_USE_KERNEL_GUARD=1", "-DARKRTOS_WDG_ENABLE=0",
      "-DSEC_HOST_EXPECT_PAGE"], "SEC-HOST PAGE OK  "),
    # 开关关档：空目标（零体积零符号），覆盖率只由开档供给。
    ("sec-disabled", "kernelsrc/tests/test_sec_host.c",
     ["kernelsrc/src/arkrtos_sec.c"],
     ["-ffunction-sections", "-DARKRTOS_USE_SECCHECK=0"],
     "SEC-HOST DISABLED OK"),
    # --- 2026-11-22 新增：板上直跑（裸文件直接进固定窗口）---
    # 窗口地址来自板级分区头，只有 h743 有；宿主按 h743 的数值从命令行
    # 注入，用例自己再核一遍「注入的形状与 SLOT_RAM 映射区间一致」。
    # 关档是空目标（零符号），覆盖率只由开档供给。
    # 自旋锁走 fallback 档（SPINLOCK=0）只需临界区 + 屏障 + 核号三个桩。
    ("apprun", "kernelsrc/tests/test_apprun_host.c",
     ["kernelsrc/src/arkrtos_apprun.c"],
     ["-DARKRTOS_USE_APPRUN=1", "-DARKRTOS_USE_VFS=1",
      "-DARKRTOS_USE_SPINLOCK=0", "-DARKRTOS_LOG_LEVEL=0",
      "-DARKRTOS_APPRUN_BASE=0x20010000", "-DARKRTOS_APPRUN_CODE_SIZE=8192",
      "-DARKRTOS_APPRUN_RAM_BASE=0x20012000", "-DARKRTOS_APPRUN_RAM_SIZE=8192",
      "-DARKRTOS_APPRUN_STACK_SIZE=4096", "-DARKRTOS_APPRUN_PRIORITY=11",
      "-DARKRTOS_APPRUN_PERIOD_MS=0", "-DARKRTOS_APPRUN_MAX_IMAGE=8192",
      "-DARKRTOS_APPRUN_PROLOGUE=0xB5E0"], "APPRUN-HOST OK"),
    # --- 2026-11-22 新增：产物落 Flash 并引导执行 ---
    # 区基址/元数据/上限全部来自 h743 的分区头，宿主按同一组数值注入；
    # 用例自己核一遍「区顶 = Flash 末尾、元数据占最后一个 flash word」。
    # 关档是空目标（零符号），覆盖率只由开档供给。日志不编译掉（改成把
    # 级别压到最低），否则 ARKRTOS_LOGI 的三个参数会在 -Wextra 下报未使用。
    ("flashapp", "kernelsrc/tests/test_flashapp_host.c",
     ["kernelsrc/src/arkrtos_flashapp.c"],
     ["-DARKRTOS_USE_FLASHAPP=1", "-DARKRTOS_USE_APPRUN=1",
      "-DARKRTOS_USE_VFS=1", "-DARKRTOS_USE_SPINLOCK=0",
      "-DARKRTOS_APPRUN_BASE=0x20010000", "-DARKRTOS_APPRUN_CODE_SIZE=8192",
      "-DARKRTOS_APPRUN_RAM_BASE=0x20012000", "-DARKRTOS_APPRUN_RAM_SIZE=8192",
      "-DARKRTOS_APPRUN_STACK_SIZE=4096", "-DARKRTOS_APPRUN_PRIORITY=11",
      "-DARKRTOS_APPRUN_PERIOD_MS=0", "-DARKRTOS_APPRUN_MAX_IMAGE=8192",
      "-DARKRTOS_APPRUN_PROLOGUE=0xB5E0",
      "-DARKRTOS_FLASHAPP_BASE=0x081E0000", "-DARKRTOS_FLASHAPP_SIZE=131072",
      "-DARKRTOS_FLASHAPP_END=0x08200000", "-DARKRTOS_FLASHAPP_META_SIZE=32",
      "-DARKRTOS_FLASHAPP_META_BASE=0x081FFFE0",
      "-DARKRTOS_FLASHAPP_MAX_IMAGE=131008",
      "-DARKRTOS_FLASHAPP_MAGIC=0x50414C46", "-DARKRTOS_FLASHAPP_VERSION=1",
      "-DARKRTOS_FLASHAPP_RAM_BASE=0x20012000", "-DARKRTOS_FLASHAPP_RAM_SIZE=8192",
      "-DARKRTOS_FLASHAPP_STACK_SIZE=4096", "-DARKRTOS_FLASHAPP_PROLOGUE=0xB5E0",
      "-DARKRTOS_FLASHAPP_PRIORITY=11"], "FLASHAPP-HOST OK"),
    # --- 2026-11-22 新增：内核事件追踪门面（SWD/BUFF/无后端/关档四档）---
    # 三个后端分支与关档空实现各占一段 #if，单档最多 100% 覆盖其中一支；
    # ark_shell.h 在 kernelsrc/shell 下，只有这个 TU 需要那条 include。
    ("trace-swd", "kernelsrc/tests/test_trace_host.c",
     ["kernelsrc/src/arkrtos_trace.c"],
     ["-DTRACE_HOST_SWD=1", "-DMDK_TRACE_BACKEND_SWD=1"], "TRACE-HOST OK  ",
     {"incs": ["-Ikernelsrc/shell", "-Ikernelsrc/shell/ark_shell"]}),
    ("trace-buff", "kernelsrc/tests/test_trace_host.c",
     ["kernelsrc/src/arkrtos_trace.c"],
     ["-DTRACE_HOST_BUFF=1", "-DMDK_TRACE_BACKEND_BUFF=1"], "TRACE-HOST OK  ",
     {"incs": ["-Ikernelsrc/shell", "-Ikernelsrc/shell/ark_shell"]}),
    ("trace-none", "kernelsrc/tests/test_trace_host.c",
     ["kernelsrc/src/arkrtos_trace.c"],
     ["-DMDK_TRACE_BACKEND_NONE=1"], "TRACE-HOST OK  ",
     {"incs": ["-Ikernelsrc/shell", "-Ikernelsrc/shell/ark_shell"]}),
    ("trace-off", "kernelsrc/tests/test_trace_host.c",
     ["kernelsrc/src/arkrtos_trace.c"],
     ["-DTRACE_HOST_OFF=1", "-DARKRTOS_USE_MDK_TRACE=0"], "TRACE-HOST OFF OK",
     {"incs": ["-Ikernelsrc/shell", "-Ikernelsrc/shell/ark_shell"]}),
    # apppack 用例必须真的链接 loader.c（产出的镜像要过装载器的 check_header），
    # 但 loader 有自己的 case，所以这里用 link 只链接、不重复统计它的覆盖率。
    ("apppack", "kernelsrc/tests/test_apppack_host.c",
     ["kernelsrc/src/arkrtos_apppack.c"],
     ["-DARKRTOS_USE_APPPACK=1", "-fno-asynchronous-unwind-tables"],
     "APPPACK-HOST OK",
     {"link": ["kernelsrc/src/arkrtos_loader.c"]}),
    # 安装器的常驻任务循环体是 while(1)，门禁把它的一轮暴露出来才能有界地驱动。
    ("insthost", "kernelsrc/tests/test_installer_host.c",
     ["kernelsrc/src/arkrtos_installer.c"],
     ["-DARKRTOS_INSTALLER_HOST_TEST=1", "-fno-asynchronous-unwind-tables"],
     "INSTALLER-HOST OK"),
    # --- 2026-11-22 新增：页表隔离后端（USE_PAGING 开/关是两段代码）---
    # 只有开档有代码，关档是空目标。板级配置取 qemu_loongarch64 那份：
    # 页表后端在 ARMv7-M 上被 arkrtos_config.h 直接 #error 拦掉，而这一层
    # 的语义（审计面 / 空间切换 / 动态映射 / 受控拷贝）与具体 port 无关，
    # 20 个 port 函数在用例里全部是桩。签名档位要一把 32 字节密钥才能过
    # features.h 的 #error，这里按值注入，不依赖宿主机上任何临时头文件。
    ("paging", "kernelsrc/tests/test_paging_host.c",
     ["kernelsrc/src/arkrtos_paging.c"],
     ['-DARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
      "-include", "arkrtos_board_config.h", SIGN_KEY_DEF,
      "-fno-asynchronous-unwind-tables"], "PAGING-HOST OK",
     {"incs": ["-Iboard/qemu_loongarch64", "-Ikernelsrc/port/loongarch/la64"]}),
    # --- 2026-11-22 新增：控制台登录（密码闸门）---
    # 盐与摘要是夹具，不来自任何构建生成物：盐 0x00..0x0F，口令 "s3cret!"，
    # 摘要是 sha256(salt || "s3cret!") 离线算出来的。这样用例断言的是
    # "输入正确口令真的解锁"，而不是把被测代码自己算出的值再比一遍。
    ("shellauth", "kernelsrc/tests/test_shell_auth_host.c",
     ["kernelsrc/src/arkrtos_shell_auth.c"],
     ["-DARKRTOS_USE_SHELL_AUTH=1", "-DSHELL_ENABLE=1",
      "-DARKRTOS_SHELL_AUTH_SALT={0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F}",
      "-DARKRTOS_SHELL_AUTH_DIGEST={0xBD,0xCD,0xB6,0x66,0x5B,0xC6,0x92,0xE8,0xEB,0xCC,0x98,0x4E,0xAF,0xDA,0xFE,0xD0,0xE7,0x81,0x34,0xB8,0x28,0x58,0x6F,0xC7,0x3B,0xA0,0x46,0x52,0x34,0xF2,0xF5,0xF1}",
      "-fno-asynchronous-unwind-tables"], "SHELL-AUTH-HOST OK",
     {"incs": ["-Ikernelsrc/shell", "-Ikernelsrc/shell/ark_shell"]}),
    # --- 2026-11-22 新增：/chosen/stdout-path 的消费（判断与拒绝原因）---
    # 依赖面只有 5 个 FDT 查询，全部在用例里打桩；不引入真实 DTB。
    # 关档（默认 0）本文件不产生符号，这里固定开档测。
    ("fdtconsole", "kernelsrc/tests/test_fdt_console_host.c",
     ["kernelsrc/src/arkrtos_fdt_console.c"],
     ["-DARKRTOS_USE_FDT_CONSOLE=1", "-DARKRTOS_USE_FDT=1",
      "-fno-asynchronous-unwind-tables"], "FDT-CONSOLE-HOST OK"),
    # --- 2026-11-22 新增：定时器 / 时基的设备树核对（驱动配置源）---
    # 七个 FDT 查询全部打桩，节点表由用例按需搭；不引入真实 DTB。
    ("fdtres", "kernelsrc/tests/test_fdt_res_host.c",
     ["kernelsrc/src/arkrtos_fdt_res.c"],
     ["-DARKRTOS_USE_FDT_RES=1", "-DARKRTOS_USE_FDT=1",
      "-fno-asynchronous-unwind-tables"], "FDT-RES-HOST OK"),
    # --- 2026-11-22 新增：扁平设备树解析器本体（1477 行，用例在宿主侧手工
    # 拼出整块 DTB：头 + 结构块 + 字符串块，再逐字段改坏） ---
    ("fdt", "kernelsrc/tests/test_fdt_host.c",
     ["kernelsrc/src/arkrtos_fdt.c"],
     ["-DARKRTOS_USE_FDT=1", "-fno-asynchronous-unwind-tables"],
     "FDT-HOST OK"),
    # --- 2026-11-22 新增：dtb overlay 重拼（2495 行，两遍扫描 + 六个拒绝码） ---
    ("fdtovl", "kernelsrc/tests/test_fdt_overlay_host.c",
     ["kernelsrc/src/arkrtos_fdt_overlay.c", "kernelsrc/src/arkrtos_fdt.c"],
     ["-DARKRTOS_USE_FDT=1", "-DARKRTOS_USE_FDT_OVERLAY=1",
      "-fno-asynchronous-unwind-tables"], "FDT-OVERLAY-HOST OK"),
    # --- 2026-11-22 新增：littlefs -> ark_vfs 桥（516 行，15 个门面桩，
    # 单写流 / BUSY / NOSYS 三条承诺） ---
    ("vfslfs", "kernelsrc/tests/test_vfs_lfs_host.c",
     ["kernelsrc/src/arkrtos_vfs_lfs.c"],
     ["-DARKRTOS_USE_VFS=1", "-DARKRTOS_USE_VFS_LFS=1",
      "-DARKRTOS_USE_FS=1", "-fno-asynchronous-unwind-tables"],
     "VFS-LFS-HOST OK",
     {"incs": ["-Ikernelsrc/components/ark_vfs/include",
               "-Ikernelsrc/components/ark_vfs/src"]}),
    # arkrtos_vfs 门面：链真 ark_vfs core + 真 ramfs，只桩板级端口层
    # （块设备窗口 / arkrtos_fs / littlefs 桥 / 日志 / 任务状态）。两档：
    # 默认档跑完整的持久卷路径，vfsnolfs 档（LFS=0）走 mount_volume 的
    # NOSYS 分支——同一个 .c 两档按行号并集计覆盖率。
    # DARK_VFS_MAX_FDS=12：ark_vfs 的 fd 表要比 App 句柄表（8）宽，才走得到
    # 「句柄槽用满、把刚开的 fd 还回去」那两条；8/8 它们是结构性不可达。
    ("vfs", "kernelsrc/tests/test_vfs_host.c",
     ["kernelsrc/src/arkrtos_vfs.c"],
     ["-DARKRTOS_MINI_READ_VFS=1", "-DARK_VFS_MAX_FDS=12",
      "-fno-asynchronous-unwind-tables"],
     "VFS-HOST OK",
     {"incs": ["-Ikernelsrc/components/ark_vfs/include",
               "-Ikernelsrc/components/ark_vfs/src",
               "-Ikernelsrc/components/ark_vfs/port"],
      "link": ["kernelsrc/components/ark_vfs/src/ark_vfs_core.c",
               "kernelsrc/components/ark_vfs/src/ark_vfs_ramfs.c"]}),
    ("vfsnolfs", "kernelsrc/tests/test_vfs_host.c",
     ["kernelsrc/src/arkrtos_vfs.c"],
     ["-DARKRTOS_MINI_READ_VFS=1", "-DARKRTOS_USE_VFS_LFS=0",
      "-DARK_VFS_MAX_FDS=12", "-fno-asynchronous-unwind-tables"],
     "VFS-HOST OK",
     {"incs": ["-Ikernelsrc/components/ark_vfs/include",
               "-Ikernelsrc/components/ark_vfs/src",
               "-Ikernelsrc/components/ark_vfs/port"],
      "link": ["kernelsrc/components/ark_vfs/src/ark_vfs_core.c",
               "kernelsrc/components/ark_vfs/src/ark_vfs_ramfs.c"]}),
    # --- 2026-11-22 新增：littlefs 门面（真 littlefs + RAM NOR） ---
    # 两档：开档跑整条持久化路径，关档（USE_FS=0）走空实现分支。
    # link 里是链库里那份 littlefs 本体：真源码、同一组 LFS_NO_* 开关，不计入覆盖率。
    ("fs", "kernelsrc/tests/test_fs_host.c",
     ["kernelsrc/src/arkrtos_fs.c"],
     ["-DARKRTOS_USE_FS=1", "-DLFS_NO_MALLOC", "-DLFS_NO_ASSERT",
      "-DLFS_NO_DEBUG", "-DLFS_NO_WARN", "-DLFS_NO_ERROR",
      "-fno-asynchronous-unwind-tables"],
     "FS-HOST OK",
     {"incs": ["-Ikernelsrc/fs",
               "-isystem", "kernelsrc/fs/littlefs"],
      "link": ["kernelsrc/fs/littlefs/lfs.c",
               "kernelsrc/fs/littlefs/lfs_util.c"]}),
    ("fsnofs", "kernelsrc/tests/test_fs_host.c",
     ["kernelsrc/src/arkrtos_fs.c"],
     ["-DARKRTOS_USE_FS=0", "-DARKRTOS_USE_MINIAPP=0",
      "-DARKRTOS_USE_VFS_LFS=0", "-DLFS_NO_MALLOC", "-DLFS_NO_ASSERT",
      "-DLFS_NO_DEBUG", "-DLFS_NO_WARN", "-DLFS_NO_ERROR",
      "-fno-asynchronous-unwind-tables"],
     "FS-HOST OK",
     {"incs": ["-Ikernelsrc/fs",
               "-isystem", "kernelsrc/fs/littlefs"]}),
    ("smpsched", "kernelsrc/tests/test_smp_sched_host.c",
     ["kernelsrc/src/arkrtos_smp_sched.c"],
     ["-DARKRTOS_USE_SMP_SCHED=1", "-DARKRTOS_USE_SMP=1",
      "-DARKRTOS_CPU_NUM=3", "-DARKRTOS_TIME_SLICE_TICKS=1",
      "-fno-asynchronous-unwind-tables"],
     "SMP-SCHED-HOST OK"),
    ("smpschedoff", "kernelsrc/tests/test_smp_sched_host.c",
     ["kernelsrc/src/arkrtos_smp_sched.c"],
     ["-DARKRTOS_USE_SMP_SCHED=0",
      "-fno-asynchronous-unwind-tables"],
     "SMP-SCHED-HOST OK"),
    ("softbus", "kernelsrc/tests/test_softbus_host.c",
     ["kernelsrc/src/arkrtos_softbus.c"],
     SOFTBUS_DEFS + ["-fno-asynchronous-unwind-tables"],
     "SOFTBUS-HOST OK",
     {"incs": SOFTBUS_INCS, "link": SOFTBUS_LINK}),
    ("softbusoff", "kernelsrc/tests/test_softbus_host.c",
     ["kernelsrc/src/arkrtos_softbus.c"],
     ["-DARKRTOS_USE_SOFTBUS=0", "-fno-asynchronous-unwind-tables"],
     "SOFTBUS-HOST OK",
     {"incs": SOFTBUS_INCS}),
    ("net", "kernelsrc/tests/test_net_host.c",
     ["kernelsrc/src/arkrtos_net.c"],
     ["-DARKRTOS_USE_LWIP=1", "-DARKRTOS_USE_MQ=1",
      "-DARKRTOS_NET_HOST_TEST=1", "-DARKRTOS_LOG_LEVEL=0",
      "-fno-asynchronous-unwind-tables"],
     "NET-HOST OK",
     {"incs": ["-Ikernelsrc/tests/net_stub"]}),
    ("netoff", "kernelsrc/tests/test_net_host.c",
     ["kernelsrc/src/arkrtos_net.c"],
     ["-DARKRTOS_USE_LWIP=0", "-DARKRTOS_LOG_LEVEL=0",
      "-fno-asynchronous-unwind-tables"],
     "NET-HOST OFF OK"),
    # --- 2026-11-22：ELF 装载器（静态档）。窗口是真的映射出来的：装载是
    # 直接 memcpy 到 VA，宿主上不映射就是往随机地址写，所以用例自己
    # VirtualAlloc / mmap(MAP_FIXED) 占住这块窗口。
    ("elf", "kernelsrc/tests/test_elf_host.c",
     ["kernelsrc/src/arkrtos_elf.c"],
     ["-DARKRTOS_USE_ELF=1",
      "-DARKRTOS_ELF_WINDOW_BASE=0x60000000u",
      "-DARKRTOS_ELF_WINDOW_SIZE=0x200000u",
      "-DARKRTOS_ELF_ARGV_SIZE=16384u",
      "-DARKRTOS_ELF_STACK_SIZE=32768u",
      "-DARKRTOS_LOG_LEVEL=0",
      "-fno-asynchronous-unwind-tables"],
     "ELF-HOST OK"),
    ("elfoff", "kernelsrc/tests/test_elf_host.c",
     ["kernelsrc/src/arkrtos_elf.c"],
     ["-DARKRTOS_USE_ELF=0", "-DARKRTOS_LOG_LEVEL=0",
      "-fno-asynchronous-unwind-tables"],
     "ELF-HOST OFF OK"),
    # --- 2026-11-22：Linux 系统调用 ABI（开档 / 关档）---
    # 这一档必须用 qemu_loongarch64 的板级配置：只有它把 USE_PAGING /
    # USE_LINUX_ABI / USE_ELF 一起打开，copy_from_user 才是真的分页实现
    # 而不是空壳。incs 排在 COMMON_INC 之前，因此不会选中 stm32f427 的
    # 板级头（那一份会把 stm32f4xx.h 拖进来，宿主上编不过）。
    #
    # -Wl,--wrap 不是可选项：错误路径的端口能力探测靠一条 __attribute__
    # ((weak)) 的默认实现回答"不支持"，而 MinGW 在 -O0 下会把同链接里的
    # 弱定义解析到本地入口（-ffunction-sections 也救不回来，实测 call 到
    # 段外地址直接崩），所以用链接期 wrap 把引用改指到用例的强定义。
    ("linuxabi", "kernelsrc/tests/test_linux_abi_host.c",
     ["kernelsrc/src/arkrtos_linux_abi.c"],
     [BOARD_QEMU_LA64_DEFS[0], "-include", "arkrtos_board_config.h",
      SIGN_KEY_DEF, "-DARKRTOS_LINUX_ABI_SYSSTAT=1",
      "-fno-asynchronous-unwind-tables", "-ffunction-sections"],
     "LINUX-ABI HOST OK",
     {"incs": ["-Iboard/qemu_loongarch64",
               "-Ikernelsrc/port/loongarch/la64"],
      "ldflags": ["-Wl,--wrap=arkrtos_port_svc_exec_frame"]}),
    # 关档：头文件把入口折成常量宏，被测模块编成空目标（nm 自证），
    # 用例只证明"每个读数都回答没有这件事"。板级仍要一份不带
    # USE_LINUX_ABI 的配置，这里用 qemu_riscv32 那份。
    ("linuxabioff", "kernelsrc/tests/test_linux_abi_host.c",
     ["kernelsrc/src/arkrtos_linux_abi.c"],
     [BOARD_QEMU_LA64_DEFS[0], "-include", "arkrtos_board_config.h",
      SIGN_KEY_DEF, "-fno-asynchronous-unwind-tables", "-ffunction-sections"],
     "LINUX-ABI HOST OFF OK",
     {"incs": ["-Iboard/qemu_riscv32"]}),
    # --- 2026-11-22：迷你应用装载器（arkrtos_mini）-------------------------
    # 开关走「包装板级头」而不是 -D：板级头每项都是 #undef + #define 写死，
    # 命令行 -D 会被覆盖。包装头在包含板级头之后再改那四项（见文件头注释）。
    # incs 必须排在 COMMON_INC 之前，否则同名 arkrtos_board_config.h 会选中
    # stm32f427 那一份（宿主上编不过）。
    ("mini", "kernelsrc/tests/test_mini_host.c",
     ["kernelsrc/src/arkrtos_mini.c"],
     ['-DARKRTOS_BOARD_CONFIG="mini_test_board.h"',
      "-fno-asynchronous-unwind-tables"],
     "MINI HOST OK",
     {"incs": ["-Ikernelsrc/tests", "-Iboard/qemu_loongarch64"]}),
    # 关档：模块编成空目标（nm 自证），用例只证明「每个读数都回答没有」。
    ("minioff", "kernelsrc/tests/test_mini_host.c",
     ["kernelsrc/src/arkrtos_mini.c"],
     ['-DARKRTOS_BOARD_CONFIG="mini_test_board_off.h"',
      "-fno-asynchronous-unwind-tables"],
     "MINI HOST OFF OK",
     {"incs": ["-Ikernelsrc/tests", "-Iboard/qemu_loongarch64"]}),
    # --- 2026-11-22：任务/调度核心（arkrtos_task）------------------------
    # 板级头只留 task 相关开关：USE_TASK_SWAP 打开（端口层四步全部由用例
    # 打桩量指针），MINI/ELF/VFS 等全部关掉，保证只编这一个模块的路径。
    ("task", "kernelsrc/tests/test_task_host.c",
     ["kernelsrc/src/arkrtos_task.c"],
     ['-DARKRTOS_BOARD_CONFIG="task_test_board.h"',
      "-fno-asynchronous-unwind-tables"],
     "TASK HOST OK",
     {"incs": ["-Ikernelsrc/tests", "-Iboard/qemu_loongarch64"]}),
    # --- 2026-11-22：Shell 杂项命令族（arkrtos_shell_misc）----------------
    # 这一族是「只读、输出直接来自别的模块」的命令，所以被测的是它自己的
    # 格式化与退出码；每个被调到的内核读数都在用例里打桩。板级口径取
    # qemu_loongarch64 那份（SHELL 开、HW_SAFE 开），再显式打开两档可选
    # 代码：LINUX_ABI_SYSSTAT 的 lxstat 与 TASK_SWAP 的 swap。
    # swap 的成功路径在宿主上走不到（要真调度器让计数上涨），只覆盖两条
    # 拒绝路径与 stat 行；这 28 行是已知且写死的地板，不是漏测。
    ("shellmisc", "kernelsrc/tests/test_shell_misc_host.c",
     ["kernelsrc/shell/arkrtos_shell_misc.c"],
     ['-DARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
      "-DARKRTOS_LINUX_ABI_SYSSTAT=1", "-DARKRTOS_USE_TASK_SWAP=1",
      SIGN_KEY_DEF, "-fno-asynchronous-unwind-tables"]
     + ["-D" + x for x in SOFTBUS_TIER_DEFS],
     "SHELL-MISC-HOST OK",
     {"incs": ["-Iboard/qemu_loongarch64", "-Ikernelsrc/shell",
               "-Ikernelsrc/shell/ark_shell", "-Ikernelsrc/fs",
               "-Ikernelsrc/fs/littlefs",
               "-Ikernelsrc/components/ark_vfs/include",
               "-Ikernelsrc/components/ark_softbus/include"]}),
    # --- 2026-11-22：用户命令注册表（arkrtos_shell_ushell）----------------
    # SVC 0x1A 的内核侧：注册表 + 跳板。板级口径与 shellmisc 同一份；不需要
    # 额外开关。用例自己提供控制台命令表与分区表（跳板靠指针反查、靠分区
    # 表判断处理器是否还在已装载镜像内）。
    # --- 2026-11-22：可执行文件与可下载模块（arkrtos_shell_mod）-----------
    # exec / wait 走 ELF64 装载器，ld / lkup / modules / unld 走 ET_REL 模块
    # 装载器与导出表；板级口径与 ushell 同一份（qemu_loongarch64 已开
    # USE_ELF / USE_EXIT_CODE / USE_MODLOAD）。用例自己提供装载器、任务表
    # 与导出表，所以这里不需要真实的 VFS 或调度器。
    ("shellmod", "kernelsrc/tests/test_shell_mod_host.c",
     ["kernelsrc/shell/arkrtos_shell_mod.c"],
     ['-DARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
      SIGN_KEY_DEF, "-fno-asynchronous-unwind-tables"]
     + ["-D" + x for x in SOFTBUS_TIER_DEFS],
     "SHELL-MOD-HOST OK",
     {"incs": ["-Iboard/qemu_loongarch64", "-Ikernelsrc/shell",
               "-Ikernelsrc/shell/ark_shell", "-Ikernelsrc/fs",
               "-Ikernelsrc/fs/littlefs",
               "-Ikernelsrc/components/ark_vfs/include",
               "-Ikernelsrc/components/ark_softbus/include"]}),
    # --- 2026-11-22：内存池读数与 CONFIG 区（arkrtos_shell_cfg）------------
    # mpool list/test 与 cfg show/load/clear。板级口径与 shellmod 同一份；
    # 用例自己提供池对象表、layout 模块与串口，所以不需要真实 Flash 或 UART。
    ("shellcfg", "kernelsrc/tests/test_shell_cfg_host.c",
     ["kernelsrc/shell/arkrtos_shell_cfg.c"],
     ['-DARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
      SIGN_KEY_DEF, "-fno-asynchronous-unwind-tables"]
     + ["-D" + x for x in SOFTBUS_TIER_DEFS],
     "SHELL-CFG-HOST OK",
     {"incs": ["-Iboard/qemu_loongarch64", "-Ikernelsrc/shell",
               "-Ikernelsrc/shell/ark_shell", "-Ikernelsrc/fs",
               "-Ikernelsrc/fs/littlefs",
               "-Ikernelsrc/components/ark_vfs/include",
               "-Ikernelsrc/components/ark_softbus/include"]}),
    ("shellimage", "kernelsrc/tests/test_shell_image_host.c",
     ["kernelsrc/shell/arkrtos_shell_image.c"],
     ['-DARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
      SIGN_KEY_DEF, "-fno-asynchronous-unwind-tables",
      "-DARKRTOS_USE_TASK_SWAP=1"]
     + ["-D" + x for x in SOFTBUS_TIER_DEFS],
     "SHELL-IMAGE-HOST OK",
     {"incs": ["-Iboard/qemu_loongarch64", "-Ikernelsrc/shell",
               "-Ikernelsrc/shell/ark_shell", "-Ikernelsrc/fs",
               "-Ikernelsrc/fs/littlefs",
               "-Ikernelsrc/components/ark_vfs/include",
               "-Ikernelsrc/components/ark_softbus/include"]}),
    # --- 2026-11-22：控制台核心（arkrtos_shell）------------------------------
    # 命令注册表（57 条内核命令逐个核对 name/handler/help/max_args）、控制台
    # 任务的收字节预算与超长行丢弃、任务注册参数、用户命令卸载。外部符号
    # （ark_shell_* / arkrtos_task_* / 57 个 cmd_*）全部由用例桩住。
    ("shellcore", "kernelsrc/tests/test_shell_core_host.c",
     ["kernelsrc/shell/arkrtos_shell.c"],
     ['-DARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
      SIGN_KEY_DEF, "-fno-asynchronous-unwind-tables",
      "-DARKRTOS_USE_TASK_SWAP=1"]
     + ["-D" + x for x in SOFTBUS_TIER_DEFS],
     "SHELL-CORE-HOST OK",
     {"incs": ["-Iboard/qemu_loongarch64", "-Ikernelsrc/shell",
               "-Ikernelsrc/shell/ark_shell", "-Ikernelsrc/fs",
               "-Ikernelsrc/fs/littlefs",
               "-Ikernelsrc/components/ark_vfs/include",
               "-Ikernelsrc/components/ark_softbus/include"]}),
    # --- 2026-11-23：分页命令族（arkrtos_shell_pmap）----------------------
    # map / check / rd / rb / wrb / unmap / dyn / frame / ftest / test / rel /
    # space / pairs / look / u1 / elf / u2 / cu 十八个子命令，逐条走拒绝路径
    # 与打印分支。板级口径与 shellcore 同一份，但板级头换成本目录里的
    # host_board_la64.h：pmap test 会直接解引用 ARKRTOS_MPU_KERNEL_ROM_BASE，
    # 而真板级值 0x00200000 在 Windows 进程里被主线程栈保留区占着（实测
    # VirtualAlloc 拿不到），这个包装头只把 CHIP_FLASH_BASE 挪到 0x02000000，
    # 其余板级开关原样沿用。所以 -Ikernelsrc/tests 必须排在 include 最前。
    # 覆盖率地板 99.4%：ftest 的"图案读回不符"分支在普通宿主内存上不可达
    # （写什么读什么），用例里如实登记为故意不覆盖，不用故障注入伪造。
    ("shellpmap", "kernelsrc/tests/test_shell_pmap_host.c",
     ["kernelsrc/shell/arkrtos_shell_pmap.c"],
     ['-DARKRTOS_BOARD_CONFIG="host_board_la64.h"',
      SIGN_KEY_DEF, "-fno-asynchronous-unwind-tables",
      "-DARKRTOS_USE_TASK_SWAP=1"]
     + ["-D" + x for x in SOFTBUS_TIER_DEFS],
     "SHELL-PMAP-HOST OK",
     {"incs": ["-Ikernelsrc/tests", "-Iboard/qemu_loongarch64",
               "-Ikernelsrc/shell", "-Ikernelsrc/shell/ark_shell",
               "-Ikernelsrc/fs", "-Ikernelsrc/fs/littlefs",
               "-Ikernelsrc/components/ark_vfs/include",
               "-Ikernelsrc/components/ark_softbus/include"]}),
    ("shelldiag", "kernelsrc/tests/test_shell_diag_host.c",
     ["kernelsrc/shell/arkrtos_shell_diag.c"],
     ['-DARKRTOS_BOARD_CONFIG="diag_test_board.h"',
      "-DARKRTOS_USE_RTSTAT=1", "-DARKRTOS_USE_TICKLESS=1",
      SIGN_KEY_DEF, "-fno-asynchronous-unwind-tables"]
     + ["-D" + x for x in SOFTBUS_TIER_DEFS],
     "SHELL-DIAG-HOST OK",
     {"incs": ["-Ikernelsrc/tests", "-Iboard/qemu_loongarch64",
               "-Ikernelsrc/shell", "-Ikernelsrc/shell/ark_shell",
               "-Ikernelsrc/fs", "-Ikernelsrc/fs/littlefs",
               "-Ikernelsrc/components/ark_vfs/include",
               "-Ikernelsrc/components/ark_softbus/include"]}),
    ("shellplat", "kernelsrc/tests/test_platform_shell_host.c",
     ["kernelsrc/shell/platform_arkrtos.c"],
     ['-DARKRTOS_BOARD_CONFIG="host_board_la64.h"', SIGN_KEY_DEF,
      "-fno-asynchronous-unwind-tables"],
     "PLATFORM-SHELL-HOST OK",
     {"incs": ["-Ikernelsrc/tests", "-Iboard/qemu_loongarch64",
               "-Ikernelsrc/shell", "-Ikernelsrc/shell/ark_shell"]}),
    # --- 2026-11-23：块设备 / littlefs 门面 / VFS 命令 / mini / pwd-cd-df ---
    # 用例自己提供两块内存 blk、littlefs 卷桩、VFS 命名空间与可编程的
    # mini/crash，所以不需要真 Flash 或真实文件系统。板级口径用 F427 而不是
    # qemu_loongarch64：后者把 MINIAPP 关掉会让 cmd_mini 不参与编译，链接
    # 报 bad reloc。覆盖率地板 99.4%：剩下 7 行是防御分支，用例里写明不可达。
    ("shellfs", "kernelsrc/tests/test_shell_fs_host.c",
     ["kernelsrc/shell/arkrtos_shell_fs.c"],
     F427_DEFS + [SIGN_KEY_DEF, "-fno-asynchronous-unwind-tables",
                  "-DARKRTOS_USE_TASK_SWAP=1"],
     "SHELL-FS-HOST OK",
     {"incs": ["-Ikernelsrc/tests"] + F427_INCS}),
    ("ushell", "kernelsrc/tests/test_shell_ushell_host.c",
     ["kernelsrc/shell/arkrtos_shell_ushell.c"],
     ['-DARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
      SIGN_KEY_DEF, "-fno-asynchronous-unwind-tables"]
     + ["-D" + x for x in SOFTBUS_TIER_DEFS],
     "USHELL-HOST OK",
     {"incs": ["-Iboard/qemu_loongarch64", "-Ikernelsrc/shell",
               "-Ikernelsrc/shell/ark_shell", "-Ikernelsrc/fs",
               "-Ikernelsrc/fs/littlefs",
               "-Ikernelsrc/components/ark_vfs/include",
               "-Ikernelsrc/components/ark_softbus/include"]}),
]


def find_cc(explicit):
    for cand in ([explicit] if explicit else []) + GCC_FALLBACKS:
        if not cand:
            continue
        if os.path.sep in cand or "/" in cand:
            if os.path.isfile(cand):
                return cand
        else:
            hit = shutil.which(cand)
            if hit:
                return hit
    return None


def mingw_prefix(cc):
    """MinGW 的 gcc/gcov 要 -B<dir> 才找得到自己的 cc1 / 运行库。"""
    norm = cc.replace("\\", "/").lower()
    if "mingw" in norm:
        return ["-B" + os.path.dirname(cc)]
    return []


def _run(cmd, cwd=None):
    """跑一条命令并把输出解出来。MinGW 的 gcc/gcov 用本地代码页（GBK）输出，
    直接按 UTF-8 解码会在中文诊断上抛异常，所以这里按顺序试。"""
    r = subprocess.run(cmd, cwd=cwd, capture_output=True)

    def dec(b):
        for enc in ("utf-8", "gbk"):
            try:
                return b.decode(enc)
            except Exception:
                pass
        return b.decode("latin-1")

    return r.returncode, dec(r.stdout), dec(r.stderr)


def parse_lines_executed(txt, src_base):
    """从 gcov 文本里取该源文件的 Lines executed 百分比。"""
    want = "File '" + src_base + "'"
    cur = None
    for line in txt.splitlines():
        line = line.strip()
        if line.startswith("File '"):
            cur = line
        elif cur and cur.startswith(want) and line.startswith("Lines executed:"):
            val = line.split("Lines executed:")[1].split("%")[0]
            return float(val)
    # gcov 只处理一个源文件时，标题行可能被省略
    for line in txt.splitlines():
        line = line.strip()
        if line.startswith("Lines executed:"):
            return float(line.split("Lines executed:")[1].split("%")[0])
    return None


def parse_gcov_sets(path):
    """从 .gcov 里取两套行号：被执行过的，与该档编进去的。

    gcov 的文本格式：<count>:<行号>:<源行>，count 为数字表示执行过，
    "#####" 表示没执行，"-" 表示这一行不生成代码。branch / function
    行不是这种格式，会被两个 try/int 自然过滤掉。

    为什么要行号而不直接拿百分比：同一个 .c 在不同宏档下编出来的是不同的
    代码（#if 的一半只在某一档存在，blk 就是如此）。单个用例的百分比只回答
    "这一档走了多少"，而问题问的是"这行代码有没有被跑过"。所以这里把行号
    集交出去，由 main() 按模块求并集再算百分比。
    """
    ex   = set()
    inst = set()
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split(":", 2)
            if len(parts) < 3:
                continue
            head = parts[0].strip()
            try:
                n = int(parts[1])
            except ValueError:
                continue
            if head == "-":
                continue
            if head == "#####":
                inst.add(n)
            elif head.isdigit() or head.endswith("*"):
                inst.add(n)
                ex.add(n)
    return ex, inst

def _object_is_empty(cc, obj):
    """目标里除了 .text/.data/.bss 这类段标记之外还有没有符号。

    nm 对空目标输出形如 "00000000 t .text" 的几行段标记（符号名以点开头）。
    只要出现任何一条真符号——已定义或未定义都算——就不是"空编译"，那时
    gcov 的 "no functions found" 属于测量故障，必须抛出去而不是记成 0 行。
    """
    nm = os.path.join(os.path.dirname(cc), "nm.exe" if os.name == "nt" else "nm")
    if not os.path.isfile(nm):
        hit = shutil.which("nm")
        if not hit:
            raise RuntimeError("找不到 nm（应与 gcc 同目录）")
        nm = hit
    rc, out, err = _run([nm, obj])
    if rc != 0:
        raise RuntimeError("nm 读不了 %s: %s" % (obj, (err or out).strip()[-200:]))
    for line in (out or "").splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        if not parts[-1].startswith("."):
            return False
    return True

def pct_of(ex, inst):
    """行覆盖率 = 跑过的行 / 编进去的行。分母为 0 时返回 0（不造假）。"""
    return (100.0 * len(ex) / len(inst)) if inst else 0.0

def _case(case):
    """把 CASES 里的一行摊平成 (name, test, mods, defs, markers, incs, argv)。"""
    name, test, mods, defs, marker = case[:5]
    extra = case[5] if len(case) > 5 else {}
    markers = [marker] if isinstance(marker, str) else list(marker)
    return (name, test, mods, defs, markers,
            list(extra.get("incs", [])), list(extra.get("argv", [])))

def measure(cc, name, test, mods, defs, markers, incs=(), argv=(), link=(),
            ldflags=()):
    """编 + 跑 + gcov，返回 {模块基名: 行覆盖率}；失败抛 RuntimeError。

    link 里的源文件会被编进来参与链接，但不做 gcov 统计——用于"必须真的链接
    生产实现、而覆盖率只记被测模块"的场景（如 apppack 用例链真 loader）。
    """
    base_cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0", "--coverage"]
                + defs + incs + COMMON_INC)
    with TempDir() as td:
        objs = []
        objmap = {}
        for src in [test] + list(mods) + list(link):
            obj = os.path.join(td, os.path.basename(src)[:-2] + ".o")
            cmd = base_cmd + ["-c", "-o", obj, os.path.abspath(src)]
            rc, _out, err = _run(cmd, cwd=ROOT)
            if rc != 0:
                raise RuntimeError("编译失败 %s: %s" % (src, err.strip()[-300:]))
            objs.append(obj)
            objmap[os.path.basename(src)[:-2]] = obj
        exe = os.path.join(td, "cov_" + name + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["--coverage", "-o", exe] + objs
               + list(ldflags))
        rc, _out, err = _run(cmd, cwd=ROOT)
        if rc != 0:
            raise RuntimeError("链接失败 %s: %s" % (name, err.strip()[-300:]))
        # 在临时目录里跑，.gcda 才会落在能读到的地方
        rc, out, err = _run([exe] + [str(a) for a in argv], cwd=td)
        out = out + err
        missing = [m for m in markers if m not in out]
        if rc != 0 or missing:
            raise RuntimeError("用例没通过 %s（缺 %s）: %s"
                               % (name, ",".join(missing), out.strip()[-300:]))

        gcov = os.path.join(os.path.dirname(cc),
                            "gcov.exe" if os.name == "nt" else "gcov")
        if not os.path.isfile(gcov):
            hit = shutil.which("gcov")
            if not hit:
                raise RuntimeError("找不到 gcov（应与 gcc 同目录）")
            gcov = hit
        res = {}
        for m in mods:
            base = os.path.basename(m)[:-2]
            # 注意：gcov 不认 gcc 的 -B，传了会 "invalid option -- B"
            rc, out, err = _run([gcov, "-o", td, os.path.abspath(m)], cwd=td)
            txt = out + err
            pct = parse_lines_executed(txt, os.path.basename(m))
            if pct is None:
                # 有两种完全不同的原因会让 gcov 说 "no functions found"：
                # 一是这一档真的把模块编成了空目标（特性关掉、文件里一个函数
                # 都不剩，export-off 就是这种），那不是"没跑到"，是"0 行编入"；
                # 二是测量本身出了问题（目标里有符号但 gcov 读不出来），那种
                # 必须报错。所以先用 nm 自己核一遍目标是不是真的空的，核过才
                # 记 0 行；核不过照旧抛。空档不会假装 100%，模块的覆盖率由
                # 开档那份供，main() 里的行号并集照算。
                if not _object_is_empty(cc, objmap[base]):
                    raise RuntimeError("gcov 没给出覆盖率 %s（目标非空，属测量故障）: %s"
                                       % (base, txt.strip()[-300:]))
                res[base] = (None, set(), set())
                continue
            gcovfile = os.path.join(td, os.path.basename(m) + ".gcov")
            if os.path.isfile(gcovfile):
                ex, inst = parse_gcov_sets(gcovfile)
            else:
                ex, inst = set(), set()
            res[base] = (pct, ex, inst)
        return res


def load_baseline():
    if not os.path.isfile(BASELINE):
        return {}
    with open(BASELINE, "r", encoding="utf-8") as f:
        return json.load(f)


def save_baseline(data):
    # 钉两位小数：门禁判据带 0.005 的容差，多留的全精度会让"基线文件里的值"
    # 看起来比之前低一点点（例如 92.02 -> 92.01680），读起来像偷偷降了门槛。
    pinned = {k: round(v + 1e-9, 2) for k, v in data.items()}
    with open(BASELINE, "w", encoding="utf-8", newline="\n") as f:
        json.dump(pinned, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gcc", default=None)
    ap.add_argument("--update", action="store_true",
                    help="用实测值重写基线（只在明确理由下用）")
    args = ap.parse_args()

    cc = find_cc(args.gcc)
    if not cc:
        print("COVERAGE-GATE FAIL: 没有宿主 C 编译器（设置 ARKRTOS_HOST_CC）")
        return 1

    baseline = load_baseline()
    merged = {}         # 模块基名 -> [被执行行集, 编进去的行集]
    bad = 0
    for case in CASES:
        name, test, mods, defs, markers, incs, argv = _case(case)
        extra = case[5] if len(case) > 5 else {}
        try:
            res = measure(cc, name, test, mods, defs, markers, incs, argv,
                          list(extra.get("link", [])),
                          list(extra.get("ldflags", [])))
        except RuntimeError as exc:
            print("FAIL %-10s %s" % (name, exc))
            bad += 1
            continue
        for base, (pct, ex, inst) in sorted(res.items()):
            slot = merged.setdefault(base, [set(), set()])
            slot[0] |= ex
            slot[1] |= inst
            # 单档读数只作参考行，不参与判（判的是下面的并集）
            if pct is None:
                print("case %-24s  空目标（特性关掉，0 行编入）  [%s]" % (base, name))
            else:
                print("case %-24s %6.2f%%  [%s]" % (base, pct, name))

    measured = {}
    for base, (ex, inst) in sorted(merged.items()):
        pct = pct_of(ex, inst)
        measured[base] = pct
        floor = baseline.get(base)
        if args.update or floor is None:
            print("new  %-24s %6.2f%%  (无基线，本次记下)" % (base, pct))
        elif pct + 0.005 < floor:
            print("FAIL %-24s %6.2f%%  < 基线 %.2f%%" % (base, pct, floor))
            bad += 1
        else:
            print("ok   %-24s %6.2f%%  (基线 %.2f%%)" % (base, pct, floor))

    if args.update:
        save_baseline(measured)
        print("基线已写入 %s（%d 个模块）" % (os.path.relpath(BASELINE, ROOT), len(measured)))

    if bad:
        print("COVERAGE-GATE FAIL: %d 处不达标" % bad)
        return 1
    print("COVERAGE-GATE OK  (%d 个模块，行覆盖率均不低于基线)" % len(measured))
    return 0


if __name__ == "__main__":
    sys.exit(main())
