#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""镜像装载/重定位（arkrtos_loader）的宿主门禁。

编并跑 kernelsrc/tests/test_loader_host.c 与 kernelsrc/src/arkrtos_loader.c。
测试文件里自带内核桩（调度器、分区表、崩溃账本、日志、port 全部空实现），
所以只需要一份宿主 C 编译器，不需要板子、不需要 ARM 工具链。

为什么值得跑：重定位错一位不会报错，只会在板上跳到错地址执行——
这类"静默错答案"在串口上是看不出来的。这里钉死两件事：

  1. arkrtos_loader_check_header() 的拒绝顺序与边界
     魔数 / 兼容号 / type 越界与掩码 / image_size 为 0 与回绕 / entry 越界 /
     reloc_offset、count、编码版本、payload_offset / 表项对齐、越尾、升序 /
     ram_size 为 0 与超单块上限。
  2. arkrtos_loader_reloc_apply() 的补丁语义
     整字表项按 ROM/RAM 分别叠加 delta；MOVW+MOVT 解成 32 位值后重编码且
     保留指令位；块尾切断指令类表项时返回表项起点（submit）、idx 不动，
     下一块接着处理；off < buf_off 的表项跳过但指针仍前进。

明确没证明什么（不要把这门禁读成"装载链路已验证"）：
  - 没有覆盖 arkrtos_loader_load_image / 流式安装 / Flash 擦写 / 搬移 /
    卸载 / 签名校验 / CRC 计算 / 崩溃计数；
  - 没有上板跑过（本机无 DAP 探针），因此"能否真正启动"仍是未验证；
  - 表项编码 ARKRTOS_APP_RELOC_ENTRY 是 (off/2)<<2，奇数偏移无法表达，
    内核里 off%2 那条检查对真实镜像不可达，只有防御意义。

用法
----
    python tools/loader_host_gate.py
    python tools/loader_host_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe

退出码 0 = 全过；1 = 有用例失败或编不出来。
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TEST_C = os.path.join(ROOT, "kernelsrc", "tests", "test_loader_host.c")
LOADER_C = os.path.join(ROOT, "kernelsrc", "src", "arkrtos_loader.c")

GCC_FALLBACKS = [
    "gcc",
    r"D:\mingw\MinGW\bin\gcc.exe",
    r"C:\mingw64\bin\gcc.exe",
]

# 头文件搜索路径：和 ARM 侧编译同一套（板级配置决定分区与槽位上限）
INCLUDES = [
    os.path.join(ROOT, "kernelsrc", "include"),
    os.path.join(ROOT, "kernelsrc", "components", "mdk_trace"),
    os.path.join(ROOT, "board", "stm32f427"),
    os.path.join(ROOT, "config"),
]


def find_gcc(explicit):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for cand in GCC_FALLBACKS:
        if os.path.sep in cand:
            if os.path.exists(cand):
                return cand
        else:
            found = shutil.which(cand)
            if found:
                return found
    return None


def main():
    ap = argparse.ArgumentParser(description="Host gate for app image loader")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留生成的 exe 便于手工复跑")
    args = ap.parse_args()

    for path in (TEST_C, LOADER_C):
        if not os.path.exists(path):
            print("test source not found: %s" % path, file=sys.stderr)
            return 1

    cc = find_gcc(args.gcc)
    if not cc:
        print("no host C compiler found (use --gcc)", file=sys.stderr)
        return 1

    td = tempfile.mkdtemp(prefix="arkrtos-loader-gate-")
    exe = os.path.join(td, "loader_host.exe" if os.name == "nt" else "loader_host")
    cmd = [cc]
    if "mingw" in cc.replace("\\", "/").lower():
        cmd.append("-B" + os.path.dirname(cc))
    # -fno-asynchronous-unwind-tables：MinGW 的 .eh_frame 在只链一个模块时
    # 偶发 "bad reloc address"，关掉即可，与测试语义无关。
    cmd += ["-std=gnu11", "-fno-asynchronous-unwind-tables"]
    cmd += ["-I" + p for p in INCLUDES]
    cmd += ["-o", exe, TEST_C, LOADER_C]

    print("gcc: %s" % cc)
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode != 0:
        print("build failed", file=sys.stderr)
        return 1

    r = subprocess.run([exe], cwd=ROOT)
    ok = (r.returncode == 0)

    if not args.keep:
        shutil.rmtree(td, ignore_errors=True)

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
