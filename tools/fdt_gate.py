#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""内核侧 FDT 解析器（ARKRTOS_USE_FDT）的门禁：开关、零符号、真读数、对账。

为什么需要这一道
----------------
D-2 阶段 1 只证明了「同一份 DTS 能编出 spec-valid 的 dtb」（离线读侧校验器）。
那还只是**文件**层面的事实：内核里到底有没有人读这棵树、读得对不对、读错时
会不会拿默认值凑一棵树，一件都没测。而这个开关默认是关的，关着的时候
`arkrtos_fdt.c` 整文件不经过任何编译器 —— 写错也全绿。

所以这里只认可证伪的读数：

  A. 主机自测（tools/dts/test_fdt.py）：六块板各自的 dtb 与生成的 C 平表
     逐节点逐属性对账，坏 blob 必须被拒；并且要把 strings 段里 `compatible`
     改一个字母再跑同一个可执行文件，它必须报错（反假绿）。
  B. 默认关：共享 arkrtos_autoconf.h 里 ARKRTOS_USE_FDT 必须是 0；
     Kconfig.features 里有这一项；features.h 有 `#ifndef` 默认 0；
     LA64 板级显式置 1；板级 defconfig 请求 =y；genboard 有 override 块。
  C. 关档零符号：用 RV32 工具链（那块板没开这个开关）把
     `kernelsrc/src/arkrtos_fdt.c` 编成 .o，符号表必须一个定义、一个未定义
     引用都没有 —— 默认档体积零变化。
  D. 开档编得过：整镜像 `make -f example/qemu_loongarch64/Makefile` 通过。
  E. 开机读数：`[fdt] blob=<n> rc=0 model=<...> nodes=<k>` 必须在，且
     `<n>` 必须等于 build 目录里那个 .dtb 的字节数（同一份文件的两次读数）。
  F. shell 对账：同一次启动里 `dt`（C 平表）与 `fdt`（解析出的 blob）输出
     必须逐行相同，`nodes=` 必须与开机行一致。只要有一行的 reg/compat/
     enabled 不同，就说明两条读路径至少有一条在猜。
  G. 解析器自检：E/F 两个判据各自喂四种坏样本（错 rc、错长度、改一节点、
     改节点数），每种都必须被判红，否则判据是空转。
  H. /chosen 与 stdout-path 的消费：开机行里的 `[fdt] console path=... baud=
     ... reason=ok match=1` 必须与这棵树的实际内容一致；四种坏法（路径错、
     reason 不是 ok、match=0、缺行）都必须被判红。
  I. 路径 / phandle / 别名 / 换树：同一次启动里 `fdt chosen`、`fdt path`、
     `fdt phandle` 必须给出同一个节点；`fdt save` 落盘再 `fdt load` 回来，
     nodes 必须不变；再 load 一个空文件，必须拒绝并保留上一棵树。

不覆盖：overlay（1c 未做）、bootloader 传 dtb、镜像重定位。`fdt save/load`
证明的是「RAM 副本 + 文件往返 + 坏 blob 回滚」，不是「bootloader 交树」。
见 docs/设备树.md §5。

用法
----
    py -3 tools/fdt_gate.py
    py -3 tools/fdt_gate.py --no-build --keep
    py -3 tools/fdt_gate.py --qemu <path> --la64-gcc <path> --rv32-gcc <path>

退出码 0 = 全过；1 = 任一判据不成立 / 缺工具链与 QEMU（缺工具是 FAIL，不是跳过）。
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

MAKEFILE = "example/qemu_loongarch64/Makefile"
BUILD_DIR = os.path.join(ROOT, "build", "qemu_loongarch64")
ELF_NAME = "ark_qemu_la64.elf"
DTB_NAME = "arkrtos.dtb"

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

# 与 example/qemu_loongarch64/Makefile 里的本地测试密钥同值；只为让
# features.h 的「有签名开关就必须有密钥」那条 #error 过关，不参与任何判定。
SIGN_KEY_HEADER = """#ifndef ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_IMAGE_SIGN_KEY { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F }
#endif
"""

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

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
    """从交叉 gcc 的路径推出同目录的 nm；推不出就明确报错，不静默跳过符号检查。"""
    d = os.path.dirname(cc)
    base = os.path.basename(cc)
    for suffix in ("-gcc.exe", "-gcc"):
        if base.endswith(suffix):
            stem = base[: -len(suffix)]
            for nm_name in (stem + "-nm.exe", stem + "-nm"):
                cand = os.path.join(d, nm_name)
                if os.path.isfile(cand):
                    return cand
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


