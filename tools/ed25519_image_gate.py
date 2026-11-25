#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""非对称镜像签名（Ed25519）的宿主门禁：内核摘要 == 打包器摘要，且开关组合被强制。

为什么需要这一道
----------------
`ARKRTOS_USE_IMAGE_SIGN_ED25519` 默认 0，开了它之后 arkrtos_sign.c 走的是
另一条后端（验签，而不是 HMAC），而验签用的 32 字节摘要必须与
`tools/pack_app.py::calc_digest()` 逐字节相同——否则打包侧签出来的镜像
在设备上一定被拒，症状同样是"自己签的自己不认，看着像密钥问题"。

这条链的两头是两套独立实现：C 侧的 arkrtos_sign_cover_header/body 与
Python 侧的 calc_digest；C 侧的验签是 arkrtos_ed25519.c 的 curve25519 运算，
Python 侧是 cryptography（只在**重签黄金值时**才需要，本门禁日常不依赖它）。

本门禁做三件事：
  1. 编译真 arkrtos_sign.c + arkrtos_ed25519.c，跑 test_sign_ed25519_host.c：
     正例（黄金签名）必须过，改负载 / 改被覆盖头字段 / 改签名必须拒，
     改 crc32 必须照旧过（覆盖规则把该字段按 0 的正面证据）。
  2. 用打包器自己的 calc_digest() 复算同一幅镜像的摘要，要求 32 字节相同。
  3. 两个开关的依赖关系必须是**编译期**失败，不是链接期：
     ED25519 后端缺 USE_ED25519 / 缺 IMAGE_SIGN 都必须编不过并给出原因。

覆盖 / 不覆盖
-------------
覆盖：覆盖范围、SHA-256 摘要、Ed25519 验签、黄金签名与公钥的配对、开关依赖。
不覆盖：真机安装期拒收、公钥真实性（信任根属 SEC-G12）、侧信道、私钥保管。
宿主全绿不等于板端已验证。

黄金签名值怎么来的（重签时必须按这个来）
----------------------------------------
test_sign_ed25519_host.c 里的 T_SIG 是对该文件那条确定性缓冲区的摘要、
用种子 0x00..0x1f 的私钥签出来的。改了 T_HDR/T_RELOC_N/T_PAYLOAD 或覆盖
规则之后它必然失效，重签步骤：

    python -c "import importlib.util as u,sys;\\
spec=u.spec_from_file_location('pa','tools/pack_app.py');m=u.module_from_spec(spec);\\
spec.loader.exec_module(m);print(m.calc_digest(hdr,rel,pl).hex())"
    # 再用 cryptography 的 Ed25519PrivateKey.from_private_bytes(bytes(range(32)))
    # 对该摘要签名，把 64 字节回填到 T_SIG；公钥 = PUBKEY（见下）

用法
----
    python tools/ed25519_image_gate.py
    python tools/ed25519_image_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe

