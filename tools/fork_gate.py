#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Linux fork/clone + wait4（ARKRTOS_USE_LINUX_FORK）的门禁。

为什么需要这一道
----------------
`ARKRTOS_USE_LINUX_FORK` 是本内核第一条「一个进程变成两个」的能力：它要在
已降权的 PLV3 任务上复制整份地址空间、再造一个任务、并且让退出码经 wait4
回到父进程。这条链上有四类假绿，每一类都能编得过、看起来像是对的：

  1. 开关没接线：Kconfig 有一项、features.h 有 `#error`，但源文件没被 `#if`
     包住 —— 默认档悄悄多出一份代码；
  2. 只改了 trap 帧、没有复制窗口：父进程能看到子的写（同一份物理页），
     子进程的 `exit(42)` 也可能"看起来"收到了 —— 这不是 fork，是共享内存；
  3. 退出码是编出来的：wait4 对着一个还没上报的任务回 0 或 SIGKILL 那种
     "看起来权威"的值；
  4. 记账不消耗：同一个号能被反复 reap，或者认不出的号也回一个数。

所以判据全部落在可证伪的读数上：默认档的符号表、开档的真编译、以及 QEMU
上真跑一遍的分父/子读数。

判据（全过才 0）
----------------
  A. 默认档：`arkrtos_autoconf.h` 里 `ARKRTOS_USE_LINUX_FORK` 必须是 0；
     Kconfig 里它是 `depends on ARKRTOS_USE_LINUX_ABI && ARKRTOS_USE_EXIT_CODE`
     且 `depends on ARKRTOS_USE_PAGING_PAGES && ARKRTOS_USE_PAGING_DEMAND`；
     features.h 有四条 `#error`（ABI / EXIT_CODE / 两个分页依赖）。
  B. 关档零符号：用 RV32 工具链把 `arkrtos_linux_abi.c` 编成 .o，不许有
     `lx_clone` / `lx_wait4` / `s_child`；用 LA64 真板级（fork 未开）把
     `arkrtos_elf.c` / `arkrtos_port.c` / `arkrtos_paging.c` 编成 .o，不许有
     `arkrtos_elf_fork` / `arkrtos_port_paging_space_clone` /
     `arkrtos_paging_space_clone`。
  C. 开档编得过：LA64 真板级 + `-DARKRTOS_USE_LINUX_FORK=1
     -DARKRTOS_USE_PAGING_DEMAND=1`，五个内核 TU 与夹具本身全编得过；
     再加一档 `-DARKRTOS_USE_PAGING_COW=1` 同样要全编得过。
  D. QEMU 真读数（四档各跑一遍）：默认 fork 档 `[fork]` 的 21 条 check、
     fork+COW 档 26 条、fork+COW+SHM 档 34 条、fork+COW+SHM+futex 档 43 条
     check 全 ` ok`（一条 FAIL 都不许）、
     `PASS: child ran in its own space, exit 42 reaped`、`exit code 0`；
     `[fork] parent pid=` 与 `[fork] child pid=` 两个数必须不同；并且同一次
     启动里 hello / FS / 其余 Linux 夹具的读数仍在（新开关不许把别人的演示
     搞坏）。三条新读数必须**逐条**出现（只比总条数会在换夹具时假绿）：
       - `blocking wait4 blocked >= 40 ms -> 1 ok`（父真的等过，不是立即返回）
       - `parent read end survived child close -> 3 ok`（fd 表是每进程一份）
       - `SIGCHLD handler ran -> 1 ok`（跨任务信号真的投到了父进程）
     COW 档另加四类内核日志断言：至少一条 `[LA64U] pg cow fork shared=<n>
     stack=<n> copied=0 frames=<n>`（共享了、栈页已私有化、一个字节都没搬）、至少一条
     `[LA64U] pg cow copy idx=<n> ref=2`（写故障时真复制了）、不许有
     `ref=1` 的 copy 行（没共享也去复制就是白搬）、以及
     `cow child saw pre-fork values (bits=3) -> 3 ok`（子进程看到的仍是
     fork 前的值，父改的是它自己那份）。
     FUTEX 档另加：九条 futex 专属读数逐条出现 —— 共享页映射成功、无等待者
     时 WAKE 回 0、值不等时 WAIT 回 -EAGAIN、不认识的 op 回 -ENOSYS、fork 出
     唤醒者、父进程在 WAIT 里真睡着且被叫醒后回 0、父进程看到唤醒者写进去的
     值、唤醒者同时看到「父等的还是 1」与「WAKE 回 1」（位掩码 3）、子进程被
     reap。这九条里最要紧的是「父 WAIT 回 0」：它在 LA64 上暴露过一个只有上板
     才看得见的坑 —— 阻塞返回时 a0（返回值寄存器）被写成 0，而 a0 同时是
     futex 的第一个参数，重放的那一次读到的 uaddr 就是 0。
     SHM 档另加：至少一条 `[LA64U] pg shm map va=<hex> pairs=<n>` 且 pairs>=1
     （共享页必须在 mmap 时刻就有实体；拖到 fork 才让父子各自缺页拿各自的帧，
     共享就退化成两次私有分配 —— 那种实现父子都不报错，只是各改各的），
     以及六条 SHM 专属读数逐条出现：两条非法形态必须回 -ENOSYS（关档/组合
     反假绿）、映射成功、起始清零、父看到子写、子看到父 fork 之后的写。
  E. 解析器自检：把合成坏日志喂给同一个判据（去掉 PASS、改退出码、插 FAIL、
     少一条 check、父子 pid 改成相同、去掉 exec rc=0），每种坏法都必须判红。

