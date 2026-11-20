#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dtb_verify - check a DTB against the Devicetree spec and against its DTS.

Why an independent reader instead of trusting the writer
--------------------------------------------------------
`dtc_lite.py` emits two products from one parse: the C table the MCU tier links
and the flattened blob the MMU tier boots.  If the verifier reused the writer's
own encoding helpers, a wrong endianness or a forgotten pad byte would pass
silently in both directions.  So this file re-implements the *reader* side from
the specification text, byte by byte, with no import from dtc_lite's encoder.
Two independent implementations agreeing is evidence; one implementation
agreeing with itself is not.

What is checked
---------------
  A. header: magic 0xd00dfeed, version 17 / last_comp_version 16, every offset
     inside totalsize, totalsize == file length, strings block non-empty;
  B. memory reservation block: 8-byte aligned, terminated by a zero pair;
  C. structure block: walks BEGIN_NODE / PROP / END_NODE / END with strict
     nesting, every node name NUL-terminated inside the block, every property
     name offset inside the strings block, values 4-byte padded, and FDT_END
     landing exactly at off_dt_struct + size_dt_struct;
  D. content: the tree read back from the blob equals the tree in the DTS --
     same node paths, same property order, same bytes per property (strings
     NUL-joined, cells big-endian, bytes raw, valueless properties empty).

Usage
-----
    py -3 tools/dts/dtb_verify.py --dts dts/qemu_loongarch64.dts
    py -3 tools/dts/dtb_verify.py --dts dts/qemu_loongarch64.dts --dtb FILE
    py -3 tools/dts/dtb_verify.py --all
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dtc_lite  # noqa: E402  (parser only: build() gives the DTS model)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

FDT_MAGIC = 0xd00dfeed
FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_NOP = 4
FDT_END = 9


class DtbError(Exception):
    pass


def _u32(blob, off, what):
    if off + 4 > len(blob):
        raise DtbError("%s: 4-byte read past end of blob at 0x%x" % (what, off))
    return struct.unpack(">I", blob[off:off + 4])[0], off + 4


def _align4(off):
    return (off + 3) & ~3


def parse_header(blob):
    if len(blob) < 40:
        raise DtbError("blob shorter than the 40-byte header (%d)" % len(blob))
    f = struct.unpack(">10I", blob[:40])
    h = dict(zip(("magic", "totalsize", "off_struct", "off_strings",
                  "off_rsvmap", "version", "last_comp", "boot_cpuid",
                  "size_strings", "size_struct"), f))
    if h["magic"] != FDT_MAGIC:
        raise DtbError("bad magic 0x%08x (want 0x%08x)" % (h["magic"], FDT_MAGIC))
    if h["version"] != 17 or h["last_comp"] != 16:
        raise DtbError("version %d/%d, want 17/16" % (h["version"], h["last_comp"]))
    if h["totalsize"] != len(blob):
        raise DtbError("totalsize %d != file length %d" % (h["totalsize"], len(blob)))
    if h["off_rsvmap"] % 8:
        raise DtbError("off_mem_rsvmap 0x%x is not 8-byte aligned" % h["off_rsvmap"])
    if h["off_struct"] % 4:
        raise DtbError("off_dt_struct 0x%x is not 4-byte aligned" % h["off_struct"])
    for k in ("off_struct", "off_strings", "off_rsvmap"):
        if h[k] >= len(blob):
            raise DtbError("%s 0x%x outside the blob" % (k, h[k]))
    if h["off_struct"] + h["size_struct"] > len(blob):
        raise DtbError("structure block runs past the end of the blob")
    if h["off_strings"] + h["size_strings"] > len(blob):
        raise DtbError("strings block runs past the end of the blob")
    if h["size_strings"] == 0:
        raise DtbError("empty strings block")
    return h


def parse_rsvmap(blob, h):
    off = h["off_rsvmap"]
    count = 0
    while True:
        if off + 16 > len(blob):
            raise DtbError("memory reservation block has no terminator")
        addr, size = struct.unpack(">QQ", blob[off:off + 16])
        off += 16
        if addr == 0 and size == 0:
            return count
        count += 1


def parse_strings(blob, h):
    base = h["off_strings"]
    end = base + h["size_strings"]
    if blob[end - 1] != 0:
        raise DtbError("strings block does not end with NUL")
    return blob[base:end]


def _string_at(strings, off, what):
    if off >= len(strings):
        raise DtbError("%s: name offset 0x%x outside strings block" % (what, off))
    end = strings.find(b"\0", off)
    if end < 0:
        raise DtbError("%s: unterminated name at 0x%x" % (what, off))
    return strings[off:end].decode("utf-8")