退出码 0 = 全过；1 = 不一致 / 编不过 / 找不到编译器。
"""

import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

TEST_C = os.path.join(ROOT, "kernelsrc", "tests", "test_sign_ed25519_host.c")
SIGN_C = os.path.join(ROOT, "kernelsrc", "src", "arkrtos_sign.c")
ED_C = os.path.join(ROOT, "kernelsrc", "src", "arkrtos_ed25519.c")
PACK_PY = os.path.join(ROOT, "tools", "pack_app.py")

# 私钥种子 0x00..0x1F 对应的公钥（T_SIG 就是这把私钥签的）。
PUBKEY = bytes.fromhex(
    "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8")

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
    """把打包器当模块用：直接借它的 calc_digest()，不在这里复制一份覆盖范围。"""
    spec = importlib.util.spec_from_file_location("ark_pack_app", PACK_PY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def base_cmd(cc):
    cmd = [cc]
    if "mingw" in cc.replace("\\", "/").lower():
        # 绝对路径调用 MinGW gcc 时，没有 -B 找不到 cc1/as，报
        # "CreateProcess: No such file or directory"。
        cmd.append("-B" + os.path.dirname(cc))
    # 头文件里的编译期断言用 `(cond) ? 1 : -1]` 数组技巧，gcc 会对它报
    # variably-modified；那是检查本身，不是被测代码的问题。
    cmd += ["-std=gnu11", "-Wno-variably-modified",
            "-I" + os.path.join(ROOT, "kernelsrc", "include"),
            "-I" + os.path.join(ROOT, "config")]
    return cmd


def pubkey_def():
    return "ARKRTOS_IMAGE_SIGN_PUBKEY={" + ",".join("0x%02X" % b for b in PUBKEY) + "}"


def check_switch_guards(cc):
    """开关依赖必须编译期失败：链接期才发现就变成"少编一个文件"的谜题。"""
    td = tempfile.mkdtemp(prefix="arkrtos-ed25519-guard-")
    cases = [
        ("ED25519 backend without the verifier",
         ["-DARKRTOS_USE_IMAGE_SIGN=1", "-DARKRTOS_USE_IMAGE_SIGN_ED25519=1",
          "-DARKRTOS_USE_ED25519=0", "-D" + pubkey_def()],
         "requires ARKRTOS_USE_ED25519"),
        ("ED25519 backend without IMAGE_SIGN",
         ["-DARKRTOS_USE_IMAGE_SIGN=0", "-DARKRTOS_USE_IMAGE_SIGN_ED25519=1",
          "-DARKRTOS_USE_ED25519=1", "-D" + pubkey_def()],
         "requires ARKRTOS_USE_IMAGE_SIGN"),
        ("ED25519 backend without a public key",
         ["-DARKRTOS_USE_IMAGE_SIGN=1", "-DARKRTOS_USE_IMAGE_SIGN_ED25519=1",
          "-DARKRTOS_USE_ED25519=1"],
         "requires ARKRTOS_IMAGE_SIGN_PUBKEY"),
    ]
    src = os.path.join(td, "guard.c")
    with open(src, "w", encoding="utf-8") as f:
        f.write('#include "arkrtos_features.h"\nint main(void) { return 0; }\n')
    try:
        for name, defs, want in cases:
            out = os.path.join(td, "guard.exe" if os.name == "nt" else "guard")
            r = subprocess.run(base_cmd(cc) + defs + ["-o", out, src],
                               cwd=ROOT, capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode == 0:
                print("ED25519-GATE FAIL: %s compiled (should be refused)" % name,
                      file=sys.stderr)
                return False
            if want not in txt:
                print("ED25519-GATE FAIL: %s failed for the wrong reason" % name,
                      file=sys.stderr)
                print(txt[-400:], file=sys.stderr)
                return False
        print("ok: switch guards refused 3 impossible combinations")
        return True
    finally:
        shutil.rmtree(td, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description="Host gate for the Ed25519 image signature")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留生成的 exe 与镜像便于复跑")
    args = ap.parse_args()

    for path in (TEST_C, SIGN_C, ED_C, PACK_PY):
        if not os.path.exists(path):
            print("missing: %s" % path, file=sys.stderr)
            return 1

    cc = find_gcc(args.gcc)
    if not cc:
        print("no host C compiler found (use --gcc)", file=sys.stderr)
        return 1

    pack = load_pack_app()

    if not check_switch_guards(cc):
        return 1

    td = tempfile.mkdtemp(prefix="arkrtos-ed25519-gate-")
    try:
        exe = os.path.join(td, "sign_ed25519.exe" if os.name == "nt" else "sign_ed25519")
        img = os.path.join(td, "image.bin")

        cmd = base_cmd(cc) + [
            "-DARKRTOS_USE_IMAGE_SIGN=1",
            "-DARKRTOS_USE_IMAGE_SIGN_ED25519=1",
            "-DARKRTOS_USE_ED25519=1",
            "-D" + pubkey_def(),
            "-o", exe, TEST_C, SIGN_C, ED_C]
        print("gcc: %s" % cc)
        r = subprocess.run(cmd, cwd=ROOT)
        if r.returncode != 0:
            print("ED25519-GATE FAIL: build failed", file=sys.stderr)
            return 1

        r = subprocess.run([exe, img], cwd=ROOT, capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or "SIGN-ED25519-HOST OK" not in txt:
            print("ED25519-GATE FAIL: harness rc=%d" % r.returncode, file=sys.stderr)
            print(txt[-800:], file=sys.stderr)
            return 1

        kernel_digest = None
        dims = None
        for line in txt.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] == "DIGEST":
                kernel_digest = parts[1]
            elif len(parts) == 5 and parts[0] == "IMAGE":
                dims = tuple(int(x) for x in parts[1:])
        if kernel_digest is None or dims is None:
            print("ED25519-GATE FAIL: harness output has no DIGEST/IMAGE line",
                  file=sys.stderr)
            return 1

        hdr_len, reloc_n, reloc_len, payload = dims
        raw = open(img, "rb").read()
        if len(raw) != hdr_len + reloc_len + payload:
            print("ED25519-GATE FAIL: image length %d != %d+%d+%d"
                  % (len(raw), hdr_len, reloc_len, payload), file=sys.stderr)
            return 1
        hdr = raw[:hdr_len]
        rel = raw[hdr_len:hdr_len + reloc_len]
        pl = raw[hdr_len + reloc_len:]
        pack_digest = pack.calc_digest(hdr, rel, pl).hex()

        if pack_digest != kernel_digest:
            print("ED25519-GATE FAIL: kernel digest != packer digest", file=sys.stderr)
            print("  kernel %s" % kernel_digest, file=sys.stderr)
            print("  packer %s" % pack_digest, file=sys.stderr)
            return 1
        print("ok: kernel digest == packer digest (%s)" % kernel_digest[:16])

        checks = [l for l in txt.splitlines() if l.startswith("SIGN-ED25519-HOST OK")]
        print("ED25519-GATE OK (%s)" % (checks[0] if checks else "harness ok"))
        return 0
    finally:
        if args.keep:
            print("kept: %s" % td)
        else:
            shutil.rmtree(td, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
