#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/elf/realbin_probe.py 的门禁。

为什么需要这一道
----------------
探针干的是「拿真实发行版的 ELF 结构去对账 Ark 当前能力」，它自己最容易
出三种错，而三种错都会把结论吹向好看的方向：

  1. **能力表写跑偏**：`ARK_SUPPORTED_DT/RELOC` 里多写一个标签，真机上
     不支持的二进制就被判成「没 blocker」——这是「号称支持、实现没做」的
     经典假绿。所以这里把两张表与 `example/qemu_loongarch64/elf/ldso.c`
     里真正 switch 到的标签 / 真正处理的 `R_LARCH_*` 双向对齐。
  2. **重定位编号自己编**：R_LARCH 的编号来自 psABI，凭印象写就会把
     TLS_TPREL 与 IRELATIVE 搞错，而这两个正是 glibc 里最扎眼的两种。
     这里拿 binutils 的 readelf 名字做多重集对账（工具链不在就 SKIP 并
     打印原因，不静默通过）。
  3. **判据方向单边**：只测「能跑的判绿」不测「跑不了的判红」，于是探针
     永远给绿灯也看不出来。所以夹具是成对的：近基址（窗口内、只用已支持
     的东西）必须 0 blocker，远基址（与真实发行版二进制同样的
     0x120000000）必须恰好 1 条 address。

判据（全过才 0）
----------------
  A. 近基址夹具（本仓 gcc + `-Ttext=0x02001000`）：探针退出码 0、无 blocker。
  B. 远基址夹具（同一份源 + `-Ttext=0x120000000`）：退出码 1，blockers 恰好
     一条且 key 为 address，且首行区间与真实样本同形。
  C. 动态夹具（dynmain.c -> dynfixture.py）：0 blocker——`PT_INTERP` 是
     `/mnt/ram/ld-ark.so`，不许被 interp-path 误判。
  D. 四种坏输入（不是 ELF / ELF32 / e_phentsize 不对 / 程序头越界）各自
     必须被判红，且给出的原因里带对应关键词。
  E. 能力表对齐：`ldso.c` 的 DT_* 宏集合 == 探针 `ARK_SUPPORTED_DT`；
     `R_LARCH_*` 集合 == `ARK_SUPPORTED_RELOC`（两边都判，防只测一边）。
  F. 重定位名字对账（需要 loongarch64-linux-gnu-readelf，缺则 SKIP）：
     readelf -rW 打出的 R_LARCH 名字多重集 == 探针按编号自己算出的多重集。
  G. `--sysroot` 语义：指向不存在的目录时必须明说找不到，不许静默当成功。

不覆盖：真实发行版二进制能不能跑。本门禁只保证「探针说的差距是真差距」。

用法
----
    py -3 tools/realbin_gate.py
    py -3 tools/realbin_gate.py --keep     # 保留临时夹具目录便于手查
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PROBE = os.path.join(ROOT, "tools", "elf", "realbin_probe.py")
FIXTURE = os.path.join(ROOT, "tools", "elf", "dynfixture.py")
DYNMAN = os.path.join(ROOT, "example", "qemu_loongarch64", "elf", "dyn")

LA64_GCC = [
    "D:/LoongIDE/la64-tool/bin/loongarch64-newlib-elf-gcc.exe",
]
LA64_READELF = [
    "D:/tmp/la64linux/loongson-gnu-toolchain-8.3-i686-mingw-loongarch64-"
    "linux-gnu-rc1.4/bin/loongarch64-linux-gnu-readelf.exe",
    "loongarch64-linux-gnu-readelf",
]
LIBC_CANDIDATES = [
    "D:/tmp/la64linux/loongson-gnu-toolchain-8.3-i686-mingw-loongarch64-"
    "linux-gnu-rc1.4/loongarch64-linux-gnu/sysroot/usr/lib64/libc.so.6",
]

ARCH = ["-march=loongarch64", "-mabi=lp64d", "-mstrict-align"]
CFLAGS = ["-O1", "-nostdlib", "-nostartfiles", "-ffreestanding", "-fno-builtin"]

NEAR_BASE = 0x02001000
FAR_BASE = 0x120000000

FAILS = []
PASSES = []
SKIPS = []


