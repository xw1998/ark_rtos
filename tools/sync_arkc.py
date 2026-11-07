# -*- coding: utf-8 -*-
"""把 arkc 源仓库同步进 Ark RTOS 的 components/arkc。

与 sync_arkvim.py 同一套做法（复制而非 submodule、脚本是唯一入口、VENDOR.md
记来源），差别在排除清单：arkc 的上游树里只有 src/ 与 include/ 属于编译器本体，
src/main.c 与 src/port/ 是主机/裸机驱动，src/backend/ 下还有三个本板不要的后端。

复制规则：
  - src/      -> src/      （.c/.h），排除 src/main.c
  - include/  -> include/  （.h）
  - 排除 src/backend/c、src/backend/x86_64、src/backend/riscv32
  - port/ 整体不复制：Ark RTOS 侧的三个接线文件取代了它
  - **不碰组件根目录的接线文件**（arkc_port_arkrtos.c、arkc_port.h、
    arkc_shell_cmd.c/.h）：它们属于 Ark RTOS，同步只允许改上面两个目录
  - stdlib/include/stdarg.h -> kernelsrc/sdk/mini_sdk/arkc/stdarg.h：板载
    SDK 是手抄进 NOR 卷的头文件集，不属于组件目录，但变参规则只能有一份
    实现，所以一并同步（见下方 SDK_MAP）

用法：
  py -3 tools/sync_arkc.py                 # 看差异
  py -3 tools/sync_arkc.py --apply         # 落盘
  py -3 tools/sync_arkc.py --check         # 漂移则退出码 2
  py -3 tools/sync_arkc.py --src D:\\path\\to\\arkc
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from datetime import date

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DST_ROOT = os.path.join(REPO, "kernelsrc", "components", "arkc")

# 源仓库相对目录 -> 目标相对目录（相对 DST_ROOT）
COPY_MAP = [
    ("src", "src"),
    ("include", "include"),
]
# 不复制：主机驱动器（自己带 main()，板上由 shell 命令代替）
EXCLUDE_NAMES = {"main.c"}
# 不复制：本板不用的后端（C / x86_64 / riscv32 三种）
EXCLUDE_DIRS = {"c", "x86_64", "riscv32"}

# 板载 mini SDK 需要的那几个标准头：落点在组件目录之外（sdk/ 下），但同样
# 是 arkc 的产物。第二份实现一定会漂，所以由这里统一搬。
# (源仓库相对路径, 本仓库相对路径)
SDK_MAP = [
    ("stdlib/include/stdarg.h", "kernelsrc/sdk/mini_sdk/arkc/stdarg.h"),
]

# 组件根目录里 Ark RTOS 侧的接线文件（同步不许动，只做存在性自检）
WIRING = [
    "arkc_port_arkrtos.c",
    "arkc_port.h",
    "arkc_shell_cmd.c",
    "arkc_shell_cmd.h",
]


def default_src():
    cand = os.path.join(os.path.dirname(REPO), "ark c compiler")
    return os.environ.get("ARKC_SRC", cand)


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
            # 排除目录只在 backend/ 这一层出现，但按名字裁掉更省事：
            # 上游以后若把同名目录搬到别处，也不会被悄悄抄进来。
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


def collect_sdk(src):
    """返回 {本仓库相对路径: 源绝对路径}，用于 SDK_MAP 里的板载头。"""
    out = {}
    for rel_src, rel_dst in SDK_MAP:
        full = os.path.join(src, rel_src.replace("/", os.sep))
        if not os.path.isfile(full):
            print("缺少源文件：%s" % full)
            return None
        out[rel_dst] = full
    return out

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
        print("找不到 arkc 源仓库：%s" % src)
        print("用 --src 指定，或设环境变量 ARKC_SRC")
        return 2
    if not os.path.isfile(os.path.join(src, "src", "compiler.c")):
        print("%s 看起来不是 arkc 仓库（缺 src/compiler.c）" % src)
        return 2

    want = collect(src)
    if want is None:
        return 2
    want_sdk = collect_sdk(src)
    if want_sdk is None:
        return 2

    have = set()
    for rel_top in ("src", "include"):
        top = os.path.join(DST_ROOT, rel_top)
        if not os.path.isdir(top):
            continue
        for root, dirs, names in os.walk(top):
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

    sdk_added, sdk_changed, sdk_same = [], [], []
    for rel in sorted(want_sdk):
        dst = os.path.join(REPO, rel.replace("/", os.sep))
        if not os.path.isfile(dst):
            sdk_added.append(rel)
        elif sha(dst) != sha(want_sdk[rel]):
            sdk_changed.append(rel)
        else:
            sdk_same.append(rel)

    print("源：%s" % src)
    print("目标：%s" % DST_ROOT)
    for tag, items in (("新增", added), ("更新", changed), ("删除", removed),
                       ("新增(SDK)", sdk_added), ("更新(SDK)", sdk_changed)):
        for it in items:
            print("  %s  %s" % (tag, it))
    print("未变 %d 个 / 新增 %d / 更新 %d / 删除 %d（SDK 头 未变 %d / 新增 %d / 更新 %d）"
          % (len(same), len(added), len(changed), len(removed),
             len(sdk_same), len(sdk_added), len(sdk_changed)))
    if missing_wiring:
        print("警告：组件根目录缺少接线文件：%s" % ", ".join(missing_wiring))

    if args.check:
        if added or changed or removed or sdk_added or sdk_changed:
            print("组件与源仓库不一致：跑 --apply 同步")
            return 2
        print("一致")
        return 0

    if not args.apply:
        print("（dry-run，未落盘；加 --apply 执行）")
        return 0

    for rel in removed:
        os.remove(os.path.join(DST_ROOT, rel.replace("/", os.sep)))
    for rel in sdk_added + sdk_changed:
        dst = os.path.join(REPO, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(want_sdk[rel], dst)
        os.utime(dst, None)
    for rel in added + changed:
        dst = os.path.join(DST_ROOT, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(want[rel], dst)
        # copy2 连 mtime 一起搬过来，而 mtime 正是 make 判断要不要重编的依据：
        # 源文件最后一次改动可能比目标 .o 还早，make 于是判定"不用重编"，产物
        # 一个字节都没变，看着像"同步生效了"（sync_arkvim.py 上踩过同一个坑）。
        os.utime(dst, None)

    head = git_head(src)
    dirty = git_dirty(src)
    lines = [
        "# arkc（同步副本，勿手改）",
        "",
        "本目录由 `tools/sync_arkc.py` 从 arkc 源仓库复制而来。",
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
        "",
        "**不含** `src/main.c`：主机驱动器自己带 `main()`，板上入口是 shell 的 "
        "`cc` 命令。",
        "**不含** `src/backend/{c,x86_64,riscv32}`：本板只有 arm 后端"
        "（`src/backend/arm` + `src/backend/registry.c`）。",
        "**不含** `port/`：`port/stm32/platform_stm32.c` 自带 "
        "`malloc/free/putchar/__user_initial_stackheap`，与 newlib 撞符号；"
        "`port/host` 是主机端。两者的职责由组件根目录的接线文件接过来。",
        "",
        "另外同步一份 `stdlib/include/stdarg.h` → "
        "`kernelsrc/sdk/mini_sdk/arkc/stdarg.h`：板上市小程序的头文件集里有它"
        "（变参规则只能有一份实现，手抄一定漂）。",
        ""
        "",
        "## 四个接线文件（Ark RTOS 侧，不在同步范围）",
        "",
        "| 文件 | 职责 |",
        "|---|---|",
        "| `arkc_port_arkrtos.c` | 端口层：`ark_log`（LF→CRLF，按 200 字节分块走"
        "控制台上锁）、`ark_abort`（打印后 longjmp，不挂死 shell）、"
        "`ark_get_date_time`（固定 1970-01-01）、5 个 `fs_*`（走 `arkrtos_vfs_*`） |",
        "| `arkc_port.h` | 上面那层给命令层用的四个函数（abort 落点 / 错误文本 / "
        "arena 清零） |",
        "| `arkc_shell_cmd.c` | `cc` 命令：解析 `-o -S -c -E -D -I -O -target`，"
        "`setjmp` 保护、每次运行前 `config_init()` + `diag_init()`、结果按返回码回报 |",
        "| `arkc_shell_cmd.h` | `arkc_shell_register()`，与 arkvim 同一种注册钩子 |",
        "",
        "## 构建接线（Makefile，不要改上游源码）",
        "",
        "arkc 的 20 个源必须用独立的 `CFLAGS_ARKC` 编译，原因只有一条：",
        "`include/platform.h` 与 `kernelsrc/shell/platform.h` **同名**。",
        "把 arkc 的 include 放进全局 `-I`，会让 shell/arkvim 的 "
        "`#include \"platform.h\"` 拿到编译器那份（或反之）；"
        "所以 arkc 的源文件单独带 `-I components/arkc/include`，"
        "接线文件则用相对路径（`#include \"include/platform.h\"`、"
        "`#include \"../../shell/platform.h\"`）把两份分清。",
        "",
        "尺寸由 `ARKC_DEFS` 收（`-DARK_POOL_SIZE=…` 等，见 Makefile），"
        "默认值在 `include/arkcc_config.h` 里都是 `#ifndef`，不需要也不应该改上游。",
        "工作区共约 178 KB，全部落在链接脚本的 `.ark_buf (NOLOAD)` 段：",
        "不占镜像体积、不花上电清零时间，代价是内容不确定，"
        "所以 `cc` 每次运行前调 `arkc_port_zero_arena()` 整段清零。",
        "",
        "## 板端已知限制",
        "",
        "- **`#include \"x.h\"` 不按包含者目录解析**：arkc 的 `read_include()` 只用"
        "文件名拼 `-I` 目录，因此板上头文件要么用 `-I` 指到目录，要么放在当前 cwd。"
        "主机上靠 `#include \"sub/x.h\"` 的相对路径写法在板上会失败——这是上游行为，"
        "不是移植缺陷，改它要改 `src/preproc.c`。",
        "- 根文件系统是 ramfs：单文件上限 512 B、最多 8 文件 / 16 目录。源码能放，"
        "但要编译真程序（连同头文件）必须换 littlefs 卷。",
        "- 输出默认写 `<file>.bin`；`-o -` 打到控制台，文本原样、二进制先提示再 hex，"
        "不会把二进制糊在终端上。",
        "- `ark_abort` 已改为 longjmp 回到 `cc`，所以池耗尽（`-DARK_POOL_SIZE` 太小）"
        "表现为 `cc: aborted: <msg>` 后回到提示符，而不是板子卡死。",
        "",
        "## 内核侧依赖",
        "",
        "`arkrtos_vfs_app_stat/open_read/read/close/write_file/list`"
        "（`kernelsrc/include/arkrtos_vfs.h`）、"
        "`arkrtos_shell_resolve_path`（`kernelsrc/include/arkrtos_shell.h`）、"
        "`platform_uart_send_buf/console_begin/console_end/tick_ms`"
        "（`kernelsrc/shell/platform.h`）。",
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
