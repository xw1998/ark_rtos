#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把一个二进制文件转成 C 头里的字节数组。

为什么需要它
------------
QEMU 这套示例没有宿主文件注入通道：littlefs 卷是启动时在 RAM 里 format
出来的，宿主机上的 hello.elf 进不去。所以把示例程序编出来，转成内核镜像
里的一段只读数据，由演示任务写进 ramfs，再走真正的装载路径（VFS ->
arkrtos_elf_exec）。装载器读的还是文件，不是这个数组——数组只是把文件
送到板子上的手段。

用法
----
    python example/qemu_loongarch64/elf_blob.py \
        --input build/qemu_loongarch64/hello.elf \
        --output build/qemu_loongarch64/hello_blob.h \
        --name ark_example_hello

输出
----
    static const unsigned char ark_example_hello[] = { ... };
    #define ARK_EXAMPLE_HELLO_LEN  1234u
"""

import argparse
import os


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--name", required=True)
    args = ap.parse_args()

    data = open(args.input, "rb").read()
    if not data:
        raise SystemExit("elf_blob: %s is empty" % args.input)

    macro = args.name.upper() + "_LEN"
    lines = []
    lines.append("/* Generated from %s by elf_blob.py - do not edit. */"
                 % os.path.basename(args.input))
    lines.append("#ifndef ARK_ELF_BLOB_%s_H" % args.name.upper())
    lines.append("#define ARK_ELF_BLOB_%s_H" % args.name.upper())
    lines.append("")
    lines.append("static const unsigned char %s[] = {" % args.name)
    for off in range(0, len(data), 12):
        chunk = data[off:off + 12]
        lines.append("    " + ", ".join("0x%02X" % b for b in chunk) + ",")
    lines.append("};")
    lines.append("")
    lines.append("#define %s  %uu" % (macro, len(data)))
    lines.append("")
    lines.append("#endif /* ARK_ELF_BLOB_%s_H */" % args.name.upper())

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("elf_blob: %s -> %s (%u bytes)" % (args.input, args.output, len(data)))


if __name__ == "__main__":
    main()
