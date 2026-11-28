#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""lwIP tcpip 线程栈预算（LA64 十档低频 INE 停机）的回归门禁。

为什么需要这一道
----------------
2026-11-23 起，LA64 十档镜像（fork+cow+shm+futex+filemap+mprotect+select+
socket+thread+appq）在 QEMU 上「偶尔」停机，签名是

    [Ark RTOS] FATAL: estat=d0000 ecode=0x0000000d era=800 badv=800 badi=0

也就是跳到零页取指（INE）。它低频、不可稳定复现，而且在开启之前的三道
检测里全都看不见：

  1. `ARKRTOS_USE_STACK_GUARD` 默认 0 —— 守卫检查整段不编译；
  2. `ARKRTOS_USE_STACK_CHECK` 的判据是 `old_psp < stack_bottom`，而本次
     sp 始终落在**自己的栈区间内**（只是把 528 字节的陷阱帧压到了栈底
     之下），所以它不报警；
  3. lwIP 本身一句错都不打。

根因（已坐实到机制级，见 docs/已知限制.md）：`sys_arch.c` 里
`LWIP_THREAD_STACK_W` 原先只在 `ARKRTOS_USE_NET_VIRTIO==1` 时给 8 KB，
其余一律 2 KB。而**只要 LINUX_SOCKET 打开**，tcpip 线程就要跑
netconn/socket 的收发与 select 唤醒，2 KB 槽被 100% 用满（0xcd 填充图案
一个不剩），528 字节陷阱帧溢出到槽下方，先改坏相邻的 `g_lwip_task_stack`
（task 11）里保存的 ra → 恢复时跳到 0x800。

所以判据分两层：源码层的栈预算不许回退，运行层必须真的跑过那段窗口。

判据（全过才 0）
----------------
  A. 源码 pin：`sys_arch.c` 的 `LWIP_THREAD_STACK_W` 有两条定义，取值必须
     是 2048 / 512；取 2048 那条的 `#if` 必须**同时**提到
     `ARKRTOS_USE_NET_VIRTIO` 与 `ARKRTOS_USE_LINUX_SOCKET`（只写净卡就是
     本次缺陷的原状）；`lwip_port.c` 的 `g_lwip_task_stack` 在 socket 档也
     必须是 1024 字；`sys_thread_new` 的上界仍然按 `LWIP_THREAD_STACK_W * 4`
     拒绝（栈变大了，这道闸门不能顺手放宽成常数）。
  B. 解析器自检：把合成日志喂给同一组判据 —— 含 FATAL、缺 `[fork] PASS`、
     缺「fork 之后仍在跑」的读数，三种坏法必须判红；干净日志必须判绿。
  C. QEMU 真读数：clean + 十档 EXTRA_CFLAGS 重编，`-icount shift=5` 纯跑
     （不做任何单步 —— gdb 单步会扰动 icount 相位，历史实测因此复现不出），
     串口上必须同时看到：无 `FATAL`、`[fork] PASS` 出现过、**在 `[fork]`
     结束之后又跑了 >= 10 轮 `[A] round=`**（故障窗口就在 fork 刚结束那几
     毫秒），且 `stack check` 报 `check ok`、没有
     `stack bottom flag overwritten`。
  D. 构建复位：本门禁为取证会 clean + 带 EXTRA_CFLAGS 重编（make 不跟踪
     EXTRA_CFLAGS），跑完必须复位成默认档，否则后续任何一次 `make` 都会
     复用这一档的 .o（默认档悄悄变成十档）。

覆盖：socket 档 tcpip 线程栈预算、溢出后被踩的相邻对象、确定性复现条件
（`-icount shift=5`）、以及「栈用量可见」这条读数的存在。
不覆盖：真实板卡上的栈预算（本机无 LA64 硬件）、ARM F427 档的 2 KB（那一
档不开 socket，链接预算是另一件事，由 stack_guard_gate 与链接期 L6406E 守）。

用法
----
    py -3 tools/icount_stack_gate.py                # 源码 pin + 自检 + 重编跑 QEMU
    py -3 tools/icount_stack_gate.py --source-only    # 只跑 A/B（快）