覆盖：fork 的返回值分流与地址空间复制、退出码经 wait4 回到父、记账是消耗品、
越界参数拒绝、**阻塞 wait4**（options=0，父进程真等到子进程退出）、
**fd 表按进程一份**（子 close 不影响父的读端）、**管道对象共享且引用计数正确**
（子写的数据父读得到）、**跨任务 SIGCHLD**（子退出后父的 handler 在下一次系统
调用返回前被调用）。

不覆盖：vfork / CLONE_VM 线程（无共享地址空间，一律 -EINVAL）、CLONE_FILES
（每进程 fd 表是 fork 语义的一部分，共享 fd 表这种形态本门禁不为它背书）、
文件映射 / MAP_FIXED / mprotect 强制（匿名 MAP_SHARED 共享页已由第三档覆盖；
本档的 SHM 只管「经 fork 继承的匿名共享对」，没有 shm_open、没有文件后端）。

用法
----
    py -3 tools/fork_gate.py
    py -3 tools/fork_gate.py --no-build --keep
    py -3 tools/fork_gate.py --qemu <path> --la64-gcc <path> --rv32-gcc <path>

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

FORK_FLAGS = ["-DARKRTOS_USE_LINUX_FORK=1", "-DARKRTOS_USE_PAGING_DEMAND=1"]
# COW 档 = fork 档 + 写时复制开关。本门禁跑两遍：先默认 fork 档（21 条），
# 再 fork+COW 档（26 条 + 内核 COW 日志）。绝不把 FORK_FLAGS 直接改成带
# COW —— 那样默认 fork 路径（整块克隆）就再也没被测过了。
COW_FLAGS = FORK_FLAGS + ["-DARKRTOS_USE_PAGING_COW=1"]
# SHM 档 = COW 档 + MAP_SHARED 匿名共享页，且必须叠在 COW 档上：共享位的账本
# 就是 COW 档的引用计数表，非 COW 克隆会把每一对实体复制，共享自然消失。
SHM_FLAGS = COW_FLAGS + ["-DARKRTOS_USE_PAGING_SHM=1"]
# FUTEX 档 = SHM 档 + futex WAIT/WAKE。叠在 SHM 档上而不是独立档：futex 的键
# 是「物理帧 + 页内偏移」，没有共享页就没有跨进程的同一个字可等。
FUTEX_FLAGS = SHM_FLAGS + ["-DARKRTOS_USE_LINUX_FUTEX=1"]

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

SIGN_KEY_HEADER = """#ifndef ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_IMAGE_SIGN_KEY { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F }
#endif
"""

# 夹具（elf/linux_fork.c）里的 check() 条数：
#   第一段（继承的 fork 语义）10 条：两条 -EINVAL、child pid > 0、
#   child pid != parent、wait4 返回子号、WEXITSTATUS==42、WIFEXITED、
#   父 marker 未被改、reap 后 -ECHILD、认不出的号 -ECHILD；
#   第二段（阻塞 wait4 / fd 继承）7 条：pipe2、clone>0、阻塞 wait4 得子号、
#   子退出码 7、阻塞时长 >= 40 ms、父读端存活、子写的数据到达；
#   第三段（跨任务 SIGCHLD）4 条：装 handler、handler 被调到、信号号 17、
#   回收退出码 9 —— 合计 21 条。
EXPECT_FORK_CHECKS = 21
# COW 段多 5 条：pipe2、clone>0、子被 reap、子看到 fork 前的值（位掩码 3）、
# 父自己的写还在。合计 26 条。
EXPECT_COW_CHECKS = 26
# SHM 段多 8 条：两条非法形态回 -ENOSYS（反假绿）、匿名共享映射成功、起始清零、
# clone>0、父看到子写、子被 reap、子看到父 fork 之后的写。合计 34 条。
EXPECT_SHM_CHECKS = 34
# FUTEX 段多 9 条：共享页映射、无等待者 WAKE==0、值不等 WAIT==-EAGAIN、
# 未知 op==-ENOSYS、clone>0、被叫醒的 WAIT==0、父看到新值、子被 reap、
# 唤醒者位掩码 3（看到 1 且 WAKE 回 1）。合计 43 条。
EXPECT_FUTEX_CHECKS = 43

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


def read_text_bytes(raw):
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
    d = os.path.dirname(cc)
    base = os.path.basename(cc)
    for suffix in ("-gcc.exe", "-gcc"):
        if base.endswith(suffix):
            for tail in ("-nm.exe", "-nm"):
                cand = os.path.join(d, base[: -len(suffix)] + tail)
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


def includes(board, port):
    return [
        "-Ikernelsrc/include",
        "-Iconfig",
        "-Iboard/" + board,
        "-Ikernelsrc/port/" + port,
    ]


def board_flags(board, extra=None):
    f = [
        "-DARKRTOS_SIGN_KEY_LOCAL=1",
        "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
        "-include", "arkrtos_board_config.h",
    ]
    if extra:
        f += extra
    return f


# ---------------------------------------------------------------- A 默认档

