# -*- coding: utf-8 -*-
"""把 ark_softbus 源仓库同步进 Ark RTOS 的 components/ark_softbus。

与 sync_ark_vfs.py 同一套路（详见那边的注释）：复制而不是 submodule，
克隆者不需要第二步就能编译；代价是两处副本会漂移，所以本脚本是唯一入口：

  - 只复制编译需要的东西（include/、src/、ports/arkrtos/ 的非测试部分）
  - **不复制 tests/**：其中的 mock 头与真内核头同名，进了内核构建路径会
    把真头文件挡掉，测试源更不该进固件
  - 不复制 tools/、sim/、mcp/、build/、examples/、platform/：那是主机侧
    与多平台构建，不属于内核组件
  - 写 VENDOR.md 记录来源 commit 与文件清单
  - 默认 dry-run 提示差异，--apply 才落盘，--check 供 CI 判断是否漂移

用法：
  py -3 tools/sync_ark_softbus.py                 # 看差异
  py -3 tools/sync_ark_softbus.py --apply         # 落盘
  py -3 tools/sync_ark_softbus.py --check         # 漂移则退出码 2
  py -3 tools/sync_ark_softbus.py --src D:\\path\\to\\ark_softbus
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from datetime import date

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DST_ROOT = os.path.join(REPO, "kernelsrc", "components", "ark_softbus")

# 源仓库目录 -> 目标目录（相对 DST_ROOT）
COPY_MAP = [
    ("include", "include"),
    ("src", "src"),
    ("ports/arkrtos", "port"),
]
# 目标目录内不允许出现的文件（测试与构建产物）
EXCLUDE_NAMES = {"Makefile", "run_port_tests.sh", "run_host_tests.sh"}
EXCLUDE_DIRS = {"tests", "mock", "build", "sim", "mcp", "tools", "examples",
                "platform"}

# 不同步进内核构建的传输/文件（同步了也编不到，但保留会让 --check 有意义）
# 目前没有：transport_sock.c 需要 POSIX socket，内核工程不把它加进源列表即可。


def default_src():
    cand = os.path.join(os.path.dirname(REPO), "ark_softbus")
    return os.environ.get("ARK_SOFTBUS_SRC", cand)


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
                if not name.endswith((".c", ".h", ".md")):
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


GEN_HEADER = os.path.join(REPO, "kernelsrc", "include",
                            "arkrtos_softbus_sync.h")


def render_header(head, dirty):
    """把「来源 commit」写成内核可直接引用的宏。

    为什么要有这个文件：内核控制台 `bus info` 会打一行 `softbus (ark_softbus
    <commit>)`。如果那串 commit 手工写在 arkrtos_softbus.c 里，同步完就没人
    改它——显示出来的「权威版本号」会是假的（2026-10-07 就是这样：组件已到
    18c6ccf，控制台还报 915c4ac）。改成由同步工具写、内核只读，commit 就只
    有一个来源。
    """
    return (
        "/* 由 tools/sync_ark_softbus.py 生成，勿手改；随组件一起提交。 */\n"
        "#ifndef __ARKRTOS_SOFTBUS_SYNC_H__\n"
        "#define __ARKRTOS_SOFTBUS_SYNC_H__\n"
        "\n"
        "#define ARKRTOS_SOFTBUS_COMMIT      \"%s\"\n"
        "#define ARKRTOS_SOFTBUS_COMMIT_FULL \"%s\"\n"
        "#define ARKRTOS_SOFTBUS_DIRTY       %d\n"
        "\n"
        "#endif /* __ARKRTOS_SOFTBUS_SYNC_H__ */\n"
        % (head[:7], head, 1 if dirty else 0))


def header_commit():
    """读回生成头里的完整 commit；文件不存在返回 None。"""
    try:
        with open(GEN_HEADER, encoding="utf-8") as f:
            for ln in f:
                if "ARKRTOS_SOFTBUS_COMMIT_FULL" in ln:
                    return ln.split('"')[1]
    except OSError:
        pass
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=default_src())
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    src = os.path.abspath(args.src)
    if not os.path.isdir(src):
        print("找不到 ark_softbus 源仓库：%s" % src)
        print("用 --src 指定，或设环境变量 ARK_SOFTBUS_SRC")
        return 2
    if not os.path.isfile(os.path.join(src, "include", "ark_bus.h")):
        print("%s 看起来不是 ark_softbus 仓库（缺 include/ark_bus.h）" % src)
        return 2

    want = collect(src)
    if want is None:
        return 2

    have = set()
    if os.path.isdir(DST_ROOT):
        for root, dirs, names in os.walk(DST_ROOT):
            dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
            for name in names:
                if name in EXCLUDE_NAMES or name == "VENDOR.md":
                    continue
                if not name.endswith((".c", ".h", ".md")):
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

    print("源：%s" % src)
    print("目标：%s" % DST_ROOT)
    for tag, items in (("新增", added), ("更新", changed), ("删除", removed)):
        for it in items:
            print("  %s  %s" % (tag, it))
    print("未变 %d 个 / 新增 %d / 更新 %d / 删除 %d"
          % (len(same), len(added), len(changed), len(removed)))

    if args.check:
        bad = bool(added or changed or removed)
        head = git_head(src)
        hc = header_commit()
        if hc != head:
            print("生成头与源 commit 不一致：头=%s 源=%s（跑 --apply 同步）"
                  % (hc, head[:12]))
            bad = True
        if bad:
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
        "# ark_softbus（同步副本，勿手改）",
        "",
        "本目录由 `tools/sync_ark_softbus.py` 从 ark_softbus 源仓库复制而来。",
        "**不要在这里直接改代码**：改动会在下次同步时被覆盖。改源仓库，再跑同步。",
        "",
        "| 项 | 值 |",
        "|---|---|",
        "| 来源 | `%s` |" % src.replace("\\", "/"),
        "| 源 commit | `%s` |" % head,
        "| 工作区 | %s |" % ("有未提交改动" if dirty else "干净"),
        "| 同步日期 | %s |" % date.today().isoformat(),
        "| 文件数 | %d |" % len(want),
        "",
        "复制范围：`include/` → `include/`、`src/` → `src/`、",
        "`ports/arkrtos/` → `port/`。",
        "**不含** `tests/`：其中的 mock 头与真内核头同名，进构建路径会把真头挡掉；",
        "`tools/`、`sim/`、`mcp/`、`examples/`、`platform/` 是主机侧与多平台构建，",
        "不属于内核组件。`src/transport/transport_sock.c` 同步进来但内核工程不编译它",
        "（需要 POSIX socket）。",
        "",
        "## 内核侧依赖",
        "",
        "端口三个钩子由 `board/stm32f427/arkrtos_bus_port.c` 提供：",
        "`ark_bus_port_tick_ms()` / `ark_bus_port_lock()` / `ark_bus_port_unlock()`，",
        "分别打到 `arkrtos_kernel_get_tick()` 与 `arkrtos_sched_lock/unlock()`。",
        "大小档位由内核工程的 `-DARK_BUS_*` 决定，见 `kernelsrc/include/arkrtos_softbus.h`。",
        "",
    ]
    with open(os.path.join(DST_ROOT, "VENDOR.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print("已同步，VENDOR.md 已更新（源 commit %s）" % head[:12])
    with open(GEN_HEADER, "w", encoding="utf-8", newline="\n") as f:
        f.write(render_header(head, dirty))
    print("已写 %s（ARKRTOS_SOFTBUS_COMMIT %s）"
          % (os.path.relpath(GEN_HEADER, REPO).replace(os.sep, "/"), head[:7]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
