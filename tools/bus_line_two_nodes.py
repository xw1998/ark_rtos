#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""软总线"物理线两节点"门禁：两个 QEMU 实例经第二路 ns16550a 对连。

为什么需要这一道
----------------
`ARKRTOS_USE_BUS_UART` 默认 0（`config/qemu_loongarch64_defconfig`），默认档
只有板内 loopback 的 `arkrtos_softbus.c`。所以下面这些代码平时一个编译器都
不经过：

    kernelsrc/include/arkrtos_softbus_line.h   对外 API
    kernelsrc/src/arkrtos_softbus_line.c       线接入层（抽干 FIFO + pump）
    kernelsrc/components/ark_softbus/port/ark_bus_line_port.h
    board/qemu_loongarch64/drvuart_line.c      第二路 UART 寄存器层
    example/qemu_loongarch64/main.c 里的 task_bus_line 演示

改了任何一处而把开关留在 0，其它门禁仍然全绿，直到真有人打开才炸 —— 与
SMP / eset / waitmulti 同源的"默认关 = 没人编译"假绿形态。

三段验证（全部真跑 QEMU，不是纸面）
-----------------------------------
  1. probe：只写第二路 UART，不建总线。对端 socket 必须收到 `ARK-BUS-LINE`
     —— 这一条钉住"第二路串口确实映射到 ARKRTOS_QEMU_UART2_BASE"。
  2. 双节点：A（role=1，node 0x2101）server=on，B（role=2，node 0x2102）
     连入。要求
       A: `[line] node=0x00002101 ... echo=ok result=ok`
       B: `[line] rx topic=line/hb len=5` 且 `[line] node=0x00002102 ...
          served=... result=ok`
     回包内容是应答方**算出来的**（每个参数字节 +1：ping → qjoh），所以
     echo=ok 同时证明"服务真执行"与"字节序没串"。
  3. 防假绿（单节点）：只跑 A、不连 B。A 必须在 20 s 后打
     `result=fail (no reply)`。如果这一条也过，说明判据根本不是"收到回包"
     而是别的东西 —— 情形 2 的 ok 就一文不值。

不覆盖
------
  * 真板：本仓库没有第二路 UART 接线的板子；这道的结论只在 QEMU 上成立。
  * 与非 Ark 对端互通：两端都是 Ark RTOS 内核，帧格式是自家的。
  * 中断驱动的收发：本移植没有接外部中断控制器，收发都是轮询。
  * 波特率/流控：QEMU 的 chardev 不看波特率，故波特率编程在真板上才有意义。

用法
----
    python tools/bus_line_two_nodes.py              # 编三档 + 跑三段
    python tools/bus_line_two_nodes.py --no-build   # 用现有 build/ 产物
    python tools/bus_line_two_nodes.py --qemu <path>

