# -*- coding: utf-8 -*-
"""把 arkmake 源仓库同步进 Ark RTOS 的 components/arkmake。

与 sync_arkc.py / sync_arkvim.py 同一套做法：复制而不是 submodule（克隆 Ark RTOS
的人不需要第二步、也不需要网络），本脚本是唯一入口，VENDOR.md 记来源。

arkmake 的树很小，同步范围就是两个目录：

  src/      -> src/      （.c/.h）
  include/  -> include/  （.h）

组件根目录的四个接线文件属于 Ark RTOS 侧（端口、shell 命令、两个头），
**同步不碰**，只做存在性自检。

与另外两个同步脚本的一处不同：**不删任何文件**。目标目录里多出来的文件
只报告，不自动移除——它们是"上游删了但本地还在"的证据，删不删该由人决定，
而一次同步顺手删掉一个还用着的文件，是这套复制式同步最容易犯的错。

用法：
  py -3 tools/sync_arkmake.py                 # 看差异
  py -3 tools/sync_arkmake.py --apply         # 落盘
  py -3 tools/sync_arkmake.py --check         # 漂移则退出码 2
  py -3 tools/sync_arkmake.py --src D:\\path\\to\\arkmake
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from datetime import date

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DST_ROOT = os.path.join(REPO, "kernelsrc", "components", "arkmake")

# 源仓库相对目录 -> 目标相对目录（相对 DST_ROOT）
COPY_MAP = [
    ("src", "src"),
    ("include", "include"),
]
# tests/ 与 docs/ 不进组件：板上不要测试驱动，文档留在源仓库
EXCLUDE_DIRS = {"tests", "docs", ".git"}

# 组件根目录里 Ark RTOS 侧的接线文件（同步不许动，只做存在性自检）
WIRING = [
    "arkmake_port_arkrtos.c",
    "arkmake_port.h",
    "arkmake_shell_cmd.c",
    "arkmake_shell_cmd.h",
]


def default_src():
    cand = os.path.join(os.path.dirname(REPO), "arkmake")
    return os.environ.get("ARKMAKE_SRC", cand)


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
        print("找不到 arkmake 源仓库：%s" % src)
        print("用 --src 指定，或设环境变量 ARKMAKE_SRC")
        return 2
    if not os.path.isfile(os.path.join(src, "include", "arkmake.h")):
        print("%s 看起来不是 arkmake 仓库（缺 include/arkmake.h）" % src)
        return 2

    want = collect(src)
    if want is None:
        return 2

    have = set()
    for rel_top in ("src", "include"):
        top = os.path.join(DST_ROOT, rel_top)
        if not os.path.isdir(top):
            continue
        for root, dirs, names in os.walk(top):
            dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
            for name in names:
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
    extra = sorted(have - set(r.replace(os.sep, "/") for r in want))

    missing_wiring = [w for w in WIRING
                      if not os.path.isfile(os.path.join(DST_ROOT, w))]

    print("源：%s" % src)
    print("目标：%s" % DST_ROOT)
    for tag, items in (("新增", added), ("更新", changed),
                       ("多余（不自动删）", extra)):
        for it in items:
            print("  %s  %s" % (tag, it))
    print("未变 %d 个 / 新增 %d / 更新 %d / 多余 %d"
          % (len(same), len(added), len(changed), len(extra)))
    if missing_wiring:
        print("警告：组件根目录缺少接线文件：%s" % ", ".join(missing_wiring))

    if args.check:
        if added or changed or extra:
            print("组件与源仓库不一致：跑 --apply 同步")
            return 2
        print("一致")
        return 0

    if not args.apply:
        print("（dry-run，未落盘；加 --apply 执行）")
        return 0

    for rel in added + changed:
        dst = os.path.join(DST_ROOT, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(want[rel], dst)

    head = git_head(src)
    dirty = git_dirty(src)
    lines = [
        "# arkmake（同步副本，勿手改）",
        "",
        "本目录由 `tools/sync_arkmake.py` 从 arkmake 源仓库复制而来。",
        "**不要改 `src/` 与 `include/` 里的代码**：改动会在下次同步时被覆盖。",
        "改源仓库，再跑同步。组件根目录的接线文件属于 Ark RTOS 侧，同步不碰，可以直接改。",
        "",
        "| 项 | 值 |",
        "|---|---|",
        "| 来源 | `%s` |" % src.replace("\\", "/"),
        "| 源 commit | `%s` |" % head,
        "| 工作区 | %s |" % ("有未提交改动" if dirty else "干净"),
        "| 同步日期 | %s |" % date.today().isoformat(),
        "| 文件数 | %d |" % len(want),
        "",
        "复制范围：`src/` → `src/`、`include/` → `include/`（只取 .c/.h）。",
        "**不含** `tests/`：板上不要测试驱动；判据在源仓库那一侧跑。",
        "",
        "## 四个接线文件（Ark RTOS 侧，不在同步范围）",
        "",
        "| 文件 | 职责 |",
        "|---|---|",
        "| `arkmake_port_arkrtos.c` | 三个回调：读（`fs_read_file`）、日志"
        "（`ark_shell_printf`）、打包（`arkc_cc_pack`） |",
        "| `arkmake_port.h` | `arkmake_ctx_t{verbose}` 与 `arkmake_port_ops()` / "
        "`arkmake_port_read()` |",
        "| `arkmake_shell_cmd.c` | `make [<makefile>] [-f f] [-o out] [-v]`：先解析"
        "命令行（写错立刻报），再读+解析工程，最后交给 `arkmake_build()` |",
        "| `arkmake_shell_cmd.h` | `arkmake_shell_register()`，与 arkvim/arkc 同一种"
        "注册钩子 |",
        "",
        "## 为什么没有第五条打包通路",
        "",
        "`arkmake` 不自己打镜像：`ops->pack` 直接调 `arkc_cc_pack()`，"
        "也就是 `cc --pack` 的本体。两条路迟早会打出不一样的镜像，而只有一条被上板跑过。",
        "代价是 `ARKRTOS_USE_ARKMAKE=1` 必须同时开 `ARKRTOS_USE_ARKC` 与 "
        "`ARKRTOS_USE_APPPACK`——这条依赖在 `arkrtos_features.h` 里写成 `#error`，"
        "不是运行期的一句\"失败了\"。",
        "",
        "## 板端已知限制",
        "",
        "- **永远全量构建**：vfs 卷不保证可靠的 mtime，用时间戳判断要不要重编会得出"
        "随机结论。代价是两源一次约 33 s（四遍 × 两份源 + 四次链接）。",
        "- **对象不可复用**：每一遍的 `.sct` 不同，arkc 在 codegen 期就把基址烤进指令，"
        "所以 `-c` 产物只能在同一份 `.sct` 下重编，不能跨遍复用。",
        "- **SRCS 上限 4**（`ARKMAKE_MAX_SRCS`，与 `CC_PACK_MAX_SRCS` 对齐）："
        "超了在解析层就报 `too many entries`，不是等四遍跑完才失败。",
        "- `-o` 覆盖工程里的 `OUT`；两者都空时产物名是 `<TARGET>.svcm`（当前目录）。",
        "- 优化档固定 -O1，与 `cc --pack` 缺省一致：四遍必须同档，否则差分会把优化"
        "差异当成地址位移补掉。",
        "",
        "## 内核侧依赖",
        "",
        "`fs_read_file` / `fs_last_error`（`components/arkc/include/flash_fs.h`）、"
        "`arkc_cc_pack`（`components/arkc/arkc_shell_cmd.h`）、"
        "`ark_shell_printf`（`kernelsrc/shell/ark_shell/ark_shell.h`）。",
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
