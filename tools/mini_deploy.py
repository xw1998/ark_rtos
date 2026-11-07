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
        start.c                     启动桩：MINISTART + __arkcc_init_globals
        mini_main.c                 main() -> MiniMain() -> 退出
        rt.c                        arkc 运行时（POSIX write / _exit）
        arkrtos.h                   arkc 版最小内核声明
        unistd.h                    POSIX 控制台子集（write/_exit）

    工程（--project，默认 example/mini_hello）
        目录里的全部文件，原名落到 NOR 根目录（makefile 也就叫 makefile）

为什么要扁平：`fs put` 走的是 littlefs 卷根，VFS 的 mkdir 只建目录、不改变
`fs put` 的落点；把 SDK 与工程都放在一层，路径最短、也最好在 `ls` 里核对。
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# (仓库内路径, 卷内名字)。卷内用短名不是偷懒：ark make 的一行值只有
# ARKMAKE_PATH_MAX=96 字节，四条长路径拼在 SRCS 里直接超限（板上实测
# "value is longer than the field that holds it"）。短名 + 相对路径是
# 那个上限唯一诚实的用法。
SDK_FILES = [
    ("kernelsrc/sdk/mini_sdk/arkrtos_mini_start_arkc.c", "/start.c"),
    ("kernelsrc/sdk/mini_sdk/arkrtos_mini_main.c",      "/mini_main.c"),
    ("kernelsrc/sdk/mini_sdk/arkc/arkrtos_mini_rt_arkc.c", "/rt.c"),
    ("kernelsrc/sdk/mini_sdk/arkc/arkrtos.h",           "/arkrtos.h"),
    ("kernelsrc/sdk/mini_sdk/arkc/unistd.h",            "/unistd.h"),
]


def collect(project_dir):
    """返回 [(本地绝对路径, 卷内路径)]；工程文件在前（先让 make 有东西读）。"""
    out = []
    for name in sorted(os.listdir(project_dir)):
        p = os.path.join(project_dir, name)
        if os.path.isfile(p):
            out.append((p, "/" + name))
    for rel, remote in SDK_FILES:
        p = os.path.join(ROOT, rel.replace("/", os.sep))
        out.append((p, remote))
    return out


def main():
    ap = argparse.ArgumentParser(description="Install the mini SDK + a project into the NOR volume")
    ap.add_argument("--port", required=True, help="串口，例如 COM3")
    ap.add_argument("--project", default=os.path.join("example", "mini_hello"),
                    help="工程目录（默认 example/mini_hello）")
    ap.add_argument("--only", help="只送名字里含该子串的文件（调试用）")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    project = args.project if os.path.isabs(args.project) else os.path.join(ROOT, args.project)
    if not os.path.isdir(project):
        print("project directory not found: %s" % project, file=sys.stderr)
        return 2

    items = collect(project)
    if args.only:
        items = [it for it in items if args.only in os.path.basename(it[0])]
    if not items:
        print("nothing to deploy", file=sys.stderr)
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
    print("现在可以在板上：cd /mnt/nor && make && mini run hello.svcm")
    return 0


if __name__ == "__main__":
    sys.exit(main())