def check_default_off():
    ac = read_text(os.path.join(ROOT, "kernelsrc/include/arkrtos_autoconf.h"))
    m = re.search(r"^#define\s+ARKRTOS_USE_LINUX_FORK\s+(\d+)\s*$", ac, re.M)
    if not m:
        bad("default-off", "arkrtos_autoconf.h 里没有 ARKRTOS_USE_LINUX_FORK（开关没进 Kconfig 生成物）")
    elif m.group(1) != "0":
        bad("default-off", "默认档 ARKRTOS_USE_LINUX_FORK=%s（必须是 0）" % m.group(1))
    else:
        ok("default-off", "arkrtos_autoconf.h: ARKRTOS_USE_LINUX_FORK 0")

    kc = read_text(os.path.join(ROOT, "config/kconfig/Kconfig.features"))
    i = kc.find("config ARKRTOS_USE_LINUX_FORK")
    if i < 0:
        bad("kconfig-wired", "Kconfig.features 里没有 config ARKRTOS_USE_LINUX_FORK")
    else:
        # 只截这一段：到下一个 config/menuconfig 为止，避免把后面的项算进来。
        tail = kc[i + len("config ARKRTOS_USE_LINUX_FORK"):]
        m_end = re.search(r"^(config|menuconfig)\s", tail, re.M)
        blk = tail[:m_end.start()] if m_end else tail
        depends = " ".join(ln for ln in blk.splitlines()
                           if ln.strip().startswith("depends on"))
        needs = [
            "ARKRTOS_USE_LINUX_ABI",
            "ARKRTOS_USE_EXIT_CODE",
            "ARKRTOS_USE_PAGING_PAGES",
            "ARKRTOS_USE_PAGING_DEMAND",
        ]
        miss = [n for n in needs if n not in depends]
        if miss:
            bad("kconfig-wired", "Kconfig 项的 depends 里缺 %s" % "、".join(miss))
        else:
            ok("kconfig-wired", "Kconfig 项存在，四条依赖都在 depends 里")

    fh = read_text(os.path.join(ROOT, "kernelsrc/include/arkrtos_features.h"))
    guards = [
        "ARKRTOS_USE_LINUX_FORK=1 requires ARKRTOS_USE_LINUX_ABI=1",
        "ARKRTOS_USE_LINUX_FORK=1 requires ARKRTOS_USE_EXIT_CODE=1",
        "ARKRTOS_USE_LINUX_FORK=1 requires ARKRTOS_USE_PAGING_PAGES=1",
        "ARKRTOS_USE_LINUX_FORK=1 requires ARKRTOS_USE_PAGING_PAGES=1 and ARKRTOS_USE_PAGING_DEMAND=1",
    ]
    miss = [g for g in guards if g not in fh]
    if miss:
        bad("features-guard", "features.h 缺 %d 条 #error（第一缺：%s）" % (len(miss), miss[0]))
    else:
        ok("features-guard", "features.h 四条 #error 都在")

    m = re.search(r"^#define\s+ARKRTOS_USE_PAGING_SHM\s+(\d+)\s*$", ac, re.M)
    if not m:
        bad("default-off-shm", "arkrtos_autoconf.h 里没有 ARKRTOS_USE_PAGING_SHM")
    elif m.group(1) != "0":
        bad("default-off-shm", "默认档 ARKRTOS_USE_PAGING_SHM=%s（必须是 0）"
            % m.group(1))
    else:
        ok("default-off-shm", "arkrtos_autoconf.h: ARKRTOS_USE_PAGING_SHM 0")

    i = kc.find("config ARKRTOS_USE_PAGING_SHM")
    if i < 0:
        bad("kconfig-wired-shm", "Kconfig.features 里没有 config ARKRTOS_USE_PAGING_SHM")
    else:
        tail = kc[i + len("config ARKRTOS_USE_PAGING_SHM"):]
        m_end = re.search(r"^(config|menuconfig)\s", tail, re.M)
        blk = tail[:m_end.start()] if m_end else tail
        if "ARKRTOS_USE_PAGING_COW" not in blk:
            bad("kconfig-wired-shm",
                "Kconfig 的 SHM 项没有 depends on ARKRTOS_USE_PAGING_COW")
        else:
            ok("kconfig-wired-shm", "Kconfig 项存在，depends on ARKRTOS_USE_PAGING_COW")

    g = "ARKRTOS_USE_PAGING_SHM=1 requires ARKRTOS_USE_PAGING_COW=1"
    if g not in fh:
        bad("features-guard-shm", "features.h 缺 `#error \"%s\"`" % g)
    else:
        ok("features-guard-shm", "features.h 的 SHM 依赖 #error 在")

    m = re.search(r"^#define\s+ARKRTOS_USE_LINUX_FUTEX\s+(\d+)\s*$", ac, re.M)
    if not m:
        bad("default-off-futex", "arkrtos_autoconf.h 里没有 ARKRTOS_USE_LINUX_FUTEX")
    elif m.group(1) != "0":
        bad("default-off-futex", "默认档 ARKRTOS_USE_LINUX_FUTEX=%s（必须是 0）"
            % m.group(1))
    else:
        ok("default-off-futex", "arkrtos_autoconf.h: ARKRTOS_USE_LINUX_FUTEX 0")

    i = kc.find("config ARKRTOS_USE_LINUX_FUTEX")
    if i < 0:
        bad("kconfig-wired-futex", "Kconfig.features 里没有 config ARKRTOS_USE_LINUX_FUTEX")
    else:
        tail = kc[i + len("config ARKRTOS_USE_LINUX_FUTEX"):]
        m_end = re.search(r"^(config|menuconfig)\s", tail, re.M)
        blk = tail[:m_end.start()] if m_end else tail
        if "ARKRTOS_USE_LINUX_ABI" not in blk:
            bad("kconfig-wired-futex",
                "Kconfig 的 futex 项没有 depends on ARKRTOS_USE_LINUX_ABI")
        else:
            ok("kconfig-wired-futex", "Kconfig 项存在，depends on ARKRTOS_USE_LINUX_ABI")

    g = "requires ARKRTOS_USE_LINUX_ABI=1"
    if re.search(r'#error\s+"ARKRTOS_USE_LINUX_FUTEX=1\s+requires ARKRTOS_USE_LINUX_ABI=1"',
                 fh) is None:
        bad("features-guard-futex", "features.h 缺 futex 对 LINUX_ABI 的 #error")
    else:
        ok("features-guard-futex", "features.h 的 futex 依赖 #error 在")

    bc = read_text(os.path.join(ROOT, "board/qemu_loongarch64/arkrtos_board_config.h"))
    if re.search(r"^#define\s+ARKRTOS_USE_LINUX_FORK\s+1\s*$", bc, re.M):
        bad("board-switch", "QEMU LA64 板级不该缺省打开 fork（应靠 EXTRA_CFLAGS 出镜像）")
    else:
        ok("board-switch", "QEMU LA64 板级没有缺省打开 fork（默认档零变化）")


