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
     -DARKRTOS_USE_PAGING_DEMAND=1`，四个内核 TU 与夹具本身全编得过。
  D. QEMU 真读数：`[fork]` 的 10 条 check 全 ` ok`（一条 FAIL 都不许）、
     `PASS: child ran in its own space, exit 42 reaped`、`exit code 0`；
     `[fork] parent pid=` 与 `[fork] child pid=` 两个数必须不同；并且同一次
     启动里 hello / FS / 其余 Linux 夹具的读数仍在（新开关不许把别人的演示
     搞坏）。
  E. 解析器自检：把合成坏日志喂给同一个判据（去掉 PASS、改退出码、插 FAIL、
     少一条 check、父子 pid 改成相同、去掉 exec rc=0），每种坏法都必须判红。

不覆盖：vfork / CLONE_VM 线程（无共享地址空间，一律 -EINVAL）、子进程继承
fd 表（本内核的 fd 表是全局单份，fork 不复制它 —— 这一条写进文档，本门禁
不为它背书）、父进程阻塞等子（没有阻塞系统调用通路，只用 WNOHANG）。

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

# 夹具（elf/linux_fork.c）里的 check() 条数：两条 -EINVAL、child pid > 0、
# child pid != parent、wait4 返回子号、WEXITSTATUS==42、WIFEXITED、父 marker
# 未被改、reap 后 -ECHILD、认不出的号 -ECHILD = 10 条。
EXPECT_FORK_CHECKS = 10

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

def check_compile_on(la64, key_dir):
    flags = LA64_ARCH + COMMON + board_flags("qemu_loongarch64", FORK_FLAGS) + [
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
    prog = run(la64, LA64_ARCH + [
        "-O2", "-Wall", "-ffreestanding", "-fno-common",
        "-nostdlib", "-nostartfiles", "-fno-builtin", "-fsyntax-only",
        "example/qemu_loongarch64/elf/linux_fork.c",
    ])
    if prog[0] != 0:
        bads.append("elf/linux_fork.c：\n%s" % prog[1][-400:])
    if bads:
        bad("compile-on", "开档编不过 —— " + bads[0])
    else:
        ok("compile-on", "6 个 TU 在 LA64 真板级 + fork 档下全编过（含夹具本身）")


# ---------------------------------------------------------------- D 真读数

def build_image():
    log = os.path.join(tempfile.gettempdir(), "ark_fork_build.log")
    env = dict(os.environ)
    env["EXTRA_CFLAGS"] = " ".join(FORK_FLAGS)
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


def judge(text):
    lines = text.replace("\r\n", "\n").splitlines()

    def count(pred):
        return sum(1 for ln in lines if pred(ln))

    fk_ok = count(lambda s: s.startswith("[fork] ") and s.rstrip().endswith(" ok"))
    fk_fail = count(lambda s: s.startswith("[fork] ") and "FAIL" in s)
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
    if fk_ok != EXPECT_FORK_CHECKS:
        problems.append("check 通过条数 %d != %d" % (fk_ok, EXPECT_FORK_CHECKS))
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

    # 新开关不许把别人的演示搞坏。
    for tag, name in (("[linux] PASS", "Linux ABI 夹具"),
                      ("[FS] PASS", "ramfs 自测"),
                      ("[hello] bye, exit code = argc", "Ark ABI 的 hello")):
        if count(lambda s, t=tag: s.startswith(t)) != 1:
            problems.append("同一次启动里丢掉了 %s 的读数" % name)

    if problems:
        return False, problems
    return True, ["%d 条 check 全 ok、PASS 行与 exit code 0 都在，父子 pid 不同（%s != %s），旁路演示未受影响"
                  % (EXPECT_FORK_CHECKS, m_par.group(1), m_chi.group(1))]


def self_check(good_text):
    """把好日志的六种坏法喂回判据，每一种都必须判红，否则解析器是空转。"""
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
    }
    missed = []
    for name, fn in mutations.items():
        mutated = fn(g)
        if mutated == g:
            missed.append("%s（坏样本没改到东西）" % name)
            continue
        passed, _ = judge(mutated)
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

    if qemu:
        if not a.no_build:
            built, why = build_image()
            if not built:
                bad("build", "make 失败：\n%s" % why)
            else:
                ok("build", "make -f %s EXTRA_CFLAGS=\"%s\" 通过"
                   % (MAKEFILE, " ".join(FORK_FLAGS)))
        if os.path.isfile(os.path.join(BUILD_DIR, ELF_NAME)):
            text = run_qemu(qemu, a.seconds)
            if text is None:
                bad("qemu-run", "QEMU 没写出串口（%s 秒内）" % a.seconds)
            else:
                passed, notes = judge(text)
                if passed:
                    ok("qemu-readout", notes[0])
                    missed = self_check(text)
                    if missed:
                        bad("parser-selfcheck", "这些坏样本没被判红：%s" % "、".join(missed))
                    else:
                        ok("parser-selfcheck",
                           "6 种坏法（缺 PASS / 错退出码 / 多 FAIL / 少 check / 父子同号 / 缺 exec）全被判红")
                else:
                    bad("qemu-readout", "；".join(notes))
                    bad("parser-selfcheck", "读数没过，自检无从谈起")
                if a.keep:
                    tail = "\n".join(text.replace("\r\n", "\n").splitlines()[-40:])
                    print("\n---- 串口末尾 ----\n%s\n------------------" % tail)
        else:
            bad("qemu-run", "找不到 %s（先编一次，或去掉 --no-build）" % ELF_NAME)

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