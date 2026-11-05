# -*- coding: utf-8 -*-
"""把 arkvim 源仓库同步进 Ark RTOS 的 components/arkvim。

为什么是复制而不是 submodule：克隆 Ark RTOS 的人不需要第二步就能编译，
也不需要网络。代价是两处副本会漂移，所以本脚本是唯一入口，并且：
  - 只复制 `src/` 里的 .c/.h（上游 src/ 下还躺着构建出来的 .o，别抄进来）
  - **不复制** `arkvim_fileio_posix.c`：那是主机端 POSIX 后端，与
    `src/arkvim_fileio_vfs.c` 实现同一组后端钩子；两个同时进构建路径会
    撞符号，而且 POSIX 那套在裸机上根本不成立
  - **不碰组件根目录里的三个接线文件**（arkvim_port_arkrtos.c、
    arkvim_fileio_vfs.c/.h、arkvim_shell_cmd.c/.h）：它们是 Ark RTOS 侧
    的适配层，不属于上游，同步只允许改 `src/`
  - 写 VENDOR.md 记录来源 commit 与文件清单
  - 默认 dry-run 提示差异，--apply 才落盘，--check 供 CI 判断是否漂移

用法：
  py -3 tools/sync_arkvim.py                 # 看差异
  py -3 tools/sync_arkvim.py --apply         # 落盘
  py -3 tools/sync_arkvim.py --check         # 漂移则退出码 2
  py -3 tools/sync_arkvim.py --src D:\\path\\to\\arkvim
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from datetime import date

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DST_ROOT = os.path.join(REPO, "kernelsrc", "components", "arkvim")
# 同步只写这里；组件根目录的接线文件不在同步范围内
DST_SRC = os.path.join(DST_ROOT, "src")

# 源仓库目录 -> 目标目录（相对 DST_ROOT）
COPY_MAP = [
    ("src", "src"),
]
# 不复制：主机端 POSIX 后端（与 vfs 后端撞同一组钩子）
EXCLUDE_NAMES = {"arkvim_fileio_posix.c"}
EXCLUDE_DIRS = {"tests", "demo"}

# 组件根目录里 Ark RTOS 侧的接线文件（同步不许动，只做存在性自检）
WIRING = [
    "arkvim_port_arkrtos.c",
    "arkvim_fileio_vfs.c",
    "arkvim_fileio_vfs.h",
    "arkvim_shell_cmd.c",
    "arkvim_shell_cmd.h",
]


def default_src():
    cand = os.path.join(os.path.dirname(REPO), "arkvim")
    return os.environ.get("ARKVIM_SRC", cand)


def git_head(src):
    try:
        out = subprocess.check_output(
            ["git", "-C", src, "rev-parse", "HEAD"],
            stderr=subprocess.DEVNULL, text=True)
        return out.strip()
    except Exception:
        return "unknown"


def git_dirty(src):
    try:
        out = subprocess.check_output(
            ["git", "-C", src, "status", "--porcelain"],
            stderr=subprocess.DEVNULL, text=True)
        return out.strip()
    except Exception:
        return ""


def collect(src):
    """返回 {目标相对路径: 源绝对路径}"""
    files = {}
    for rel_src, rel_dst in COPY_MAP:
        base = os.path.join(src, rel_src)
        if not os.path.isdir(base):
            print("缺少源目录：%s" % base)
            return None
        for root, dirs, names in os.walk(base):
            dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
            for name in sorted(names):
                if name in EXCLUDE_NAMES:
                    continue
                if not name.endswith((".c", ".h")):
                    continue
                full = os.path.join(root, name)
                sub = os.path.relpath(full, base)
                files[os.path.join(rel_dst, sub).replace(os.sep, "/")] = full
    return files


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=default_src())
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    src = os.path.abspath(args.src)
    if not os.path.isdir(src):
        print("找不到 arkvim 源仓库：%s" % src)
        print("用 --src 指定，或设环境变量 ARKVIM_SRC")
        return 2
    if not os.path.isfile(os.path.join(src, "src", "arkvim.h")):
        print("%s 看起来不是 arkvim 仓库（缺 src/arkvim.h）" % src)
        return 2

    want = collect(src)
    if want is None:
        return 2

    have = set()
    if os.path.isdir(DST_SRC):
        for root, dirs, names in os.walk(DST_SRC):
            dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
            for name in names:
                if name in EXCLUDE_NAMES:
                    continue
                if not name.endswith((".c", ".h")):
                    continue
                rel = os.path.relpath(os.path.join(root, name), DST_ROOT)
                have.add(rel.replace(os.sep, "/"))

    added, changed, same = [], [], []
    for rel in sorted(want):
        dst = os.path.join(DST_ROOT, rel.replace("/", os.sep))
        norm = rel.replace(os.sep, "/")
        if norm not in have:
            added.append(norm)
        elif sha(dst) != sha(want[rel]):
            changed.append(norm)
        else:
            same.append(norm)
    removed = sorted(have - set(r.replace(os.sep, "/") for r in want))

    missing_wiring = [w for w in WIRING
                      if not os.path.isfile(os.path.join(DST_ROOT, w))]

    print("源：%s" % src)
    print("目标：%s" % DST_ROOT)
    for tag, items in (("新增", added), ("更新", changed), ("删除", removed)):
        for it in items:
            print("  %s  %s" % (tag, it))
    print("未变 %d 个 / 新增 %d / 更新 %d / 删除 %d"
          % (len(same), len(added), len(changed), len(removed)))
    if missing_wiring:
        print("警告：组件根目录缺少接线文件：%s" % ", ".join(missing_wiring))

    if args.check:
        if added or changed or removed:
            print("组件与源仓库不一致：跑 --apply 同步")
            return 2
        print("一致")
        return 0

    if not args.apply:
        print("（dry-run，未落盘；加 --apply 执行）")
        return 0

    for rel in removed:
        os.remove(os.path.join(DST_ROOT, rel.replace("/", os.sep)))
    for rel in added + changed:
        dst = os.path.join(DST_ROOT, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(want[rel], dst)

    head = git_head(src)
    dirty = git_dirty(src)
    lines = [
        "# arkvim（同步副本，勿手改）",
        "",
        "本目录由 `tools/sync_arkvim.py` 从 arkvim 源仓库复制而来。",
        "**不要改 `src/` 里的代码**：改动会在下次同步时被覆盖。改源仓库，再跑同步。",
        "组件根目录的接线文件属于 Ark RTOS 侧，同步不碰，可以直接改。",
        "",
        "| 项 | 值 |",
        "|---|---|",
        "| 来源 | `%s` |" % src.replace("\\", "/"),
        "| 源 commit | `%s` |" % head,
        "| 工作区 | %s |" % ("有未提交改动" if dirty else "干净"),
        "| 同步日期 | %s |" % date.today().isoformat(),
        "| 文件数 | %d |" % len(want),
        "",
        "复制范围：`src/` → `src/`（只取 .c/.h）。",
        "**不含** `arkvim_fileio_posix.c`：主机端 POSIX 后端，和",
        "`arkvim_fileio_vfs.c` 实现同一组后端钩子，同进构建路径会撞符号。",
        "",
        "## 三个接线文件（Ark RTOS 侧，不在同步范围）",
        "",
        "| 文件 | 职责 |",
        "|---|---|",
        "| `arkvim_port_arkrtos.c` | 端口薄包：字节收发/延时打到 `ark_shell` 的 `platform.h`，"
        "控制台锁与 `>` 重定向捕获都在那一层，绕过去会在重定向下丢字符 |",
        "| `arkvim_fileio_vfs.c/.h` | 文件后端：装载/保存/`cat`/`ls` 走 `arkrtos_vfs_*`，"
        "路径解析统一用 `arkrtos_shell_resolve_path()`（一个 shell 只该有一个 cwd 语义） |",
        "| `arkvim_shell_cmd.c/.h` | `vim <file>` 命令与命令表注册钩子 |",
        "",
        "## 尺寸裁剪（不要改上游源码）",
        "",
        "上游每个上限都是 `#ifndef`，尺寸由内核工程 Makefile 的 `ARKVIM_DEFS` 决定：",
        "`ARKVIM_GB_MAX_SIZE=8192` / `ARKVIM_LT_MAX_LINES=512` / "
        "`ARKVIM_UNDO_MAX_DEPTH=32`。",
        "`sizeof(arkvim_editor_t)` 在默认尺寸下是 31148 B（16K 文本 + 4100 行表 + "
        "9736 撤销栈），8K 文本后仍占十几 KB，RAM 只有 440KB，所以不能按上游默认开。",
        "`ARKVIM_GB_MAX_SIZE` 同时是文件后端拒绝超大文件的判据，二者永不脱节。",
        "",
        "## 板端已知限制",
        "",
        "根文件系统是 ramfs：单文件上限 512 B、最多 8 个文件 / 16 个目录"
        "（`ark_vfs_ramfs.c`）。正式配置 8K 缓冲比这大得多，所以 \"file too big\" "
        "分支在板端几乎打不到，要用 `-DARKVIM_GB_MAX_SIZE=256` 的临时构建才能验证。",
        "",
        "## 内核侧依赖",
        "",
        "`arkrtos_shell_resolve_path()`（kernelsrc/include/arkrtos_shell.h）、",
        "`arkrtos_vfs_app_stat/open_read/read/close/write_file/list`"
        "（kernelsrc/include/arkrtos_vfs.h）。",
        "少了它们会在链接期报未定义，而不是在运行时出错。",
        "",
    ]
    with open(os.path.join(DST_ROOT, "VENDOR.md"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print("已同步，VENDOR.md 已更新（源 commit %s）" % head[:12])
    return 0


if __name__ == "__main__":
    sys.exit(main())