退出码 0 = 全过；1 = 任一判据不成立 / 缺 QEMU 或工具链（缺工具是 FAIL）。
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAKEFILE = "example/qemu_loongarch64/Makefile"
BUILD_DIR = os.path.join(ROOT, "build", "qemu_loongarch64")
ELF = os.path.join(BUILD_DIR, "ark_qemu_la64.elf")

SYS_ARCH = os.path.join(ROOT, "kernelsrc", "components", "lwip", "port", "sys_arch.c")
LWIP_PORT = os.path.join(ROOT, "kernelsrc", "components", "lwip", "port", "lwip_port.c")

# 十档：与 docs 里那条确定性复现用的命令逐字一致。少一条就不是这一档。
TIER_FLAGS = [
    "-DARKRTOS_USE_LINUX_FORK=1",
    "-DARKRTOS_USE_PAGING_DEMAND=1",
    "-DARKRTOS_USE_PAGING_COW=1",
    "-DARKRTOS_USE_PAGING_SHM=1",
    "-DARKRTOS_USE_LINUX_FUTEX=1",
    "-DARKRTOS_USE_PAGING_FILEMAP=1",
    "-DARKRTOS_USE_PAGING_MPROTECT=1",
    "-DARKRTOS_USE_LINUX_SELECT=1",
    "-DARKRTOS_USE_LINUX_SOCKET=1",
    "-DARKRTOS_USE_LWIP=1",
    "-DARKRTOS_USE_LINUX_THREAD=1",
    "-DARKRTOS_USE_APP_QUOTA=1",
    "-DARKRTOS_APP_MAX_TASKS=3",
]

QEMU_CANDIDATES = [
    "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe",
    "qemu-system-loongarch64",
]

PORT = 45713

PASSES = []
FAILS = []


