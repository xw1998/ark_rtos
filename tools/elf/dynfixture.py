#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build a dynamic-linking test fixture (PT_INTERP + PT_DYNAMIC) from a
statically linked LoongArch ELF.

Why this exists
---------------
The toolchain shipped with this repo (`loongarch64-newlib-elf-ld` 2.31.1)
refuses `-shared` and `-pie`; driving gcc with `-shared` silently produces a
static ET_EXEC with no .dynamic at all.  So there is no way to get a real
PIE / .so from the toolchain on this machine.

This script takes a *statically linked* image produced by that same
toolchain and rewrites it into a dynamic one:

  * adds PT_INTERP (the ld.so path),
  * adds PT_DYNAMIC with an STRTAB / SYMTAB / HASH / RELA set,
  * keeps the original PT_LOAD segments byte-for-byte,
  * adds one RW PT_LOAD holding those tables,
  * makes sure the program header table is covered by a PT_LOAD so that
    AT_PHDR = base + 64 holds (the assumption every ld.so makes).

The relocations are *authored* by the manifest (which symbol, which kind,
which slot), not produced by a linker.  That is deliberate: it lets the
test drive exactly the relocation kinds the loader is supposed to support
(RELATIVE / R_LARCH_64 / JUMP_SLOT) and lets a gate break one of them on
purpose to prove the loader fails loudly instead of silently running.

Inputs
------
  --input   static ELF, unstripped (the symbol table is read for addresses)
  --manifest JSON:
      {
        "type": "dyn" | "exec",
        "base": 0,                     # link-time base (0 for dyn, window base for exec)
        "interp": "/mnt/ram/ld-ark.so",
        "soname": "libanswer.so" | null,
        "needed": ["libanswer.so"],
        "exports": ["answer_add", "g_base"],
        "imports": ["answer_add"],
        "relocs": [
          {"kind": "relative", "at": "g_ptr", "addend_sym": "g_base"},
          {"kind": "slot64",   "at": "got_absptr", "sym": "abs_ptr"},
          {"kind": "jumpslot", "at": "got_answer", "sym": "answer_add"}
        ]
      }
  --output  output ELF path

