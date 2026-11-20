#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Linux 动态链接（PT_INTERP + ld.so，ARKRTOS_USE_DYNLOAD）的门禁。

为什么需要这一道
----------------
`ARKRTOS_USE_DYNLOAD` 是一个默认关的开关：打开后内核按 Linux 约定装载
`PT_INTERP` 指定的解释器、铺 auxv、把入口换成解释器；解释器是本仓自带的
`example/qemu_loongarch64/elf/ldso.c`（ark ld.so），它自己 mmap 装库、按
各对象的 PT_TLS 建一个线程块（`$tp` 指向块起点）、做 RELATIVE /
R_LARCH_64 / JUMP_SLOT / R_LARCH_TLS_TPREL64 四种重定位，再跳回主程序。

这一类功能有三处典型的假绿：

  1. 开关没接线：`elf.c` 里的动态分支编不进去，或板级没给
     `ARKRTOS_ELF_INTERP_BASE`，于是只有编译没有行为；
  2. 夹具自己就是错的：`PT_INTERP` 路径不对、`AT_PHDR = base + 64` 这条
     所有 ld.so 都依赖的约定不成立（ELF 头不在任何 PT_LOAD 里），
     ld.so 在真机上会以一堆难以归因的 fault 收场；
  3. "跑通了"其实是没验证：重定位表全 0、JUMP_SLOT 指向一个碰巧存在的
     地址，程序照样打 PASS——所以这里必须有一个**故意坏掉的夹具**
     （JUMP_SLOT 指向库里根本没有的 no_such_symbol），要求 ld.so 明确失败。

判据（全过才 0）
----------------
  A. 默认关：`arkrtos_autoconf.h` 里 `ARKRTOS_USE_DYNLOAD` 必须是 0；
     Kconfig 项存在且 `depends on ARKRTOS_USE_ELF`；`elf.c` 有「开动态链接
     必须给 INTERP_BASE/INTERP_SIZE」的 `#error`；QEMU LA64 板级给了这两个
     宏且两段不重叠（主程序窗口与解释器区间）。
  B. 关档零符号：用真板级头（默认关档）把 `elf_demo.c` 编成 .o，符号表里
     不许有 `ark_example_dynmain` / `ark_example_ld_ark` 这些夹具 blob；
     把同一份源用 `-DARKRTOS_USE_DYNLOAD=1` 编，必须都有——两个方向都测，
     只测一向时「开关写成了 1 但代码没包住」和「根本没编」看着一样。
  C. 开档编得过：`arkrtos_elf.c` / `arkrtos_linux_abi.c` 以及四个用户态
     源（ldso.c / dynmain.c / dynmain_exec 同一份 / libanswer.c）在 LA64
     真板级下 `-fsyntax-only` 全过。
  D. 夹具形状（静态、逐字节读产物）：ELF 头类型、`e_phoff == 64`、
     首个 PT_LOAD 从文件头开始（`offset == 0` 且 `vaddr == base`，这条是
     `AT_PHDR = base + 64` 的前提）、PT_INTERP 字符串、PT_DYNAMIC、
     DT_NEEDED 条数、坏夹具里确实有 `no_such_symbol`、ld.so 自己的入口
     等于解释器基址。再加一条**重建确定性**：同一个 manifest 再跑一次
     `dynfixture.py`，产物必须与构建目录里的逐字节相同。
     D2. TLS 形状：三个带 `__thread` 的夹具都必须有 PT_TLS（主程序
     filesz=memsz=16、库 8），`.rela` 里各有一条 type 11；`.dynsym` 里
     `m_tls` 是 STT_TLS 且 st_value=8（**非 0**，这是"槽没被重定位、还是 0、
     正好读到偏移 0"那把假绿的闸门）、`lib_tls` 是 STT_TLS 且 st_value=0。
  E. QEMU 真读数：两次 `[dyn] PASS: ... + TLS all applied` 且两次
     `exit code 105`；两次 `[ldso] tls base=` 与坏夹具那次共 3 条、每条
     `TPREL64=1`（库与主程序各一，共 4 条）；`TPREL64 m_tls offset -> 8`
     与 `TLS lib value -> 4242` / `after write -> 4243` 各两次；坏夹具
     `[ldso] FAIL: unresolved PLT symbol no_such_symbol` 且 `exit code 127`，
     并且**一次 PASS 都没有多出来**；`[dyn] info` 里 `interp_base=0x02100000`；
     同一次启动里 `[linux] PASS` / `[FS] PASS` 仍在（新功能不许搞坏别人）。
  F. 解析器自检：把好日志按四种坏法改动（删一条 PASS、把 127 改成 105、
     删掉 ldso 的 FAIL 行、删一条 write），每种都必须被判红。

不覆盖：真实发行版的 LA64 二进制。它们的 `PT_INTERP` 指向 glibc 的
`ld-linux-loongarch-lp64d.so.1`，仓里没有这个文件；glibc 的 ld.so 还要
futex / rseq / TLS / 文件映射等一大片本内核没有的 syscall。本门禁只为
「内核侧 Linux 约定 + 本仓 ld.so + 真实重定位路径」背书。

用法
----
    py -3 tools/dynlink_gate.py
    py -3 tools/dynlink_gate.py --no-build --keep

