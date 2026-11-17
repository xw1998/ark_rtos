#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""exec/ld 签名门（ARKRTOS_USE_EXEC_SIGN）的宿主门禁。

为什么需要这一道
----------------
这条链的两头是两套完全独立的实现：

    构建/打包   tools/sign_payload.py      （Python hashlib + hmac）
    内核        kernelsrc/src/arkrtos_sign.c（手写 SHA-256 + HMAC）

只要一边把「覆盖多少字节、密钥怎么展开、tag 是追加在尾部还是头字段」记错，
结果就是「自己签的文件自己拒收」，而症状（装载报 -8/-11）看起来像文件坏了
或者密钥不对。两边都是**平时不参与默认构建**的代码：开关默认 0，一个编译器
都不经过。于是这里把两头都真的跑一遍：

  A. MAC 对拍：真内核 C 代码（宿主 gcc 编 test_payload_sign_host.c +
     arkrtos_sign.c）对一段确定性缓冲区算 tag；Python 侧用 sign_payload.py
     同一段数据复算，32 字节逐字节相同。流式入口与一次性入口也必须一致，
     且错 tag 必须被拒。
  B. 工具行为：sign 追加 32 字节且幂等、check 认得出、tamper 之后 check 必拒。
  C. 接线：把 arkrtos_elf.c / arkrtos_modld.c 预处理两遍——板级当前档（开）
     里必须出现 arkrtos_sign_payload；把板级那两行改成 0 之后必须一个都不出现
     （默认档零符号，不是「开着但没接线」）。
  D. 守卫：EXEC_SIGN=1 而 IMAGE_SIGN=0 必须编译期报错，且错误必须点名
     ARKRTOS_USE_EXEC_SIGN。
  E. 口径 pin：tag 长度 32、ELF 错误码 -8、模块错误码 -11 不许漂。

不覆盖：真机拒收/放行（那由 QEMU 上 elf_demo / mod_demo 的 tamper check
每轮开机驱动，读数见 docs/MMU进程模型与ELF装载.md）。宿主全绿 ≠ 板端已验证。

用法
----
    python tools/payload_sign_gate.py
    python tools/payload_sign_gate.py --gcc <host gcc> --la64-gcc <cross gcc>

退出码 0 = 全过；1 = 有一步不成立或找不到编译器（缺编译器是 FAIL，不是跳过）。
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

HOST_GCC = [r"D:\mingw\MinGW\bin\gcc.exe", r"C:\mingw64\bin\gcc.exe", "gcc", "cc"]
LA64_GCC = [r"D:/LoongIDE/la64-tool/bin/loongarch64-newlib-elf-gcc.exe"]

SIGN_C = os.path.join(ROOT, "kernelsrc", "src", "arkrtos_sign.c")
TEST_C = os.path.join(ROOT, "kernelsrc", "tests", "test_payload_sign_host.c")
TOOL_PY = os.path.join(ROOT, "tools", "sign_payload.py")
BOARD = os.path.join(ROOT, "board", "qemu_loongarch64", "arkrtos_board_config.h")

KEY = bytes(range(32))
KEY_DEF = "ARKRTOS_IMAGE_SIGN_KEY={" + ",".join("0x%02X" % b for b in KEY) + "}"

LA64_ARCH = ["-march=loongarch64", "-mabi=lp64d", "-mstrict-align"]
COMMON = ["-O0", "-Wall", "-ffreestanding", "-fno-common", "-nostdlib",
          "-nostartfiles", "-fno-builtin"]

failures = []


def fail(msg):
    failures.append(msg)


def find(name, cands, given):
    if given:
        return given
    for c in cands:
        if os.path.isabs(c):
            if os.path.isfile(c):
                return c
        else:
            w = shutil.which(c)
            if w:
                return w
    print("FAIL: 找不到 %s（缺编译器是 FAIL，不是跳过）" % name)
    sys.exit(1)


