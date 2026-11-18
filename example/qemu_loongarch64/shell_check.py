#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""QEMU LoongArch64 内核 Shell 的端到端检查。

为什么要单独一个脚本：`make run` 的 `-serial file:out.txt` 是单向的，只能
证明 shell 把横幅和提示符打出来了；而 shell 是一条**双向**链路
（输入 -> 行编辑器 -> 命令分发 -> 输出），只有把 host 的字节真的送进去并且
断言命令的效果，才能说它"能用"。所以这里把 ns16550a 接到 TCP chardev。

判据（任一不满足即 FAIL，退出码非 0）：
  1. 出现 shell 横幅 "Ark RTOS kernel shell" 与提示符 "ark@arkrtos:"；
  2. 启动期存储自检 "[FS] PASS" 出现（shell 的 ls/cat 依赖同一条 VFS）；
  3. `help` 列出 blk / mount / reboot 三个命令；
  4. `uname` 报出内核名；
  5. `dt` 报出本机 model 与串口节点（compatible=ns16550a、地址 0x1fe001e0）——
     这张表由 dts/qemu_loongarch64.dts 生成，改了树不改表会被 ci_gate 抓住；
  6. `ls /mnt/ram` 列出 hello.txt（证明 ls 走的是 VFS，不是编造的目录）；
  7. `cat /proc/version` 打印内核版本行（procfs 在 shell 里可读）；
  8. `stack` 的任务数 >= 5、与 `ps` 的 PID 行数相等、`0 unmeasured`
     且 `0 at/over 90%`（不写死任务数，见下面的坑）；
  9. `utest` 汇总行是 "N matched, N passed, 0 failed"，且 N >= 1
     （不绑用例名：用例表随开关变；但**必须绑匹配数**，见下面的坑）；
 10. 全程没有 UNEXPECTED / FATAL / FAILED。

第 8 条的教训（2026-11-18）：这条判据原本写死 "5 task(s)"，示例后来
多注册了 ELF/MOD 两个演示任务、总数变 9，于是脚本长期 FAIL，而失败
跟被测对象无关。改成「下限 + 与 ps 交叉核对」，既不写死也不放水。

第 9 条的教训（2026-11-16）：这条判据最早只查 ", 0 failed,"，而板端当时的
读数是 "0 matched, 0 passed, 0 failed, 0 skipped" —— 一条用例都没跑，
判据却通过了，是彻底的假绿。根因是示例目标的 start.S 从不遍历构造函数表，
而 UTEST_CASE 靠 __attribute__((constructor)) 自注册（详见
docs/QEMU模拟器移植与验证.md §5.13.3）。所以这里改成同时要求
matched >= 1 且 matched == passed。

跑法（仓库根目录）：

    python example/qemu_loongarch64/shell_check.py