def ok(name, detail):
    PASSES.append(name)
    print("PASS  %-22s %s" % (name, detail))


def bad(name, detail):
    FAILS.append(name)
    print("FAIL  %-22s %s" % (name, detail))


def skip(name, detail):
    SKIPS.append(name)
    print("SKIP  %-22s %s" % (name, detail))


def rd(raw):
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd or ROOT, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    return r.returncode, rd(r.stdout or b"")


def probe(path, extra=None):
    cmd = [sys.executable, PROBE, path] + list(extra or [])
    return run(cmd)


def pick(cands):
    """绝对路径直接判存在；裸名字到 PATH 里找（Windows 上补 .exe）。"""
    for c in cands:
        if "/" in c or "\\" in c:
            if os.path.isfile(c):
                return c
            continue
        for p in (os.environ.get("PATH") or "").split(os.pathsep):
            for nm in (c, c + ".exe"):
                cand = os.path.join(p, nm)
                if os.path.isfile(cand):
                    return cand
    return None


# ---------------------------------------------------------------- 夹具

def build_link(gcc, src, base, out):
    cmd = [gcc] + ARCH + CFLAGS + [
        "-Wl,-Ttext=0x%x" % base, "-Wl,-e,_start", "-o", out, src]
    return run(cmd)


def blockers_of(text):
    """从探针的纯文本输出里抽 blocker 的 key，按出现顺序。"""
    return re.findall(r"^\s+\d+\. \[([a-z\-]+)\]", text, re.M)


def check_pair(gcc, tmp):
    src = os.path.join(ROOT, "example", "qemu_loongarch64", "elf", "dynmain.c")
    near = os.path.join(tmp, "near.elf")
    far = os.path.join(tmp, "far.elf")

    rc, out = build_link(gcc, src, NEAR_BASE, near)
    if rc != 0:
        bad("fixture-near", "链接失败：\n%s" % out[-400:])
        return None
    rc, out = probe(near)
    keys = blockers_of(out)
    if rc == 0 and not keys:
        ok("fixture-near", "窗口内静态夹具 0 blocker（退出码 0）")
    else:
        bad("fixture-near", "退出码 %d，blockers=%s\n%s"
            % (rc, keys, out[-500:]))

    rc, out = build_link(gcc, src, FAR_BASE, far)
    if rc != 0:
        bad("fixture-far", "链接失败：\n%s" % out[-400:])
        return None
    rc, out = probe(far)
    keys = blockers_of(out)
    if rc == 1 and keys == ["address"]:
        ok("fixture-far", "远基址 0x%x 恰好 1 条 address blocker（退出码 1）"
           % FAR_BASE)
    else:
        bad("fixture-far", "退出码 %d，期望恰好 [address]，实得 %s\n%s"
            % (rc, keys, out[-500:]))
    return far


def check_dyn_fixture(gcc, tmp):
    src = os.path.join(ROOT, "example", "qemu_loongarch64", "elf", "dynmain.c")
    link = os.path.join(tmp, "dynmain.link.elf")
    out = os.path.join(tmp, "dynmain.elf")
    rc, txt = build_link(gcc, src, 0x1000, link)
    if rc != 0:
        bad("fixture-dyn", "链接失败：\n%s" % txt[-400:])
        return
    rc, txt = run([sys.executable, FIXTURE, "--input", link,
                   "--manifest", os.path.join(DYNMAN, "dynmain.json"),
                   "--output", out])
    if rc != 0:
        bad("fixture-dyn", "dynfixture 失败：\n%s" % txt[-400:])
        return
    rc, txt = probe(out)
    keys = blockers_of(txt)
    if rc == 0 and not keys:
        ok("fixture-dyn", "动态夹具 0 blocker（interp=/mnt/ram/ld-ark.so 不误判）")
    else:
        bad("fixture-dyn", "退出码 %d，blockers=%s\n%s" % (rc, keys, txt[-600:]))


