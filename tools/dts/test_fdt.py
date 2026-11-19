#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Host self-test for the DTB reader (kernelsrc/src/arkrtos_fdt.c).

Steps, per board under dts/:
  1. compile the DTS to a flattened blob (dtc_lite.py --format dtb);
  2. build tools/dts/test_fdt.c against that board's generated C table
     (board/<board>/arkrtos_dt.c) and the FDT reader;
  3. run it: node for node, name / compatible / reg / enabled / property
     count must agree with the table, the node order must agree, and damaged
     blobs (bad magic, oversized totalsize, truncated struct block) must be
     refused with the specific error code.

Why both outputs are linked into one binary: they come from the same parse,
but they are produced by two different code paths (C table emitter vs DTB
emitter) and read back by two different implementations (the table's query
functions vs the FDT walker).  Comparing them is therefore a real cross-check,
not a tautology.

Exit code 0 means every board passed.  Uses a host gcc; the host shims live in
tools/dts/host/.
"""

import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DTC = os.path.join("tools", "dts", "dtc_lite.py")
HOST_INC = os.path.join("tools", "dts", "host")
KINC = os.path.join("kernelsrc", "include")
BUILD = os.path.join("build", "dtb_selftest")


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, encoding="utf-8",
                          errors="replace")


def find_gcc():
    for c in (os.environ.get("CC"), "gcc",
              r"D:/mingw/MinGW/bin/gcc.exe", r"C:/mingw/bin/gcc.exe"):
        if not c:
            continue
        try:
            subprocess.run([c, "--version"], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=True)
            return c
        except Exception:
            pass
    return None


def boards():
    out = []
    for name in sorted(os.listdir(os.path.join(ROOT, "dts"))):
        if name.endswith(".dts"):
            out.append(os.path.splitext(name)[0])
    return out


def mutate_and_run(exe, dtb):
    """Flip one letter of the property name 'compatible' in the strings block.

    Returns True when the mutated blob still passes (that would mean the
    comparison is blind), False when it is refused, None when the blob has no
    such string to mutate.
    """
    data = bytearray(open(os.path.join(ROOT, dtb), "rb").read())
    at = data.find(b"compatible")
    if at < 0:
        return None
    data[at + 1] ^= 0x20
    path = os.path.join(ROOT, BUILD, "mutated.dtb")
    with open(path, "wb") as f:
        f.write(bytes(data))
    r = run([exe, path])
    return r.returncode == 0


def main():
    gcc = find_gcc()
    if gcc is None:
        print("test_fdt: no host gcc found (set CC)")
        return 2

    py = [sys.executable]
    os.makedirs(os.path.join(ROOT, BUILD), exist_ok=True)
    bad = []

    for b in boards():
        dtb = os.path.join(BUILD, "%s.dtb" % b)
        r = run(py + [DTC, "--dts", "dts/%s.dts" % b,
                      "--out", dtb, "--format", "dtb"])
        if r.returncode != 0:
            print("test_fdt: dtc_lite failed for %s\n%s" % (b, r.stderr))
            bad.append(b)
            continue

        exe = os.path.join(ROOT, BUILD, "%s.exe" % b)
        cc = [gcc, "-std=c99", "-Wall", "-Wextra", "-Werror",
              "-DARKRTOS_USE_FDT=1",
              "-I", HOST_INC, "-I", KINC]
        r = run(cc + ["-o", exe, "tools/dts/test_fdt.c",
                      "kernelsrc/src/arkrtos_fdt.c",
                      "board/%s/arkrtos_dt.c" % b])
        if r.returncode != 0:
            print("test_fdt: %s build failed\n%s" % (b, r.stderr))
            bad.append(b)
            continue
        r = run([exe, dtb])
        if r.returncode != 0:
            print("test_fdt: %s self-test failed\n%s%s" % (b, r.stdout, r.stderr))
            bad.append(b)
            continue

        # 反假绿自检：把 strings 段里的属性名 compatible 改一个字母，
        # 再跑同一个可执行文件——它必须报错。若照样通过，说明逐节点比对
        # 根本没在比（例如两边都读同一个来源），这一档就变成永远通过。
        blind = mutate_and_run(exe, dtb)
        if blind is None:
            print("test_fdt: %s: no compatible string, mutation check skipped" % b)
        elif blind:
            print("test_fdt: %s: mutation self-check FAILED (flipped property "
                  "name still passed)" % b)
            bad.append(b)
            continue
        print("test_fdt: %s: %s%s" % (b, (r.stdout or "").strip(),
                                      "" if blind is None
                                      else " [mutation caught]"))

    if bad:
        print("test_fdt: %d board(s) failed: %s" % (len(bad), ", ".join(bad)))
        return 1
    print("test_fdt: all pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
