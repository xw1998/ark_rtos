#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""格式基线门禁：把「风格靠人」换成四条能被机器判的规则。

为什么只有四条
--------------
这台机器上没有 clang-format，网络也不通（装不了）；而任何「整文件重排版」的
工具都会顺手改写不属于本次改动的字节，这与仓库的字节级补丁纪律直接冲突
（2026-11-18 之前 `kernelsrc/include` 里还有 22 个 GBK 文件，重排版会把它们改坏；
现在 own 源已统一 UTF-8，但 vendored 树仍是上游字节，纪律不变）。所以这里不做
格式化，只钉住四条**不需要重新排版、也不需要读懂代码**的规则：

  F1 无制表符      .c/.h 里不许出现 0x09
  F2 无行尾空白    行末不许留空格或制表符
  F3 文件以换行结尾
  F4 无 UTF-8 BOM  .c/.h 开头不许是 EF BB BF

四条都是纯字节判断：不解码、不转码、不碰换行风格，所以对任何编码都成立，
改起来也只是删几个字节（不会触发整文件重写）。换行风格（CRLF/LF）不在这里管，
它已经由 `tools/ci_gate.py` 的 encoding / mixed-eol 两项负责，那条口径是
「一个文件内部不许混」，而不是「全仓统一」。

为什么扫的是工作区而不是 index blob
----------------------------------
`ci_gate` 扫 index blob，是因为它要判「提交上去的是什么」。这四条规则对
`core.autocrlf` 的行尾改写免疫：改写只在 `\\n` 前插 `\\r`，既不会产生行尾空白
（判之前先剥 `\\r`），也不会增删制表符/BOM/末行换行。所以这里直接读工作区，
省一次 `git show` per file（548 个文件）。

豁免表是判据的一部分，不是补丁
------------------------------
第三方与生成物不参与判：lwip / cm_backtrace / nr_micro_shell / arkc / arkvim /
littlefs / ark_softbus 都是**逐字节与上游仓库对齐**的 vendored 树（同步脚本一
对不上就红），改它们的空白等于制造下一次同步冲突；`Drivers/`、`CMSIS/`、
`ARKRTOS_TEST/`、`docs/.gen_utf8/` 同理。汇编 `.s/.S` 也豁免 F1 —— 那里制表符
是列对齐的惯例（本仓 `kernelsrc/port/arm/*/arkrtos_context.S` 在用）。

用法
----
    python tools/format_gate.py              # 先自检，再扫全仓
    python tools/format_gate.py --selfcheck   # 只自检
    python tools/format_gate.py --list 30     # 违规明细最多列 30 条（默认 20）

退出码 0 = 四条规则全过且自检有效；1 = 有违规；2 = 自检失败（门禁本身失明）。
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# vendored / 生成物 / 第三方：不参与判。末尾带 / 的是目录前缀。
EXEMPT_PREFIXES = (
    "kernelsrc/components/lwip/",
    "kernelsrc/components/cm_backtrace/",
    "kernelsrc/components/nr_micro_shell/",
    "kernelsrc/components/arkc/",
    "kernelsrc/components/arkvim/",
    "kernelsrc/components/ark_softbus/",
    "kernelsrc/fs/littlefs/",
    "docs/.gen_utf8/",
)
EXEMPT_MARKERS = ("/Drivers/", "/CMSIS/", "/Middlewares/", "/RTE/", "/ARKRTOS_TEST/")

# 只判 C 头/源。汇编只受 F2/F3/F4 管（见 docstring），脚本与文档不在本门禁。
C_SUFFIX = (".c", ".h")
ASM_SUFFIX = (".s", ".S")

RULES = ("F1", "F2", "F3", "F4")

GITENV = dict(os.environ)
GITENV.pop("PYTHONHOME", None)
GITENV.pop("PYTHONPATH", None)


def tracked_files():
    r = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, env=GITENV,
                       capture_output=True)
    if r.returncode != 0:
        raise SystemExit("git ls-files failed (run this inside the repo)")
    return [p for p in r.stdout.decode("utf-8", "replace").split("\0") if p]


def is_exempt(rel):
    if any(rel.startswith(p) for p in EXEMPT_PREFIXES):
        return True
    slash = "/" + rel
    return any(m in slash for m in EXEMPT_MARKERS)