Exit codes: 0 ok, 2 bad input / manifest, 3 layout impossible.
"""

import argparse
import io
import json
import struct
import sys

PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3
PT_PHDR = 6
PT_TLS = 7

ET_EXEC = 2
ET_DYN = 3
EM_LOONGARCH = 258

SHT_SYMTAB = 2
SHN_UNDEF = 0

DT_NULL = 0
DT_NEEDED = 1
DT_PLTRELSZ = 2
DT_HASH = 4
DT_STRTAB = 5
DT_SYMTAB = 6
DT_RELA = 7
DT_RELASZ = 8
DT_RELAENT = 9
DT_STRSZ = 10
DT_SYMENT = 11
DT_SONAME = 14
DT_PLTREL = 20
DT_JMPREL = 23

R_LARCH_64 = 2
R_LARCH_RELATIVE = 3
R_LARCH_JUMP_SLOT = 5
R_LARCH_TLS_TPREL64 = 11

KIND_TO_TYPE = {"slot64": R_LARCH_64, "jumpslot": R_LARCH_JUMP_SLOT,
                "tprel64": R_LARCH_TLS_TPREL64}

STT_TLS = 6

PHDR = struct.Struct("<IIQQQQQQ")
SHDR = struct.Struct("<IIQQQQIIQQ")
SYM = struct.Struct("<IBBHQQ")
DYN = struct.Struct("<qQ")


def align_up(v, a):
    return (v + a - 1) & ~(a - 1)


class InputElf(object):
    def __init__(self, data):
        self.data = data
        if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
            raise ValueError("not a little-endian ELF64 file")
        (self.e_type, self.e_machine, self.e_version, self.e_entry,
         self.e_phoff, self.e_shoff, self.e_flags, self.e_ehsize,
         self.e_phentsize, self.e_phnum, self.e_shentsize, self.e_shnum,
         self.e_shstrndx) = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
        if self.e_machine != EM_LOONGARCH:
            raise ValueError("not a LoongArch ELF (e_machine=%d)" % self.e_machine)

        self.phdrs = []
        for i in range(self.e_phnum):
            off = self.e_phoff + i * self.e_phentsize
            p = PHDR.unpack_from(data, off)
            self.phdrs.append({
                "type": p[0], "flags": p[1], "offset": p[2], "vaddr": p[3],
                "paddr": p[4], "filesz": p[5], "memsz": p[6], "align": p[7],
            })

        self.sections = []
        if self.e_shoff and self.e_shnum:
            for i in range(self.e_shnum):
                off = self.e_shoff + i * self.e_shentsize
                s = SHDR.unpack_from(data, off)
                self.sections.append({
                    "name": s[0], "type": s[1], "flags": s[2], "addr": s[3],
                    "offset": s[4], "size": s[5], "link": s[6], "info": s[7],
                    "addralign": s[8], "entsize": s[9],
                })

        self.symbols = {}
        self._read_symbols()

    def _read_symbols(self):
        for sec in self.sections:
            if sec["type"] != SHT_SYMTAB or sec["entsize"] != SYM.size:
                continue
            strtab = self.sections[sec["link"]]
            base = strtab["offset"]
            n = sec["size"] // sec["entsize"]
            for i in range(n):
                off = sec["offset"] + i * sec["entsize"]
                nameoff, info, other, shndx, value, size = SYM.unpack_from(
                    self.data, off)
                end = self.data.find(b"\0", base + nameoff)
                name = self.data[base + nameoff:end].decode("utf-8", "replace")
                if not name:
                    continue
                self.symbols[name] = {
                    "value": value, "size": size, "shndx": shndx,
                    "info": info,
                }

    def loads(self):
        return [p for p in self.phdrs if p["type"] == PT_LOAD]


class DynStr(object):
    def __init__(self):
        self.buf = bytearray(b"\0")
        self.off = {"": 0}

    def add(self, s):
        if s in self.off:
            return self.off[s]
        o = len(self.buf)
        self.buf += s.encode("utf-8") + b"\0"
        self.off[s] = o
        return o


def build(input_path, manifest, output_path):
    data = io.open(input_path, "rb").read()
    e = InputElf(data)

    if e.e_type != ET_EXEC:
        raise ValueError("input must be a static ET_EXEC image (got type %d)"
                         % e.e_type)

    imgtype = manifest.get("type", "dyn")
    if imgtype in ("dyn", "exec"):
        out_type = ET_DYN if imgtype == "dyn" else ET_EXEC
    else:
        raise ValueError("manifest type must be dyn or exec")

    base = int(manifest.get("base", 0))
    if imgtype == "dyn" and base != 0:
        raise ValueError("a dyn fixture must be linked at base 0")
    if imgtype == "exec" and base == 0:
        raise ValueError("an exec fixture needs the window base in manifest")

    interp = manifest.get("interp", "")
    if not interp:
        raise ValueError("manifest.interp is required (PT_INTERP path)")

    loads = sorted(e.loads(), key=lambda p: p["vaddr"])
    if not loads:
        raise ValueError("input has no PT_LOAD segments")

    # TLS 段原样搬到输出：本仓的 ld.so 要按 PT_TLS 建线程块，所以夹具必须
    # 带着真实的 tdata 模板与 memsz，不能自己编。偏移按输出布局重算
    # （输出里 PT_LOAD 的 file offset == vaddr - base）。
    tls_seg = None
    for p in e.phdrs:
        if p["type"] == PT_TLS:
            tls_seg = p
            break
    tls_exports = list(manifest.get("tls_exports", []))
    if tls_exports and tls_seg is None:
        raise ValueError("manifest lists tls_exports but the input has no PT_TLS")

    # The header table must sit in a PT_LOAD, otherwise AT_PHDR does not
    # exist.  When the linker put the first LOAD above the image base (very
    # common for exec images at 0x02000000), synthesize a read-only LOAD
    # covering [base, first_vaddr).
    min_vaddr = loads[0]["vaddr"]
    synthetic = None
    if min_vaddr > base:
        synthetic = {"vaddr": base, "memsz": min_vaddr - base}
    elif min_vaddr < base:
        raise ValueError("a segment starts below the image base")

    # ---- symbol tables -------------------------------------------------
    exports = list(manifest.get("exports", []))
    imports = list(manifest.get("imports", []))

    def sym_value(name):
        if name not in e.symbols:
            raise ValueError("symbol %r not found in the input .symtab" % name)
        return e.symbols[name]["value"]

    relocs = manifest.get("relocs", [])
    for r in relocs:
        name = r.get("sym")
        if r.get("kind") == "tprel64":
            # TLS 符号必须由清单显式声明（它的 st_value 是块内偏移，不是
            # 绝对地址），不能走下面"看到定义就当普通导出"的默认路径。
            if name not in tls_exports:
                raise ValueError("tprel64 target %r must be listed in tls_exports"
                                 % (name,))
            continue
        if name and name not in exports and name not in imports:
            entry = e.symbols.get(name)
            if entry is not None and entry["shndx"] != SHN_UNDEF:
                exports.append(name)
            else:
                imports.append(name)

    # de-duplicate, keep order
    seen = set()
    exports = [x for x in exports if not (x in seen or seen.add(x))]
    seen = set()
    imports = [x for x in imports if not (x in seen or seen.add(x))]

    dynstr = DynStr()
    soname_off = dynstr.add(manifest["soname"]) if manifest.get("soname") else None
    needed_offs = [dynstr.add(n) for n in manifest.get("needed", [])]
    interp_off_str = dynstr.add(interp)

    # dynsym: null + exports (defined) + imports (undefined)
    dynsym = [(0, 0, 0, 0, 0)]  # name, info, other, shndx, value
    name_off = {}
    for name in exports:
        off = dynstr.add(name)
        v = sym_value(name)
        dynsym.append((off, 0x12, 0, 1, v))  # GLOBAL FUNC; shndx 1 = defined
        name_off[name] = len(dynsym) - 1
    for name in tls_exports:
        off = dynstr.add(name)
        # STT_TLS 的 st_value 本身就是"相对 TLS 块起点的偏移"（实测本仓
        # 工具链：lib_tls 的 st_value = 0，而 PT_TLS.p_vaddr = 0x5178），
        # 不是绝对地址，所以这里不能减 p_vaddr。
        v = sym_value(name)
        if v < 0 or v >= tls_seg["memsz"]:
            raise ValueError("TLS symbol %r (off 0x%x) is outside PT_TLS "
                             "(memsz 0x%x)" % (name, v, tls_seg["memsz"]))
        dynsym.append((off, 0x16, 0, 1, v))  # GLOBAL | STT_TLS
        name_off[name] = len(dynsym) - 1
    for name in imports:
        off = dynstr.add(name)
        dynsym.append((off, 0x12, 0, 0, 0))
        name_off[name] = len(dynsym) - 1

    # ---- layout of the new RW segment ---------------------------------
    max_vaddr = max(p["vaddr"] + p["memsz"] for p in loads)
    cur = align_up(max(max_vaddr, base + (synthetic["memsz"] if synthetic else 0)), 16)

    def place(n, a):
        nonlocal cur
        cur = align_up(cur, a)
        here = cur
        cur += n
        return here

    dynstr_addr = place(len(dynstr.buf), 1)
    dynsym_addr = place(len(dynsym) * SYM.size, 8)
    nchain = len(dynsym)
    nbucket = 1
    hash_blob = struct.pack("<II", nbucket, nchain)
    hash_blob += struct.pack("<I", 0)                       # one bucket
    hash_blob += b"".join(struct.pack("<I", 0) for _ in range(nchain))
    hash_addr = place(len(hash_blob), 4)

    rela_entries = []
    plt_entries = []
    for r in relocs:
        kind = r["kind"]
        at = sym_value(r["at"])
        if kind == "relative":
            addend = sym_value(r["addend_sym"])
            rela_entries.append((at, (0 << 32) | R_LARCH_RELATIVE, addend))
        elif kind in ("slot64", "jumpslot"):
            idx = name_off[r["sym"]]
            rtype = KIND_TO_TYPE[kind]
            ent = (at, (idx << 32) | rtype, 0)
            (plt_entries if kind == "jumpslot" else rela_entries).append(ent)
        elif kind == "tprel64":
            idx = name_off[r["sym"]]
            ent = (at, (idx << 32) | R_LARCH_TLS_TPREL64, 0)
            rela_entries.append(ent)
        else:
            raise ValueError("unknown reloc kind %r" % kind)

    rela_addr = place(len(rela_entries) * 24, 8) if rela_entries else 0
    jmprel_addr = place(len(plt_entries) * 24, 8) if plt_entries else 0

    dyntab = []

    def dtag(tag, val):
        dyntab.append((tag, val))

    for off in needed_offs:
        dtag(DT_NEEDED, off)
    if soname_off is not None:
        dtag(DT_SONAME, soname_off)
    dtag(DT_STRTAB, dynstr_addr)
    dtag(DT_SYMTAB, dynsym_addr)
    dtag(DT_STRSZ, len(dynstr.buf))
    dtag(DT_SYMENT, SYM.size)
    dtag(DT_HASH, hash_addr)
    if rela_addr:
        dtag(DT_RELA, rela_addr)
        dtag(DT_RELASZ, len(rela_entries) * 24)
        dtag(DT_RELAENT, 24)
    if jmprel_addr:
        dtag(DT_JMPREL, jmprel_addr)
        dtag(DT_PLTRELSZ, len(plt_entries) * 24)
        dtag(DT_PLTREL, DT_RELA)
    dtag(DT_NULL, 0)

    dyn_addr = place(len(dyntab) * DYN.size, 8)
    interp_addr = place(len(interp) + 1, 1)

    extra_lo = align_up(max(max_vaddr, base + (synthetic["memsz"] if synthetic else 0)), 16)
    extra_hi = cur

    # ---- program headers ----------------------------------------------
    phdrs = []
    phdrs.append({"type": PT_PHDR, "flags": 4, "offset": 64,
                  "vaddr": base + 64, "filesz": 0, "memsz": 0, "align": 8})
    phdrs.append({"type": PT_INTERP, "flags": 4,
                  "offset": interp_addr - base, "vaddr": interp_addr,
                  "filesz": len(interp) + 1, "memsz": len(interp) + 1,
                  "align": 1})
    if synthetic:
        phdrs.append({"type": PT_LOAD, "flags": 4, "offset": 0,
                      "vaddr": synthetic["vaddr"],
                      "filesz": synthetic["memsz"],
                      "memsz": synthetic["memsz"], "align": 0x1000})
    for p in loads:
        phdrs.append({"type": PT_LOAD, "flags": p["flags"],
                      "offset": p["vaddr"] - base, "vaddr": p["vaddr"],
                      "filesz": p["filesz"], "memsz": p["memsz"],
                      "align": 0x1000})
    phdrs.append({"type": PT_LOAD, "flags": 6,
                  "offset": extra_lo - base, "vaddr": extra_lo,
                  "filesz": extra_hi - extra_lo,
                  "memsz": extra_hi - extra_lo, "align": 0x1000})
    if tls_seg is not None:
        phdrs.append({"type": PT_TLS, "flags": 4,
                      "offset": tls_seg["vaddr"] - base,
                      "vaddr": tls_seg["vaddr"], "filesz": tls_seg["filesz"],
                      "memsz": tls_seg["memsz"], "align": tls_seg["align"]})
    phdrs.append({"type": PT_DYNAMIC, "flags": 6,
                  "offset": dyn_addr - base, "vaddr": dyn_addr,
                  "filesz": len(dyntab) * DYN.size,
                  "memsz": len(dyntab) * DYN.size, "align": 8})

    # PT_PHDR filesz needs the final count
    phdrs[0]["filesz"] = len(phdrs) * PHDR.size
    phdrs[0]["memsz"] = len(phdrs) * PHDR.size

    if synthetic and (min_vaddr - base) < (64 + len(phdrs) * PHDR.size):
        raise ValueError("no room for the header table below the first LOAD")

    # ---- write the file ------------------------------------------------
    total = 0
    for p in phdrs:
        if p["type"] in (PT_LOAD,):
            total = max(total, p["offset"] + p["filesz"])
    total = max(total, extra_hi - base, 64 + len(phdrs) * PHDR.size)

    out = bytearray(total)

    for p in loads:
        off = p["offset"]
        dst = p["vaddr"] - base
        out[dst:dst + p["filesz"]] = data[off:off + p["filesz"]]

    # extra segment contents
    o = dynstr_addr - base
    out[o:o + len(dynstr.buf)] = dynstr.buf
    o = dynsym_addr - base
    for i, (nameoff, info, other, shndx, value) in enumerate(dynsym):
        SYM.pack_into(out, o + i * SYM.size, nameoff, info, other, shndx,
                      value, 0)
    o = hash_addr - base
    out[o:o + len(hash_blob)] = hash_blob
    if rela_addr:
        o = rela_addr - base
        for i, (roff, rinfo, addend) in enumerate(rela_entries):
            struct.pack_into("<QQq", out, o + i * 24, roff, rinfo, addend)
    if jmprel_addr:
        o = jmprel_addr - base
        for i, (roff, rinfo, addend) in enumerate(plt_entries):
            struct.pack_into("<QQq", out, o + i * 24, roff, rinfo, addend)
    o = dyn_addr - base
    for i, (tag, val) in enumerate(dyntab):
        DYN.pack_into(out, o + i * DYN.size, tag, val)
    o = interp_addr - base
    out[o:o + len(interp) + 1] = interp.encode("utf-8") + b"\0"

    # ELF header + program header table
    e_ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8
    struct.pack_into("<16sHHIQQQIHHHHHH", out, 0,
                     e_ident, out_type, EM_LOONGARCH, 1, e.e_entry, 64, 0,
                     e.e_flags, 64, PHDR.size, len(phdrs), 0, 0, 0)
    for i, p in enumerate(phdrs):
        PHDR.pack_into(out, 64 + i * PHDR.size, p["type"], p["flags"],
                       p["offset"], p["vaddr"], p["vaddr"], p["filesz"],
                       p["memsz"], p["align"])

    io.open(output_path, "wb").write(bytes(out))

    return {
        "type": imgtype,
        "base": base,
        "phnum": len(phdrs),
        "entry": e.e_entry,
        "extra": [extra_lo, extra_hi],
        "dynsym": len(dynsym),
        "rela": len(rela_entries),
        "jmprel": len(plt_entries),
        "tls": len(tls_exports),
        "size": total,
    }


def main(argv):
    ap = argparse.ArgumentParser(description="build a PT_INTERP test fixture")
    ap.add_argument("--input", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--output", required=True)
    args = ap.parse_args(argv)

    manifest = json.loads(io.open(args.manifest, "r", encoding="utf-8").read())
    try:
        info = build(args.input, manifest, args.output)
    except (ValueError, KeyError) as exc:
        sys.stderr.write("dynfixture: %s\n" % exc)
        return 2
    print("dynfixture: %s type=%s phnum=%d dynsym=%d rela=%d jmprel=%d size=%d"
          % (args.output, info["type"], info["phnum"], info["dynsym"],
             info["rela"], info["jmprel"], info["size"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))