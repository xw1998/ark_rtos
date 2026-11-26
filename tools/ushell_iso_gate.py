#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SEC-G08 隔离执行（upcall）的 QEMU 端到端门禁。

为什么必须有这一层：M-08-1..3 的宿主用例只能证明「函数被调用了、参数摆
对了」，证明不了**真的跨了特权级**——宿主上没有 PLV、没有窗口、没有
`syscall 0x23` 回内核这条路。而 SEC-G08 的全部意义就是「App 的回调跑在
PLV3，碰不到内核 RAM」。所以判据必须落在 QEMU 的真实读数上。

判据（任一不满足即 FAIL，退出码非 0）：
  1. 演示任务把 `ushell_iso` 送进 VFS 并起跑（`[ushell] write ... rc=0`
     `[ushell] exec rc=0`），App 自己报 `registered, waiting for callbacks`；
  2. 控制台键入 `hi a b` 后，**App 侧**打出 `cb argc=3` 与三条
     `argv[i]=hi/a/b`——参数是内核在起跑前搬进 App 窗口的副本，读得到
     就说明「PLV3 侧能拿到内核给的数据」这条路通了；
  3. App 打出自己的 `cb argv=` / `func=` / `sp=` 三个地址，且**都落在窗口
     内**（`0x02...`）——证明回调看到的是 App 窗口里的地址，不是内核栈；
  4. **内核侧**打出 `[ushell] isolated rc=0 tid=N space=0 plv=3`：
     `plv` 取自 trap 帧的 PRMD 两位，是本门禁唯一能证明「真降权」的读数；
  5. 同一条命令**再发一次**仍然 `rc=0`（`s_up_busy` 在返回路径上归零，
     否则这条 App 的 shell 命令第二次就永久 busy）；
  6. 反面：`hix` 必须 `command not found`，且**不触发**回调（App 侧没有新的
     `cb argc=`）——证明分发是「按注册名精确匹配」，不是「有前缀就调」；
  7. 全程没有 UNEXPECTED / FATAL / FAILED。

反假绿（`--mutate`，默认不做，跑一次约 3 分钟）：
  - `stub-syscall`：把桩里的 `syscall 0x23` 换成别的号 ⇒ 内核收不到返回，
    第 4 条（`isolated rc=0`）必须红；
  - `priv-flag`：把 `p_task->is_priv = 0u` 改成 `1u` ⇒ 回调跑在 PLV0，
    第 4 条必须红在 `plv=3` 上；
  - `busy-clear`：返回路径不再清 `s_up_busy` ⇒ 第 5 条（第二次调用）必须红。
  三条都做完自动还原源码。

跑法（仓库根目录）：

    python tools/ushell_iso_gate.py             # 默认：直接跑现成镜像
    python tools/ushell_iso_gate.py --mutate    # 连三条变异一起验
