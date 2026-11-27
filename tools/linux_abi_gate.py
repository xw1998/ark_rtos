#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Linux 系统调用 ABI（ARKRTOS_USE_LINUX_ABI）的门禁：开关、号表、符号、真读数。

为什么需要这一道
----------------
`ARKRTOS_USE_LINUX_ABI` 是一个默认关的兼容层：它允许一个**按 Linux 约定写的
静态程序**（`syscall 0` + `$a7` 放号，负值 = -errno）直接在本内核的 PLV3 任务里
跑起来。它有三类假绿：

  1. 开关没接线：Kconfig 里有一项、features.h 里有一句 `#error`，但源文件没被
     `#if` 包住 —— 默认档悄悄多出一份代码（体积变了，行为没变）；
  2. 号表抄错：把 `read` 写成 64、`write` 写成 63，宿主侧一切正常，只有真程序
     读不到东西；或者号段和 Ark 自己的服务号（RV32 的 0x10..0x22）撞了，
     两个 ABI 互相吞调用；
  3. 只有编译没有读数：代码编得过，但用户指针一律被判越界（2026-11-19 的真
     事故：ELF 任务的 rom/ram 窗口在分页档没写回 TCB，于是所有 `write()` 都回
     -EFAULT，程序照跑、控制台一个字也看不到）、或者退出码回错了任务槽。

所以这里只用可证伪的读数：默认档的符号表、两条 port 的符号表（开/关各一次）、
号表与 Linux UAPI 的逐条对账、以及 QEMU 上真跑一遍的串口日志。

判据（全过才 0）
----------------
  A. 默认档：`arkrtos_autoconf.h` 里 `ARKRTOS_USE_LINUX_ABI` 必须是 0；Kconfig
     里它是 `depends on ARKRTOS_USE_ELF`；features.h 有「要 ABI 必须开 ELF」的
     `#error`（两处都要在，缺一就是纸面开关）。
  B. 关档零符号：用 RV32 工具链（那块板没开这个开关）把
     `kernelsrc/src/arkrtos_linux_abi.c` 编成 .o，符号表必须是空的 —— 一个
     定义、一个未定义引用都不许有。
  C. 接线两向：LA64 port 在**去掉板级开关**的探针头上编，必须没有
     `arkrtos_port_linux_syscall_num`；用真板级头编，必须有。只测一向时，
     「宏写成了 1 但函数体是空壳」和「函数根本没编进去」看起来一样。
  D. 开档编得过：LA64 真板级下 `arkrtos_linux_abi.c` / `arkrtos_task.c` /
     `arkrtos_elf.c` / la64 port / `elf/linux_hello.c` 全编得过。
  E. 号表对账：源里的 `LX_SYS_*` 与 asm-generic 的 Linux UAPI 号逐条相等；
     LA64 的分界是「`syscall` 立即数 == 0」，RV32 的分界是 0x10..0x22 留给 Ark。
  F. QEMU 真读数：`[linux]` 的 14 条 check 全 ` ok`（一条 FAIL 都不许）、
     `PASS: every syscall answered as specified`、`exit code 7`；`[pipe]` 的
     30 条、`[sig]` 的 31 条、`[dup]` 的 56 条同理（PASS 行 + 退出码
     55 / 138 / 56）；并且同一次启动里 hello/modhello/FS 的读数仍在（兼容层
     不许把别人的演示搞坏）。
  G. 解析器自检：把合成坏日志喂给同一个判据（去掉 PASS、把退出码改成 3、
     插一行 FAIL、少一条 check），每种坏法都必须被判红。

不覆盖：fork/clone、父子 fd 继承（没有 fork 就没有第二个进程）、socket、
futex；真 mmap 语义（MAP_FIXED 的同址丢弃）已做，但它在按需分页档才存在，
读数在 tools/fork_gate.py 的 `[fork]` 块（本门禁跑默认档，那里回 -ENOSYS）；
管道（pipe2：默认阻塞 + O_NONBLOCK 档 + SIGPIPE）已做，读数在 `[pipe]`
块（跨执行流的阻塞等待在 fork 档的 `[fork]` 块）；dup / dup3 已做，读数在
`[dup]` 块（fd 槽复制 + 标准流别名 + 控制台引用，socket 明确不支持 dup）；信号
（同一任务自我投递的最小一档：rt_sigaction/rt_sigprocmask/kill/tkill/tgkill/
rt_sigreturn）已做，读数在 `[sig]` 块。两块都按行首锚点判，不靠 [linux] 的
行数（mmap/brk 只在 ELF 窗口内单调分配）。动态链接（PT_INTERP）
另有专门门禁 tools/dynlink_gate.py。以上各项是明确不支持，文档
`docs/Linux可执行文件兼容-系统调用ABI.md` 里逐条写着；本门禁不为它们背书。

用法
----
    py -3 tools/linux_abi_gate.py
    py -3 tools/linux_abi_gate.py --no-build --keep     # 复用已有 ELF，打印日志
    py -3 tools/linux_abi_gate.py --qemu <path> --la64-gcc <path> --rv32-gcc <path>

退出码 0 = 全过；1 = 任一判据不成立 / 缺工具链与 QEMU（缺工具是 FAIL，不是跳过）。
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

MAKEFILE = "example/qemu_loongarch64/Makefile"
BUILD_DIR = os.path.join(ROOT, "build", "qemu_loongarch64")
ELF_NAME = "ark_qemu_la64.elf"

