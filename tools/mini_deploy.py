#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把小程序的 SDK 与一个工程装进板上的 NOR 卷（挂载点 /mnt/nor）。

板子上没有交叉工具链，也没有网络：工程文件只能用串口送进去。这一步就是
"把源码 scp 到设备上"，只是这里的传输是 `fs put`（内核的分块写 flash 命令，
见 tools/fs_put.py）。装一次，之后每次构建都在板上 `make`，不再碰主机。

用法
----
    python tools/mini_deploy.py --port COM3
    python tools/mini_deploy.py --port COM3 --project example/mini_hello
    python tools/mini_deploy.py --port COM3 --only hello.c

部署清单
--------
    SDK（源头在 kernelsrc/sdk/mini_sdk，落到 NOR 根目录，保持扁平）
        start.c                     启动桩：MINISTART -> __arkcc_init_globals -> main()
        rt.c                        arkc 运行时（POSIX write / _exit / printf 取参桩）
        arkrtos.h                   arkc 版最小内核声明
        unistd.h                    POSIX 控制台子集（write / _exit）
        stdio.h                     printf / puts / putchar
        stdarg.h                    va_list / va_start / va_arg / va_end

    工程（--project，默认 example/mini_hello）
        目录里的全部文件，落到 **/mnt/nor/hello/** 子目录，原名不变

为什么工程要进子目录、而 SDK 留在根：SDK 是"这套编译器运行时"，一份；工程是
"你要编的东西"，可以有很多份。放到子目录之后同一条 NOR 上能并排放几个工程
（hello/、blink/、…），各自的 makefile 用绝对路径指回根上的 SDK 就能编。

fs put 的落点是 littlefs 卷根，`..` 之类不参与解析，所以子目录必须先建成
真目录：`mkdir /<dir>` 走的是 littlefs 的 mkdir（VFS 里就是 mkdir /mnt/nor/hello）。
"""

import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# 工程在卷里的子目录名（默认 hello/，与 docs 里的演示命令一致）。--remote-dir
# 可以改成别的名字，这样同一块卷上能并排放几个工程（hello/、pf/、…）。
PROJECT_DIR_REMOTE = "/hello"

# (仓库内路径, 卷内名字)。卷内用短名不是偷懒：ark make 的一行值只有
# ARKMAKE_PATH_MAX=96 字节，几条长路径拼在 SRCS 里直接超限（板上实测
# "value is longer than the field that holds it"）。短名 + 绝对路径是
# 那个上限唯一诚实的用法。
SDK_FILES = [
    ("kernelsrc/sdk/mini_sdk/arkrtos_mini_start_arkc.c", "/start.c"),
    ("kernelsrc/sdk/mini_sdk/arkc/arkrtos_mini_rt_arkc.c", "/rt.c"),
    ("kernelsrc/sdk/mini_sdk/arkc/arkrtos.h",           "/arkrtos.h"),
    ("kernelsrc/sdk/mini_sdk/arkc/unistd.h",            "/unistd.h"),
    ("kernelsrc/sdk/mini_sdk/arkc/stdio.h",             "/stdio.h"),
    ("kernelsrc/sdk/mini_sdk/arkc/stdarg.h",            "/stdarg.h"),
]

# 老的部署形态留下的文件：SDK 不再需要它（arkc 档的入口就是工程的 main）。
STALE_FILES = ["/mini_main.c"]


def collect(project_dir, remote_dir):
    """返回 [(本地绝对路径, 卷内路径)]；工程文件在前（先让 make 有东西读）。"""
    out = []
    for name in sorted(os.listdir(project_dir)):
        p = os.path.join(project_dir, name)
        if os.path.isfile(p):
            out.append((p, remote_dir + "/" + name))
    for rel, remote in SDK_FILES:
        p = os.path.join(ROOT, rel.replace("/", os.sep))
        out.append((p, remote))
    return out


def prepare_volume(port, baud, dry_run, remote_dir):
    """在卷上把工程的子目录建出来，并清掉老形态留下的文件。

    为什么要自己开一次串口：fs put 只负责"把字节写进一个已存在的路径"，建目录
    与删除都得走 shell 命令。这里不做重试与解析，命令失败只往日志里写一行——
    目录已存在是正常情况（第二次部署），不能因此中断。
    """
    if dry_run:
        print("(dry-run) mkdir %s ; rm %s" % (remote_dir, " ".join(STALE_FILES)))
        return 0
    try:
        import serial
    except ImportError:
        sys.stderr.write("pyserial is not installed (pip install pyserial)\n")
        return 2
    ser = serial.Serial(port, baud, timeout=0.2)
    try:
        time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(("mkdir /mnt/nor%s\r\n" % remote_dir).encode())
        time.sleep(0.6)
        for stale in STALE_FILES:
            ser.write(("rm /mnt/nor%s\r\n" % stale).encode())
            time.sleep(0.4)
        log = b""
        end = time.time() + 0.5
        while time.time() < end:
            d = ser.read(4096)
            if d:
                log += d
        text = log.decode("utf-8", "replace")
        for line in text.splitlines():
            if ("mkdir" in line) or ("removed" in line) or ("FAILED" in line):
                print("   volume: %s" % line.strip())
        return 0
    finally:
        ser.close()


def main():
    ap = argparse.ArgumentParser(description="Install the mini SDK + a project into the NOR volume")
    ap.add_argument("--port", required=True, help="串口，例如 COM3")
    ap.add_argument("--project", default=os.path.join("example", "mini_hello"),
                    help="工程目录（默认 example/mini_hello）")
    ap.add_argument("--only", help="只送名字里含该子串的文件（调试用）")
    ap.add_argument("--remote-dir", default=PROJECT_DIR_REMOTE,
                    help="工程落到卷里的哪个子目录（默认 %s）" % PROJECT_DIR_REMOTE)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    project = args.project if os.path.isabs(args.project) else os.path.join(ROOT, args.project)
    if not os.path.isdir(project):
        print("project directory not found: %s" % project, file=sys.stderr)
        return 2

    items = collect(project, args.remote_dir)
    if args.only:
        items = [it for it in items if args.only in os.path.basename(it[0])]
    if not items:
        print("nothing to deploy", file=sys.stderr)
        return 2

    if prepare_volume(args.port, 115200, args.dry_run, args.remote_dir) != 0:
        return 2

    put = os.path.join(HERE, "fs_put.py")
    failed = 0
    for local, remote in items:
        size = os.path.getsize(local)
        print("==> %s (%d B) -> %s" % (os.path.basename(local), size, remote))
        if args.dry_run:
            continue
        cmd = [sys.executable, put, local, "--port", args.port, "--path", remote]
        # fs_put 会自己开串口、自己等设备的 ready 标记
        rc = subprocess.call(cmd, env=dict(os.environ, MSYS_NO_PATHCONV="1"))
        if rc != 0:
            print("FAILED: %s" % local, file=sys.stderr)
            failed += 1

    print("\n%d file(s) deployed, %d failed" % (len(items) - failed, failed))
    if failed:
        return 1
    print("现在可以在板上：cd /mnt/nor%s && make && mini run hello.svcm" % args.remote_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
