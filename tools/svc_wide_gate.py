#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""宽参 ABI（ARKRTOS_USE_WIDE_SVC_ARGS）的拒绝路径与进程状态清空顺序的门禁。

为什么需要这一道
----------------
1. 宽内核 + 窄 App 时，旧的实现只把 -1 写进返回寄存器：App 在用户态继续跑，
   每一条 SVC 都读错参数、都返回 -1，串口上看起来就是"卡住"——与"内核没
   收到调用"长得一样。现在拒绝路径必须**终止调用任务**（走
   arkrtos_task_kill_internal，与 THREAD_SUB_KILL 同一条路），这样 exec/wait
   能观察到它结束、装载器台账能记一笔。判据不是"打了日志"，是"守卫里真的
   调了 reject、reject 里真的杀了任务"——少任何一环都会退回静默空转。

2. Linux 兼容层的状态（fd 表、堆游标、信号表）是内核全局的，只能属于
   "正在跑的那一个程序"。清空必须发生在**新任务被挂进就绪集之前**：
   放在 arkrtos_elf_exec_space 尾部时，新任务可能先跑起来，第一次 brk/mmap
   读到的是上一个程序留下的游标。实测签名是 fixture 的 stage1 brk(0) 比
   窗口堆基点高 8192，而且随代码布局时红时绿（同一份源码、只加 800 字节
   就翻）。所以这条顺序也钉进门禁，位置错了直接红。

判据
----
  A. 头文件里 reject 有原型；守卫同时含 `-1` 返回与 reject 调用；
  B. 窄档的守卫展开成空语句（默认档体积与行为零变化）；
  C. reject 的实现里有日志、有 `current_task_id > 0` 的前置、真的调
     arkrtos_task_kill_internal；
  D. wide_ok 仍按 ARKRTOS_SVC_SUB_WORD 与整块 sizeof 判；
  E. task.c 里 SVC_WIDE_GUARD 的使用点不少于 15 个（当前正好 15 个；
     每个取参数块的 svc_* 处理函数都要过守卫，少一个就是漏一条入口）；
  F. elf.c 里清空只有一处，且在 `id = elf_start_task(` 之前，并带
     ARKRTOS_USE_LINUX_ABI 条件编译；
  G. 变异自检：把上面每一条各自破坏一次，verify() 必须报错——否则这些
     断言只是"看起来在查"。

不覆盖：宽档运行期读数本身（QEMU 取证见 docs/MMU进程模型与ELF装载.md §11
与 docs/SVC服务号与子命令表.md 的"参数块字宽"一节）；mini_sdk/arkc 仍是
32 位基，不在本机制内。
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HDR = os.path.join(ROOT, "kernelsrc", "include", "arkrtos_kernel_check.h")
TASK = os.path.join(ROOT, "kernelsrc", "src", "arkrtos_task.c")
ELF = os.path.join(ROOT, "kernelsrc", "src", "arkrtos_elf.c")
ABI = os.path.join(ROOT, "kernelsrc", "include", "arkrtos_svc_abi.h")


def read(path):
    with io.open(path, "rb") as f:
        return f.read().decode("utf-8", "replace").replace("\r\n", "\n")


def body_of(text, sig):
    """取函数体：从签名到其后第一个行首 `}`。取不到返回 None。"""
    i = text.find(sig)
    if i < 0:
        return None
    j = text.find("\n}\n", i)
    if j < 0:
        return None
    return text[i:j]

