#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把一个真实 ELF 要什么，压成一张 Ark RTOS 能对账的清单。

为什么需要这一道
----------------
「让发行版的二进制跑起来」这句话里没有信息：alpine 的 musl 静态 hello 与
debian 的 glibc 动态 hello 之间差着一个数量级的工程。差别不在体积，在于
二进制**向内核要什么**：落在哪个地址、有没有 PT_TLS、动态段里有没有
GNU_HASH / 符号版本 / IFUNC、重定位里有没有 TLS 与 IRELATIVE。

这些东西 readelf 都能打，但 readelf 不会告诉你哪几条 Ark 现在做不到。
本脚本做两件事：

  1. 把 ELF 的结构性需求完整读出来（不依赖 binutils，纯 struct 解析，
     所以门禁在任何机器上都能跑）；
  2. 拿 Ark 当前的装载器能力去对账，输出**按阻塞顺序排列**的 blockers。

对账用的能力表（ARK_SUPPORTED_*）是刻意写死的常量，不 import 仓库代码：
它是「我们声称支持的东西」。tools/realbin_gate.py 会把这张表与
example/qemu_loongarch64/elf/ldso.c 里真正 switch 到的标签对齐，防止
文档与实现各说各话。

结论怎么读
----------
退出码 0 = 这个二进制只用 Ark 已经支持的东西，且落在当前窗口里；
退出码 1 = 有 blocker，blockers 列表就是「要做什么」，顺序即依赖顺序。
**退出码 1 是探针的正常结果**，不是脚本出错。

用法
----
    py -3 tools/elf/realbin_probe.py <elf> [<elf> ...]
    py -3 tools/elf/realbin_probe.py <elf> --sysroot <dir> --json