退出码 0 = 三段全过；1 = 任何一段失败（缺 QEMU 是 FAIL，不是跳过）。
"""

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

MAKEFILE = os.path.join(ROOT, "example", "qemu_loongarch64", "Makefile")

# 三档的宏。ROLE 0=probe 1=发起方 2=应答方。
LINE_DEFS = "-DARKRTOS_USE_BUS_UART=1 -DARKRTOS_BUS_LINE_DEMO=1"
TIERS = {
    "a":     LINE_DEFS + " -DARKRTOS_BUS_LINE_ROLE=1 -DARKRTOS_BUS_LINE_NODE_ID=0x2101 -DARKRTOS_BUS_LINE_PEER_ID=0x2102",
    "b":     LINE_DEFS + " -DARKRTOS_BUS_LINE_ROLE=2 -DARKRTOS_BUS_LINE_NODE_ID=0x2102 -DARKRTOS_BUS_LINE_PEER_ID=0x2101",
    "probe": LINE_DEFS + " -DARKRTOS_BUS_LINE_ROLE=0 -DARKRTOS_BUS_LINE_NODE_ID=0x2101 -DARKRTOS_BUS_LINE_PEER_ID=0x2102",
    # 与 a 同一份代码，只把"等不到回包就判失败"的超时从 20 s 覆盖成 2 s。
    # 判据语义不变（仍是"没有对端必须自己认失败"），只是不让自动化测试
    # 为了证明一件必然发生的事而空等 20 s。
    "solo":  LINE_DEFS + " -DARKRTOS_BUS_LINE_ROLE=1 -DARKRTOS_BUS_LINE_NODE_ID=0x2101 -DARKRTOS_BUS_LINE_PEER_ID=0x2102 -DLINE_GIVE_UP_TICKS=4000u",
}

QEMU_CANDIDATES = [
    r"D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe",
    "qemu-system-loongarch64",
]

BUILD_DIR = "build/la64_line_{}"

A_OK = re.compile(r"\[line\] node=0x00002101\b.*echo=ok result=ok")
B_OK = re.compile(r"\[line\] node=0x00002102\b.*served=\d+ result=ok")
B_RX = re.compile(r"\[line\] rx topic=line/hb len=5")
A_FAIL = re.compile(r"\[line\] node=0x00002101\b.*result=fail \(no reply\)")
PROBE_OK = b"ARK-BUS-LINE"


# ---------------------------------------------------------------- 判据
# 纯函数：便于做变异自检（喂一份人工造的日志，检查判据会不会误判）。
def judge_probe(payload):
    return PROBE_OK in payload


def judge_two_nodes(a_log, b_log):
    return bool(A_OK.search(a_log)) and bool(B_RX.search(b_log)) and bool(B_OK.search(b_log))


def judge_single_node(a_log):
    return bool(A_FAIL.search(a_log))


def self_check():
    """变异自检：把已知好/坏日志喂给判据，判据必须给出对应结论。

    这一段是"门禁自己的防假绿"：如果某天有人把判据改成只看"日志非空"
    或者把 B 的日志判据放宽到任意 result=ok，这里会先炸。
    """
    good_a = "[line] node=0x00002101 pubs=2 rpc=1 reply_len=4 echo=ok result=ok\n"
    good_b = ("[line] rx topic=line/hb len=5\n"
              "[line] node=0x00002102 rx=1 served=1 result=ok\n")
    bad_a_echo = "[line] node=0x00002101 pubs=2 rpc=1 reply_len=4 echo=BAD result=fail\n"
    nreply_a = "[line] node=0x00002101 pubs=6 rpc=9 result=fail (no reply)\n"
    bad_b = "[line] rx topic=line/hb len=5\n"

    fails = []
    if not judge_two_nodes(good_a, good_b):
        fails.append("two_nodes: 好日志被判失败")
    if judge_two_nodes(bad_a_echo, good_b):
        fails.append("two_nodes: echo=BAD 被误判通过")
    if judge_two_nodes(good_a, bad_b):
        fails.append("two_nodes: B 未 served 被误判通过")
    if judge_two_nodes(nreply_a, good_b):
        fails.append("two_nodes: no reply 被误判通过")
    if not judge_single_node(nreply_a):
        fails.append("single_node: no reply 被判失败")
    if judge_single_node(good_a):
        fails.append("single_node: 成功日志被误判为 no reply")
    if not judge_probe(b"xxARK-BUS-LINExx"):
        fails.append("probe: banner 判决失败")
    if judge_probe(b"ARK-BUS-LI"):
        fails.append("probe: 半截 banner 被误判通过")
    return fails


# ---------------------------------------------------------------- 构建
def find_qemu(explicit):
    if explicit:
        if os.path.exists(explicit):
            return explicit
        return None
    for c in QEMU_CANDIDATES:
        if os.path.sep in c or ":" in c:
            if os.path.exists(c):
                return c
        else:
            p = shutil.which(c)
            if p:
                return p
    return None


def build_tier(tier, defs, jobs):
    bdir = os.path.join(ROOT, BUILD_DIR.format(tier).replace("/", os.sep))
    stamp = os.path.join(bdir, ".line_defs")
    # make 不跟踪 -D：宏变了必须先 clean，否则会静默复用旧对象。
    if os.path.isdir(bdir):
        old = None
        if os.path.exists(stamp):
            with open(stamp, "r", encoding="utf-8") as f:
                old = f.read().strip()
        if old != defs:
            subprocess.run([os.environ.get("MAKE", "make"), "-f", MAKEFILE,
                            "BUILD=" + BUILD_DIR.format(tier), "clean"],
                           cwd=ROOT, check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    cmd = [os.environ.get("MAKE", "make"), "-f", MAKEFILE,
           "BUILD=" + BUILD_DIR.format(tier), "EXTRA_CFLAGS=" + defs,
           "-j%d" % jobs]
    r = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        print("BUILD %s FAILED" % tier)
        sys.stdout.buffer.write(r.stdout[-4000:])
        return None
    os.makedirs(bdir, exist_ok=True)
    with open(stamp, "w", encoding="utf-8") as f:
        f.write(defs)
    elf = os.path.join(bdir, "ark_qemu_la64.elf")
    return elf if os.path.exists(elf) else None


# ---------------------------------------------------------------- 运行
class Console(object):
    """QEMU 控制台的增量读取器。

    为什么不用 `-serial file:`：QEMU 的 file chardev 会攒块再落盘，两三行
    的日志可能一直等到进程退出才看得见。把它读成"没有输出"，门禁就会把跑
    对的用例判红——实测过一次：A 已经拿到 echo=ok（说明 B 确实应答了），
    B 自己的汇总行却还堵在缓冲里，于是 case two 被判失败。socket chardev
    逐字节转发，读到就算数，既不用等进程结束，也不依赖 terminate 时的 flush。
    """

    def __init__(self, sock):
        self.sock = sock
        self.buf = b""
        self.sock.setblocking(False)

    def text(self, settle=0.0):
        """收下已经到达的字节并解码；settle>0 时再多等一小会儿等尾巴。"""
        end = time.time() + settle
        while True:
            try:
                d = self.sock.recv(4096)
            except BlockingIOError:
                d = None
            except OSError:
                d = b""
            if d:
                self.buf += d
                continue
            if d == b"" or settle <= 0.0 or time.time() >= end:
                break
            time.sleep(0.02)
        return self.buf.decode("utf-8", "replace")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def _connect_console(port, proc, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=2)
        except OSError:
            if proc.poll() is not None:
                return None
            time.sleep(0.05)
    return None


def run_qemu(qemu, elf, tmp, tag, extra, wait=0.0):
    """启动一个 QEMU 实例。返回 (proc, Console)；控制台没连上时第二个为 None。

    extra 里给出与第二路串口相关的 -chardev/-serial 片段。
    """
    cport = free_port()
    local_elf = os.path.join(tmp, "%s.elf" % tag)
    shutil.copyfile(elf, local_elf)
    cmd = [qemu, "-M", "virt", "-m", "128M", "-smp", "2",
           "-display", "none", "-monitor", "none"] + extra + [
           "-chardev",
           "socket,id=con,host=127.0.0.1,port=%d,server=on,wait=off" % cport,
           "-serial", "chardev:con",
           "-serial", "chardev:bus",
           "-kernel", local_elf]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    s = _connect_console(cport, p)
    if s is None:
        stop(p)
        return p, None
    con = Console(s)
    if wait:
        time.sleep(wait)
    return p, con


def close_console(con):
    if con is not None:
        con.close()


def stop(p):
    try:
        p.terminate()
        p.wait(timeout=10)
    except Exception:
        try:
            p.kill()
        except Exception:
            pass


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def case_probe(qemu, elf, tmp):
    port = free_port()
    chardev = "socket,id=bus,host=127.0.0.1,port=%d,server=on,wait=off" % port
    p, con = run_qemu(qemu, elf, tmp, "probe", ["-chardev", chardev])
    got = b""
    conn = None
    if con is None:
        stop(p)
        print("  case probe : console chardev 没连上 -> fail")
        return False
    try:
        for _ in range(150):
            if p.poll() is not None:
                break
            try:
                conn = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                time.sleep(0.2)
        if conn is not None:
            conn.settimeout(30)
            deadline = time.time() + 30
            while time.time() < deadline:
                try:
                    d = conn.recv(4096)
                except socket.timeout:
                    break
                if not d:
                    break
                got += d
                if PROBE_OK in got:
                    break
    finally:
        if conn is not None:
            conn.close()
        close_console(con)
        stop(p)
    ok = judge_probe(got)
    print("  case probe : recv=%r -> %s" % (got[:64], "ok" if ok else "fail"))
    return ok


def case_two_nodes(qemu, a_elf, b_elf, tmp, budget):
    port = free_port()
    ch_a = "socket,id=bus,host=127.0.0.1,port=%d,server=on,wait=off" % port
    ch_b = "socket,id=bus,host=127.0.0.1,port=%d,server=off" % port
    a, ca = run_qemu(qemu, a_elf, tmp, "a", ["-chardev", ch_a])
    if ca is None:
        stop(a)
        print("  case two   : console chardev 没连上 -> fail")
        return False
    b, cb = run_qemu(qemu, b_elf, tmp, "b", ["-chardev", ch_b], wait=1.0)
    if cb is None:
        close_console(ca)
        stop(b)
        stop(a)
        print("  case two   : console chardev 没连上 -> fail")
        return False
    ok = False
    try:
        deadline = time.time() + budget
        while time.time() < deadline:
            if judge_two_nodes(ca.text(), cb.text()):
                ok = True
                break
            if a.poll() is not None and b.poll() is not None:
                break
            time.sleep(0.2)
    finally:
        # 先把还没读完的尾巴收进缓冲，再关进程。socket 逐字节转发，
        # 这里不需要依赖 terminate 时 QEMU 的 flush。
        ta, tb = ca.text(settle=0.3), cb.text(settle=0.3)
        close_console(ca)
        close_console(cb)
        stop(b)
        stop(a)
    # 放宽的观测：即使失败也把两边的 line 行打出来，方便定位
    for l in ta.splitlines():
        if "[line]" in l:
            print("    A:", l)
    for l in tb.splitlines():
        if "[line]" in l:
            print("    B:", l)
    print("  case two   : %s" % ("ok" if ok else "fail"))
    return ok


def case_single_node(qemu, a_elf, tmp):
    """防假绿：没有 B，A 必须自己判失败。

    console 这里**必须**走 socket 而不是 `-serial file:`：QEMU 的 file
    chardev 会把不满一块的输出先攒着，20 s 才写出的那一行在 70 s 时仍可能
    读不到（实测如此），于是门禁会把"跑对了"读成"没跑出来"。socket 是
    逐字节转发的，读到就算数。
    """
    port = free_port()
    ch_a = "socket,id=bus,host=127.0.0.1,port=%d,server=on,wait=off" % port
    a, con = run_qemu(qemu, a_elf, tmp, "solo", ["-chardev", ch_a])
    if con is None:
        stop(a)
        print("  case solo  : console chardev 没连上 -> fail")
        return False
    ok = False
    try:
        # 2 s 判据 + 余量。这一档的 ELF 用 -DLINE_GIVE_UP_TICKS=4000u 编，
        # 所以不需要为了等判据而空转 20 s。
        deadline = time.time() + 25
        while time.time() < deadline:
            if judge_single_node(con.text()):
                ok = True
                break
            if a.poll() is not None:
                break
            time.sleep(0.2)
    finally:
        ta = con.text(settle=0.3)
        close_console(con)
        stop(a)
    for l in ta.splitlines():
        if "[line]" in l:
            print("    A:", l)
    if not ok:
        print("    [diag] A console tail: %r" % ta[-600:])
    print("  case solo  : %s" % ("ok" if ok else "fail"))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--budget", type=float, default=45.0,
                    help="双节点等待上限（秒）")
    args = ap.parse_args()

    print("== bus_line_two_nodes ==")
    print("[self-check]")
    sc = self_check()
    if sc:
        for m in sc:
            print("  SELF-CHECK FAIL:", m)
        print("RESULT: FAIL")
        return 1
    print("  ok（判据能区分好/坏日志）")

    qemu = find_qemu(args.qemu)
    if qemu is None:
        print("RESULT: FAIL (找不到 qemu-system-loongarch64，可用 --qemu 指定)")
        return 1
    print("qemu =", qemu)

    elves = {}
    if args.no_build:
        for t in ("probe", "a", "b", "solo"):
            e = os.path.join(ROOT, BUILD_DIR.format(t).replace("/", os.sep), "ark_qemu_la64.elf")
            if not os.path.exists(e):
                print("RESULT: FAIL (--no-build 但缺 %s)" % e)
                return 1
            elves[t] = e
    else:
        print("[build]")
        for t in ("probe", "a", "b", "solo"):
            e = build_tier(t, TIERS[t], args.jobs)
            if e is None:
                print("RESULT: FAIL (构建 %s 失败)" % t)
                return 1
            elves[t] = e
            print("  %s -> %s" % (t, os.path.relpath(e, ROOT)))

    tmp = tempfile.mkdtemp(prefix="ark_busline_")
    results = {}
    try:
        print("[cases]")
        results["probe"] = case_probe(qemu, elves["probe"], tmp)
        results["two"] = case_two_nodes(qemu, elves["a"], elves["b"], tmp, args.budget)
        results["solo"] = case_single_node(qemu, elves["solo"], tmp)
    finally:
        # QEMU 起不来的极端情形也收一遍，避免留孤儿
        shutil.rmtree(tmp, ignore_errors=True)

    print("[summary]")
    for k in ("probe", "two", "solo"):
        print("  %-6s %s" % (k, "PASS" if results.get(k) else "FAIL"))
    allok = all(results.get(k) for k in ("probe", "two", "solo"))
    print("RESULT: %s" % ("PASS" if allok else "FAIL"))
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
