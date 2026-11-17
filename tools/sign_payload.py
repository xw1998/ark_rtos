#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""给「整份文件」追加 32 字节 HMAC-SHA256 尾签名（exec / ld 的签名门）。

为什么用「尾部追加」而不是改格式
--------------------------------
ELF 可执行文件（ET_EXEC）与目标文件（ET_REL）都是按 `e_phoff` / `p_offset` /
`e_shoff` / 节头表读的，文件末尾多出 32 字节既不在任何段里、也不在任何节里，
装载器天然看不见它们。于是签名不需要动任何既有格式，只要「签名之后再生成
blob /（将来）拷进 Flash」这一步而已。

覆盖范围与内核口径完全一致
--------------------------
tag = HMAC-SHA256(key, payload[0 : len-32])
  - 装入档：`ARKRTOS_USE_EXEC_SIGN=1`（需要 `ARKRTOS_USE_IMAGE_SIGN=1`）
  - 内核两端：`arkrtos_sign_payload()` / `arkrtos_sign_payload_begin/update/end()`
    （kernelsrc/src/arkrtos_sign.c），与 App 镜像签名共用同一把预共享密钥。
  - 宿主对拍：`tools/payload_sign_gate.py` 用真内核 C 代码复算，逐字节比对。

用法
----
    # 生成（就地覆盖，或 --out 另存）
    python tools/sign_payload.py sign build/qemu_loongarch64/hello.elf
    python tools/sign_payload.py sign hello.elf --out hello.signed.elf

    # 校验（0=通过，1=不通过）
    python tools/sign_payload.py check build/qemu_loongarch64/hello.elf

    # 换密钥（默认 0x00..0x1F，与 example/*/Makefile 里生成的
    # arkrtos_sign_key_local.h、tools/sign_host_gate.py 的 KEY 同值）
    python tools/sign_payload.py sign hello.elf --key-header build/.../arkrtos_sign_key_local.h
    python tools/sign_payload.py sign hello.elf --key-hex 000102...1f

注意
----
`sign` 是幂等的：文件末尾已经是本密钥下的正确 tag 时，原样通过、不再追加
第二个 tag。这一点很重要，因为 make 可能重复跑同一条规则。
"""

import argparse
import hashlib
import hmac
import os
import re
import sys

TAG_LEN = 32
DEFAULT_KEY = bytes(range(32))


def load_key(args):
    if args.key_hex:
        raw = bytes.fromhex(args.key_hex.strip())
        if len(raw) != 32:
            raise SystemExit("sign_payload: --key-hex must be 32 bytes (got %d)"
                             % len(raw))
        return raw
    if args.key_header:
        with open(args.key_header, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        m = re.search(r"ARKRTOS_IMAGE_SIGN_KEY\s*\{([^}]*)\}", text)
        if not m:
            raise SystemExit("sign_payload: %s has no ARKRTOS_IMAGE_SIGN_KEY {..}"
                             % args.key_header)
        vals = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", m.group(1))]
        if len(vals) != 32:
            raise SystemExit("sign_payload: key header gave %d bytes, need 32"
                             % len(vals))
        return bytes(vals)
    return DEFAULT_KEY


def mac(key, payload):
    return hmac.new(key, payload, hashlib.sha256).digest()


def looks_signed(data, key):
    """末尾 32 字节是否已经是本密钥下对前缀的正确 tag。"""
    if len(data) <= TAG_LEN:
        return False
    body = data[:-TAG_LEN]
    return hmac.compare_digest(mac(key, body), data[-TAG_LEN:])


def cmd_sign(args):
    key = load_key(args)
    with open(args.file, "rb") as f:
        data = f.read()
    if not data:
        raise SystemExit("sign_payload: %s is empty" % args.file)

    if looks_signed(data, key):
        # 幂等：已经签过就不再追加。放到 --out 时也要写一份完整的。
        out_data = data
        note = "already signed, unchanged"
    else:
        tag = mac(key, data)
        out_data = data + tag
        note = "payload %d B + tag %d B = %d B" % (len(data), TAG_LEN, len(out_data))

    out = args.out or args.file
    with open(out, "wb") as f:
        f.write(out_data)
    print("sign_payload: %s -> %s (%s)" % (args.file, out, note))
    return 0


def cmd_check(args):
    key = load_key(args)
    with open(args.file, "rb") as f:
        data = f.read()
    if len(data) <= TAG_LEN:
        print("sign_payload: %s too short to carry a tag" % args.file)
        return 1
    if not looks_signed(data, key):
        print("sign_payload: %s: tag missing or does not match" % args.file)
        return 1
    print("sign_payload: %s: tag ok (payload %d B)" % (args.file, len(data) - TAG_LEN))
    return 0


def cmd_tamper(args):
    """把 tag 的最后一字节翻掉：给「上板必须被拒」准备一份反例文件。

    只用于自测/门禁，不参与正常构建产物；它保证「拒绝」这件事是被驱动过的，
    而不是因为文件根本不存在或格式坏了才失败。
    """
    with open(args.file, "rb") as f:
        data = bytearray(f.read())
    if len(data) <= TAG_LEN:
        raise SystemExit("sign_payload: %s too short to tamper" % args.file)
    data[-1] ^= 0xFF
    out = args.out or args.file
    with open(out, "wb") as f:
        f.write(data)
    print("sign_payload: %s -> %s (last byte of tag flipped)" % (args.file, out))
    return 0


def cmd_tag(args):
    key = load_key(args)
    with open(args.file, "rb") as f:
        data = f.read()
    print(mac(key, data).hex())
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="action", required=True)

    for name, fn in (("sign", cmd_sign), ("check", cmd_check), ("tag", cmd_tag),
                     ("tamper", cmd_tamper)):
        p = sub.add_parser(name)
        p.add_argument("file")
        p.add_argument("--out", help="sign 的输出路径（默认就地覆盖）")
        p.add_argument("--key-hex", help="32 字节密钥的 hex（64 个字符）")
        p.add_argument("--key-header",
                       help="从生成的 arkrtos_sign_key_local.h 里读密钥")
        p.set_defaults(func=fn)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