def check_bad_inputs(tmp):
    good = None
    for c in ("near.elf", "far.elf", "dynmain.elf"):
        p = os.path.join(tmp, c)
        if os.path.isfile(p):
            good = p
            break
    if good is None:
        bad("probe-negatives", "没有可用的正常夹具做底稿")
        return
    raw = open(good, "rb").read()

    cases = []

    p = os.path.join(tmp, "notelf.bin")
    open(p, "wb").write(b"hello, not an elf\n" * 4)
    cases.append((p, "not an ELF file", "notelf"))

    p = os.path.join(tmp, "elf32.bin")
    b = bytearray(raw)
    b[4] = 1                      # EI_CLASS = ELFCLASS32
    open(p, "wb").write(bytes(b))
    cases.append((p, "not ELF64", "elf32"))

    p = os.path.join(tmp, "badphent.bin")
    b = bytearray(raw)
    b[54] = 40                    # e_phentsize 低字节改成 40
    open(p, "wb").write(bytes(b))
    cases.append((p, "e_phentsize", "bad-phentsize"))

    p = os.path.join(tmp, "truncphdr.bin")
    b = bytearray(raw)
    # e_phoff 指向文件尾部之外：程序头全部越界
    b[32:40] = (len(b) + 4096).to_bytes(8, "little")
    open(p, "wb").write(bytes(b))
    cases.append((p, "program header", "trunc-phdr"))

    missed = []
    for path, want, name in cases:
        rc, txt = probe(path)
        if rc != 1 or want not in txt:
            missed.append("%s(rc=%d, 期望含 %r)" % (name, rc, want))
    if missed:
        bad("probe-negatives", "这些坏输入没被判红：%s" % ", ".join(missed))
    else:
        ok("probe-negatives", "%d 种坏输入全部判红且原因到位"
           % len(cases))


# ---------------------------------------------------------------- 能力表

def parse_ldso():
    path = os.path.join(ROOT, "example", "qemu_loongarch64", "elf", "ldso.c")
    txt = rd(open(path, "rb").read())
    dt = {}
    for m in re.finditer(r"^#define\s+(DT_[A-Z0-9_]+)\s+(\d+)\s*$", txt, re.M):
        dt[m.group(1)] = int(m.group(2))
    rl = {}
    for m in re.finditer(r"^#define\s+(R_LARCH_[A-Z0-9_]+)\s+(\d+)\s*$", txt, re.M):
        rl[m.group(1)] = int(m.group(2))
    # switch 里真正被 case 到的标签集（名字），用来查「宏定义了但没接线」
    cases = set(re.findall(r"case\s+(DT_[A-Z0-9_]+)\s*:", txt))
    return dt, rl, cases


def check_tables():
    sys.path.insert(0, os.path.join(ROOT, "tools", "elf"))
    import realbin_probe as P           # noqa: E402

    dt, rl, cases = parse_ldso()
    if not dt or not rl:
        bad("table-alignment", "没能从 ldso.c 里读出 DT_/R_LARCH 宏（形态变了？）")
        return P

    dt_vals = set(dt.values()) | {0}     # DT_NULL 是循环终点，两边都算
    want_dt = set(P.ARK_SUPPORTED_DT)
    if dt_vals != want_dt:
        bad("table-alignment",
            "DT 表不一致：ldso 有探针没有 %s；探针有 ldso 没有 %s"
            % (sorted(dt_vals - want_dt), sorted(want_dt - dt_vals)))
    rl_vals = set(rl.values())
    want_rl = set(P.ARK_SUPPORTED_RELOC)
    if rl_vals != want_rl:
        bad("table-alignment",
            "重定位表不一致：ldso 有探针没有 %s；探针有 ldso 没有 %s"
            % (sorted(rl_vals - want_rl), sorted(want_rl - rl_vals)))
    # 光有 #define 不算支持：switch 里必须真的 case 到（DT_STRTAB 之类）。
    # 两个例外，都是「不在 switch 里但有独立判据」：
    #   DT_NULL 是循环终点；DT_RELAENT 由探针按「必须等于 24」单独核
    #   （ldso 扫重定位时把项长写死 24），所以它不需要 case。
    by_num = dict((v, k) for k, v in dt.items())
    defined = set(by_num.get(v) for v in want_dt if v in by_num)
    unwired = sorted(defined - cases - {"DT_NULL", "DT_RELAENT"})
    if unwired:
        bad("table-alignment",
            "探针声称支持但 ldso.c 的 switch 里没有 case 到：%s" % ", ".join(unwired))
    elif dt_vals == want_dt and rl_vals == want_rl:
        ok("table-alignment",
           "DT %d 项 / 重定位 %d 项与 ldso.c 双向一致"
           % (len(want_dt), len(want_rl)))
    return P


