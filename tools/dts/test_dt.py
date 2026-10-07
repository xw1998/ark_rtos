#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Host self-test for the Ark RTOS device tree toolchain.

Steps
  1. regenerate board/<chip>/arkrtos_dt.c from dts/<chip>.dts (both boards);
  2. build tools/dts/test_dt.c against the F427 data and run it: the API
     must answer exactly what the DTS described (enabled-only lookup,
     reg windows, phandle resolutions, missing-property behaviour);
  3. compile-only the F401 data so a second board is proven to build too.

Exit code 0 means all of the above passed.  Uses a host gcc; the
ARKRTOS_* host shims live in tools/dts/host/.
"""

import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DTC = os.path.join("tools", "dts", "dtc_lite.py")
HOST_INC = os.path.join("tools", "dts", "host")
KINC = os.path.join("kernelsrc", "include")
BOARDS = ["stm32f427", "stm32f401"]


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, **kw)


def find_gcc():
    cand = [os.environ.get("CC"), "gcc",
            r"D:/mingw/MinGW/bin/gcc.exe", r"C:/mingw/bin/gcc.exe"]
    for c in cand:
        if not c:
            continue
        try:
            subprocess.run([c, "--version"], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=True)
            return c
        except Exception:
            pass
    return None


def main():
    gcc = find_gcc()
    if gcc is None:
        print("test_dt: no host gcc found (set CC)")
        return 2

    py = [sys.executable]
    for b in BOARDS:
        r = run(py + [DTC, "--dts", "dts/%s.dts" % b,
                      "--out", "board/%s/arkrtos_dt.c" % b])
        if r.returncode != 0:
            print("test_dt: dtc_lite failed for %s" % b)
            return 1

    exe = os.path.join(ROOT, "build", "dt_selftest.exe")
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    cc = [gcc, "-std=c99", "-Wall", "-Wextra", "-Werror",
          "-I", HOST_INC, "-I", KINC]
    r = run(cc + ["-o", exe, "tools/dts/test_dt.c",
                  "board/stm32f427/arkrtos_dt.c"])
    if r.returncode != 0:
        print("test_dt: F427 self-test build failed")
        return 1
    r = run([exe])
    if r.returncode != 0:
        return 1

    # second board: prove the generated data also compiles cleanly.
    r = run(cc + ["-c", "-o", os.path.join(ROOT, "build", "dt_f401.o"),
                  "board/stm32f401/arkrtos_dt.c"])
    if r.returncode != 0:
        print("test_dt: F401 data failed to compile")
        return 1
    print("test_dt: all pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