LA64_FALLBACKS = [
    "D:/LoongIDE/la64-tool/bin/loongarch64-newlib-elf-gcc.exe",
]
RV32_FALLBACKS = [
    "D:/Artchip/luban-lite-master/toolchain/bin/riscv64-unknown-elf-gcc.exe",
]
QEMU_CANDIDATES = (
    "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe",
)

LA64_ARCH = ["-march=loongarch64", "-mabi=lp64d", "-mstrict-align"]
RV32_ARCH = ["-march=rv32imac", "-mabi=ilp32", "-mcmodel=medany", "-mno-relax"]

COMMON = [
    "-O2",
    "-Wall",
    "-ffreestanding",
    "-fno-common",
    "-nostdlib",
    "-nostartfiles",
    "-fno-builtin",
    "-fsyntax-only",
    "-DARKRTOS_SIGN_KEY_LOCAL=1",
]

# 与 example/qemu_loongarch64/Makefile 里的本地测试密钥同值；只为让
# features.h 的「有签名开关就必须有密钥」那条 #error 过关，不参与任何判定。
SIGN_KEY_HEADER = """#ifndef ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_IMAGE_SIGN_KEY { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F }
#endif
"""

# asm-generic 号表：这是 Linux UAPI 的事实，不是从源里读回来再自证。
EXPECT_SYS = {
    "DUP": 23,
    "DUP3": 24,
    "FCNTL": 25,
    "RSEQ": 293,
    "OPENAT": 56,
    "CLOSE": 57,
    "PIPE2": 59,
    "READ": 63,
    "WRITE": 64,
    "EXIT": 93,
    "EXIT_GROUP": 94,
    "SET_TID_ADDRESS": 96,
    "CLOCK_GETTIME": 113,
    "UNAME": 160,
    "GETCPU": 168,
    "GETPID": 172,
    "GETUID": 174,
    "GETEUID": 175,
    "BRK": 214,
    "MUNMAP": 215,
    "EXECVE": 221,
    "MMAP": 222,
    "MPROTECT": 226,
    "PREAD64": 67,
    "KILL": 129,
    "TKILL": 130,
    "TGKILL": 131,
    "RT_SIGACTION": 134,
    "RT_SIGPROCMASK": 135,
    "RT_SIGRETURN": 139,
}

# 程序自己打印的检查条数（linux_hello.c 里每一个 check() 一行）
EXPECT_CHECKS = 14
# 管道夹具（linux_pipe.c）的 check 条数：O_NONBLOCK 档 16 条（pipe2 rc /
# 两个 fd / 写 5 字节 / 读 5 字节 / 内容逐字节 / 空读 -EAGAIN / 关写端 /
# 读回 EOF / O_CLOEXEC 被接受的两条 / 第二条 pipe2 / 满时短写 / 读回 256 /
# 排空后再写 / 先忽略 SIGPIPE / 读端关掉 -EPIPE）＋ 默认阻塞档 14 条（阻塞
# pipe2、写/读/内容、关写端、阻塞读 EOF、SIG_IGN 前置、SIG_IGN 下 -EPIPE、
# 装 handler 的 sigaction、-EPIPE、handler 次数/信号号、收尾 close）= 30 条。
EXPECT_PIPE_CHECKS = 30
# 信号夹具（linux_signal.c）的 check 条数：动作/旧动作、kill 返回值跨处理
# 函数、hits/signo、嵌套 hits/depth/rc、block/query/unblock、sigsetsize、
# SIGKILL、无 restorer、被拒动作未装上、别的 pid、sig=99、tgkill 的
# tgid!=tid、SIG_IGN 静音、还原 SIG_DFL、浮点寄存器区（handler 里看到 / 
# sigreturn 之后恢复）= 31 条。LSX 档另有两条向量区判据，那两条只在
# EXTRA_CFLAGS=-DARKRTOS_USE_LSX=1 的构建里存在（本门禁跑默认档，因此
# 默认档恒 31 条；LSX 档实跑同为 31 条，只是最后两条换成 vr0 的两条）。
EXPECT_SIG_CHECKS = 31
# dup 夹具（linux_dup.c）的 check 条数：dup 拿新号 / 走副本写读 / 关掉源后
# 副本仍在 / 两端全关 EOF / dup3 的两个失败面（EBADF、old==new 的 EINVAL）/
# dup3(1,6,O_CLOEXEC) 成功且 F_GETFD 读回 FD_CLOEXEC（原先这里是「被拒」）/
# dup3 覆盖已有 fd / dup3 把 1 与 2 重定向再撤销 / dup(0) 的控制台引用 /
# 同一号关两次 -EBADF / 表满 -ENFILE 与「复用已有别名那一格」，
# 再加第 6 组 fcntl(25) 的 20 条（F_GETFD/F_SETFD 往返、管道两端与控制台的
# F_GETFL、对管道清 O_NONBLOCK 回 0 + 清完再查/再置再查（U-26 起管道两端
# 可自由清置 NONBLOCK）、F_DUPFD / F_DUPFD_CLOEXEC 取号、未知 cmd 与坏 fd、
# 四条收尾 close）= 56 条。
EXPECT_DUP_CHECKS = 56
# rseq 夹具（linux_rseq.c）的 check 条数：getcpu rc / cpu / node 三条；rseq
# 注册 rc 与 cpu_id_start / cpu_id 的写回三条；信号中止四条（aborted、
# 提交段没跑、rseq_cs 被清、handler 跑了一次）；没有信号的临界区三条
# （正常提交、没被误中止、提交后 rseq_cs 被清）；注册契约与注销三条
# （len != 32、flags != 0、NULL 注销）= 16 条。
EXPECT_RSEQ_CHECKS = 16