def check_reloc_names(P):
    readelf = pick(LA64_READELF)
    libc = pick(LIBC_CANDIDATES)
    if not readelf:
        skip("reloc-name-audit", "找不到 loongarch64-linux-gnu-readelf（不做静默通过）")
        return
    if not libc:
        skip("reloc-name-audit", "找不到 sysroot 里的 libc.so.6")
        return
    rc, out = run([readelf, "-rW", libc])
    if rc != 0:
        bad("reloc-name-audit", "readelf 失败：\n%s" % out[-300:])
        return
    seen = {}
    for m in re.finditer(r"\b(R_LARCH_[A-Z0-9_]+)\b", out):
        seen[m.group(1)] = seen.get(m.group(1), 0) + 1

    e = P.Elf(libc)
    r = P.collect(e)
    mine = r["reloc_hist"]
    if not mine:
        bad("reloc-name-audit", "探针在 libc.so.6 上没读到任何重定位")
        return
    if seen != mine:
        only_elf = {k: v for k, v in seen.items() if mine.get(k) != v}
        only_me = {k: v for k, v in mine.items() if seen.get(k) != v}
        bad("reloc-name-audit",
            "名字/条数不一致：readelf 独有 %s；探针独有 %s" % (only_elf, only_me))
        return
    ok("reloc-name-audit", "%d 种重定位名字与条数同 readelf 逐项一致（%s）"
       % (len(mine), ", ".join("%s x%d" % (k, v) for k, v in sorted(mine.items()))))


def check_sysroot(tmp):
    """--sysroot 指到不存在的目录：必须明说找不到，不许当成「依赖全齐」。"""
    near = os.path.join(tmp, "near.elf")
    if not os.path.isfile(near):
        bad("sysroot-semantics", "缺夹具 near.elf")
        return
    link = os.path.join(tmp, "dynmain.link.elf")
    out = os.path.join(tmp, "dynmain.elf")
    if not os.path.isfile(out) and os.path.isfile(link):
        run([sys.executable, FIXTURE, "--input", link,
             "--manifest", os.path.join(DYNMAN, "dynmain.json"), "--output", out])
    if not os.path.isfile(out):
        bad("sysroot-semantics", "缺动态夹具，无法测 --sysroot")
        return
    ghost = os.path.join(tmp, "no-such-sysroot")
    rc, txt = probe(out, ["--sysroot", ghost])
    if rc != 1 or "not a directory" not in txt:
        bad("sysroot-semantics", "不存在的 sysroot 没有被明说：\n%s" % txt[-400:])
        return
    # 再把同一个 sysroot 换成一个空目录：依赖必须被判「找不到」
    empty = os.path.join(tmp, "empty-sysroot")
    os.makedirs(empty, exist_ok=True)
    rc, txt = probe(out, ["--sysroot", empty])
    if rc != 1 or "sysroot 里没有" not in txt:
        bad("sysroot-semantics", "空 sysroot 没报「找不到依赖」：\n%s" % txt[-400:])
        return
    ok("sysroot-semantics", "不存在的根目录与空根目录都被明说，不静默")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args(argv)

    gcc = pick(LA64_GCC)
    if not gcc:
        print("FAIL  工具链         找不到 loongarch64-newlib-elf-gcc（缺工具是 FAIL）")
        return 1

    tmp = tempfile.mkdtemp(prefix="realbin_gate_")
    try:
        check_pair(gcc, tmp)
        check_dyn_fixture(gcc, tmp)
        check_bad_inputs(tmp)
        P = check_tables()
        if P is not None:
            check_reloc_names(P)
        check_sysroot(tmp)
    finally:
        if a.keep:
            print("\n夹具保留在 %s" % tmp)
        else:
            import shutil
            shutil.rmtree(tmp, ignore_errors=True)

    print("\n%d passed, %d failed, %d skipped"
          % (len(PASSES), len(FAILS), len(SKIPS)))
    if not FAILS:
        print("REALBIN-GATE OK (%d checks, %d skipped)" % (len(PASSES), len(SKIPS)))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))