退出码 0 = 全过；1 = 任一判据不成立 / 缺工具链与 QEMU（缺工具是 FAIL）。
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

MAKEFILE = "example/qemu_loongarch64/Makefile"
BUILD_DIR = os.path.join(ROOT, "build", "qemu_loongarch64")
ELF_NAME = "ark_qemu_la64.elf"
DYN_FLAG = "ARKRTOS_USE_DYNLOAD=1"

LA64_FALLBACKS = [
    "D:/LoongIDE/la64-tool/bin/loongarch64-newlib-elf-gcc.exe",
]
QEMU_CANDIDATES = (
    "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe",
)

LA64_ARCH = ["-march=loongarch64", "-mabi=lp64d", "-mstrict-align"]

INTERP_PATH = "/mnt/ram/ld-ark.so"
INTERP_BASE = 0x02100000

PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3
PT_TLS = 7
ET_EXEC = 2
ET_DYN = 3
DT_NULL = 0
DT_NEEDED = 1
DT_HASH = 4
DT_STRTAB = 5
DT_SYMTAB = 6
DT_RELA = 7
DT_RELASZ = 8
DT_SONAME = 14
DT_SYMENT = 11
DT_JMPREL = 23
STT_TLS = 6

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


def run(cc, args):
    r = subprocess.run([cc] + args, cwd=ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return r.returncode, read_text_bytes((r.stdout or b"") + (r.stderr or b""))


def _pick(cands, given, what):
    if given:
        if os.path.isfile(given):
            return given
        bad("toolchain", "%s 不存在：%s" % (what, given))
        return None
    for c in cands:
        if os.path.isfile(c):
            return c
    bad("toolchain", "找不到 %s（缺工具是 FAIL，不是跳过）" % what)
    return None


def nm_of(cc):
    d = os.path.dirname(cc)
    base = os.path.basename(cc)
    for suffix in ("-gcc.exe", "-gcc"):
        if base.endswith(suffix):
            stem = base[: -len(suffix)]
            for tail in ("-nm.exe", "-nm"):
                cand = os.path.join(d, stem + tail)
                if os.path.isfile(cand):
                    return cand
    return None


# ---------------------------------------------------------------- ELF 读

def elf_header(path):
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) < 64 or raw[:4] != b"\x7fELF":
        return None
    if raw[4] != 2:  # ELFCLASS64
        return None
    (e_type, e_machine, _ver, e_entry, e_phoff, e_shoff, _flags,
     _ehsize, e_phentsize, e_phnum) = struct.unpack_from("<HHIQQQIHHH", raw, 16)
    return {
        "raw": raw,
        "e_type": e_type,
        "e_machine": e_machine,
        "e_entry": e_entry,
        "e_phoff": e_phoff,
        "e_phentsize": e_phentsize,
        "e_phnum": e_phnum,
    }


def elf_phdrs(h):
    out = []
    raw = h["raw"]
    for i in range(h["e_phnum"]):
        off = h["e_phoff"] + i * h["e_phentsize"]
        if off + 56 > len(raw):
            return None
        (p_type, p_flags, p_offset, p_vaddr, p_paddr,
         p_filesz, p_memsz, p_align) = struct.unpack_from("<IIQQQQQQ", raw, off)
        out.append({
            "type": p_type, "flags": p_flags, "offset": p_offset,
            "vaddr": p_vaddr, "filesz": p_filesz, "memsz": p_memsz,
            "align": p_align,
        })
    return out


def phdr_of(h, p_type):
    ph = elf_phdrs(h)
    if ph is None:
        return None
    for p in ph:
        if p["type"] == p_type:
            return p
    return None


def interp_string(h):
    p = phdr_of(h, PT_INTERP)
    if p is None or p["filesz"] == 0:
        return None
    raw = h["raw"]
    s = raw[p["offset"]:p["offset"] + p["filesz"]]
    return s.split(b"\x00")[0].decode("utf-8", "replace")


def dyn_entries(h):
    p = phdr_of(h, PT_DYNAMIC)
    if p is None:
        return None
    raw = h["raw"]
    out = []
    n = p["filesz"] // 16
    for i in range(n):
        tag, val = struct.unpack_from("<QQ", raw, p["offset"] + i * 16)
        out.append((tag, val))
        if tag == DT_NULL:
            break
    return out


def vaddr_to_off(h, vaddr):
    """只有 PT_LOAD 覆盖的地址能换；换不了返回 None（不猜）。"""
    ph = elf_phdrs(h)
    if ph is None:
        return None
    for p in ph:
        if p["type"] != PT_LOAD:
            continue
        if p["vaddr"] <= vaddr < p["vaddr"] + p["filesz"]:
            return p["offset"] + (vaddr - p["vaddr"])
    return None