"""

import argparse
import json
import os
import struct
import sys

# ---------------------------------------------------------------- 常量

PT_NULL, PT_LOAD, PT_DYNAMIC, PT_INTERP = 0, 1, 2, 3
PT_NOTE, PT_PHDR, PT_TLS = 4, 6, 7
PT_GNU_EH_FRAME, PT_GNU_STACK, PT_GNU_RELRO = 0x6474E550, 0x6474E551, 0x6474E552

DT_NULL, DT_NEEDED, DT_PLTRELSZ, DT_PLTREL = 0, 1, 2, 20
DT_PLTGOT, DT_DEBUG, DT_TEXTREL, DT_BIND_NOW = 3, 21, 22, 24
DT_RELR = 36
DT_JMPREL = 23
DT_HASH, DT_STRTAB, DT_SYMTAB, DT_RELA = 4, 5, 6, 7
DT_RELASZ, DT_RELAENT, DT_STRSZ, DT_SYMENT = 8, 9, 10, 11
DT_INIT, DT_FINI, DT_SONAME, DT_RPATH = 12, 13, 14, 15
DT_INIT_ARRAY, DT_FINI_ARRAY = 25, 26
DT_INIT_ARRAYSZ, DT_FINI_ARRAYSZ = 27, 28
DT_RUNPATH, DT_FLAGS = 29, 30
DT_GNU_HASH, DT_VERSYM = 0x6FFFFEF5, 0x6FFFFFF0
DT_RELACOUNT, DT_FLAGS_1 = 0x6FFFFFF9, 0x6FFFFFFB
DT_VERDEF, DT_VERDEFNUM = 0x6FFFFFFC, 0x6FFFFFFD
DT_VERNEED, DT_VERNEEDNUM = 0x6FFFFFFE, 0x6FFFFFFF

DT_NAMES = {
    DT_NEEDED: "NEEDED", DT_PLTRELSZ: "PLTRELSZ", DT_PLTREL: "PLTREL",
    DT_HASH: "HASH", DT_STRTAB: "STRTAB", DT_SYMTAB: "SYMTAB",
    DT_RELA: "RELA", DT_RELASZ: "RELASZ", DT_RELAENT: "RELAENT",
    DT_STRSZ: "STRSZ", DT_SYMENT: "SYMENT", DT_INIT: "INIT",
    DT_FINI: "FINI", DT_SONAME: "SONAME", DT_RPATH: "RPATH",
    DT_INIT_ARRAY: "INIT_ARRAY", DT_FINI_ARRAY: "FINI_ARRAY",
    DT_INIT_ARRAYSZ: "INIT_ARRAYSZ", DT_FINI_ARRAYSZ: "FINI_ARRAYSZ",
    DT_RUNPATH: "RUNPATH", DT_FLAGS: "FLAGS", DT_GNU_HASH: "GNU_HASH",
    DT_VERSYM: "VERSYM", DT_RELACOUNT: "RELACOUNT", DT_FLAGS_1: "FLAGS_1",
    DT_VERDEF: "VERDEF", DT_VERDEFNUM: "VERDEFNUM",
    DT_VERNEED: "VERNEED", DT_VERNEEDNUM: "VERNEEDNUM",
    DT_PLTGOT: "PLTGOT", DT_PLTREL: "PLTREL", DT_DEBUG: "DEBUG",
    DT_TEXTREL: "TEXTREL", DT_BIND_NOW: "BIND_NOW", DT_RELR: "RELR",
}

# LoongArch64 重定位类型（ABI 里的前 8 位）
R_LA_NONE, R_LA_64, R_LA_RELATIVE = 0, 2, 3
R_LA_JUMP_SLOT = 5
R_LA_IRELATIVE = 12
R_LA_TLS_TPREL64 = 11

# 编号来自 LoongArch ELF psABI（TLS 占 6..11，IRELATIVE=12）。
# tools/realbin_gate.py 会拿 binutils readelf 的名字与本表逐条对账，
# 所以这张表错了会被门禁抓住，而不是把错名字印给读者。
R_NAMES = {
    R_LA_NONE: "R_LARCH_NONE", 1: "R_LARCH_32", R_LA_64: "R_LARCH_64",
    R_LA_RELATIVE: "R_LARCH_RELATIVE", 4: "R_LARCH_COPY",
    R_LA_JUMP_SLOT: "R_LARCH_JUMP_SLOT",
    6: "R_LARCH_TLS_DTPMOD32", 7: "R_LARCH_TLS_DTPMOD64",
    8: "R_LARCH_TLS_DTPREL32", 9: "R_LARCH_TLS_DTPREL64",
    10: "R_LARCH_TLS_TPREL32", 11: "R_LARCH_TLS_TPREL64",
    R_LA_IRELATIVE: "R_LARCH_IRELATIVE",
    20: "R_LARCH_MARK_LA", 21: "R_LARCH_MARK_PCREL",
    22: "R_LARCH_SOP_PUSH_PCREL", 23: "R_LARCH_SOP_PUSH_ABSOLUTE",
    24: "R_LARCH_SOP_PUSH_DUP", 25: "R_LARCH_SOP_PUSH_GPREL",
    26: "R_LARCH_SOP_PUSH_TLS_TPREL", 27: "R_LARCH_SOP_PUSH_TLS_GOT",
    28: "R_LARCH_SOP_PUSH_TLS_GD", 29: "R_LARCH_SOP_PUSH_PLT_PCREL",
    30: "R_LARCH_SOP_ASSERT", 31: "R_LARCH_SOP_NOT", 32: "R_LARCH_SOP_SUB",
    33: "R_LARCH_SOP_SL", 34: "R_LARCH_SOP_SR", 35: "R_LARCH_SOP_ADD",
    36: "R_LARCH_SOP_AND", 37: "R_LARCH_SOP_IF_ELSE", 38: "R_LARCH_SOP_POP_32_S_10_5",
    39: "R_LARCH_SOP_POP_32_U_10_12", 40: "R_LARCH_SOP_POP_32_S_10_12",
    41: "R_LARCH_SOP_POP_32_S_10_16", 42: "R_LARCH_SOP_POP_32_S_10_16_S2",
    43: "R_LARCH_SOP_POP_32_S_5_20", 44: "R_LARCH_SOP_POP_32_S_0_5_10_16_S2",
    45: "R_LARCH_SOP_POP_32_S_0_10_10_16_S2", 46: "R_LARCH_SOP_POP_32_U",
    47: "R_LARCH_ADD8", 48: "R_LARCH_ADD16", 49: "R_LARCH_ADD24",
    50: "R_LARCH_ADD32", 51: "R_LARCH_ADD64", 52: "R_LARCH_SUB8",
    53: "R_LARCH_SUB16", 54: "R_LARCH_SUB24", 55: "R_LARCH_SUB32",
    56: "R_LARCH_SUB64", 57: "R_LARCH_GNU_VTINHERIT", 58: "R_LARCH_GNU_VTENTRY",
    59: "R_LARCH_B16", 60: "R_LARCH_B21", 61: "R_LARCH_B26",
    62: "R_LARCH_ABS_HI20", 63: "R_LARCH_ABS_LO12", 64: "R_LARCH_ABS64_LO20",
    65: "R_LARCH_ABS64_HI12", 66: "R_LARCH_PCALA_HI20", 67: "R_LARCH_PCALA_LO12",
    68: "R_LARCH_PCALA64_LO20", 69: "R_LARCH_PCALA64_HI12",
    70: "R_LARCH_GOT_PC_HI20", 71: "R_LARCH_GOT_PC_LO12",
    72: "R_LARCH_GOT64_PC_LO20", 73: "R_LARCH_GOT64_PC_HI12",
    74: "R_LARCH_GOT_HI20", 75: "R_LARCH_GOT_LO12",
    76: "R_LARCH_GOT64_LO20", 77: "R_LARCH_GOT64_HI12",
    78: "R_LARCH_TLS_LE_HI20", 79: "R_LARCH_TLS_LE_LO12",
    80: "R_LARCH_TLS_LE64_LO20", 81: "R_LARCH_TLS_LE64_HI12",
    82: "R_LARCH_TLS_IE_PC_HI20", 83: "R_LARCH_TLS_IE_PC_LO12",
    84: "R_LARCH_TLS_IE64_PC_LO20", 85: "R_LARCH_TLS_IE64_PC_HI12",
    86: "R_LARCH_TLS_IE_HI20", 87: "R_LARCH_TLS_IE_LO12",
    88: "R_LARCH_TLS_IE64_LO20", 89: "R_LARCH_TLS_IE64_HI12",
    90: "R_LARCH_TLS_LD_PC_HI20", 91: "R_LARCH_TLS_LD_HI20",
    92: "R_LARCH_TLS_GD_PC_HI20", 93: "R_LARCH_TLS_GD_HI20",
    94: "R_LARCH_32_PCREL", 95: "R_LARCH_RELAX",
}

# ---------------------------------------------------------------- Ark 现状
#
# 这三张表就是「我们声称支持什么」。改它们必须同时改实现与门禁，
# tools/realbin_gate.py 会把它们和 ldso.c / elf.c 对账。
#
ARK_SUPPORTED_RELOC = {R_LA_RELATIVE, R_LA_64, R_LA_JUMP_SLOT,
                       R_LA_TLS_TPREL64}
# 静态 PT_TLS：内核装载器自 2026-11-20 起读 PT_TLS 建 tdata/tbss 块并把
# TP 指向块起点（fcc7ddf 真实 glibc 静态二进制 __thread 实跑），所以带
# PT_TLS 的二进制不再算「还差能力」。真实发行版的 glibc 会在静态档自建
# TCB（内核给 tls_placed=0），那一条不影响这里的判定。
ARK_SUPPORTS_PT_TLS = True
ARK_SUPPORTED_DT = {
    DT_NULL, DT_NEEDED, DT_PLTRELSZ, DT_HASH, DT_STRTAB, DT_SYMTAB,
    DT_RELA, DT_RELASZ, DT_RELAENT, DT_STRSZ, DT_SYMENT, DT_JMPREL,
}

# 只描述、不生成工作的标签：看到它们不需要补任何代码。
#   PLTGOT   GOT 基址，只对 lazy binding 有用（Ark 不做 lazy binding）
#   PLTREL   PLT 重定位是 REL 还是 RELA（值在 pltrel_is_rela 里单独判）
#   DEBUG    ld.so 自己回写调试信息用的槽位
#   BIND_NOW / TEXTREL 是策略标记，真正的约束由 FLAGS / 重定位表体现
# 放进这里会让 dynamic-tags 的 blocker 只剩「真的要做」的那几条。
ARK_IGNORED_DT = {
    DT_PLTGOT, DT_PLTREL, DT_DEBUG, DT_BIND_NOW, DT_TEXTREL,
}

ARK_WINDOW_BASE = 0x02000000
ARK_WINDOW_SIZE = 2 * 1024 * 1024
ARK_WINDOW_END = ARK_WINDOW_BASE + ARK_WINDOW_SIZE
ARK_INTERP_BASE = 0x02100000
ARK_INTERP_SIZE = 64 * 1024


# ---------------------------------------------------------------- 解析

class Elf(object):
    def __init__(self, path):
        self.path = path
        self.size = 0
        self.data = b""
        self.ok = False
        self.err = ""
        self._load()

    def _load(self):
        try:
            with open(self.path, "rb") as f:
                self.data = f.read()
        except OSError as e:
            self.err = "cannot read: %s" % e
            return
        self.size = len(self.data)
        d = self.data
        if len(d) < 64 or d[:4] != b"\x7fELF":
            self.err = "not an ELF file"
            return
        if d[4] != 2 or d[5] != 1:
            self.err = "not ELF64 little-endian (class=%d data=%d)" % (d[4], d[5])
            return
        (self.e_type, self.e_machine, _ver, self.e_entry, self.e_phoff,
         self.e_shoff, _flags, _ehsize, self.e_phentsize, self.e_phnum) = \
            struct.unpack_from("<HHIQQQIHHH", d, 16)
        if self.e_phentsize != 56:
            self.err = "e_phentsize=%d, expected 56" % self.e_phentsize
            return
        self.phdrs = []
        for i in range(self.e_phnum):
            off = self.e_phoff + i * 56
            if off + 56 > len(d):
                self.err = "program header %d out of file" % i
                return
            (p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz,
             p_memsz, p_align) = struct.unpack_from("<IIQQQQQQ", d, off)
            self.phdrs.append(dict(type=p_type, flags=p_flags, off=p_offset,
                                   vaddr=p_vaddr, filesz=p_filesz,
                                   memsz=p_memsz, align=p_align))
        self.ok = True

    def loads(self):
        return [p for p in self.phdrs if p["type"] == PT_LOAD]

    def first(self, p_type):
        for p in self.phdrs:
            if p["type"] == p_type:
                return p
        return None

    def vaddr_to_off(self, va):
        for p in self.loads():
            lo = p["vaddr"]
            if lo <= va < lo + p["filesz"]:
                return p["off"] + (va - lo)
        return None

    def cstr(self, off):
        if off is None or off >= len(self.data):
            return ""
        end = self.data.find(b"\x00", off)
        if end < 0:
            end = len(self.data)
        return self.data[off:end].decode("utf-8", "replace")

    def dynamic(self):
        """返回 [(tag, val)]，按文件顺序。"""
        dyn = self.first(PT_DYNAMIC)
        if dyn is None:
            return []
        out = []
        off = dyn["off"]
        for _ in range(dyn["filesz"] // 16):
            if off + 16 > len(self.data):
                break
            tag, val = struct.unpack_from("<qQ", self.data, off)
            out.append((tag, val))
            off += 16
            if tag == DT_NULL:
                break
        return out


def collect(e):
    """把一份 ELF 的结构性需求读成 dict。"""
    if not e.ok:
        # 没读成 ELF 时不能碰任何头部字段：坏输入必须得到一条原因，不是异常。
        return dict(path=e.path, size=e.size, ok=False, err=e.err)
    r = dict(path=e.path, size=e.size, elftype=e.e_type,
             machine=e.e_machine, entry=e.e_entry, ok=e.ok, err=e.err)

    loads = e.loads()
    lo = min([p["vaddr"] for p in loads]) if loads else 0
    hi = max([p["vaddr"] + p["memsz"] for p in loads]) if loads else 0
    r["load_lo"] = lo
    r["load_hi"] = hi
    r["span"] = hi - lo
    r["max_align"] = max([p["align"] for p in loads]) if loads else 0
    r["n_load"] = len(loads)
    r["writable_exec"] = any((p["flags"] & 0x3) == 0x3 for p in loads)

    interp = e.first(PT_INTERP)
    r["interp"] = e.cstr(e.vaddr_to_off(interp["vaddr"])) if interp else ""
    r["has_tls"] = e.first(PT_TLS) is not None
    if r["has_tls"]:
        t = e.first(PT_TLS)
        r["tls"] = dict(filesz=t["filesz"], memsz=t["memsz"], align=t["align"])
    r["has_relro"] = e.first(PT_GNU_RELRO) is not None
    st = e.first(PT_GNU_STACK)
    r["stack_exec"] = bool(st and (st["flags"] & 0x1))

    dyn = e.dynamic()
    has = {}
    for tag, val in dyn:
        has[tag] = has.get(tag, 0) + 1
    needed = []
    strtab = None
    for tag, val in dyn:
        if tag == DT_STRTAB:
            strtab = val
    for tag, val in dyn:
        if tag == DT_NEEDED and strtab is not None:
            needed.append(e.cstr(e.vaddr_to_off(strtab + val)))
    r["needed"] = [x for x in needed if x]
    r["dyn_tags"] = sorted(t for t in has if t != DT_NULL)
    r["pltrel_is_rela"] = None
    r["relaent"] = None
    for tag, val in dyn:
        if tag == DT_PLTREL:
            r["pltrel_is_rela"] = (val == DT_RELA)
        elif tag == DT_RELAENT:
            r["relaent"] = val
    r["has_gnu_hash"] = DT_GNU_HASH in has
    r["has_sysv_hash"] = DT_HASH in has
    r["has_versym"] = DT_VERSYM in has
    r["has_verneed"] = DT_VERNEED in has
    r["has_init_array"] = DT_INIT_ARRAY in has
    r["has_relr"] = 36 in has            # DT_RELR
    # FLAGS 里 BIND_NOW 位 = 0x8；FLAGS_1 里 NOW = 0x1
    r["bind_now"] = False
    for tag, val in dyn:
        if tag == DT_FLAGS and (val & 0x8):
            r["bind_now"] = True
        if tag == DT_FLAGS_1 and (val & 0x1):
            r["bind_now"] = True

    # 重定位直方图
    #
    # 坑：glibc 的 libc.so.6 里 DT_RELASZ 把 .rela.plt 也包进去了
    # （RELASZ 0x7ed8 = .rela.dyn 0x7c20 + .rela.plt 0x2b8），于是
    # DT_JMPREL 那一段在两次扫描里会被数两遍。按文件偏移去重，这样
    # tools/realbin_gate.py 才能与 readelf 的计数逐项对齐。
    hist = {}
    seen_off = set()
    for rtag, rsz in ((DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ)):
        addr = None
        size = 0
        for tag, val in dyn:
            if tag == rtag:
                addr = val
            elif tag == rsz:
                size = val
        if addr is None:
            continue
        off = e.vaddr_to_off(addr)
        if off is None:
            continue
        for i in range(size // 24):
            p = off + i * 24
            if p + 24 > len(e.data):
                break
            if p in seen_off:
                continue
            seen_off.add(p)
            _roff, rinfo, _add = struct.unpack_from("<QQq", e.data, p)
            t = rinfo & 0xFF
            hist[t] = hist.get(t, 0) + 1
    r["reloc_hist"] = dict((R_NAMES.get(t, "R_LARCH_%d" % t), n)
                           for t, n in sorted(hist.items()))

    # 符号版本需求（VERNEED 是一条链）
    verneed = []
    for tag, val in dyn:
        if tag != DT_VERNEED:
            continue
        off = e.vaddr_to_off(val)
        if off is None:
            break
        p = off
        seen = 0
        while p + 16 <= len(e.data) and seen < 32:
            vn_version, vn_cnt, vn_file, vn_aux, vn_next = \
                struct.unpack_from("<HHIII", e.data, p)
            fname = e.cstr(e.vaddr_to_off(
                [v for t, v in dyn if t == DT_STRTAB][0] + vn_file))
            a = p + vn_aux
            for _ in range(vn_cnt):
                if a + 16 > len(e.data):
                    break
                _h, _fl, _oth, vna_name, vna_next = \
                    struct.unpack_from("<IHHII", e.data, a)
                sname = e.cstr(e.vaddr_to_off(
                    [v for t, v in dyn if t == DT_STRTAB][0] + vna_name))
                verneed.append("%s:%s" % (fname or "?", sname))
                if vna_next == 0:
                    break
                a += vna_next
            seen += 1
            if vn_next == 0:
                break
            p += vn_next
    r["verneed"] = verneed

    return r


SYSROOT_SUBDIRS = ("usr/lib64", "lib64", "usr/lib", "lib",
                   "usr/lib64/lp64d", "lib64/lp64d", "usr/lib/loongarch64-linux-gnu")

def find_in_sysroot(sysroot, name):
    """在 sysroot 里按常见的库目录找 name，找不到回传空串。"""
    if os.path.isabs(name):
        cands = [name]
    else:
        cands = [os.path.join(sysroot, name)]
        cands += [os.path.join(sysroot, sub, name) for sub in SYSROOT_SUBDIRS]
    for c in cands:
        if os.path.isfile(c):
            return c
    return ""

def resolve_deps(r, sysroot, depth=3, seen=None):
    """把 DT_NEEDED 逐个去 sysroot 里找，找到的就再读一层。

    只报「找到没有」与「它自己要什么」，不把依赖树的每一层都展开成完整报告：
    libc.so.6 的 DT_NEEDED 就是 ld.so.1，而 glibc 的 ld.so 与 libc 是成对的
    内部 ABI——这条链本身就是结论，不是待办。
    """
    if seen is None:
        seen = set()
    deps = {}
    for name in r["needed"]:
        if name in seen:
            deps[name] = dict(found=True, cyclic=True, blockers=[])
            continue
        seen.add(name)
        path = find_in_sysroot(sysroot, name)
        if not path:
            deps[name] = dict(found=False, blockers=[])
            continue
        d = Elf(path)
        dr = collect(d)
        deps[name] = dict(found=True, path=path, size=dr.get("size", 0),
                          interp=dr.get("interp", ""),
                          blockers=judge(dr) if dr["ok"] else [("read", dr["err"], "")])
        if depth > 1 and dr.get("needed"):
            deps[name]["deps"] = resolve_deps(dr, sysroot, depth - 1, seen)
    return deps

def judge(r):
    """按依赖顺序列 blockers：地址 -> 装载 -> 重定位 -> 运行期。"""
    b = []
    if not r["ok"]:
        return [("read", r["err"], "先修文件本身")]

    lo, hi = r["load_lo"], r["load_hi"]
    # ET_DYN 的装载地址由装载器选，链接期基址 0 不是阻塞点；只有「把基址
    # 固定在窗口以外」的 ET_EXEC 才是真的装不下。ET_DYN 另判跨度。
    fixed = r["elftype"] != 3
    if fixed and (lo < ARK_WINDOW_BASE or hi > ARK_WINDOW_END):
        b.append(("address",
                  "PT_LOAD 覆盖 [0x%x, 0x%x)，当前窗口是 [0x%x, 0x%x)"
                  % (lo, hi, ARK_WINDOW_BASE, ARK_WINDOW_END),
                  "装载器要能按 ELF 自己要的 VA 装（每进程 VA 空间 + 动态建页表），"
                  "不是只有 2 MB 固定窗口"))
    if r["span"] > ARK_WINDOW_SIZE:
        b.append(("span",
                  "镜像跨度 %d KB > 窗口 %d KB" % (r["span"] // 1024, ARK_WINDOW_SIZE // 1024),
                  "同上：窗口尺寸要可配置，或改成按需映射"))
    if r["interp"] and not r["interp"].startswith("/mnt/ram/"):
        b.append(("interp-path",
                  "PT_INTERP 指向 %s，VFS 里没有这个路径" % r["interp"],
                  "把解释器放到该路径，或让装载器按 /lib、/lib64 前缀去 /mnt/ram 找"))
    if r["has_tls"] and not ARK_SUPPORTS_PT_TLS:
        b.append(("tls",
                  "有 PT_TLS（filesz=%d memsz=%d align=%d）"
                  % (r["tls"]["filesz"], r["tls"]["memsz"], r["tls"]["align"]),
                  "内核/loader 要建 TCB 与 tdata/tbss 并把 TP 指向它；现在 elf.c 完全没读 PT_TLS"))
    if r["has_gnu_hash"] and not r["has_sysv_hash"]:
        b.append(("gnu-hash",
                  "只有 DT_GNU_HASH，没有 .hash",
                  "自研 ld.so 只认 DT_HASH，读不了这份符号表"))

    unsupported_dt = [t for t in r["dyn_tags"]
                      if t not in ARK_SUPPORTED_DT and t not in ARK_IGNORED_DT]
    if unsupported_dt:
        b.append(("dynamic-tags",
                  "动态段里有 Ark 没实现的标签：%s"
                  % ", ".join(DT_NAMES.get(t, "0x%x" % t) for t in unsupported_dt),
                  "按标签逐个补：INIT_ARRAY 要执行构造函数，VERNEED/VERSYM 要做符号版本匹配"))
    # ldso 把 RELA 项长度写死成 24（一个 Elf64_Rela）；这个常数成立的前提
    # 就是 DT_RELAENT == 24，所以这个标签不是「不需要，而是「必须核对」。
    if r.get("relaent") not in (None, 24):
        b.append(("relaent",
                  "DT_RELAENT=%d，不是 24（ldso 按 24 字节一条扫）" % r["relaent"],
                  "要么按 DT_RELAENT 走，要么在装载期直接拒"))
    if r.get("pltrel_is_rela") is False:
        b.append(("pltrel-kind",
                  "DT_PLTREL 不是 RELA（=7），PLT 重定位不是 .rela 格式",
                  "ldso 只扫 .rela.dyn / .rela.plt；REL 形态要另写一套"))
    # 「Ark 处理哪些重定位」只由上面的 ARK_SUPPORTED_RELOC 说，这里不再抄
    # 一份名字表——抄一份的下场是加了 TLS_TPREL64 之后两处说法不一致。
    ok_reloc_names = set(R_NAMES[v] for v in ARK_SUPPORTED_RELOC)
    ok_reloc_names.add("R_LARCH_NONE")
    bad_reloc = {}
    for name, n in r["reloc_hist"].items():
        if name in ok_reloc_names:
            continue
        bad_reloc[name] = n
    if bad_reloc:
        b.append(("relocations",
                  "有 Ark 不处理的重定位：%s"
                  % ", ".join("%s x%d" % (k, v) for k, v in sorted(bad_reloc.items())),
                  "IRELATIVE 要调用 resolver（IFUNC）；动态 TLS 的 DTPMOD/DTPREL/"
                  "TLSDESC 要跨对象 TLS 模型，COPY 要跨对象拷字节"))
    # 依赖本身不是阻塞项：能不能装载依赖属于「装载器能力」，会在 dependent
    # 自己的 blockers 里出现。只有解析过 sysroot 且真的找不到时才算阻塞。
    if r["needed"] and r.get("deps"):
        miss = []
        deep = []
        for name, dep in r["deps"].items():
            if not dep.get("found"):
                miss.append(name)
            elif dep.get("blockers"):
                deep.append("%s(%s)"
                            % (name, "/".join(k for k, _w, _t in dep["blockers"])))
        if miss:
            b.append(("needed",
                      "sysroot 里找不到 %s" % ", ".join(miss),
                      "这些对象要先能被按名找到；名字里找不到时不许猜路径"))
        elif deep:
            b.append(("needed",
                      "被依赖对象自身也有 blocker：%s" % ", ".join(deep),
                      "依赖整条链一起满足，不是只把主程序装进去"))
    if r["stack_exec"]:
        b.append(("stack", "PT_GNU_STACK 要求可执行栈",
                  "当前整窗一条 RWX 条目恰好能跑，但没有 NX 语义"))
    return b


def fmt(r):
    out = []
    a = out.append
    a("== %s" % r["path"])
    if not r["ok"]:
        a("  ERROR: %s" % r["err"])
        return "\n".join(out)
    a("  type=%s machine=%d entry=0x%x size=%d B"
      % ({2: "ET_EXEC", 3: "ET_DYN"}.get(r["elftype"], r["elftype"]),
         r["machine"], r["entry"], r["size"]))
    a("  PT_LOAD x%d  span=%d B  [0x%x, 0x%x)  max_align=0x%x%s"
      % (r["n_load"], r["span"], r["load_lo"], r["load_hi"], r["max_align"],
         "  W+X 段存在" if r["writable_exec"] else ""))
    a("  PT_INTERP: %s" % (r["interp"] or "(none, static)"))
    a("  PT_TLS: %s   RELRO=%s   exec-stack=%s"
      % ("yes" if r["has_tls"] else "no", r["has_relro"], r["stack_exec"]))
    a("  DT_NEEDED: %s" % (", ".join(r["needed"]) or "(none)"))
    a("  hash: gnu=%s sysv=%s | versym=%s verneed=%s | init_array=%s | relr=%s | bind_now=%s"
      % (r["has_gnu_hash"], r["has_sysv_hash"], r["has_versym"],
         r["has_verneed"], r["has_init_array"], r["has_relr"], r["bind_now"]))
    a("  relocs: %s" % (", ".join("%s x%d" % (k, v)
                                  for k, v in sorted(r["reloc_hist"].items())) or "(none)"))
    if r["verneed"]:
        a("  symbol versions needed: %s" % ", ".join(sorted(set(r["verneed"]))))
    for name, dep in sorted(r.get("deps", {}).items()):
        if not dep.get("found"):
            a("  dep %s -> (sysroot 里没有)" % name)
            continue
        if dep.get("cyclic"):
            a("  dep %s -> (已在本次解析链上看过)" % name)
            continue
        a("  dep %s -> %s  %d B%s"
          % (name, dep["path"], dep["size"],
             ("  INTERP=%s" % dep["interp"]) if dep["interp"] else ""))
        if dep["blockers"]:
            a("      自身 blocker: %s"
              % "; ".join("[%s] %s" % (k, w) for k, w, _t in dep["blockers"]))
    bl = judge(r)
    if bl:
        a("  blockers (%d, 按依赖顺序):" % len(bl))
        for i, (key, what, todo) in enumerate(bl, 1):
            a("    %d. [%s] %s" % (i, key, what))
            a("       -> %s" % todo)
    else:
        a("  blockers: none (这个二进制落在当前窗口内，且只用已支持的标签/重定位)")
    return "\n".join(out)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--sysroot", default="", help="递归解析 DT_NEEDED 的根目录")
    a = ap.parse_args(argv)

    reports = []
    for path in a.files:
        e = Elf(path)
        r = collect(e)
        if a.sysroot and r["ok"] and r["needed"]:
            if not os.path.isdir(a.sysroot):
                r["deps"] = {n: dict(found=False) for n in r["needed"]}
                r["sysroot_error"] = "not a directory: %s" % a.sysroot
            else:
                r["deps"] = resolve_deps(r, a.sysroot)
        reports.append(r)

    if a.json:
        print(json.dumps(reports, indent=2, ensure_ascii=False))
    else:
        for r in reports:
            if r.get("sysroot_error"):
                print("!! --sysroot %s" % r["sysroot_error"])
            print(fmt(r))
            print("")

    rc = 0
    for r in reports:
        if judge(r):
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))