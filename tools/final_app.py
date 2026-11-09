#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""arkc 产物上板运行 —— 板级回归（M1 固定窗口 / M2 可重定位）。

这一份是「已经上板验证过的结论」的可重复版本：把 M1、M2 的判据写成
可以在一条命令里跑完的用例，每条只有两种结果——PASS 或 FAIL，没有「看起来
应该没问题」。

为什么这么做：M1/M2 的结论都建立在串口上出现的**源码独有字符串**与
`mini stat` 里的**落点/块号**上。这些读数人工看一遍很容易看错（尤其「跑起来
了」和「跑的是上一份镜像」在串口上长得很像），所以它们必须由脚本比对：
  * 期望串用正则表达**完整一行**，不用子串包含（"R2 alive" 是 "R2 alive
    (stale)" 的子串）；
  * 每次运行前清掉 /tmp 上的同名文件（ramfs 只有 3 个槽，残留文件会让
    打包在第四份时失败，而失败原因跟被测对象无关）；
  * 反假绿用例与被测用例写在同一个文件里：改 `.sct` 让窗口对不上时，M1
    必须**报错**而不是恰好跑通。

用法：
    py -3 tools/final_app.py                     # 烧当前 build 再跑全部用例
    py -3 tools/final_app.py --noflash           # 不烧，直接跑（板子已有固件）
    py -3 tools/final_app.py --only m2-pack      # 只跑名字里含 m2-pack 的用例
    py -3 tools/final_app.py --list              # 只列用例

前置：
  * 板子接在 `--port`（默认 COM3）上，115200；
  * 固件按 `USE_APPRUN=1 USE_ARKC=1 USE_APPPACK=1 USE_MINI_VFS=1` 构建，
    M2 的「两个落点」用例还要求 `MINI_PRIORITY` 低于控制台（见用例注释）；
  * 烧录走 OpenOCD（路径可用环境变量覆盖，见下）。