FAILS = []
PASSES = []


def ok(name, detail):
    PASSES.append(name)
    print("PASS  %-24s %s" % (name, detail))


def bad(name, detail):
    FAILS.append(name)
    print("FAIL  %-24s %s" % (name, detail))


def read_text(path):
    with open(path, "rb") as f:
        raw = f.read()
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def _pick(cands, given, what):
    if given:
        if os.path.isfile(given):
            return given
        bad("toolchain", "%s 不存在：%s" % (what, given))
        return None
    for c in cands:
        if os.path.isfile(c):
            return c
    bad("toolchain", "找不到 %s（缺工具链是 FAIL，不是跳过）" % what)
    return None


def nm_of(cc):
    """从交叉 gcc 的路径推出同目录的 nm；推不出就明确报错，不静默跳过符号检查。"""
    d = os.path.dirname(cc)
    base = os.path.basename(cc)
    for suffix in ("-gcc.exe", "-gcc"):
        if base.endswith(suffix):
            cand = os.path.join(d, base[: -len(suffix)] + "-nm.exe")
            if os.path.isfile(cand):
                return cand
            cand = os.path.join(d, base[: -len(suffix)] + "-nm")
            if os.path.isfile(cand):
                return cand
    for name in os.listdir(d):
        if name.endswith("-nm.exe") and "newlib" in name:
            return os.path.join(d, name)
    return None