"""

import os
import re
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
BUILD = os.path.join(ROOT, "build", "qemu_loongarch64")
ELF = os.path.join(BUILD, "ark_qemu_la64.elf")
MAKEFILE = os.path.join("example", "qemu_loongarch64", "Makefile")

QEMU = os.environ.get(
    "QEMU_LA64",
    r"D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe",
)

# 窗口内地址的唯一真源（config/arkrtos_partition.h 里的 ARKRTOS_ELF_WINDOW_BASE）。
WIN_BASE = 0x02000000
WIN_SIZE = 0x00200000

# 变异要改的三处源码（(文件, 原串, 变异串)）。
MUTATIONS = {
    "stub-syscall": (
        os.path.join("kernelsrc", "port", "loongarch", "la64", "arkrtos_port.c"),
        "((unsigned long)LA_UP_SYSCALL_RET << 32)",
        "((0x22ul) << 32)",
    ),
    "priv-flag": (
        os.path.join("kernelsrc", "shell", "arkrtos_shell_ushell.c"),
        "    p_task->is_priv = 0u;",
        "    p_task->is_priv = 1u;",
    ),
    "busy-clear": (
        os.path.join("kernelsrc", "shell", "arkrtos_shell_ushell.c"),
        # 变异点必须在 ret() 里那个「清 busy」上：插在 s_up_rc 之前没用
        # （下面两行自己会清），实测就是「变异没抓到」。
        "        s_up_last_plv[i] = plv;\n        s_up_busy[i]     = 0u;",
        "        s_up_last_plv[i] = plv;\n        s_up_busy[i]     = 1u;   /* MUTANT: keep busy */",
    ),
}


def fail(msg, log=""):
    print("FAIL: " + msg)
    if log:
        print("---- 串口原始输出 ----")
        print(log)
    return 1


def _free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def build():
    """编镜像（变异后要重编，所以单独抽出来）。"""
    rc = subprocess.call(
        ["make", "-f", MAKEFILE, "-j8"],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if rc != 0:
        raise SystemExit("构建失败（make -f %s）" % MAKEFILE)


class Console(object):
    """QEMU 串口（TCP chardev）：双向、可分段 pump。"""

    def __init__(self):
        self.port = _free_port()
        self.proc = subprocess.Popen(
            [
                QEMU,
                # 相对文件名 + cwd=BUILD：仓库路径含非 ASCII，QEMU 在
                # Windows 上打不开那种绝对路径且不报错（表现为永远连不上）。
                "-kernel", os.path.basename(ELF),
                "-display", "none",
                "-chardev",
                "socket,id=c0,host=127.0.0.1,port=%d,server=on,wait=on" % self.port,
                "-serial", "chardev:c0",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            cwd=BUILD,
        )
        self.log = ""
        self.sock = None
        for _ in range(100):
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.port), 0.2)
                break
            except OSError:
                time.sleep(0.1)
        if self.sock is None:
            raise SystemExit("连不上 QEMU 的 chardev 端口 %d" % self.port)
        self.sock.settimeout(0.2)

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                chunk = self.sock.recv(4096)
                if chunk:
                    # 串口是 \r\n：不抹掉 \r，`^hi a b$` 这种行首/行尾锚点
                    # 永远匹配不上（踩过：判据报「0 次回调」而日志里明明有）。
                    self.log += chunk.decode("utf-8", "replace").replace("\r", "")
            except socket.timeout:
                pass
            except OSError:
                break
        return self.log

    def run(self, cmdline, wait=2.0):
        mark = len(self.log)
        self.sock.sendall(cmdline.encode("ascii") + b"\r")
        self.pump(wait)
        return self.log[mark:]

    def close(self):
        try:
            self.proc.terminate()
            self.proc.wait(timeout=5)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass


def _win_addr(text):
    """`0x021f3fd0` -> 整数；越界返回 None。"""
    if text is None:
        return None
    try:
        v = int(text, 16)
    except ValueError:
        return None
    if WIN_BASE <= v < (WIN_BASE + WIN_SIZE):
        return v
    return None


def judge(log, slot_log):
    """判据全在这里：喂一份串口日志，返回 (ok, reason)。

    抽成纯函数是为了下面三条变异能复用同一份判据——变异只换镜像，判据不
    动，这样「变异被抓到」才说明判据真的在看行为。
    """
    # 1) 起跑
    if "registered, waiting for callbacks" not in log:
        return False, "App 没注册成功（没等到 registered）"
    if re.search(r"\[ushell\] write \S+ rc=0 size=\d+", log) is None:
        return False, "演示任务没把 ushell_iso 写进 VFS"
    if "[ushell] exec rc=0" not in log:
        return False, "arkrtos_elf_exec 没成功"

    # 2) + 3) App 侧：参数与地址
    if len(slot_log) < 2:
        return False, "只抓到 %d 次回调，期望 >= 2" % len(slot_log)
    first = slot_log[0]
    if "[ushell_iso] cb argc=3" not in first:
        return False, "首次回调 argc 不是 3\n" + first
    for i, want in enumerate(("hi", "a", "b")):
        if ("[ushell_iso] argv[%d]=%s" % (i, want)) not in first:
            return False, ("首次回调 argv[%d] 不是 %r（参数没搬进窗口 / 被覆盖）\n%s"
                           % (i, want, first))
    m = re.search(r"\[ushell_iso\] cb argv=(0x[0-9A-Fa-f]+) "
                  r"func=(0x[0-9A-Fa-f]+) sp=(0x[0-9A-Fa-f]+)", first)
    if m is None:
        return False, "App 没打出 argv/func/sp 三个地址\n" + first
    for name, val in zip(("argv", "func", "sp"), m.groups()):
        if _win_addr(val) is None:
            return False, "%s=%s 不在 App 窗口内（内核把内核地址漏给 PLV3 了）\n%s" \
                % (name, val, first)

    # 4) 内核侧：rc + PLV 证据（就在首次回调那个分片里）
    if re.search(r"\[ushell\] isolated rc=0 tid=\d+ space=0 plv=3", first) is None:
        return False, ("内核侧读数不是 rc=0/plv=3（降权没生效或返回值没回来）\n"
                       + first)

    # 5) 第二次仍然 rc=0（busy 位归零）
    second = slot_log[1]
    if "[ushell_iso] cb argc=3" not in second:
        return False, "第二次调用没进回调（busy 没归零？）\n" + second
    if re.search(r"\[ushell\] isolated rc=0 tid=\d+ space=0 plv=3", second) is None:
        return False, ("第二次内核侧不是 rc=0（busy 残留）\n" + second)

    # 6) 反面：未知命令。既要报 not found，也**不能**误调到回调
    #    （前缀/模糊匹配会把 hix 当成 hi）。
    if len(slot_log) < 3:
        return False, "只抓到 %d 个命令分片，期望 3（hi / hi / hix）" % len(slot_log)
    if "hix: command not found" not in slot_log[2]:
        return False, "hix 没报 command not found\n" + slot_log[2]
    if "cb argc" in slot_log[2]:
        return False, "hix 被误分发到 hi 的回调了\n" + slot_log[2]
    return True, ""


def split_slots(log):
    """按「控制台回显的命令行」把日志切成 per-command 的片段。

    QEMU 串口是**双向**的，输入的命令会被回显，所以切分点取回显行本身；
    这里只认我们自己发的三条（两条 hi、一条 hix），别的回显忽略。
    """
    marks = [m.start() for m in re.finditer(r"^hi a b\r?$|^hix\r?$", log, re.M)]
    slots = []
    for i, at in enumerate(marks):
        end = marks[i + 1] if (i + 1) < len(marks) else len(log)
        slots.append(log[at:end])
    return slots


def run_once():
    con = Console()
    try:
        # 启动期：任务 A/B、存储自检、mod 演示、App 装载。给 12 秒。
        con.pump(12.0)
        if "registered, waiting for callbacks" not in con.log:
            return fail("App 没注册上（12 秒内没看到 registered）", con.log)

        con.run("hi a b", wait=2.5)
        con.run("hi a b", wait=2.5)
        con.run("hix", wait=1.5)

        for bad in ("UNEXPECTED", "FATAL", "FAILED"):
            if bad in con.log:
                return fail("全程出现 %s" % bad, con.log)

        slots = split_slots(con.log)
        ok, why = judge(con.log, slots)
        if not ok:
            return fail(why, con.log)

        print("PASS: SEC-G08 upcall 隔离执行端到端通"
              "（PLV3 回调 / 参数搬进窗口 / rc 回传 / busy 归零 / 未知名不误调）")
        return 0
    finally:
        con.close()


def patch_source(path, old, new):
    with open(path, "r", newline="", encoding="utf-8") as f:
        s = f.read()
    if s.count(old) != 1:
        raise SystemExit("变异点不唯一（%d 处）：%s" % (s.count(old), old[:60]))
    with open(path, "w", newline="", encoding="utf-8") as f:
        f.write(s.replace(old, new, 1))


def mutate():
    """三条变异：每条都必须让判据报红，然后还原。"""
    bad = 0
    for name, (rel, old, new) in MUTATIONS.items():
        path = os.path.join(ROOT, rel)
        with open(path, "r", newline="", encoding="utf-8") as f:
            original = f.read()
        try:
            patch_source(path, old, new)
            build()
            con = Console()
            try:
                con.pump(12.0)
                con.run("hi a b", wait=2.5)
                con.run("hi a b", wait=2.5)
                con.run("hix", wait=1.5)
                slots = split_slots(con.log)
                ok, why = judge(con.log, slots)
            finally:
                con.close()
            if ok:
                print("FAIL: 变异 %s 没被抓到（判据是瞎的）" % name)
                bad += 1
            else:
                print("  [mutation caught] %s -> %s" % (name, why.split("\n")[0]))
        finally:
            with open(path, "w", newline="", encoding="utf-8") as f:
                f.write(original)
    # 还原后重编一次，别把变异后的镜像留给后面的人
    build()
    if bad:
        print("FAIL: %d/3 条变异没被抓到" % bad)
        return 1
    print("PASS: 3/3 条变异都被抓到（stub 号 / is_priv / busy 归零）")
    return 0


def main():
    if not os.path.isfile(ELF):
        return fail("找不到 " + ELF + "，先 make -f " + MAKEFILE)
    if "--mutate" in sys.argv[1:]:
        return mutate()
    return run_once()


if __name__ == "__main__":
    sys.exit(main())