def parse_struct(blob, h):
    """Walk the structure block; return {path: [(prop_name, bytes), ...]}."""
    strings = parse_strings(blob, h)
    off = h["off_struct"]
    end = h["off_struct"] + h["size_struct"]
    path = []
    tree = {}
    order = []

    while True:
        if off >= end:
            raise DtbError("structure block ended without FDT_END")
        tok, off = _u32(blob, off, "struct token")
        if tok == FDT_BEGIN_NODE:
            name_end = blob.find(b"\0", off, end)
            if name_end < 0:
                raise DtbError("node name at 0x%x has no NUL inside the block" % off)
            name = blob[off:name_end].decode("utf-8")
            off = _align4(name_end + 1)
            path.append(name)
            p = "/" + "/".join([x for x in path if x])
            if p != "/" and p not in tree:
                tree[p] = []
                order.append(p)
            elif p == "/":
                tree["/"] = []
                order.insert(0, "/")
        elif tok == FDT_END_NODE:
            if not path:
                raise DtbError("FDT_END_NODE with an empty node stack")
            path.pop()
        elif tok == FDT_PROP:
            length, off = _u32(blob, off, "property length")
            nameoff, off = _u32(blob, off, "property name offset")
            if off + length > end:
                raise DtbError("property value runs past the structure block")
            val = blob[off:off + length]
            off = _align4(off + length)
            if not path:
                raise DtbError("property outside any node")
            p = "/" + "/".join([x for x in path if x])
            tree[p].append((_string_at(strings, nameoff, "property"),
                            bytes(val)))
        elif tok == FDT_NOP:
            continue
        elif tok == FDT_END:
            if path:
                raise DtbError("FDT_END with %d unclosed node(s)" % len(path))
            if off != end:
                raise DtbError("FDT_END at 0x%x but block ends at 0x%x; %d byte(s) "
                               "of trailing garbage" % (off, end, end - off))
            return tree, order
        else:
            raise DtbError("unknown structure token 0x%x at 0x%x" % (tok, off - 4))


def model_from_dts(dts_path):
    """The same tree as the DTS describes, encoded by an independent writer."""
    root = dtc_lite.build(dts_path)
    # dtc_lite only adds __symbols__/__fixups__ on the dtb output path; the
    # model must include them or the comparison below reports them as
    # 'node only in DTB'.
    dtc_lite.add_meta_nodes(root)
    tree = {}
    order = []

    def walk(node, path):
        tree[path] = []
        order.append(path)
        for pname in node.prop_order:
            kind, vals = node.props[pname]
            if kind == dtc_lite.STR:
                val = b"".join(s.encode("utf-8") + b"\0" for s in vals)
            elif kind == dtc_lite.CELLS:
                val = b"".join(struct.pack(">I", v & 0xffffffff) for v in vals)
            elif kind == dtc_lite.BYTES:
                val = bytes(vals)
            else:
                val = b""
            tree[path].append((pname, val))
        for c in node.children:
            walk(c, (path.rstrip("/") + "/" + c.full_name))

    walk(root, "/")
    return tree, order


def compare(dtb_tree, dts_tree):
    problems = []
    only_dtb = sorted(set(dtb_tree) - set(dts_tree))
    only_dts = sorted(set(dts_tree) - set(dtb_tree))
    for p in only_dtb:
        problems.append("node only in DTB: %s" % p)
    for p in only_dts:
        problems.append("node only in DTS: %s" % p)
    for p in sorted(set(dtb_tree) & set(dts_tree)):
        a, b = dtb_tree[p], dts_tree[p]
        if a != b:
            if [n for n, _ in a] != [n for n, _ in b]:
                problems.append("%s: property names/order differ (%s vs %s)"
                                % (p, [n for n, _ in a], [n for n, _ in b]))
            else:
                for (n, va), (_n2, vb) in zip(a, b):
                    if va != vb:
                        problems.append("%s/%s: bytes differ (%d vs %d)"
                                        % (p, n, len(va), len(vb)))
    return problems


def verify(dts_path, dtb_path=None):
    """Return (ok, messages). Raises nothing: a gate wants every problem at once."""
    msgs = []
    root = dtc_lite.build(dts_path)
    dtc_lite.add_meta_nodes(root)
    blob = dtc_lite.gen_dtb(root) if dtb_path is None else open(dtb_path, "rb").read()
    try:
        h = parse_header(blob)
        rsv = parse_rsvmap(blob, h)
        msgs.append("header ok (version 17/16, totalsize %d, %d byte(s) reserved)"
                    % (h["totalsize"], rsv * 16))
        dtb_tree, dtb_order = parse_struct(blob, h)
        msgs.append("structure block ok (%d node(s), %d byte(s))"
                    % (len(dtb_order), h["size_struct"]))
    except DtbError as e:
        return False, msgs + ["FAIL: %s" % e]
    dts_tree, dts_order = model_from_dts(dts_path)
    problems = compare(dtb_tree, dts_tree)
    if problems:
        return False, msgs + problems
    msgs.append("content matches the DTS (%d node(s), %d property/properties)"
                % (len(dts_order), sum(len(v) for v in dts_tree.values())))
    return True, msgs


def main(argv):
    ap = argparse.ArgumentParser(description="verify a DTB against the spec and its DTS")
    ap.add_argument("--dts")
    ap.add_argument("--dtb", help="verify this file; default: generate in memory")
    ap.add_argument("--all", action="store_true", help="every dts/*.dts")
    args = ap.parse_args(argv)

    os.chdir(ROOT)
    if args.all:
        names = sorted(f for f in os.listdir("dts") if f.endswith(".dts"))
        bad = 0
        for f in names:
            ok, msgs = verify(os.path.join("dts", f))
            print("%-24s %s" % (f, "OK" if ok else "FAIL"))
            for m in msgs:
                if m.startswith("FAIL") or not ok:
                    print("    " + m)
            bad += 0 if ok else 1
        return 1 if bad else 0

    if not args.dts:
        ap.error("--dts or --all is required")
    ok, msgs = verify(args.dts, args.dtb)
    for m in msgs:
        print(("  " if ok else "  ") + m)
    print("DTB-VERIFY %s: %s" % ("OK" if ok else "FAIL", args.dts))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))