退出码：0 = 全部 PASS（或按 --only/--list 无事可做），1 = 有 FAIL 或环境问题。
"""
import argparse
import os
import re
import subprocess
import sys
import threading
import time

# ---------------------------------------------------------------- 环境与常量

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.path.join(REPO, "example", "stm32h743", "kernel",
                           "ARKRTOS_TEST", "GCC", "build", "ARKRTOS_TEST.bin")

CUBE = os.environ.get(
    "ARK_CUBEIDE",
    r"D:\ST\STM32CubeIDE_1.16.1\STM32CubeIDE\plugins")
OCD = os.environ.get("ARK_OPENOCD", os.path.join(
    CUBE, "com.st.stm32cube.ide.mcu.externaltools.openocd.win32_2.3.200.202404091248",
    "tools", "bin", "openocd.exe"))
OCD_SCRIPTS = os.environ.get("ARK_OPENOCD_SCRIPTS", os.path.join(
    CUBE, "com.st.stm32cube.ide.mcu.debug.openocd_2.2.100.202406131243",
    "resources", "openocd", "st_scripts"))
OCD_CFG = os.path.join(os.environ.get("TEMP", "."), "ocd.cfg")

# 串口每行之间的默认等待：编四遍约 0.3 s，装载 + 起任务约 0.1 s，剩下的余量
# 是给 shell 把回显与日志刷完。写死为「够用且不浪费时间」的一档，个别行可以
# 在自己的 Step 里覆盖。
DEFAULT_WAIT = 1.6

# ---------------------------------------------------------------- 用例数据


class Step(object):
    """一条要发到 shell 的命令，以及它在串口上必须/不得留下的痕迹。

    expect / forbid 都是正则，且用 search() 语义——所以写的时候要自己带上
    边界（例如 r"^R2 alive$" 配 re.M），否则子串匹配会放过「上一份镜像」。
    """

    def __init__(self, cmd, expect=None, forbid=None, wait=DEFAULT_WAIT, check=None):
        self.cmd = cmd
        self.expect = expect or []
        self.forbid = forbid or []
        self.wait = wait
        # check: 可选的「按本行输出现算期望」的判据（expect 只能写死正则，
        # 写死就意味着把判据改成「这块板今天几 B 一块」，而不是被测对象）。
        # 回调收到清理后的输出，返回问题字符串列表（空 = 通过）。
        self.check = check


class Case(object):
    def __init__(self, name, why, steps):
        self.name = name
        self.why = why          # 这条用例在证明什么（打印给人看）
        self.steps = steps

class Reset(object):
    """复位板子（不重烧、不断电），然后把复位后那段输出当一次「步骤输出」判。

    为什么需要它：有一类行为只在**开机那一刻**发生（卷挂上时的自启），
    串口上敲命令复现不了。不能靠「重烧」代替复位——重烧会重新写 flash，
    而这里要证明的恰恰是「同一个固件、同一块 NOR，换个开机就自动跑起来」。

    expect/forbid 与 Step 完全同一套语义（search + re.M），所以写的时候
    仍然要自己带边界。wait 是从 reset 命令返回后开始 Drain 的时长，要盖住
    banner + 挂载 + 装载，默认给得比较宽。
    """

    def __init__(self, wait=8.0, expect=None, forbid=None, check=None):
        self.cmd = "<reset>"
        self.expect = expect or []
        self.forbid = forbid or []
        self.wait = wait
        self.check = check


# --- 源码：两个都被 M1/M2 实测过。不要随手加长：板的 arkc 池只有 80 KB，
#     809 B 的源在 pass A 就会 memory pool exhausted（实测），所以这里给的是
#     能过四遍流水线的下限附近。
SRC_M1 = [
    "typedef unsigned int u32;",
    "extern void __arkcc_init_globals(void);",
    "int __svc(0x1A) svc_shell(u32 *p);",
    "int __svc(0x1B) svc_thread(u32 *p);",
    "volatile u32 g_anchor = 0xA5A5A5A5u;",
    "void Reset_Handler(void)",
    "{",
    "u32 p[3];",
    "__arkcc_init_globals();",
    'p[0] = 3u; p[1] = (u32)"M1 alive\\r\\n"; p[2] = 0u;',
    "if (g_anchor == 0xA5A5A5A5u) { svc_shell(p); }",
    'p[1] = (u32)"M1 tag alpha\\r\\n";',
    "svc_shell(p);",
    "p[0] = 3u; p[1] = 0u; p[2] = 0u;",
    "svc_thread(p);",
    "for (;;) { }",
    "}",
]

# M1 的窗口由 .sct 决定。格式与 tools/gen_app_sct.py 输出的 dev 形态同构：
# 一行 LR_ 头 + 一行 RO + 一行 RW。两个基址/长度必须与
# config/stm32h743/arkrtos_partition.h 的 APPRUN_* 一致，否则 app run 会
# （正确地）报 -23：初始 SP = RAM 窗口顶，对不上就是「为别的布局链的镜像」。
SCT_OK = [
    "LR_IROM1 0x24070000 0x00002000",
    "{",
    "ER_IROM1 0x24070000 0x00002000 { *.o (RESET, +First) .ANY (+RO) }",
    "ER_IRAM1 0x24072000 0x00002000 { .ANY (+RW +ZI) }",
    "}",
]
# 反假绿：代码窗换个地方、RAM 窗跟着换，长度也换成 0x3000（于是初始 SP 变成
# 0x24076000）。这份镜像必须被拒——若它跑通，说明窗口校验没在起作用。
SCT_BAD = [
    "LR_IROM1 0x24040000 0x00002000",
    "{",
    "ER_IROM1 0x24040000 0x00002000 { *.o (RESET, +First) .ANY (+RO) }",
    "ER_IRAM1 0x24043000 0x00002000 { .ANY (+RW +ZI) }",
    "}",
]

# 阶段 3c：同一份源码换一条链接链——text/rodata 链到片内 Flash 应用区
# （0x081E0000，bank2 尾部 128 KB），data/bss/stack 仍链到 APPRUN 的 RAM
# 数据窗口（0x24072000/8K，于是初始 SP = 0x24074000）。SP 这条与 app run
# 完全相同：flash app 与 app run 共用数据窗口，所以窗口校验天然通用。
# 区别只有一处——代码不再进 RAM，掉电不丢。
SRC_M5 = [
    "typedef unsigned int u32;",
    "extern void __arkcc_init_globals(void);",
    "int __svc(0x1A) svc_shell(u32 *p);",
    "int __svc(0x1B) svc_thread(u32 *p);",
    "volatile u32 g_anchor = 0x5A5A5A5Au;",
    "void Reset_Handler(void)",
    "{",
    "u32 p[3];",
    "__arkcc_init_globals();",
    'p[0] = 3u; p[1] = (u32)"M5 alive in flash\\r\\n"; p[2] = 0u;',
    "if (g_anchor == 0x5A5A5A5Au) { svc_shell(p); }",
    'p[1] = (u32)"M5 tag bravo\\r\\n";',
    "svc_shell(p);",
    "p[0] = 3u; p[1] = 0u; p[2] = 0u;",
    "svc_thread(p);",
    "for (;;) { }",
    "}",
]

SCT_M5 = [
    "LR_IROM1 0x081E0000 0x00020000",
    "{",
    "ER_IROM1 0x081E0000 0x00020000 { *.o (RESET, +First) .ANY (+RO) }",
    "ER_IRAM1 0x24072000 0x00002000 { .ANY (+RW +ZI) }",
    "}",
]
# 反假绿：RAM 数据窗口挪 4 KB（于是初始 SP 变成 0x24075000）。这份镜像必须
# 在「擦之前」就被拒——它是这条用例的第二判据：先验后擦。
SCT_M5_BAD = [
    "LR_IROM1 0x081E0000 0x00020000",
    "{",
    "ER_IROM1 0x081E0000 0x00020000 { *.o (RESET, +First) .ANY (+RO) }",
    "ER_IRAM1 0x24073000 0x00002000 { .ANY (+RW +ZI) }",
    "}",
]

# M2 用一份「自己量落点」的程序：块号 = (字面量绝对地址 - 池基址) >> 12，
# 打成 '0'+n 一个字输出。所以串口上的单个数字不是推断，是程序自己算的。
SRC_M2 = [
    "typedef unsigned int u32;",
    "extern void __arkcc_init_globals(void);",
    "int __svc(0x1A) svc_shell(u32 *p);",
    "volatile u32 g_anchor = 0xA5A5A5A5u;",
    "void Reset_Handler(void)",
    "{",
    "u32 p[3];",
    "u32 w[2];",
    "u32 d;",
    "__arkcc_init_globals();",
    'p[0] = 3u; p[1] = (u32)"M2 alive\\r\\n"; p[2] = 0u;',
    "svc_shell(p);",
    'd = (u32)"M2 alive\\r\\n" - 0x24070000u;',
    "d = d >> 12;",
    "w[0] = 0x30u + d;",
    "if (d > 9u) { w[0] = 0x3Fu; }",
    "if (g_anchor == 0u) { w[0] = 0x21u; }",
    "w[0] = w[0] | 0x000A0D00u;",
    "w[1] = 0u;",
    "p[1] = (u32)w;",
    "svc_shell(p);",
    "for (;;) { }",
    "}",
]


# --- M3-d 跨复位自启：被测程序 -------------------------------------------
# 这个程序只有一个作用：打一行**只有它有**的字符串然后自己退出。
# 它必须自己退出，不能死循环：自启的判据不是「窗口被占住了」，而是「没人敲
# mini run，这行字也出现在串口上」；若它死循环，就会把窗口与单实例闸门一起
# 占住，后面的用例全毁。
SRC_AUTO = [
    "typedef unsigned int u32;",
    "extern void __arkcc_init_globals(void);",
    "int __svc(0x1A) svc_shell(u32 *p);",
    "int __svc(0x1B) svc_thread(u32 *p);",
    "void Reset_Handler(void)",
    "{",
    "u32 p[3];",
    "__arkcc_init_globals();",
    'p[0] = 3u; p[1] = (u32)"C3 autostart alive\\r\\n"; p[2] = 0u;',
    "svc_shell(p);",
    "p[0] = 3u; p[1] = 0u; p[2] = 0u;",
    "svc_thread(p);",
    "for (;;) { }",
    "}",
]

# --- M2 判据 C：跨翻译单元（两源 -> 两对象 -> 一次 -link -> app run）------
# 判据 C 要的是「四类绝对地址各至少命中一次」：
#   ① 带初值的全局      g_init = 7          （.data，落在 ER_IRAM1）
#   ② 字符串字面量      tag = "C1 tag alpha"（.rodata，落在 ER_IROM1）
#   ③ 函数指针          fp = b_add           （跨 TU 的函数地址，靠重定位）
#   ④ 跨文件调用        fp(g_init, 5) == 12  （b.c 里的函数，另一个对象）
# 真值由程序自己算：7 + 5 = 12 就打 "C1 ok"，否则 "C1 BAD"——判据不是"有没有
# 输出"，而是"这个数字是不是用对了地址算出来的"。写错重定位会得到 BAD 或
# 直接跑飞（HARD/MEMFAULT），两种都在 forbid 里。
SRC_C1A = [
    "typedef unsigned int u32;",
    "extern void __arkcc_init_globals(void);",
    # p[0] 是 SVC 0x1A 的子命令，不是参数个数：1=注册 2=注销 3=打印。
    # 写错成 2 的后果是"注销一个名叫 C1 tag alpha 的命令"——SVC 返回错误码、
    # 屏幕上一个字都不出。第一版就是这么写的，板上表现为 app run 打印
    # "started (task 4)" 之后彻底安静，既不报错也不退出（下面还挂着死循环）。
    "int __svc(0x1A) svc_shell(u32 *p);",
    # 0x1B 子命令 3 = 当前线程退出。宿主镜像必须显式走这一条：
    # app run 的窗口只在"任务退出"时归还（arkrtos_apprun_on_task_exit），
    # 挂一个死循环会把窗口和单实例闸门一起永久占住——板上实测只剩
    # 复位一条路，而复位会清掉 ramfs，编译产物全丢。
    "int __svc(0x1B) svc_thread(u32 *p);",
    "int g_init = 7;",
    'const char tag[] = "C1 tag alpha\\r\\n";',
    "extern int b_add(int a, int v);",
    "int (*fp)(int, int) = b_add;",
    "void Reset_Handler(void)",
    "{",
    "u32 p[4];",
    "int r;",
    "__arkcc_init_globals();",
    "p[0] = 3u; p[1] = (u32)tag; p[2] = 0u;",
    "svc_shell(p);",
    "r = fp(g_init, 5);",
    "p[0] = 3u;",
    'p[1] = (u32)((r == 12) ? "C1 ok\\r\\n" : "C1 BAD\\r\\n");',
    "p[2] = 0u;",
    "svc_shell(p);",
    "p[0] = 3u; p[1] = 0u; p[2] = 0u;",
    "svc_thread(p);",
    "for (;;) { }",
    "}",
]

# b.c 自己也带一个初值全局：这样第二个对象同样有 .data 窗口，链接器必须把
# 它挪到第一个对象之后（--reloc-data）并把 b_add 里烤进去的地址改成新基址，
# 同时把两个对象的 __arkcc_init_globals 链起来各跑一遍。没有这一项就只能
# 验到"跨文件调用"，验不到"每个对象的 .data 都初始化对了"——实测缺它时
# 第二份 init 体被同名合并吃掉，b_bias 停在 0，结果算成 7 而不是 12。
SRC_C1B = [
    "int b_bias = 5;",
    "int b_add(int a, int v)",
    "{",
    "return a + v + (b_bias - 5);",
    "}",
]

# 散列文件与 M1 同款窗口：代码 0x24070000/0x2000、RAM 0x24072000/0x2000。
# 只有第一个对象带 --vector-table（0x400 的洞），第二个不必——两个都带会白扔 1 KB。
SCT_C1 = SCT_OK

def _emit(path, lines):
    """把源码/散列文件写进板上 ramfs。用 shell 的 echo（shell 只有 128 B 行缓冲，
    这些行都远短于它）。"""
    out = ["rm %s" % path]
    for line in lines:
        out.append("echo '%s' >> %s" % (line, path))
    return out


def _steps_emit(path, lines, wait=DEFAULT_WAIT):
    return [Step(c, wait=0.25) for c in _emit(path, lines)]


# ---------------------------------------------------------------- 用例定义

# 每个用例开工前统一清场。ramfs 的文件槽数由 ARK_RAMFS_MAX_FILES 决定
# （片内 RAM 档 3 槽，SDRAM 档 8 槽），残留一份就可能让被测操作撞上「槽用完」
# 并报 I/O error——那个失败与被测对象无关，却会让人去查打包器（本轮实测踩过）。
CLEAN = [
    # 每个用例都从根目录开工：`make-demo` 会 cd /demo，它半路 FAIL 时后面的
    # 用例不该继承那个 cwd——相对路径的成败取决于它，而这跟被测对象无关。
    Step("cd /", wait=0.5),
    Step("rm /tmp/m1.c", wait=0.25), Step("rm /tmp/m1.sct", wait=0.25),
    Step("rm /tmp/m1.bin", wait=0.25), Step("rm /tmp/m2.c", wait=0.25),
    Step("rm /tmp/m2.svcm", wait=0.25), Step("rm /tmp/pp.sct", wait=0.25),
    # 阶段 3c 的四个文件（两源 + 两散列 + 两产物）。ramfs 槽数是硬上限，
    # 一个用例半路 FAIL 留下的文件会让下一个用例撞上「槽用完」。
    # 小程序的两个用例：一个在 /tmp 里建一份"缺 MiniMain"的工程，一个在
    # /mnt/nor 上构建现成的工程（那份是设备上常驻的，不能删）。ramfs 在 SDRAM
    # 档只有 11 个槽，上一轮半路 FAIL 留下的文件会把槽占住。
    Step("rm /tmp/nmk", wait=0.25), Step("rm /tmp/nom.svcm", wait=0.25),
    # 判据 C 的六个文件（两源 + 两对象 + 散列 + 产物）
    Step("rm /tmp/c1a.c", wait=0.25), Step("rm /tmp/c1b.c", wait=0.25),
    Step("rm /tmp/c1.sct", wait=0.25), Step("rm /tmp/c1a.obj", wait=0.25),
    Step("rm /tmp/c1b.obj", wait=0.25), Step("rm /tmp/c1.bin", wait=0.25),
    Step("rm /tmp/m5.c", wait=0.25), Step("rm /tmp/m5.sct", wait=0.25),
    Step("rm /tmp/m5b.sct", wait=0.25), Step("rm /tmp/m5.bin", wait=0.25),
    Step("rm /tmp/m5bad.bin", wait=0.25),
]


# 开工前的一次性清场（不属于任何一个用例）。上一轮只要有一条用例在半路
# FAIL，它启动的小程序就还在跑：既占着 0x24070000 的窗口（让 M1 报
# rc=-21「窗口拿不到」），又把 64 KB 池切碎（让 `cc --pack` 报「32 KB +
# 16 KB 拿不到」）。两处都不是被测对象的问题，所以必须先停干净再开始。
PRELUDE = [
    Step("mini stop", wait=2.0),
    Step("mini", expect=[r"^mini: none running"], wait=1.5),
]


# --- M2 判据 B 的「按实际落点现算」判据 -------------------------------------
# 这条用例的源码会把「字符串字面量的绝对地址 − 0x24070000」右移 12 位当落点号打出来，
# 于是「同一份镜像换一个落点」就变成了「屏幕上那个数字跟着变」。
#
# 关键点：**不能把块大小写死**。伙伴池的块是「≥ 负载的最小2 的幂」（1 KB…32 KB），
# 负载 1652 B 时是 2048 B 块，两个落点差 0x800，右移 12 位后两边都是 0 —— 不是程序
# 量错了，是 4 KB 一格的口径不适用了。所以期望值改成从 `mini run` 自己报的
# `code 0x........` 现算：拿到真实落点，再算「这个落点应该打印几」。
# 这样断的东西就不再是「这块板今天几 B 一块」，而是「程序读自己的绝对地址读对了」。
_POOL_BASE = 0x24070000


def _m2_landing(out):
    """从 `mini run` 的输出里取本次落点。返回 (base, block) 或 None。"""
    m = re.search(r"MINI:\d+ /tmp/m2\.svcm: \d+ B payload -> code 0x([0-9A-Fa-f]{8})"
                  r" \((\d+) B block\)", out)
    if not m:
        return None
    return int(m.group(1), 16), int(m.group(2))


def _m2_digit_ok(out):
    """本行输出里的落点号是否等于「按真实落点算出来的那一位」。"""
    land = _m2_landing(out)
    if land is None:
        return ["missing the load line (no landing address in this output)"]
    base, block = land
    if base < _POOL_BASE or (base - _POOL_BASE) % block != 0:
        return ["landing 0x%08X is not a block start of the 64 KB pool" % base]
    want = (base - _POOL_BASE) >> 12
    if want > 9:
        return ["landing 0x%08X is more than 9 blocks from the pool base; "
                "the fixture only prints one decimal digit" % base]
    # 串口回显里 `M2 alive` 前面还挂着提示符（`ark@arkrtos:~$ M2 alive`），
    # 所以这里不能用 `^` 锚行首；只锚 `M2 alive` 之后那一行。
    m = re.search(r"M2 alive\n\s*(\S)\s*$", out, re.M)
    if not m:
        return ["missing the program's own landing digit after `M2 alive`"]
    got = m.group(1)
    if got != str(want):
        return ["program measured landing digit %r but 0x%08X is %d blocks "
                "from 0x%08X (expected %r)"
                % (got, base, want, _POOL_BASE, str(want))]
    return []


def _m2_two_blocks_ok(out):
    """`mini` 的两条记录：两块确实相邻，且块大小一致。"""
    ent = re.findall(r"code (\d+) B in (\d+) B block @ 0x([0-9A-Fa-f]{8})", out)
    if len(ent) != 2:
        return ["expected 2 running entries in `mini`, got %d" % len(ent)]
    szs = [int(b) for _, b, _ in ent]
    bases = [int(a, 16) for _, _, a in ent]
    if bases[0] != _POOL_BASE:
        return ["first landing 0x%08X is not the pool base 0x%08X"
                % (bases[0], _POOL_BASE)]
    if szs[0] != szs[1]:
        return ["two landings got different block sizes %d / %d" % (szs[0], szs[1])]
    if bases[1] != bases[0] + szs[0]:
        return ["second landing 0x%08X is not the next block after 0x%08X (%d B each)"
                % (bases[1], bases[0], szs[0])]
    return []


CASES = [
    Case(
        "m1-fixed-window",
        "M1：板上编出的镜像按固定窗口跑起来，打印源码独有字符串，退出后窗口回收",
        CLEAN
        + _steps_emit("/tmp/m1.c", SRC_M1)
        + _steps_emit("/tmp/m1.sct", SCT_OK)
        + [
            Step("cc /tmp/m1.c --scatter /tmp/m1.sct --vector-table -o /tmp/m1.bin",
                 expect=[r"^cc: done in \d+ ms$"], wait=6.0),
            Step("app run /tmp/m1.bin",
                 expect=[r"APPRUN:\d+ /tmp/m1\.bin: \d+ B -> code 0x24070000",
                         r"M1 alive$", r"M1 tag alpha$",
                         r"APPRUN:\d+ task \d+ exited, window returned to the pool$"],
                 wait=3.0),
            # `fault` 不能断言「表为空」：`mini stop` 会把它杀掉的每个任务记成
            # 一条 TASKKILL（本轮实测），而清场恰恰要先停小程序。所以这里判的
            # 是「没有真正的异常」——四种 CPU 异常名一个都不许出现。
            Step("fault", forbid=[r"(HARD|MEM|BUS|USG)FAULT"]),
        ],
    ),
    Case(
        "m1-wrong-window",
        "M1 反假绿：.sct 换成别的窗口 → 必须报 -23，不能恰好跑通",
        CLEAN
        + _steps_emit("/tmp/m1.c", SRC_M1)
        + _steps_emit("/tmp/m1.sct", SCT_BAD)
        + [
            Step("cc /tmp/m1.c --scatter /tmp/m1.sct --vector-table -o /tmp/m1.bin",
                 expect=[r"^cc: done in \d+ ms$"], wait=6.0),
            Step("app run /tmp/m1.bin",
                 expect=[r"rc=-23", r"image was not linked for this window"],
                 forbid=[r"^M1 alive$"], wait=3.0),
            Step("rm /tmp/m1.c"),
            Step("rm /tmp/m1.sct"),
            Step("rm /tmp/m1.bin"),
        ],
    ),
    Case(
        "m2-pack-run",
        "M2：板上四遍差分打包出的 .svcm 被 mini 装载执行，四类表项都非零",
        CLEAN
        + _steps_emit("/tmp/m2.c", SRC_M2)
        + [
            Step("cc /tmp/m2.c --pack -o /tmp/m2.svcm",
                 expect=[r"^cc: pack .* \(4 passes, -O1\)$",
                         r"^cc:   A \d+ B$", r"^cc:   B \d+ B$",
                         r"^cc:   C \d+ B$", r"^cc:   D \d+ B$",
                         r"^cc: .*: payload \d+ \+ table \d+ x 4 = \d+ B$",
                         # 四类计数都必须非零：某一类恒 0 等于那一类没被验证
                         r"^cc:   reloc rom [1-9]\d* / ram [1-9]\d* / "
                         r"rom_movw [1-9]\d* / ram_movw [1-9]\d*",
                         r"^cc: +packed in \d+ ms$"],
                 # 窗口必须比实测耗时留足余量：这条四遍差分打包实测 8350 ms
                 # （1 份源），原来的 8.0 s 正好压在边界上——同一份固件、同一条
                 # 命令，跑快跑慢决定 PASS/FAIL。判据是"输出里有没有那几行"，
                 # 窗口太紧等于把判据换成了"这台板子今天多快"。
                 wait=25.0),
            Step("rm /tmp/m2.c"),
            Step("mini run /tmp/m2.svcm",
                 expect=[r"MINI:\d+ /tmp/m2\.svcm: \d+ B payload -> code 0x2407[0-9A-F]{4}",
                         r"M2 alive$"],
                 wait=3.0),
            Step("mini", expect=[r"\[0\] task \d+ entry 0x2407"], wait=2.0),
            Step("mini stop", expect=[r"^mini stopped: all blocks returned to the pool$"],
                 wait=2.0),
            Step("mini", expect=[r"^mini: none running"], wait=2.0),
        ],
    ),
    Case(
        "m2-two-landings",
        "M2 判据 B：同一份 .svcm 连跑两次，落点不同、程序自己量出的落点号与真实落点对得上"
        "（期望值按 `mini run` 报的基址现算，不写死块大小）",
        CLEAN
        + _steps_emit("/tmp/m2.c", SRC_M2)
        + [
            Step("mini stop", wait=2.0),      # 上一用例若半路 FAIL 会留下在跑的实例
            Step("cc /tmp/m2.c --pack -o /tmp/m2.svcm",
                 expect=[r"^cc: +packed in \d+ ms$"], wait=25.0),   # 同上：实测 8.35 s
            Step("rm /tmp/m2.c"),
            Step("mini run /tmp/m2.svcm",
                 expect=[r"M2 alive$"], wait=3.0, check=_m2_digit_ok),
            Step("mini run /tmp/m2.svcm",
                 expect=[r"M2 alive$"], wait=3.0, check=_m2_digit_ok),
            Step("mini",
                 expect=[r"\[0\] task \d+ entry 0x2407",
                         r"\[1\] task \d+ entry 0x2407",
                         r"2 running of 2 allowed"],
                 wait=2.0, check=_m2_two_blocks_ok),
            Step("mini stop", expect=[r"^mini stopped: all blocks returned to the pool$"],
                 wait=2.0),
        ],
    ),
    Case(
        "m2-cross-tu-link",
        "M2 判据 C：两源 -> 两对象 -> 一次 -link -> 固定窗口跑起来；带初值全局、字符串字面量、"
        "函数指针、跨文件调用四类绝对地址各命中一次",
        CLEAN
        + _steps_emit("/tmp/c1a.c", SRC_C1A)
        + _steps_emit("/tmp/c1b.c", SRC_C1B)
        + _steps_emit("/tmp/c1.sct", SCT_C1)
        + [
            # 第一个对象留向量表洞，第二个不留：-link --vector-table 只要求镜像头部
            # 是空的，两个对象都留等于白扔 1 KB 代码窗。
            # 每个对象都按同一份 .sct 编：arkc 把 .data/.bss 的绝对地址烤进
            # 指令，所以对象只能对它将来真正落的那块内存编基址；--scatter 一
            # 出现在命令行上，reloc-data/reloc-text 就默认打开（对象里记下可
            # 重定位站点），-link 才有东西可改。
            Step("cc /tmp/c1a.c --scatter /tmp/c1.sct --vector-table -c -o /tmp/c1a.obj",
                 expect=[r"^cc: /tmp/c1a\.c -> /tmp/c1a\.obj \(target arm, -O\d\), -c$",
                         r"^cc: scatter /tmp/c1\.sct$",
                         r"^cc: done in \d+ ms$"], wait=8.0),
            Step("cc /tmp/c1b.c --scatter /tmp/c1.sct -c -o /tmp/c1b.obj",
                 expect=[r"^cc: /tmp/c1b\.c -> /tmp/c1b\.obj \(target arm, -O\d\), -c$",
                         r"^cc: scatter /tmp/c1\.sct$",
                         r"^cc: done in \d+ ms$"], wait=8.0),
            Step("cc -link /tmp/c1a.obj /tmp/c1b.obj --scatter /tmp/c1.sct "
                 "--vector-table -o /tmp/c1.bin",
                 expect=[r"^cc: link 2 obj -> /tmp/c1\.bin$",
                         r"^cc: scatter /tmp/c1\.sct$",
                         r"^cc: reloc-text on$",
                         r"^cc: reloc-data on$",
                         r"^cc: done in \d+ ms$"], wait=10.0),
            Step("app run /tmp/c1.bin",
                 # tag 的字节在 .rodata（代码窗），g_init 在 .data（RAM 窗），
                 # b_add 在另一个对象里——三样都命中才算判据 C 过。
                 expect=[r"C1 tag alpha$", r"C1 ok$",
                         r"APPRUN:\d+ task \d+ exited, window returned to the pool$"],
                 forbid=[r"C1 BAD", r"(HARD|MEM|BUS|USG)FAULT", r"rc=-"],
                 wait=3.0),
            Step("rm /tmp/c1a.c"), Step("rm /tmp/c1b.c"), Step("rm /tmp/c1.sct"),
            Step("rm /tmp/c1a.obj"), Step("rm /tmp/c1b.obj"), Step("rm /tmp/c1.bin"),
        ],
    ),
    Case(
        "mini-sdk-std-hello",
        "演示（主线）：NOR 上的 hello 工程是标准 C（<stdio.h> + printf + int main），cd 进目录 make，mini run 打出 Hello, World!",
        CLEAN
        + [
            # 上电就挂好 /mnt/nor（ARKRTOS_AUTOMOUNT_NOR，见 main.c）。这一条
            # 就是那句"完全 Linux 的体验"的闸门：不敲 fs mount / mount 也能 ls。
            #
            # 布局是 SDK 在根、工程在 hello/ 子目录：SDK 是"这套编译器运行时"，
            # 一份；工程是要编的东西，可以有好几份并排放在同一块卷上。
            Step("ls -l /mnt/nor",
                 # ls -l 的列是"... size name"，数字在名字**前面**，所以只锚名字。
                 expect=[r"\d  start\.c$", r"\d  rt\.c$", r"\d  arkrtos\.h$",
                         r"\d  unistd\.h$", r"\d  stdio\.h$", r"\d  stdarg\.h$",
                         r"\d  hello$"],
                 forbid=[r"ls: no such file or directory", r"ls: no mount"],
                 wait=2.0),
            # 反假绿：`hello` 这个"罐装演示"命令已经被删掉了。它若又冒出来（有人
            # 把备料器加回去），演示的形态就退回"敲一条魔法命令"，这条会炸。
            Step("hello", expect=[r"hello: command not found"], wait=1.2),
            Step("cd /mnt/nor/hello"),
            Step("ls -l",
                 expect=[r"\d  hello\.c$", r"\d  makefile$"],
                 wait=1.5),
            # 这条用例最要紧的一行：工程里就是**标准 C**，没有一句 Ark RTOS
            # 的东西。下面 make 出来的 hello.svcm 是这句话的产物，不是另一份源码。
            Step("cat hello.c",
                 expect=[r"#include <stdio\.h>$",
                         r'printf\("Hello, World!\\n"\);$'],
                 wait=1.5),
            Step("make",
                 expect=[r"^make: hello\.svcm <- /mnt/nor/start\.c \(\+more\)$",
                         r"^make:   src /mnt/nor/rt\.c$",
                         r"^make:   src hello\.c$",
                         r"^cc: pack 3 sources -> hello\.svcm \(4 passes, -O1\)$",
                         r"^cc:   A \d+ B$", r"^cc:   B \d+ B$",
                         r"^cc:   C \d+ B$", r"^cc:   D \d+ B$",
                         # 平铺小程序：没有 Reset_Handler，就不该留那 0x400 的
                         # 向量表洞，入口只能是 +0。这两行是那条判定的读数本身。
                         r"^cc:   entry mode: flat miniapp, entry at \+0 "
                         r"\(no Reset_Handler\)$",
                         r"^cc:   A \+0 is 0xB5E0 \(flat image, no vector table",
                         r"^cc: hello\.svcm: payload \d+ \+ table \d+ x 4 = \d+ B$",
                         r"^cc:   reloc rom \d+ / ram \d+ / rom_movw [1-9]\d* / "
                         r"ram_movw [1-9]\d*, entry \+0x0$",
                         r"^cc: packed in \d+ ms$",
                         r"^make: ok$"],
                 forbid=[r"unresolved symbol", r"pack failed",
                         r"does not fit its"],
                 wait=60.0),
            # 相对名：cwd 在 /mnt/nor/hello，mini run 必须按 shell 的 cwd 解析。
            Step("mini run hello.svcm",
                 expect=[r"MINI:\d+ /mnt/nor/hello/hello\.svcm: \d+ B payload -> code 0x2407",
                         # printf 的输出接在提示符后面（小程序在控制台之下，
                         # 先返回提示符再轮到它），所以不能带 ^。
                         r"Hello, World!$",
                         r"task \d+ returned: MiniApp finished, "
                         r"code and RAM blocks back in the pool$"],
                 forbid=[r"mini run: hello\.svcm failed", r"(HARD|MEM|BUS|USG)FAULT"],
                 wait=5.0),
            # 程序自己退出，所以判"池里没有在跑的实例"（不是 mini stop 的措辞）。
            Step("mini", expect=[r"^mini: none running"], wait=2.0),
            Step("cd /"),
        ],
    ),
    Case(
        "mini-sdk-va",
        "变参（真机）：工程里是标准 C 的 va_start/va_arg（<stdarg.h>），板上 cc 编出来 mini run 打出正确数值",
        CLEAN
        + [
            # 这条用例存在的原因：宿主门禁（arkc 的 examples/run_va_gate.py）
            # 走的是主机侧 GNU as；板上的 `cc --pack` 走 arkc 自己的内部汇编器。
            # 变参的寄存器压栈、栈参数寻址、64 位取整、float 提升，只有真跑过
            # 才算数。工程本身部署在 /mnt/nor/va（tools/mini_deploy.py
            # --project example/mini_va --remote-dir /va）。
            Step("ls -l /mnt/nor",
                 expect=[r"\d  stdarg\.h$", r"\d  va$"],
                 forbid=[r"ls: no such file or directory"],
                 wait=2.0),
            Step("cd /mnt/nor/va"),
            Step("cat va.c",
                 expect=[r"#include <stdarg\.h>$", r"va_start\(ap, n\);"],
                 wait=1.5),
            Step("make",
                 expect=[r"^make: va\.svcm <- /mnt/nor/start\.c \(\+more\)$",
                         r"^cc: pack 3 sources -> va\.svcm \(4 passes, -O1\)$",
                         r"^cc:   entry mode: flat miniapp, entry at \+0 "
                         r"\(no Reset_Handler\)$",
                         r"^make: ok$"],
                 forbid=[r"unresolved symbol", r"pack failed",
                         r"does not fit its", r"is not supported by the ARM"],
                 wait=60.0),
            Step("mini run va.svcm",
                 # 第一行不锚行首：shell 先回显提示符、小程序随后接管输出，两者
                 # 之间没有同步点，实测会出现 "ark@arkrtos:/mnt/nor/va$ va: ..."
                 # 挤在同一行（日志里就是这么出现的）。其余各行都是程序独占的
                 # 完整行，仍然锚定，读数一个字节都不放宽。
                 expect=[r"va: companion built with the SDK's stdarg\.h$",
                         r"^va sum6      = 21 \(want 21\)$",
                         # 21 个参数字：4 个在 r0-r3，17 个在栈上。这一行是
                         # 「栈参数没有上限」在真机上的读数。
                         r"^va sum20     = 210 \(want 210\)$",
                         r"^va ll_hi     = 17 \(want 17\)$",
                         r"^va tail      = 21 \(want 21\)$",
                         r"^va two_ll    = 20 \(want 20\)$",
                         r"^va four_int  = 15 \(want 15\)$",
                         r"^va dbl_tag   = 255 \(want 255\)$",
                         r"^va struct_s3 = 6 \(want 6\)$",
                         r"^va: done$",
                         r"task \d+ returned: MiniApp finished"],
                 forbid=[r"(HARD|MEM|BUS|USG)FAULT", r"mini run: va\.svcm failed"],
                 wait=8.0),
            Step("mini", expect=[r"^mini: none running"], wait=2.0),
            Step("cd /"),
        ],
    ),
    Case(
        "mini-setjmp",
        "setjmp/longjmp（真机）：SDK 里的 setjmp 是 ARM 汇编函数，工程跨 6 层递归 longjmp 回来，"
        "五条读数全对；它同时是 `cc --pack` 多对象数据段重定位的闸门",
        CLEAN
        + [
            # 为什么这对函数必须是汇编写在工程的 SRCS 里：setjmp 要存调用者的
            # sp/lr，用 C 包装拿到的永远是包装自己的帧——包装 return 后那帧
            # 会被下一次调用复用，longjmp 恢复 sp 再 bx 回去，就顺着正常返回
            # 路径一路 unwind（本项目实测过：main 拿到 99 而不是 7）。
            Step("ls -l /mnt/nor",
                 expect=[r"\d  setjmp\.h$", r"\d  setjmp\.s$", r"\d  sj$"],
                 forbid=[r"ls: no such file or directory"],
                 wait=2.0),
            Step("cd /mnt/nor/sj"),
            Step("cat sj.c",
                 expect=[r"#include <setjmp\.h>$", r"longjmp\(back, 7\);"],
                 wait=1.5),
            Step("make",
                 expect=[r"^make: sj\.svcm <- /mnt/nor/start\.c \(\+more\)$",
                         r"^make:   src /mnt/nor/setjmp\.s$",
                         r"^make:   src sj\.c$",
                         r"^cc: pack 4 sources -> sj\.svcm \(4 passes, -O1\)$",
                         r"^cc:   A \d+ B$", r"^cc:   B \d+ B$",
                         r"^cc:   C \d+ B$", r"^cc:   D \d+ B$",
                         # ram_movw 必须 >0：.s 对象自己没有数据段，但顺序在它
                         # 之后的 C 对象带全局量，链接器得给它们各分一块 SRAM
                         # 并改写地址。这一行是「多对象数据段真的被搬过」的读数
                         # ——它曾经是 >0 却仍失败的形态（.s 对象谎报窗口）。
                         r"^cc:   reloc rom \d+ / ram \d+ / rom_movw [1-9]\d* / "
                         r"ram_movw [1-9]\d*, entry \+0x0$",
                         r"^cc: packed in \d+ ms$",
                         r"^make: ok$"],
                 # 这三条是这一轮真缺陷的原文：`cc --pack` 不打开 reloc-data 时
                 # 两个带全局量的对象会撞 SRAM；打开后 .s 对象又谎报前一个
                 # 编译单元的窗口。两者都不许再出现。
                 forbid=[r"SRAM overlap between objects", r"carries no relocatable address",
                         r"link failed", r"pack failed", r"unresolved symbol"],
                 wait=90.0),
            Step("mini run sj.svcm",
                 # 第一行不锚行首：shell 先回显提示符、小程序随后接管输出，
                 # 实测会出现 "ark@arkrtos:/mnt/nor/sj$ sj: first pass r=0"
                 # 挤在同一行（mini-sdk-va 同型）。其余各行是程序独占的完整行。
                 expect=[r"sj: first pass r=0$",
                         r"^sj: jumped back r=7 \(want 7\)$",
                         r"^sj: frames abandoned = 6 \(want 6\)$",
                         r"^sj: longjmp\(env, 0\) -> 1 \(want 1\)$",
                         r"^sj: ok$",
                         r"task \d+ returned: MiniApp finished"],
                 forbid=[r"dive returned normally", r"FAIL",
                         r"(HARD|MEM|BUS|USG)FAULT", r"mini run: sj\.svcm failed"],
                 wait=8.0),
            Step("mini", expect=[r"^mini: none running"], wait=2.0),
            Step("cd /"),
        ],
    ),
    Case(
        "mini-tls",
        "__thread（真机读数）：这块板上的 ARM 后端把 __thread 当普通全局量——没有 TP 基址、单实例，"
        "初值与清零都正确；这条用例把这个边界写成可重复的读数",
        CLEAN
        + [
            # 这条不是「TLS 能用」，恰恰相反：它把「在这条链路上 __thread 只有
            # 一个实例」这件事变成板上的读数，免得有人把 __thread 当成每线程
            # 一份来写，然后在多线程场景里静默出错。（arkc 的 ARM 后端不输出
            # 任何数据段，全局初值由 __arkcc_init_globals 逐条写，.tdata/.tbss
            # 分支在这条路上没有任何差别。）
            Step("ls -l /mnt/nor",
                 expect=[r"\d  tls$"],
                 forbid=[r"ls: no such file or directory"],
                 wait=2.0),
            Step("cd /mnt/nor/tls"),
            Step("cat tls.c",
                 # 板上 cat 把非 ASCII 打印成 \xE5... 转义，所以整行末尾是注释；
                 # 只锚行首，不断言行尾。
                 expect=[r"^__thread int counter = 5;"],
                 wait=1.5),
            Step("make",
                 expect=[r"^make: tls\.svcm <- /mnt/nor/start\.c \(\+more\)$",
                         r"^cc: pack 3 sources -> tls\.svcm \(4 passes, -O1\)$",
                         r"^cc:   reloc rom \d+ / ram \d+ / rom_movw [1-9]\d* / "
                         r"ram_movw [1-9]\d*, entry \+0x0$",
                         r"^cc: packed in \d+ ms$",
                         r"^make: ok$"],
                 forbid=[r"SRAM overlap between objects", r"carries no relocatable address",
                         r"link failed", r"pack failed", r"unresolved symbol"],
                 wait=90.0),
            Step("mini run tls.svcm",
                 # 首行不锚行首：实测提示符与第一行打印挤在同一行（同 setjmp）。
                 expect=[r"tls: init=5 \(want 5\)$",
                         r"^tls: after write=9 \(want 9\)$",
                         r"^tls: zeroed=0 \(want 0\)$",
                         r"^tls: ok \(single instance, no TP base",
                         r"task \d+ returned: MiniApp finished"],
                 forbid=[r"(HARD|MEM|BUS|USG)FAULT", r"mini run: tls\.svcm failed"],
                 wait=8.0),
            Step("mini", expect=[r"^mini: none running"], wait=2.0),
            Step("cd /"),
        ],
    ),
    Case(
        "mini-sdk-missing-main",
        "反假绿：工程没有 int main(void) 时链接必须报 unresolved symbol main，不许出一个空程序",
        CLEAN
        # 这条守的是一处**静默**错误。历史：SDK 的启动骨架曾经是「main() 调
        # MiniMain()」，而 SDK 同时提供一个弱定义的空 MiniMain——arkc 在同一个
        # 翻译单元内把它绑死，工程自己那份强定义永远进不来，表现是「程序创建
        # 当拍就退出、什么都不打印」，既不报错也没有诊断。
        #
        # 现在 arkc 档的入口就是工程自己写的 int main(void)，SDK 不提供任何默认
        # main；所以这里必须是链接失败。工程只给启动桩与运行时（两份 SDK 文件），
        # 一个 main 都没有。
        + _steps_emit("/tmp/nmk", [
            "TARGET = nom",
            "OUT = nom.svcm",
            "SRCS = /mnt/nor/start.c /mnt/nor/rt.c",
            "INCS = /mnt/nor",
        ])
        + [
            Step("make -f /tmp/nmk",
                 expect=[r'^/tmp/ppA\.bin: Error: unresolved symbol "main"$',
                         r"^cc: pass A failed$"],
                 # 关键否定：不许"报错但照样出文件"。以前链接器只报诊断、
                 # 仍然写出镜像，四遍差分照做，最后在打包阶段报一个不相干的
                 # -33（实测）。
                 forbid=[r"^make: ok$", r"pack failed"],
                 wait=60.0),
            Step("rm /tmp/nmk"),
        ],
    ),
    Case(
        "stack-usage",
        "栈水印：每个任务的栈用量是真读数（填充图案 + 最低 PSP），不是 n/a 也不是 0",
        CLEAN
        + [
            # 先把小程序停干净：正在跑的实例会让任务表多一行、也会让最坏
            # 百分比漂移，而这条用例判的是**读数本身成立**，不是某个具体值。
            Step("mini stop", wait=2.0),
            Step("mini", expect=[r"^mini: none running"], wait=1.5),
            # 反假绿：统计没编进来（ARKRTOS_USE_STACK_USAGE=0）时命令必须
            # 说"compiled out"，不许给数字。上面两种形态都在源码里，所以
            # 这里判的是本固件走的是哪一支——走错支就该红。
            Step("stack",
                 expect=[r"^id  total    used     free     pct    entry$",
                         r"^stack: \d+ task\(s\), worst [1-9]\d*% \(task \d+\), "
                         r"0 unmeasured, 0 at/over 90%  \(accounting on\)$"],
                 forbid=[r"n/a", r"unmeas 0x",
                         r"accounting is compiled out",
                         r"^ \d+ +0 +\d+ "],
                 wait=2.0),
            # 判词一行：全部任务都测得到，且没有一个到 90%。返回 0 时打印
            # ok；任何一条不成立它打 FAILED（这条用例就靠这两行分辨）。
            Step("stack check",
                 expect=[r"^stack: check ok - \d+ task\(s\) measured, worst "
                         r"[1-9]\d*% \(task \d+\), none at/over 90%$"],
                 forbid=[r"^stack: check FAILED"],
                 wait=2.0),
        ],
    ),
    Case(
        "vfs-create",
        "VFS 新建：卷根与子目录都能落文件；父目录不存在时要说 no such file，"
        "不能报成 backend error",
        CLEAN
        + [
            # 先清掉上一轮可能留下的同名件，否则 touch 走的是「已存在就什么都不做」
            # 那一支，这条用例就变成什么都没测。
            Step("rm /mnt/nor/c3gate.txt", wait=0.6),
            Step("rm /mnt/nor/va/c3gate.txt", wait=0.6),
            # 1) 卷根新建：ls 必须把它列成 0 字节文件
            Step("touch /mnt/nor/c3gate.txt", wait=1.0),
            Step("ls /mnt/nor", expect=[r"^  c3gate\.txt +0$"], wait=1.5),
            # 2) 子目录新建：同一卷下 /va 里也要能落
            Step("touch /mnt/nor/va/c3gate.txt", wait=1.0),
            Step("ls /mnt/nor/va", expect=[r"^  c3gate\.txt +0$"], wait=1.5),
            # 3) 反假绿：父目录不存在时必须报「没有这个文件」，把 littlefs 的
            #    LFS_ERR_NOENT 一路漏到 shell 上就变成 -11 backend error，
            #    那是在报“驱动出错”，而真正的原因是路径不存在。
            Step("touch /mnt/nor/c3nodir/x.txt",
                 expect=[r"FAILED \(-1: no such file or mount\)"],
                 forbid=[r"backend error"],
                 wait=2.0),
            Step("rm /mnt/nor/c3gate.txt", wait=0.6),
            Step("rm /mnt/nor/va/c3gate.txt", wait=0.6),
        ],
    ),
    Case(
        "fs-mount-blank",
        "M3-c：第一次挂一块没格式化过的卷必须明确报错，且不许静默格盘——"
        "报错后字节一个都没写（同样的挂载再失败一次），真卷挂回来照常可用",
        CLEAN
        + [
            # 目标卷：NOR 上 4 MiB 之后那块从来没被格式化过的区域（出厂 0xFF）。
            # 先卸掉真卷——严格挂载是「同一时刻只有一个卷」，这也是它比
            # 「挂着一个再挂一个」更值得测的地方：卸完还能挂回来。
            Step("fs unmount", expect=[r"^unmounted$"], wait=1.0),
            Step("fs mount nor0 4194304 1048576",
                 expect=[
                     r"^mount: FAILED \(littlefs error -(84|22): LFS_ERR_(CORRUPT|INVAL)\)$",
                     r"^.*these bytes hold no littlefs superblock.*$",
                     r"^.*Nothing was written\.$",
                     r"^.*'fs format nor0 0x400000 1048576' creates an empty volume on purpose.*$",
                 ],
                 # 格盘是不许发生的事：出现格式化成功的行，或者出现
                 # 「mounted …」都说明它抢在被测对象前面动了手。
                 forbid=[r"format : ok", r"^mounted nor0 at 0x400000"],
                 wait=2.5),
            # 反假绿的第二问：报错到底有没有写字节？再挂一次同样失败，且卷里
            # 依旧没有文件系统——若上一次「失败但顺手格了盘」，这一次就会成功。
            Step("fs mount nor0 4194304 1048576",
                 expect=[r"^mount: FAILED \(littlefs error -(84|22): LFS_ERR_(CORRUPT|INVAL)\)$"],
                 forbid=[r"^mounted nor0 at 0x400000"],
                 wait=2.5),
            # 真卷挂回来，并且要能过 VFS 用（/mnt/nor 是自启时挂上的那层）：
            # 只验 fs 层会说谎——上板实测过「fs info 说 yes、/mnt/nor 打不开」
            # 那种状态，所以两条都问。
            Step("fs mount nor0 0 4194304",
                 expect=[r"^mounted nor0 at 0x0, 4194304 bytes$"], wait=2.0),
            Step("fs info", expect=[r"^mounted  : yes"], wait=1.0),
            Step("ls /mnt/nor",
                 expect=[r"^  hello +0 +<dir>$"], wait=2.0),
        ],
    ),
    Case(
        "mini-autostart-reboot",
        "M3-d 跨复位自启：清单里的路径在开机时由内核自己跑起来（没人敲 mini run）；"
        "从清单里删掉再开机就必须不再跑——证明那行输出来自清单，而不是别的路",
        CLEAN
        + [
            # 0) 先把上一轮可能残留的痕清干净（那才是真正会污染后面的东西：
            #    清单在 NOR 上，复位清不掉）。空改动的 del 也是成功的，所以
            #    这里不写 expect 文字，下一步自己会把状态读回来。
            # 清单本来就不存在时的读数是 "no list (/mini.autostart)"（文件都不在），
            # 存在但删空时才走 "empty list"。这里要的是"卷上真的没有清单"，
            # 所以判 no list——它比 empty list 更强。
            Step("mini autostart del /mnt/nor/c3auto.svcm", wait=1.2),
            Step("mini autostart",
                 expect=[r"^mini autostart: no list \(/mini\.autostart\)"],
                 wait=1.5),
        ]
        # 1) 造镜像：必须落在 NOR 上。ramfs 复位就没了，而这条用例的全部
        #    内容就是"复位之后还在"。
        + _steps_emit("/tmp/auto.c", SRC_AUTO)
        + [
            Step("cc /tmp/auto.c --pack -o /mnt/nor/c3auto.svcm",
                 expect=[r"^cc: +packed in \d+ ms$"],
                 forbid=[r"unresolved symbol", r"pack failed"],
                 wait=25.0),
            Step("rm /tmp/auto.c"),
            # 2) 先手工跑一次：否则“开机没跑起来”分不清是清单的错还是镜像的错。
            Step("mini run /mnt/nor/c3auto.svcm",
                 expect=[r"C3 autostart alive$",
                         r"task \d+ returned: MiniApp finished"],
                 forbid=[r"mini run: .* failed", r"(HARD|MEM|BUS|USG)FAULT"],
                 wait=4.0),
            # 3) 写清单，并把清单读回来（“写进去了”和“内核看到的是它”是两件事）
            Step("mini autostart add /mnt/nor/c3auto.svcm",
                 expect=[r"^mini autostart: /mnt/nor/c3auto\.svcm will start at boot$"],
                 wait=1.5),
            Step("mini autostart",
                 expect=[r"^  \[1\] /mnt/nor/c3auto\.svcm$",
                         r"^  1 listed, 0 missing in the volume, 0 disabled$"],
                 wait=1.5),
            # 4) 复位（不重烧、不断电）。从这里开始串口上的每一行都是内核
            #    自己跑出来的：没有人敲 mini run。
            Reset(
                wait=9.0,
                expect=[r"autostart: /mnt/nor/c3auto\.svcm started$",
                        r"boot autostart: 1 started from /mini\.autostart$",
                        r"C3 autostart alive$"],
                forbid=[r"autostart: /mnt/nor/c3auto\.svcm not started",
                        r"(HARD|MEM|BUS|USG)FAULT"],
            ),
            Step("mini", expect=[r"^mini: none running"], wait=2.0),
            # 5) 反假绿：把它从清单里删掉，再复位一次。若那行字又出现，就说明
            #    它根本不是清单触发的（例如 loader 扫描 slot 自启了它）——
            #    那样这条用例测的就是别的东西。
            Step("mini autostart del /mnt/nor/c3auto.svcm",
                 expect=[r"^mini autostart: /mnt/nor/c3auto\.svcm will not start at boot$"],
                 wait=1.5),
            Step("mini autostart",
                 expect=[r"^mini autostart: no list \(/mini\.autostart\)"],
                 wait=1.5),
            Reset(
                wait=9.0,
                expect=[r"no autostart list \(/mini\.autostart\)$"],
                forbid=[r"boot autostart: \d+ started", r"C3 autostart alive"],
            ),
            # 6) 收尾：镜像也不留在卷上（下一轮从干净状态开始）
            Step("rm /mnt/nor/c3auto.svcm", wait=1.0),
            Step("ls /mnt/nor", expect=[r"^  hello +0 +<dir>$"], wait=1.5),
        ],
    ),
    Case(
        "m5-flashapp",
        "阶段 3c：产物烧进片内 Flash 应用区（0x081E0000，bank2）后从 Flash 取指执行，复位后仍在",
        CLEAN
        + [
            # 这条用例要证明「没烧过的时候是 -44」，所以先擦一次：上一轮（或
            # 手工调试）留下的镜像必须先清掉，否则第一步就在测别人的产物。
            Step("app flash erase",
                 expect=[r"^app flash erase: 0x081E0000\.\.0x08200000 cleared$"],
                 wait=4.0),
            Step("app flash info",
                 expect=[r"rc=-44", r"no image in the flash app area"], wait=1.0),
        ]
        + _steps_emit("/tmp/m5.c", SRC_M5)
        + _steps_emit("/tmp/m5.sct", SCT_M5)
        + [
            Step("cc /tmp/m5.c --scatter /tmp/m5.sct --vector-table -o /tmp/m5.bin",
                 expect=[r"^cc: done in \d+ ms$"], wait=6.0),
            # 写路径自带三条读数：长度/CRC、目标地址、以及「回读逐字节比对通过」。
            Step("app flash write /tmp/m5.bin",
                 expect=[r"FLASHAPP:\d+ /tmp/m5\.bin: \d+ B, crc 0x[0-9A-F]{8} -> 0x081E0000",
                         r"^app flash write: /tmp/m5\.bin, \d+ B at 0x081E0000 \(verified\)$"],
                 wait=6.0),
            Step("app flash info",
                 expect=[r"^flash app: \d+ B, crc 0x[0-9A-F]{8}, entry 0x081E0[0-9A-F]{3}, "
                         r"area 0x081E0000 \+128K$"],
                 wait=1.0),
            # 判据：日志里的入口点击在 0x081E0000 区（真从 Flash 取指），且程序
            # 自己打出了源码独有字符串（那几条指令确实被执行了）。两条缺一不可
            # ——只看字符串，把镜像拷到 RAM 里跑也能通过。
            Step("app flash run",
                 expect=[r"FLASHAPP:\d+ \d+ B from flash 0x081E0000, entry 0x081E0[0-9A-F]{3}",
                         r"M5 alive in flash$", r"M5 tag bravo$",
                         r"FLASHAPP:\d+ task \d+ exited, data window returned to the pool$"],
                 wait=3.0),
            # 掉电不丢：复位后 /tmp/m5.bin 已经没了（ramfs 是易失的），Flash 里
            # 那份还在、仍能跑。这是「烧在 Flash」与「读进 RAM」的唯一分辨点。
            Reset(wait=9.0,
                  expect=[r"Ark RTOS kernel shell"],
                  forbid=[r"(HARD|MEM|BUS|USG)FAULT"]),
            Step("ls /tmp", forbid=[r"m5\.bin"], wait=1.0),
            Step("app flash info", expect=[r"^flash app: \d+ B, crc 0x[0-9A-F]{8}"], wait=1.0),
            Step("app flash run",
                 expect=[r"FLASHAPP:\d+ \d+ B from flash 0x081E0000", r"M5 alive in flash$"],
                 wait=3.0),
            # 反假绿 1：擦掉之后必须报 -44，不能「跑上一次缓存的那份」。
            Step("app flash erase",
                 expect=[r"^app flash erase: 0x081E0000\.\.0x08200000 cleared$"], wait=4.0),
            Step("app flash run", expect=[r"rc=-44"], forbid=[r"M5 alive in flash$"], wait=1.5),
            # 反假绿 2：没有这条子命令时用法行必须列全四条，否则「怎么用」只能靠猜。
            Step("app flash bogus",
                 expect=[r"usage: app flash \[write <path> \| run \| erase \| info\]"], wait=0.5),
            Step("fault", forbid=[r"(HARD|MEM|BUS|USG)FAULT"]),
            Step("rm /tmp/m5.c", wait=0.25), Step("rm /tmp/m5.sct", wait=0.25),
            Step("rm /tmp/m5.bin", wait=0.25),
        ],
    ),
    Case(
        "m5-wrong-window",
        "阶段 3c 反假绿：数据窗口错位的镜像在擦除之前就被拒（-42），区里那份还能跑",
        CLEAN
        + [
            Step("app flash erase", wait=4.0),
        ]
        + _steps_emit("/tmp/m5.c", SRC_M5)
        + _steps_emit("/tmp/m5.sct", SCT_M5)
        + _steps_emit("/tmp/m5b.sct", SCT_M5_BAD)
        + [
            Step("cc /tmp/m5.c --scatter /tmp/m5.sct --vector-table -o /tmp/m5.bin",
                 expect=[r"^cc: done in \d+ ms$"], wait=6.0),
            Step("cc /tmp/m5.c --scatter /tmp/m5b.sct --vector-table -o /tmp/m5bad.bin",
                 expect=[r"^cc: done in \d+ ms$"], wait=6.0),
            Step("app flash write /tmp/m5.bin",
                 expect=[r"^app flash write: /tmp/m5\.bin, \d+ B at 0x081E0000 \(verified\)$"],
                 wait=6.0),
            # 关键：坏镜像被拒之后，区里那份**必须还在**。若这里 info 报 -44，
            # 说明实现是「先擦后验」，等于拿一个明显不对的文件毁掉了上一份。
            Step("app flash write /tmp/m5bad.bin",
                 expect=[r"rc=-42", r"image was not linked for the flash app area"],
                 forbid=[r"^app flash write: /tmp/m5bad\.bin"], wait=6.0),
            Step("app flash info",
                 expect=[r"^flash app: \d+ B, crc 0x[0-9A-F]{8}, entry 0x081E0[0-9A-F]{3}"],
                 wait=1.0),
            Step("app flash run",
                 expect=[r"FLASHAPP:\d+ \d+ B from flash 0x081E0000", r"M5 alive in flash$"],
                 wait=3.0),
            # 收尾：区里不留镜像（后面的用例从头开始，这条用例也不该污染它们）
            Step("app flash erase",
                 expect=[r"^app flash erase: 0x081E0000\.\.0x08200000 cleared$"], wait=4.0),
            Step("fault", forbid=[r"(HARD|MEM|BUS|USG)FAULT"]),
            Step("rm /tmp/m5.c", wait=0.25), Step("rm /tmp/m5.sct", wait=0.25),
            Step("rm /tmp/m5b.sct", wait=0.25), Step("rm /tmp/m5.bin", wait=0.25),
            Step("rm /tmp/m5bad.bin", wait=0.25),
        ],
    ),
]


# ---------------------------------------------------------------- 串口驱动

class Board(object):
    def __init__(self, port, baud=115200):
        try:
            import serial  # noqa: F401  (只有这里需要 pyserial)
        except ImportError:
            sys.stderr.write("FAIL: pyserial is not installed (pip install pyserial)\n")
            raise SystemExit(1)
        import serial as pyserial
        self.ser = pyserial.Serial(port, baud, timeout=0.2)
        self.ser.reset_input_buffer()
        self.buf = []
        self.stop = [False]
        self.th = threading.Thread(target=self._reader, daemon=True)
        self.th.start()

    def _reader(self):
        while not self.stop[0]:
            try:
                data = self.ser.read(8192)
            except Exception:
                break
            if data:
                self.buf.append(data)

    def drain(self, wait):
        time.sleep(wait)
        text = b"".join(self.buf).decode("utf-8", "replace")
        del self.buf[:]
        return text

    def send(self, cmd, wait):
        self.ser.write((cmd + "\r\n").encode("utf-8"))
        return self.drain(wait)

    def drop(self):
        """丢掉已经不新鲜的输出（复位前用）：不这样做，复位后的“证据”里
        会混进复位前最后几行，而那正是判据最容易自己骗自己的地方。"""
        del self.buf[:]
        try:
            self.ser.reset_input_buffer()
        except Exception:
            pass

    def close(self):
        self.stop[0] = True
        self.th.join(2)
        try:
            self.ser.close()
        except Exception:
            pass


# 断言为什么只锚行尾、不锚行首：串口上一条程序输出经常与前面的东西同处一
# 行——内核日志行带 "[  15.837] <6>" 前缀，程序的 printf 又常紧跟在提示符
# "ark@arkrtos:~$ " 之后。锚行首会把这些**正确**的输出判成缺失（本轮实际踩
# 到过：四条用例全 FAIL，日志里输出其实都在）。所以「源码独有字符串」一律
# 用 r"<text>$" 配 re.M，靠行尾锚保证它不是别人的前缀。
#
# shell 的 ANSI 回显里有退格擦除序列（"\x1b[D\x1b[K" 逐字符重画），断言前要
# 去掉，否则 "M1 alive" 在回显里是 "M\x1b[D\x1b[K1..." 这种形状。
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def clean(text):
    text = ANSI.sub("", text)
    text = text.replace("\x08", "")
    return text.replace("\r\n", "\n").replace("\r", "\n")


def reset_board():
    """复位 MCU（不重烧）：openocd 只做 reset run 就退出。

    判据为什么是“进程退出码 0 且日志里没有 Error”：openocd 对 reset 没有
    类似烧录的 “** Verified OK **” 那种肯定句，而「板子真重启了」这件事
    由用例自己的 expect（banner / 开机日志）来证——复位本身就是那个前题。
    """
    if not os.path.isfile(OCD):
        sys.stderr.write("FAIL: openocd not found: %s (set ARK_OPENOCD)\n" % OCD)
        return False
    cmd = "init; reset_config srst_only srst_nogate; reset run; shutdown"
    proc = subprocess.run([OCD, "-s", OCD_SCRIPTS, "-f", OCD_CFG, "-c", cmd],
                          capture_output=True, text=True)
    out = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode != 0 or ("Error" in out):
        sys.stderr.write("FAIL: reset via openocd: rc=%d\n%s\n"
                         % (proc.returncode, out[-1200:]))
        return False
    return True

def run_case(board, case):
    problems = []
    for step in case.steps:
        if isinstance(step, Reset):
            board.drop()
            if not reset_board():
                problems.append("could not reset the board")
                break
            out = clean(board.drain(step.wait))
        else:
            out = clean(board.send(step.cmd, step.wait))
        for pat in step.expect:
            if not re.search(pat, out, re.M):
                problems.append("missing /%s/ after `%s`" % (pat, step.cmd))
        for pat in step.forbid:
            if re.search(pat, out, re.M):
                problems.append("forbidden /%s/ appeared after `%s`" % (pat, step.cmd))
        if step.check is not None and not problems:
            for p in step.check(out):
                problems.append("%s after `%s`" % (p, step.cmd))
        if problems:
            problems.append("---- last output of `%s` ----\n%s" % (step.cmd, out.strip()))
            break
    return problems


# ---------------------------------------------------------------- 烧录

def flash(binpath):
    if not os.path.isfile(binpath):
        sys.stderr.write("FAIL: firmware not found: %s\n" % binpath)
        return False
    if not os.path.isfile(OCD):
        sys.stderr.write("FAIL: openocd not found: %s (set ARK_OPENOCD)\n" % OCD)
        return False
    cmd = ("init; reset halt; program {%s} 0x08000000 verify; "
           "reset_config srst_only srst_nogate; reset run; shutdown" % binpath)
    proc = subprocess.run([OCD, "-s", OCD_SCRIPTS, "-f", OCD_CFG, "-c", cmd],
                          capture_output=True, text=True)
    out = (proc.stdout or "") + (proc.stderr or "")
    if "** Verified OK **" not in out:
        sys.stderr.write("FAIL: flash did not verify\n%s\n" % out[-2000:])
        return False
    print("[flash] verified OK  (%s)" % binpath)
    time.sleep(2.5)          # 等 banner 与 vfs 起来
    return True


# ---------------------------------------------------------------- main

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--bin", default=DEFAULT_BIN)
    ap.add_argument("--noflash", action="store_true")
    ap.add_argument("--only", default=None, help="只跑名字含该子串的用例")
    ap.add_argument("--list", action="store_true", help="只列用例")
    args = ap.parse_args(argv)

    cases = [c for c in CASES if (args.only is None) or (args.only in c.name)]

    if args.list:
        for c in cases:
            print("%-18s %s" % (c.name, c.why))
        return 0
    if not cases:
        sys.stderr.write("no case matches --only %r\n" % args.only)
        return 1

    if not args.noflash:
        if not flash(args.bin):
            return 1

    board = Board(args.port)
    results = []
    try:
        boot = board.drain(1.0)
        if "arkrtos" not in clean(boot).lower():
            print("[warn] no banner seen; assuming the shell is already up")
        print("\n=== board-clean ===\n    停掉上一轮可能残留的小程序，"
              "确认池与窗口都是空的")
        prelude_problems = []
        for step in PRELUDE:
            out = clean(board.send(step.cmd, step.wait))
            for pat in step.expect:
                if not re.search(pat, out, re.M):
                    prelude_problems.append("missing /%s/ after `%s`\n%s"
                                            % (pat, step.cmd, out.strip()))
        if prelude_problems:
            for p in prelude_problems:
                print("  FAIL " + p)
            print("\n    板子状态不干净，后面的用例没有意义，先停。")
            return 1
        print("  PASS")

        for c in cases:
            print("\n=== %s ===\n    %s" % (c.name, c.why))
            problems = run_case(board, c)
            if problems:
                for p in problems:
                    print("  FAIL " + p)
                results.append((c.name, False))
            else:
                print("  PASS")
                results.append((c.name, True))
    finally:
        board.close()

    print("\n---- summary ----")
    for name, ok in results:
        print("  %-18s %s" % (name, "PASS" if ok else "FAIL"))
    failed = [n for n, ok in results if not ok]
    if failed:
        print("\n%d of %d FAILED: %s" % (len(failed), len(results), ", ".join(failed)))
        return 1
    print("\nall %d cases PASS" % len(results))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