# ---------------------------------------------------------------- B 关档零符号

def _sym_defs(nm, obj):
    r = subprocess.run([nm, obj], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    text = read_text_bytes((r.stdout or b"") + (r.stderr or b""))
    return text


def check_off_zero_symbols(rv32, la64, nm, key_dir):
    tmp = tempfile.mkdtemp(prefix="ark_fork_off_")

    obj = os.path.join(tmp, "linux_abi_off.o")
    flags = RV32_ARCH + [
        "-c", "-o", obj,
    ] + board_flags("qemu_riscv32") + ["-I" + key_dir] + \
        includes("qemu_riscv32", "riscv/rv32imac")
    rc, out = run(rv32, flags + ["kernelsrc/src/arkrtos_linux_abi.c"])
    if rc != 0:
        bad("off-zero-symbols", "关档把 linux_abi.c 编成 .o 失败：\n%s" % out[-400:])
    else:
        text = _sym_defs(nm, obj)
        hits = [t for t in ("lx_clone", "lx_wait4", "s_child") if t in text]
        if hits:
            bad("off-zero-symbols", "关档 linux_abi.o 里仍有 %s" % "、".join(hits))
        else:
            ok("off-zero-symbols", "关档 linux_abi.o 没有 fork 记账与两个入口")

    pairs = [
        ("kernelsrc/src/arkrtos_elf.c", "arkrtos_elf_fork"),
        ("kernelsrc/port/loongarch/la64/arkrtos_port.c",
         "arkrtos_port_paging_space_clone"),
        ("kernelsrc/src/arkrtos_paging.c", "arkrtos_paging_space_clone"),
        ("kernelsrc/port/loongarch/la64/arkrtos_port.c",
         "arkrtos_port_paging_map_shared"),
    ]
    leaking = []
    for rel, sym in pairs:
        obj2 = os.path.join(tmp, os.path.basename(rel) + ".o")
        flags2 = LA64_ARCH + ["-c", "-o", obj2] + board_flags("qemu_loongarch64") + \
            ["-I" + key_dir] + includes("qemu_loongarch64", "loongarch/la64") + \
            ["-Ikernelsrc/components/ark_vfs/include",
             "-Ikernelsrc/components/ark_vfs/port",
             "-Ikernelsrc/components/ark_vfs/src",
             "-Ikernelsrc/fs", "-Ikernelsrc/fs/littlefs",
             "-Ikernelsrc/shell", "-Ikernelsrc/shell/ark_shell",
             "-Ikernelsrc/components/ark_softbus/include",
             "-Ikernelsrc/components/ark_softbus/src",
             "-Ikernelsrc/components/ark_softbus/port",
             "-Ikernelsrc/components/mdk_trace"]
        rc2, out2 = run(la64, flags2 + [rel])
        if rc2 != 0:
            leaking.append("%s 编不过：%s" % (rel, out2[-200:]))
            continue
        text2 = _sym_defs(nm, obj2)
        if re.search(r"\s[Tt]\s+%s\b" % re.escape(sym), text2):
            leaking.append("%s 里仍有 %s" % (rel, sym))
    if leaking:
        bad("off-zero-port", "；".join(leaking))
    else:
        ok("off-zero-port", "关档 elf/port/paging 三处都没有 fork 的三个新符号")


# ---------------------------------------------------------------- C 开档编得过

def check_compile_on(la64, key_dir, extra=None, tag="fork"):
    defs = FORK_FLAGS + list(extra or [])
    flags = LA64_ARCH + COMMON + board_flags("qemu_loongarch64", defs) + [
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
        "kernelsrc/src/arkrtos_elf.c",
        "kernelsrc/src/arkrtos_paging.c",
        "kernelsrc/src/arkrtos_task.c",
        "kernelsrc/port/loongarch/la64/arkrtos_port.c",
    ]
    bads = []
    for rel in files:
        rc, out = run(la64, flags + [rel])
        if rc != 0:
            bads.append("%s：\n%s" % (rel, out[-400:]))
    prog = run(la64, LA64_ARCH + list(extra or []) + [
        "-O2", "-Wall", "-ffreestanding", "-fno-common",
        "-nostdlib", "-nostartfiles", "-fno-builtin", "-fsyntax-only",
        "example/qemu_loongarch64/elf/linux_fork.c",
    ])
    if prog[0] != 0:
        bads.append("elf/linux_fork.c：\n%s" % prog[1][-400:])
    if bads:
        bad("compile-on", "开档编不过（%s）—— " % tag + bads[0])
    else:
        ok("compile-on", "6 个 TU 在 LA64 真板级 + %s 档下全编过（含夹具本身）" % tag)


# ---------------------------------------------------------------- D 真读数

def build_image(flags=None):
    flags = FORK_FLAGS if flags is None else flags
    log = os.path.join(tempfile.gettempdir(), "ark_fork_build.log")
    env = dict(os.environ)
    env["EXTRA_CFLAGS"] = " ".join(flags)
    with open(log, "wb") as f:
        # 先 clean：make 不跟踪 EXTRA_CFLAGS，上一档留下的 .o 会让这一次只做
        # 链接，于是镜像里根本没有 fork —— 那是本门禁最想抓的假绿。
        r = subprocess.run(["make", "-f", MAKEFILE, "clean"], cwd=ROOT, env=env,
                           stdout=f, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            return False, read_text(log)[-1200:]
        r = subprocess.run(["make", "-f", MAKEFILE, "-j8"], cwd=ROOT, env=env,
                           stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        return False, read_text(log)[-1200:]
    return True, ""


def restore_default_build():
    """把构建目录恢复成默认档。

    本门禁为了读 fork 的现场证据会 clean + 带 EXTRA_CFLAGS 重编；如果就这么
    收尾，后面任何一次 `make` 都会复用这些 .o（make 不跟踪 EXTRA_CFLAGS），
    把默认档悄悄变成 fork 档 —— 下一个人看到的是"默认档也有 [fork] 读数"
    这种假绿。所以收尾必须重编一次默认档。
    """
    log = os.path.join(tempfile.gettempdir(), "ark_fork_restore.log")
    with open(log, "wb") as f:
        r = subprocess.run(["make", "-f", MAKEFILE, "clean"], cwd=ROOT,
                           stdout=f, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            return False, read_text(log)[-600:]
        r = subprocess.run(["make", "-f", MAKEFILE, "-j8"], cwd=ROOT,
                           stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        return False, read_text(log)[-1200:]
    return True, ""


def run_qemu(qemu, seconds):
    """跑 QEMU，串口走管道（file 后端在 kill 时会丢最后一个不满 4 KB 的块）。"""
    import threading
    import time

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

    marker = b"[fork] finished after"
    deadline = time.time() + seconds
    while time.time() < deadline:
        if marker in b"".join(chunks):
            break
        time.sleep(0.05)

    # 看到 marker 只说明这一行开始上串口；立刻 kill 会把 "exit code 0" 截掉。
    time.sleep(0.5)

    p.kill()
    p.wait()
    th.join(timeout=5)
    data = b"".join(chunks)
    if not data:
        return None
    return read_text_bytes(data)


def judge(text, expect_checks=EXPECT_FORK_CHECKS, cow=False, shm=False,
          futex=False):
    lines = text.replace("\r\n", "\n").splitlines()

    def count(pred):
        return sum(1 for ln in lines if pred(ln))

    fk_ok = count(lambda s: s.startswith("[fork] ") and s.rstrip().endswith(" ok"))
    # 汇总行 `[fork] FAIL: N checks failed` 不是一条 check，别把它算进去
    fk_fail = count(lambda s: s.startswith("[fork] ") and "FAIL" in s
                    and not s.startswith("[fork] FAIL:"))
    passline = count(lambda s: s.startswith(
        "[fork] PASS: child ran in its own space, exit 42 reaped"))
    exitline = count(lambda s: re.search(
        r"^\[fork\] finished after \d+ ms, exit code 0$", s) is not None)
    werr = count(lambda s: s.startswith("[fork] write /mnt/ram/linux_fork rc=0"))
    xerr = count(lambda s: s.startswith("[fork] exec rc=0"))

    m_par = re.search(r"^\[fork\] parent pid=(\d+)$", text.replace("\r\n", "\n"), re.M)
    m_chi = re.search(r"^\[fork\] child pid=(\d+)$", text.replace("\r\n", "\n"), re.M)

    problems = []
    if werr != 1:
        problems.append("缺 `[fork] write /mnt/ram/linux_fork rc=0`")
    if xerr != 1:
        problems.append("缺 `[fork] exec rc=0`（装载器没接受夹具）")
    if fk_fail != 0:
        problems.append("有 %d 条 check 打了 FAIL" % fk_fail)
    if fk_ok != expect_checks:
        problems.append("check 通过条数 %d != %d" % (fk_ok, expect_checks))
    # 三条新读数必须逐条出现：只比总条数时，换一个夹具（少写一条、多写一条）
    # 仍可能凑出同一个数，那是假绿。
    req = {
        "blocking wait4 blocked >= 40 ms -> 1 ok": "阻塞 wait4 没读出等待时长",
        "parent read end survived child close -> 3 ok": "fd 表不是每进程一份（父读端被子关掉）",
        "SIGCHLD handler ran -> 1 ok": "跨任务 SIGCHLD 没投递到父进程",
    }
    if cow:
        req["cow child saw pre-fork values (bits=3) -> 3 ok"] = \
            "COW 没生效（子看到了父 fork 之后的写）"
        req["parent cow write kept -> 48879 ok"] = \
            "父进程自己的写没落到自己那份页"
    if shm:
        req["mmap(shared without anon) == -ENOSYS -> -38 ok"] = \
            "没带 MAP_ANONYMOUS 的 MAP_SHARED 居然被接受"
        req["mmap(private|shared) == -ENOSYS -> -38 ok"] = \
            "PRIVATE|SHARED 同时给居然被接受"
        req["mmap(MAP_SHARED|ANON) > 0 -> 1 ok"] = "匿名共享映射失败"
        req["shared page starts zeroed -> 0 ok"] = "共享页不是从 0 开始"
        # 这一条读的是子写进去的值本身（等值比较），所以行尾是 300 而不是 1：
        # 只判「非 0」会把「写到了别的值」也放过。
        req["parent saw child shared write -> 300 ok"] = \
            "父进程看不到子进程对共享页的写（这根本不是共享）或者看到的不是 300"
        req["shm child saw parent post-fork write (bits=1) -> 1 ok"] = \
            "子进程看不到父进程 fork 之后对共享页的写（共享对被当私有复制了）"
    if futex:
        req["futex mmap shared page > 0 -> 1 ok"] = "futex 用的共享页没建起来"
        req["futex(WAKE) with no waiter == 0 -> 0 ok"] = \
            "没有等待者时 WAKE 居然回非 0"
        req["futex(WAIT) on a different value == -EAGAIN -> -11 ok"] = \
            "值不等时 WAIT 没有立即回 -EAGAIN（说明它去睡了）"
        req["futex op other than WAIT/WAKE == -ENOSYS -> -38 ok"] = \
            "不认识的 futex op 被接受了"
        req["clone for futex child > 0 -> 1 ok"] = "futex 唤醒者没 fork 出来"
        # 最要紧的一条：LA64 上阻塞返回时 a0 被写成 0，而 a0 就是 uaddr。
        req["futex(WAIT) returned 0 after being woken -> 0 ok"] = \
            "父进程没被 WAKE 叫醒（或重放时把 uaddr 读成了返回值 0）"
        req["parent saw the value the waker wrote -> 2 ok"] = \
            "父进程醒来后没看到唤醒者写的 2（等的不是同一个字）"
        req["futex child reaped -> 12 ok"] = "futex 子进程没被 reap"
        req["futex waker saw 1 and woke 1 (bits=3) -> 3 ok"] = \
            "唤醒者看到的值不是父进程等的那个 1，或 WAKE 没真叫醒 1 个等待者"
    for line, why in req.items():
        if count(lambda s2, t=line: s2.strip() == "[fork] " + t) != 1:
            problems.append("缺 `[fork] %s` —— %s" % (line, why))
    if passline != 1:
        problems.append("缺 `[fork] PASS: child ran in its own space, exit 42 reaped`")
    if exitline != 1:
        problems.append("缺 `[fork] finished after <ms>, exit code 0`")
    if not m_par:
        problems.append("缺 `[fork] parent pid=<n>`")
    if not m_chi:
        problems.append("缺 `[fork] child pid=<n>`")
    if m_par and m_chi and m_par.group(1) == m_chi.group(1):
        problems.append("父子 pid 相同（%s）—— 这不是 fork" % m_par.group(1))

    if cow:
        # 内核日志证明 fork 只挂引用、栈页已私有化、一个字节都没搬，
        # 且复制只在写故障时发生。注意：上面 text 可能带 \r\n，$ 匹配不到
        # 行尾的 \r —— 先归一化再跑正则（否则有日志也报「缺日志」）。
        nl = text.replace("\r\n", "\n")
        shares = re.findall(
            r"^\[LA64U\] pg cow fork shared=(\d+) stack=(\d+) copied=(\d+) frames=(\d+)$",
            nl, re.M)
        if not shares:
            problems.append("缺 `[LA64U] pg cow fork shared=<n> stack=<n> copied=0 frames=<n>`"
                            "（fork 没走 COW 路径）")
        else:
            broke = [t for t in shares if t[2] != "0" or int(t[0]) < 1]
            if broke:
                problems.append("COW fork 日志不对（shared=%s stacked=%s copied=%s）："
                                "应为 shared>=1 且 copied=0"
                                % (broke[0][0], broke[0][1], broke[0][2]))
            nopriv = [t for t in shares if int(t[1]) < 1]
            if nopriv:
                problems.append("栈页没有私有化（stack=%s）—— trap 帧压在栈上，"
                                "栈页必须留可写" % nopriv[0][1])
        copies = re.findall(r"^\[LA64U\] pg cow copy idx=(\d+) ref=(\d+)$", nl, re.M)
        if not copies:
            problems.append("缺 `[LA64U] pg cow copy idx=<n> ref=2`"
                            "（一次写时复制都没发生，COW 等于没接线）")
        elif any(ref == "1" for _i, ref in copies):
            problems.append("出现 ref=1 的 copy 行（没共享也去复制，白搬字节）")

    if shm:
        nl = text.replace("\r\n", "\n")
        maps = re.findall(
            r"^\[LA64U\] pg shm map va=0x[0-9a-f]+ pairs=(\d+)$", nl, re.M)
        if not maps:
            problems.append("缺 `[LA64U] pg shm map va=<hex> pairs=<n>`"
                            "（共享页没在 mmap 时刻建实体：fork 时才各自缺页拿各自的帧，"
                            "共享退化成两次私有分配）")
        elif any(int(n) < 1 for n in maps):
            problems.append("pg shm map 的 pairs=0（什么都没建）")

    # 新开关不许把别人的演示搞坏。
    for tag, name in (("[linux] PASS", "Linux ABI 夹具"),
                      ("[FS] PASS", "ramfs 自测"),
                      ("[hello] bye, exit code = argc", "Ark ABI 的 hello")):
        if count(lambda s, t=tag: s.startswith(t)) != 1:
            problems.append("同一次启动里丢掉了 %s 的读数" % name)

    if problems:
        return False, problems
    return True, ["%d 条 check 全 ok、PASS 行与 exit code 0 都在，父子 pid 不同（%s != %s），旁路演示未受影响"
                  % (expect_checks, m_par.group(1), m_chi.group(1))]


def self_check(good_text, cow=False, shm=False, futex=False):
    """把好日志的九种坏法喂回判据，每一种都必须判红，否则解析器是空转。"""
    g = good_text.replace("\r\n", "\n")
    mutations = {
        "去掉 PASS 行": lambda s: s.replace(
            "[fork] PASS: child ran in its own space, exit 42 reaped\n", ""),
        "退出码改成 3": lambda s: re.sub(r"exit code 0$", "exit code 3", s, flags=re.M),
        "插一条 FAIL": lambda s: s.replace(
            "[fork] child pid > 0 -> 1 ok",
            "[fork] child pid > 0 -> 1 ok\n[fork] bogus -> 0 FAIL (want 1)"),
        "少一条 check": lambda s: s.replace(
            "[fork] parent marker intact -> 1 ok\n", ""),
        "父子 pid 改成相同": lambda s: re.sub(
            r"^\[fork\] child pid=\d+$", lambda m: m.group(0), s, flags=re.M
        ).replace(_pid_line(s, "child"), _pid_line(s, "parent")),
        "去掉 exec rc=0": lambda s: s.replace("[fork] exec rc=0\n", ""),
        "阻塞读数改成没等": lambda s: s.replace(
            "[fork] blocking wait4 blocked >= 40 ms -> 1 ok",
            "[fork] blocking wait4 blocked >= 40 ms -> 0 FAIL (want 1)"),
        "父读端改成 -EBADF": lambda s: s.replace(
            "[fork] parent read end survived child close -> 3 ok",
            "[fork] parent read end survived child close -> -9 FAIL (want 3)"),
        "SIGCHLD 没被调到": lambda s: s.replace(
            "[fork] SIGCHLD handler ran -> 1 ok",
            "[fork] SIGCHLD handler ran -> 0 FAIL (want 1)"),
    }
    if cow:
        mutations.update({
            "COW 位掩码丢一位": lambda s: s.replace(
                "[fork] cow child saw pre-fork values (bits=3) -> 3 ok",
                "[fork] cow child saw pre-fork values (bits=3) -> 1 FAIL (want 3)"),
            "COW 父写丢失": lambda s: s.replace(
                "[fork] parent cow write kept -> 48879 ok",
                "[fork] parent cow write kept -> 0 FAIL (want 48879)"),
            # 真日志里有多条 copy 行：只删一条仍能通过判据，那不是坏样本。
            # 整类删光才是「一次写时复制都没发生」的形态。
            "缺 cow copy 行": lambda s: re.sub(
                r"^\[LA64U\] pg cow copy idx=\d+ ref=\d+$\n", "", s, flags=re.M),
            "fork 时搬了字节": lambda s: re.sub(
                r"^(\[LA64U\] pg cow fork shared=\d+ stack=\d+) copied=0",
                r"\1 copied=2",
                s, count=1, flags=re.M),
        })
    if shm:
        mutations.update({
            "缺 pg shm map 日志": lambda s: re.sub(
                r"^\[LA64U\] pg shm map va=0x[0-9a-f]+ pairs=\d+$\n", "",
                s, flags=re.M),
            "共享写没被父看到": lambda s: s.replace(
                "[fork] parent saw child shared write -> 300 ok",
                "[fork] parent saw child shared write -> 200 FAIL (want 300)"),
            "子看不到父的写": lambda s: s.replace(
                "[fork] shm child saw parent post-fork write (bits=1) -> 1 ok",
                "[fork] shm child saw parent post-fork write (bits=1) -> 0 FAIL (want 1)"),
            "共享页起始非 0": lambda s: s.replace(
                "[fork] shared page starts zeroed -> 0 ok",
                "[fork] shared page starts zeroed -> 7 FAIL (want 0)"),
            "缺共享映射成功读数": lambda s: s.replace(
                "[fork] mmap(MAP_SHARED|ANON) > 0 -> 1 ok\n", ""),
        })
    if futex:
        mutations.update({
            "futex WAIT 没被叫醒": lambda s: s.replace(
                "[fork] futex(WAIT) returned 0 after being woken -> 0 ok",
                "[fork] futex(WAIT) returned 0 after being woken -> -38 FAIL (want 0)"),
            "futex 唤醒者只看到一半": lambda s: s.replace(
                "[fork] futex waker saw 1 and woke 1 (bits=3) -> 3 ok",
                "[fork] futex waker saw 1 and woke 1 (bits=3) -> 1 FAIL (want 3)"),
            "futex 无等待者 WAKE 回 1": lambda s: s.replace(
                "[fork] futex(WAKE) with no waiter == 0 -> 0 ok",
                "[fork] futex(WAKE) with no waiter == 0 -> 1 FAIL (want 0)"),
        })
    missed = []
    nchk = (EXPECT_FUTEX_CHECKS if futex else
            (EXPECT_SHM_CHECKS if shm else
             (EXPECT_COW_CHECKS if cow else EXPECT_FORK_CHECKS)))
    for name, fn in mutations.items():
        mutated = fn(g)
        if mutated == g:
            missed.append("%s（坏样本没改到东西）" % name)
            continue
        passed, _ = judge(mutated, nchk, cow, shm, futex)
        if passed:
            missed.append(name)
    return missed


def _pid_line(text, who):
    m = re.search(r"^\[fork\] %s pid=\d+$" % who, text, re.M)
    return m.group(0) if m else "\x00"


# ---------------------------------------------------------------- main

def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--la64-gcc", default="")
    ap.add_argument("--rv32-gcc", default="")
    ap.add_argument("--qemu", default="")
    ap.add_argument("--no-build", action="store_true", help="复用已有 ELF")
    ap.add_argument("--seconds", type=int, default=45)
    ap.add_argument("--keep", action="store_true", help="打印串口日志尾部")
    a = ap.parse_args(argv)

    la64 = _pick(LA64_FALLBACKS, a.la64_gcc, "LoongArch64 交叉编译器")
    rv32 = _pick(RV32_FALLBACKS, a.rv32_gcc, "RISCV32 交叉编译器")
    qemu = _pick([q for q in QEMU_CANDIDATES], a.qemu, "qemu-system-loongarch64")
    nm = nm_of(la64) if la64 else None
    if la64 and nm is None:
        bad("toolchain", "找不到与 %s 配套的 nm（符号检查不能跳过）" % os.path.basename(la64))

    key_dir = tempfile.mkdtemp(prefix="ark_fork_key_")
    with open(os.path.join(key_dir, "arkrtos_sign_key_local.h"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(SIGN_KEY_HEADER)

    check_default_off()
    if rv32 and la64 and nm:
        check_off_zero_symbols(rv32, la64, nm, key_dir)
    if la64:
        check_compile_on(la64, key_dir)
        check_compile_on(la64, key_dir, ["-DARKRTOS_USE_PAGING_COW=1"], "fork+cow")
        check_compile_on(la64, key_dir, ["-DARKRTOS_USE_PAGING_COW=1",
                                         "-DARKRTOS_USE_PAGING_SHM=1"],
                         "fork+cow+shm")
        check_compile_on(la64, key_dir, ["-DARKRTOS_USE_PAGING_COW=1",
                                         "-DARKRTOS_USE_PAGING_SHM=1",
                                         "-DARKRTOS_USE_LINUX_FUTEX=1"],
                         "fork+cow+shm+futex")

    if qemu:
        # 四档各跑一遍：默认 fork（21 条）、fork+COW（26 条 + 内核 COW 日志）、
        # fork+COW+SHM（34 条 + 内核共享映射日志）、fork+COW+SHM+futex
        # （43 条 + 跨进程 WAIT/WAKE 配对）。别把 SHM 并进 COW 档、也别把
        # futex 并进 SHM 档 —— 那样「关档时 mmap(MAP_SHARED) / futex 仍回
        # -ENOSYS」这两条反假绿就永远测不到了。
        # 每次都要先 clean（make 不跟踪 EXTRA_CFLAGS），否则上一次的 .o 会让
        # 这一次只做链接，镜像里根本没有这一档的代码 —— 本门禁最想抓的假绿。
        rounds = [
            (FORK_FLAGS, EXPECT_FORK_CHECKS, False, False, "fork"),
            (COW_FLAGS, EXPECT_COW_CHECKS, True, False, "fork+cow"),
            (SHM_FLAGS, EXPECT_SHM_CHECKS, True, True, "fork+cow+shm"),
            (FUTEX_FLAGS, EXPECT_FUTEX_CHECKS, True, True, "fork+cow+shm+futex"),
        ]
        if a.no_build:
            rounds = rounds[-1:]
        for flags, nchk, cow, shm, tag in rounds:
            if not a.no_build:
                built, why = build_image(flags)
                if not built:
                    bad("build", "[%s] make 失败：\n%s" % (tag, why))
                    continue
                ok("build", "[%s] make -f %s EXTRA_CFLAGS=\"%s\" 通过"
                   % (tag, MAKEFILE, " ".join(flags)))
            if not os.path.isfile(os.path.join(BUILD_DIR, ELF_NAME)):
                bad("qemu-run", "[%s] 找不到 %s（先编一次，或去掉 --no-build）"
                    % (tag, ELF_NAME))
                continue
            text = run_qemu(qemu, a.seconds)
            if text is None:
                bad("qemu-run", "[%s] QEMU 没写出串口（%s 秒内）" % (tag, a.seconds))
                continue
            # 串口整段留档：以前读数不对时只剩一句「缺 X 行」，到底是夹具
            # 没打、还是串口只收到一半，分不出来 —— 那会把一次真缺陷读成
            # 一次抖动。落盘一次，失败时可回看原文。
            dump = os.path.join(tempfile.gettempdir(),
                                "ark_fork_%s.log" % tag.replace("+", "_"))
            try:
                with open(dump, "w", encoding="utf-8", newline="") as f:
                    f.write(text)
            except OSError:
                dump = ""

            futex = (tag == "fork+cow+shm+futex")
            passed, notes = judge(text, nchk, cow, shm, futex)
            if passed:
                ok("qemu-readout", "[%s] %s" % (tag, notes[0]))
                missed = self_check(text, cow, shm, futex)
                if missed:
                    bad("parser-selfcheck",
                        "[%s] 这些坏样本没被判红：%s" % (tag, "、".join(missed)))
                else:
                    ok("parser-selfcheck",
                       "[%s] 坏法（含 COW 专属）全被判红" % tag)
            else:
                bad("qemu-readout", "[%s] %s（串口 %d 行，原文见 %s）"
                    % (tag, "；".join(notes),
                       len(text.replace("\r\n", "\n").splitlines()),
                       dump or "（落盘失败）"))
                bad("parser-selfcheck", "[%s] 读数没过，自检无从谈起" % tag)
            if a.keep:
                tail = "\n".join(text.replace("\r\n", "\n").splitlines()[-40:])
                print("\n---- 串口末尾（%s） ----\n%s\n------------------" % (tag, tail))

        good, why = restore_default_build()
        if not good:
            bad("restore-default", "恢复默认档失败：\n%s" % why)
        else:
            ok("restore-default", "构建目录已恢复默认档（避免下一次 make 复用 fork 档 .o）")

    print("\n%d passed, %d failed" % (len(PASSES), len(FAILS)))
    if not FAILS:
        print("FORK-GATE OK")
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))