def check_bytes(raw, allow_tab):
    """Return a list of (rule, lineno) for one file's bytes."""
    hits = []
    if raw.startswith(b"\xef\xbb\xbf"):
        hits.append(("F4", 1))
    if not raw.endswith(b"\n"):
        hits.append(("F3", 0))
    for i, line in enumerate(raw.split(b"\n"), 1):
        body = line[:-1] if line.endswith(b"\r") else line
        if not allow_tab and b"\t" in body:
            hits.append(("F1", i))
        if body != body.rstrip():
            hits.append(("F2", i))
    return hits


def scan(roots_files):
    bad = []
    counts = dict((r, 0) for r in RULES)
    for rel, path in roots_files:
        try:
            with open(path, "rb") as f:
                raw = f.read()
        except OSError as exc:
            bad.append((rel, "F?", 0, str(exc)))
            continue
        allow_tab = rel.endswith(ASM_SUFFIX)
        for rule, line in check_bytes(raw, allow_tab):
            counts[rule] += 1
            bad.append((rel, rule, line, ""))
    return bad, counts


def selfcheck():
    """Deliberate violations must be seen, and an exempt path must not be.

    A format gate that silently stops matching (bad walker, wrong suffix, an
    over-broad exemption) reports "0 violations" forever.  So every rule gets
    one poisoned file, plus one clean control and one exempt control.
    """
    tmp = tempfile.mkdtemp(prefix="arkfmt_")
    cases = {
        "clean.c": b"int a;\nint b;\n",
        "tab.c": b"int a;\n\tint b;\n",
        "ws.c": b"int a;   \n",
        "eof.c": b"int a;",
        "bom.c": b"\xef\xbb\xbfint a;\n",
        "asm_ok.s": b"\t.globl f\n",
    }
    want = {
        "clean.c": [],
        "tab.c": [("F1", 2)],
        "ws.c": [("F2", 1)],
        "eof.c": [("F3", 0)],
        "bom.c": [("F4", 1)],
        "asm_ok.s": [],
    }
    ok = 0
    try:
        for name, data in cases.items():
            with open(os.path.join(tmp, name), "wb") as f:
                f.write(data)
            got = check_bytes(data, name.endswith(ASM_SUFFIX))
            if sorted(got) == sorted(want[name]):
                ok += 1
            else:
                print("selfcheck FAIL %-10s got=%r want=%r" % (name, got, want[name]))
        # exemption: a poisoned file inside a vendored tree must not be picked up.
        if is_exempt("kernelsrc/components/lwip/src/core/tcp.c") and \
           not is_exempt("kernelsrc/src/arkrtos_task.c"):
            ok += 1
        else:
            print("selfcheck FAIL exemption table")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return ok, len(cases) + 1


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--selfcheck", action="store_true")
    ap.add_argument("--list", type=int, default=20, dest="nlist")
    a = ap.parse_args(argv)

    ok, total = selfcheck()
    if ok != total:
        print("FORMAT-GATE selfcheck FAIL (%d/%d) - gate is blind, refusing to report"
              % (ok, total))
        return 2
    if a.selfcheck:
        print("selfcheck ok %d/%d" % (ok, total))
        return 0

    files = []
    exempt = 0
    for rel in tracked_files():
        if rel.endswith(C_SUFFIX) or rel.endswith(ASM_SUFFIX):
            if is_exempt(rel):
                exempt += 1
                continue
            files.append((rel, os.path.join(ROOT, rel.replace("/", os.sep))))

    bad, counts = scan(files)
    if bad:
        for rel, rule, line, extra in bad[:max(a.nlist, 1)]:
            print("FAIL %s %s:%d %s" % (rule, rel, line, extra))
        print("FORMAT-GATE FAIL: %d violation(s) in %d file(s) - F1 tab %d / F2 trailing-ws %d"
              " / F3 no-eof-newline %d / F4 bom %d"
              % (len(bad), len(set(b[0] for b in bad)),
                 counts["F1"], counts["F2"], counts["F3"], counts["F4"]))
        return 1

    print("selfcheck ok %d/%d" % (ok, total))
    print("FORMAT-GATE OK: %d own C/asm file(s) clean on F1-F4 (%d vendored/generated exempt)"
          % (len(files), exempt))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