def rela_list(h):
    """返回 [(r_offset, type, symidx, addend)]；读不到返回 None。

    对 ET_DYN 与 ET_EXEC 都一样：dynfixture 把 DT_RELA 写成绝对 vaddr，
    ET_EXEC 时它已含窗口基址，所以 vaddr_to_off 直接能换。
    """
    ents = dyn_entries(h)
    if ents is None:
        return None
    d = dict(ents)
    if DT_RELA not in d:
        return []
    sz = d.get(DT_RELASZ, 0)
    off = vaddr_to_off(h, d[DT_RELA])
    if off is None:
        return None
    out = []
    for i in range(sz // 24):
        r_off, r_info, r_add = struct.unpack_from("<QQq", h["raw"], off + i * 24)
        out.append((r_off, r_info & 0xFFFFFFFF, r_info >> 32, r_add))
    return out

def dyn_symbols(h):
    """返回 [dict(name, info, shndx, value)]；条数取自 DT_HASH 的 nchain。"""
    ents = dyn_entries(h)
    if ents is None:
        return None
    d = dict(ents)
    if DT_SYMTAB not in d or DT_STRTAB not in d or DT_HASH not in d:
        return None
    hash_off = vaddr_to_off(h, d[DT_HASH])
    sym_off = vaddr_to_off(h, d[DT_SYMTAB])
    str_off = vaddr_to_off(h, d[DT_STRTAB])
    if hash_off is None or sym_off is None or str_off is None:
        return None
    _nb, nchain = struct.unpack_from("<II", h["raw"], hash_off)
    syment = d.get(DT_SYMENT, 24)
    rawb = h["raw"]
    out = []
    for i in range(nchain):
        nameoff, info, _other, shndx, value, _size = struct.unpack_from(
            "<IBBHQQ", rawb, sym_off + i * syment)
        end = rawb.find(b"\0", str_off + nameoff)
        name = rawb[str_off + nameoff:end].decode("utf-8", "replace")
        out.append({"name": name, "info": info, "shndx": shndx, "value": value})
    return out

def dyn_count(h, tag):
    ents = dyn_entries(h)
    if ents is None:
        return None
    return sum(1 for t, _v in ents if t == tag)


# ---------------------------------------------------------------- A 默认关

def check_default_off():
    ac = read_text(os.path.join(ROOT, "kernelsrc/include/arkrtos_autoconf.h"))
    m = re.search(r"^#define\s+ARKRTOS_USE_DYNLOAD\s+(\d+)\s*$", ac, re.M)
    if not m:
        bad("default-off", "arkrtos_autoconf.h 里没有 ARKRTOS_USE_DYNLOAD（开关没进生成物）")
    elif m.group(1) != "0":
        bad("default-off", "默认档 ARKRTOS_USE_DYNLOAD=%s（必须是 0）" % m.group(1))
    else:
        ok("default-off", "arkrtos_autoconf.h: ARKRTOS_USE_DYNLOAD 0")

    kc = read_text(os.path.join(ROOT, "config/kconfig/Kconfig.features"))
    idx = kc.find("config ARKRTOS_USE_DYNLOAD")
    if idx < 0:
        bad("kconfig-wired", "Kconfig.features 里没有 config ARKRTOS_USE_DYNLOAD")
    else:
        block = kc[idx:idx + 400]
        if "depends on ARKRTOS_USE_ELF" not in block:
            bad("kconfig-wired", "Kconfig 项没有 depends on ARKRTOS_USE_ELF")
        elif "default y" in block:
            bad("kconfig-wired", "Kconfig 项写了 default y（这个功能必须默认关）")
        else:
            ok("kconfig-wired", "Kconfig 项存在、depends on USE_ELF、默认关")

    src = read_text(os.path.join(ROOT, "kernelsrc/src/arkrtos_elf.c"))
    need = ('ARKRTOS_ELF_INTERP_BASE', 'ARKRTOS_ELF_INTERP_SIZE')
    missing = [n for n in need if ('#error' not in src or n not in src)]
    if missing:
        bad("elf-guards", "elf.c 缺少对 %s 的 #error 护栏" % ", ".join(missing))
    else:
        ok("elf-guards", "elf.c 对 INTERP_BASE/INTERP_SIZE 有编译期护栏")

    vals = board_numbers()
    if vals is None:
        return
    ib, isz, wb, wsz = (vals.get(k) for k in (
        "ARKRTOS_ELF_INTERP_BASE", "ARKRTOS_ELF_INTERP_SIZE",
        "ARKRTOS_ELF_WINDOW_BASE", "ARKRTOS_ELF_WINDOW_SIZE"))
    if None in (ib, isz, wb, wsz):
        bad("board-layout", "板级四个宏不全：%r" % (vals,))
    elif (ib < wb) or (ib + isz > wb + wsz):
        bad("board-layout", "解释器区间 [0x%x,0x%x) 不在 ELF 窗口 [0x%x,0x%x) 内"
            % (ib, ib + isz, wb, wb + wsz))
    elif ib <= wb:
        bad("board-layout", "解释器基址不在主程序窗口之上（内核按 img.hi <= interp.lo 分区）")
    else:
        ok("board-layout", "解释器区间 = 0x%x..0x%x，与窗口 0x%x..0x%x 不重叠"
           % (ib, ib + isz, wb, wb + wsz))


def board_numbers():
    """让编译器把板级宏展开成整数（宏之间互相引用，自己递归解析等于重写预处理器）。

    做法：把四个宏各写一行 `ARK_NUM(x) x` 之后用 -E -P 展开，再从展开结果里
    只取算术表达式、按白名单字符校验后求值。取不到就明确失败，不猜默认值。
    """
    names = ["ARKRTOS_ELF_WINDOW_BASE", "ARKRTOS_ELF_WINDOW_SIZE",
             "ARKRTOS_ELF_INTERP_BASE", "ARKRTOS_ELF_INTERP_SIZE"]
    tmp = tempfile.mkdtemp(prefix="ark_dyn_mac_")
    hdr = os.path.join(tmp, "arkrtos_sign_key_local.h")
    with open(hdr, "w", encoding="utf-8", newline="\n") as f:
        f.write("#ifndef ARKRTOS_SIGN_KEY_LOCAL_H\n#define ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_IMAGE_SIGN_KEY { 1,2,3,4 }\n#endif\n")
    src = os.path.join(tmp, "probe.c")
    with open(src, "w", encoding="utf-8", newline="\n") as f:
        for n in names:
            # 第一个 token 不是宏（前缀不是它），会原样留下当行首标记；第二个展开成数字表达式。
            f.write("ark_num_%s %s\n" % (n, n))
    cc = LA64_FALLBACKS[0]
    if not os.path.isfile(cc):
        bad("board-layout", "找不到交叉编译器，无法展开板级宏")
        return None
    rc, out = run(cc, ["-E", "-P",
                       "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
                       "-include", "arkrtos_board_config.h",
                       "-I" + tmp, "-Iboard/qemu_loongarch64",
                       "-Ikernelsrc/include", "-Iconfig", src])
    if rc != 0:
        bad("board-layout", "板级宏展开失败：\n%s" % out[-400:])
        return None
    vals = {}
    for line in out.splitlines():
        m = re.match(r"^ark_num_(\w+)\s+(.+)$", line.strip())
        if not m:
            continue
        expr = m.group(2).strip()
        expr = re.sub(r"(?<=[0-9a-fA-F])[uUlL]+", "", expr)
        if not re.match(r"^[0-9a-fA-FxX+\-*() \t]+$", expr):
            continue
        try:
            vals[m.group(1)] = int(eval(expr, {"__builtins__": {}}, {}))
        except Exception:
            continue
    return vals


# ---------------------------------------------------------------- B 关档零符号

def _compile_demo(cc, on, tag, key_dir):
    obj = os.path.join(key_dir, "elf_demo_%s.o" % tag)
    flags = LA64_ARCH + [
        "-O2", "-Wall", "-ffreestanding", "-fno-common",
        "-nostdlib", "-nostartfiles", "-fno-builtin", "-c", "-o", obj,
        "-DARKRTOS_SIGN_KEY_LOCAL=1",
        "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
        "-include", "arkrtos_board_config.h",
        "-I" + key_dir,
        "-Ikernelsrc/include",
        "-Iconfig",
        "-Iboard/qemu_loongarch64",
        "-Ikernelsrc/port/loongarch/la64",
        "-Iexample/qemu_loongarch64",
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
        "-Ibuild/qemu_loongarch64",
    ]
    if on:
        flags += ["-D" + DYN_FLAG]
    rc, out = run(cc, flags + ["example/qemu_loongarch64/elf_demo.c"])
    return (obj if rc == 0 else None), out


def check_off_no_blob(cc, nm, key_dir):
    markers = ["ark_example_dynmain", "ark_example_ld_ark", "ark_example_dynlib"]
    obj_off, out_off = _compile_demo(cc, False, "off", key_dir)
    if obj_off is None:
        bad("off-no-blob", "关档编 elf_demo.c 失败：\n%s" % out_off[-400:])
        return
    obj_on, out_on = _compile_demo(cc, True, "on", key_dir)
    if obj_on is None:
        bad("off-no-blob", "开档编 elf_demo.c 失败（夹具 blob 没生成？先 make 一次）：\n%s" % out_on[-500:])
        return

    def syms(obj):
        r = subprocess.run([nm, obj], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return read_text_bytes((r.stdout or b"") + (r.stderr or b""))

    off_syms = syms(obj_off)
    on_syms = syms(obj_on)
    leak = [m for m in markers if m in off_syms]
    absent = [m for m in markers if m not in on_syms]
    if leak:
        bad("off-no-blob", "关档 .o 里仍有夹具符号 %s（开关没包住）" % ", ".join(leak))
    elif absent:
        bad("off-no-blob", "开档 .o 里没有夹具符号 %s（开关没接线）" % ", ".join(absent))
    else:
        ok("off-no-blob", "关档无夹具符号 / 开档有三个（两个方向都测了）")


# ---------------------------------------------------------------- C 开档编得过

def check_compile_on(cc, key_dir):
    flags = LA64_ARCH + [
        "-O2", "-Wall", "-ffreestanding", "-fno-common",
        "-nostdlib", "-nostartfiles", "-fno-builtin", "-fsyntax-only",
        "-D" + DYN_FLAG,
        "-DARKRTOS_SIGN_KEY_LOCAL=1",
        "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
        "-include", "arkrtos_board_config.h",
        "-I" + key_dir,
        "-Ikernelsrc/include",
        "-Iconfig",
        "-Iboard/qemu_loongarch64",
        "-Ikernelsrc/port/loongarch/la64",
    ]
    files = [
        "kernelsrc/src/arkrtos_elf.c",
        "kernelsrc/src/arkrtos_linux_abi.c",
    ]
    bads = []
    for rel in files:
        rc, out = run(cc, flags + [rel])
        if rc != 0:
            bads.append("%s：\n%s" % (rel, out[-400:]))
    user = [
        "example/qemu_loongarch64/elf/ldso.c",
        "example/qemu_loongarch64/elf/dynmain.c",
        "example/qemu_loongarch64/elf/libanswer.c",
    ]
    for rel in user:
        rc, out = run(cc, LA64_ARCH + [
            "-O2", "-Wall", "-ffreestanding", "-fno-common",
            "-nostdlib", "-nostartfiles", "-fno-builtin", "-fsyntax-only", rel])
        if rc != 0:
            bads.append("%s：\n%s" % (rel, out[-400:]))
    if bads:
        bad("compile-on", "开档编不过 —— " + bads[0])
    else:
        ok("compile-on", "2 个内核 TU + 3 个用户态 TU 全编过（开档）")


# ---------------------------------------------------------------- D 夹具形状

def _check_one(path, want_type, want_interp, label):
    h = elf_header(path)
    if h is None:
        return ["%s 不是 ELF64（没有这个产物？）" % label]
    problems = []
    if h["e_type"] != want_type:
        problems.append("%s e_type=%d，应为 %d" % (label, h["e_type"], want_type))
    if h["e_phoff"] != 64:
        problems.append("%s e_phoff=%d（ld.so 只认 64：AT_PHDR = base + 64）"
                        % (label, h["e_phoff"]))
    ph = elf_phdrs(h)
    if ph is None:
        return problems + ["%s 程序头读不全" % label]
    if phdr_of(h, PT_LOAD) is None:
        return problems + ["%s 没有 PT_LOAD" % label]

    # AT_PHDR = base + 64 成立的前提：ELF 头 + 整个程序头表都必须落在某个
    # PT_LOAD 里，且该段在 ET_DYN 时从 vaddr 0 / offset 0 开始。
    need = 64 + h["e_phnum"] * h["e_phentsize"]
    base = 0 if want_type == ET_DYN else None
    covered = [p for p in ph if p["type"] == PT_LOAD
               and p["offset"] == 0 and p["filesz"] >= need
               and (base is None or p["vaddr"] == base)]
    if not covered:
        first = next(p for p in ph if p["type"] == PT_LOAD)
        problems.append("%s 没有从文件头开始、覆盖 phdr 表的 PT_LOAD"
                        "（首个 LOAD offset=0x%x vaddr=0x%x filesz=0x%x，需要 >=0x%x）"
                        % (label, first["offset"], first["vaddr"], first["filesz"], need))
    if want_interp is not None:
        got = interp_string(h)
        if got != want_interp:
            problems.append("%s PT_INTERP=%r，应为 %r" % (label, got, want_interp))
    if phdr_of(h, PT_DYNAMIC) is None:
        problems.append("%s 没有 PT_DYNAMIC" % label)
    return problems


def check_fixtures():
    paths = {
        "libanswer.so": (os.path.join(BUILD_DIR, "libanswer.so"), ET_DYN),
        "dynmain": (os.path.join(BUILD_DIR, "dynmain"), ET_DYN),
        "dynmain_exec": (os.path.join(BUILD_DIR, "dynmain_exec"), ET_EXEC),
        "dynmain_bad": (os.path.join(BUILD_DIR, "dynmain_bad"), ET_DYN),
    }
    problems = []
    for label, (path, want) in paths.items():
        if not os.path.isfile(path):
            problems.append("%s 不存在（没有构建产物）" % label)
            continue
        problems += _check_one(path, want, INTERP_PATH, label)
    if problems:
        bad("fixture-shape", "；".join(problems[:4]))
        return

    h_lib = elf_header(paths["libanswer.so"][0])
    if dyn_count(h_lib, DT_NEEDED) not in (0, None):
        bad("fixture-shape", "libanswer.so 自己的 DT_NEEDED 应为 0 条")
        return
    h_main = elf_header(paths["dynmain"][0])
    if dyn_count(h_main, DT_NEEDED) != 1:
        bad("fixture-shape", "dynmain 的 DT_NEEDED 应为 1 条（libanswer.so）")
        return
    h_bad = elf_header(paths["dynmain_bad"][0])
    ents = dyn_entries(h_bad)
    strtab_v = dict(ents).get(DT_STRTAB)
    if strtab_v is None:
        bad("fixture-shape", "dynmain_bad 没有 DT_STRTAB")
        return
    off = vaddr_to_off(h_bad, strtab_v)
    if off is None:
        bad("fixture-shape", "dynmain_bad 的 DT_STRTAB 不在任何 PT_LOAD 里（写坏夹具了）")
        return
    if b"no_such_symbol" not in h_bad["raw"][off:off + 512]:
        bad("fixture-shape", "dynmain_bad 的 .dynstr 里没有 no_such_symbol（反假绿夹具没造对）")
        return
    if dyn_count(h_bad, DT_JMPREL) != 1:
        bad("fixture-shape", "dynmain_bad 应有 1 张 JMPREL 表")
        return
    # ld.so 自己就是链接产物（不经过 dynfixture：它不需要 PT_INTERP/PT_DYNAMIC），
    # blob 也是直接从它取的，所以这里查的就是被写进 ramfs 的那一份。
    ldso_path = os.path.join(BUILD_DIR, "ld-ark.link.elf")
    if not os.path.isfile(ldso_path):
        bad("fixture-shape", "ld-ark.link.elf 不存在（ld.so 没构建）")
        return
    h_ld = elf_header(ldso_path)
    if h_ld is None or h_ld["e_type"] != ET_EXEC:
        bad("fixture-shape", "ld.so 应为 ET_EXEC（bias 固定 0，不需要自举重定位）")
        return
    first_ld = next(p for p in elf_phdrs(h_ld) if p["type"] == PT_LOAD)
    if first_ld["vaddr"] != INTERP_BASE:
        bad("fixture-shape", "ld.so 首个 PT_LOAD vaddr=0x%x，应等于解释器基址 0x%x"
            % (first_ld["vaddr"], INTERP_BASE))
        return
    if not (INTERP_BASE <= h_ld["e_entry"] < INTERP_BASE + first_ld["filesz"]):
        bad("fixture-shape", "ld.so 入口 0x%x 不在解释器第一个 PT_LOAD 里" % h_ld["e_entry"])
        return
    ok("fixture-shape", "4 个夹具 + ld.so 的头/phdr/PT_INTERP/DT_NEEDED/坏符号全对")

    problems = []
    tls_want = (("dynmain", 16), ("dynmain_exec", 16), ("libanswer.so", 8))
    for label, want in tls_want:
        h = elf_header(paths[label][0])
        seg = phdr_of(h, PT_TLS)
        if seg is None:
            problems.append("%s 没有 PT_TLS（TLS 模板不会被装载器看到）" % label)
            continue
        if seg["filesz"] != want or seg["memsz"] != want:
            problems.append("%s PT_TLS filesz=0x%x memsz=0x%x，应为 0x%x"
                            % (label, seg["filesz"], seg["memsz"], want))
        rel = rela_list(h)
        if rel is None:
            problems.append("%s 的 .rela 读不出来" % label)
            continue
        n11 = sum(1 for _o, t, _s, _a in rel if t == 11)
        if n11 != 1:
            problems.append("%s .rela 里 R_LARCH_TLS_TPREL64 有 %d 条，应为 1" % (label, n11))
    # .dynsym 的 STT_TLS 值：主程序那条必须非 0（否则"槽没重定位"也能碰巧读对）
    for label, sym, want_val in (("dynmain", "m_tls", 8),
                                 ("libanswer.so", "lib_tls", 0)):
        h = elf_header(paths[label][0])
        syms = dyn_symbols(h)
        if syms is None:
            problems.append("%s 的 .dynsym 读不出来" % label)
            continue
        hit = [s for s in syms if s["name"] == sym]
        if not hit:
            problems.append("%s 的 .dynsym 里没有 %s" % (label, sym))
            continue
        s = hit[0]
        if (s["info"] & 0xF) != STT_TLS:
            problems.append("%s 的 %s 不是 STT_TLS（info=0x%02x）" % (label, sym, s["info"]))
        if s["value"] != want_val:
            problems.append("%s 的 %s st_value=0x%x，应为 0x%x"
                            % (label, sym, s["value"], want_val))
    if problems:
        bad("fixture-tls", "；".join(problems[:4]))
        return
    ok("fixture-tls", "三个夹具的 PT_TLS 尺寸、type 11 条数、.dynsym 的 STT_TLS 值全对")

    # 重建确定性：同一 manifest 再跑一次，逐字节比。
    tmp = tempfile.mkdtemp(prefix="ark_dyn_rebuild_")
    again = os.path.join(tmp, "dynmain")
    link = os.path.join(BUILD_DIR, "dynmain.link.elf")
    man = os.path.join(ROOT, "example/qemu_loongarch64/elf/dyn/dynmain.json")
    if not all(os.path.isfile(p) for p in (link, man)):
        bad("fixture-rebuild", "缺 dynmain.link.elf 或 dynmain.json，无法复建")
        return
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools/elf/dynfixture.py"),
                        "--input", link, "--manifest", man, "--output", again],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        bad("fixture-rebuild", "重建 dynfixture 失败：\n%s"
            % read_text_bytes(r.stdout)[-400:])
        return
    a = open(again, "rb").read()
    b = open(os.path.join(BUILD_DIR, "dynmain"), "rb").read()
    if a != b:
        bad("fixture-rebuild", "同一 manifest 两次产物不同（%d vs %d 字节）" % (len(a), len(b)))
    else:
        ok("fixture-rebuild", "重建 dynmain 与构建产物逐字节相同（%d 字节）" % len(a))


# ---------------------------------------------------------------- E 真读数

def restore_default_build():
    """把构建目录恢复成默认档。

    本门禁为了读动态链接的现场证据会 clean + 带 -DARKRTOS_USE_DYNLOAD=1 重编；
    如果就这么收尾，后面任何一次 `make` 都会复用这些 .o（make 不跟踪
    EXTRA_CFLAGS），把默认档悄悄变成开档——下一个人看到的是"默认档也有
    [dyn] 读数"这种假绿。所以收尾必须重编一次默认档。
    """
    log = os.path.join(tempfile.gettempdir(), "ark_dynlink_restore.log")
    with open(log, "wb") as f:
        r = subprocess.run(["make", "-f", MAKEFILE, "clean"], cwd=ROOT,
                           stdout=f, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            return False, read_text(log)[-600:]
        r = subprocess.run(["make", "-f", MAKEFILE, "-j8"], cwd=ROOT,
                           stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        return False, read_text(log)[-600:]
    if not os.path.isfile(os.path.join(BUILD_DIR, ELF_NAME)):
        return False, "默认档重编后没有 %s" % ELF_NAME
    return True, ""


def build_image():
    log = os.path.join(tempfile.gettempdir(), "ark_dynlink_build.log")
    env = dict(os.environ)
    env["EXTRA_CFLAGS"] = "-D" + DYN_FLAG
    with open(log, "wb") as f:
        # 先 clean：make 不跟踪 EXTRA_CFLAGS，上一档留下的 .o 会让这一次只做链接，
        # 于是镜像里根本没有动态演示——那是本门禁最想抓的假绿，不能自己制造。
        r = subprocess.run(["make", "-f", MAKEFILE, "clean"], cwd=ROOT, env=env,
                           stdout=f, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            return False, read_text(log)[-1200:]
        r = subprocess.run(["make", "-f", MAKEFILE, "-j8"], cwd=ROOT, env=env,
                           stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        return False, read_text(log)[-1200:]
    return True, ""


def run_qemu(qemu, seconds):
    """串口走管道而不是 -serial file:：file 后端块缓冲，进程被 kill 时最后
    一个不满 4 KB 的块会丢，读数会变成抽签。"""
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

    deadline = time.time() + seconds
    while time.time() < deadline:
        joined = b"".join(chunks)
        if joined.count(b"[dyn] finished after") >= 3:
            break
        time.sleep(0.05)
    # 收尾行刚出现时可能只写了一半。
    time.sleep(0.6)
    p.kill()
    p.wait()
    th.join(timeout=5)
    data = b"".join(chunks)
    if not data:
        return None
    return read_text_bytes(data)


def judge(text):
    notes = []
    lines = text.splitlines()

    def count(pred):
        return sum(1 for ln in lines if pred(ln))

    writes = count(lambda s: s.startswith("[dyn] write /mnt/ram/") and " rc=0 size=" in s)
    passes = count(lambda s: s.startswith(
        "[dyn] PASS: RELATIVE + R_LARCH_64 + JUMP_SLOT + TLS all applied"))
    e105 = count(lambda s: re.search(r"^\[dyn\] finished after \d+ ms, exit code 105$", s) is not None)
    e127 = count(lambda s: re.search(r"^\[dyn\] finished after \d+ ms, exit code 127$", s) is not None)
    ldso_fail = count(lambda s: s.startswith("[ldso] FAIL: unresolved PLT symbol no_such_symbol"))
    ldso_other_fail = count(lambda s: s.startswith("[ldso] FAIL:") and
                            "unresolved PLT symbol no_such_symbol" not in s and
                            "relocation failed" not in s)
    staged_fail = count(lambda s: s.startswith("[dyn] FAIL:"))
    interp_ok = count(lambda s: "interp_base=34603008" in s and s.startswith("[dyn] info"))
    execs = count(lambda s: re.match(r"^\[dyn\] exec \S+ rc=0$", s) is not None)
    tls_base = count(lambda s: s.startswith("[ldso] tls base="))
    # 库在三次装载里都会被重定位（dynmain / dynmain_exec / 坏夹具），主程序两次。
    tprel_lib = count(lambda s: s.startswith("[ldso] reloc libanswer.so:")
                      and s.endswith("TPREL64=1"))
    tprel_main = count(lambda s: s.startswith("[ldso] reloc dynmain")
                       and s.endswith("TPREL64=1"))
    tls_off8 = count(lambda s: s.startswith("[dyn] TPREL64 m_tls offset -> 8 ok"))
    tls_lib = count(lambda s: s.startswith("[dyn] TLS lib value -> 4242 ok"))
    tls_libw = count(lambda s: s.startswith("[dyn] TLS lib after write -> 4243 ok"))

    problems = []
    if writes != 5:
        problems.append("write rc=0 的条数 %d != 5（ld.so + 库 + 两个主程序 + 坏夹具）" % writes)
    if execs != 3:
        problems.append("exec rc=0 的条数 %d != 3" % execs)
    if passes != 2:
        problems.append("[dyn] PASS 条数 %d != 2（多出来多半是坏夹具被放过了）" % passes)
    if e105 != 2:
        problems.append("exit code 105 条数 %d != 2" % e105)
    if e127 != 1:
        problems.append("exit code 127 条数 %d != 1（坏夹具必须明确失败）" % e127)
    if ldso_fail != 1:
        problems.append("缺 `[ldso] FAIL: unresolved PLT symbol no_such_symbol`")
    if ldso_other_fail != 0:
        problems.append("ld.so 出现了期望之外的失败行")
    if staged_fail != 0:
        problems.append("夹具没送进 /mnt/ram/（[dyn] FAIL 出现了）")
    if tls_base != 3:
        problems.append("[ldso] tls base= 条数 %d != 3（两个主程序 + 坏夹具）" % tls_base)
    if tprel_lib != 3:
        problems.append("`reloc libanswer.so ... TPREL64=1` 条数 %d != 3" % tprel_lib)
    if tprel_main != 2:
        problems.append("`reloc dynmain* ... TPREL64=1` 条数 %d != 2" % tprel_main)
    if tls_off8 != 2:
        problems.append("`TPREL64 m_tls offset -> 8 ok` 条数 %d != 2（TLS 可能没真做）" % tls_off8)
    if tls_lib != 2:
        problems.append("`TLS lib value -> 4242 ok` 条数 %d != 2（跨对象 TLS 没读对）" % tls_lib)
    if tls_libw != 2:
        problems.append("`TLS lib after write -> 4243 ok` 条数 %d != 2" % tls_libw)
    if interp_ok != 3:
        problems.append("`[dyn] info ... interp_base=34603008`（0x02100000）出现 %d 次 != 3" % interp_ok)
    for tag, name in (("[linux] PASS", "Linux ABI 演示"),
                      ("[FS] PASS", "ramfs 自测"),
                      ("[ELF] tamper check PASS", "签名反例自检")):
        if count(lambda s, t=tag: s.startswith(t)) != 1:
            problems.append("同一次启动里丢掉了 %s 的读数" % name)
    if problems:
        return False, problems
    return True, ["两条 PASS + exit 105 与坏夹具 exit 127 全对，旁路演示未受影响"]


def self_check(good_text):
    good = good_text.replace("\r\n", "\n")
    mutations = {
        "删一条 PASS": lambda s: s.replace(
            "[dyn] PASS: RELATIVE + R_LARCH_64 + JUMP_SLOT + TLS all applied\n", "", 1),
        "坏夹具改判成功": lambda s: s.replace("exit code 127", "exit code 105"),
        "删掉 ldso 的 FAIL": lambda s: s.replace(
            "[ldso] FAIL: unresolved PLT symbol no_such_symbol\n", ""),
        "删一条 write": lambda s: re.sub(r"^\[dyn\] write /mnt/ram/libanswer\.so[^\n]*\n", "", s, count=1, flags=re.M),
        "删一条 TLS 检查": lambda s: re.sub(r"^\[dyn\] TLS lib value -> 4242 ok\n", "", s, count=1, flags=re.M),
        "抹掉 TPREL64 计数": lambda s: s.replace("TPREL64=1", "TPREL64=0"),
        "删一行 tls base": lambda s: re.sub(r"^\[ldso\] tls base=[^\n]*\n", "", s, count=1, flags=re.M),
    }
    missed = []
    for name, fn in mutations.items():
        mutated = fn(good)
        if mutated == good:
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
    ap.add_argument("--qemu", default="")
    ap.add_argument("--no-build", action="store_true", help="复用已有产物")
    ap.add_argument("--seconds", type=int, default=45)
    ap.add_argument("--keep", action="store_true", help="打印串口日志尾部")
    ap.add_argument("--keep-build", action="store_true",
                    help="收尾不恢复默认档（调试用；下次 make 会复用开档 .o）")
    a = ap.parse_args(argv)

    la64 = _pick(LA64_FALLBACKS, a.la64_gcc, "LoongArch64 交叉编译器")
    qemu = _pick(QEMU_CANDIDATES, a.qemu, "qemu-system-loongarch64")
    nm = nm_of(la64) if la64 else None
    if la64 and nm is None:
        bad("toolchain", "找不到与 %s 配套的 nm（符号检查不能跳过）" % os.path.basename(la64))

    key_dir = tempfile.mkdtemp(prefix="ark_dyn_key_")
    with open(os.path.join(key_dir, "arkrtos_sign_key_local.h"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write("#ifndef ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_IMAGE_SIGN_KEY { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, "
                "0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, "
                "0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F }\n"
                "#endif\n")

    check_default_off()
    if la64:
        check_compile_on(la64, key_dir)

    if qemu:
        if not a.no_build:
            built, why = build_image()
            if not built:
                bad("build", "make 失败：\n%s" % why)
            else:
                ok("build", "make -f %s EXTRA_CFLAGS=-D%s 通过" % (MAKEFILE, DYN_FLAG))
        # 关档符号检查要拿开档编出来的 blob 做对照，所以在构建之后跑。
        if la64 and nm:
            check_off_no_blob(la64, nm, key_dir)
        check_fixtures()
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
                        bad("parser-selfcheck", "这些坏样本没被判红：%s" % ", ".join(missed))
                    else:
                        ok("parser-selfcheck", "7 种坏法（少 PASS / 坏夹具当成功 / 少 FAIL 行 / 少 write / 少 TLS 检查 / TPREL64 计数被抹 / 少 tls base 行）全被判红")
                else:
                    bad("qemu-readout", "；".join(notes))
                    bad("parser-selfcheck", "读数没过，自检无从谈起")
                if a.keep:
                    tail = "\n".join(text.splitlines()[-45:])
                    print("\n---- 串口末尾 ----\n%s\n------------------" % tail)
        else:
            bad("qemu-run", "找不到 %s（先编一次，或去掉 --no-build）" % ELF_NAME)
        if not a.no_build and not a.keep_build:
            good, why = restore_default_build()
            if not good:
                bad("restore-default", "恢复默认档失败：\n%s" % why)
            else:
                ok("restore-default", "构建目录已恢复默认档（避免下一次 make 复用开档 .o）")

    print("\n%d passed, %d failed" % (len(PASSES), len(FAILS)))
    if not FAILS:
        print("DYNLINK-GATE OK")
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))