def load_tool():
    spec = importlib.util.spec_from_file_location("ark_sign_payload", TOOL_PY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def lcg_bytes(seed, n):
    x = seed
    out = bytearray()
    for _ in range(n):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        out.append((x >> 16) & 0xFF)
    return bytes(out)


def check_mac(gcc, tool, td):
    """A. 真内核 C 代码 vs Python hashlib，一次性与流式两条路。"""
    exe = os.path.join(td, "payload_host.exe" if os.name == "nt" else "payload_host")
    cmd = [gcc, "-std=gnu11"]
    if "mingw" in gcc.replace("\\", "/").lower():
        cmd.append("-B" + os.path.dirname(gcc))
    cmd += ["-DARKRTOS_USE_IMAGE_SIGN=1", "-DARKRTOS_USE_EXEC_SIGN=1",
            "-D" + KEY_DEF,
            "-I" + os.path.join(ROOT, "kernelsrc", "include"),
            "-I" + os.path.join(ROOT, "config"),
            "-o", exe, TEST_C, SIGN_C]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        fail("A 宿主编译失败：\n" + ((r.stdout or "") + (r.stderr or ""))[-600:])
        return

    r = subprocess.run([exe], cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        fail("A 宿主用例退出码 %d" % r.returncode)
        return
    out = r.stdout or ""
    m = re.search(r"^PAYLOAD ([0-9a-f]{64})$", out, re.M)
    s = re.search(r"^SHIFTED ([0-9a-f]{64})$", out, re.M)
    if not m or not s:
        fail("A 宿主用例没有打印两个 MAC：\n" + out[-400:])
        return

    buf = lcg_bytes(7, 4093)
    want = tool.mac(KEY, buf).hex()
    if m.group(1) != want:
        fail("A 一次性 MAC 不一致：kernel=%s python=%s" % (m.group(1)[:16], want[:16]))
    if s.group(1) != tool.mac(KEY, buf[1:]).hex():
        fail("A 偏移一位后的 MAC 不一致（覆盖范围口径漂了）")
    if "STREAM_MATCH 1" not in out:
        fail("A 流式入口与一次性入口算出的 tag 不一致")
    if "WRONG_TAG_REFUSED 1" not in out:
        fail("A 错的 tag 竟然被 check 接受（比较退化成恒真）")


def check_tool(tool, td):
    """B. sign / check / tamper 的行为。"""
    p = os.path.join(td, "blob.bin")
    payload = lcg_bytes(99, 1000)
    with open(p, "wb") as f:
        f.write(payload)

    tool.main(["sign", p, "--key-hex", KEY.hex()])
    data = open(p, "rb").read()
    if len(data) != 1000 + 32:
        fail("B sign 之后长度是 %d，应为 1032" % len(data))
    if data[-32:] != tool.mac(KEY, payload):
        fail("B sign 追加的 tag 与 hashlib 复算不一致")
    if tool.main(["check", p, "--key-hex", KEY.hex()]) != 0:
        fail("B 刚签过的文件 check 不通过")

    # 幂等：再签一次不许变成 +64
    tool.main(["sign", p, "--key-hex", KEY.hex()])
    if len(open(p, "rb").read()) != 1032:
        fail("B sign 不幂等（重复签名追加了第二个 tag）")

    # tamper 之后必须拒
    tool.main(["tamper", p, "--out", p + ".bad"])
    if tool.main(["check", p + ".bad", "--key-hex", KEY.hex()]) == 0:
        fail("B tamper 之后 check 仍然通过")

    # 换密钥必须拒
    other = bytes([b ^ 0x5A for b in KEY])
    if tool.main(["check", p, "--key-hex", other.hex()]) == 0:
        fail("B 换一把密钥后 check 仍然通过")


def check_wiring(cc, td):
    """C. 预处理两遍：板级开档必须有 payload 调用，关档必须一个都没有。"""
    key_dir = os.path.join(td, "key")
    os.makedirs(key_dir, exist_ok=True)
    with open(os.path.join(key_dir, "arkrtos_sign_key_local.h"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write("#ifndef ARKRTOS_SIGN_KEY_LOCAL_H\n#define ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_IMAGE_SIGN_KEY {" +
                ",".join("0x%02X" % b for b in KEY) + " }\n#endif\n")

    board_txt = open(BOARD, encoding="utf-8").read()
    needle = "#define ARKRTOS_USE_EXEC_SIGN        1"
    if needle not in board_txt:
        fail("C 板级里找不到 %r，这条判据已过期" % needle)
        return

    off_dir = os.path.join(td, "board_off")
    os.makedirs(off_dir, exist_ok=True)
    with open(os.path.join(off_dir, "arkrtos_board_config_off.h"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(board_txt.replace(needle,
                                  "#define ARKRTOS_USE_EXEC_SIGN        0", 1))

    def pp(board_hdr, board_dir, rel):
        cmd = [cc] + LA64_ARCH + COMMON + [
            "-E", "-DARKRTOS_SIGN_KEY_LOCAL=1",
            "-DARKRTOS_BOARD_CONFIG=\"%s\"" % board_hdr,
            "-include", board_hdr,
            "-I" + key_dir, "-I" + board_dir,
            "-Ikernelsrc/include", "-Iconfig",
            "-Iboard/qemu_loongarch64",
            "-Ikernelsrc/port/loongarch/la64",
        ] + [rel]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                           errors="replace")
        return r.returncode, (r.stdout or "") + (r.stderr or "")

    for rel in ("kernelsrc/src/arkrtos_elf.c", "kernelsrc/src/arkrtos_modld.c"):
        rc, out = pp("arkrtos_board_config.h", os.path.join(ROOT, "board", "qemu_loongarch64"), rel)
        if rc != 0:
            fail("C 开档预处理失败 %s：\n%s" % (rel, out[-400:]))
        elif "arkrtos_sign_payload" not in out:
            fail("C 开关开着但 %s 里没有任何 arkrtos_sign_payload 调用（没接线）" % rel)
        rc, out = pp("arkrtos_board_config_off.h", off_dir, rel)
        if rc != 0:
            fail("C 关档预处理失败 %s：\n%s" % (rel, out[-400:]))
        elif "arkrtos_sign_payload" in out:
            fail("C 关档下 %s 仍引用 arkrtos_sign_payload（默认档不是零变化）" % rel)


def check_guard(cc, td):
    """D. EXEC_SIGN=1 而 IMAGE_SIGN=0 必须编译期报错并点名。"""
    src = os.path.join(td, "guard.c")
    with open(src, "w", encoding="utf-8", newline="\n") as f:
        f.write('#include "arkrtos_features.h"\nint main(void){return 0;}\n')

    def build(defs):
        cmd = [cc]
        if "mingw" in cc.replace("\\", "/").lower():
            cmd.append("-B" + os.path.dirname(cc))
        cmd += COMMON + defs + [
            "-I" + os.path.join(ROOT, "kernelsrc", "include"),
            "-I" + os.path.join(ROOT, "config"), "-c", src,
            "-o", os.path.join(td, "guard.o")]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                           errors="replace")
        return r.returncode, (r.stdout or "") + (r.stderr or "")

    rc, out = build(["-DARKRTOS_USE_EXEC_SIGN=1", "-DARKRTOS_USE_IMAGE_SIGN=0"])
    if rc == 0:
        fail("D EXEC_SIGN=1 且 IMAGE_SIGN=0 竟然编过了（依赖守卫没生效）")
    elif "ARKRTOS_USE_EXEC_SIGN" not in out:
        fail("D 编不过但不是 EXEC_SIGN 那条 #error：\n" + out[-400:])

    rc, out = build(["-DARKRTOS_USE_EXEC_SIGN=1", "-DARKRTOS_USE_IMAGE_SIGN=1",
                     "-D" + KEY_DEF])
    if rc != 0:
        fail("D 两个开关都开、密钥给了，仍然编不过：\n" + out[-400:])


def check_pins():
    """E. 格式口径：32 字节 tag、错误码 -8 / -11。"""
    sign_h = open(os.path.join(ROOT, "kernelsrc", "include", "arkrtos_sign.h"),
                  encoding="utf-8", errors="replace").read()
    if "#define ARKRTOS_SIGN_LEN            (32u)" not in sign_h:
        fail("E ARKRTOS_SIGN_LEN 不是 32")
    elf_h = open(os.path.join(ROOT, "kernelsrc", "include", "arkrtos_elf.h"),
                 encoding="utf-8", errors="replace").read()
    if "ARKRTOS_ELF_ERR_SIGN        (-8)" not in elf_h:
        fail("E ELF 签名错误码不是 -8")
    mod_h = open(os.path.join(ROOT, "kernelsrc", "include", "arkrtos_modld.h"),
                 encoding="utf-8", errors="replace").read()
    if "ARKRTOS_MOD_ERR_SIGN        (-11)" not in mod_h:
        fail("E 模块签名错误码不是 -11")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--gcc", default="")
    ap.add_argument("--la64-gcc", default="")
    a = ap.parse_args()

    gcc = find("宿主 C 编译器", HOST_GCC, a.gcc)
    la64 = find("LoongArch64 交叉编译器", LA64_GCC, a.la64_gcc)
    tool = load_tool()

    td = tempfile.mkdtemp(prefix="ark_payload_sign_")
    try:
        check_mac(gcc, tool, td)
        check_tool(tool, td)
        check_wiring(la64, td)
        check_guard(gcc, td)
        check_pins()
    finally:
        shutil.rmtree(td, ignore_errors=True)

    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1

    print("PAYLOAD-SIGN-GATE OK: kernel MAC == python MAC（一次性+流式）/ "
          "sign 幂等 / tamper 必拒 / 开档接线且关档零符号 / 依赖守卫 / 口径未漂")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