def run(cc, args):
    r = subprocess.run([cc] + args, cwd=ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out = (r.stdout or b"") + (r.stderr or b"")
    return r.returncode, read_text_bytes(out)


def read_text_bytes(raw):
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def includes(board, port):
    return [
        "-Ikernelsrc/include",
        "-Iconfig",
        "-Iboard/" + board,
        "-Ikernelsrc/port/" + port,
    ]


# ---------------------------------------------------------------- A 默认档

def check_default_off():
    ac = read_text(os.path.join(ROOT, "kernelsrc/include/arkrtos_autoconf.h"))
    m = re.search(r"^#define\s+ARKRTOS_USE_LINUX_ABI\s+(\d+)\s*$", ac, re.M)
    if not m:
        bad("default-off", "arkrtos_autoconf.h 里没有 ARKRTOS_USE_LINUX_ABI（开关没进 Kconfig 生成物）")
    elif m.group(1) != "0":
        bad("default-off", "默认档 ARKRTOS_USE_LINUX_ABI=%s（必须是 0）" % m.group(1))
    else:
        ok("default-off", "arkrtos_autoconf.h: ARKRTOS_USE_LINUX_ABI 0")

    kc = read_text(os.path.join(ROOT, "config/kconfig/Kconfig.features"))
    if "config ARKRTOS_USE_LINUX_ABI" not in kc:
        bad("kconfig-wired", "Kconfig.features 里没有 config ARKRTOS_USE_LINUX_ABI")
    elif "depends on ARKRTOS_USE_ELF" not in kc:
        bad("kconfig-wired", "Kconfig 项没有 depends on ARKRTOS_USE_ELF（会编出没有装载器的兼容层）")
    else:
        ok("kconfig-wired", "Kconfig 项存在且 depends on ARKRTOS_USE_ELF")

    fh = read_text(os.path.join(ROOT, "kernelsrc/include/arkrtos_features.h"))
    if "ARKRTOS_USE_LINUX_ABI == 1) && (ARKRTOS_USE_ELF != 1" not in fh:
        bad("features-guard", "features.h 缺「开 ABI 必须开 ELF」的 #error")
    else:
        ok("features-guard", "features.h 有 ABI 依赖 ELF 的编译期护栏")

    bc = read_text(os.path.join(ROOT, "board/qemu_loongarch64/arkrtos_board_config.h"))
    if not re.search(r"^#define\s+ARKRTOS_USE_LINUX_ABI\s+1\s*$", bc, re.M):
        bad("board-switch", "QEMU LA64 板级没有把 ARKRTOS_USE_LINUX_ABI 置 1")
    else:
        ok("board-switch", "QEMU LA64 板级显式置 1（这一档才是实跑档）")


# ---------------------------------------------------------------- B 关档零符号

def check_off_zero_symbols(rv32, nm, key_dir):
    tmp = tempfile.mkdtemp(prefix="ark_lxabi_off_")
    obj = os.path.join(tmp, "linux_abi_off.o")
    flags = RV32_ARCH + [
        "-c", "-o", obj,
        "-DARKRTOS_SIGN_KEY_LOCAL=1",
        "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
        "-include", "arkrtos_board_config.h",
        "-I" + key_dir,
    ] + includes("qemu_riscv32", "riscv/rv32imac")
    rc, out = run(rv32, flags + ["kernelsrc/src/arkrtos_linux_abi.c"])
    if rc != 0:
        bad("off-zero-symbols", "关档把 arkrtos_linux_abi.c 编成 .o 失败：\n%s" % out[-500:])
        return
    if not os.path.isfile(obj):
        bad("off-zero-symbols", "关档编译没产出 .o")
        return
    r = subprocess.run([nm, obj], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    text = read_text_bytes((r.stdout or b"") + (r.stderr or b""))
    defs = [ln for ln in text.splitlines() if re.search(r"\s[TtDdBbRr]\s", ln)]
    undef = [ln for ln in text.splitlines() if re.search(r"^\s+U\s", ln)]
    if defs or undef:
        bad("off-zero-symbols",
            "关档仍有 %d 个定义符号 / %d 个未定义引用（第一行：%r）"
            % (len(defs), len(undef), (defs + undef)[0].strip()))
    else:
        ok("off-zero-symbols", "关档 .o 符号表全空（零体积，不是「没测」）")


# ---------------------------------------------------------------- C 接线两向

def _board_without_switch(tmp):
    """把板级里的两行 Linux ABI 开关删掉，得到一个「默认档」探针头。"""
    src = read_text(os.path.join(ROOT, "board/qemu_loongarch64/arkrtos_board_config.h"))
    pat = re.compile(r"^#undef\s+ARKRTOS_USE_LINUX_ABI\s*\n"
                     r"^#define\s+ARKRTOS_USE_LINUX_ABI\s+1[^\n]*\n",
                     re.M)
    if len(pat.findall(src)) != 1:
        return None
    out = pat.sub("", src, count=1)
    d = os.path.join(tmp, "probe_board")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "arkrtos_board_config.h"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(out)
    return d


def check_port_wiring(la64, nm, key_dir):
    tmp = tempfile.mkdtemp(prefix="ark_lxabi_port_")
    probe = _board_without_switch(tmp)
    if probe is None:
        bad("port-wiring", "板级里找不到成对的 ABI 开关两行，判据已过期")
        return

    def build(board_dir, tag):
        obj = os.path.join(tmp, "port_%s.o" % tag)
        flags = LA64_ARCH + [
            "-c", "-o", obj,
            "-DARKRTOS_SIGN_KEY_LOCAL=1",
            "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
            "-include", "arkrtos_board_config.h",
            "-I" + key_dir,
            "-I" + board_dir,
            "-Ikernelsrc/include",
            "-Iconfig",
            "-Ikernelsrc/port/loongarch/la64",
        ]
        rc, out = run(la64, flags + ["kernelsrc/port/loongarch/la64/arkrtos_port.c"])
        if rc != 0:
            return None, "编 %s 档失败：\n%s" % (tag, out[-400:])
        r = subprocess.run([nm, obj], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return read_text_bytes((r.stdout or b"") + (r.stderr or b"")), None

    off_text, err = build(probe, "off")
    if err:
        bad("port-wiring", err)
        return
    on_text, err = build(os.path.join(ROOT, "board/qemu_loongarch64"), "on")
    if err:
        bad("port-wiring", err)
        return

    has_off = "arkrtos_port_linux_syscall_num" in off_text
    has_on = "arkrtos_port_linux_syscall_num" in on_text
    if has_off:
        bad("port-wiring", "关档 port 里仍有 arkrtos_port_linux_syscall_num（开关没包住实现）")
    elif not has_on:
        bad("port-wiring", "开档 port 里没有 arkrtos_port_linux_syscall_num（开关没接线）")
    else:
        ok("port-wiring", "关档无符号 / 开档有符号（两个方向都测了）")


# ---------------------------------------------------------------- D 开档编得过

def check_compile_on(la64, key_dir):
    flags = LA64_ARCH + COMMON + [
        "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
        "-include", "arkrtos_board_config.h",
        "-I" + key_dir,
        "-Ikernelsrc/components/mdk_trace",
        "-Ikernelsrc/shell",
        "-Ikernelsrc/shell/ark_shell",
        "-Ikernelsrc/components/ark_vfs/include",
        "-Ikernelsrc/components/ark_vfs/port",
        "-Ikernelsrc/components/ark_vfs/src",
        "-Ikernelsrc/fs",
        "-Ikernelsrc/fs/littlefs",
        "-Ikernelsrc/components/ark_softbus/include",
        "-Ikernelsrc/components/ark_softbus/src",
        "-Ikernelsrc/components/ark_softbus/port",
    ] + includes("qemu_loongarch64", "loongarch/la64")
    files = [
        "kernelsrc/src/arkrtos_linux_abi.c",
        "kernelsrc/src/arkrtos_task.c",
        "kernelsrc/src/arkrtos_elf.c",
        "kernelsrc/port/loongarch/la64/arkrtos_port.c",
    ]
    bads = []
    for rel in files:
        rc, out = run(la64, flags + [rel])
        if rc != 0:
            bads.append("%s：\n%s" % (rel, out[-400:]))
    # 用户程序自己：没有 Ark SDK，只有 Linux 号
    prog = run(la64, LA64_ARCH + [
        "-O2", "-Wall", "-ffreestanding", "-fno-common",
        "-nostdlib", "-nostartfiles", "-fno-builtin", "-fsyntax-only",
        "example/qemu_loongarch64/elf/linux_hello.c",
        "example/qemu_loongarch64/elf/linux_execve.c",
        "example/qemu_loongarch64/elf/linux_pipe.c",
        "example/qemu_loongarch64/elf/linux_signal.c",
        "example/qemu_loongarch64/elf/linux_dup.c",
    ])
    if prog[0] != 0:
        bads.append("elf/linux_hello.c：\n%s" % prog[1][-400:])
    if bads:
        bad("compile-on", "开档编不过 —— " + bads[0])
    else:
        ok("compile-on", "9 个 TU 在 LA64 真板级下全编过（含五个用户程序本身）")


# ---------------------------------------------------------------- E 号表

def check_syscall_table():
    src = read_text(os.path.join(ROOT, "kernelsrc/src/arkrtos_linux_abi.c"))
    got = {}
    # 号名里可能有数字（PIPE2），名字类不许漏掉这一类。
    for m in re.finditer(r"^#define\s+LX_SYS_([A-Z0-9_]+)\s+\((\d+)u?\)", src, re.M):
        got[m.group(1)] = int(m.group(2))
    problems = []
    for name, want in sorted(EXPECT_SYS.items()):
        if name not in got:
            problems.append("缺 LX_SYS_%s" % name)
        elif got[name] != want:
            problems.append("LX_SYS_%s=%d（Linux UAPI 是 %d）" % (name, got[name], want))
    if problems:
        bad("syscall-table", "；".join(problems))
    else:
        ok("syscall-table", "%d 条号与 asm-generic 逐条相等" % len(EXPECT_SYS))

    la = read_text(os.path.join(ROOT, "kernelsrc/port/loongarch/la64/arkrtos_port.c"))
    rv = read_text(os.path.join(ROOT, "kernelsrc/port/riscv/rv32imac/arkrtos_port.c"))
    if "(insn & LA_INSN_SYSCALL_NUM) != 0u" not in la:
        bad("abi-split-la64", "LA64 里没有「立即数 != 0 就不是 Linux ABI」的分界")
    else:
        ok("abi-split-la64", "LA64：syscall 立即数 0 = Linux ABI，非 0 = Ark 号")

    # SVC 0x23（SEC-G08 upcall 返回通道）把 Ark 的号段推到 0x23：号表这边
    # 必须同步，否则「0x23 归谁也不说」—— Linux 侧恰好没有 0x23（最小 56），
    # 但把号段写死成 0x22 会让 future 的 0x23 变成「先到先得」。
    m = re.search(r"num\s*>=\s*0x10u\)\s*&&\s*\(num\s*<=\s*0x23u", rv)
    if not m:
        bad("abi-split-rv32", "RV32 里找不到 0x10..0x23 归 Ark 的判据")
    else:
        # 号段不撞这条判据要分两步看，否则「Ark 把号段留宽一点」会被误判成缺陷：
        #   * 落在 0x10..0x23 里的 Linux 号只在「这个端口真的跑 Linux ABI」时
        #     才有歧义（RV32 的 port 按「先看号段、再看表」分派，号段赢）；
        #   * RV32 板级从不打开 ELF，而 features.h 守着「开 ABI 必须开 ELF」，
        #     所以 ABI 在 RV32 上由构造保证是关的。这里按文件核实，不靠推断。
        collide = sorted(n for n in EXPECT_SYS.values() if 0x10 <= n <= 0x23)
        rvboard = read_text(os.path.join(ROOT, "board/qemu_riscv32/arkrtos_board_config.h"))
        abi_on = re.search(r"^#\s*define\s+ARKRTOS_USE_LINUX_ABI\s+1\b", rvboard, re.M)
        elf_on = re.search(r"^#\s*define\s+ARKRTOS_USE_ELF\s+1\b", rvboard, re.M)
        if abi_on or elf_on:
            bad("abi-split-rv32",
                "RV32 板级打开了 ELF/ABI，而 Linux 号 %s 落进 Ark 号段 0x10..0x23"
                % collide)
        elif collide:
            ok("abi-split-rv32",
               "RV32：0x10..0x23 归 Ark；Linux 号 %s 落在这段里，但 RV32 板级不开 "
               "ELF/ABI（开 ABI 必须开 ELF 是 features.h 的 #error），歧义不可达"
               % collide)
        else:
            ok("abi-split-rv32", "RV32：0x10..0x23 归 Ark，Linux 号表与号段不交")

    # 0x23 的另一半：内核里必须真有这条分派（号表 + 分派 + 头文件三处同源），
    # 否则「号段归 Ark」只是让位，叫了没人应答。
    task_src = read_text(os.path.join(ROOT, "kernelsrc/src/arkrtos_task.c"))
    def_src = read_text(os.path.join(ROOT, "kernelsrc/include/arkrtos_def.h"))
    if "#define ARKRTOS_SVC_USHELL_RET        (0x23)" not in def_src:
        bad("svc-0x23", "arkrtos_def.h 里没有 ARKRTOS_SVC_USHELL_RET (0x23)")
    elif "case ARKRTOS_SVC_USHELL_RET:" not in task_src:
        bad("svc-0x23", "SVC_Server 里没有 0x23 的分派")
    elif "arkrtos_ushell_upcall_ret" not in task_src:
        bad("svc-0x23", "0x23 的分派没有落到 arkrtos_ushell_upcall_ret")
    else:
        ok("svc-0x23", "0x23 归 Ark：号定义、分派、upcall 返回通道三处同源")


# ---------------------------------------------------------------- F 真读数

def build_image():
    log = os.path.join(tempfile.gettempdir(), "ark_lxabi_build.log")
    with open(log, "wb") as f:
        r = subprocess.run(["make", "-f", MAKEFILE, "-j8"], cwd=ROOT,
                           stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        return False, read_text(log)[-1200:]
    return True, ""


def run_qemu(qemu, seconds):
    """跑 QEMU，把串口从 stdout 管道收回来。

    不用 `-serial file:` 的理由是可证伪的：QEMU 的 file 后端是块缓冲，进程被
    kill 时最后一个不满 4 KB 的块会丢。于是同一份固件有时「读数齐全」、有时
    「少了末尾几条」——那会把判据变成抽签。管道不会：写进去的字节立刻可读。
    """
    import threading
    import time

    # -kernel 只吃相对名：仓库路径含非 ASCII 字符，QEMU 在该参数上用窄字符
    # API 打开绝对路径会直接失败。所以 cwd 切到 BUILD_DIR，只传文件名。
    cmd = [qemu, "-machine", "virt", "-kernel", ELF_NAME,
           "-nographic", "-monitor", "none"]
    p = subprocess.Popen(cmd, cwd=BUILD_DIR, stdin=subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    chunks = []

    def drain():
        while True:
            b = p.stdout.read(1024)
            if not b:
                break
            chunks.append(b)

    th = threading.Thread(target=drain)
    th.daemon = True
    th.start()

    # 取最后一块的 marker：[linux] 之后还有 pipe 与 sig 两块，等的若是
    # [linux] finished 或 [pipe] finished，后面那一块会被 kill 截掉（实测过）。
    marker = b"[sig] finished after"
    deadline = time.time() + seconds
    while time.time() < deadline:
        if marker in b"".join(chunks):
            break
        time.sleep(0.05)

    # 看到 marker 只说明这一行**开始**上了串口；立刻 kill 会把 "exit code 7"
    # 那一截截掉（实测过：同一份固件有时整行、有时只剩 "exit cod"）。
    # 宽限一小段时间，让这一行写完。
    time.sleep(0.5)

    p.kill()
    p.wait()
    th.join(timeout=5)
    data = b"".join(chunks)
    if not data:
        return None
    return read_text_bytes(data)


def judge(text):
    """返回 (是否通过, 说明列表)。判据全部按行首锚点，不用子串猜。"""
    notes = []
    lines = text.splitlines()

    def count(pred):
        return sum(1 for ln in lines if pred(ln))

    checks_ok = count(lambda s: s.startswith("[linux] ") and s.rstrip().endswith(" ok"))
    checks_fail = count(lambda s: s.startswith("[linux] ") and "FAIL" in s)
    passline = count(lambda s: s.startswith("[linux] PASS: every syscall answered as specified"))
    exitline = count(lambda s: re.search(r"^\[linux\] finished after \d+ ms, exit code 7$", s) is not None)
    pipe_fail = count(lambda s: s.startswith("[pipe] ") and "FAIL" in s)
    pipe_ok = count(lambda s: s.startswith("[pipe] ") and s.rstrip().endswith(" ok"))
    pipe_pass = count(lambda s: s.startswith("[pipe] PASS: pipe2 two fds"))
    pipe_exit = count(lambda s: re.search(
        r"^\[pipe\] finished after \d+ ms, exit code 55$", s) is not None)
    sig_fail = count(lambda s: s.startswith("[sig] ") and "FAIL" in s)
    sig_ok = count(lambda s: s.startswith("[sig] ") and s.rstrip().endswith(" ok"))
    sig_pass = count(lambda s: s.startswith("[sig] PASS: sigaction installed"))
    sig_exit = count(lambda s: re.search(
        r"^\[sig\] finished after \d+ ms, exit code 138$", s) is not None)
    dup_fail = count(lambda s: s.startswith("[dup] ") and "FAIL" in s)
    dup_ok = count(lambda s: s.startswith("[dup] ") and s.rstrip().endswith(" ok"))
    dup_pass = count(lambda s: s.startswith("[dup] PASS: dup new fd"))
    dup_exit = count(lambda s: re.search(
        r"^\[dup\] finished after \d+ ms, exit code 56$", s) is not None)
    rseq_fail = count(lambda s: s.startswith("[rseq] ") and "FAIL" in s)
    rseq_ok = count(lambda s: s.startswith("[rseq] ") and s.rstrip().endswith(" ok"))
    rseq_pass = count(lambda s: s.startswith("[rseq] PASS: getcpu both outputs zero"))
    rseq_exit = count(lambda s: re.search(
        r"^\[rseq\] finished after \d+ ms, exit code 57$", s) is not None)
    rseq_werr = count(lambda s: s.startswith("[rseq] write /mnt/ram/linux_rseq rc=0"))
    rseq_xerr = count(lambda s: s.startswith("[rseq] exec rc=0"))
    dup_werr = count(lambda s: s.startswith("[dup] write /mnt/ram/linux_dup rc=0"))
    dup_xerr = count(lambda s: s.startswith("[dup] exec rc=0"))
    sig_werr = count(lambda s: s.startswith("[sig] write /mnt/ram/linux_signal rc=0"))
    sig_xerr = count(lambda s: s.startswith("[sig] exec rc=0"))
    werr = count(lambda s: s.startswith("[linux] write /mnt/ram/linux_hello rc=0"))
    xerr = count(lambda s: s.startswith("[linux] exec rc=0"))

    problems = []
    if werr != 1:
        problems.append("缺 `[linux] write ... rc=0`（把程序送进 ramfs 这一步没走到）")
    if xerr < 1:
        problems.append("缺 `[linux] exec rc=0`（装载器没接受这个程序）")
    if checks_fail != 0:
        problems.append("有 %d 条 check 打了 FAIL（兼容层回了错答案）" % checks_fail)
    if checks_ok != EXPECT_CHECKS:
        problems.append("check 通过条数 %d != %d" % (checks_ok, EXPECT_CHECKS))
    if passline != 1:
        problems.append("缺 `[linux] PASS: every syscall answered as specified`")
    if exitline != 1:
        problems.append("缺 `exit code 7`（退出码没回到装载器的读数）")

    # 管道（pipe2）那一块：30 条 check 一条 FAIL 都不许，PASS 行与退出码 55。
    if pipe_exit != 1:
        problems.append("缺 `[pipe] finished after <ms>, exit code 55`（管道夹具没跑到底）")
    if pipe_fail != 0:
        problems.append("管道夹具里有 %d 条 FAIL" % pipe_fail)
    if pipe_ok != EXPECT_PIPE_CHECKS:
        problems.append("[pipe] check 通过条数 %d != %d" % (pipe_ok, EXPECT_PIPE_CHECKS))
    if pipe_pass != 1:
        problems.append("缺 `[pipe] PASS: pipe2 two fds ...`")

    # dup 那一块：56 条 check 一条 FAIL 都不许，PASS 行与退出码 56。
    if dup_werr != 1:
        problems.append("缺 `[dup] write /mnt/ram/linux_dup rc=0`（dup 夹具没送进去）")
    if dup_xerr != 1:
        problems.append("缺 `[dup] exec rc=0`（装载器没接受 dup 夹具）")
    if dup_fail != 0:
        problems.append("dup 夹具里有 %d 条 FAIL" % dup_fail)
    if dup_ok != EXPECT_DUP_CHECKS:
        problems.append("[dup] check 通过条数 %d != %d" % (dup_ok, EXPECT_DUP_CHECKS))
    if dup_pass != 1:
        problems.append("缺 `[dup] PASS: dup new fd ...`")
    if dup_exit != 1:
        problems.append("缺 `[dup] finished after <ms>, exit code 56`（dup 夹具没跑到底）")

    # rseq/getcpu 那一块：16 条 check 一条 FAIL 都不许，PASS 行与退出码 57。
    if rseq_werr != 1:
        problems.append("缺 `[rseq] write /mnt/ram/linux_rseq rc=0`（rseq 夹具没送进去）")
    if rseq_xerr != 1:
        problems.append("缺 `[rseq] exec rc=0`（装载器没接受 rseq 夹具）")
    if rseq_fail != 0:
        problems.append("rseq 夹具里有 %d 条 FAIL（中止/注册契约回了错答案）" % rseq_fail)
    if rseq_ok != EXPECT_RSEQ_CHECKS:
        problems.append("[rseq] check 通过条数 %d != %d" % (rseq_ok, EXPECT_RSEQ_CHECKS))
    if rseq_pass != 1:
        problems.append("缺 `[rseq] PASS: getcpu both outputs zero ...`")
    if rseq_exit != 1:
        problems.append("缺 `[rseq] finished after <ms>, exit code 57`（rseq 夹具没跑到底）")

    # 信号那一块：31 条 check 一条 FAIL 都不许，PASS 行与退出码 138
    # （默认动作是「结束任务」，内核用 128+10 表达）。
    if sig_werr != 1:
        problems.append("缺 `[sig] write /mnt/ram/linux_signal rc=0`（信号夹具没送进去）")
    if sig_xerr != 1:
        problems.append("缺 `[sig] exec rc=0`（装载器没接受信号夹具）")
    if sig_fail != 0:
        problems.append("信号夹具里有 %d 条 FAIL" % sig_fail)
    if sig_ok != EXPECT_SIG_CHECKS:
        problems.append("[sig] check 通过条数 %d != %d" % (sig_ok, EXPECT_SIG_CHECKS))
    if sig_pass != 1:
        problems.append("缺 `[sig] PASS: sigaction installed ...`")
    if sig_exit != 1:
        problems.append("缺 `[sig] finished after <ms>, exit code 138`（默认动作不是 128+signo）")

    # 兼容层不许把别人的演示搞坏：同一次启动里这三条必须在
    # 这些读数在 Linux 块之前一定出现（同一次启动），所以「缺了」只可能是
    # 兼容层把别人的演示搞坏了，不可能是调度顺序。
    for tag, name in (("[hello] bye, exit code = argc", "Ark ABI 的 hello"),
                      ("[ELF] tamper check PASS", "签名反例自检"),
                      ("[FS] PASS", "ramfs 自测")):
        if count(lambda s, t=tag: s.startswith(t)) != 1:
            problems.append("同一次启动里丢掉了 %s 的读数" % name)

    if problems:
        for p in problems:
            notes.append(p)
        return False, notes
    return True, ["%d 条 check 全 ok、PASS 行与 exit code 7 都在，管道 %d 条、信号 %d 条、dup %d 条、rseq %d 条 check 全 ok（退出码 55 / 138 / 56 / 57），旁路演示未受影响"
                  % (EXPECT_CHECKS, EXPECT_PIPE_CHECKS, EXPECT_SIG_CHECKS,
                     EXPECT_DUP_CHECKS, EXPECT_RSEQ_CHECKS)]


def self_check(good_text):
    """把好日志的四种坏法喂回判据，每一种都必须被判红，否则解析器是空转。"""
    # 串口是 CRLF，坏样本按 \n 写，不归一化就一条都改不到（判红会变成假绿）。
    good_text = good_text.replace("\r\n", "\n")
    mutations = {
        "去掉 PASS 行": lambda s: s.replace(
            "[linux] PASS: every syscall answered as specified\n", ""),
        "退出码改成 3": lambda s: s.replace("exit code 7", "exit code 3"),
        "插一条 FAIL": lambda s: s.replace(
            "[linux] uname -> 0 ok",
            "[linux] uname -> 0 ok\n[linux] read 4 bytes -> 0 FAIL (want 4)"),
        "少一条 check": lambda s: s.replace("[linux] close -> 0 ok\n", ""),
        "去掉管道 PASS 行": lambda s: s.replace(
            "[pipe] PASS: pipe2 two fds", "[pipe] (quiet) pipe2 two fds"),
        "管道退出码改成 1": lambda s: s.replace(
            "exit code 55", "exit code 1"),
        "去掉信号 PASS 行": lambda s: s.replace(
            "[sig] PASS: sigaction installed", "[sig] (quiet) sigaction installed"),
        "信号退出码改成 1": lambda s: s.replace(
            "exit code 138", "exit code 1"),
        "去掉 dup PASS 行": lambda s: s.replace(
            "[dup] PASS: dup new fd", "[dup] (quiet) dup new fd"),
        "dup 退出码改成 1": lambda s: s.replace(
            "exit code 56", "exit code 1"),
        "插一条 dup FAIL": lambda s: s.replace(
            "[dup] close the reader = 0 ok",
            "[dup] close the reader = 0 ok\n[dup] bogus = 1 FAIL"),
        "少一条 dup check": lambda s: s.replace(
            "[dup] close the console reference = 0 ok\n", ""),
        "去掉 rseq PASS 行": lambda s: s.replace(
            "[rseq] PASS: getcpu both outputs zero", "[rseq] (quiet) getcpu both outputs zero"),
        "rseq 退出码改成 1": lambda s: s.replace(
            "exit code 57", "exit code 1"),
        "插一条 rseq FAIL": lambda s: s.replace(
            "[rseq] rseq(NULL) deregisters = 0 ok",
            "[rseq] rseq(NULL) deregisters = 0 ok\n[rseq] bogus = 1 FAIL"),
        "少一条 rseq check": lambda s: s.replace(
            "[rseq] getcpu node = 0 ok\n", ""),
    }
    missed = []
    for name, fn in mutations.items():
        mutated = fn(good_text)
        if mutated == good_text:
            missed.append("%s（坏样本没改到东西）" % name)
            continue
        passed, _ = judge(mutated)
        if passed:
            missed.append(name)
    return missed


# ---------------------------------------------------------------- main

def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--la64-gcc", default="")
    ap.add_argument("--rv32-gcc", default="")
    ap.add_argument("--qemu", default="")
    ap.add_argument("--no-build", action="store_true", help="复用已有 ELF")
    ap.add_argument("--seconds", type=int, default=35)
    ap.add_argument("--keep", action="store_true", help="打印串口日志尾部")
    a = ap.parse_args(argv)

    la64 = _pick(LA64_FALLBACKS, a.la64_gcc, "LoongArch64 交叉编译器")
    rv32 = _pick(RV32_FALLBACKS, a.rv32_gcc, "RISCV32 交叉编译器")
    qemu = _pick([q for q in QEMU_CANDIDATES], a.qemu, "qemu-system-loongarch64")
    nm = nm_of(la64) if la64 else None
    if la64 and nm is None:
        bad("toolchain", "找不到与 %s 配套的 nm（符号检查不能跳过）" % os.path.basename(la64))

    key_dir = tempfile.mkdtemp(prefix="ark_lxabi_key_")
    with open(os.path.join(key_dir, "arkrtos_sign_key_local.h"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(SIGN_KEY_HEADER)

    check_default_off()
    if rv32 and nm:
        check_off_zero_symbols(rv32, nm, key_dir)
    if la64 and nm:
        check_port_wiring(la64, nm, key_dir)
        check_compile_on(la64, key_dir)
    check_syscall_table()

    # 真读数
    if qemu:
        if not a.no_build:
            built, why = build_image()
            if not built:
                bad("build", "make 失败：\n%s" % why)
            else:
                ok("build", "make -f %s 通过" % MAKEFILE)
        if os.path.isfile(os.path.join(BUILD_DIR, ELF_NAME)):
            text = run_qemu(qemu, a.seconds)
            if text is None:
                bad("qemu-run", "QEMU 没写出串口文件（%s 秒内）" % a.seconds)
            else:
                passed, notes = judge(text)
                if passed:
                    ok("qemu-readout", notes[0])
                    missed = self_check(text)
                    if missed:
                        bad("parser-selfcheck", "这些坏样本没被判红：%s" % ", ".join(missed))
                    else:
                        ok("parser-selfcheck",
                           "%d 种坏法（Linux 块缺 PASS / 错退出码 / 多 FAIL / 少 check，"
                           "管道与信号块各去 PASS 行 / 各改退出码，dup 块去 PASS / "
                           "改退出码 / 插 FAIL / 少 check，rseq 块去 PASS / 改退出码 / "
                           "插 FAIL / 少 check）全被判红" % 16)
                else:
                    bad("qemu-readout", "；".join(notes))
                    bad("parser-selfcheck", "读数没过，自检无从谈起")
                if a.keep:
                    tail = "\n".join(text.splitlines()[-40:])
                    print("\n---- 串口末尾 ----\n%s\n------------------" % tail)
        else:
            bad("qemu-run", "找不到 %s（先编一次，或去掉 --no-build）" % ELF_NAME)

    print("\n%d passed, %d failed" % (len(PASSES), len(FAILS)))
    if not FAILS:
        print("LINUX-ABI-GATE OK")
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
