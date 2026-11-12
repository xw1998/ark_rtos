#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""镜像签名（HMAC-SHA256）的宿主门禁：内核 MAC == 打包器 MAC。

为什么需要这一道
----------------
`ARKRTOS_USE_IMAGE_SIGN` 默认 0，`arkrtos_sign.c` 平时不参与任何编译，
也没有任何一门测试跑过它的算法。签名链的两头分别是：

    打包器  tools/pack_app.py::calc_sign()      （写 signature）
    内核    kernelsrc/src/arkrtos_sign.c        （安装期复算）

只要两头对"哪些字段按 0、重定位表与负载算多长"有一处不同，结果就是
"自己签的镜像被自己拒收"，而且症状（安装报签名不符）看起来像密钥问题。
本门禁用 gcc 编译**真实的** arkrtos_sign.c，对一个确定性缓冲区算 MAC，
再用打包器自己的 calc_sign() 复算，要求 32 字节逐字节相同。

覆盖 / 不覆盖
-------------
覆盖：字段清零口径、MAC 覆盖范围、SHA-256/HMAC 实现、密钥读取方式。
不覆盖：真机安装期拒收、密钥如何保管、非对称签名（未实现）。
宿主全绿不等于板端已验证。

用法
----
    python tools/sign_host_gate.py
    python tools/sign_host_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe

退出码 0 = 一致；1 = 不一致 / 编不过 / 找不到编译器。
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

TEST_C = os.path.join(ROOT, "kernelsrc", "tests", "test_sign_host.c")
SIGN_C = os.path.join(ROOT, "kernelsrc", "src", "arkrtos_sign.c")
PACK_PY = os.path.join(ROOT, "tools", "pack_app.py")

# 与 test_sign_host.c 里的 static key 必须一致：0x00..0x1F
KEY = bytes(range(32))

GCC_FALLBACKS = [
    r"D:\mingw\MinGW\bin\gcc.exe",
    r"C:\mingw64\bin\gcc.exe",
    "gcc",
    "cc",
]


def find_gcc(explicit):
    cands = [explicit] if explicit else GCC_FALLBACKS
    for cand in cands:
        if not cand:
            continue
        if os.path.isabs(cand):
            if os.path.exists(cand):
                return cand
        else:
            found = shutil.which(cand)
            if found:
                return found
    return None


def load_pack_app():
    """把打包器当模块用：直接借它的 calc_sign()，不在这里复制一份覆盖范围。"""
    import importlib.util
    spec = importlib.util.spec_from_file_location("ark_pack_app", PACK_PY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    ap = argparse.ArgumentParser(description="Host gate for the image signature")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留生成的 exe 与镜像便于复跑")
    args = ap.parse_args()

    for path in (TEST_C, SIGN_C, PACK_PY):
        if not os.path.exists(path):
            print("missing: %s" % path, file=sys.stderr)
            return 1

    cc = find_gcc(args.gcc)
    if not cc:
        print("no host C compiler found (use --gcc)", file=sys.stderr)
        return 1

    pack = load_pack_app()

    td = tempfile.mkdtemp(prefix="arkrtos-sign-gate-")
    exe = os.path.join(td, "sign_host.exe" if os.name == "nt" else "sign_host")
    img = os.path.join(td, "image.bin")
    keydef = "ARKRTOS_IMAGE_SIGN_KEY={" + ",".join("0x%02X" % b for b in KEY) + "}"

    cmd = [cc]
    if "mingw" in cc.replace("\\", "/").lower():
        cmd.append("-B" + os.path.dirname(cc))
    # 头文件里的编译期断言用的是 `(cond) ? 1 : -1]` 数组技巧，宿主 gcc 会对
    # 它报 variably-modified；那是编译期检查本身，不是被测代码的问题。
    cmd += ["-std=gnu11", "-Wno-variably-modified",
            "-DARKRTOS_USE_IMAGE_SIGN=1",
            "-D" + keydef,
            "-I" + os.path.join(ROOT, "kernelsrc", "include"),
            "-I" + os.path.join(ROOT, "config"),
            "-o", exe, TEST_C, SIGN_C]

    print("gcc: %s" % cc)
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode != 0:
        print("SIGN-GATE FAIL: build failed", file=sys.stderr)
        return 1

    r = subprocess.run([exe, img], cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        print("SIGN-GATE FAIL: harness rc=%d" % r.returncode, file=sys.stderr)
        print((r.stdout or "") + (r.stderr or ""), file=sys.stderr)
        return 1

    got = None
    dims = None
    for line in (r.stdout or "").splitlines():
        if line.startswith("MAC "):
            got = line[4:].strip().lower()
        elif line.startswith("IMAGE "):
            dims = [int(x) for x in line[6:].split()]

    if got is None or dims is None:
        print("SIGN-GATE FAIL: harness produced no MAC/IMAGE line", file=sys.stderr)
        print(r.stdout or "", file=sys.stderr)
        return 1

    buf = open(img, "rb").read()
    hdr_len, reloc_n, reloc_len, payload_len = dims
    total = hdr_len + reloc_len + payload_len
    if len(buf) != total:
        print("SIGN-GATE FAIL: image is %d bytes, expected %d" % (len(buf), total),
              file=sys.stderr)
        return 1
    if reloc_n * 4 != reloc_len:
        print("SIGN-GATE FAIL: reloc_count %d != %d bytes" % (reloc_n, reloc_len),
              file=sys.stderr)
        return 1

    header = buf[:hdr_len]
    reloc = buf[hdr_len:hdr_len + reloc_len]
    payload = buf[hdr_len + reloc_len:]

    want = pack.calc_sign(header, reloc, payload, KEY).hex()

    if got != want:
        print("SIGN-GATE FAIL: kernel MAC != packer MAC", file=sys.stderr)
        print("  kernel: %s" % got, file=sys.stderr)
        print("  packer: %s" % want, file=sys.stderr)
        return 1

    print("ok: kernel arkrtos_sign_image() == tools/pack_app.calc_sign()")
    print("SIGN-GATE OK (mac=%s...)" % got[:16])

    if not args.keep:
        shutil.rmtree(td, ignore_errors=True)
    else:
        print("kept: %s" % td)

    return 0


if __name__ == "__main__":
    sys.exit(main())