def read_text(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


def check(name, cond, detail_ok, detail_bad):
    if cond:
        PASSES.append(name)
        print("  ok   %-22s %s" % (name, detail_ok))
    else:
        FAILS.append(name)
        print("  FAIL %-22s %s" % (name, detail_bad))


# ---------------------------------------------------------------- A. 源码 pin

def _sites(lines, needle):
    """所有含 needle 的代码行（跳过注释行），每行配它最近的 #if/#else 守卫。

    不要求行首是 '#'：`static uint32 g_lwip_task_stack[1024];` 这类定义不是
    宏，但同样要在守卫里。注释行必须跳过，否则上面那段解释文字会被当成定义。
    """
    out = []
    for i, ln in enumerate(lines):
        s = ln.lstrip()
        if s.startswith("/*") or s.startswith("*"):
            continue
        if needle not in ln:
            continue
        guard = ""
        for j in range(i - 1, -1, -1):
            g = lines[j].lstrip()
            if g.startswith("#if") or g.startswith("#else"):
                guard = g
                break
        out.append((ln, guard))
    return out


def check_source():
    src = read_text(SYS_ARCH)
    lines = src.splitlines()
    defs = []
    for ln, guard in _sites(lines, "define LWIP_THREAD_STACK_W"):
        tok = ln.split("define LWIP_THREAD_STACK_W", 1)[1].strip().split()[0]
        defs.append((tok, guard))
    vals = sorted(tok.rstrip("uU") for tok, _g in defs if tok)

    check("stack-value-pair",
          vals == ["2048", "512"],
          "两个取值 = 2048 / 512（8 KB / 2 KB）",
          "取值不是 {2048, 512}：%r（2 KB 是本次缺陷的原状）" % (vals,))

    big_guard = ""
    for tok, guard in defs:
        if tok.rstrip("uU") == "2048":
            big_guard = guard
    check("stack-socket-cond",
          ("ARKRTOS_USE_NET_VIRTIO" in big_guard) and
          ("ARKRTOS_USE_LINUX_SOCKET" in big_guard),
          "8 KB 那条的 #if 同时含 NET_VIRTIO 与 LINUX_SOCKET",
          "8 KB 那条的 #if 缺条件：%r（只看净卡就是本次缺陷的原状）" % big_guard)

    port = read_text(LWIP_PORT)
    plines = port.splitlines()
    ppairs = []
    for ln, guard in _sites(plines, "static uint32 g_lwip_task_stack["):
        m = re.search(r"g_lwip_task_stack\[(\d+)\]", ln)
        if m:
            ppairs.append((m.group(1), guard))
    big_ok = any(v == "1024" and "ARKRTOS_USE_LINUX_SOCKET" in g for v, g in ppairs)
    small_ok = any(v == "256" and g.startswith("#else") for v, g in ppairs)
    check("lwip-task-stack",
          big_ok and small_ok,
          "g_lwip_task_stack socket 档 1024 字（4 KB）/ 其余 256 字（1 KB）",
          "g_lwip_task_stack 两条定义不对：%r" % (ppairs,))

    check("thread-new-bound",
          "LWIP_THREAD_STACK_W * 4u" in src or "LWIP_THREAD_STACK_W * 4" in src,
          "sys_thread_new 上界仍按 LWIP_THREAD_STACK_W * 4 拒绝",
          "sys_thread_new 的栈上界不再跟随 LWIP_THREAD_STACK_W（栈变大后被改成了常数？）")


# ------------------------------------------------------------ B. 解析器自检

FATAL_RE = re.compile(r"\[Ark RTOS\] FATAL:")


STK_RE = re.compile(r"\[STK\] task (\d+):")
DEMO_MARK = "stack guard demo"
# 标记行带任务号（演示先打完再踩栈底），判据按号对账而不是按行距窗口。
DEMO_ID_RE = re.compile(r"stack guard demo: task (\d+)")


def judge_qemu(txt):
    """把 QEMU 串口全文变成判据。返回问题列表（空 = 通过）。

    注意：这块板（`board/qemu_loongarch64/arkrtos_board_config.h`）把
    `ARKRTOS_USE_STACK_GUARD` 强制打开，而镜象里有一个**故意**踩自己栈底字
    的演示任务（`example/qemu_loongarch64/main.c`），所以
    "[STK] task N: stack bottom flag overwritten" 本身不是故障。

    对账方式是**按任务号**，不是按行距窗口：演示任务先打一行
    "[boot] stack guard demo: task N about to clobber ..." 再去踩，
    判据是「被报的号 == 标记里的号」；任何别的号被报都算红。
    历史故障正是这一形态：演示的 task 6 之后，task 12（tcpip）也被报 ——
    而那时它踩坏的是相邻任务的保存帧，所以报得比停机早、但拦不住。

    为什么不按行距猜（2026-11-28 实测）：旧口径要求 STK 行落在标记行之后 40 行内，
    而标记以前是在**踩完之后**才打的；那一次守卫在打印到第 9 个字节时就把任务
    摘了，串口上只剩 "[boot] sta"（标记文本一起没了），于是健康镜像被判红。
    现在两边都改了：演示先打完再踩（任务号写在行里），判据按号对账。
    """
    bad = []
    if FATAL_RE.search(txt):
        m = FATAL_RE.search(txt)
        bad.append("出现内核 FATAL：%s" % txt[m.start():m.start() + 90].splitlines()[0])
    if "[fork] PASS" not in txt:
        bad.append("没有看到 [fork] PASS（这一档没跑到该跑的段）")

    lines = txt.splitlines()
    demo_ids = set()
    for ln in lines:
        m = DEMO_ID_RE.search(ln)
        if m:
            demo_ids.add(int(m.group(1)))
    if demo_ids:
        extras = sorted(set(int(m.group(1)) for m in STK_RE.finditer(txt)
                            if int(m.group(1)) not in demo_ids))
        if extras:
            bad.append("非演示任务的栈底被踩：task %s（tcpip 栈溢出会这样报）"
                       % ",".join(str(x) for x in extras))
        elif not STK_RE.search(txt):
            bad.append("演示任务自己没有被栈底守卫报出来（守卫没生效就不会发现溢出）")
    elif STK_RE.search(txt):
        bad.append("有 [STK] 报告但没有带任务号的演示标记（认不出被报的是不是演示任务）")
    # 故障窗口在 fork 刚结束之后：必须证明「fork 之后还在跑」。
    idx = txt.find("[fork] PASS")
    if idx >= 0:
        tail = txt[idx:]
        rounds = tail.count("[A] round=")
        if rounds < 10:
            bad.append("fork 之后只跑了 %d 轮 [A] round=（故障窗口就在这段时间）" % rounds)
    return bad


def self_check():
    demo = ("[boot] stack guard demo: task 6 about to clobber its own bottom "
            "guard word\n"
            "[STK] task 6: stack bottom flag overwritten\n")
    good = ("boot ok\n" + demo + "[fork] PASS: child ran in its own space, exit 42 reaped\n"
            + "".join("[A] round=%d tick=%d\n" % (i, i) for i in range(20)))
    cases = [
        ("clean-with-demo", good, 0),
        ("with-fatal", good + "[Ark RTOS] FATAL: estat=d0000 ecode=0x0000000d era=800\n", 1),
        ("no-fork-pass", good.replace("[fork] PASS", "[fork] almost"), 1),
        ("stopped-right-after-fork",
         demo + "[fork] PASS: ok\n" + "".join("[A] round=%d\n" % i for i in range(9)), 1),
        ("tcpip-guard-stomped", good + "[STK] task 12: stack bottom flag overwritten\n", 1),
        ("stray-stk-before-demo", "[STK] task 9: stack bottom flag overwritten\n" + good, 1),
        # 标记与被报的号对不上：认错人就是红，不允许「附近有标记就算过」
        ("demo-id-mismatch",
         "[boot] stack guard demo: task 6 about to clobber\n"
         "[STK] task 7: stack bottom flag overwritten\n"
         "[fork] PASS: ok\n" + "".join("[A] round=%d\n" % i for i in range(12)), 1),
        # 标记与 STK 之间隔着上百行也不能红：判据是号不是行距
        ("far-away-demo", good.replace(
            "[STK] task 6: stack bottom flag overwritten\n",
            "".join("[LA64U] filler %d\n" % i for i in range(200))
            + "[STK] task 6: stack bottom flag overwritten\n"), 0),
        # 有 STK 但没有带号的标记：认不出人，宁可判红
        ("stk-without-demo-id",
         "[boot] stack guard demo\n"
         "[STK] task 6: stack bottom flag overwritten\n"
         "[fork] PASS: ok\n" + "".join("[A] round=%d\n" % i for i in range(12)), 1),
        # 标记在、守卫却没报：演示要证明的是「守卫能报」，一声不响不算过
        ("demo-never-reported",
         "[boot] stack guard demo: task 6 about to clobber\n"
         "[fork] PASS: ok\n" + "".join("[A] round=%d\n" % i for i in range(12)), 1),
    ]
    bad = []
    for name, txt, want in cases:
        got = len(judge_qemu(txt))
        if (got > 0) != (want > 0):
            bad.append("%s: 期望%s，实得 %d 条问题" % (name, "判红" if want else "判绿", got))
    check("parser-self-check",
          not bad,
          "%d 种合成日志全部按预期判红/判绿（演示标记与 STK 按任务号对账）"
          % len(cases),
          "; ".join(bad))


# --------------------------------------------------------- C. 构建 + QEMU 取证

def find_qemu():
    for c in QEMU_CANDIDATES:
        if os.path.isabs(c):
            if os.path.exists(c):
                return c
        else:
            for d in os.environ.get("PATH", "").split(os.pathsep):
                p = os.path.join(d, c + ".exe")
                if os.path.exists(p):
                    return p
    return None


def build(tier):
    env = dict(os.environ)
    if tier:
        env["EXTRA_CFLAGS"] = " ".join(TIER_FLAGS)
    else:
        env.pop("EXTRA_CFLAGS", None)
    r = subprocess.run(["make", "-f", MAKEFILE, "clean"], cwd=ROOT, env=env,
                       capture_output=True, text=True)
    if r.returncode != 0:
        return "clean 失败：%s" % ((r.stderr or r.stdout)[-300:])
    r = subprocess.run(["make", "-f", MAKEFILE, "-j8"], cwd=ROOT, env=env,
                       capture_output=True, text=True)
    if r.returncode != 0:
        tail = ((r.stderr or "") + (r.stdout or ""))[-500:]
        return "构建失败：%s" % tail
    return ""


def run_qemu(qemu, seconds, log_path):
    """TCP 串口起 QEMU，轮询发 stack check，收 seconds 秒的全文。"""
    f = open(log_path, "wb")
    p = subprocess.Popen(
        [qemu, "-machine", "virt", "-kernel", "ark_qemu_la64.elf",
         "-nographic", "-monitor", "none",
         "-icount", "shift=5,align=off,sleep=off",
         "-serial", "tcp:127.0.0.1:%d,server=on,wait=off" % PORT],
        cwd=BUILD_DIR, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT)
    buf = b""
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
        s.settimeout(0.5)
        t0 = time.time()
        asked = 0
        while time.time() - t0 < seconds:
            if asked < 3 and (time.time() - t0) > 2.0 + asked * 1.5:
                s.sendall(b"\r")
                time.sleep(0.2)
                s.sendall(b"stack check\r")
                asked += 1
            try:
                d = s.recv(65536)
                if d:
                    buf += d
            except socket.timeout:
                pass
        s.close()
    finally:
        p.kill()
        p.wait()
        f.write(b"\n===== TAIL =====\n" + buf)
        f.close()
    return buf.decode("utf-8", "replace")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-only", action="store_true")
    a = ap.parse_args(argv)

    print("[A] 源码 pin")
    check_source()

    print("[B] 解析器自检")
    self_check()

    if a.source_only:
        print("\n%d passed, %d failed (--source-only：未跑 QEMU)" % (len(PASSES), len(FAILS)))
        return 0 if not FAILS else 1

    print("[C] 构建 + QEMU 取证")
    qemu = find_qemu()
    if not qemu:
        check("qemu-present", False, "", "找不到 qemu-system-loongarch64（缺工具是 FAIL）")
    else:
        err = build(tier=True)
        if err:
            check("tier-build", False, "", err)
        else:
            check("tier-build", True, "十档镜像编得过（%d B）" % os.path.getsize(ELF), "")
            log = os.path.join(ROOT, "build", "icount_stack_gate.log")
            txt = run_qemu(qemu, 45.0, log)
            problems = judge_qemu(txt)
            check("qemu-shift5", not problems,
                  "shift=5 纯跑 45 s：无 FATAL、[fork] PASS、fork 后仍跑多轮",
                  "; ".join(problems))
            check("stack-check-ok", "stack: check ok" in txt,
                  "shell 的 stack check 报 check ok（栈用量可见）",
                  "没有拿到 `stack: check ok`（读数不存在就不能宣称栈够用）")
            goal = re.search(r"^\s*12\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)%", txt, re.M)
            if goal:
                total, used, free_, pct = (int(goal.group(i)) for i in range(1, 5))
                check("tcpip-stack-headroom",
                      total >= 8192 and pct < 90,
                      "task 12 tcpip 栈 total=%d used=%d（%d%%）free=%d" % (total, used, pct, free_),
                      "task 12 栈 total=%d used=%d（%d%%）——余量不足或没换成 8 KB"
                      % (total, used, pct))
            else:
                check("tcpip-stack-headroom", False, "",
                      "没读到 task 12 的 `stack` 行（读数缺失不等于通过）")
            print("     日志 = %s（build*/ 在 .gitignore 里，不会入库）" % log)

            print("[D] 复位默认档")
            err = build(tier=False)
            check("restore-default-build", not err, "默认档已复位", err)

    print("\n%d passed, %d failed" % (len(PASSES), len(FAILS)))
    if not FAILS:
        print("ICOUNT-STACK-GATE OK")
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