"""

import os
import re
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BUILD = os.path.join(ROOT, "build", "qemu_loongarch64")
ELF = os.path.join(BUILD, "ark_qemu_la64.elf")

QEMU = os.environ.get(
    "QEMU_LA64",
    r"D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe",
)


def _free_port():
    """先抢一个空端口再放掉。

    固定端口会在反复跑时撞上上一次没退干净的 QEMU：脚本连上去、看到完整的
    启动横幅（其实是上一个实例的），然后命令发进一个已经没人收的 chardev，
    表现为"输入没反应"。RV32 那边在 uart_irq_check.py 里踩过一次。
    """
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


PORT = int(os.environ.get("SHELL_CHECK_PORT", "0")) or _free_port()


def fail(msg, log=""):
    print("FAIL: " + msg)
    if log:
        print("---- 串口原始输出 ----")
        print(log)
    return 1


def main():
    if not os.path.isfile(ELF):
        return fail("找不到 " + ELF +
                    "，先在仓库根目录 make -f example/qemu_loongarch64/Makefile")

    cmd = [
        QEMU,
        # 传相对文件名 + cwd=BUILD：仓库路径含非 ASCII（中文）字符，QEMU 在
        # Windows 上打不开那种绝对路径的 -kernel 且不报错，表现为永远连不上。
        "-kernel", os.path.basename(ELF),
        "-display", "none",
        # wait=on：QEMU 先监听、等脚本连上再开始执行，否则启动横幅会在
        # connect 完成之前就打完（实测丢横幅）。
        "-chardev", "socket,id=c0,host=127.0.0.1,port=%d,server=on,wait=on" % PORT,
        "-serial", "chardev:c0",
    ]

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        cwd=BUILD,
    )

    log = ""
    try:
        sock = None
        for _ in range(100):
            try:
                sock = socket.create_connection(("127.0.0.1", PORT), 0.2)
                break
            except OSError:
                time.sleep(0.1)
        if sock is None:
            return fail("连不上 QEMU 的 chardev 端口 %d" % PORT)

        sock.settimeout(0.2)

        def pump(seconds):
            nonlocal log
            end = time.time() + seconds
            while time.time() < end:
                try:
                    chunk = sock.recv(4096)
                    if chunk:
                        log += chunk.decode("utf-8", "replace")
                except socket.timeout:
                    pass
                except OSError:
                    break
            return log

        def run(cmdline, wait=1.2):
            nonlocal log
            mark = len(log)
            sock.sendall(cmdline.encode("ascii") + b"\r")
            pump(wait)
            return log[mark:]

        # 1) 等 shell 就绪：横幅 + 提示符。启动期还要跑任务 A/B、存储自检
        #    （format + 写读 + NOR 负对照），给足 10 秒。
        pump(10.0)
        if "Ark RTOS kernel shell" not in log:
            return fail("没有出现 shell 横幅（shell 任务没起来）", log)
        if "ark@arkrtos:" not in log:
            return fail("没有出现提示符 ark@arkrtos:", log)
        if "[FS] PASS" not in log:
            return fail("存储自检没通过（shell 依赖的 VFS 不可信）", log)

        # 2) help
        out = run("help")
        for name in ("blk", "mount", "reboot"):
            if name not in out:
                return fail("help 输出里没有命令 %r\n%s" % (name, out), log)

        # 3) uname
        out = run("uname")
        if "arkrtos" not in out.lower():
            return fail("uname 没报出内核名\n" + out, log)

        # 4) dt：本机设备树。串口节点必须带真实地址，否则表是空壳。
        out = run("dt")
        if "qemu" not in out.lower():
            return fail("dt 没报出本机 model\n" + out, log)
        if "ns16550a" not in out or "0x1fe001e0" not in out.lower():
            return fail("dt 里没有串口节点或地址不对\n" + out, log)

        # 5) ls /mnt/ram：证明 shell 的 ls 真的走 VFS
        out = run("ls /mnt/ram")
        if "hello.txt" not in out:
            return fail("ls /mnt/ram 没有列出 hello.txt\n" + out, log)

        # 6) cat /proc/version：procfs 在 shell 里可读
        out = run("cat /proc/version")
        if "Ark RTOS" not in out:
            return fail("cat /proc/version 没打印版本行\n" + out, log)

        # 7) stack：任务表里必须有 shell 任务，且栈计量真的在算。
        #    这里**不写死任务数**（2026-11-18 的教训）：本判据最早写的是
        #    "5 task(s)"（A/B/FS/shell/idle），后来示例又注册了 ELF/MOD
        #    两个演示任务，总数变 9，判据就一直红着，而红的原因跟被测对象
        #    无关。改成三条不随示例成长的判据：
        #      a) >= 5（A/B/FS/shell/idle 是下限，少一个说明有任务没起来）；
        #      b) stack 的任务数与 ps 的 PID 行数**相等**——两个独立视图
        #         对不上就是有一边在编数字，比一个常数可靠；
        #      c) "0 unmeasured" 与 "0 at/over 90%" 同时出现。
        out = run("stack")
        m = re.search(r"stack:\s*(\d+)\s+task\(s\)", out)
        if m is None:
            return fail("stack 汇总行不可解析\n" + out, log)
        n_stack = int(m.group(1))
        if n_stack < 5:
            return fail("任务数 %d < 5（A/B/FS/shell/idle 至少 5 个）\n"
                        % n_stack + out, log)
        out_ps = run("ps")
        n_ps = len(re.findall(r"^\s+\d+\s+\d+\s+\S+\s+0x[0-9A-Fa-f]{8}",
                              out_ps, re.M))
        if n_ps != n_stack:
            return fail("stack 报 %d 个任务，ps 列了 %d 行 PID，两个视图不一致"
                        "\n" + out_ps, log)
        if "0 unmeasured" not in out:
            return fail("栈计量没有生效（unmeasured 非零）\n" + out, log)
        if "0 at/over 90%" not in out:
            return fail("有任务栈用量到/超 90%\n" + out, log)

        # 8) utest：板端单元测试全量。不绑单个用例的名字（用例表会随功能
        #    开关变），但**必须绑匹配数**：只查 ", 0 failed," 会把
        #    "0 matched, 0 passed, 0 failed" 判成通过（真实踩过，见文件头）。
        out = run("utest", wait=15.0)
        m = re.search(r"(\d+) matched,\s*(\d+) passed,\s*0 failed", out)
        if not m:
            return fail("utest 汇总行不可解析或不是 0 failed\n" + out, log)
        matched, passed = int(m.group(1)), int(m.group(2))
        if matched < 1:
            return fail("utest 一条用例都没匹配到（构造函数表没跑？）\n" + out, log)
        if matched != passed:
            return fail("utest 匹配 %d 通过 %d\n%s" % (matched, passed, out), log)

        # 9) 全程负向标记
        for bad in ("UNEXPECTED", "FATAL", "FAILED"):
            if bad in log:
                return fail("全程出现 %s" % bad, log)

        print("PASS: LA64 内核 Shell 端到端通"
              "（横幅/help/uname/dt/ls/cat/stack/utest 全部通过）")
        return 0
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())