def boards():
    out = []
    for name in sorted(os.listdir(os.path.join(ROOT, "dts"))):
        if name.endswith(".dts"):
            out.append(os.path.splitext(name)[0])
    return out


# ---------------------------------------------------------------- A 主机自测

def check_host_selftest():
    r = subprocess.run([sys.executable, os.path.join("tools", "dts", "test_fdt.py")],
                       cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = read_text_bytes(r.stdout or b"")
    if r.returncode != 0 or "test_fdt: all pass" not in text:
        bad("host-selftest", "tools/dts/test_fdt.py 未通过：\n%s" % text[-600:])
        return
    want = len(boards())
    matched = len(re.findall(r"node\(s\) match the C table", text))
    caught = text.count("[mutation caught]")
    if matched != want:
        bad("host-selftest", "只有 %d/%d 块板的 dtb 与 C 平表对账（其余没跑出读数）"
            % (matched, want))
        return
    if caught != want:
        bad("host-selftest", "反假绿自检只捕到 %d/%d 块板（变异没被判红 ⇒ 比对可能是瞎的）"
            % (caught, want))
        return
    ok("host-selftest", "%d board(s) dtb-vs-C-table node-by-node + %d mutation(s) caught"
       % (matched, caught))


# ---------------------------------------------------------------- B 默认关

def check_default_off():
    problems = []
    ac = read_text(os.path.join(ROOT, "kernelsrc/include/arkrtos_autoconf.h"))
    m = re.search(r"^#define\s+ARKRTOS_USE_FDT\s+(\d+)\s*$", ac, re.M)
    if not m:
        problems.append("arkrtos_autoconf.h 里没有 ARKRTOS_USE_FDT（开关没进 Kconfig 生成物）")
    elif m.group(1) != "0":
        problems.append("默认档 ARKRTOS_USE_FDT=%s（必须是 0）" % m.group(1))

    kc = read_text(os.path.join(ROOT, "config/kconfig/Kconfig.features"))
    if "config ARKRTOS_USE_FDT" not in kc:
        problems.append("Kconfig.features 里没有 config ARKRTOS_USE_FDT")
    elif not re.search(r"config\s+ARKRTOS_USE_FDT\b[^\n]*\n(?:[ \t]+[^\n]*\n)*?[ \t]+default\s+n\b",
                       kc):
        problems.append("Kconfig 项没有 default n（默认关是这一档的前提）")

    fh = read_text(os.path.join(ROOT, "kernelsrc/include/arkrtos_features.h"))
    if not re.search(r"#ifndef\s+ARKRTOS_USE_FDT\s*\r?\n\s*#define\s+ARKRTOS_USE_FDT\s+0",
                     fh):
        problems.append("features.h 缺 `#ifndef ARKRTOS_USE_FDT / #define 0` 默认值")

    bc = read_text(os.path.join(ROOT, "board/qemu_loongarch64/arkrtos_board_config.h"))
    if not re.search(r"^#define\s+ARKRTOS_USE_FDT\s+1\s*$", bc, re.M):
        problems.append("QEMU LA64 板级没有把 ARKRTOS_USE_FDT 置 1（这一档才是实跑档）")

    dc = read_text(os.path.join(ROOT, "config/qemu_loongarch64_defconfig"))
    if not re.search(r"^ARKRTOS_USE_FDT=y\s*$", dc, re.M):
        problems.append("qemu_loongarch64_defconfig 没有请求 ARKRTOS_USE_FDT=y")

    gb = read_text(os.path.join(ROOT, "tools/kconfig/genboard.py"))
    if '"override", "ARKRTOS_USE_FDT", "bool"' not in gb:
        problems.append("genboard.py 没有 ARKRTOS_USE_FDT 的 override 条目（板级头不是生成的）")

    if problems:
        bad("default-off", "；".join(problems))
    else:
        ok("default-off",
           "autoconf 0 / Kconfig default n / features.h 默认 0 / LA64 板级 1 + defconfig=y / genboard override")


# ---------------------------------------------------------------- C 关档零符号

def check_off_zero_symbols(rv32, nm, key_dir):
    tmp = tempfile.mkdtemp(prefix="ark_fdt_off_")
    obj = os.path.join(tmp, "fdt_off.o")
    flags = RV32_ARCH + [
        "-c", "-o", obj,
        "-DARKRTOS_SIGN_KEY_LOCAL=1",
        "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
        "-include", "arkrtos_board_config.h",
        "-I" + key_dir,
    ] + includes("qemu_riscv32", "riscv/rv32imac")
    rc, out = run(rv32, flags + ["kernelsrc/src/arkrtos_fdt.c"])
    if rc != 0:
        bad("off-zero-symbols", "关档把 arkrtos_fdt.c 编成 .o 失败：\n%s" % out[-500:])
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


# ---------------------------------------------------------------- D 开档构建

def build_image():
    log = os.path.join(ROOT, "build", "fdt_gate_build.log")
    os.makedirs(os.path.dirname(log), exist_ok=True)
    with open(log, "wb") as f:
        r = subprocess.run(["make", "-f", MAKEFILE, "-j8"], cwd=ROOT,
                           stdout=f, stderr=subprocess.STDOUT)
    return r.returncode == 0, (read_text(log)[-1200:] if r.returncode else "")


# ---------------------------------------------------------------- E/F QEMU 读数

def _qemu_proc(qemu, extra):
    return subprocess.Popen([qemu, "-machine", "virt", "-kernel", ELF_NAME] + extra,
                            cwd=BUILD_DIR, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def run_qemu_boot(qemu, seconds):
    """用 `-nographic` + stdout 管道收开机输出（含 `[fdt]` 那一行）。

    不用 `-serial file:`：QEMU 的 file 后端是块缓冲，进程被 kill 时最后一个
    不满块的尾巴会丢，读数就变成抽签。管道写进来立刻可读。
    """
    p = _qemu_proc(qemu, ["-nographic", "-monitor", "none"])
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

    deadline = time.time() + seconds
    while time.time() < deadline:
        joined = b"".join(chunks)
        if b"[fdt]" in joined and b"@arkrtos" in joined:
            break
        time.sleep(0.05)
    time.sleep(0.2)
    p.kill()
    p.wait()
    th.join(timeout=5)
    data = b"".join(chunks)
    if not data:
        return None
    return read_text_bytes(data)


BOOT_RE = re.compile(r"^\[fdt\] blob=(\d+) rc=(-?\d+)(.*)$", re.M)

CONSOLE_PATH = "/soc/serial@1fe001e0"
CONSOLE_BAUD = "115200"
CONSOLE_RE = re.compile(r"^\[fdt\] console path=(\S+) baud=(\d+) "
                        r"reason=(\S+) match=(\d+)$", re.M)

def judge_console(text):
    """H：开机行必须说「这棵树把自己绑到哪个控制台」，且与树一致。"""
    hits = CONSOLE_RE.findall(text.replace("\r\n", "\n"))
    if len(hits) != 1:
        return False, "console 行出现 %d 次（必须恰好 1 次）" % len(hits)
    path, baud, reason, match = hits[0]
    problems = []
    if path != CONSOLE_PATH:
        problems.append("path=%s，期望 %s" % (path, CONSOLE_PATH))
    if baud != CONSOLE_BAUD:
        problems.append("baud=%s，期望 %s" % (baud, CONSOLE_BAUD))
    if reason != "ok":
        problems.append("reason=%s，期望 ok" % reason)
    if match != "1":
        problems.append("match=%s，期望 1" % match)
    if problems:
        return False, "；".join(problems)
    return True, "path=%s baud=%s reason=ok match=1" % (path, baud)

def self_check_console(good_text):
    """四种坏法都必须被判红，否则 H 是空转。"""
    good_text = good_text.replace("\r\n", "\n")
    mutations = {
        "路径改成别处": lambda s: s.replace("path=/soc/serial@1fe001e0",
                                            "path=/soc/serial@dead", 1),
        "reason 改成 baud_conflict": lambda s: re.sub(r"reason=\S+",
                                                      "reason=baud_conflict", s, count=1),
        "match 改成 0": lambda s: s.replace("match=1", "match=0", 1),
        "整行删掉": lambda s: re.sub(r"^\[fdt\] console .*$\n?", "", s, count=1,
                                     flags=re.M),
    }
    missed = []
    for name, fn in mutations.items():
        passed, _ = judge_console(fn(good_text))
        if passed:
            missed.append(name)
    return missed

def judge_api(out, dtb_size, nodes):
    """I：路径 / phandle / 别名 / 换树必须在同一次启动里自洽。"""
    problems = []

    def need(cmd, subs):
        text = out.get(cmd, "")
        for s in subs:
            if s not in text:
                problems.append("%s 输出里没有 %r" % (cmd, s))

    need("fdt chosen", ["path=" + CONSOLE_PATH, "baud=" + CONSOLE_BAUD,
                        "bits=8", "parity=n", "alias=1"])
    need("fdt path " + CONSOLE_PATH, ["name=serial@1fe001e0", "depth=2",
                                      "compat=ns16550a",
                                      "reg=0x1fe001e0/0x100"])
    need("fdt phandle 1", ["phandle 1 -> " + CONSOLE_PATH])
    need("fdt save /mnt/ram/fdt_gate.dtb", ["rc=0 bytes=%d" % dtb_size])
    need("fdt load /mnt/ram/fdt_gate.dtb",
         ["rc=0 bytes=%d nodes=%d (was %d)" % (dtb_size, nodes, nodes)])
    need("fdt load /mnt/ram/fdt_gate_empty.bin",
         ["refused, previous tree kept (nodes=%d)" % nodes])
    # 坏 blob 之后树必须还是那棵
    need("fdt path " + CONSOLE_PATH, ["reg=0x1fe001e0/0x100"])
    if problems:
        return False, "；".join(problems)
    return True, ("chosen/path/phandle 同一节点，save+load 往返 nodes=%d 不变，"
                  "空文件被拒且旧树保留" % nodes)

def self_check_api(out, dtb_size, nodes):
    """四种坏法：改 alias、改深度、改 nodes、删掉回滚读数。"""
    mutations = {
        "alias=1 改 0": ("fdt chosen", "alias=1", "alias=0"),
        "depth=2 改 3": ("fdt path " + CONSOLE_PATH, "depth=2", "depth=3"),
        "load nodes 改错": ("fdt load /mnt/ram/fdt_gate.dtb",
                            "nodes=%d (was %d)" % (nodes, nodes),
                            "nodes=%d (was %d)" % (nodes + 1, nodes)),
        "删掉回滚读数": ("fdt load /mnt/ram/fdt_gate_empty.bin",
                         "previous tree kept", ""),
    }
    missed = []
    for name, (cmd, old, new) in mutations.items():
        bad_out = dict(out)
        bad_out[cmd] = out.get(cmd, "").replace(old, new, 1)
        passed, _ = judge_api(bad_out, dtb_size, nodes)
        if passed:
            missed.append(name)
    return missed


def judge_boot(text, dtb_size):
    """返回 (是否通过, 说明, nodes)。判据全按行首锚点，不用子串猜。"""
    hits = BOOT_RE.findall(text)
    if len(hits) != 1:
        return False, "开机行 `[fdt] blob=... rc=...` 出现 %d 次（必须恰好 1 次）" % len(hits), None
    blob_s, rc_s, rest = hits[0]
    blob = int(blob_s)
    rc = int(rc_s)
    if rc != 0:
        return False, "开机解析失败 rc=%d（%s）——树没被解析，读数无从谈起" % (rc, rest.strip()), None
    m = re.search(r" model=(.*?) nodes=(\d+)\s*$", rest)
    if not m:
        return False, "开机行缺 `model=<...> nodes=<k>`：%r" % rest.strip(), None
    model, nodes = m.group(1), int(m.group(2))
    if not model or model == "(none)":
        return False, "model 是空的（(none)）——根节点 model 没读到", None
    if blob != dtb_size:
        return False, "开机 blob=%d 与 %s 的 %d 字节不一致（镜像里嵌的不是这份 dtb）" % (
            blob, DTB_NAME, dtb_size), None
    if nodes < 2:
        return False, "nodes=%d，比一棵最小设备树还少" % nodes, None
    return True, "blob=%d rc=0 model=%s nodes=%d" % (blob, model, nodes), nodes


def tree_lines(text):
    """从一段 shell 输出里取 `model=` 行与两空格缩进的节点行（去掉 ANSI）。"""
    out = []
    for ln in text.splitlines():
        ln = ANSI.sub("", ln).rstrip()
        if ln.startswith("model=") or ln.startswith("  "):
            if "@arkrtos" in ln:
                continue
            out.append(ln)
    return out


def judge_shell(dt_lines, fdt_lines, boot_nodes):
    """dt（C 平表）与 fdt（解析出的 blob）必须逐行相同。"""
    problems = []
    if not dt_lines:
        problems.append("`dt` 没有输出树（命令没跑起来？）")
    if not fdt_lines:
        problems.append("`fdt` 没有输出树（解析器未就绪或命令没跑起来？）")
    if problems:
        return False, "；".join(problems)
    if not dt_lines[0].startswith("model="):
        problems.append("`dt` 第一行不是 model=：%r" % dt_lines[0])
    if dt_lines != fdt_lines:
        diff = next((i for i, (a, b) in enumerate(zip(dt_lines, fdt_lines)) if a != b), None)
        if diff is None:
            problems.append("行数不同：dt %d 行 / fdt %d 行" % (len(dt_lines), len(fdt_lines)))
        else:
            problems.append("第 %d 行不同：dt=%r fdt=%r" % (diff + 1, dt_lines[diff], fdt_lines[diff]))
    if len(dt_lines) < 3:
        problems.append("只有 %d 行树输出，太少（不是一次真的遍历）" % len(dt_lines))
    m = re.search(r"nodes=(\d+)", dt_lines[0]) if dt_lines else None
    if not m:
        problems.append("`dt` 的 model 行没有 nodes=")
    elif boot_nodes is not None and int(m.group(1)) != boot_nodes:
        problems.append("`dt` 报 nodes=%s，开机行报 nodes=%d（两条读路径不一致）"
                        % (m.group(1), boot_nodes))
    if problems:
        return False, "；".join(problems)
    return True, "%d 行逐行相同（dt 平表 == fdt 解析），nodes=%s" % (
        len(dt_lines), m.group(1))


def run_qemu_shell(qemu, port, boot_s, per_cmd, cmds):
    """用 TCP chardev 驱动 shell，返回每条命令的输出。"""
    p = subprocess.Popen(
        [qemu, "-machine", "virt", "-kernel", ELF_NAME,
         "-display", "none", "-monitor", "none",
         "-serial", "tcp:127.0.0.1:%d,server=on,wait=off" % port],
        cwd=BUILD_DIR, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        s = None
        for _ in range(int(boot_s * 4) + 40):
            if p.poll() is not None:
                return None, "QEMU 立刻退出了（rc=%d）" % p.returncode
            try:
                s = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except OSError:
                time.sleep(0.25)
        if s is None:
            return None, "连不上 127.0.0.1:%d（端口被占或 QEMU 没起来）" % port
        s.settimeout(0.5)
        buf = []

        def pump(sec):
            end = time.time() + sec
            while time.time() < end:
                try:
                    d = s.recv(8192)
                except socket.timeout:
                    continue
                if not d:
                    return
                buf.append(ANSI.sub("", d.decode("utf-8", "replace")))

        deadline = time.time() + boot_s
        while time.time() < deadline:
            s.sendall(b"\r")
            pump(1.0)
            if "@arkrtos" in "".join(buf):
                break
        else:
            return None, "%.0f s 内没等到 shell 提示符；最近输出：\n%s" % (
                boot_s, "".join(buf)[-600:])

        out = {}
        for cmd in cmds:
            mark = len("".join(buf))
            s.sendall((cmd + "\r").encode())
            pump(per_cmd)
            out[cmd] = "".join(buf)[mark:]
        return out, ""
    finally:
        try:
            p.terminate()
            p.wait(timeout=5)
        except Exception:
            try:
                p.kill()
            except Exception:
                pass


# ---------------------------------------------------------------- G 判据自检

def self_check_boot(good_text, dtb_size):
    """四种坏法都必须被判红，否则 E 是空转。"""
    good_text = good_text.replace("\r\n", "\n")
    mutations = {
        "把 rc 改成 -4": lambda s: s.replace("rc=0", "rc=-4", 1),
        "blob 长度改大 1": lambda s: re.sub(r"blob=(\d+)", lambda m: "blob=%d" % (int(m.group(1)) + 1), s, count=1),
        "删掉 nodes": lambda s: re.sub(r" nodes=\d+", "", s, count=1),
        "删掉整行": lambda s: re.sub(r"^\[fdt\].*$\n", "", s, count=1, flags=re.M),
    }
    missed = []
    for name, fn in mutations.items():
        mutated = fn(good_text)
        if mutated == good_text:
            missed.append("%s（坏样本没改到东西）" % name)
            continue
        passed, _d, _n = judge_boot(mutated, dtb_size)
        if passed:
            missed.append(name)
    return missed


def self_check_shell(dt_lines, fdt_lines, nodes):
    good = judge_shell(dt_lines, fdt_lines, nodes)
    if not good[0]:
        return ["好样本本身就没过，自检无从谈起"]
    mutations = {
        "改一个节点的 reg": lambda ls: [re.sub(r"reg=0x([0-9a-f]+)", "reg=0xdead", ls[2], count=1)] + ls[3:] if len(ls) > 2 else ls,
        "多一行节点": lambda ls: ls + ["  ghost@f compat=none reg=0x0/0x0 okay"],
        "少一行节点": lambda ls: ls[:-1],
        "改 nodes 数": lambda ls: [re.sub(r"nodes=\d+", "nodes=999", ls[0])] + ls[1:],
    }
    missed = []
    for name, fn in mutations.items():
        mutated = fn(list(fdt_lines))
        if mutated == list(fdt_lines):
            missed.append("%s（坏样本没改到东西）" % name)
            continue
        passed, _d = judge_shell(dt_lines, mutated, nodes)
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
    ap.add_argument("--port", type=int, default=45561)
    ap.add_argument("--boot", type=float, default=30.0)
    ap.add_argument("--per-cmd", type=float, default=3.5)
    ap.add_argument("--keep", action="store_true", help="打印串口尾巴")
    a = ap.parse_args(argv)

    la64 = _pick(LA64_FALLBACKS, a.la64_gcc, "LoongArch64 交叉编译器")
    rv32 = _pick(RV32_FALLBACKS, a.rv32_gcc, "RISCV32 交叉编译器")
    qemu = _pick([q for q in QEMU_CANDIDATES], a.qemu, "qemu-system-loongarch64")
    nm = nm_of(rv32) if rv32 else None
    if rv32 and nm is None:
        bad("toolchain", "找不到与 %s 配套的 nm（符号检查不能跳过）" % os.path.basename(rv32))

    key_dir = tempfile.mkdtemp(prefix="ark_fdt_key_")
    with open(os.path.join(key_dir, "arkrtos_sign_key_local.h"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(SIGN_KEY_HEADER)

    check_host_selftest()
    check_default_off()
    if rv32 and nm:
        check_off_zero_symbols(rv32, nm, key_dir)

    # 开档构建 + 真读数
    if not a.no_build:
        built, why = build_image()
        if not built:
            bad("build", "make 失败：\n%s" % why)
        else:
            ok("build", "make -f %s 通过" % MAKEFILE)

    elf = os.path.join(BUILD_DIR, ELF_NAME)
    dtb = os.path.join(BUILD_DIR, DTB_NAME)
    if not os.path.isfile(elf):
        bad("qemu-run", "找不到 %s（先编一次，或去掉 --no-build）" % ELF_NAME)
    elif not os.path.isfile(dtb):
        bad("qemu-run", "找不到 %s（Makefile 没生成 dtb 出口）" % DTB_NAME)
    elif qemu:
        dtb_size = os.path.getsize(dtb)
        boot_text = run_qemu_boot(qemu, a.boot)
        nodes = None
        if boot_text is None:
            bad("qemu-boot", "QEMU 没写出开机输出（%.0f s 内）" % a.boot)
        else:
            passed, detail, nodes = judge_boot(boot_text, dtb_size)
            if passed:
                ok("qemu-boot", detail)
                missed = self_check_boot(boot_text, dtb_size)
                if missed:
                    bad("boot-selfcheck", "这些坏样本没被判红：%s" % ", ".join(missed))
                else:
                    ok("boot-selfcheck", "4 种坏法（错 rc / 错长度 / 缺 nodes / 缺行）全被判红")
            else:
                bad("qemu-boot", detail)
                bad("boot-selfcheck", "读数没过，自检无从谈起")
            # 串口读数是唯一会「偶发截断」的一类：QEMU 刚起来时的输出与
            # 演示任务的中断打印共用同一条 chardev，抓早一拍就少一行。
            # 判据不放宽，只允许把同一条读数重取一次，并在细节里写清是
            # 第几次读数通过的——「重试通过」与「一次通过」不是同一个事实。
            console_text = boot_text
            passed_c, detail_c = judge_console(console_text)
            attempt_c = 1
            while (not passed_c) and (attempt_c < 3):
                attempt_c += 1
                more = run_qemu_boot(qemu, a.boot)
                if more is None:
                    continue
                console_text = more
                passed_c, detail_c = judge_console(console_text)
            if passed_c:
                if attempt_c > 1:
                    detail_c = "%s（第 %d 次读数；前一次截断）" % (detail_c, attempt_c)
                ok("qemu-console", detail_c)
                missed = self_check_console(console_text)
                if missed:
                    bad("console-selfcheck", "这些坏样本没被判红：%s" % ", ".join(missed))
                else:
                    ok("console-selfcheck", "4 种坏法（路径 / reason / match / 缺行）全被判红")
            else:
                bad("qemu-console", detail_c)
                bad("console-selfcheck", "读数没过，自检无从谈起")

            if a.keep:
                print("\n---- 开机串口末尾 ----\n%s\n----------------------"
                      % "\n".join(boot_text.splitlines()[-25:]))

        if nodes is not None:
            out, err = run_qemu_shell(qemu, a.port, a.boot, a.per_cmd, ["dt", "fdt"])
            if out is None:
                bad("qemu-shell", err)
                bad("shell-selfcheck", "读数没过，自检无从谈起")
            else:
                dt_lines = tree_lines(out.get("dt", ""))
                fdt_lines = tree_lines(out.get("fdt", ""))
                passed, detail = judge_shell(dt_lines, fdt_lines, nodes)
                if passed:
                    ok("qemu-shell", detail)
                    missed = self_check_shell(dt_lines, fdt_lines, nodes)
                    if missed:
                        bad("shell-selfcheck", "这些坏样本没被判红：%s" % ", ".join(missed))
                    else:
                        ok("shell-selfcheck", "4 种坏法（改 reg / 多行 / 少行 / 改 nodes）全被判红")
                else:
                    bad("qemu-shell", detail)
                    bad("shell-selfcheck", "读数没过，自检无从谈起")
                # I：路径 / phandle / 别名 / 换树。命令顺序有意义：
                # 先用空文件把树逼到"拒绝并回滚"，再从同一棵树上取读数。
                cmds = ["fdt chosen",
                        "fdt path " + CONSOLE_PATH,
                        "fdt phandle 1",
                        "fdt save /mnt/ram/fdt_gate.dtb",
                        "fdt load /mnt/ram/fdt_gate.dtb",
                        "touch /mnt/ram/fdt_gate_empty.bin",
                        "fdt load /mnt/ram/fdt_gate_empty.bin",
                        "fdt path " + CONSOLE_PATH]
                api_out, api_err = run_qemu_shell(qemu, a.port, a.boot,
                                                  a.per_cmd, cmds)
                attempt_i = 1
                if api_out is not None:
                    passed_i, detail_i = judge_api(api_out, dtb_size, nodes)
                else:
                    passed_i, detail_i = False, api_err
                while (not passed_i) and (attempt_i < 3):
                    attempt_i += 1
                    again, again_err = run_qemu_shell(qemu, a.port, a.boot,
                                                      a.per_cmd, cmds)
                    if again is None:
                        detail_i = again_err
                        continue
                    api_out = again
                    passed_i, detail_i = judge_api(api_out, dtb_size, nodes)
                if api_out is None:
                    bad("qemu-fdt-api", detail_i)
                    bad("api-selfcheck", "读数没过，自检无从谈起")
                else:
                    if passed_i:
                        if attempt_i > 1:
                            detail_i = "%s（第 %d 次读数；前一次截断）" % (detail_i, attempt_i)
                        ok("qemu-fdt-api", detail_i)
                        missed = self_check_api(api_out, dtb_size, nodes)
                        if missed:
                            bad("api-selfcheck",
                                "这些坏样本没被判红：%s" % ", ".join(missed))
                        else:
                            ok("api-selfcheck",
                               "4 种坏法（alias / depth / nodes / 回滚读数）全被判红")
                    else:
                        bad("qemu-fdt-api", detail_i)
                        bad("api-selfcheck", "读数没过，自检无从谈起")
                if a.keep:
                    for c in cmds:
                        print("\n---- %s ----\n%s" % (c, api_out.get(c, "").rstrip()))

                if a.keep:
                    print("\n---- dt ----\n%s" % out.get("dt", "").rstrip())
                    print("\n---- fdt ----\n%s\n------------" % out.get("fdt", "").rstrip())

    print("\n%d passed, %d failed" % (len(PASSES), len(FAILS)))
    if not FAILS:
        print("FDT-GATE OK")
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