def verify(hdr, task, elf, abi):
    bad = []

    # A. 原型 + 守卫两头都在
    if "void arkrtos_kernel_svc_wide_reject(void);" not in hdr:
        bad.append("A: 头文件里没有 arkrtos_kernel_svc_wide_reject 的原型")
    if "arkrtos_kernel_svc_wide_reject();" not in hdr:
        bad.append("A: SVC_WIDE_GUARD 没有调用 reject（拒绝只会回 -1，App 空转）")
    if "ARKRTOS_SVC_RET(p_svc_ctx, (uint32)(-1));" not in hdr:
        bad.append("A: SVC_WIDE_GUARD 丢了拒绝返回值")
    if "#if (ARKRTOS_SVC_WIDE_ARGS == 1)" not in hdr:
        bad.append("A: 守卫没有按 ARKRTOS_SVC_WIDE_ARGS 分档")

    # B. 窄档展开为空
    if "do { } while(0)" not in hdr:
        bad.append("B: 窄档的 SVC_WIDE_GUARD 不再展开成空语句（默认档体积会变）")

    # C. reject 的实现（只认函数体内，不认文件里别处的同名调用）
    rb = body_of(task, "void arkrtos_kernel_svc_wide_reject(void)")
    if rb is None:
        bad.append("C: task.c 里没有 reject 的实现")
    else:
        if "arkrtos_task_kill_internal();" not in rb:
            bad.append("C: reject 没有终止调用任务（回到静默空转）")
        if "if(arkrtos_current_task_id > 0)" not in rb:
            bad.append("C: reject 缺 current_task_id > 0 前置（内核上下文会走收尸路径）")
        if "wide-args kernel got a 32-bit param block" not in rb:
            bad.append("C: reject 缺字宽不符的日志")
        if "ARKRTOS_USE_WIDE_SVC_ARGS" not in rb:
            bad.append("C: 日志没有点名开关，用户不知道该对齐哪一档")

    # D. wide_ok 的判据（同样只看函数体）
    wb = body_of(task, "uint8 arkrtos_kernel_svc_wide_ok(const void *p_blk)")
    if wb is None:
        bad.append("D: task.c 里没有 wide_ok 的实现")
    else:
        if "ARKRTOS_SVC_SUB_WORD" not in wb:
            bad.append("D: wide_ok 不再看子命令标记位")
        if "sizeof(arkrtos_svc_arg_t)" not in wb:
            bad.append("D: wide_ok 不再校验整块大小")

    # E. 守卫覆盖面
    n = task.count("SVC_WIDE_GUARD(p_svc_ctx")
    if n < 15:
        bad.append("E: SVC_WIDE_GUARD 只剩 %d 个使用点（应 >= 15）" % n)

    # F. 兼容层状态清空的位置
    if elf.count("arkrtos_linux_abi_reset();") != 1:
        bad.append("F: elf.c 里 arkrtos_linux_abi_reset 出现 %d 次（应为 1）"
                   % elf.count("arkrtos_linux_abi_reset();"))
    i_reset = elf.find("arkrtos_linux_abi_reset();")
    i_start = elf.find("id = elf_start_task(")
    if (i_reset < 0) or (i_start < 0) or (i_reset > i_start):
        bad.append("F: 清空没有落在 elf_start_task 之前（新任务可能先跑一步）")
    else:
        window = elf[max(0, i_reset - 900):i_reset]
        if "ARKRTOS_USE_LINUX_ABI" not in window:
            bad.append("F: 清空没有被 ARKRTOS_USE_LINUX_ABI 条件编译包住")

    # 附：abi 头的分档定义仍然是原生字宽
    if "typedef unsigned long arkrtos_svc_arg_t" not in abi and \
       "unsigned long arkrtos_svc_arg_t" not in abi:
        bad.append("abi: 宽档参数类型不再是原生字宽")
    if "0x80000000UL" not in abi:
        bad.append("abi: ARKRTOS_SVC_SUB_WORD 的标记值没了")

    return bad, n


def main():
    hdr = read(HDR)
    task = read(TASK)
    elf = read(ELF)
    abi = read(ABI)

    bad, n = verify(hdr, task, elf, abi)
    if bad:
        for b in bad:
            print("FAIL " + b)
        print("SVC-WIDE-GATE FAIL")
        return 1

    # 变异自检：每条断言都要能真的报错。cut() 先断言命中次数，命中数不对
    # 就抛异常——否则"变异没删掉"会伪装成"断言在空转"。
    def cut(text, needle, repl="", expect=1):
        if text.count(needle) != expect:
            raise AssertionError("变异目标 %r 命中 %d 次（应为 %d）"
                                 % (needle, text.count(needle), expect))
        return text.replace(needle, repl, expect)

    muts = [
        ("guard-reject", cut(hdr, "arkrtos_kernel_svc_wide_reject();", ";"), task, elf, abi),
        ("guard-ret", cut(hdr, "ARKRTOS_SVC_RET(p_svc_ctx, (uint32)(-1));"), task, elf, abi),
        ("narrow-empty", cut(hdr, "do { } while(0)", "do { } while(1)"), task, elf, abi),
        ("kill", hdr, cut(task, "\n        arkrtos_task_kill_internal();"), elf, abi),
        ("subword", hdr, task.replace("ARKRTOS_SVC_SUB_WORD", "0u"), elf, abi),
        ("reset-order", hdr, task, cut(elf, "    arkrtos_linux_abi_reset();\n", "    /* moved */\n").replace(
            "    id = elf_start_task(task_entry, stack_bottom, stack_bytes, space, tp);",
            "    id = elf_start_task(task_entry, stack_bottom, stack_bytes, space, tp);\n"
            "    arkrtos_linux_abi_reset();"), abi),
    ]
    for name, h2, t2, e2, a2 in muts:
        got, _ = verify(h2, t2, e2, a2)
        if not got:
            print("FAIL 自检没抓住变异 %s（说明这条断言在空转）" % name)
            print("SVC-WIDE-GATE FAIL")
            return 1

    print("guard call sites: %d" % n)
    print("reset order: before elf_start_task, 1 occurrence")
    print("mutation self-check: %d/%d red as expected" % (len(muts), len(muts)))
    print("SVC-WIDE-GATE OK (reject kills the caller, state reset before the new task runs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
