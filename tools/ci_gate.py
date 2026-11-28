#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Ark RTOS CI gate: the checks that must pass before anything lands on master.

Every check here exists because the corresponding mistake actually happened in
this repo.  A check that only prints and never fails is worse than no check, so
each one returns pass/fail and the script exits non-zero on any failure.

The first version of this script produced three false failures and one false
pass, all of which are now fixed and worth knowing about:
  * the F401 partition header is *supposed* to differ on chip capability
    (flash / ram / CCM / hw compat id) - only the derived geometry must match;
  * CMSIS device headers and DSP tables are third party and full of peripheral
    base addresses, so they must not be policed for hardcoded pool addresses;
  * `gen_scatter.py --check` validates the layout, it does NOT prove a .sct on
    disk matches the generator.  That is a separate question and is answered by
    "is the .sct tracked at all" - if it is not tracked, a hand edit cannot
    reach master in the first place.

Usage:
    py -3 tools/ci_gate.py                 # structural checks only
    py -3 tools/ci_gate.py --build         # also compile both kernel projects
    py -3 tools/ci_gate.py --uv4 "D:/Keil_v5/UV4/UV4.exe"
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from tempdir_util import TempDir  # noqa: E402

PART_HDR_427 = "config/arkrtos_partition.h"
PART_HDR_401 = "config/stm32f401/arkrtos_partition.h"

# Macros that describe the *board*, not the layout.  The F401 twin is expected
# to differ on exactly these; anything else shared between the two headers is
# layout geometry and must stay identical.
#
# "Does this board have a second dev slot / a NOR / a filesystem at all" is a
# board capability question, and the answer on F401 is "no" - which is spelled
# 0, not "absent".  Those zeros are not geometry drift.
CHIP_ONLY_PREFIXES = ("CHIP_", "ARKRTOS_HW_COMPAT_ID")
CHIP_ONLY_EXACT = {
    "ARKRTOS_DEV_RAM_WINDOW",
    "ARKRTOS_DEV_SLOT2_UNIT",
    "ARKRTOS_DEV_SLOT2_BASE",
    "ARKRTOS_DEV_SLOT2_SIZE",
    "ARKRTOS_DEV_SLOT2_TYPE",
    "ARKRTOS_DEV_SLOT3_UNIT",
    "ARKRTOS_DEV_SLOT3_TYPE",
    "ARKRTOS_NOR_CHIP_SIZE",
    "ARKRTOS_FS_SIZE",
}

# Third-party trees: not ours to police.
THIRD_PARTY_MARKERS = ("/Drivers/", "/Middlewares/", "/RTE/", "/DebugConfig/",
                       "/CMSIS/", "Drivers/CMSIS")

# Trees that get compiled into a firmware image.  tools/ is host side and is
# policed by nothing here on purpose (see check_single_address_source).
TARGET_CODE_ROOTS = ("kernelsrc/", "board/", "config/", "example/")

# Vendored components are kept byte-compatible with their own repositories
# (tools/sync_arkc.py --check fails the moment one of their files differs).
# arkc's default load addresses are the compiler's notion of "where a bare
# binary runs", not a copy of this board's partition table, so a layout change
# here must not edit them: patching upstream would also make every future sync
# conflict.  They are therefore out of scope for the single-address-source
# check, exactly like the generated .sct files.
VENDOR_MARKERS = ("/components/arkc/src/", "/components/arkc/include/",
                  "/components/arkvim/src/")

# Images that are linked separately from the kernel and therefore must stay
# layout-agnostic.  The kernel's own loader (kernelsrc/src/arkrtos_loader.c) is
# part of the kernel image and *must* see the partition header, so kernelsrc/src
# and kernelsrc/include are deliberately absent here.
APP_SIDE_MARKERS = ("kernelsrc/sdk/", "/app_sdk/", "/driver_sdk/")

# Own sources are UTF-8 only since 2026-11-18 (see check_encoding_eol).
ALLOWED_ENCODINGS = ("utf-8",)

# Vendored trees keep whatever their upstream repo ships: converting them
# would break byte-compatibility with the sync scripts (cm_backtrace ships a
# GBK zh-CN language header).  Only this prefix may still decode as GBK.
VENDORED_GBK_PREFIX = "kernelsrc/components/"

SKIP_DIRS = {".git", "build", "docs", "_nova", "RTE", "DebugConfig",
             "Objects", "Listings"}

FAILURES = []
PASSED = []
SKIPPED = []


def fail(check, detail):
    FAILURES.append("[%s] %s" % (check, detail))
    print("FAIL %-26s %s" % (check, detail))


def ok(check, detail=""):
    PASSED.append(check)
    print("ok   %-26s %s" % (check, detail))

def skip(check, detail):
    """A check that did not run is not a pass: it is counted on its own."""
    SKIPPED.append(check)
    print("skip %-26s %s" % (check, detail))


def read(path):
    with open(path, "rb") as f:
        return f.read()


GITENV = dict(os.environ)
GITENV.pop("PYTHONHOME", None)
GITENV.pop("PYTHONPATH", None)

_TRACKED = None


def tracked_files():
    global _TRACKED
    if _TRACKED is None:
        r = subprocess.run(["git", "ls-files"], cwd=ROOT, env=GITENV,
                           capture_output=True)
        _TRACKED = set(l for l in (r.stdout or b"").decode("utf-8", "replace").splitlines() if l)
    return _TRACKED


def stored_bytes(rel, path):
    """The bytes that would actually be committed for this file.

    The working tree is *not* the truth: core.autocrlf=true rewrites LF-only
    files as CRLF on checkout, so a byte level EOL scan of the working tree
    reports failures git itself normalises away, and misses the one case that
    matters - a blob that already mixes both.  The index blob is what lands on
    master, so that is what gets scanned.  An untracked file has no blob yet,
    so its working copy *is* the future blob.

    Consequence: stage first, then run the gate.
    """
    if rel in tracked_files():
        r = subprocess.run(["git", "show", ":" + rel], cwd=ROOT, env=GITENV,
                           capture_output=True)
        if r.returncode == 0:
            return r.stdout
    return read(path)


def is_third_party(rel):
    return any(m in rel for m in THIRD_PARTY_MARKERS)


def own_sources():
    """Tracked-looking sources that are ours (no third-party trees)."""
    out = []
    for base in ("kernelsrc", "board", "config", "tools", "example"):
        root = os.path.join(ROOT, base)
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for name in filenames:
                if not name.endswith((".c", ".h", ".s", ".S", ".py")):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, name), ROOT).replace("\\", "/")
                if is_third_party(rel):
                    continue
                out.append((rel, os.path.join(dirpath, name)))
    return sorted(out)


def strip_comments_bytes(raw):
    """Byte level C comment stripper.

    Works on bytes, not text: even though every own source is UTF-8 since
    2026-11-18, the vendored trees still carry GBK, so decoding with 'replace'
    before scanning can shift what is left.  Handles // line comments,
    /* */ block comments including the multi-line form whose continuation lines
    start with '*', and skips string / char literals so that a '/*' inside a
    string does not swallow the rest of the line (which would hide a real hit).
    An address that survives this is a constant, not prose.
    """
    out = bytearray()
    i, n = 0, len(raw)
    quote = 0
    while i < n:
        c = raw[i]
        if quote:
            out.append(c)
            if c == 0x5C and i + 1 < n:          # backslash escape
                out.append(raw[i + 1])
                i += 2
                continue
            if c == quote:
                quote = 0
            i += 1
            continue
        if c in (0x22, 0x27):                    # " or '
            quote = c
            out.append(c)
            i += 1
            continue
        if c == 0x2F and i + 1 < n and raw[i + 1] == 0x2F:      # //
            while i < n and raw[i] != 0x0A:
                i += 1
            continue
        if c == 0x2F and i + 1 < n and raw[i + 1] == 0x2A:      # /*
            i += 2
            while i < n:
                if raw[i] == 0x2A and i + 1 < n and raw[i + 1] == 0x2F:
                    i += 2
                    break
                if raw[i] == 0x0A:
                    out.append(0x0A)             # keep the line structure
                i += 1
            continue
        out.append(c)
        i += 1
    return bytes(out)


def code_lines(raw):
    """Yield (lineno, bytes) for every line, comments already removed."""
    stripped = strip_comments_bytes(raw)
    for i, line in enumerate(stripped.split(b"\n"), 1):
        yield i, line


# ---------------------------------------------------------------- check 1
def check_partition_twins():
    """Layout geometry must agree between the two partition headers.

    Chip capability is allowed to differ by design (F427 1MB/128KB/CCM64 vs
    F401 512KB/96KB/no CCM, different hw compat id).  Everything else is
    derived geometry and a drift there means one board runs a layout that
    exists nowhere in the repository.
    """
    b = os.path.join(ROOT, PART_HDR_401)
    if not os.path.exists(b):
        ok("partition-twin", "no F401 copy present")
        return

    def macros(path):
        txt = read(path).decode("utf-8", "replace")
        out = {}
        for m in re.finditer(r"^\s*#define\s+(\w+)\s+(.+?)\s*$", txt, re.M):
            # Drop the trailing comment: the same value documented differently
            # in the two twins is not a drift.
            val = re.sub(r"/\*.*?\*/", "", m.group(2)).strip()
            out[m.group(1)] = val
        return out

    ma, mb = macros(os.path.join(ROOT, PART_HDR_427)), macros(b)
    geometry, chip_only = [], []
    for k in ma:
        if not (k.startswith("ARKRTOS_") or k.startswith("CHIP_")):
            continue
        if k.startswith(CHIP_ONLY_PREFIXES) or k in CHIP_ONLY_EXACT:
            chip_only.append(k)
        else:
            geometry.append(k)

    drift = []
    for k in geometry:
        if k in mb and ma[k] != mb[k]:
            drift.append("%s: %s vs %s" % (k, ma[k], mb[k]))
    missing = [k for k in geometry if k not in mb]
    if drift or missing:
        detail = list(drift)
        if missing:
            detail.append("missing in F401: %s" % ", ".join(missing))
        fail("partition-twin", "; ".join(detail[:6]))
    else:
        ok("partition-twin",
           "%d geometry macros agree, %d chip-only allowed to differ"
           % (len(geometry), len(chip_only)))


# ---------------------------------------------------------------- check 2
def check_single_address_source():
    """No hardcoded pool / slot / image-ram address in code that ships in an image.

    The partition headers (and the generated .sct) are the only sources.
    Comments are excluded: a comment that *names* an address is documentation,
    not a constant a layout change can leave stale.

    Scope is deliberately the trees that get compiled into a firmware image
    (kernelsrc / board / example).  tools/ is host side: its only literals are
    GUI form defaults and an error message that offers 0x08040000 as an example
    of a legal integer (arkrtos_host_gui.py).  Those are UI text, not layout
    constants, and policing them produced the first version's false failures.
    """
    pat = re.compile(rb"0x0[28]0[0-9A-Fa-f]{5}")
    hits = []
    for rel, path in own_sources():
        if not rel.startswith(TARGET_CODE_ROOTS):
            continue
        if rel.endswith((".sct", "arkrtos_partition.h")):
            continue
        if any(m in rel for m in VENDOR_MARKERS):
            continue
        for i, line in code_lines(stored_bytes(rel, path)):
            if b"0x080" not in line and b"0x200" not in line:
                continue
            if pat.search(line):
                hits.append("%s:%d" % (rel, i))
    if hits:
        fail("single-address-source", "%d literal(s): %s" % (len(hits), ", ".join(hits[:8])))
    else:
        ok("single-address-source", "no literals in image code")


# ---------------------------------------------------------------- check 3
def check_encoding_eol():
    """Every own source is UTF-8; no blob mixes CRLF and LF.

    Mixed endings are a failure, not a note: they are what turned a real
    153-line change into a 5739-line diff that nobody could review.

    Scanned on the index blob (see stored_bytes) - the working tree is
    autocrlf-rewritten and says nothing about what would be committed.

    Encoding used to accept GBK so the old sources could stay as they were.
    On 2026-11-18 all 51 GBK own sources were transcoded to UTF-8 (one commit,
    no logic touched: every Chinese character sits in a comment or in an
    #error text, so the LA64 image stayed byte-identical, sha256 fbd1094e...).
    Since then UTF-8 is the only accepted encoding for own sources, and a GBK
    file fails here instead of being silently tolerated.  Vendored trees
    (kernelsrc/components/) are exempt: they are byte-compatible with their
    own repositories and one of them ships a GBK zh-CN header.
    """
    # 自检：这一项的全部判断力就在"哪种编码能解"，先证明它分得清。
    # 拿一个 UTF-8 解不开、GBK 解得开的两字节样本：任何一边失效，这一项都会
    # 变成永远通过（假绿），所以宁可直接判失败。
    try:
        b"\xd6\xd0".decode("utf-8")
        fail("encoding", "self-check: a GBK sample decoded as UTF-8 (check is blind)")
        return
    except UnicodeDecodeError:
        pass
    if b"\xd6\xd0".decode("gbk") != "\u4e2d":
        fail("encoding", "self-check: decoder mismatch (check is blind)")
        return

    bad, mixed = [], []
    gbk_ok = 0
    for rel, path in own_sources():
        raw = stored_bytes(rel, path)
        if not raw:
            continue
        allowed = ALLOWED_ENCODINGS
        if rel.startswith(VENDORED_GBK_PREFIX):
            allowed = ALLOWED_ENCODINGS + ("gbk",)
        decoded = None
        for enc in allowed:
            try:
                raw.decode(enc)
                decoded = enc
                break
            except UnicodeDecodeError:
                continue
        if decoded is None:
            bad.append(rel)
            continue
        if decoded == "gbk":
            gbk_ok += 1
        crlf = raw.count(b"\r\n")
        lf = raw.count(b"\n") - crlf
        if crlf and lf:
            mixed.append("%s (crlf=%d lf=%d)" % (rel, crlf, lf))
    if bad:
        fail("encoding", "%d own source(s) are not UTF-8: %s"
             % (len(bad), ", ".join(bad[:5])))
    else:
        detail = "all own sources decode as utf-8"
        if gbk_ok:
            detail += " (%d vendored file(s) still GBK)" % gbk_ok
        ok("encoding", detail)
    if mixed:
        fail("mixed-eol", "%d file(s): %s" % (len(mixed), ", ".join(mixed[:6])))
    else:
        ok("mixed-eol", "no file mixes CRLF and LF")


# ---------------------------------------------------------------- check 3b
def check_format_baseline():
    """格式基线（.editorconfig 里声明的四条）必须真被判到，且门禁自己不失明。

    这几条是纯字节判断（无制表符 / 无行尾空白 / 文件以换行结尾 / 无 UTF-8 BOM），
    不解码、不转码 —— 这也是选了这四条、而不是上 clang-format 的原因：这台机器
    没有 clang-format，网络也装不了，而整文件重排版会顺手改写不属于本次改动的
    字节（转码前那些文件是 GBK，任何重排版都会把它们改坏）。

    它抓到的第一件真事就够本：`kernelsrc/src/arkrtos_task.c` 里 1143 行、加上
    `arkrtos_shell.h` 1 行，行尾是 `\r\r\n`（多一个 CR）。这种文件在编译器眼里
    等同于普通 CRLF，但任何按行处理的脚本（diff、统计、断言）都会把它当成"行尾有
    空白"，而且它让「这一行到底改了什么」永远看不出来。

    自检失败不算通过：`FORMAT-GATE selfcheck FAIL` 说明判据本身没在匹配（走错目录、
    后缀写错、豁免表过宽），那时候报"0 违规"是假绿。
    """
    r = _run_check([sys.executable, os.path.join("tools", "format_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if "selfcheck ok" not in txt:
        fail("format", "selfcheck did not run: %s" % txt.strip()[-300:])
        return
    if r.returncode == 0 and "FORMAT-GATE OK" in txt:
        ok("format", txt.strip().splitlines()[-1])
    else:
        fail("format", txt.strip()[-400:])

# ---------------------------------------------------------------- check 4
def check_gate_scripts_live():
    """门禁脚本里不许有「终止语句之后的死代码」。

    真事（U-16 那笔，提交 09e8036）：tools/paging_gate.py 把 13 条源码 pin 的
    汇总循环连同它们的自检一起挪到了 pin_v 的 `return bad` 之后 —— 那段永远
    不执行，而门禁照样打印「…pin 成立且自检有效」，一路绿了好几个提交。实测
    把 hal.h 里的一条关档回退改坏，门禁仍然全绿，这就是假绿。

    这种缺陷靠人看不出来（脚本语法合法、输出漂亮），只能用结构判据钉住：
    return / raise / continue / break 之后，同一个块里不许再有条目。
    """
    import ast

    def scan(path, src):
        try:
            tree = ast.parse(src)
        except SyntaxError as e:
            return ["%s: 语法错 %s" % (path, e)]
        out = []
        for n in ast.walk(tree):
            body = getattr(n, "body", None)
            if not isinstance(body, list):
                continue
            for i, st in enumerate(body):
                if isinstance(st, (ast.Return, ast.Raise, ast.Continue, ast.Break)) \
                        and (i + 1 < len(body)):
                    out.append("%s: %s() 第 %d 行之后还有 %d 条不可达语句"
                               % (path, getattr(n, "name", "<module>"),
                                  st.lineno, len(body) - i - 1))
        return out

    bad = []
    tdir = os.path.join(ROOT, "tools")
    n_scan = 0
    for rel in sorted(os.listdir(tdir)):
        if not rel.endswith(".py"):
            continue
        n_scan += 1
        bad.extend(scan("tools/" + rel,
                        read(os.path.join(tdir, rel)).decode("utf-8", "replace")))

    # 自检：同一套判据喂两段合成源码，一段有死代码、一段是合法的多出口
    sel_bad = scan("<selftest>", "def f():\n    return 1\n    x = 2\n")
    sel_ok = scan("<selftest>", "def f():\n    if True:\n        return 1\n    return 2\n")
    if (len(sel_bad) != 1) or sel_ok:
        bad.append("自检：判据在合成源码上没按预期回答（有死代码=%r，正常=%r）"
                   % (sel_bad, sel_ok))

    if bad:
        fail("gate-live", "; ".join(bad[:4]))
    else:
        ok("gate-live",
           "%d tool script(s) have no unreachable statement after return/raise/break/continue"
           % n_scan)

def check_doc_index(dry_run=None):
    """Every docs/*.md page must be reachable from docs/README.md.

    This is not tidiness: the index is the only map a reader has.  A page that
    exists but is not listed is a page nobody finds, and it drifted once for
    real (stm32h743-external-memory.md was added and never listed).

    Matching is by file name, so a rename breaks the link (which is the point)
    and the fix is to update the index, not to loosen the check.
    """
    docs_dir = os.path.join(ROOT, "docs")
    index_path = os.path.join(docs_dir, "README.md")
    if not os.path.exists(index_path):
        fail("doc-index", "docs/README.md is missing")
        return
    index = read(index_path).decode("utf-8", "replace")
    pages = sorted(f for f in os.listdir(docs_dir)
                   if f.endswith(".md") and f != "README.md")
    missing = [f for f in pages if f not in index]
    if missing:
        fail("doc-index", "%d page(s) absent from docs/README.md: %s"
             % (len(missing), ", ".join(missing[:6])))
        return

    # The English index (docs/README.en.md) is a second map over the same pages,
    # so it can lose a page exactly the same way.  It is checked by file name too:
    # the English row for a page keeps the page's own (Chinese) file name, which is
    # the house convention and is what makes this a mechanical check rather than a
    # translation review.  Descriptions may differ; the page set may not.
    en_path = os.path.join(docs_dir, "README.en.md")
    if not os.path.exists(en_path):
        fail("doc-index", "docs/README.en.md is missing (the English index is part of the map)")
        return
    en = read(en_path).decode("utf-8", "replace")
    if "README.en.md" not in index:
        fail("doc-index", "docs/README.md does not mention README.en.md, so English readers cannot find the English index")
        return
    en_missing = [f for f in pages + ["README.en.md"] if f not in en]
    if en_missing:
        fail("doc-index", "%d page(s) absent from docs/README.en.md: %s"
             % (len(en_missing), ", ".join(en_missing[:6])))
        return

    # A link that points at a file nobody renamed into existence is the other half
    # of the same failure: the index says the page is there and it is not.
    broken = []
    for target in re.findall(r"\]\(([^)#]+)\)", en):
        if target.startswith("http://") or target.startswith("https://"):
            continue
        if not os.path.exists(os.path.join(docs_dir, target)):
            broken.append(target)
    if broken:
        fail("doc-index", "%d link(s) in docs/README.en.md point at missing files: %s"
             % (len(broken), ", ".join(sorted(set(broken))[:6])))
        return

    ok("doc-index", "all %d docs pages are indexed (zh + en), and every en link resolves"
       % len(pages))


def check_evidence_levels():
    """每条读数都要写明载体级别，级别不允许被自我抬高。

    这个仓库的读数有三种量级：编得过、跑在 QEMU 上、真机串口日志。散文里
    三者长得一样，于是"A QEMU run counts as a board run"很容易被读成"上板了"。
    tools/evidence_levels.tsv 把每条主张压成一行（级别 / 载体 / 出处提交号），
    tools/evidence_gate.py 机械校验：L2 的载体必须是存在的 board/qemu_* 目录、
    L3 的载体不能是 qemu_*、证据里必须有能在 git 历史里查到的提交号、
    L3 行数冻结（真机读数只能靠人抄回来，不能靠改代码长出来）。

    不覆盖：散文里有没有偷偷把 L2 说成 L3；证据内容与主张是否一致。
    """
    r = _run_check([sys.executable, os.path.join("tools", "evidence_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "PASS  evidence-levels" in txt:
        ok("evidence-levels", txt.splitlines()[-1].replace("PASS  evidence-levels", "").strip()
           or "ledger is self-consistent")
    else:
        fail("evidence-levels", txt[-320:])


def check_api_doc_fresh():
    """docs/api/Ark RTOS_API参考.md must match what the generator produces today.

    The reference sat at 2026-10-07 for five weeks while the kernel changed under
    it, which is exactly how an API doc becomes worse than none: a reader trusts
    a stale signature more than a missing one.

    The check regenerates into the working tree and asks git whether the file
    changed.  It therefore has to be run *after* the file has been committed at
    least once - the first time it runs on a new doc it will fail with a diff,
    which is the intended behaviour (commit the regenerated file).
    """
    gen = os.path.join(ROOT, "tools", "gen_api_doc.py")
    doc = os.path.join(ROOT, "docs", "api", "Ark RTOS_API参考.md")
    if not os.path.exists(gen) or not os.path.exists(doc):
        fail("api-doc", "generator or reference is missing")
        return
    env = dict(os.environ)
    env.pop("PYTHONHOME", None)
    env.pop("PYTHONPATH", None)
    r = subprocess.run([sys.executable, gen, "--md"], cwd=ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        fail("api-doc", "generator failed: %s"
             % (r.stdout or b"").decode("utf-8", "replace").strip()[-200:])
        return
    d = subprocess.run(["git", "diff", "--stat", "--", "docs/api/Ark RTOS_API参考.md"],
                       cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if (d.stdout or b"").strip():
        fail("api-doc", "reference is stale, regenerate and commit it")
    else:
        ok("api-doc", "reference matches the current sources")


def check_layout_and_scatter():
    """Two different questions, answered separately.

    (a) Does the layout in the partition header satisfy every constraint?
        gen_scatter.py --check answers this for real.
    (b) Is a .sct on disk guaranteed to be the generator's output?
        Only if the file is not tracked: an untracked build product cannot
        carry a hand edit into master.  Claiming (b) from (a) is a false pass.
    """
    env = dict(os.environ)
    env.pop("PYTHONHOME", None)
    env.pop("PYTHONPATH", None)
    r = subprocess.run([sys.executable, os.path.join("tools", "gen_scatter.py"), "--check"],
                       cwd=ROOT, env=env, capture_output=True, encoding="utf-8",
                       errors="replace")
    if r.returncode == 0 and "检查通过" in (r.stdout or ""):
        ok("layout-check", "partition layout has no overlap / out of range")
    else:
        fail("layout-check", ((r.stdout or "") + (r.stderr or "")).strip()[:300])

    r = subprocess.run(["git", "ls-files", "*.sct"], cwd=ROOT, env=env,
                       capture_output=True, encoding="utf-8", errors="replace")
    tracked = [l for l in (r.stdout or "").splitlines() if l.strip()]
    if tracked:
        fail("sct-not-tracked", "generated .sct is in the index: %s" % ", ".join(tracked[:5]))
    else:
        ok("sct-not-tracked", "no .sct is tracked (build product stays out of git)")


# ---------------------------------------------------------------- check 5
def check_no_partition_include_in_app():
    """Separately linked images must not include the partition header.

    App / driver SDK code and the example app / driver projects take their
    layout at run time through SVC 0x18.  Only #include lines count - a comment
    that points at the header as the place where the layout is decided is
    exactly the documentation we want.

    board/ and the kernel projects are *allowed* to include it (they are the
    single source), and so is kernelsrc/src - the kernel's own loader reads the
    pool layout from there by design.  The first version of this check policed
    everything outside kernelsrc/src and flagged all five legitimate includers.
    """
    bad = []
    for rel, path in own_sources():
        if not any(m in rel for m in APP_SIDE_MARKERS):
            continue
        if rel.endswith("arkrtos_partition.h"):
            continue
        for _, line in code_lines(stored_bytes(rel, path)):
            s = line.strip()
            if s.startswith(b"#") and b"include" in s and b"arkrtos_partition.h" in s:
                bad.append(rel)
                break
    if bad:
        fail("no-app-partition-include", ", ".join(sorted(set(bad))[:5]))
    else:
        ok("no-app-partition-include", "App / SDK stay layout-agnostic")


# ---------------------------------------------------------------- check 6
def check_builds(uv4):
    """Both kernel projects must compile with 0 errors (1 warning is the baseline)."""
    if not uv4 or not os.path.exists(uv4):
        fail("build", "UV4 not found: %s" % uv4)
        return
    for proj in ("example/stm32f427/kernel/ARKRTOS_TEST/MDK-ARM/ARKRTOS_TEST.uvprojx",
                 "example/stm32f401/kernel/ARKRTOS_TEST/MDK-ARM/ARKRTOS_TEST.uvprojx"):
        full = os.path.join(ROOT, proj)
        name = "F427" if "f427" in proj else "F401"
        if not os.path.exists(full):
            fail("build:%s" % name, "missing project")
            continue
        # UV4 needs an absolute -o path that actually exists; C:\tmp is the
        # convention in this repo (D:\tmp does not exist and UV4 then silently
        # writes no log at all).
        logdir = "C:/tmp" if os.path.isdir("C:/tmp") else os.environ.get("TEMP", ".")
        log = os.path.join(logdir, "ci_gate_%s.log" % name)
        env = dict(os.environ)
        env.pop("PYTHONHOME", None)
        env.pop("PYTHONPATH", None)
        try:
            r = subprocess.run([uv4, "-r", full, "-o", log], env=env,
                               capture_output=True, encoding="utf-8",
                               errors="replace", timeout=900)
        except subprocess.TimeoutExpired:
            fail("build:%s" % name, "compile timed out")
            continue
        txt = open(log, "r", errors="replace").read() if os.path.exists(log) else ""
        m = re.search(r"(\d+)\s+Error\(s\)", txt)
        errs = int(m.group(1)) if m else -1
        if errs == 0:
            ok("build:%s" % name, "0 Error(s)")
        else:
            fail("build:%s" % name, "exit=%d errors=%s" % (r.returncode, errs))


# ---------------------------------------------------------------- check 7
def _dump_log(name, text):
    """把一道闸门的完整输出落盘，返回路径。

    失败信息只截最后几百字符，够说明「哪一条没过」，不够定位「为什么」：
    QEMU 起不来、构建目录里的旧 .o、串口截断，三种原因都只在完整输出里
    分得清。落盘一份，失败信息里带上路径，下一次就不必重跑一遍去猜。
    """
    try:
        d = os.path.join(ROOT, "build", "gate_logs")
        if not os.path.isdir(d):
            os.makedirs(d)
        path = os.path.join(d, name + ".log")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        return path
    except Exception:
        return "(日志落盘失败)"


def _run_check(cmd):
    env = dict(os.environ)
    env.pop("PYTHONHOME", None)
    env.pop("PYTHONPATH", None)
    return subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True,
                          encoding="utf-8", errors="replace")


def check_config_generated():
    """The generated headers / tables must match their sources.

    Two build products are committed on purpose (autoconf.h and each board's
    arkrtos_dt.c) so a clone can build without running the generators.  That
    only holds if they are regenerated whenever their input changes, so this
    check regenerates in memory and compares.  A hand-edited product, or a
    source change that forgot to regenerate, fails here instead of showing up
    as a mystery on the board.
    """
    r = _run_check([sys.executable, "tools/kconfig/genconfig.py",
                    "--defconfig", "config/arkrtos_defconfig",
                    "--out", "kernelsrc/include/arkrtos_autoconf.h",
                    "--check"])
    if r.returncode == 0:
        ok("config-generated", "autoconf.h matches Kconfig + defconfig")
    else:
        fail("config-generated", ((r.stdout or "") + (r.stderr or "")).strip()[:300])


def check_devicetree_generated():
    """Every committed board/*/arkrtos_dt.c must be the dtc_lite output."""
    boards = []
    for dts in sorted(os.listdir(os.path.join(ROOT, "dts"))):
        if dts.endswith(".dts"):
            boards.append(os.path.splitext(dts)[0])
    if not boards:
        fail("devicetree-generated", "no dts/*.dts found")
        return
    bad = []
    for b in boards:
        r = _run_check([sys.executable, "tools/dts/dtc_lite.py",
                        "--dts", "dts/%s.dts" % b,
                        "--out", "board/%s/arkrtos_dt.c" % b, "--check"])
        if r.returncode != 0:
            bad.append(b)
    if bad:
        fail("devicetree-generated", "stale board data for: %s" % ", ".join(bad))
    else:
        ok("devicetree-generated", "%d board(s) match their DTS" % len(boards))

def check_devicetree_dtb():
    """Every dts/*.dts must produce a spec-valid flattened blob.

    The MMU tier boots a .dtb (see docs/设备树.md), so the DTS has to be
    encodable as a standard flattened device tree, not only as the C table
    the MCU tier links.  The verifier here is a separate reader-side
    implementation on purpose: reusing the writer's own helpers would let a
    byte-order or padding mistake corroborate itself, and this check would
    then pass while the blob was still wrong.
    """
    r = _run_check([sys.executable, "tools/dts/dtb_verify.py", "--all"])
    if r.returncode == 0:
        out = (r.stdout or "").strip().splitlines()
        n = len([ln for ln in out if ln.endswith(" OK")])
        ok("devicetree-dtb", "%d board(s) produce spec-valid DTB" % n)
    else:
        fail("devicetree-dtb",
             ((r.stdout or "") + (r.stderr or "")).strip()[:300])

def check_fdt():
    """内核侧 FDT 解析器（ARKRTOS_USE_FDT）必须两头都是读数。

    阶段 1 只证明「DTS 能编出 spec-valid 的 dtb」，那是文件层面的事实：内核
    里有没有人读这棵树、读得对不对、读错时会不会拿默认值凑一棵树，一件都
    没测。而这个开关默认关，关着的时候 arkrtos_fdt.c 整文件不经过编译器，
    写错也全绿。

    tools/fdt_gate.py 把四件事钉住：关档 .o 符号表全空；六块板的 dtb 与生成的
    C 平表逐节点对账且变异会被抓；LA64 整镜像编得过并真跑一遍（开机
    `[fdt] blob=<n> rc=0 model=... nodes=<k>`，n 必须等于 build 里那个 .dtb 的
    字节数）；同一次启动里 dt（C 平表）与 fdt（解析出的 blob）输出逐行相同。
    两个读数的判据各自喂坏样本，必须被判红。同一次启动里还要证明
    `/chosen/stdout-path` 被消费（[fdt] console ... reason=ok match=1）、
    路径 / 别名 / phandle 指向同一个节点、fdt save+load 往返 nodes 不变、
    空文件被拒且旧树保留。最后另起一次启动做 overlay：应用前
    `fdt path /overlay-demo-node` 必须取不到，应用后 nodes 8→9、
    model=fdt-overlay-demo、fragment 写进去的两条属性都读得回。

    不覆盖：带 target=<&label> / __fixups__ / __symbols__ / 删除语义的
    fragment（实现侧明确拒绝）、bootloader 交 dtb、镜像重定位。
    """
    r = _run_check([sys.executable, os.path.join("tools", "fdt_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "FDT-GATE OK" in txt:
        ok("fdt",
           "default off zero symbols / 6 boards dtb-vs-C-table + 6 mutation caught / "
           "LA64 build / boot blob==dtb size, nodes=8 / dt==fdt line by line / "
           "console path+baud match / fdt chosen+path+phandle same node / "
           "save+load nodes unchanged, empty file refused and old tree kept / "
           "overlay absent-before -> nodes 8->9 + model/props read back / "
           "5 self-check groups")
    else:
        fail("fdt", "%s（完整输出：%s）" % (txt[-320:], _dump_log("fdt_gate", txt)))

def check_board_config_generated():
    """Every generated board header must be the genboard.py output.

    The MMU tier describes its machine in a Kconfig fragment plus a defconfig
    and derives board/<board>/arkrtos_board_config.h from them.  The header is
    committed so a clone can build without running the generator, which only
    holds if it is regenerated whenever a value changes.  This check also
    catches a defconfig value that a depends-on silently eats: genboard
    refuses to write in that case, so the check fails with the offending
    symbol instead of producing a quietly smaller board.

    Boards that keep a hand-written header (the MCU + MDK tier, see
    docs/Kconfig配置系统.md section 9) are not in this list on purpose.
    """
    boards = []
    frag_dir = os.path.join(ROOT, "config", "kconfig", "board")
    if os.path.isdir(frag_dir):
        for f in sorted(os.listdir(frag_dir)):
            if f.startswith("Kconfig."):
                boards.append(f.split(".", 1)[1])
    if not boards:
        fail("board-config-generated", "no config/kconfig/board/Kconfig.* found")
        return
    bad = []
    for bd in boards:
        r = _run_check([sys.executable, "tools/kconfig/genboard.py",
                        "--board", bd, "--check"])
        if r.returncode != 0:
            bad.append(bd)
    if bad:
        fail("board-config-generated",
             "stale board header for: %s" % ", ".join(bad))
    else:
        ok("board-config-generated",
           "%d generated board header(s) match their Kconfig + defconfig"
           % len(boards))


# ---------------------------------------------------------------- check 8
HOST_CC_CANDIDATES = (
    "D:/mingw/MinGW/bin/gcc.exe",
    "C:/mingw/MinGW/bin/gcc.exe",
)

def _find_host_cc():
    import shutil
    env = os.environ.get("ARKRTOS_HOST_CC")
    if env and os.path.exists(env):
        return env
    for cand in HOST_CC_CANDIDATES:
        if os.path.exists(cand):
            return cand
    return shutil.which("gcc")

def check_utest_host():
    """The utest framework's own accounting must be provably correct on the host.

    tools/utest_host_selftest.c drives the real kernelsrc/src/arkrtos_utest.c
    with a known number of passes / failures / skips / assertions and checks the
    report, so a framework regression is caught without a board in the loop.
    A missing host compiler is a FAIL, not a skip: an unrun check is not a pass.
    """
    cc = _find_host_cc()
    if not cc:
        fail("utest-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with TempDir() as td:
        exe = os.path.join(td, "utest_host_selftest.exe" if os.name == "nt"
                               else "utest_host_selftest")
        cmd = [cc]
        if "mingw" in cc.replace("\\", "/").lower():
            cmd.append("-B" + os.path.dirname(cc))
        cmd += ["-std=gnu11", "-Ikernelsrc/include", "-o", exe,
                "tools/utest_host_selftest.c", "kernelsrc/src/arkrtos_utest.c"]
        r = _run_check(cmd)
        if r.returncode != 0:
            fail("utest-host", "selftest did not build: %s"
                 % (r.stderr or "").strip().splitlines()[-1:])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode == 0 and "HOST-SELFTEST OK" in txt:
            ok("utest-host", "framework accounting verified on host")
        else:
            fail("utest-host", "selftest failed: %s" % txt.strip()[:160])

# 指针宽度类型在四份 types.h 里必须逐字一致。
#
# 这不是「为了整齐」：ulog 的 %p 要用 arkrtos_uptr_t，而 App / 驱动 / 小程序
# 编的是 SDK 侧的精简 types.h，不是内核那份。2026-11-26 加了 %p 的宽度分支
# 之后，真正跑出来的第一次全量门禁就红了——eset-syntax 与 analyzer 两处
# armclang / rv32-gcc 都报 'arkrtos_uptr_t' undeclared，因为三份 SDK 副本
# 里没有这个类型。这条 pin 只在四份文件同时改对时才绿。
#
# 真源是内核那份：canon 直接从 kernelsrc/include/arkrtos_types.h 里截，
# 不在这里再抄一遍——抄一份就是给同一件事留两个会分叉的写法。
_SDK_TYPES_COPIES = (
    "kernelsrc/sdk/app_sdk/arkrtos_types.h",
    "kernelsrc/sdk/driver_sdk/arkrtos_types.h",
    "kernelsrc/sdk/mini_sdk/arkrtos_types.h",
)

def _ptr_width_canon():
    """Return the pointer-width block as it stands in the kernel types.h."""
    txt = read(os.path.join(ROOT, "kernelsrc/include/arkrtos_types.h"))
    txt = txt.decode("utf-8", "replace").replace("\r\n", "\n")
    head = "#if (defined(__SIZEOF_POINTER__)"
    i = txt.find(head)
    if i < 0:
        return None
    tail_mark = "typedef unsigned int arkrtos_uptr_t;"
    j = txt.find(tail_mark, i)
    if j < 0:
        return None
    mark = "\n#endif\n#endif"
    k = txt.find(mark, j)
    if k < 0:
        return None
    k += len(mark)
    return txt[i:k]

def _sdk_types_pin_bad():
    """Copies whose pointer-width block differs from the kernel header's."""
    canon = _ptr_width_canon()
    if not canon:
        return ["kernelsrc/include/arkrtos_types.h lacks the pointer-width block"]
    bad = []
    for rel in _SDK_TYPES_COPIES:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            bad.append(rel + " missing")
            continue
        txt = read(path).decode("utf-8", "replace").replace("\r\n", "\n")
        if canon not in txt:
            bad.append(rel + " differs from the kernel copy")
    return bad

def check_sdk_stdio_host():
    """The SDK's printf shim must produce exactly the right bytes.

    arkrtos_stdio.c is what makes `#include <stdio.h>` + `printf("Hello, World!\\n")`
    work in an App or a mini program without any toolchain retarget.  Its failure
    mode is silent: the program prints a *different* sentence, which nobody
    notices on a board but a host byte comparison catches immediately.
    A missing host compiler is a FAIL, not a skip.
    """
    bad = _sdk_types_pin_bad()
    if bad:
        fail("sdk-stdio-host", "pointer-width type drifted: " + "; ".join(bad))
        return
    cc = _find_host_cc()
    if not cc:
        fail("sdk-stdio-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with TempDir() as td:
        exe = os.path.join(td, "stdio_host.exe" if os.name == "nt" else "stdio_host")
        cmd = [cc]
        if "mingw" in cc.replace("\\", "/").lower():
            cmd.append("-B" + os.path.dirname(cc))
        cmd += ["-std=gnu11", "-Ikernelsrc/include", "-o", exe,
                "kernelsrc/sdk/app_sdk/tests/test_stdio_host.c",
                "kernelsrc/sdk/app_sdk/arkrtos_stdio.c"]
        r = _run_check(cmd)
        if r.returncode != 0:
            fail("sdk-stdio-host", "gate did not build: %s"
                 % (r.stderr or "").strip().splitlines()[-1:])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode == 0 and "STDIO-HOST OK" in txt:
            ok("sdk-stdio-host", "printf/puts/putchar byte-exact on host + pointer-width type identical in all 4 types.h copies")
        else:
            fail("sdk-stdio-host", "gate failed: %s" % txt.strip()[:160])

def check_sbuf_host():
    """Byte-stream / message buffers: the ring arithmetic must be provably right.

    kernelsrc/tests/test_sbuf_host.c links the real arkrtos_sbuf.c against stub
    scheduler / critical-section / trace functions, so the byte moving (wrap
    around, partial send, length-prefixed framing, discard-without-desync) is
    exercised on the host.  The blocking paths are NOT covered here - they need
    a real scheduler - so this gate must not be read as "the module is verified".
    A missing host compiler is a FAIL, not a skip.
    """
    cc = _find_host_cc()
    if not cc:
        fail("sbuf-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with TempDir() as td:
        exe = os.path.join(td, "sbuf_host.exe" if os.name == "nt" else "sbuf_host")
        cmd = [cc]
        if "mingw" in cc.replace("\\", "/").lower():
            cmd.append("-B" + os.path.dirname(cc))
        cmd += ["-std=gnu11", "-DARKRTOS_USE_SBUF=1",
                "-Ikernelsrc/include", "-Ikernelsrc/components/mdk_trace",
                "-Iboard/stm32f427", "-Iconfig", "-o", exe,
                "kernelsrc/tests/test_sbuf_host.c", "kernelsrc/src/arkrtos_sbuf.c"]
        r = _run_check(cmd)
        if r.returncode != 0:
            fail("sbuf-host", "gate did not build: %s"
                 % (r.stderr or "").strip().splitlines()[-1:])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode == 0 and "SBUF-HOST OK" in txt:
            ok("sbuf-host", "ring + message framing verified on host")
        else:
            fail("sbuf-host", "gate failed: %s" % txt.strip()[:160])

def check_mempool_host():
    """Fixed-block pool: the pointer arithmetic must be provably right.

    kernelsrc/tests/test_mempool_host.c links the real arkrtos_mempool.c against
    stub critical-section / task-id functions, so creation guards (block
    geometry, arena alignment, size wrap), the O(1) alloc/free path and the
    "a refused free must not touch the pool" contract are exercised on the host.
    SVC 0x20 argument checking and App-side wrappers are NOT covered here - they
    need the SVC boundary - so this gate must not be read as "the channel is
    verified". A missing host compiler is a FAIL, not a skip.
    """
    cc = _find_host_cc()
    if not cc:
        fail("mempool-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with TempDir() as td:
        exe = os.path.join(td, "mempool_host.exe" if os.name == "nt" else "mempool_host")
        cmd = [cc]
        if "mingw" in cc.replace("\\", "/").lower():
            cmd.append("-B" + os.path.dirname(cc))
        cmd += ["-std=gnu11", "-DARKRTOS_USE_MEMPOOL=1",
                "-Ikernelsrc/include", "-Ikernelsrc/components/mdk_trace",
                "-Iboard/stm32f427", "-Iconfig", "-o", exe,
                "kernelsrc/tests/test_mempool_host.c", "kernelsrc/src/arkrtos_mempool.c"]
        r = _run_check(cmd)
        if r.returncode != 0:
            fail("mempool-host", "gate did not build: %s"
                 % (r.stderr or "").strip().splitlines()[-1:])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode == 0 and "MEMPOOL-HOST OK" in txt:
            ok("mempool-host", "block geometry + alloc/free contract verified on host")
        else:
            fail("mempool-host", "gate failed: %s" % txt.strip()[:160])

def check_mq_host():
    """Message queues: the slot bookkeeping must be provably right.

    kernelsrc/tests/test_mq_host.c links the real arkrtos_mq.c against stub
    scheduler / critical-section / trace functions, so the ring of slots (tail
    wrap, declared-length copy, truncation that still consumes the whole
    message, "full" refusing without overwriting, delete invalidating the
    handle) is exercised on the host.  The blocking paths are NOT covered here
    - they need a real scheduler - so this gate must not be read as "the module
    is verified". A missing host compiler is a FAIL, not a skip.
    """
    cc = _find_host_cc()
    if not cc:
        fail("mq-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with TempDir() as td:
        exe = os.path.join(td, "mq_host.exe" if os.name == "nt" else "mq_host")
        cmd = [cc]
        if "mingw" in cc.replace("\\", "/").lower():
            cmd.append("-B" + os.path.dirname(cc))
        cmd += ["-std=gnu11", "-DARKRTOS_USE_MQ=1", "-DARKRTOS_MQ_HOST_TEST",
                "-Ikernelsrc/include", "-Ikernelsrc/components/mdk_trace",
                "-Iboard/stm32f427", "-Iconfig", "-o", exe,
                "kernelsrc/tests/test_mq_host.c", "kernelsrc/src/arkrtos_mq.c"]
        r = _run_check(cmd)
        if r.returncode != 0:
            fail("mq-host", "gate did not build: %s"
                 % (r.stderr or "").strip().splitlines()[-1:])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode == 0 and "MQ-HOST OK" in txt:
            ok("mq-host", "slot wrap + declared-length contract verified on host")
        else:
            fail("mq-host", "gate failed: %s" % txt.strip()[:160])

def check_sync_host():
    """Synchronisation primitives: the token-conservation rules must hold.

    kernelsrc/tests/test_sync_host.c links the real arkrtos_sync.c against stub
    scheduler / trace functions, but unlike the other host gates it hands
    arkrtos_task_get_current() a *settable* pointer, so ownership decisions
    (who may unlock, what counts as a recursive lock) are really exercised.
    Covered: initial count, take/exhaust/0-timeout, post parking the token in
    the count, delete invalidating every entry point, non-owner unlock refused,
    deleted object reported as not held.  Blocking, hand-off and priority
    inheritance need a real scheduler and are NOT covered - this gate must not
    be read as "the module is verified". A missing host compiler is a FAIL.
    """
    cc = _find_host_cc()
    if not cc:
        fail("sync-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with TempDir() as td:
        exe = os.path.join(td, "sync_host.exe" if os.name == "nt" else "sync_host")
        cmd = [cc]
        if "mingw" in cc.replace("\\", "/").lower():
            cmd.append("-B" + os.path.dirname(cc))
        cmd += ["-std=gnu11", "-DARKRTOS_USE_MTX_RECURSIVE=1", "-DARKRTOS_SYNC_HOST_TEST",
                "-Ikernelsrc/include", "-Ikernelsrc/components/mdk_trace",
                "-Iboard/stm32f427", "-Iconfig", "-o", exe,
                "kernelsrc/tests/test_sync_host.c", "kernelsrc/src/arkrtos_sync.c"]
        r = _run_check(cmd)
        if r.returncode != 0:
            fail("sync-host", "gate did not build: %s"
                 % (r.stderr or "").strip().splitlines()[-1:])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode == 0 and "SYNC-HOST OK" in txt:
            ok("sync-host", "token conservation + ownership verified on host")
        else:
            fail("sync-host", "gate failed: %s" % txt.strip()[:160])

def check_loader_host():
    """App image loader: header rejection order and relocation semantics.

    kernelsrc/tests/test_loader_host.c links the real arkrtos_loader.c against
    stub scheduler / partition-table / crash-ledger / port functions. A
    relocation applied to the wrong byte does not fail, it just makes the app
    jump somewhere else on the board - the kind of silent wrong answer a UART
    log cannot show. Covered: check_header's rejection order and boundaries
    (magic, compat id, type out of range / not in mask, image_size 0 and
    wrap, entry out of range, reloc_offset / count / encoding version /
    payload_offset, table entry alignment / past the end / ordering, ram_size
    0 and over the single-block cap), and reloc_apply's patch contract (word
    entries add delta_rom / delta_ram separately, MOVW+MOVT decode-recode
    keeps the instruction bits, a table entry cut by the chunk end returns
    its start and leaves idx alone, entries before buf_off are skipped but
    still consume an index), and - with -DARKRTOS_USE_IMAGE_CRC_RECHECK=1 -
    crc_recheck on a relocated image in both directions (nominal crc matched
    after the two deltas are undone, plus ERR_CRC / ERR_PARAM / ERR_SIZE /
    ERR_RELOC rejections).

    NOT covered: load_image, streaming install, flash erase/program, pool
    relocation, unload, signature check, crash accounting, and any real
    start-up. The CRC recheck is verified on host only for the pure function;
    install-then-recheck still needs a board or QEMU. Nothing here has run on a board, so this gate must not be read
    as "the install path is verified". A missing host compiler is a FAIL.
    """
    cc = _find_host_cc()
    if not cc:
        fail("loader-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with TempDir() as td:
        exe = os.path.join(td, "loader_host.exe" if os.name == "nt" else "loader_host")
        cmd = [cc]
        if "mingw" in cc.replace("\\", "/").lower():
            cmd.append("-B" + os.path.dirname(cc))
        # 与 tools/coverage_gate.py 的 CASES loader 项保持一致：
        # 编入 -DARKRTOS_USE_IMAGE_CRC_RECHECK=1 才能覆盖 crc_recheck 的正/负向。
        cmd += ["-std=gnu11", "-fno-asynchronous-unwind-tables",
                "-DARKRTOS_USE_IMAGE_CRC_RECHECK=1",
                "-Ikernelsrc/include", "-Ikernelsrc/components/mdk_trace",
                "-Iboard/stm32f427", "-Iconfig", "-o", exe,
                "kernelsrc/tests/test_loader_host.c", "kernelsrc/src/arkrtos_loader.c"]
        r = _run_check(cmd)
        if r.returncode != 0:
            fail("loader-host", "gate did not build: %s"
                 % (r.stderr or "").strip().splitlines()[-1:])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode == 0 and "LOADER-HOST OK" in txt:
            ok("loader-host", "header rejection order + reloc patch contract verified on host")
        else:
            fail("loader-host", "gate failed: %s" % txt.strip()[:160])

def check_loader_accept_verify():
    """Claiming an image must not trust the state word alone (SEC-G09).

    ARKRTOS_USE_IMAGE_ACCEPT_VERIFY defaults to 0, so the recheck inside
    arkrtos_loader_accept() is compiled out of every default build: a pool
    copy that was damaged after installation is claimed on the strength of one
    VALID word.  With the switch on, the installer's check is re-run on the
    image as it lies in Flash -- and Flash holds the *relocated* form, so the
    ROM / RAM deltas have to be undone before the bytes can be compared.  Two
    ways that goes silently wrong: the deltas are computed from different
    inputs than at install time, or a MOVW entry is decoded by a second, copied
    implementation that drifts.  This compiles the real arkrtos_loader.c +
    arkrtos_sign.c with both switches on and drives the known-answer image the
    host test builds: landed-but-relocated verify must pass; flags / payload /
    patched word / reloc table / signature tampering must each be refused;
    runtime_ram_base drift must be detected (it feeds the delta); an unpatched
    ram base and short caps must report "cannot tell", never "matched".
    It also compiles the CRC-only fallback (ACCEPT_VERIFY without IMAGE_SIGN)
    so that branch is not dead text.  Host only: the claim path inside a real
    pool scan still needs a board or QEMU.  A missing host compiler is a FAIL.
    """
    cc = _find_host_cc()
    if not cc:
        fail("loader-accept-verify", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    key = "-DARKRTOS_IMAGE_SIGN_KEY={" + ",".join(
        "0x%02X" % (i & 0xFF) for i in range(32)) + "}"
    with TempDir() as td:
        exe = os.path.join(td, "acc_verify.exe" if os.name == "nt" else "acc_verify")
        cmd = [cc]
        if "mingw" in cc.replace("\\", "/").lower():
            cmd.append("-B" + os.path.dirname(cc))
        cmd += ["-std=gnu11", "-fno-asynchronous-unwind-tables",
                "-DARKRTOS_USE_IMAGE_SIGN=1",
                "-DARKRTOS_USE_IMAGE_ACCEPT_VERIFY=1",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Ikernelsrc/include", "-Ikernelsrc/components/mdk_trace",
                "-Iboard/stm32f427", "-Iconfig", "-o", exe,
                "kernelsrc/tests/test_loader_accept_host.c",
                "kernelsrc/src/arkrtos_loader.c",
                "kernelsrc/src/arkrtos_sign.c", key]
        r = _run_check(cmd)
        if r.returncode != 0:
            fail("loader-accept-verify", "gate did not build: %s"
                 % (r.stderr or "").strip().splitlines()[-1:])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or "ACCEPT-VERIFY-HOST OK" not in txt:
            fail("loader-accept-verify", "gate failed: %s" % txt.strip()[:200])
            return
        # 没有签名档时 accept() 退回 CRC 反算：那条分支只在这种组合下编入，
        # 默认档与上面的签名档都看不到它，所以这里单独编一遍。
        obj = os.path.join(td, "loader_crc_only.o")
        cmd2 = [cc]
        if "mingw" in cc.replace("\\", "/").lower():
            cmd2.append("-B" + os.path.dirname(cc))
        cmd2 += ["-std=gnu11", "-c",
                 "-DARKRTOS_USE_IMAGE_ACCEPT_VERIFY=1",
                 "-Ikernelsrc/include", "-Ikernelsrc/components/mdk_trace",
                 "-Iboard/stm32f427", "-Iconfig", "-o", obj,
                 "kernelsrc/src/arkrtos_loader.c"]
        r = _run_check(cmd2)
        if r.returncode != 0:
            fail("loader-accept-verify", "crc-only fallback did not build: %s"
                 % (r.stderr or "").strip().splitlines()[-1:])
            return
    ok("loader-accept-verify",
       "relocated recheck verifies + 5 tamper classes refused + unpatched ram base is not a pass")

def check_fifo_host():
    """Byte FIFO: capacity accounting and the two silent-loss counters.

    kernelsrc/tests/test_fifo_host.c links the real arkrtos_fifo.c; the module
    needs no scheduler, no critical section and no board resource, so the test
    file carries no kernel stubs. Covered: create's size threshold (size <= 8
    returns 0 without touching the caller's buffer; usable capacity is
    size - 9), write refusing the remainder when full without overwriting
    unread data, ring wrap preserving byte order, short read returning what
    is stored, len <= 0 as a no-op, bad-magic reporting on both write and
    read (read reports calls only, it loses no bytes), NULL handle on the
    same path, and that write_refused accumulates across all FIFOs.

    NOT covered: anything beyond the documented single-producer /
    single-consumer contract, size > 65543 (uint16 truncation, a known
    limit), and a hand-built size == 0 header (division by zero, caller
    breach, not provoked here). A missing host compiler is a FAIL.
    """
    cc = _find_host_cc()
    if not cc:
        fail("fifo-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with TempDir() as td:
        exe = os.path.join(td, "fifo_host.exe" if os.name == "nt" else "fifo_host")
        cmd = [cc]
        if "mingw" in cc.replace("\\", "/").lower():
            cmd.append("-B" + os.path.dirname(cc))
        cmd += ["-std=gnu11", "-fno-asynchronous-unwind-tables",
                "-Ikernelsrc/include", "-Ikernelsrc/components/mdk_trace",
                "-Iboard/stm32f427", "-Iconfig", "-o", exe,
                "kernelsrc/tests/test_fifo_host.c", "kernelsrc/src/arkrtos_fifo.c"]
        r = _run_check(cmd)
        if r.returncode != 0:
            fail("fifo-host", "gate did not build: %s"
                 % (r.stderr or "").strip().splitlines()[-1:])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode == 0 and "FIFO-HOST OK" in txt:
            ok("fifo-host", "capacity + wrap order + refused/bad-magic counters verified on host")
        else:
            fail("fifo-host", "gate failed: %s" % txt.strip()[:160])

def check_fault_host():
    """Fault record ring: the index semantics the post-mortem depends on.

    kernelsrc/tests/test_fault_host.c links the real arkrtos_fault.c against
    three stubs (enter/exit critical, arkrtos_kernel_tick). That table is the
    only clue left after a crash, and an off-by-one in the ring shows the
    previous fault - a plausible but wrong answer over the console. Covered:
    empty after init, 0 = oldest before the ring fills, count capped at
    ARKRTOS_FAULT_RECORD_NUM, overwrite-oldest on wrap, "0 = total - N"
    after several wraps, out-of-range index returns NULL, tick captured per
    record, init resets the write cursor too, and every record sits between
    a matched enter/exit pair.

    NOT covered: atomicity under real interrupt nesting (the stubs no-op the
    critical section, so the count only proves pairing), calling it from a
    real HardFault, and the SVC read packing format. A missing host compiler
    is a FAIL.
    """
    cc = _find_host_cc()
    if not cc:
        fail("fault-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with TempDir() as td:
        exe = os.path.join(td, "fault_host.exe" if os.name == "nt" else "fault_host")
        cmd = [cc]
        if "mingw" in cc.replace("\\", "/").lower():
            cmd.append("-B" + os.path.dirname(cc))
        cmd += ["-std=gnu11", "-fno-asynchronous-unwind-tables",
                "-Ikernelsrc/include", "-Ikernelsrc/components/mdk_trace",
                "-Iboard/stm32f427", "-Iconfig", "-o", exe,
                "kernelsrc/tests/test_fault_host.c", "kernelsrc/src/arkrtos_fault.c"]
        r = _run_check(cmd)
        if r.returncode != 0:
            fail("fault-host", "gate did not build: %s"
                 % (r.stderr or "").strip().splitlines()[-1:])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode == 0 and "FAULT-HOST OK" in txt:
            ok("fault-host", "ring index semantics + tick capture + critical pairing verified on host")
        else:
            fail("fault-host", "gate failed: %s" % txt.strip()[:160])

def check_release_consistency():
    """Version header, both READMEs, release notes and the latest tag must agree.

    The header comment has claimed since day one that the tag and the README
    badge move together with the three numbers; nothing enforced it, and the
    repo ended up claiming `0.0.2` in the README with no tag at all.  The
    checks live in tools/release_check.py so they can also be run alone before
    tagging; this wrapper is what makes them part of "before it lands".
    """
    r = _run_check([sys.executable, "tools/release_check.py"])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0:
        ok("release-consistency", txt.splitlines()[-1] if txt else "")
    else:
        fail("release-consistency", txt[-300:])


def check_tickless():
    """tickless idle: the accounting must be provably right, and the ON path must compile.

    tools/tickless_host_gate.py does both halves for the same reason: the switch
    defaults to 0, so without the second half every line of this feature is dead
    code in the normal build and a typo in it would sit there until someone
    turned the switch on. A missing compiler is a FAIL, not a skip.
    """
    r = _run_check([sys.executable, os.path.join("tools", "tickless_host_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "TICKLESS-GATE OK" in txt:
        ok("tickless", "phase accounting (host) + ON-path compile verified")
    else:
        fail("tickless", txt[-260:])

def check_arkmake_syntax():
    """ark make must compile with the switch ON.

    ARKRTOS_USE_ARKMAKE defaults to 0 and the component is only wired into the
    H743 GCC project, while --build runs the F427/F401 Keil projects. Without
    this check the two wiring files are never compiled by any gate: a typo in
    the command layer stays green until someone turns the switch on. The gate
    script does the four files with the H743 include/define set; a missing
    armclang is a FAIL, not a skip.
    """
    r = _run_check([sys.executable, os.path.join("tools", "arkmake_syntax_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "ARKMAKE-GATE OK" in txt:
        ok("arkmake", "ON-path compile verified (4 files)")
    else:
        fail("arkmake", txt[-260:])

def check_eset_host():
    """32-bit event set (ARKRTOS_USE_ESET): judge/deliver accounting on host.

    kernelsrc/tests/test_eset_host.c links the real arkrtos_eset.c against stub
    scheduler / trace / port functions.  The object table is exported through
    -DARKRTOS_ESET_HOST_TEST so the test can park two waiters on one set (the
    CLEAR single-consumer rule and the hand-off snapshot can only be checked
    that way), and the blocking hand-off path is driven by a hook that runs
    inside block_in_critical.  Covered: create/naming/slot exhaustion, set
    accumulation, get/clear, non-blocking try, OR vs AND, CLEAR taken once,
    timeout leaving no phantom registration, the handed-off value computed at
    hand-off time, non-HANDOFF wake not counted as a hit, delete waking
    everyone, task-release reaping, argument validation.  NOT covered: real
    priority/wake ordering, the user-mode SVC polling path and ISR latency --
    those need a board.  A missing host compiler is a FAIL.
    """
    r = _run_check([sys.executable, os.path.join("tools", "eset_host_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "ESET-HOST OK" in txt:
        ok("eset-host", "OR/AND judge + CLEAR single-consumer + hand-off snapshot verified")
    else:
        fail("eset-host", "gate failed: %s" % txt.strip()[-300:])

def check_waitmulti_host():
    """Multi-object ready probe: dispatch, bit mask and ANY/ALL judgement.

    kernelsrc/tests/test_waitmulti_host.c links the real arkrtos_waitmulti.c
    against programmable stubs for the four *_ready_internal() queries and an
    empty disable/enable-irq pair.  The module has no scheduler involvement and
    no token hand-off, so the whole decision surface is checkable on host.
    Covered: all-not-ready, ANY partial (bit-exact mask), ALL partial, ALL
    satisfied, n=0 / n over ARKRTOS_WAITMULTI_MAX_OBJS, illegal opt, NULL
    pointers, one dead handle rejecting the whole group with the mask cleared,
    unsupported handle class, dispatch to all four object kinds exactly once,
    and that probing is read-only.  NOT covered: real concurrency (the window
    between "probe says ready" and "the caller takes it" can only be observed
    on a board), the user-mode SVC polling wrapper's timeout and 1 ms wake
    granularity, and interrupt-off time under the real scheduler. A missing
    host compiler is a FAIL.
    """
    r = _run_check([sys.executable, os.path.join("tools", "waitmulti_host_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "WM-HOST OK" in txt:
        ok("waitmulti-host", "dispatch + bit mask + ANY/ALL verified")
    else:
        fail("waitmulti-host", "gate failed: %s" % txt.strip()[-300:])

def check_waitmulti_syntax():
    """WAITMULTI must compile with the switch ON.

    ARKRTOS_USE_WAITMULTI defaults to 0, so arkrtos_waitmulti.c, the four ready
    queries in sync/mq/event, the SVC 0x22 handler and dispatch entry in
    arkrtos_task.c and the SDK polling wrapper are never seen by a compiler in
    a default build.  Same false-green shape as the eset / rtstat gates.
    armclang -fsyntax-only with the switch forced to 1 over six files; a
    missing armclang is a FAIL, not a skip. Syntax only -- SVC behaviour and
    the wake-up latency still need a board.
    """
    r = _run_check([sys.executable, os.path.join("tools", "waitmulti_syntax_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "WM-GATE OK" in txt:
        ok("waitmulti-syntax", "ON-path compile verified (6 files)")
    else:
        fail("waitmulti-syntax", txt[-260:])

def check_eset_syntax():
    """ESET must compile with the switch ON.

    ARKRTOS_USE_ESET defaults to 0, so arkrtos_eset.c in full, the SVC 0x21
    handler and dispatch entry in arkrtos_task.c, the boot-time init and both
    SDK wrappers are never seen by a compiler in a default build.  Same
    false-green shape as the rtstat gate.  armclang -fsyntax-only with the
    switch forced to 1; a missing armclang is a FAIL, not a skip.  Syntax only
    -- SVC/ISR behaviour still needs a board.
    """
    r = _run_check([sys.executable, os.path.join("tools", "eset_syntax_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "ESET-GATE OK" in txt:
        ok("eset-syntax", "ON-path compile verified (5 files)")
    else:
        fail("eset-syntax", txt[-260:])

def check_waitq():
    """Waiter capacity (ARKRTOS_USE_WAIT_UNBOUNDED) must hold in both modes.

    With the switch OFF every object table uses ARKRTOS_MAX_SYNC_WAITERS; with
    it ON ARKRTOS_WQ_SLOTS grows to the task table size, so no object can ever
    overflow its waiter array (a task waits on at most one object at a time).
    Two parts: armclang -fsyntax-only over the five waiter-owning sources with
    the switch forced to 1 (sync / mq / sbuf / event / waitmulti), and the host
    test suite re-run in --unbounded mode where it asserts the derived sizes
    compile-time and at runtime.  A missing armclang is a FAIL, not a skip.
    Scope: mq / sbuf / event have compile-level coverage only -- their waiter
    arrays are checked for size, not for behaviour under a full queue.
    """
    r = _run_check([sys.executable, os.path.join("tools", "waitq_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "WAITQ-GATE OK" in txt:
        ok("waitq", "ON-path compile + host size assertions verified (5 files)")
    else:
        fail("waitq", txt[-260:])

def check_rtstat_syntax():
    """RTSTAT (DWT cycle-based latency metering) must compile with the switch ON.

    ARKRTOS_USE_RTSTAT defaults to 0, so the five #if blocks that carry the
    measurement body, the tick stamps, the boot-time arming and the `rt` shell
    command are never seen by a compiler in a default build.  A typo there stays
    green until someone arms it on a board and gets a wrong reading instead of a
    build error.  This runs armclang -fsyntax-only with the switch forced to 1
    over the kernel files and all three port layers; a missing armclang is a
    FAIL, not a skip.  It checks syntax only -- DWT behaviour and whether the
    numbers mean anything still need a board.
    """
    r = _run_check([sys.executable, os.path.join("tools", "rtstat_syntax_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "RTSTAT-GATE OK" in txt:
        ok("rtstat", "ON-path compile verified (7 files)")
    else:
        fail("rtstat", txt[-260:])

def check_smp():
    """SMP scaffolding must build with the switch ON and keep its books straight.

    ARKRTOS_USE_SMP defaults to 0, so arkrtos_smp.h/c, the boot call in
    arkrtos_init.c, the two new port hooks in arkrtos_hal.h and the three
    single-core port stubs are never seen by a compiler in a default build.
    Two parts: a host run of test_smp_host.c with programmable port stubs
    (online mask, out-of-range core ids, refused start requests, IPI target
    check, re-init clearing, per-core counters), armclang -fsyntax-only
    with the switch forced to 1 over five files, and the LA64 pin U
    (start.S secondary-core park branch assembled for real + port.c compiled
    with SMP=1 + a self-check that a removed CSR.TID read is caught).  A
    missing compiler is a FAIL, not a skip.  Scope: there is no multi-core
    board here, so this says the scaffolding is self-consistent, not that SMP
    works -- the scheduler is still single-core and secondary cores only spin
    in WFI.  The LA64 park branch is unreachable under QEMU's -kernel direct
    boot (secondary PC is the pflash slave_boot_code), see
    docs/QEMU模拟器移植与验证.md section 9.5.
    """
    r = _run_check([sys.executable, os.path.join("tools", "smp_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "SMP-GATE OK" in txt:
        ok("smp", "host accounting + ARM ON-path compile (5 files) + LA64 park/CSR pin U")
    else:
        fail("smp", txt[-260:])

def check_mps2_m4():
    """MPS2-AN386（Cortex-M4）镜像要真在 QEMU 里跑出读数。

    本档不参与任何 Keil 工程，`--build` 也编不到它，所以它的退化只能是
    「谁都没注意」。这道检查编一次、跑 20 秒、判串口读数（节拍增量、延时
    增量、SVC 与节拍换算、信号量唤醒）四条口径互相印证，外加镜像头
    8 字节的 Cortex-M 复位向量与解析器自检。缺 arm-none-eabi-gcc / QEMU /
    make 一律 FAIL——跳过会把「没验」写成「通过」。

    边界：FPU/MPU/PRIV 在本档显式关闭（板级头里写明理由），所以这道门禁
    不构成任何 MPU 保护机制的验证；它验的是节拍、任务切换、SVC 读数、
    信号量同步四条链路在 QEMU 的 mps2-an386 模型上成立。
    """
    r = _run_check([sys.executable, os.path.join("tools", "qemu_m4_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "QEMU-M4-GATE OK" in txt:
        ok("mps2-m4", "linker pin + image head + QEMU run + parser self-check")
    else:
        fail("mps2-m4", txt[-320:])


def check_seccheck_syntax():
    """SECCHECK (security baseline self-check) must compile with the switch ON.

    ARKRTOS_USE_SECCHECK defaults to 0, so arkrtos_sec.c in full, the three
    prototypes in arkrtos_sec.h and `cmd_sec` plus its command-table line are
    never seen by a compiler in a default build.  Same false-green shape as the
    rtstat gate: a typo there stays green until someone arms it for an
    assessment and gets a build break instead of a reading.  This runs
    armclang -fsyntax-only with the switch forced to 1 over the module and the
    shell wiring (optional components forced on so the whole command table is
    walked); a missing armclang is a FAIL, not a skip.  Syntax only -- whether
    the twelve verdicts are right still needs a board (docs/安全基线自检.md §4).
    """
    r = _run_check([sys.executable, os.path.join("tools", "seccheck_syntax_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "SECCHECK-GATE OK" in txt:
        ok("seccheck", "ON-path compile verified (2 files)")
    else:
        fail("seccheck", txt[-260:])


def check_sign_host():
    """IMAGE_SIGN (HMAC-SHA256) must agree between kernel and packer.

    ARKRTOS_USE_IMAGE_SIGN defaults to 0, so arkrtos_sign.c is not compiled by
    any build and nothing ever runs it.  A mismatch in "which header bytes are
    zeroed / how much is covered" makes a device reject the image its own
    packer signed, and the symptom looks like a key problem.  This compiles the
    real arkrtos_sign.c on the host and compares its MAC against
    tools/pack_app.py::calc_sign() on the same bytes.  No board, no key file:
    a missing host compiler is a FAIL, not a skip.
    """
    r = _run_check([sys.executable, os.path.join("tools", "sign_host_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "SIGN-GATE OK" in txt:
        ok("sign-host", "kernel MAC == packer MAC + key channel, default off (4 TUs on host)")
    else:
        fail("sign-host", txt[-260:])


def check_sign_ed25519():
    """IMAGE_SIGN_ED25519 (Ed25519) must agree between kernel and packer.

    Same failure mode as sign-host, one notch further out: the switch defaults to
    0, so arkrtos_sign.c takes the HMAC backend and arkrtos_ed25519.c is empty,
    and nothing ever runs the verify path.  A mismatch in the covered fields
    makes a device reject an image its own packer signed.  This compiles the
    real arkrtos_sign.c + arkrtos_ed25519.c, checks a golden signature over the
    deterministic buffer, requires tampering to be refused, compares the kernel
    digest against tools/pack_app.py::calc_digest(), and requires the three
    impossible switch combinations to be refused at compile time.
    """
    r = _run_check([sys.executable, os.path.join("tools", "ed25519_image_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "ED25519-GATE OK" in txt:
        ok("sign-ed25519", "kernel digest == packer digest + golden sig + 3 switch guards")
    else:
        fail("sign-ed25519", txt[-260:])

def check_swap_syntax():
    """Seamless upgrade (A/B task swap) must compile with the switch ON.

    ARKRTOS_USE_TASK_SWAP defaults to 0, so the swap primitives in
    arkrtos_task.c, both consumers (arkrtos_mini.c / arkrtos_loader.c), the two
    SVC hooks and the `mini swap` / `app swap` shell branches are never seen by
    a compiler in a default build.  The whole point of that code is that it
    rewrites an exception frame and moves the PSP -- a typo there builds fine
    and misbehaves only on a board, which is the worst possible place to find
    it.  This runs armclang -fsyntax-only with the switch forced to 1 over the
    affected translation units; a missing armclang is a FAIL, not a
    skip.  It also drives three real cross compilers as a two-way contract:
    LA64 and RV32IMAC must now COMPILE with the switch on (each port rewrites
    its own trap frame: mepc / ERA), while LA32 must still be REFUSED at
    compile time (that frame layout is not implemented; enabling it there
    would rewrite the wrong registers, which is the worst kind of silent
    wrong answer).  Syntax only -- whether the hand-off is actually seamless
    is measured on QEMU (docs/无缝升级-双缓冲热切换.md §5/§6).
    """
    r = _run_check([sys.executable, os.path.join("tools", "swap_syntax_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "SWAP-GATE OK" in txt:
        ok("swap", "ON-path compile verified (15 files) + LA64/RV32 compile both ways + LA32 still refused when ON")
    else:
        fail("swap", txt[-260:])

def check_la64_build_warnings():
    """The LoongArch64 target build must compile with zero warnings.

    Why this exists: `warn-clean` only covers the 32-bit host TUs.  The target
    side had no reading at all, so 241 `-Wint-to-pointer-cast` /
    `-Wpointer-to-int-cast` warnings sat in the default LA64 build for months
    (all of `svc_*()` reading the 32-bit SVC parameter block on a 64-bit
    target, plus the loader's 32-bit address model).  One root cause, one
    wording, 241 times - which is exactly the shape that hides the next real
    slip.  The fix (docs/欠账盘点.md「LA64 构建告警清零」) makes every pointer
    <-> integer conversion explicit through `arkrtos_uptr_t`, so the build can
    be held at zero from now on.

    A missing cross compiler is a FAIL, not a skip: this gate exists to be a
    reading, and "no toolchain" is not "clean".
    """
    logdir = os.path.join(ROOT, "build")
    try:
        os.makedirs(logdir)
    except OSError:
        pass
    log = os.path.join(logdir, "ci_gate_la64_build.log")
    env = dict(os.environ)
    env.pop("PYTHONHOME", None)
    env.pop("PYTHONPATH", None)
    try:
        with open(log, "wb") as f:
            r = subprocess.run(["make", "-f", "example/qemu_loongarch64/Makefile",
                                "-j8"], cwd=ROOT, stdout=f,
                               stderr=subprocess.STDOUT, env=env, timeout=900)
    except (OSError, subprocess.TimeoutExpired) as exc:
        fail("la64-build", "make did not run: %s" % exc)
        return
    txt = read(log).decode("utf-8", "replace")
    warns = [ln.strip() for ln in txt.splitlines() if "warning:" in ln]
    if r.returncode != 0:
        fail("la64-build", "build failed (exit=%d), log: %s" % (r.returncode, log))
    elif warns:
        fail("la64-build", "%d warning(s), first: %s (log: %s)"
             % (len(warns), warns[0][:200], log))
    else:
        ok("la64-build", "LA64 full build: 0 warning / 0 error "
           "(host 32-bit TUs stay under warn_gate)")

def check_paging_switch():
    """Isolation backend switch must actually switch (both directions).

    ARKRTOS_USE_PAGING picks between two implementations of one contract:
    off = arkrtos_mpu.c (window backend), on = arkrtos_paging.c (page table).
    Whichever side is off is an empty translation unit, so a default build
    never sees the other half -- the same false-green shape as seccheck / swap.
    Three ways that goes wrong silently: the paging body does not compile, the
    switch is not wired (paging字面上打开、跑的仍是窗口后端), or the three
    #error guards in arkrtos_config.h are decoration and a build with no TLB
    requests paging anyway.  tools/paging_gate.py turns all three into a
    reading with the real LA64 and RV32 cross compilers; a missing compiler is
    a FAIL, not a skip.  Not covered: whether the isolation actually holds on
    a board (that needs QEMU / hardware, see docs/内存保护与App越权验证.md).
    """
    r = _run_check([sys.executable, os.path.join("tools", "paging_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "PAGING-GATE OK" in txt:
        ok("paging", "default off + LA64 ON compiles (2 TUs) + 2 negative cases refused")
    else:
        fail("paging", txt[-260:])

def check_shell_auth():
    """Console login (ARKRTOS_USE_SHELL_AUTH, SEC-G01) must actually gate.

    The switch defaults to 0, so arkrtos_shell_auth.c, the shell gate hook and
    the two login/logout command registrations are never seen by a compiler in
    a default build -- the usual false-green shape.  Two of the failure modes
    are worse than a build break: a gate that sits *after* command lookup
    leaves the exec fallback wide open ("./prog" runs without a password), and
    an "enabled" build with no salt/digest is a console nobody can log into.
    tools/shell_auth_gate.py makes all of that a reading with the real LA64
    cross compiler: switch in three places, zero symbols when off (nm on a real
    object), three #error negatives that must each name their symbol, the
    constant-time 32-byte compare, the hook position, the failure limit and hard
    lock, the two command registrations plus the table budget, and the demo
    credentials recomputed by tools/shell_auth_cred.py.  Every source pin has a
    self-check that a deliberately broken copy is caught.  A missing compiler is
    a FAIL, not a skip.  Not covered: the runtime behaviour (login refused /
    accepted on a real console) -- that is a QEMU reading, see
    docs/QEMU模拟器移植与验证.md section 5.27.
    """
    r = _run_check([sys.executable, os.path.join("tools", "shell_auth_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "SHELL-AUTH-GATE OK" in txt:
        ok("shell-auth",
           "default off (no symbols) + LA64 ON compiles (3 TUs) + 3 negatives refused")
    else:
        fail("shell-auth", txt[-260:])

def check_shellauth_host():
    """Console login gate (arkrtos_shell_auth.c) on host, decisions and count.

    shell_auth already had a compile-side check (shell-auth: switch wired, zero
    symbols when off, constant-time compare in the source).  What was missing
    is the state machine: what the gate does on each of the four states (boot
    locked / locked / unlocked / locked-hard), how many wrong passwords it
    takes to reach the hard lock, and whether the counter really resets.

    Pinned here:
      * boot starts locked; NULL is refused without printing; login / logout /
        help pass while locked and do not count;
      * a wrong password increments the counter and prints (n/TRIES); the last
        one flips to the hard lock;
      * under the hard lock the correct password is still refused and the
        message says a reboot is needed -- the gate's message names the count;
      * the correct password unlocks exactly once, resets the counter, and a
        second login says "already logged in" without touching the count;
      * logout while locked / while hard-locked reports, and a real logout
        locks again and clears the counter.

    The salt/digest are a fixture (salt 0x00..0x0F, password "s3cret!",
    digest computed offline), so a successful unlock means the password really
    hashed to the stored digest, not that the module agreed with itself.

    Not covered: serial-line exposure, offline brute force, real timing
    constancy, and the board console.  One line (auth_streq's NULL guard) is
    unreachable from the public API and stays uncovered by design.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("shellauth-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    link = []
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name == "shellauth":
            extra = case[5] if len(case) > 5 else {}
            pick = (_markers, test, mods, defs, incs)
            link = list(extra.get("link", []))
            break
    if pick is None:
        fail("shellauth-host", "case shellauth missing from coverage_gate.CASES")
        return

    markers, test, mods, defs, incs = pick
    with TempDir() as td:
        exe = os.path.join(td, "shellauthhost" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods + link)
        r = _run_check(cmd)
        if r.returncode != 0:
            tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
            fail("shellauth-host", "did not build: %s" % tail[:200])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt:
            fail("shellauth-host", "failed: %s" % txt.strip()[:220])
            return
        last = txt.strip().splitlines()[-1]

    ok("shellauth-host",
       "boot locked / exempt words / wrong-password count / hard lock / unlock+logout: %s"
       % last)

def check_fdtconsole_host():
    """Consume /chosen/stdout-path on host: decisions and every refusal reason.

    arkrtos_fdt_console.c is wrapped in #if (ARKRTOS_USE_FDT_CONSOLE == 1) and
    the switch defaults to 0, so a default build never sees a line of it.  The
    module's whole job is to *refuse*, and a refusal reason that is wrong is
    worse than no check at all, because it sends the reader to the wrong line
    of the device tree.

    Pinned here, with the five FDT queries stubbed (no real DTB needed):
      * no stdout-path / unparsable value -> NO_STDOUT / BAD_PATH;
      * path does not resolve, node disabled -> NO_NODE / DISABLED, with the
        fields read so far still filled for the log line;
      * OK fills path / compatible / reg / clock / baud / bits / parity;
      * stdout-path options vs node current-speed conflict -> BAUD;
      * expect mismatches are separate codes: COMPAT, ADDR, CLOCK (missing
        clock-frequency counts as a clock mismatch, not "unchecked");
      * a truncated copy still yields a NUL-terminated prefix;
      * reason names are stable strings and unknown codes return "?".

    Two defensive lines stay uncovered by design: console_str_eq's NULL guard
    and console_copy's NULL/cap guard -- both callers pass non-NULL buffers.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("fdtconsole-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    link = []
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name == "fdtconsole":
            extra = case[5] if len(case) > 5 else {}
            pick = (_markers, test, mods, defs, incs)
            link = list(extra.get("link", []))
            break
    if pick is None:
        fail("fdtconsole-host", "case fdtconsole missing from coverage_gate.CASES")
        return

    markers, test, mods, defs, incs = pick
    with TempDir() as td:
        exe = os.path.join(td, "fdtconsolehost" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods + link)
        r = _run_check(cmd)
        if r.returncode != 0:
            tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
            fail("fdtconsole-host", "did not build: %s" % tail[:200])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt:
            fail("fdtconsole-host", "failed: %s" % txt.strip()[:220])
            return
        last = txt.strip().splitlines()[-1]

    ok("fdtconsole-host",
       "stdout-path decisions (8 refusal codes + OK) and truncated-copy safety: %s"
       % last)

def check_fdtres_host():
    """Timer / timebase check on host: every refusal names a tree fact.

    arkrtos_fdt_res.c is wrapped in #if (ARKRTOS_USE_FDT_RES == 1), default 0,
    so a default build never compiles it.  Its output is a frequency the
    kernel's tick depends on, and a wrong-but-plausible answer shows up as
    "time runs fast/slow", which reads like a scheduler bug.  So the point of
    this case is the *reason codes*: "the tree did not write it" and "the tree
    disagrees with the board" must stay distinguishable.

    Pinned here, with the seven FDT queries stubbed (node table built by the
    case, no real DTB):
      * NULL expect / NULL out -> NO_CPUS without touching the tree;
      * not initialised or /cpus missing -> NO_CPUS; missing timebase ->
        NO_TIMEBASE only when one is expected (expect 0 means "do not check");
      * differing timebase -> TIMEBASE_DIFF with the value still in out;
      * timer_compat == 0 stops after the timebase;
      * iterator failure, a walk with only disabled / other-compatible nodes,
        and a malformed-blob error all -> NO_TIMER;
      * missing reg / reg mismatch / missing clock-frequency / clock mismatch
        are four codes, and earlier reads stay in out;
      * expect fields at 0 skip their check; long node names are copied
        truncated with a NUL; reason names are stable and unknown -> "?".

    Not covered: real DTB parsing (arkrtos_fdt.c's own case) and whether the
    timer really ticks at that rate -- that is a board reading.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("fdtres-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    link = []
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name == "fdtres":
            extra = case[5] if len(case) > 5 else {}
            pick = (_markers, test, mods, defs, incs)
            link = list(extra.get("link", []))
            break
    if pick is None:
        fail("fdtres-host", "case fdtres missing from coverage_gate.CASES")
        return

    markers, test, mods, defs, incs = pick
    with TempDir() as td:
        exe = os.path.join(td, "fdtreshost" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods + link)
        r = _run_check(cmd)
        if r.returncode != 0:
            tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
            fail("fdtres-host", "did not build: %s" % tail[:200])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt:
            fail("fdtres-host", "failed: %s" % txt.strip()[:220])
            return
        last = txt.strip().splitlines()[-1]

    ok("fdtres-host",
       "timebase and timer-node decisions (8 refusal codes + OK): %s" % last)

def check_fdt_host():
    """Flattened device tree reader on host: the refusals, not the happy path.

    arkrtos_fdt.c is the base of the whole FDT family (console path, timer
    node, overlay), all wrapped in #if (ARKRTOS_USE_FDT == 1) and off by
    default.  A blob that parses "almost right" is worse than one that is
    refused: the kernel would then read a node name from the wrong offset or
    walk a tree whose END tokens do not balance, and every later symptom
    would look like a driver bug.

    Pinned here, with the case building real DTB blobs byte by byte (header +
    structure block + strings block) and then breaking one field at a time:
      * every header field has its own code -- null/short (ARG), bad magic,
        version below 16 or last_comp above version, totalsize past the
        buffer, misaligned offsets;
      * unbalanced BEGIN/END, END at depth 0, bytes after END, an unknown
        token, a node name without a NUL, a property name offset outside the
        strings block, and a property length past the block are all FORMAT;
      * nesting past ARKRTOS_FDT_MAX_DEPTH is DEPTH, and init is
        transactional -- a bad blob leaves the previous tree usable;
      * the read side: depth-first order, depth counted from 1, meta nodes
        (__symbols__ / __fixups__ / __local_fixups__) excluded from both the
        count and the walk *including their subtrees* (the two paths used to
        disagree; that is what this case found), property / string / u32
        cells / enabled status, absolute paths, phandle and linux,phandle,
        aliases in string and phandle form, and stdout-path in all three
        shapes plus every option spelling that must be refused.

    Not covered: that a board's real dtb parses (tools/fdt_gate.py) and that
    any driver actually uses the node it found -- separate gates.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("fdt-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    link = []
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name == "fdt":
            extra = case[5] if len(case) > 5 else {}
            pick = (_markers, test, mods, defs, incs)
            link = list(extra.get("link", []))
            break
    if pick is None:
        fail("fdt-host", "case fdt missing from coverage_gate.CASES")
        return

    markers, test, mods, defs, incs = pick
    with TempDir() as td:
        exe = os.path.join(td, "fdthost" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods + link)
        r = _run_check(cmd)
        if r.returncode != 0:
            tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
            fail("fdt-host", "did not build: %s" % tail[:200])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt:
            fail("fdt-host", "failed: %s" % txt.strip()[:220])
            return
        last = txt.strip().splitlines()[-1]

    ok("fdt-host",
       "device tree refusals and lookups (8 codes + meta/path/phandle): %s"
       % last)

def check_fdtovl_host():
    """dtb overlay merge (arkrtos_fdt_overlay) on host: six refusals plus a
    result that the real parser can read back.

    The merge produces a new dtb from a base blob and an overlay blob.  What
    must hold is not "the bytes look like dtc output" but "the result is a
    legal dtb whose tree says the right thing" -- so the case runs every
    accepted merge through arkrtos_fdt_init and queries it by path, and only
    pins header/reserve-map layout byte by byte.

    Pinned here:
      * target-path and target=<0xffffffff> resolved through the fragment's
        __fixups__ and the base tree's __symbols__; a resolved phandle, a
        relative path, a missing node, "//" and a trailing '/' are all
        TARGET (-12) -- the splitter does not normalise;
      * property override, new property, new child subtree, later fragment
        winning, delete property (length 0) and delete child (__delete),
        with DEL (-16) when the target is absent;
      * OVERLAY (-11) for shape errors, DUP (-13) for duplicate children,
        SIZE (-14) for an arena too small, TAG (-15) for __symbols__ /
        __local_fixups__ on the fragment side, DEPTH (-7) when the
        base tree is past ARKRTOS_FDT_MAX_DEPTH;
      * the reserve map copied verbatim, and argument guards (NULL dst,
        NULL out_len, misaligned arena, cap below the documented minimum).

    Not covered: that a real dtbo from dtc merges (our delete encoding is
    deliberately not Linux's) and that the merged tree boots -- those are
    board readings.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("fdtovl-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    link = []
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name == "fdtovl":
            extra = case[5] if len(case) > 5 else {}
            pick = (_markers, test, mods, defs, incs)
            link = list(extra.get("link", []))
            break
    if pick is None:
        fail("fdtovl-host", "case fdtovl missing from coverage_gate.CASES")
        return

    markers, test, mods, defs, incs = pick
    with TempDir() as td:
        exe = os.path.join(td, "fdtovlhost" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods + link)
        r = _run_check(cmd)
        if r.returncode != 0:
            tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
            fail("fdtovl-host", "did not build: %s" % tail[:200])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt:
            fail("fdtovl-host", "failed: %s" % txt.strip()[:220])
            return
        last = txt.strip().splitlines()[-1]

    ok("fdtovl-host",
       "dtb overlay merge read back through the parser (6 codes + target/depth): %s"
       % last)

def check_vfslfs_host():
    """littlefs -> ark_vfs bridge (arkrtos_vfs_lfs) on the host, against a RAM
    fake, with all 15 arkrtos_fs_* facade entries stubbed.

    The bridge itself is what is under test -- not littlefs.  Three promises
    are pinned:

      * reads go through arkrtos_fs_read_at (random access, several read fds
        at once), writes go through the single streaming handle;
      * one write handle at a time: a second open for write is BUSY (-15),
        and while a write stream is live read() and truncate() are BUSY too,
        because they share the one file cache;
      * what cannot be done is refused instead of approximated: seek on a
        write handle and truncate(non-zero) on it are NOSYS (-12), chmod and
        chown are NULL callbacks (the core turns them into NOSYS).

    Also pinned: the mount root cannot be open()ed (ISDIR), read-only
    mounts refuse write intent (ROFS), a missing parent on create is NOENT
    rather than a blanket IO, every failure path that is not "no such
    entry" reports IO while keeping the raw arkrtos_fs error for the log,
    stat reports 0755/0644 with mtime/uid/gid 0 (littlefs keeps none),
    readdir skips "." and ".." and does not advance on NOSPC/NAMETOOLONG,
    and an over-32-char entry name is NAMETOOLONG (-13) instead of being
    truncated.

    Not covered: real littlefs behaviour (that is the arkrtos_fs cases) and
    the core path walk/mount table (that is the ark_vfs cases).  This case
    calls the driver table directly, so a broken contract between core and
    driver would not be caught here.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("vfslfs-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name == "vfslfs":
            pick = (_markers, test, mods, defs, incs)
            break
    if pick is None:
        fail("vfslfs-host", "case vfslfs missing from coverage_gate.CASES")
        return

    markers, test, mods, defs, incs = pick
    with TempDir() as td:
        exe = os.path.join(td, "vfslfshost" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
        r = _run_check(cmd)
        if r.returncode != 0:
            tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
            fail("vfslfs-host", "did not build: %s" % tail[:200])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt:
            fail("vfslfs-host", "failed: %s" % txt.strip()[:220])
            return
        last = txt.strip().splitlines()[-1]

    ok("vfslfs-host",
       "littlefs->ark_vfs bridge on a RAM fake (single writer, BUSY, NOSYS): %s"
       % last)

def check_vfs_host():
    """arkrtos_vfs facade (arkrtos_vfs.c) on the host, linked against the real
    ark_vfs core and the real ramfs; only the board-side port layer is stubbed
    (block window, arkrtos_fs, littlefs bridge, log, task status).

    So the namespace behaviour under test is the real one: /dev is mounted by
    the port before init, / after it, longest-prefix lookup, the mount table,
    and the core fd table all come from production code.

    Pinned here:

      * init is idempotent, and a failing step leaves the facade NOT ready --
        no "half ready" state that later reports a zero-length mount table;
      * the standard directories are created, and directory-slot exhaustion
        warns instead of pretending /mnt and /media exist;
      * mount_volume validates parameters before capability (INVAL for a bad
        call, NOSYS only when the build really has no littlefs bridge), refuses
        a second volume, a geometry mismatch and a broken block window, and
        always leaves "volume mounted but mount point missing" cleaned up;
      * the app handle layer is a capability, not a number: handle = tag | seq
        | slot, a recycled slot never matches an old handle, another task gets
        BADF, a dead task's slots are reclaimed only when a new handle is
        needed, and the position/size mirror is the kernel's own accounting
        (the two filesystems disagree on what lseek returns);
      * O_APPEND lands at the end on every write, a short buffer is NOSPC
        rather than a silently truncated file, and a dirent that does not fit
        the caller's buffer is kept for the next call instead of vanishing.

    Not covered here: real littlefs, a real block device, real task switching
    under the handle table, and the two p_copy() overflow exits (a normalized
    path is always shorter than the destination buffer, so they are
    structurally unreachable).

    Runs twice worth of code: the default tier plus -DARKRTOS_USE_VFS_LFS=0
    for the NOSYS branch; coverage_gate.CASES carries both.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("vfs-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    link = []
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name == "vfs":
            extra = case[5] if len(case) > 5 else {}
            pick = (_markers, test, mods, defs, incs)
            link = list(extra.get("link", []))
            break
    if pick is None:
        fail("vfs-host", "case vfs missing from coverage_gate.CASES")
        return

    markers, test, mods, defs, incs = pick
    with TempDir() as td:
        exe = os.path.join(td, "vfshost" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods + link)
        r = _run_check(cmd)
        if r.returncode != 0:
            tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
            fail("vfs-host", "did not build: %s" % tail[:200])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt:
            fail("vfs-host", "failed: %s" % txt.strip()[:220])
            return
        last = txt.strip().splitlines()[-1]

    ok("vfs-host",
       "arkrtos_vfs facade on the real ark_vfs core + ramfs (mount table, "
       "handle capability, O_APPEND, NOSPC): %s" % last)

def check_fs_host():
    """arkrtos_fs facade (arkrtos_fs.c) on the host, linked against the real
    littlefs in kernelsrc/fs -- not a stub.  The disk underneath is a 512 KB
    RAM NOR written by the test harness: a write may only clear bits (1 -> 0)
    and an erase refills with 0xFF, which is the property that makes "does the
    facade really make littlefs erase before programming?" a real reading.

    Pinned here:

      * every entry point reports a failing call the same way (FAIL plus a
        last_error distinct from littlefs's own codes: -1001 bad args/geometry,
        -1002 no such block device, -1004 already mounted, -1005 nothing
        mounted, -1006 the device refused it, -1007 short write, -1008 busy);
      * mount validates geometry before touching the medium (unknown device,
        unaligned offset, volume smaller than one erase unit, volume past the
        end of the device, erase_unit = 0, fewer than three blocks), and a
        blank medium comes back as CORRUPT with the "unformatted" predicate
        true -- mount never formats silently;
      * format(NULL) and format("nope") fail before a single erase happens;
      * one stream at a time: the single file cache (256 B) is the reason, so a
        second open or a truncate while a stream is up is BUSY, and unmount
        drops an open stream instead of stranding the caller;
      * whole-file and streaming writes that do not fit return SHORT (-1007)
        rather than a silently truncated file, while a failed close reports the
        device's own code (the two are different observables and the test keeps
        them apart);
      * a device read that breaks *after* a stream is open surfaces as the
        device error (-5), never as a zero-length "end of file";
      * truncate extends with zeros and a size that cannot be allocated is
        LFS_ERR_NOSPC;
      * "." and ".." are handed to the caller by littlefs and the facade passes
        them through, so a directory's entry count is real entries + 2;
      * the compiled-out tier (-DARKRTOS_USE_FS=0) answers every entry point
        with FAIL and "nothing is mounted", so "no file system on this board"
        and "no volume mounted" cannot be told apart -- that is deliberate.

    Not covered here: the three block-window bounds guards in the adapter
    (littlefs never asks for a block outside the volume it was given, so they
    are structurally unreachable), the lfs_unmount / lfs_fs_size failure exits
    (would need a medium that breaks between a successful mount and the very
    next call), and the read/close failure exits inside read_file / read_at /
    list / list_names (the calls are atomic, there is no seam to inject into).

    Runs the default tier and the -DARKRTOS_USE_FS=0 tier; coverage_gate.CASES
    carries both and takes the union of the two line sets.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("fs-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    link = []
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name == "fs":
            extra = case[5] if len(case) > 5 else {}
            pick = (_markers, test, mods, defs, incs)
            link = list(extra.get("link", []))
            break
    if pick is None:
        fail("fs-host", "case fs missing from coverage_gate.CASES")
        return

    markers, test, mods, defs, incs = pick
    with TempDir() as td:
        exe = os.path.join(td, "fshost" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods + link)
        r = _run_check(cmd)
        if r.returncode != 0:
            tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
            fail("fs-host", "did not build: %s" % tail[:200])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt:
            fail("fs-host", "failed: %s" % txt.strip()[:220])
            return
        last = txt.strip().splitlines()[-1]

    ok("fs-host",
       "arkrtos_fs facade on the real littlefs over a RAM NOR (geometry, "
       "format/mount, files, one-stream rule, SHORT/NOSPC, 1->0 only): %s"
       % last)

def check_elf_loader():
    """ELF64 executable loader (ARKRTOS_USE_ELF) must be wired, not decorative.

    Same false-green shape as paging / seccheck / swap: the whole of
    arkrtos_elf.c is wrapped in #if (ARKRTOS_USE_ELF == 1), so the default
    build never compiles it.  tools/elf_gate.py compiles it for real both ways
    (LA64 with the feature on, RV32 where the 64-bit guard must refuse), plus
    a board probe with the window macros removed and a pin on the two wire
    constants (EM_LOONGARCH=258, ET_EXEC=2) that decide what counts as an
    executable at all.  A missing compiler is a FAIL, not a skip.
    Not covered: runtime rejection of wrong machine / ET_DYN / out-of-window
    segments -- those are QEMU readings, see docs/MMU进程模型与ELF装载.md §5.
    """
    r = _run_check([sys.executable, os.path.join("tools", "elf_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "ELF-GATE OK" in txt:
        ok("elf", "default off + LA64 ON compiles (2 TUs) + 2 negative cases refused"
                 + " + TLS: LA64 compiles, RV32 refused by #error")
    else:
        fail("elf", txt[-260:])

def check_payload_sign():
    """exec/ld 的尾签名门（ARKRTOS_USE_EXEC_SIGN）必须两头对得上。

    和 sign-host 同型：这条链的两头是两套独立实现（tools/sign_payload.py
    用 Python hashlib，内核用手写 SHA-256），而两边都默认不参与构建。
    tools/payload_sign_gate.py 把真内核 C 代码编到宿主上算 MAC，与 Python
    逐字节比对；另外核三件事：sign 幂等、tamper 必拒、关档时两个装载器
    的预处理结果里一个 arkrtos_sign_payload 都不出现（默认零变化）。
    """
    r = _run_check([sys.executable, os.path.join("tools", "payload_sign_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "PAYLOAD-SIGN-GATE OK" in txt:
        ok("payload-sign",
           "kernel MAC == python MAC (once+stream) / sign idempotent / tamper refused / "
           "ON wired, OFF zero symbols / dependency guard / -8,-11 pinned")
    else:
        fail("payload-sign", txt[-260:])


def check_stack_guard():
    """栈底守卫（ARKRTOS_USE_STACK_GUARD）必须两头都在。

    U-13 的原状是「标志字写了但从来没人读」，这种代码不会报错、不会变慢，
    只会让人以为有保护。tools/stack_guard_gate.py 把两个方向都变成读数：
    开档时切换路径与两个 port 的 trap 入口都调检查、命中记 STACKOVF 并摘任务；
    关档时预处理结果里 stack_guard 一个字都不出现。
    """
    r = _run_check([sys.executable, os.path.join("tools", "stack_guard_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "STACK-GUARD-GATE OK" in txt:
        ok("stack-guard",
           "default off / LA64 ON has check+STACKOVF+task id / OFF zero symbols / "
           "RV32 port ON compiles / removal wired / board + demo wired")
    else:
        fail("stack-guard", txt[-260:])


def check_icount_stack():
    """lwIP tcpip 线程的栈预算（LA64 十档低频 INE 停机）必须两头都在。

    2026-11-23 起十档镜像会低频停在零页（`ecode=0x0d` INE）。根因坐实到
    机制级：`sys_arch.c` 里 `LWIP_THREAD_STACK_W` 只在 `NET_VIRTIO` 时给
    8 KB，而 socket 档的 tcpip 线程也要跑 netconn/select，2 KB 槽被 100%
    用满，528 字节陷阱帧溢出到槽下方，改坏相邻任务保存的 ra。
    检测当时为何失灵：守卫字确实报了 task 12，但报的是「已发生」——
    它踩坏的相邻帧不会因为把 task 12 摘掉而复原。

    tools/icount_stack_gate.py 因此判两层：源码层栈预算不许回退（8 KB 的
    那条 `#if` 必须同时含 NET_VIRTIO 与 LINUX_SOCKET），运行层必须真的跑
    过那段窗口（`-icount shift=5` 纯跑、不许任何单步——gdb 单步会扰动
    icount 相位，历史实测因此复现不出），且只允许演示任务被报栈底守卫。
    本项会 clean 重编两次（十档 + 复位默认档），是慢检查。
    """
    r = _run_check([sys.executable, os.path.join("tools", "icount_stack_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "ICOUNT-STACK-GATE OK" in txt:
        # 自检样例数从门禁自己的输出里取，别在这里写死一个会过期的数字。
        m = re.search(r"ok\s+parser-self-check\s+(\d+)\s", txt)
        n = m.group(1) if m else "?"
        ok("icount-stack",
           "source pins (8 KB iff NET_VIRTIO or LINUX_SOCKET, lwip task 4 KB) / "
           "parser self-check %s/%s / ten-tier build + -icount shift=5 pure run: no "
           "FATAL, [fork] PASS, keeps running after fork / tcpip stack headroom / "
           "default build restored" % (n, n))
    else:
        fail("icount-stack", txt[-360:])


def check_svc_wide():
    """宽参 ABI（ARKRTOS_USE_WIDE_SVC_ARGS）的拒绝路径与清空顺序都必须有读数。

    宽内核 + 窄 App 时，旧实现只回 -1，App 在用户态继续跑、每条 SVC 都读错
    参数，串口上看起来就是"卡住"——与"内核没收到调用"不可区分。现在拒绝路径
    必须终止调用任务，这样 exec/wait 才能观察到「干净失败」。
    tools/svc_wide_gate.py 钉住四件事：守卫真的调了 reject、reject 真的杀了任务、
    15 个取参数块的 svc_* 入口都过守卫、Linux 兼容层状态在新任务起跑前清空；
    并且带 6 条变异自检（破坏任一条断言必须报红）。

    不覆盖：宽档运行期读数本身（QEMU 三档取证见 docs/SVC服务号与子命令表.md）。
    """
    r = _run_check([sys.executable, os.path.join("tools", "svc_wide_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "SVC-WIDE-GATE OK" in txt:
        ok("svc-wide",
           "reject kills the caller / 15 guard sites / linux abi state reset "
           "before elf_start_task / 6 mutation self-checks")
    else:
        fail("svc-wide", txt[-260:])


def check_svc_args_mini():
    """小程序 SDK 的参数块字宽不能靠猜：窄档 / 宽档 / 空档 / 拒绝四条都要有读数。

    kernelsrc/sdk/mini_sdk/arkc/arkrtos_mini_rt_arkc.c 拿不到 arkrtos_svc_abi.h
    （arkc 只吃部署清单里的文件，宿主门禁也不带 -I），所以字宽规则在那边落了
    一份镜像。镜像最容易出的错是「悄悄按 4 字节走」——块长了一倍却按 4 字节解，
    板子上只表现为控制台没输出，与「内核没收到调用」不可区分。
    tools/mini_svc_args_gate.py 钉四条：默认 12 字节块无标记；32 位目标上开档
    与不开档输出逐字相同（开关是空档）；喂进 8 字节指针宽度时块 24 字节且
    p[0] 带 0x80000000；工具链不报指针宽度时必须编不过而不是默默窄化。另有
    一条源码断言钉 _exit() 的标记（它最后是死循环，宿主上调不回来）。
    """
    r = _run_check([sys.executable, os.path.join("tools", "mini_svc_args_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "MINI-SVC-ARGS-GATE OK" in txt:
        ok("svc-args-mini",
           "narrow 12B / 32-bit switch-on is a no-op / wide 24B + bit31 / "
           "unknown pointer width refused / _exit marker pinned")
    else:
        fail("svc-args-mini", txt[-320:])


def check_linux_abi():
    """Linux 系统调用 ABI（ARKRTOS_USE_LINUX_ABI）必须两头都是读数。

    这一档最容易出的假绿是「默认关 = 没人编译」：开关关着时
    arkrtos_linux_abi.c 整文件、port 里的号判定、内核钩子一个编译器都不经过，
    写错了也全绿。tools/linux_abi_gate.py 双向编（关档 .o 符号表必须全空 /
    开档五个 TU 必须编过）、逐条对 15 个 asm-generic 号、钉两个 ABI 的分界，
    并在 QEMU 上真跑一份纯 Linux ABI 的静态程序（14 条 check + PASS + exit 7），
    而且解析器要能把自己喂进去的四种坏日志判红。

    不覆盖：动态链接（PT_INTERP）、信号、socket、futex——这些是**不支持**，
    不是欠验；fork/clone + wait4 + 真 mmap 语义（MAP_FIXED）见 check_fork，
    出处 docs/Linux可执行文件兼容-系统调用ABI.md。
    """
    r = _run_check([sys.executable, os.path.join("tools", "linux_abi_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "LINUX-ABI-GATE OK" in txt:
        ok("linux-abi",
           "default off zero symbols / LA64 ON 5 TU compile / 15 syscall numbers "
           "match asm-generic / LA64 instr!=0 is Ark, RV32 0x10..0x23 is Ark / "
           "QEMU 14 checks + exit 7 / parser self-check 4/4")
    else:
        fail("linux-abi", txt[-320:])


def check_bus_line():
    """软总线物理线两节点：两个 QEMU 实例经第二路 UART 对连，必须真出读数。

    这一档证的不是「协议对不对」（那是 softbus-host 的事），而是**物理线那一段**：
    字节真的从一侧 UART 寄存器出去、经一根线进到另一侧、被解析成消息并回答一次
    RPC。判据三条同时成立才算过：

      * probe 档：对端 socket 真的收到 ARK-BUS-LINE（线通）；
      * two 档：B 侧收到 A 的心跳（`rx topic=line/hb len=5`），A 侧的 RPC 回包
        逐字节 +1 校验 `echo=ok`（内容对，不是"有字节回来"就算）；
      * solo 档：没有对端时必须 `result=fail (no reply)` —— 没有这条，一个永远
        打印成功的假绿会一直绿下去。

    `tools/bus_line_two_nodes.py` 自己带变异自检（好/坏/半截日志先喂判据），
    所以「判据能区分好坏」不是声称。

    不覆盖：真板（缺一路空闲 UART、波特率编程与硬件流控）、中断驱动收发、
    硬件流控与线上误码。见 docs/QEMU模拟器移植与验证.md §9.14。
    """
    r = _run_check([sys.executable, os.path.join("tools", "bus_line_two_nodes.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "RESULT: PASS" in txt:
        ok("bus-line",
           "two QEMU instances over the 2nd UART: probe recv / peer rx + RPC echo=ok / "
           "no-peer must fail / checker self-check")
    else:
        fail("bus-line", txt[-320:])

def check_dynlink():
    """Linux 动态链接（PT_INTERP + 自研 ld.so）必须是开档有读数、关档零符号。

    这一档比 Linux ABI 更难测：静态语法检查看不出 ld.so 有没有真的做重定位，
    而"跑通了"也可能是夹具碰巧对——比如 JUMP_SLOT 指向一个本来就存在的地址。
    所以 tools/dynlink_gate.py 除了两条正常路径（ET_DYN / ET_EXEC 主程序，
    都要求 [dyn] PASS + exit 105），还必须有一个故意坏掉的夹具：JUMP_SLOT
    指向库里没有的 no_such_symbol，要求 ld.so 打
    `[ldso] FAIL: unresolved PLT symbol no_such_symbol` 并以 127 退出，且
    不许出现 PASS。再加上"关档不编 blob / 开档编"的双向符号检查、
    AT_PHDR = base + 64 的 PT_LOAD 覆盖检查、夹具重建的逐字节确定性、
    以及解析器自检（四种坏日志必须判红）。

    不覆盖：真实发行版二进制（PT_INTERP 指向 glibc 的 ld.so，仓里没有；
    它还要 futex/rseq/TLS 等一大片 syscall）。见 docs/动态链接与PT_INTERP.md。
    """
    r = _run_check([sys.executable, os.path.join("tools", "dynlink_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "DYNLINK-GATE OK" in txt:
        ok("dynlink",
           "default off + no fixture blobs / LA64 ON 5 TU compile / fixture shape "
           "(PT_INTERP, AT_PHDR=base+64, DT_NEEDED, bad symbol, rebuild byte-equal) / "
           "QEMU 2x PASS + exit 105 and bad fixture exit 127 / parser self-check 4/4")
    else:
        fail("dynlink", txt[-320:])

def check_realbin():
    """真实发行版 LA64 二进制「差在哪」必须有可复算的清单。

    这一档交付的不是「能跑」，是**仪器**：tools/elf/realbin_probe.py 纯 struct
    解析真实 ELF（不 shell 出去调 readelf），按依赖顺序列出 blockers 并按退出码
    表达结果（0 = 无 blocker，1 = 有 blocker，1 是正常结果不是失败）。
    最容易出的假绿是「探针把什么都判绿」或「跟 binutils 的名字表各说各话」，
    所以 tools/realbin_gate.py 必须同时有：
      A. 正例：窗口内的静态夹具 0 blocker（退出码 0）；
      B. 负例：远基址 0x120000000 恰好 1 条 address blocker（退出码 1）；
      C. 动态夹具：interp 指向仓内自研 ld.so 时不误判成 interp-path；
      D. 探针自身的坏输入（非 ELF / ELF32 / e_phentsize=40 / e_phoff 越界）
         必须判红并说原因——这条在实现时真抓出了探针的崩溃缺陷；
      E. 能力表与 ldso.c 的 #define / case 双向对齐（防「文档说支持、实现没做」）；
      F. 重定位名字与条数和 binutils readelf 逐项一致（缺工具则 SKIP 并说原因）。

    不覆盖：真实二进制能不能真跑起来（现在一定跑不起来，地址窗口 2 MB 与
    glibc 1.5 MB 就是硬墙），也不覆盖 syscall 分布——见
    docs/真实Linux二进制-需求清单与差距.md。
    """
    r = _run_check([sys.executable, os.path.join("tools", "realbin_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "REALBIN-GATE OK" in txt:
        ok("realbin",
           "probe exit-code semantics (0 clean / 1 with blockers) / static in-window "
           "0 blocker vs far-base exactly 1 address blocker / dyn fixture interp ok / "
           "4 bad inputs judged red with reasons / DT+reloc tables match ldso.c both "
           "ways / reloc names match readelf (SKIP if toolchain absent)")
    else:
        fail("realbin", txt[-320:])

def check_fork():
    """Linux fork/clone + wait4（ARKRTOS_USE_LINUX_FORK）必须两头都是读数。

    这一档的假绿有四种形态，每一种都编得过、跑得出一份「看起来对」的日志：
开关没接线（默认档悄悄多出代码）、只改了 trap 帧没复制窗口（父子共享物理页，
那不是 fork 是共享内存）、退出码是编出来的（对着没上报的任务回 0 或 SIGKILL）、
记账不消耗（同一个号能反复 reap）。tools/fork_gate.py 因此把判据全落在可证伪
的读数上：

      A. 默认档：autoconf 里 ARKRTOS_USE_LINUX_FORK 必须是 0，features.h 四道
         #error，Kconfig 的两条 depends on 都还在；
      B. 关档零符号：RV32 编 linux_abi.o 不许有 lx_clone/lx_wait4/s_child，
         LA64 真板级编 elf/port/paging 不许有 fork 相关符号；
      C. 开档编得过：LA64 + 两个 flag，四个内核 TU 与夹具全过；
      D. QEMU 真读数：[fork] 二十一条 check 全 ok、父子 pid 不同、PASS 与
         exit code 0 都在，且同一次启动里 hello/FS 等既有夹具读数仍在；
         三条新读数逐条校验：阻塞 wait4 真的等过（>= 40 ms）、父的管道读端
         没被子进程 close 掉（fd 表每进程一份）、子退出后父的 SIGCHLD
         handler 在下一次系统调用返回前被调用；
      E. 解析器自检：九种坏日志（去 PASS、改退出码、插 FAIL、少一条 check、
         父子 pid 相同、去 exec rc=0、阻塞读数坏、读端坏、SIGCHLD 坏）必须判红。

    覆盖：fork 返回值分流 / 地址空间复制 / 退出码经 wait4 回收 / 记账消耗 /
参数拒绝 / **阻塞 wait4** / **每进程 fd 表** / **共享管道对象的引用计数** /
**跨任务 SIGCHLD**。
不覆盖：vfork 与 CLONE_VM（无共享地址空间，一律 -EINVAL）、CLONE_FILES、
写时复制（本版本是装载期整块克隆，不是 COW）。
    """
    r = _run_check([sys.executable, os.path.join("tools", "fork_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "FORK-GATE OK" in txt:
        ok("fork",
           "default off zero symbols (RV32 abi + LA64 elf/port/paging) / LA64 ON "
           "4 TU compile / QEMU 27 fork checks (blocking wait4 + per-proc fd table "
           "+ SIGCHLD + MAP_FIXED) + child pid != parent + exit code 0 with hello/FS "
           "readings intact / parser self-check 9/9")
    else:
        fail("fork", "%s（完整输出：%s）" % (txt[-320:], _dump_log("fork_gate", txt)))

def check_bus_crypto():
    """软总线侧信道加密（SEC-G06）：三档矩阵 + 默认档零成本。

    串口与网络是这条总线上别人也能读的一段。组件本来就有认证（HMAC 标签）、
    加密（ChaCha20）、重放窗口（16 位滑动窗口）三档能力，难点不在密码学，
    而在"接对了没有"：两层的宏（ARK_BUS_*_ENABLED 与 ARKRTOS_USE_BUS_*）只要
    有一个对不上，struct 里密钥字段就会错位，链路时好时坏且报错落在别处。
    tools/bus_crypto_gate.py 因此把判据全落在可证伪的读数上：

      * 默认档：autoconf / defconfig / Kconfig 三项都是关，ENCRYPT 与 REPLAY
        挂在 CRYPTO 之下（组件里没有"加密开、认证关"这种档位）；
      * 一致性闸门两个方向都测：只给 -DARK_BUS_CRYPTO_ENABLED=1 而 Kconfig
        是 0 时必须编不过，两边都给 1 时必须编得过；
      * 关档零成本：默认档的 .o 里没有 crypto / replay / chacha / sha256 符号，
        也没有 `bus key` 这段子命令文案；开档编时必须引用到
        ark_bus_crypto_set_key（只测一向的话，"写成 1 但代码没包住"与"根本没
        编"长得一样）；
      * 构建侧接线：三个从 autoconf 取档位的 Makefile、F427 的 GCC Makefile
        与 Keil 工程、以及 f427_env.py 的默认 Tier；
      * 宿主矩阵 6 格：组件级 test_bus_crypto_host.c（RFC 4231 HMAC /
        RFC 8439 ChaCha20 / SHA-256 向量 / 无钥明文不变 / 只装一端 E_AUTH /
        错钥拒绝后换钥通过 / 重放拒绝）与内核级 test_softbus_host.c，各跑
        full / auth-only / off 三档，成功行必须自带档位标签；
      * 判据自检：5 种坏输出（少 OK / 档位标签错 / 计数错 / off 档却有计数 /
        有 FAILED）必须全被判红。

    不覆盖：物理串口上的两个节点、真节拍下的密钥轮换、密钥从哪来（部署问题）。
    """
    r = _run_check([sys.executable, os.path.join("tools", "bus_crypto_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "BUS-CRYPTO-GATE OK" in txt:
        ok("bus-crypto",
           "default off + Kconfig depends + #error both ways / OFF object has no "
           "crypto symbols or `bus key` / 6 host cells (full 95, auth 71, kernel "
           "127/127/84) / parser self-check 5/5")
    else:
        fail("bus-crypto", txt[-320:])


def check_sec_crc():
    """镜像 CRC 开机复核（SEC-G17 / ARKRTOS_USE_IMAGE_CRC_RECHECK）必须两档都对。

    和 rtstat / seccheck 一样是"默认关 = 没人编译"的假绿形态：开关关着时
    arkrtos_loader_crc_recheck() 与 sec 里调它的那一支一个编译器都不经过，
    被改成永远返回 PASS 也照样全绿。tools/sec_crc_gate.py 用真 armclang 编
    两个 TU 的两档，并钉八条只靠编译发现不了的约定（开关默认 n / 实现在
    #if 内 / load_buffer 把重定位表折进 crc_written / 全 unknown 走 SKIP /
    默认档 SKIP 文案逐字未变 / autoconf 默认 0），每条都带变异自检。

    不覆盖：真实的安装与复位后复核，那是 QEMU 的
    `utest run loader_crc_recheck_pool_e2e` 读数；真 Flash 的电离位翻转也
    不在本门禁能力范围内（见 docs/安全认证准备.md 的 SEC-G17 一节）。
    """
    r = _run_check([sys.executable, os.path.join("tools", "sec_crc_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "SEC-CRC-GATE OK" in txt:
        ok("sec-crc",
           "two builds compile (on/off) / 8 pins with mutation self-check "
           "(default off, table folded into crc_written, all-unknown is SKIP, "
           "legacy SKIP text unchanged)")
    else:
        fail("sec-crc", txt[-260:])

def check_smp_sched_host():
    """arkrtos_smp_sched.c on the host: the per-core run queue and the
    explicit migration path, with no board involved.

    What is pinned here:

      * task_to answers -1 (bad task id / core id), -2 (cpu 0 goes through
        task_back), -3 (not READY), -4 (already owned by another core) before
        it touches any queue; a successful move removes the task from the
        CPU0 ready chain and disarms its delay entry;
      * the four switch stages: a handoff of the *running* task saves its
        frame and frees the core, a handoff of a task still sitting in the
        queue just removes it, and a handoff of neither is left registered
        for the next tick;
      * the four landing outcomes are told apart by readings -- stay in place
        (idle to idle with no switch counter bump, slice not exhausted, or
        rotation that lands on the same task again), switch to another task,
        fall back to this core's idle frame, and the "stack pointer is 0"
        guard that returns the incoming frame instead of 0;
      * poll only re-adds a returned task whose status is READY, and clears
        the pending return flag either way;
      * enter() never returns (it is a WFI loop): the test harness makes
        arkrtos_port_wfi longjmp after one loop iteration, so both the
        illegal-core spin and the legal-core "start this core's timer, then
        wait for the first tick" path are observable rather than skipped;
      * the compiled-out tier (-DARKRTOS_USE_SMP_SCHED=0) must leave an empty
        translation unit: nm on the object shows no defined global, otherwise
        a stub would keep answering PASS-shaped questions.

    Not covered: sms_q_push returning -1 (the queue holds at most one entry
    per task and capacity equals ARKRTOS_TASK_MAX_NUM, so it cannot fill),
    the defensive skip of a bad queue index in sms_pick, the re-add on a
    failed push, and the bad-handoff-index clear -- none of them are reachable
    through the public API, so no fake call is manufactured to touch them.
    Real IPI delivery, the secondary core's exception frame, and cross-core
    memory ordering are board-only facts.

    Runs the default tier and the -DARKRTOS_USE_SMP_SCHED=0 tier;
    coverage_gate.CASES carries both and takes the union of the line sets.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("smp-sched-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    picks = {}
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name in ("smpsched", "smpschedbal", "smpschedoff"):
            picks[name] = (test, mods, defs, incs, _markers)
    for name in ("smpsched", "smpschedbal", "smpschedoff"):
        if name not in picks:
            fail("smp-sched-host", "case %s missing from coverage_gate.CASES" % name)
            return

    with TempDir() as td:
        lines = []
        for name in ("smpsched", "smpschedbal", "smpschedoff"):
            test, mods, defs, incs, markers = picks[name]
            exe = os.path.join(td, name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
                fail("smp-sched-host", "%s did not build: %s" % (name, tail[:200]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 or markers[0] not in txt:
                fail("smp-sched-host", "%s failed: %s" % (name, txt.strip()[:220]))
                return
            lines.append(txt.strip().splitlines()[-1])

        # OFF means an empty translation unit: nm on the object must show no
        # defined global. A surviving stub would still answer questions.
        off_defs = picks["smpschedoff"][2]
        obj = os.path.join(td, "smpsched_off.o")
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"] + off_defs
               + COMMON_INC + ["-c", "-o", obj, "kernelsrc/src/arkrtos_smp_sched.c"])
        r = _run_check(cmd)
        if r.returncode != 0:
            fail("smp-sched-host", "OFF object did not build: %s"
                 % ((r.stderr or "").strip().splitlines() or [""])[-1][:200])
            return
        nm = os.path.join(os.path.dirname(cc), "nm.exe" if os.name == "nt" else "nm")
        if not os.path.isfile(nm):
            nm = shutil.which("nm") or "nm"
        r = _run_check([nm, obj])
        nm_txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0:
            fail("smp-sched-host", "nm failed on the OFF object: %s"
                 % nm_txt.strip()[:200])
            return
        defined = []
        for ln in nm_txt.splitlines():
            parts = ln.split()
            if len(parts) >= 3 and parts[-2].isupper():
                defined.append(parts[-1])
        if defined:
            fail("smp-sched-host",
                 "USE_SMP_SCHED=0 object still defines symbols: %s"
                 % ", ".join(defined[:8]))
            return

    ok("smp-sched-host",
       "arkrtos_smp_sched per-core queues + migration on the host, and an "
       "empty TU when off (task_to/task_back gates, four switch stages, "
       "slice/rotation, idle-frame fallback, 0-sp guard): %s" % lines[0])


def check_softbus_host():
    """The softbus integration layer on the host, driving the real component.

    arkrtos_softbus.c is thin -- install, build, pump, info, selftest, shell
    command -- but every one of those calls lands inside the vendored
    ark_softbus component (core / frame / qos / rpc / stream / discovery /
    transport_loopback / port glue). So this check links the component for
    real (coverage_gate.CASES carries the source list in its "link" slot) and
    asks the kernel-side entry points to prove the wiring:

      * pump before init answers NOTUP, init is idempotent, pump(0) means one
        round;
      * the tier printed by info is the tier that was compiled in
        (msg<=128 topic<=12 dev<=4 qos=2 rpc=4 stream=1) -- the header's
        #error guards a mismatch, but only this line proves the values the
        running image actually uses;
      * selftest's ten stages (codec / reader / discovery / pubsub /
        directed+ack / rpc / stream / qos-timeout / errors / topics) all
        report PASS and the tally says "failed 0";
      * every shell subcommand and its error paths: pub / rpc / stream data
        paths, the oversize payload and oversize RPC argument refusals, the
        reply truncation at the local 32-byte result buffer, the number
        parser rejecting trailing garbage, the topic table filling up and
        reporting why, and the usage text for an unknown subcommand.

    The port hooks are stubbed with a virtual clock: ark_bus_port_tick_ms
    returns g_now and arkrtos_task_wait_internal advances it, so the 200 ms
    QoS retransmit interval is deterministic instead of a real 12-second
    wait.

    The compiled-out tier (-DARKRTOS_USE_SOFTBUS=0) must leave an empty
    translation unit -- nm on the object shows no defined global -- and the
    test's own off-branch must still run, otherwise a stub would keep
    answering PASS-shaped questions.

    Not covered: two nodes on a physical UART (two boards, or board plus PC),
    retransmit jitter under the real tick, and concurrent access to the bus
    tables from more than one core. Those are board-only facts; see
    docs/软总线接入.md.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("softbus-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    picks = {}
    for case in CASES:
        name, test, mods, defs, markers, incs, _argv = _case(case)
        if name in ("softbus", "softbusoff"):
            extra = case[5] if len(case) > 5 else {}
            picks[name] = (test, mods, defs, incs, markers,
                           list(extra.get("link", [])))
    for name in ("softbus", "softbusoff"):
        if name not in picks:
            fail("softbus-host", "case %s missing from coverage_gate.CASES" % name)
            return
    if len(picks["softbus"][5]) < 10:
        fail("softbus-host",
             "the ON case is not linking the component (%d extra sources)"
             % len(picks["softbus"][5]))
        return

    with TempDir() as td:
        lines = []
        for name in ("softbus", "softbusoff"):
            test, mods, defs, incs, markers, link = picks[name]
            exe = os.path.join(td, name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods + link)
            r = _run_check(cmd)
            if r.returncode != 0:
                err = ((r.stderr or "").strip().splitlines() or [""])
                fail("softbus-host", "%s did not build: %s"
                     % (name, [x for x in err if x.strip()][-1][:220]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            bad = r.returncode != 0 or markers[0] not in txt or "FAIL" in txt
            if bad:
                fail("softbus-host", "%s failed: %s" % (name, txt.strip()[-260:]))
                return
            if name == "softbusoff" and " off" not in txt:
                fail("softbus-host",
                     "the OFF tier did not take its own branch: %s"
                     % txt.strip()[-160:])
                return
            lines.append(txt.strip().splitlines()[-1])

        # OFF means an empty translation unit: nm on the object must show no
        # defined global. A surviving stub would still answer questions.
        off_defs = picks["softbusoff"][2]
        off_incs = picks["softbusoff"][3]
        obj = os.path.join(td, "softbus_off.o")
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"] + off_defs
               + off_incs + COMMON_INC
               + ["-c", "-o", obj, "kernelsrc/src/arkrtos_softbus.c"])
        r = _run_check(cmd)
        if r.returncode != 0:
            fail("softbus-host", "OFF object did not build: %s"
                 % ((r.stderr or "").strip().splitlines() or [""])[-1][:200])
            return
        nm = os.path.join(os.path.dirname(cc), "nm.exe" if os.name == "nt" else "nm")
        if not os.path.isfile(nm):
            nm = shutil.which("nm") or "nm"
        r = _run_check([nm, obj])
        nm_txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0:
            fail("softbus-host", "nm failed on the OFF object: %s"
                 % nm_txt.strip()[:200])
            return
        defined = []
        for ln in nm_txt.splitlines():
            parts = ln.split()
            if len(parts) >= 3 and parts[-2].isupper():
                defined.append(parts[-1])
        if defined:
            fail("softbus-host",
                 "USE_SOFTBUS=0 object still defines symbols: %s"
                 % ", ".join(defined[:8]))
            return

    ok("softbus-host",
       "arkrtos_softbus drives the real ark_softbus component over the "
       "in-board loopback on the host (init/pump/info/selftest ten stages, "
       "all shell subcommands and their error paths), empty TU when off: %s"
       % lines[0])

def check_net_host():
    """The socket layer on the host, driven against a fake lwIP.

    arkrtos_net.c is a request-slot service: the App-side entry drops a
    request into a slot and the net task picks it up and calls the stack.
    Linking the real lwIP would drag in tcpip threads, sys_arch and a NIC, so
    a failure inside that block would be indistinguishable from a kernel bug
    -- and the code under test is the layer this repo rewrote, not the stack.
    So this check links a fake lwIP (kernelsrc/tests/net_stub) that can force
    each entry to fail and records the last arguments, and asks the kernel
    entry points to prove:

      * a0 is a pointer to the argument block (not the first argument) --
        getting that wrong segfaults;
      * EPENDING on the first call, then the result of the worker's actual
        execution on the second;
      * handle lifetime: an accept that lands while the table is full closes
        the new fd and returns ENOBUFS; a close failure still frees the row;
      * every errno in the mapping table, one by one, through send failure;
      * per-operation failure mapping for the whole socket API;
      * a handle that is not owned by the caller is refused with EINVAL
        before it ever reaches the stack;
      * when the owner dies mid-flight, the DONE answer is dropped and the
        worker's result is still EINVAL (read through host_req_result, which
        is the only way to see an answer the design intentionally discards).

    The compiled-out tier (-DARKRTOS_USE_LWIP=0) must return honest refusals
    (start 0, ready 0, svc ENOTSUP) rather than empty success, and the test's
    own off-branch must still run.

    Not covered: the real lwIP socket lifecycle, a real NIC and real timing,
    two boards talking to each other, and the non-blocking connect
    EINPROGRESS progression. Those are board-only facts.

    Host is 32-bit MinGW (ILP32, pointer == long == 4 bytes), so
    ARKRTOS_SVC_WIDE_ARGS is 0 at compile time here and the wide-arg branch
    is not compiled; it is covered on 64-bit targets, not disguised here.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("net-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    picks = {}
    for case in CASES:
        name, test, mods, defs, markers, incs, _argv = _case(case)
        if name in ("net", "netoff"):
            extra = case[5] if len(case) > 5 else {}
            picks[name] = (test, mods, defs, incs, markers,
                           list(extra.get("link", [])))
    for name in ("net", "netoff"):
        if name not in picks:
            fail("net-host", "case %s missing from coverage_gate.CASES" % name)
            return

    with TempDir() as td:
        lines = []
        for name in ("net", "netoff"):
            test, mods, defs, incs, markers, link = picks[name]
            exe = os.path.join(td, name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods + link)
            r = _run_check(cmd)
            if r.returncode != 0:
                err = ((r.stderr or "").strip().splitlines() or [""])
                fail("net-host", "%s did not build: %s"
                     % (name, [x for x in err if x.strip()][-1][:220]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            bad = r.returncode != 0 or markers[0] not in txt or "FAIL" in txt
            if bad:
                fail("net-host", "%s failed: %s" % (name, txt.strip()[-260:]))
                return
            if name == "netoff" and " off" not in txt.lower():
                fail("net-host",
                     "the OFF tier did not take its own branch: %s"
                     % txt.strip()[-160:])
                return
            lines.append(txt.strip().splitlines()[-1])

    ok("net-host",
       "arkrtos_net request slot / owner / window check against a fake lwIP "
       "on the host (errno table, per-op failures, handle lifetime, owner "
       "death), honest ENOTSUP when off: %s" % lines[0])

def check_net_virtio():
    """Real Ethernet frames over QEMU virtio-net (ARKRTOS_USE_NET_VIRTIO).

    The switch defaults to 0, so every line of the driver, the lwIP link
    layer and the ping demo is dead code in the normal build.  "It pings"
    is also the easiest claim in this repo to fake (the loopback netif can
    make a raw callback fire, and a log line saying "reply" proves nothing),
    so tools/net_virtio_gate.py judges on readings that exist outside our
    own printf: a QEMU filter-dump pcap with the ARP exchange and the three
    echo round trips, plus a `-net none` run that must fail.  It cleans and
    rebuilds for both tiers (make does not track EXTRA_CFLAGS), so it is a
    slow check; a missing toolchain is a FAIL, not a skip.
    """
    r = _run_check([sys.executable, os.path.join("tools", "net_virtio_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "NET-VIRTIO-GATE OK" in txt:
        ok("net-virtio", txt.splitlines()[-1])
    else:
        fail("net-virtio", txt[-360:])

def check_elf_host():
    """The ELF loader's static path, driven on the host.

    arkrtos_elf.c is the one place where a file stopped being bytes and
    became an address space: header checks, per-segment range checks against
    the window, the write pass (filesz copy + memsz-filesz zero), the Linux
    argv/auxv stack, and the atomic task hand-over. A wrong range check does
    not fail loudly -- it silently overwrites the kernel, and the symptom
    shows up later as a random crash in an unrelated task. So this check
    builds a fake VFS and a crafted ELF image per case and asserts on the
    exact error code returned *before the first byte is written*:

      * param gate: bad path, argc range, null argv element;
      * header gate: magic, class, endianness, machine, e_type, phentsize,
        phnum over the cap, phdr offset past EOF (short read);
      * segment gate: filesz > memsz, window escapes, stack overlap, zero
        PT_LOADs, address wraparound;
      * write pass: filesz copied, the rest of memsz zeroed, and the byte
        right after memsz left untouched;
      * stack: argv strings top-down and 8-byte aligned, AT_RANDOM 16 bytes
        aligned and non-zero, AT_PHDR only when the table is inside a LOAD,
        sp pointing at argc;
      * task: registered, de-privileged, window written back to the TCB,
        MPU context rebuilt;
      * ledger: running / running_in / info / last_space / BUSY, and lazy
        cleanup once the task dies;
      * exec_replace: no new task, the caller's id is kept.

    The window is really mapped (VirtualAlloc / mmap(MAP_FIXED)): the loader
    copies straight to the VA by design, so an unmapped window would mean
    writing to random addresses. If the mapping fails the case FAILs -- it
    never "skips", because skipping would run every assertion on unmapped
    memory and look like a pass.

    Host is 32-bit MinGW (ILP32), so only the ELF32 path is compiled here:
    ELF64, PT_INTERP/ld.so, TLS blocks, paging spaces, exit codes and
    signature enforcement are board-only and are covered on QEMU LoongArch64,
    not disguised here. The OFF tier (-DARKRTOS_USE_ELF=0) must return honest
    refusals and its own macros must still answer.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("elf-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    picks = {}
    for case in CASES:
        name, test, mods, defs, markers, incs, _argv = _case(case)
        if name in ("elf", "elfoff"):
            extra = case[5] if len(case) > 5 else {}
            picks[name] = (test, mods, defs, incs, markers,
                           list(extra.get("link", [])))
    for name in ("elf", "elfoff"):
        if name not in picks:
            fail("elf-host", "case %s missing from coverage_gate.CASES" % name)
            return

    with TempDir() as td:
        lines = []
        for name in ("elf", "elfoff"):
            test, mods, defs, incs, markers, link = picks[name]
            exe = os.path.join(td, name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods + link)
            r = _run_check(cmd)
            if r.returncode != 0:
                err = ((r.stderr or "").strip().splitlines() or [""])
                fail("elf-host", "%s did not build: %s"
                     % (name, [x for x in err if x.strip()][-1][:220]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            bad = r.returncode != 0 or markers[0] not in txt or "FAIL" in txt
            if bad:
                fail("elf-host", "%s failed: %s" % (name, txt.strip()[-260:]))
                return
            if name == "elfoff" and " off" not in txt.lower():
                fail("elf-host",
                     "the OFF tier did not take its own branch: %s"
                     % txt.strip()[-160:])
                return
            lines.append(txt.strip().splitlines()[-1])

    ok("elf-host",
       "arkrtos_elf static path on the host against a crafted image and a "
       "fake VFS (header / segment / stack / task ledger / exec_replace), "
       "honest refusals when off: %s" % lines[0])

def check_linux_abi_host():
    """The Linux syscall ABI, driven on the host: one call from entry to the
    value written back into a0.

    arkrtos_linux_abi.c is the layer that decides whether a syscall was
    accepted, what errno the program sees, and whether user memory was
    touched. Every one of those can be wrong in a way that still "runs":
    a copy that advances the fd offset before it knows the copy succeeded
    loses bytes silently, a signal frame written in the wrong order leaves
    a half-installed handler behind. So this check drives both tiers with a
    simulated user window and asserts on the exact return code and on the
    exact bytes the program would see:

      * entry: Linux-vs-Ark split (0xFFFFFFFF is not ours), number table,
        exit family must not come back;
      * return classes: pointer results written wide, errors classified,
        exec success writes nothing (Linux: execve does not return);
      * fd table: open/close bounds, EBADF for free slots, O_* refusals,
        offsets on failure (no data loss, no replay);
      * pipes: read/write wrap, EOF only when no writer is left, EPIPE,
        a read that fails on copy_to_user must not consume the pipe;
      * signals: rt_sigaction capability probe before argument checks,
        mask &= ~(SIGKILL|SIGSTOP) semantics, delivery at syscall return,
        rt_sigreturn restoring the frame, ENOSYS when the port cannot
        deliver;
      * execve: argv read as 8-byte slots, E2BIG past the cap, errno
        mapping per loader failure, frame swap only after a successful
        load, ENOSYS when the port has no frame swap;
      * reset: fd/pipe/signal tables cleared, the caller's context is the
        caller's business (reset does not touch registers).

    Host is 32-bit MinGW (ILP32, unsigned long == 4 bytes). Three things
    this check explicitly does not claim to prove:
      1) the user window is simulated (copy_from_user/to_user are stubs),
         so the real paging backend's window test is not exercised;
      2) the 64-bit signal-frame layout (32-byte header, payload at 32) is
         not reproduced here -- only the 32-bit shape (16-byte header);
      3) a >4GB user address cannot be expressed, so argv slots are read
         as 8-byte slots with the high half forced to zero.
    The QEMU LoongArch64 run in linux-abi covers (1) and (2).

    The FUTEX tier (-DARKRTOS_USE_LINUX_FUTEX=1) drives all six futex ops
    (WAIT / WAKE / REQUEUE / CMP_REQUEUE / WAIT_BITSET / WAKE_BITSET, plus
    timeout) against the same simulated window: the key is the physical
    frame, so the test supplies the paging backend's four readers and
    asserts that two different pages are two different keys, that a WAKE on
    one key does not wake a waiter on the other, that a REQUEUE moves a
    waiter onto uaddr2's key and a CMP_REQUEUE only does so when the word
    matches, that a bitset WAKE only wakes waiters whose mask intersects,
    that a value change or a signal account-close the registration, that a
    relative WAIT timeout and an absolute WAIT_BITSET deadline both expire
    as -ETIMEDOUT (the deadline is stored, not the duration, so a replay
    does not restart the clock), that CLOCK_REALTIME is refused -ENOSYS
    rather than faked, and that a port without restart answers -ENOSYS
    instead of pretending to have slept.

    The OFF tier (-DARKRTOS_USE_LINUX_ABI=0) must answer "there is no such
    thing" for every reading, never an empty success.

    The SELECT tier (-DARKRTOS_USE_LINUX_SELECT=1) drives pselect6 against
    the same window: a ready fd counts once, the caller's fd_set is rewritten
    to only the ready bits, a descriptor with no object behind it answers
    -EBADF, exceptfds is accepted but never reported, timeout=0 polls and
    does not sleep, a zero timeout and an expired deadline both answer 0 with
    the sets cleared, the deadline is stored (a replay does not restart the
    clock) while readiness wins over a deadline that has not passed, a task
    exit clears a pending deadline so a reused task id does not inherit it,
    and a port without restart answers -EAGAIN instead of pretending to wait.

    Link-time note: the "port cannot swap frames" default is a weak symbol,
    and MinGW at -O0 resolves a weak definition inside the same link to the
    local entry (-ffunction-sections does not help; the call lands outside
    any section and faults), so both tiers are linked with
    -Wl,--wrap=arkrtos_port_svc_exec_frame and the test supplies __wrap_.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("linux-abi-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    picks = {}
    for case in CASES:
        name, test, mods, defs, markers, incs, _argv = _case(case)
        if name in ("linuxabi", "linuxabioff", "linuxabifutex",
                    "linuxabiselect", "linuxabisocket"):
            extra = case[5] if len(case) > 5 else {}
            picks[name] = (test, mods, defs, incs, markers,
                           list(extra.get("link", [])),
                           list(extra.get("ldflags", [])))
    for name in ("linuxabi", "linuxabioff", "linuxabifutex",
                 "linuxabiselect", "linuxabisocket"):
        if name not in picks:
            fail("linux-abi-host",
                 "case %s missing from coverage_gate.CASES" % name)
            return

    with TempDir() as td:
        lines = []
        for name in ("linuxabi", "linuxabioff", "linuxabifutex",
                     "linuxabiselect", "linuxabisocket"):
            test, mods, defs, incs, markers, link, ldflags = picks[name]
            exe = os.path.join(td, name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods + link
                   + ldflags)
            r = _run_check(cmd)
            if r.returncode != 0:
                err = ((r.stderr or "").strip().splitlines() or [""])
                fail("linux-abi-host", "%s did not build: %s"
                     % (name, [x for x in err if x.strip()][-1][:220]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            bad = r.returncode != 0 or markers[0] not in txt or "FAIL" in txt
            if bad:
                fail("linux-abi-host", "%s failed: %s"
                     % (name, txt.strip()[-260:]))
                return
            if name == "linuxabioff" and " off" not in txt.lower():
                fail("linux-abi-host",
                     "the OFF tier did not take its own branch: %s"
                     % txt.strip()[-160:])
                return
            lines.append(txt.strip().splitlines()[-1])

    ok("linux-abi-host",
       "arkrtos_linux_abi syscall round trip on the host against a simulated "
       "user window (entry split, return classes, fd / pipe / signal / execve "
       "paths, futex six ops, pselect6 readiness, socket translation, "
       "reset), honest refusals "
       "when off: %s" % lines[0])

def check_mini_host():
    """The MiniApp loader, driven on the host: one image from header read to a
    registered task, and the two switch tiers that share the same load path.

    arkrtos_mini.c decides, in order: is the path an identity string, is
    there a free slot / is the concurrency cap reached, is the image header
    self-consistent, may we borrow a code block and a RAM block, did every
    chunk read come back whole, does the CRC match, can a task be registered,
    and only then does the instance become RUN. Every one of those steps can
    be skipped in a way that still "works" once: a slot left occupied after a
    failed load leaks a block pair per attempt, a failed rewind leaves an
    unpatched image that runs and silently computes the wrong thing.

    So this check drives the module with loader / ptable / task / crash
    stubs and asserts on the call ledger (which byte range went to whom, how
    many times, in what order) plus the exact error code on each failure
    path, and on the seam where the second copy is armed but the switch has
    not landed yet. Two tiers:
      * ON  (mini_test_board.h)   -- ARKRTOS_USE_MINIAPP=1, LA64 paging
        board, read path through arkrtos_vfs_read_at;
      * OFF (mini_test_board_off.h) -- same board with the switch at 0; the
        module must compile to an empty object and every reading must answer
        "there is no such thing".

    Honest limits (also written down in the test's own header):
      1) loader_check_header / reloc_apply / crc_header are stubs -- what the
         loader decides is covered by its own host case;
      2) ptable_mini_alloc/free is a stub (first-fit over four 64 KiB
         blocks); the real allocator's granularity is covered by its own;
      3) the MPU/paging backend is stubbed: we only record that a window was
         built for the right task, not that it is in effect (board-only);
      4) scheduler lock / activate / yield are stubs: call counts only, no
         real timing;
      5) crash bookkeeping is a stub: ownership + "disabled after the cap"
         is exercised, survival across reset is board-only.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("mini-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    picks = {}
    for case in CASES:
        name, test, mods, defs, markers, incs, _argv = _case(case)
        if name in ("mini", "minioff"):
            picks[name] = (test, mods, defs, incs, markers)
    for name in ("mini", "minioff"):
        if name not in picks:
            fail("mini-host", "case %s missing from coverage_gate.CASES" % name)
            return

    with TempDir() as td:
        lines = []
        for name in ("mini", "minioff"):
            test, mods, defs, incs, markers = picks[name]
            exe = os.path.join(td, name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                err = (r.stderr or "").strip().splitlines() or [""]
                fail("mini-host", "%s did not build: %s"
                     % (name, [x for x in err if x.strip()][-1][:220]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            bad = r.returncode != 0 or markers[0] not in txt or "FAIL" in txt
            if bad:
                fail("mini-host", "%s failed: %s" % (name, txt.strip()[-260:]))
                return
            if name == "minioff" and " off" not in txt.lower():
                fail("mini-host",
                     "the OFF tier did not take its own branch: %s"
                     % txt.strip()[-160:])
                return
            lines.append(txt.strip().splitlines()[-1])

    ok("mini-host",
       "arkrtos_mini image load on the host (path / slot / header / borrow / "
       "chunk read / CRC / task seam, failure unwinds, swap arm-take, "
       "autostart list), honest refusals when off: %s" % lines[0])

def check_task_host():
    """The scheduler core (arkrtos_task.c) driven on the host.

    arkrtos_task.c is the largest single module in the kernel and the one whose
    failures are quiet: a ready bucket that keeps a bit for a task that is no
    longer on the chain makes ready_top return a stale slot, and the caller's
    +1 turns 255 into 256 -- an out of bounds task table read that shows up on
    LoongArch64 as an occasional ADEM, not as a failed build. The same class of
    "numbers look right, the set disagrees" applies to the delay chain, the
    stack watermark, the fault recovery rebuild and the A/B swap block move.

    So this case links the real module against a board header that keeps only
    the task switches, stubs every port function (stack init, switch, MPU,
    guard, swap frame rewrite/activate, console, trace) and asserts on:
      * the ready bitmap / bucket / delay chain invariants, including the
        reprio path and the defensive heal of an out of range bucket head;
      * sched_next / sched_activate / sched_is_switching, the recovered-task
        rebuild inside the fault handler, and the selftest swap hook;
      * the whole SVC dispatch table: at least one accepted subcommand and one
        refusal per service number, plus the refusal for services that are not
        compiled in (SBUF / MEMPOOL must answer -1, not pretend success);
      * the swap four steps measured through the stubs (which frame went to
        which address, which entry, what the TCB ends up holding).
    511 checks, marker "TASK HOST OK".

    Honest limits:
      1) PendSV itself is not run: sched_activate is called directly and the
         port switch stub only counts; the real register save/restore is a
         QEMU/board reading;
      2) the swap frame rewrite is a stub -- what the port does to the trap
         frame (ARM basic frame / RV mepc / LA64 ERA) is covered per port;
      3) MPU region programming and privilege switching are stubs: we check a
         window was built for the right task, not that hardware took it;
      4) the recovery path's stack_init goes through the host stub, so "a new
         frame was built" is a call fact, not a disassembly;
      5) SMP scheduling, the second core's tables and the board-only parts of
         the guard scanner are out of scope for this TU.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("task-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    for case in CASES:
        name, test, mods, defs, markers, incs, _argv = _case(case)
        if name == "task":
            pick = (test, mods, defs, incs, markers)
            break
    if pick is None:
        fail("task-host", "case task missing from coverage_gate.CASES")
        return

    test, mods, defs, incs, markers = pick
    with TempDir() as td:
        exe = os.path.join(td, "task" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
        r = _run_check(cmd)
        if r.returncode != 0:
            err = (r.stderr or "").strip().splitlines() or [""]
            fail("task-host", "the case did not build: %s"
                 % [x for x in err if x.strip()][-1][:220])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt or "FAIL" in txt:
            fail("task-host", txt.strip()[-260:])
            return
        last = txt.strip().splitlines()[-1]

    ok("task-host",
       "arkrtos_task on the host (ready bitmap / delay chain / sched activate / "
       "fault rebuild / A/B swap / SVC dispatch table): %s" % last)

def check_shell_misc_host():
    """The Shell misc command family (arkrtos_shell_misc.c) on the host.

    This file is where several read-only commands live: history / uname /
    free / ps / which / man / dmesg / hwsafe / swap / lxstat.  They are the
    part of the kernel a field engineer actually types at, and their failure
    mode is quiet: a row that prints the wrong field still looks like a
    table, a command that returns 0 after refusing still looks successful,
    and a percentage computed from a zero total divides by zero on a target
    where nobody is watching.

    So this case links the real file against stubs for every kernel reading
    it touches (partition table + stats, task table, hwsafe ledger, log ring,
    Linux ABI counters, task swap) and asserts on 128 checks:
      * each command's formatted line, with fixture numbers distinct enough
        that printing the wrong field cannot pass;
      * the documented exit code for every refusal (bad usage, no log ring,
        unknown manual page, unknown command, swap cannot register / cannot
        start) and for the happy paths;
      * the defensive arithmetic: total == 0 gives 0 percent, used > total
        clamps free at 0 instead of underflowing;
      * the "stop on first bad index" loops (hwsafe device / ledger).

    Honest limits:
      1) the swap success path is not reachable on the host -- it needs the
         test task to really run so its counters advance, which needs a
         scheduler and a trap frame.  Only the two refusal paths and the
         stat line are covered; those 28 lines are a known floor;
      2) the UART is a buffer here: this proves what the command emits, not
         that it reaches a wire;
      3) the fixture tables are not real ledgers -- a real hwsafe / Linux
         ABI table has shapes this stub does not reproduce.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("shellmisc-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    for case in CASES:
        name, test, mods, defs, markers, incs, _argv = _case(case)
        if name == "shellmisc":
            pick = (test, mods, defs, incs, markers)
            break
    if pick is None:
        fail("shellmisc-host", "case shellmisc missing from coverage_gate.CASES")
        return

    test, mods, defs, incs, markers = pick
    with TempDir() as td:
        exe = os.path.join(td, "shellmisc" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
        r = _run_check(cmd)
        if r.returncode != 0:
            err = (r.stderr or "").strip().splitlines() or [""]
            fail("shellmisc-host", "the case did not build: %s"
                 % [x for x in err if x.strip()][-1][:220])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt or "FAIL" in txt:
            fail("shellmisc-host", txt.strip()[-260:])
            return
        last = txt.strip().splitlines()[-1]

    ok("shellmisc-host",
       "arkrtos_shell_misc on the host (history / uname / free / ps / which / "
       "man / dmesg / hwsafe / swap / lxstat): %s" % last)

def check_shell_ushell_host():
    """The user-command registry (arkrtos_shell_ushell.c) on the host.

    This is the kernel side of SVC 0x1A: an App registers a command name and
    a handler, the kernel copies the strings into its own RAM, appends a
    trampoline row to the console command table, and refuses to call the
    handler once the image that owns it is gone.  Its failure modes are the
    quiet kind: a registry row that keeps a pointer the caller has already
    freed, a table row left behind after unregistration (help then lists a
    command that cannot run), or a trampoline that jumps into erased Flash
    instead of saying the handler is gone.

    So this case links the real file against stubs for the console (output
    capture + the command table) and the partition table, and asserts on 278
    checks (M-08-4 added the upcall arm: the marshaled argv block is pinned
    byte by byte in the App window, and the registered stack must stop at the
    worker's sp so the initial trap frame cannot paint over those arguments):
      * case-insensitive duplicate detection over the whole name (a prefix
        is not a match), and case-insensitive unregistration;
      * the copy bounds: exactly NAME_MAX characters is accepted and one
        more is refused; help is clipped to HELP_MAX; a NULL help becomes
        the empty string;
      * four PARAM refusals with no registry state moved, plus the two
        different FULLs (kernel table full is checked before the registry
        fills), and slot reuse after a free;
      * unregistration shifting the later rows down so help order has no
        hole, with the cleared slot's strings wiped;
      * the trampoline: no argv / no argv[0] / argc <= 0 / unmatched name
        all answer "user command not available" and never call; an entry
        whose handler is outside every loaded slot answers "handler no
        longer available"; boundary (first byte inside, one past the end
        outside), FREE / base 0 / not-loaded slots skipped, slot_max clamp,
        and a NULL partition table treated as "cannot prove it is alive";
      * a successful call returns the handler's code and raises
        arkrtos_kernel_ushell_active only for the duration of the call;
      * arkrtos_shell_print_n chunks at 64 bytes (exact multiples emit no
        empty tail), copies only the first len bytes, and refuses NULL.

    Honest limits:
      1) no SVC transition and no user RAM window check: the entry is called
         directly, so "an App can really reach this" is not shown here. On the
         isolated path that half is covered end-to-end on QEMU by
         tools/ushell_iso_gate.py ([ushell] isolated ... plv=3), which is a
         separate script because it needs QEMU and this gate must not;
      2) the active flag is only observed to be raised and dropped -- what
         the task side does with it is covered by the task case;
      3) the one uncovered line is the NULL/NULL arm of the name comparison:
         every caller checks for NULL before calling, so it is structurally
         unreachable on this path.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("ushell-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    for case in CASES:
        name, test, mods, defs, markers, incs, _argv = _case(case)
        if name == "ushell":
            pick = (test, mods, defs, incs, markers)
            break
    if pick is None:
        fail("ushell-host", "case ushell missing from coverage_gate.CASES")
        return

    test, mods, defs, incs, markers = pick
    # 两份镜像，一份源码：
    #   * 带 -DARKRTOS_USE_USHELL_APP_CB=1 —— 注册表本体（278 条）；
    #   * 不带 —— 在「特权级分离 + 分页」的板级头上必须 fail-closed：
    #     App 侧注册返回 ARKRTOS_USHELL_ERR_PRIV，内核注册照旧。
    # 只测前一份会把 SEC-G08 的补丁测成「注册全都不通」还是「全都能通」，
    # 两种都是假绿。
    defs_on = list(defs)
    defs_off = [d for d in defs if not d.startswith("-DARKRTOS_USE_USHELL_APP_CB")]
    if defs_off == defs_on:
        fail("ushell-host", "the ushell case no longer pins "
             "ARKRTOS_USE_USHELL_APP_CB; the fail-closed arm would be untested")
        return
    with TempDir() as td:
        last = []
        for tag, d in (("ushell", defs_on), ("ushell_refused", defs_off)):
            exe = os.path.join(td, tag + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + d + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                err = (r.stderr or "").strip().splitlines() or [""]
                fail("ushell-host", "%s did not build: %s"
                     % (tag, [x for x in err if x.strip()][-1][:220]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            want = markers[0] if tag == "ushell" else "USHELL-HOST REFUSED"
            if r.returncode != 0 or want not in txt or "FAIL" in txt:
                fail("ushell-host", "%s: %s" % (tag, txt.strip()[-260:]))
                return
            last.append(txt.strip().splitlines()[-1])

    ok("ushell-host",
       "arkrtos_shell_ushell on the host (register / unregister / trampoline "
       "alive check / chunked output): %s; fail-closed on priv+paging: %s"
       % (last[0], last[1]))

def check_shell_mod_host():
    """The two execution models of arkrtos_shell_mod.c on the host.

    exec runs a linked ET_EXEC through the ELF loader and waits for that
    address space only; ld runs a board-linked ET_REL through the module
    loader and does not wait at all.  Both are the place where a wrong
    reading is expensive precisely because it looks plausible: an exec that
    prints a truncated entry address, a wait that reports 0 for a task that
    never set a code, or an ld that prints a bare rc instead of the missing
    symbol name all send the reader hunting in the wrong layer.

    So this case links the real file against stubs for both loaders, the
    task table and the export table, and asserts on 131 checks:
      * exec usage, and a load failure that repeats the loader's own error
        name and rc (not a shell-side rewording);
      * the success reading prints bytes / segments / entry / sp / argc /
        task / space from the fixture, then the duration, and the exit code
        only when the instance reported one -- the "no code" arm prints the
        duration alone rather than a zero that looks like a status;
      * the 30 s cap gives the prompt back and says the program is still
        running, with exactly 30000/20 polls and one yield per poll;
      * the fallback only takes words that look like paths (/x, ./x, ../x),
        and answers -1 for a bare word, "." and ".." so the caller still
        reports command not found;
      * wait refuses five malformed inputs (missing id, non-numeric id,
        id 0, id past the task table, non-numeric timeout) before it reads
        the task table, refuses the shell's own task, refuses an unused
        slot, and then reports exit code / no code / still-running;
      * ld reports the missing symbol name and the export-table hint, or the
        bad relocation number, or one line for anything else; on success it
        describes the row it just added and stays silent when the count did
        not grow or the row cannot be read;
      * lkup prints an address, "not exported", or the whole table (with "?"
        for a row whose name is NULL) and says so when the table is absent;
      * modules says "no module loaded" on an empty list, prints one line
        per readable row with run/done state, and skips unreadable rows;
      * unld refuses a missing or non-numeric index and reports a refused
        unload with the loader's name and rc.

    Honest limits:
      1) nothing is parsed, mapped, relocated or scheduled: both loaders,
         the task table and the export table are stubs, so this shows the
         shell's logic, not that a real image loads;
      2) the 30 s cap is 1500 yields, not 30 s of wall time;
      3) the shell task's stack is not stressed here -- the 3080 B peak that
         arkrtos_modld.h warns about is a board reading, not a host one.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("shellmod-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    for case in CASES:
        name, test, mods, defs, markers, incs, _argv = _case(case)
        if name == "shellmod":
            pick = (test, mods, defs, incs, markers)
            break
    if pick is None:
        fail("shellmod-host", "case shellmod missing from coverage_gate.CASES")
        return

    test, mods, defs, incs, markers = pick
    with TempDir() as td:
        exe = os.path.join(td, "shellmod" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
        r = _run_check(cmd)
        if r.returncode != 0:
            err = (r.stderr or "").strip().splitlines() or [""]
            fail("shellmod-host", "the case did not build: %s"
                 % [x for x in err if x.strip()][-1][:220])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt or "FAIL" in txt:
            fail("shellmod-host", txt.strip()[-260:])
            return
        last = txt.strip().splitlines()[-1]

    ok("shellmod-host",
       "arkrtos_shell_mod on the host (exec reading + 30 s cap / wait "
       "refusals and exit code / ld symbol and reloc naming / lkup / "
       "modules / unld): %s" % last)


def check_shell_cfg_host():
    """Pool readings and the device CONFIG region on the host.

    arkrtos_shell_cfg.c carries the two families whose wrong answer costs the
    most because it looks right: "mpool list" and "cfg show" print numbers a
    reader will act on, and "cfg load" answers a host tool with ACK/NAK -- a
    device that ACKs a record it did not write leaves the host believing the
    layout is applied.  So this case links the real file against a four-block
    pool stub, a layout stub and a programmable byte stream, and asserts on
    132 checks:
      * mpool list prints one row per live pool with every column fed from
        the snapshot, silently skips a handle that is alive but cannot be
        queried, and prints "(no pool)" only when nothing was printed;
      * mpool test reports each check separately (create / alloc all four /
        exhaust / stats / free+reuse / reject bad free / duplicate name /
        tiny block / delete / alive-after-delete) and returns -1 the moment
        any of them fails -- each FAIL line is reached by injecting exactly
        that defect, so the summary line is never the only evidence;
      * mpool usage returns -1 with the two-line synopsis;
      * cfg_recv_exact reassembles a record from short reads and gives up
        after the accumulated idle time reaches the timeout;
      * cfg show prints mode / source / region geometry / reclaim / knobs /
        boot line, then one of the three record verdicts (rejected with the
        reason name, accepted, compile-time default), and the slot table only
        when the count is non-zero;
      * cfg load drains the console (both "empty" and "64 reads of junk"
        shapes), sends ACK for the first three chunks, and on the last chunk
        either ACKs after a successful write (with and without the "region
        erased first" suffix) or prints the reason / the write rc and NAKs;
        the timed-out chunk NAKs without ever writing;
      * cfg clear reports the erase rc, and cfg usage returns -1.

    Honest limits:
      1) the pool below is a bookkeeping stub, not the kernel pool -- block
         ownership and the free list are tested in mempool-host;
      2) the layout module is stubbed down to return codes, so this says
         nothing about where a record lands in Flash;
      3) "5000 ms" is 2500 counted yields, not wall time, and the console is
         a byte array, not a UART.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("shellcfg-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    for case in CASES:
        name, test, mods, defs, markers, incs, _argv = _case(case)
        if name == "shellcfg":
            pick = (test, mods, defs, incs, markers)
            break
    if pick is None:
        fail("shellcfg-host", "case shellcfg missing from coverage_gate.CASES")
        return

    test, mods, defs, incs, markers = pick
    with TempDir() as td:
        exe = os.path.join(td, "shellcfg" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
        r = _run_check(cmd)
        if r.returncode != 0:
            err = (r.stderr or "").strip().splitlines() or [""]
            fail("shellcfg-host", "the case did not build: %s"
                 % [x for x in err if x.strip()][-1][:220])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt or "FAIL" in txt:
            fail("shellcfg-host", txt.strip()[-260:])
            return
        last = txt.strip().splitlines()[-1]

    ok("shellcfg-host",
       "arkrtos_shell_cfg on the host (mpool row/verdict paths and every "
       "selftest failure / cfg show verdicts and slot table / cfg load "
       "ACK-NAK over a 4-chunk record, timeout, validate and write "
       "failures): %s" % last)

def check_shell_image_host():
    """Image and installer command families on the host.

    arkrtos_shell_image.c is where a wrong answer costs hardware: "install"
    erases the flash of a slot before it receives the frame, so erasing a slot
    that is still executing stops the running app with USGFAULT and the
    operator only sees "install ok"; "info" prints the pool census people size
    their images from; "app"/"drv" are the only way to see which record is
    RUNNING.  This case links the real file against a fixture partition table,
    programmable loader/installer return codes and a fixed-shape console, and
    asserts on 168 checks:
      * info prints layout / pool census / task and tick lines, and a pool map
        that is ordered by base, not by index, and that skips FREE records,
        base==0 records and anything below the last printed base, saying
        "(pool is empty)" when nothing printed;
      * app lists only APP records (an EMPTY record is still listed) and
        prints the held reason per row; start / stop / uninstall each name
        their own usage, success and failure lines; install refuses a slot
        number outside fixed-slot mode, refuses an out-of-range / NULL slot,
        names the missing file system, and reports the installer rc;
      * app swap (compiled in with -DARKRTOS_USE_TASK_SWAP=1) reports
        ERR_STATE, SWAP_ERR_LAYOUT and ERR_BUSY as three different lines;
      * drv does the same over DRIVER records;
      * install refuses a bare command in fixed-slot mode, refuses a slot that
        cannot be parsed / does not exist / exists only in auto mode, and in
        fixed mode walks the whole slot table: FREE records and non-overlapping
        records are skipped, an overlapping record that is not running is
        skipped, and an overlapping RUNNING record is stopped first (or the
        erase is refused when the stop fails, and a failed erase is reported);
      * after the one-shot window the console is drained (N reads then 0) and
        the installer result is reported.

    Honest limits:
      1) nothing is really started, stopped or installed: every loader and
         installer entry point is a programmable return code;
      2) the partition table and arkrtos_ptable_stats() are fixtures, and
         arkrtos_dev_read_internal()/arkrtos_shell_uart_open() are stubs, so
         this says nothing about a real UART or a real flash erase;
      3) the apprun and flashapp blocks are NOT compiled here: the loongarch64
         board sets ARKRTOS_USE_APPRUN=0 and ARKRTOS_USE_FLASHAPP=0 (that
         layout has neither an apprun window nor a flash app area), so those
         two families have no host reading yet.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("shellimage-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    for case in CASES:
        name, test, mods, defs, markers, incs, _argv = _case(case)
        if name == "shellimage":
            pick = (test, mods, defs, incs, markers)
            break
    if pick is None:
        fail("shellimage-host", "case shellimage missing from coverage_gate.CASES")
        return

    test, mods, defs, incs, markers = pick
    with TempDir() as td:
        exe = os.path.join(td, "shellimage" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
        r = _run_check(cmd)
        if r.returncode != 0:
            err = (r.stderr or "").strip().splitlines() or [""]
            fail("shellimage-host", "the case did not build: %s"
                 % [x for x in err if x.strip()][-1][:220])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt or "FAIL" in txt:
            fail("shellimage-host", txt.strip()[-260:])
            return
        last = txt.strip().splitlines()[-1]

    ok("shellimage-host",
       "arkrtos_shell_image on the host (info pool map ordering / app and drv "
       "usage + start + stop + uninstall + install refusals / swap ERR_STATE, "
       "LAYOUT and BUSY / install stop-before-erase with failing stop and "
       "failing erase / console drain): %s" % last)

def check_shell_pmap_host():
    """Paging command family: the eighteen pmap subcommands on the host.

    arkrtos_shell_pmap.c is where a user asks the machine "is this address
    really translated".  A silent regression here reads as a green board test,
    so every refusal path and every printed branch is pinned by 295 checks:
      * the bare command and an unknown subcommand print the paging audit
        counters and the full usage block, and return 0 (a read-only command
        must not look like an error to a script);
      * map / check / unmap / rd / rb / wrb / dyn / frame / ftest / rel /
        space / pairs / look / u1 / elf / u2 / cu each accept their own
        argument shape and reject a malformed one with their own usage line
        and their own exit code (the -1-vs-1 split between "wrong usage" and
        "the operation failed" is asserted, not assumed);
      * rd clamps the word count at 8; pmap_hex refuses an empty string, a
        bare "0x", a non-digit and accepts lowercase digits;
      * ftest walks 1 + 3 frames, writes and reads back a 64-word pattern,
        and refuses an out-of-pool free with -1 and a double free with -3;
      * test maps a ROM page outside the static mapping, compares it with the
        direct address, proves unmap drops the entry (lookup, not just rc=0),
        rebinds the same VA and compares again; the first comparison is shown
        to only report (mismatch=1 still passes), which is exactly why the
        rebind loop is the one that gates;
      * u1 writes two words through one VA in two spaces and reads both back;
        both a total failure (no frame switch) and a partial one (only the
        second switch fails, "1 of 2 reads wrong") are driven;
      * elf prints the per-space ledger and caps the loop at the task table,
        not at the space count;
      * u2 loads one ELF path into two spaces and checks from the same VA that
        each load only touches its own frame, that both instances are live
        with distinct task ids bound to their own space, and every failure
        string the command can emit (reused space, sentinel still there,
        neighbour written, second load clobbering the first, shared task id,
        wrong binding, no live task);
      * cu drives all four refusal codes of the controlled user access
        (-1 wrap, -2 no user window / kernel caller, -3 outside span / kernel
        pointer / unmapped / write to ROM, -4 wrong space), both success
        directions, and the "reports ok but copies nothing" byte check.

    Honest limits:
      1) no real MMU: every paging / port / ELF entry point is a stub, and the
         TLB is modelled by a per-space shadow copy of the 2 MB window, so
         "same VA, two frames" is a memory model, not hardware translation;
      2) one branch is deliberately left uncovered - ftest's pattern mismatch
         cannot fire on plain host RAM (it reads back what was just written),
         and the test refuses to fake it with a fault injector; measured
         coverage is therefore 99.42% (512 of 515 lines), not 100%;
      3) the swap (U-16), ASID (U-7) and invop / asidinval families are
         compiled out on qemu_loongarch64 and are not measured here.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("shellpmap-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    for case in CASES:
        name, test, mods, defs, markers, incs, _argv = _case(case)
        if name == "shellpmap":
            pick = (test, mods, defs, incs, markers)
            break
    if pick is None:
        fail("shellpmap-host", "case shellpmap missing from coverage_gate.CASES")
        return

    test, mods, defs, incs, markers = pick
    with TempDir() as td:
        exe = os.path.join(td, "shellpmap" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
        r = _run_check(cmd)
        if r.returncode != 0:
            err = (r.stderr or "").strip().splitlines() or [""]
            fail("shellpmap-host", "the case did not build: %s"
                 % [x for x in err if x.strip()][-1][:220])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt or "FAIL" in txt:
            fail("shellpmap-host", txt.strip()[-260:])
            return
        last = txt.strip().splitlines()[-1]

    ok("shellpmap-host",
       "arkrtos_shell_pmap on the host (18 subcommands, map/check/unmap/rd/rb/"
       "wrb/ftest/test/u1/elf/u2/cu refusal paths with their own exit codes, "
       "same-VA two-frame memory model, cu -1/-2/-3/-4): %s" % last)

def check_shell_diag_host():
    """Diagnostic command family: the twenty subcommands on the host.

    arkrtos_shell_diag.c is where an operator reads the machine back out --
    task tables, stack peaks, watchdog contracts, crash journals, the device
    tree, RT samples, sync objects, security baseline, board unit tests and
    the pool layout.  A wrong number here is worse than a crash: it reads as
    evidence.  This case links the real file against stub kernel hooks and
    asserts on 275 checks over every command and every printed branch:
      * task: table listing with the peak column, the 56-word frame dump, the
        four "no such task" refusals (non-numeric, id 0, past the table, no
        stack) and the optional blocks/wake-reason line;
      * stack: the listing with all three "cannot measure" forms, the warn and
        danger marks, check's three verdicts, raw's two read-out sources plus
        its n/a row and the idle-context line both armed and refused;
      * guard: disabled / requested-but-not-running / armed, per-slot rows for
        all four states, an unset age printing "-", an empty slot skipped and
        a slot the port cannot read stopping the table;
      * crash: empty journal, per-slot rows, a MiniApp row whose hash matches
        an autostart path, one that only has its raw hash, and the empty
        MiniApp case;
      * audit: clean and failing passes, the offender count as the exit code,
        a bit whose name is unknown skipped rather than printed;
      * sched: consistent and inconsistent, both mismatch directions;
      * dt: a device node printed, a non-device skipped, a NULL hole skipped;
      * fdt: not-ready, the whole-tree dump (with a node without compatible
        skipped and a walk that ends on an error), path (usage / refused /
        found), phandle (bad argument / refused / resolved), chosen (absent /
        string / cell form / resolve refused / resolved), get (missing
        property / string / string list / cells with a non-cell tail / a
        control byte inside a NUL-terminated value), overlay (not built /
        refused with reason / merged but rejected / applied), save (no tree /
        written / write refused) and load (read refused / blob refused with
        the previous tree kept / applied);
      * fault, syncinfo (all three tables, the empty row, waiters, the four
        tables that stop on an unreadable row, and the semaphore event
        ledger), log (level listing, change, out-of-range refusal, console /
        fifo / sync counters), rt (no cycle counter, a full report with both
        counter and clock-source states, a row with no sample, late/skip and
        jitter, tickless, reset, run with a bad count / dead counter / refused
        wake / refused preempt with its reason code / success), sec (all five
        filters, an unknown filter, both verdicts), utest (list with the real
        output callback, a passing run, a failing run, usage) and pool (auto
        layout, fixed layout with a matched and an empty slot).

    Three real defects were found while pinning the branches, all of them
    inverted success tests, all fixed in the same commit:
      * cmd_syncinfo stopped at the first row it could read (the getters
        return 0 on success), so the object tables were never printed at all;
      * cmd_log treated a *successful* parse as bad usage (and therefore
        passed an uninitialised level through on a non-numeric argument);
      * cmd_rt "run" refused every valid sample count for the same reason.
    The first would have been read as "no sync objects exist" on a board.

    Honest limits:
      1) nothing here touches hardware: the task table, watchdog, crash
         journal, device tree, RT sampler, sync tables, security report, unit
         test runner and pool layout are all stubs, so what is proved is the
         decoding, formatting and exit code, not the readings themselves;
      2) the stack-usage "accounting compiled out" branch is not in this
         build (qemu_loongarch64 sets ARKRTOS_USE_STACK_USAGE=1);
      3) the fdt overlay / save / load paths run against an in-memory stub,
         not a real VFS or a real dtb, and the swap / waitmulti families are
         compiled out here;
      4) measured line coverage is 100.00% (633 of 633 lines), which is a
         statement about this file under this board configuration only.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("shelldiag-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    for case in CASES:
        name, test, mods, defs, markers, incs, _argv = _case(case)
        if name == "shelldiag":
            pick = (test, mods, defs, incs, markers)
            break
    if pick is None:
        fail("shelldiag-host", "case shelldiag missing from coverage_gate.CASES")
        return

    test, mods, defs, incs, markers = pick
    with TempDir() as td:
        exe = os.path.join(td, "shelldiag" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
        r = _run_check(cmd)
        if r.returncode != 0:
            err = (r.stderr or "").strip().splitlines() or [""]
            fail("shelldiag-host", "the case did not build: %s"
                 % [x for x in err if x.strip()][-1][:220])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt or "FAIL" in txt:
            fail("shelldiag-host", txt.strip()[-260:])
            return
        last = txt.strip().splitlines()[-1]

    ok("shelldiag-host",
       "arkrtos_shell_diag on the host (task/stack/guard/crash/audit/sched/dt/"
       "fdt/fault/syncinfo/log/rt/sec/utest/pool, every refusal path and exit "
       "code; fixed three inverted success tests in syncinfo/log/rt): %s" % last)

def check_shell_platform_host():
    """Shell platform layer: the nine functions ark-shell calls on the kernel.

    platform_arkrtos.c is the seam between the upstream console core and this
    kernel: it opens the console device, frames output with the console lock,
    diverts writes into a pipeline capture buffer, reads the tick and asks the
    board to reboot.  Every one of those is an ordering or pairing rule that a
    reader cannot see from the call site, so the case records exact bytes and
    exact call counts:

      * closed port: send / send_string / send_buf write nothing, take no
        lock, ask for no capture, and recv answers -1 without touching the
        device;
      * open: a failed open keeps the handle negative and the port retries on
        the next init (and again through shell_uart_open), a successful open
        is idempotent, and the baud rate is passed through unchanged;
      * console hold: the outer begin locks once, nested begins do not lock
        again, the inner end keeps the hold, the outer end releases it, an
        extra end does nothing, and a *refused* lock is never released;
      * capture: a consumed byte / string / buffer never reaches the UART,
        while a capture that refuses the bytes falls back to the device;
      * senders: one byte is one write, one string / buffer is one output
        unit with exactly one lock (none when the console is already held),
        a refused write is retried rather than dropped, and a device that
        never accepts the byte spends exactly SHELL_TX_SPIN_LIMIT = 200000
        retries per byte and then gives up -- still releasing its lock;
      * recv: an empty FIFO is -1, a byte is returned unsigned (0xFF is 255,
        not -1) and the device is asked for exactly one byte;
      * misc: tick passthrough, the mode record in all three states, the
        cleanup no-op, and reboot going to the board hook instead of AIRCR.

    Honest limits:
      1) the device table, console lock, capture buffer, tick and board reboot
         are stubs here: what is proved is the port's own ordering, not that a
         real UART sends, that the lock masks interrupts, or that a board
         resets;
      2) the AIRCR branch of platform_reboot is compiled out on this board
         (qemu_loongarch64 defines ARKRTOS_PLATFORM_REBOOT_PORT), and the
         self-spin loop after the reset request cannot be entered without a
         real reset -- the test longjmps out of the board hook, so that one
         line ("for(;;)") stays uncovered on purpose;
      3) measured line coverage is 98.91% (90 of 91 lines) under this board
         configuration; the single miss is the spin loop named above.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("shellplat-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    for case in CASES:
        name, test, mods, defs, markers, incs, _argv = _case(case)
        if name == "shellplat":
            pick = (test, mods, defs, incs, markers)
            break
    if pick is None:
        fail("shellplat-host", "case shellplat missing from coverage_gate.CASES")
        return

    test, mods, defs, incs, markers = pick
    with TempDir() as td:
        exe = os.path.join(td, "shellplat" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
        r = _run_check(cmd)
        if r.returncode != 0:
            err = (r.stderr or "").strip().splitlines() or [""]
            fail("shellplat-host", "the case did not build: %s"
                 % [x for x in err if x.strip()][-1][:220])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt or "FAIL" in txt:
            fail("shellplat-host", txt.strip()[-260:])
            return
        last = txt.strip().splitlines()[-1]

    ok("shellplat-host",
       "arkrtos_shell platform layer on the host (open / send / recv routing, "
       "console hold pairing, capture diversion, 200000-retry spin limit, tick "
       "and mode passthrough, reboot via the board hook): %s" % last)

def check_shell_fs_host():
    """Block devices, the littlefs facade, VFS commands and the mini applet.

    arkrtos_shell_fs.c is the largest shell file and the only place the four
    operator-facing families meet the storage layer, so a silent regression
    here costs data instead of printing an error.  The case links the real file
    against in-memory fixtures and walks every refusal path it can reach:

      * blk: list / probe / rd / wr / erase / test; a write only lands on
        erased (0xFF) bytes and an erase only on erase-unit boundaries, so a
        success cannot come from a write that did nothing;
      * fs: mount / unmount / format / info / ls / wr / rd / put / delete-file
        / test / err, including the unformatted-volume hint, the chunked
        upload's ACK/NAK handshake, and a "test" whose second read (after the
        remount) is made to fail;
      * vfs: mount / umount / ls (-l, -a, file argument, and pseudo rows when
        stat fails) / cat (file and stdin, with non-printables escaped as \\xNN)
        / delete-file / mkdir / rmdir / mv / cp / touch / chmod / chown /
        stat, plus path completion at every branch;
      * mini: run / stop / count / limit / forget / swap / autostart and list;
      * pwd / cd / df.

    Honest limits:
      1) every backend is a stub with a programmable return code: what is
         proved is the command layer's ordering and wording, not that a real
         chip, littlefs volume or VFS exists;
      2) the fixture deliberately keeps the real constraints that let "ok"
         mean something (erased-before-write, unit-aligned erase, read-back in
         blk test, remount-before-second-read in fs test);
      3) measured line coverage is 99.4% (1157 of 1164 lines).  The 7 misses
         are unreachable defence branches, named in the test's header: the
         completion "would not fit" guard and the three mini "path too long"
         branches (the shell can never hand over a long enough argument);
      4) the board configuration is STM32F427, where MINIAPP is on.  The
         qemu_loongarch64 board turns MINIAPP off, which drops cmd_mini from
         the link, so this case must not use that include path.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("shellfs-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    for case in CASES:
        name, test, mods, defs, markers, incs, _argv = _case(case)
        if name == "shellfs":
            pick = (test, mods, defs, incs, markers)
            break
    if pick is None:
        fail("shellfs-host", "case shellfs missing from coverage_gate.CASES")
        return

    test, mods, defs, incs, markers = pick
    with TempDir() as td:
        exe = os.path.join(td, "shellfs" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
        r = _run_check(cmd)
        if r.returncode != 0:
            err = (r.stderr or "").strip().splitlines() or [""]
            fail("shellfs-host", "the case did not build: %s"
                 % [x for x in err if x.strip()][-1][:220])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt or "FAIL" in txt:
            fail("shellfs-host", txt.strip()[-260:])
            return
        last = txt.strip().splitlines()[-1]

    ok("shellfs-host",
       "block device / littlefs facade / VFS commands / mini / pwd-cd-df on "
       "the host, refusal paths included: %s" % last)

def check_drvblk_host():
    """Board block device registration: the two thin adapters over storage.

    board/stm32f427/drvblk.c is the only seam between everything above (the
    loader, the file system, the shell) and the two non-volatile backends this
    board has.  It is small, but a wrong descriptor field or an address slip is
    silent: the pool would read from the wrong address, or the NOR would answer
    a size it does not have.  This case links the real file and walks:

      * registration: int0 and nor0 are offered in order, every descriptor
        field matches the contract (name, size, erase unit, bringup, size_fn,
        the three IO pointers), and calling init touches neither flash nor SPI;
      * a refused registration returns -1 without short-circuiting the other
        backend (the second descriptor is still offered);
      * int0: read is a memcpy from IMAGE_POOL_BASE + off; write and erase are
        translated to the same absolute address before reaching the port layer,
        length and buffer pass through unchanged, and a port failure comes back
        unchanged;
      * nor0: bringup / size_fn / read / write / erase are pure forwards, the
        capacity is asked of the chip (the descriptor size is 0 by design), and
        a driver failure comes back unchanged.

    Honest limits:
      1) the port layer (arkrtos_port_flash_*) and drvnor are stubs with a
         programmable return code: what is proved is what this layer hands to
         whom, not that a real flash controller or SPI part works;
      2) real erase/program timing and a power-loss window are not observable
         here;
      3) the internal pool is really mapped at IMAGE_POOL_BASE (VirtualAlloc on
         Windows, MAP_FIXED mmap on POSIX) -- a mapping failure is a hard FAIL,
         never a skip, because skipping would run the memcpy assertions on an
         illegal address and read like a pass;
      4) measured line coverage is 100% (14 of 14 lines) under this board
         configuration.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("drvblk-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    for case in CASES:
        name, test, mods, defs, markers, incs, _argv = _case(case)
        if name == "drvblk":
            pick = (test, mods, defs, incs, markers)
            break
    if pick is None:
        fail("drvblk-host", "case drvblk missing from coverage_gate.CASES")
        return

    test, mods, defs, incs, markers = pick
    with TempDir() as td:
        exe = os.path.join(td, "drvblk" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
        r = _run_check(cmd)
        if r.returncode != 0:
            err = (r.stderr or "").strip().splitlines() or [""]
            fail("drvblk-host", "the case did not build: %s"
                 % [x for x in err if x.strip()][-1][:220])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt or "FAIL" in txt:
            fail("drvblk-host", txt.strip()[-260:])
            return
        last = txt.strip().splitlines()[-1]

    ok("drvblk-host",
       "board block device registration on the host (two descriptor contracts, "
       "int0 address arithmetic, nor0 pure forwarding, refusal paths): %s" % last)

def check_shell_core_host():
    """Console core: command registry, console task loop and task release.

    arkrtos_shell.c is the only place the kernel command table is written, so
    a mistake here is either a silent overwrite of g_cmd_table or a command
    that exists but is unreachable.  This case links the real file against
    stub ark_shell_* / arkrtos_task_* hooks and stub handlers, and asserts on
    280 checks:
      * the nine name tables return the exact wording for every enum value and
        "?" / "unknown" / "-" for anything else (no other shell case reads
        them: they all stub these out);
      * parse_u32 accepts "0" and "123" and rejects NULL, "" and non-digits
        without touching the output;
      * register_kernel_commands() writes the shipped 57-entry registry in
        order -- name, handler pointer, help text and max_args each compared --
        then a NULL sentinel, with g_cmd_count equal to the entries written;
      * the table-full guard runs before the first write: from slot 31 the
        50-slot bound lands on 81 > ARK_SHELL_MAX_COMMANDS, so it returns -1,
        leaves the table and g_cmd_count untouched, and the console task says
        "command table full" rather than overwriting memory;
      * arkrtos_shell_init() passes entry, stack base, stack size,
        SHELL_TASK_PRIORITY and SHELL_TASK_PERIOD_MS to arkrtos_task_register()
        unchanged and returns its rc;
      * the console loop drains at most SHELL_RX_DRAIN_MAX bytes per round,
        stops early on an empty UART, goes round again, yields for
        SHELL_TASK_PERIOD_MS, and drops an over-long line (clears the editor,
        resets the state machine, says so) instead of poisoning every later
        command;
      * arkrtos_shell_release_task() refuses task id 0, drops exactly the
        entries owned by the requested task and returns the count.

    Honest limits:
      1) no real serial port, shell state machine or scheduler is involved:
         every ark_shell_* and arkrtos_task_* entry point is a stub;
      2) every one of the 57 handlers is a stub too, so this proves the
         registration (name / pointer / help / max_args), not the behaviour
         of the commands;
      3) the locked-console path (ARKRTOS_USE_SHELL_AUTH), board commands
         (ARKRTOS_SHELL_BOARD_CMDS) and the arkvim / arkc / ark make
         registration hooks are all compiled out in this build, and the
         SHELL_ENABLE==0 fallback stubs at the end of the file are the branch
         this build does not take.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("shellcore-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    for case in CASES:
        name, test, mods, defs, markers, incs, _argv = _case(case)
        if name == "shellcore":
            pick = (test, mods, defs, incs, markers)
            break
    if pick is None:
        fail("shellcore-host", "case shellcore missing from coverage_gate.CASES")
        return

    test, mods, defs, incs, markers = pick
    with TempDir() as td:
        exe = os.path.join(td, "shellcore" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
        r = _run_check(cmd)
        if r.returncode != 0:
            err = (r.stderr or "").strip().splitlines() or [""]
            fail("shellcore-host", "the case did not build: %s"
                 % [x for x in err if x.strip()][-1][:220])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt or "FAIL" in txt:
            fail("shellcore-host", txt.strip()[-260:])
            return
        last = txt.strip().splitlines()[-1]

    ok("shellcore-host",
       "arkrtos_shell on the host (57-entry registry name/handler/help/max_args "
       "/ table-full guard before first write / task register args / RX drain "
       "budget and over-long line discard / release_task ownership): %s" % last)

def check_modld():
    """VxWorks-style downloadable modules (ARKRTOS_USE_MODLOAD) must be wired.

    Same false-green shape as elf: the whole of arkrtos_modld.c /
    arkrtos_exports.c sits inside #if (ARKRTOS_USE_MODLOAD == 1), and the
    expensive failure is not "does it compile" but "does the demo .o only
    reference symbols the export table really carries" -- an export missing
    from the whitelist is ld rc=-6 on the board and green everywhere else.
    tools/modld_gate.py compiles both TUs with the switch on, checks the
    preprocessed result is not an empty TU, compiles the example module and
    reconciles every undefined symbol against s_exports, and pins the
    guards (ET_REL check, bounded reloc read/write, unknown-type report,
    ARKRTOS_MOD_AREA_SIZE #error) plus the Kconfig/board wiring, and requires
    the RV32 and LA32 toolchains to REJECT the switch (the loader reads ELF64
    RELA/symtab field widths that do not fit in a 32-bit long).
    Not covered: runtime load/reloc/unld -- those are QEMU readings, see
    docs/MMU平台可下载模块-VxWorks式.md.
    """
    r = _run_check([sys.executable, os.path.join("tools", "modld_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "MODLD-GATE OK" in txt:
        ok("modld", "default off + LA64 ON compiles + export table covers the demo .o"
                    " + RV32/LA32 rejected by the 64-bit guard")
    else:
        fail("modld", txt[-260:])

def check_features_switches():
    """Every gate in arkrtos_features.h must be declared in Kconfig.

    features.h is the #if landing point; Kconfig is what *decides* the value
    (docs/Kconfig配置系统.md section 7).  A gate that lives only in features.h
    is invisible to menuconfig and cannot be set from a defconfig, so it
    silently stays at its #ifndef default.  That is exactly how
    ARKRTOS_USE_UTEST / _SOFTBUS / _PROCFS / _MEMPOOL / _LWIP / HW_SAFE and the
    two MiniApp knobs slipped through, so it is a gate failure now rather than
    a surprise later.
    """
    src = read(os.path.join(ROOT, "kernelsrc/include/arkrtos_features.h"))
    gates = set(re.findall(r"^#ifndef[ \t]+(ARKRTOS_[A-Z0-9_]+)",
                           src.decode("utf-8", "replace"), re.M))
    syms = set()
    kfiles = [os.path.join(ROOT, "Kconfig")]
    kdir = os.path.join(ROOT, "config/kconfig")
    if os.path.isdir(kdir):
        kfiles += [os.path.join(kdir, n) for n in sorted(os.listdir(kdir))]
    pat = re.compile(r"^\s*config\s+(ARKRTOS_[A-Z0-9_]+)", re.M)
    for f in kfiles:
        if os.path.isfile(f):
            with open(f, encoding="utf-8", errors="replace") as fh:
                syms.update(pat.findall(fh.read()))
    missing = sorted(gates - syms)
    if missing:
        fail("features-switches", "gate(s) absent from Kconfig: %s"
             % ", ".join(missing))
    else:
        ok("features-switches", "%d gate(s) all declared in Kconfig" % len(gates))

def check_softbus_sync_header():
    """arkrtos_softbus_sync.h must name the same commit as VENDOR.md.

    The kernel prints that commit on the console (`bus info`); VENDOR.md records
    where the copy came from.  Both are in-tree and written by
    tools/sync_ark_softbus.py, so if they disagree the copy was updated without
    regenerating the header -- and the console would report a stale, still
    authoritative-looking version.  That is exactly what happened on
    2026-10-07: the component was at 18c6ccf while the console said 915c4ac.
    """
    vpath = os.path.join(ROOT, "kernelsrc/components/ark_softbus/VENDOR.md")
    hpath = os.path.join(ROOT, "kernelsrc/include/arkrtos_softbus_sync.h")
    if not (os.path.isfile(vpath) and os.path.isfile(hpath)):
        fail("softbus-sync", "VENDOR.md or arkrtos_softbus_sync.h missing")
        return
    vend = read(vpath).decode("utf-8", "replace")
    hdr = read(hpath).decode("utf-8", "replace")
    m = re.search(r"源 commit \|\s*`([0-9a-fA-F]{7,40})`", vend)
    h = re.search(r'ARKRTOS_SOFTBUS_COMMIT_FULL\s+"([0-9a-fA-F]+)"', hdr)
    if not m or not h:
        fail("softbus-sync", "cannot parse commit from VENDOR.md or header")
        return
    if m.group(1) != h.group(1):
        fail("softbus-sync", "header=%s VENDOR.md=%s"
             % (h.group(1)[:12], m.group(1)[:12]))
    else:
        ok("softbus-sync", "header commit matches VENDOR.md (%s)"
           % h.group(1)[:12])

def check_blk_host():
    """Block-device table: registration, guards, and the compiled-out build.

    arkrtos_blk.c had no host case at all until 2026-11-21. It is a pure table
    front end (four slots, name match, ready/desc/size/range/align/check
    guards) over a bringup/read/write/erase hook, so its whole decision surface
    is reachable on the host with a recording fake backend.

    Two configurations are built and run, because they are different code:
      * ON  (-DARKRTOS_USE_BLK=1): register/reject/find/init/size/guard/table-full
      * OFF (-DARKRTOS_USE_BLK=0): every entry point must answer
        ERR_DISABLED / 0 / NULL and never touch the backend -- that is the
        "off means zero footprint, not just unused" claim, and it is a separate
        #else branch that the ON build never compiles.

    Read it for what it is: a fake backend on the host. Media timing, erase
    timeouts, DMA and power-fail windows are NOT covered; only a board proves
    those. A missing host compiler is a FAIL, not a skip.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("blk-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    want = (("blk", "BLK-HOST OK"), ("blk-off", "BLK-HOST OFF OK"))
    picked = {}
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name in [w[0] for w in want]:
            picked[name] = (test, mods, defs, incs)
    for name, _marker in want:
        if name not in picked:
            fail("blk-host", "case %s missing from coverage_gate.CASES" % name)
            return

    lines = []
    with TempDir() as td:
        for name, marker in want:
            test, mods, defs, incs = picked[name]
            exe = os.path.join(td, name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
                fail("blk-host", "%s did not build: %s" % (name, tail[:200]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 or marker not in txt:
                fail("blk-host", "%s failed: %s" % (name, txt.strip()[:220]))
                return
            lines.append(txt.strip().splitlines()[-1])
    ok("blk-host", "device table + guards on host (%s / %s)"
       % (lines[0], lines[1]))

def check_dev_host():
    """Device framework: slots, refcount, guards, and the ownership ledger.

    arkrtos_dev.c had no host case at all until 2026-11-21. It is a pure table
    front end (name -> slot -> instance) plus a per-slot refcount, so its whole
    decision surface is reachable on the host with a recording fake driver.

    Two configurations are built and run, because they are different code:
      * HW_SAFE=1: every open/close also writes the App-crash ownership ledger
        (claim/release), ctrl_obj() looks a device up by instance pointer, and
        unregister drops the ledger entry.
      * HW_SAFE=0: exactly the pre-ledger behaviour -- that half is never
        compiled in the ON build, so it needs its own run.

    Both builds also pin the tombstone semantics (unregister clears the slot in
    place so an already-issued handle cannot slide onto another device) and the
    refcount ceiling. Read it for what it is: fake driver on the host. Real
    hardware timing and concurrent open/close from two tasks are NOT covered; a
    board and the real scheduler prove those. A missing host compiler is a
    FAIL, not a skip.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("dev-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    want = (("dev", "DEV-HOST OK"), ("dev-nosafe", "DEV-HOST NOSAFE OK"))
    picked = {}
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name in [w[0] for w in want]:
            picked[name] = (test, mods, defs, incs)
    for name, _marker in want:
        if name not in picked:
            fail("dev-host", "case %s missing from coverage_gate.CASES" % name)
            return

    lines = []
    with TempDir() as td:
        for name, marker in want:
            test, mods, defs, incs = picked[name]
            exe = os.path.join(td, name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
                fail("dev-host", "%s did not build: %s" % (name, tail[:200]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 or marker not in txt:
                fail("dev-host", "%s failed: %s" % (name, txt.strip()[:220]))
                return
            lines.append(txt.strip().splitlines()[-1])
    ok("dev-host", "device framework on host (%s / %s)"
       % (lines[0], lines[1]))


def check_event_host():
    """Event objects: auto-reset flag, waiter queue, and task teardown.

    arkrtos_event.c had no host case until 2026-11-21. It is a small table of
    objects plus a waiter queue and one auto-reset flag, and its expensive
    failure is silent: a task that registers but never reaches the blocking
    path (or the reverse) is simply never woken by set -- a permanent hang that
    no board log would explain.

    Two configurations are built and run, because they are different code:
      * WAITMULTI=0: the default build; ready_internal() is not compiled in.
      * WAITMULTI=1: adds the read-only "is this event set" probe used by
        multi-object waits, which must not consume the flag.

    The blocking path is driven by a programmable host stub, so what is proven
    is the state machine and the accounting (register/remove, handoff bit,
    timeout, deletion, spurious wake, full queue, creator teardown). Real
    scheduler timing and two tasks truly blocking at once are NOT covered; a
    board proves those. A missing host compiler is a FAIL, not a skip.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("event-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    want = (("event", "EVENT-HOST OK"),
            ("event-multi", "EVENT-HOST MULTI OK"))
    picked = {}
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name in [w[0] for w in want]:
            picked[name] = (test, mods, defs, incs)
    for name, _marker in want:
        if name not in picked:
            fail("event-host", "case %s missing from coverage_gate.CASES" % name)
            return

    lines = []
    with TempDir() as td:
        for name, marker in want:
            test, mods, defs, incs = picked[name]
            exe = os.path.join(td, name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
                fail("event-host", "%s did not build: %s" % (name, tail[:200]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 or marker not in txt:
                fail("event-host", "%s failed: %s" % (name, txt.strip()[:220]))
                return
            lines.append(txt.strip().splitlines()[-1])
    ok("event-host", "event objects on host (%s / %s)"
       % (lines[0], lines[1]))

def check_timer_host():
    """Software timers: object table, tick countdown, and the service task body.

    arkrtos_timer.c had no host case until 2026-11-21. The tick handler only
    counts down and sets a flag; the callback runs from a dedicated service
    task. The expensive failure is the hand-off between the two: a service
    task that is woken when it is not waiting (or not woken when it is)
    leaves callbacks either never running or running over a live task.

    Three configurations are built and run, because they are three different
    bodies of code:
      * USE_TIMER=1: the default build; the service loop runs on target, so
        the host case drives it a bounded number of passes through
        ARKRTOS_TIMER_HOST_TEST's budget (default build: this symbol and the
        whole branch do not exist -- the object file is byte-identical to the
        pre-hook file except the recorded source filename).
      * USE_TIMER=1 + USE_TICKLESS=1: adds next_ticks(), the "when is the
        nearest deadline" query used by tickless idle.
      * USE_TIMER=0: every entry point must refuse with -1 instead of
        pretending to create a timer whose callback can never run.

    What is proven is the state machine and the accounting (slot allocation,
    handle gates, ms-to-tick conversion including a 32-bit multiply overflow
    floored to one tick, countdown/reload, hand-off to the service task, the
    callback snapshot rules, self stop/delete from inside a callback). Real
    tick timing and a real scheduler are NOT covered; a board proves those.
    A missing host compiler is a FAIL, not a skip.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("timer-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    want = (("timer", "TIMER-HOST OK"),
            ("timer-tickless", "TIMER-HOST TL OK"),
            ("timer-off", "TIMER-HOST OFF OK"))
    names = [w[0] for w in want]
    picked = {}
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name in names:
            picked[name] = (test, mods, defs, incs)
    for name, _marker in want:
        if name not in picked:
            fail("timer-host", "case %s missing from coverage_gate.CASES" % name)
            return

    lines = []
    with TempDir() as td:
        for name, marker in want:
            test, mods, defs, incs = picked[name]
            exe = os.path.join(td, name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
                fail("timer-host", "%s did not build: %s" % (name, tail[:200]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 or marker not in txt:
                fail("timer-host", "%s failed: %s" % (name, txt.strip()[:220]))
                return
            lines.append(txt.strip().splitlines()[-1])
    ok("timer-host", "software timers on host (%s / %s / %s)"
       % (lines[0], lines[1], lines[2]))

def check_cfg_host():
    """Task table and TCB construction: arkrtos_task_register itself.

    Every task in the system is born here -- the application main, the built-in
    service tasks and the loader's App start all funnel through this one
    function -- so a wrong slot decision is not a local bug. The expensive
    mistakes are quiet ones:
      * a released slot (INVALID) and a slot waiting for recovery (INVALID +
        recover_pending) look almost the same; reusing the second one silently
        cancels a pending fault recovery;
      * the parameter gates run after slot allocation, so "did a failed call
        consume a slot" has to be answered by observation, not by reading;
      * the stack-top alignment mask must be 64-bit wide (the address was
        widened with the LA64 work); a 32-bit mask corrupts the high bits.

    Three configurations are built and run:
      * default (STACK_USAGE=1, STACK_GUARD=0): the shipping shape; the stack
        fill/peak-low path is live.
      * STACK_GUARD=1: the two arming lines only exist in this build, and they
        are what let the guard check judge a task's stack bottom at all.
      * STACK_USAGE=0: the fill and peak-low lines disappear; the case proves
        the file still builds and does not touch the stack when the feature is
        off.
    A missing host compiler is a FAIL, not a skip.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("cfg-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    want = (("cfg", "CFG-HOST OK"),
            ("cfg-guard", "CFG-HOST GUARD OK"),
            ("cfg-nousage", "CFG-HOST NOUSAGE OK"))
    names = [w[0] for w in want]
    picked = {}
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name in names:
            picked[name] = (test, mods, defs, incs)
    for name, _marker in want:
        if name not in picked:
            fail("cfg-host", "case %s missing from coverage_gate.CASES" % name)
            return

    lines = []
    with TempDir() as td:
        for name, marker in want:
            test, mods, defs, incs = picked[name]
            exe = os.path.join(td, name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
                fail("cfg-host", "%s did not build: %s" % (name, tail[:200]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 or marker not in txt:
                fail("cfg-host", "%s failed: %s" % (name, txt.strip()[:220]))
                return
            lines.append(txt.strip().splitlines()[-1])
    ok("cfg-host", "task table on host (%s / %s / %s)"
       % (lines[0], lines[1], lines[2]))

def check_export_host():
    """The kernel symbol export table: what a downloaded module may call.

    arkrtos_exports.c had no host case until 2026-11-21. It holds two things
    that both fail quietly:
      * the name lookup -- a prefix or case-insensitive match would hand a
        module the address of a symbol it did not name, and the normal path
        (module spells the name right) would still work, so nothing shows;
      * the module printf -- it is the only output channel a module has, so
        a null %s, a trailing %, and an oversized line must all have a
        defined answer instead of reading past the buffer.

    Two configurations are built and run:
      * MODLOAD=1: the whole module. The table's addresses are the stubs in
        the case, which is exactly what the assertions check (name -> which
        function), and the console is captured so the formatted bytes are
        compared, not merely the return value.
      * MODLOAD=0: the file is an empty object with zero symbols. The case
        proves the switch is really off and that the header macros answer
        "no such thing" without dragging in any undefined reference.
    A missing host compiler is a FAIL, not a skip.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("export-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    want = (("export", "EXPORT-HOST OK"),
            ("export-off", "EXPORT-HOST OFF OK"))
    names = [w[0] for w in want]
    picked = {}
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name in names:
            picked[name] = (test, mods, defs, incs)
    for name, _marker in want:
        if name not in picked:
            fail("export-host", "case %s missing from coverage_gate.CASES" % name)
            return

    lines = []
    with TempDir() as td:
        for name, marker in want:
            test, mods, defs, incs = picked[name]
            exe = os.path.join(td, name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
                fail("export-host", "%s did not build: %s" % (name, tail[:200]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 or marker not in txt:
                fail("export-host", "%s failed: %s" % (name, txt.strip()[:220]))
                return
            lines.append(txt.strip().splitlines()[-1])
    ok("export-host", "kernel export table on host (%s / %s)"
       % (lines[0], lines[1]))

def check_init_host():
    """Kernel module init order: arkrtos_kernel_module_init itself.

    The function does nothing but call other modules' init in a fixed order, and
    that order is the whole contract -- it is the single authoritative startup
    sequence, so an application main that inits modules by hand is a second
    startup path that will drift. Two orderings have consequences that show up
    far from this file: SMP must be brought up before the scheduler starts
    (a late secondary core finds a scheduler it does not own), and the timer
    service task is registered by arkrtos_task_register, so it has to sit after
    cfg_load and before the scheduler starts.

    Two configurations are built and run:
      * default (SBUF / ESET / SMP off): the shipping shape for those switches.
      * SBUF+ESET+SMP on: the three extra steps must land in the documented
        positions, not at the end of the list.
    The case compares the recorded name sequence item by item, and separately
    counts the calls, so "someone was added twice" and "someone was dropped"
    both go red.

    It also pins that there is no re-entry guard: two calls run the sequence
    twice. That is a reading, not a promise -- if a guard is added the case must
    be updated in the same commit, which is exactly the point of pinning it.

    A missing host compiler is a FAIL, not a skip.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("init-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    want = (("init", "INIT-HOST OK"),
            ("init-on", "INIT-HOST ON OK"))
    names = [w[0] for w in want]
    picked = {}
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name in names:
            picked[name] = (test, mods, defs, incs)
    for name, _marker in want:
        if name not in picked:
            fail("init-host", "case %s missing from coverage_gate.CASES" % name)
            return

    lines = []
    with TempDir() as td:
        for name, marker in want:
            test, mods, defs, incs = picked[name]
            exe = os.path.join(td, name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
                fail("init-host", "%s did not build: %s" % (name, tail[:200]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 or marker not in txt:
                fail("init-host", "%s failed: %s" % (name, txt.strip()[:220]))
                return
            lines.append(txt.strip().splitlines()[-1])
    ok("init-host", "kernel init order on host (%s / %s)"
       % (lines[0], lines[1]))

def check_log_host():
    """The kernel log: level filter, dmesg-shaped prefix, console lock, ring.

    arkrtos_log.c is the only channel the system has left once something else
    has already gone wrong, so every way it can be quiet is a real failure and
    none of them shows up as an error code at the call site:

      * the level filter written as a closed interval -- ERROR gets muted along
        with DEBUG, and what the operator sees is simply fewer lines;
      * the prefix -- the timestamp / syslog priority / reset pair is what log
        scrapers parse, so one missing space or a colour leaking into the body
        changes the format for every consumer;
      * the console lock -- it exists so a line is never spliced, but a writer
        that cannot take it must still write (dropping output is harder to
        diagnose than an occasional join). "Spin the whole budget, then give up
        and count it" is therefore a pinned behaviour, not an implementation
        detail;
      * the per-byte transmit budget -- a full pipe is waited out, and only a
        byte that never left inside the budget is counted as dropped;
      * the dmesg ring -- overwrite the oldest, and dump oldest-first after the
        buffer has wrapped.

    Three configurations are built and run:
      * default (LOG=1, RING=1) with the ring shrunk to 64 bytes so the wrap is
        reached by a handful of lines;
      * RING=0: the ring block is compiled out and capacity/used/dump must
        answer 0 rather than pretend to be an empty ring;
      * LOG=0: the whole module becomes the "always refuse" stub.
    A missing host compiler is a FAIL, not a skip.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("log-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    want = (("log", "LOG-HOST OK"),
            ("log-noring", "LOG-HOST NORING OK"),
            ("log-off", "LOG-HOST OFF OK"))
    names = [w[0] for w in want]
    picked = {}
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name in names:
            picked[name] = (test, mods, defs, incs)
    for name, _marker in want:
        if name not in picked:
            fail("log-host", "case %s missing from coverage_gate.CASES" % name)
            return

    lines = []
    with TempDir() as td:
        for name, marker in want:
            test, mods, defs, incs = picked[name]
            exe = os.path.join(td, name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
                fail("log-host", "%s did not build: %s" % (name, tail[:200]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 or marker not in txt:
                fail("log-host", "%s failed: %s" % (name, txt.strip()[:220]))
                return
            lines.append(txt.strip().splitlines()[-1])
    ok("log-host", "kernel log on host (%s / %s / %s)"
       % (lines[0], lines[1], lines[2]))

def check_ptable_host():
    """The partition table: layout init, slot alloc/free, two mini allocators.

    arkrtos_ptable.c is what the loader stands on: it decides which address an
    image or a driver may occupy, and every way of getting that wrong hands out
    memory that is already in use. The failures are quiet ones:

      * an allocation that reuses a range already recorded as in use -- the two
        owners then corrupt each other and nothing reports an error;
      * a gap search that only looks at the aligned landing spot and gives up
        instead of falling back to the gap start, or one that walks past the
        pool end;
      * the two mini-app blocks (code + RAM, not tied to a slot) both handing
        out the same block, or a block freed with its slot still counted as
        used;
      * the layout header rewritten on every arkrtos_ptable_get() once the
        table lives in shared RAM (magic/version/hw_compat must survive).

    The module works on absolute addresses (IMAGE_POOL_BASE, SLOT_RAM_BASE,
    SHARE_RAM_BASE from config/arkrtos_partition.h), so the host test reserves
    both the RAM window and the flash window for real with VirtualAlloc/mmap
    before any assertion, and returns non-zero (FAIL) when the mapping fails
    rather than running assertions against a null pointer.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("ptable-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    picked = None
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name == "ptable":
            picked = (test, mods, defs, incs)
            break
    if picked is None:
        fail("ptable-host", "case ptable missing from coverage_gate.CASES")
        return

    test, mods, defs, incs = picked
    with TempDir() as td:
        exe = os.path.join(td, "ptable" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
        r = _run_check(cmd)
        if r.returncode != 0:
            tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
            fail("ptable-host", "ptable did not build: %s" % tail[:200])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or "PTABLE-HOST OK" not in txt:
            fail("ptable-host", "ptable failed: %s" % txt.strip()[:220])
            return
        ok("ptable-host",
           "partition table + slot allocator + two mini allocators (%s)"
           % txt.strip().splitlines()[-1])

def check_layout_host():
    """The on-device layout record: validate, scan, adopt, publish, write.

    arkrtos_layout.c is what turns a 512-byte record in the config region into
    the runtime layout. The quiet failures it guards against:

      * a record with a good magic but a bad CRC or bad version being adopted --
        the board then runs on garbage slot addresses;
      * a slot table that overlaps, runs past the pool end, or leaves the first
        8 slots unaligned, accepted because the header alone was checked;
      * scan() picking the newest record by overwrite order instead of by the
        append index, so a CRC-bad tail hides the last good record;
      * a reason code that is always 0 because scan() already skipped the bad
        record and init()'s failure branch is unreachable -- the console then
        cannot tell why a board fell back to defaults;
      * write() publishing the new record but never applying the runtime knobs,
        so the log level only changes after a reboot.

    The test maps [CONFIG_BASE, +CONFIG_SIZE) for real, drives both flash ports
    with recording doubles, and links a self-checked CRC32 (zlib known answers)
    instead of pulling in the whole loader. Returns non-zero (FAIL) when the
    mapping fails rather than asserting against a null pointer.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("layout-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    picked = None
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name == "layout":
            picked = (test, mods, defs, incs)
            break
    if picked is None:
        fail("layout-host", "case layout missing from coverage_gate.CASES")
        return

    test, mods, defs, incs = picked
    with TempDir() as td:
        exe = os.path.join(td, "layout" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
        r = _run_check(cmd)
        if r.returncode != 0:
            tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
            fail("layout-host", "layout did not build: %s" % tail[:200])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or "LAYOUT-HOST OK" not in txt:
            fail("layout-host", "layout failed: %s" % txt.strip()[:220])
            return
        ok("layout-host",
           "layout record validate/scan/adopt/write + live reason code (%s)"
           % txt.strip().splitlines()[-1])

def check_crash_host():
    """The cross-reset crash journal, on host, at both grades.

    arkrtos_crash.c is the half of fault handling that has to outlive the reset
    it is reacting to: which slot keeps crashing, why, and whether it has
    already been held out of autostart. The failures it exists for are quiet:

      * identity filed under a slot index instead of the image address, so the
        re-scan after the table moves charges a newcomer with someone else's
        crashes (or loses the history of the image that is still installed);
      * the "nothing claims this address" sweep reporting a change for entries
        that were already blank -- then every boot commits a new sequence
        number for nothing (found by this test, fixed in the module);
      * the hold not being re-applied on every boot, so an operator's manual
        "app start" from the previous session survives the reset;
      * a limit of 0 read as "unlimited" instead of "use the compiled default",
        which would silently switch the policy off;
      * the OFF grade still touching the partition table, or still reporting a
        count / a disable for a fault it never recorded.

    Two builds, because the module is two blocks of code: ON (the real journal,
    which the test links and drives directly) and OFF (every entry point
    compiled to a constant, no ledger at all). The test supplies partition
    table / tick / critical-section doubles and compiles the logging macros out
    with -DARKRTOS_LOG_LEVEL=0.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("crash-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    picks = {}
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name in ("crash", "crash-off"):
            picks[name] = (test, mods, defs, incs)
    for need in ("crash", "crash-off"):
        if need not in picks:
            fail("crash-host", "case %s missing from coverage_gate.CASES" % need)
            return

    results = []
    for case_name, marker in (("crash", "CRASH-HOST OK"),
                              ("crash-off", "CRASH-HOST OFF OK")):
        test, mods, defs, incs = picks[case_name]
        with TempDir() as td:
            exe = os.path.join(td, case_name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
                fail("crash-host",
                     "%s did not build: %s" % (case_name, tail[:200]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 or marker not in txt:
                fail("crash-host",
                     "%s failed: %s" % (case_name, txt.strip()[:220]))
                return
            results.append(txt.strip().splitlines()[-1])
    ok("crash-host",
       "journal rebind/sweep/publish/mini + OFF constants: %s | %s"
       % (results[0], results[1]))

def check_apprun_host():
    """Run-on-board: a bare arkc output loaded into a fixed window, on host.

    arkrtos_apprun.c is the last step of the "compile then execute" chain: it
    takes the bytes that "cc -o" wrote, puts them into one borrowed window and
    starts a non-privileged task at the vector table's Reset_Handler. There is
    no relocation anywhere -- the program was linked for those addresses, so
    every check is about refusing, loudly, anything that is not exactly that.
    The quiet failures it guards against:

      * a first-fit block that is not the window base being accepted, so a
        program linked for one address runs at another (jumps into nothing);
      * an image bigger than the code window being truncated at the window edge,
        which cuts an instruction in half and makes the rest random;
      * "exactly the window size" not being told apart from "one byte more":
        the extra probe read is the only thing that separates them;
      * taking the entry from the first half-word of the file instead of the
        vector table -- string literals are emitted before every function, so
        the first bytes of a real program are characters, not instructions;
      * the state word not being cleared when the task cannot be created, so
        the next attempt answers BUSY forever, and the window leaks;
      * the exit hook returning the window for a task that is not ours.

    The module reads the window by absolute address, so the test maps
    [SLOT_RAM_BASE, +SLOT_RAM_TOTAL) for real and bails out (non-zero, FAIL)
    when the mapping cannot be taken. The window constants live in the h743
    board header only; they are injected on the command line and the test
    asserts their shape against that mapped range. OFF is an empty
    translation unit, so coverage comes from this grade alone.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("apprun-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    picked = None
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name == "apprun":
            picked = (test, mods, defs, incs)
            break
    if picked is None:
        fail("apprun-host", "case apprun missing from coverage_gate.CASES")
        return

    test, mods, defs, incs = picked
    with TempDir() as td:
        exe = os.path.join(td, "apprun" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
        r = _run_check(cmd)
        if r.returncode != 0:
            tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
            fail("apprun-host", "apprun did not build: %s" % tail[:200])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or "APPRUN-HOST OK" not in txt:
            fail("apprun-host", "apprun failed: %s" % txt.strip()[:220])
            return
        ok("apprun-host",
           "window borrow + vector table + task/MPU + exit hook (%s)"
           % txt.strip().splitlines()[-1])

def check_flashapp_host():
    """Burn into intra-chip flash, verify, and boot from it, on host.

    arkrtos_flashapp.c is the only software step of "product goes back into
    flash and runs from there". It shares the data window with app run but
    takes its code from the flash app area, and everything it refuses is about
    that address contract:

      * a file that was linked for another layout (wrong initial SP, entry
        outside the area, entry past the payload, wrong arkc prologue) must be
        rejected BEFORE the area is erased -- a broken upload must not destroy
        the program that still works;
      * "program returned 0" is not proof: the area is read back byte for byte
        and a single flipped byte is VERIFY, not success;
      * metadata lives in the last flash word, so a half-written image reads as
        "nothing here" instead of looking runnable;
      * a first-fit data window that is not exactly the apprun data block is
        refused (the product is linked for that address);
      * write / erase / run are three separate single-instance gates: writing
        while running would erase the sector the CPU is fetching from;
      * the exit hook only returns the window for its own task, and still
        clears the state when the return itself fails.

    The module reads both the flash area and the data window by absolute
    address, so the test maps [FLASHAPP_BASE, +SIZE) and
    [SLOT_RAM_BASE, +SLOT_RAM_TOTAL) for real and bails out (non-zero, FAIL)
    when a mapping cannot be taken. Flash is modelled over the mapped memory
    (erase = fill 0xFF, write = memcpy) with knobs for per-call failures and
    post-write corruption. OFF is an empty translation unit, so coverage comes
    from this grade alone.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("flashapp-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    picked = None
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name == "flashapp":
            picked = (test, mods, defs, incs)
            break
    if picked is None:
        fail("flashapp-host", "case flashapp missing from coverage_gate.CASES")
        return

    test, mods, defs, incs = picked
    with TempDir() as td:
        exe = os.path.join(td, "flashapp" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
        r = _run_check(cmd)
        if r.returncode != 0:
            tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
            fail("flashapp-host", "flashapp did not build: %s" % tail[:200])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or "FLASHAPP-HOST OK" not in txt:
            fail("flashapp-host", "flashapp failed: %s" % txt.strip()[:220])
            return
        ok("flashapp-host",
           "burn + read-back verify + metadata + boot from flash (%s)"
           % txt.strip().splitlines()[-1])

def check_trace_host():
    """Kernel event trace facade over the mdk_trace component, on host.

    arkrtos_trace.c is what turns kernel call sites into component events, and
    what the console `trace` command reports. The failures that matter are:

      * an event that fires once too often or not at all: a timeline with an
        unexplained switch or a hole is worse than no timeline;
      * recording while the facade is disabled: the promise is that tracing
        does not change the thing it measures, so nothing may enter the ring;
      * a switch counter that grows on a rejected id, or is not reset by
        reset/init;
      * the console refusing a subcommand it should accept, or accepting a
        near-miss like "startx" as "start";
      * PendSV round-trip accounting dropping samples when the trace is off
        (it is deliberately measured outside the on/off gate).

    Four builds are checked, because the module has four compile-time faces:
    the SWD backend (compressed ring, readouts come from the control block),
    the BUFF backend (record ring, readouts from the component API), no
    backend at all, and USE_MDK_TRACE=0 where every entry is a no-op and the
    console command is refused.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("trace-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    wanted = ("trace-swd", "trace-buff", "trace-none", "trace-off")
    picks = {}
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name in wanted:
            picks[name] = (test, mods, defs, incs)
    for need in wanted:
        if need not in picks:
            fail("trace-host", "case %s missing from coverage_gate.CASES" % need)
            return

    results = []
    for case_name, marker in (("trace-swd", "TRACE-HOST OK"),
                              ("trace-buff", "TRACE-HOST OK"),
                              ("trace-none", "TRACE-HOST OK"),
                              ("trace-off", "TRACE-HOST OFF OK")):
        test, mods, defs, incs = picks[case_name]
        with TempDir() as td:
            exe = os.path.join(td, case_name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
                fail("trace-host",
                     "%s did not build: %s" % (case_name, tail[:200]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 or marker not in txt:
                fail("trace-host",
                     "%s failed: %s" % (case_name, txt.strip()[:220]))
                return
            results.append(txt.strip().splitlines()[-1])
    ok("trace-host",
       "events / switch counters / console over SWD + BUFF + none + off: %s | %s | %s | %s"
       % (results[0], results[1], results[2], results[3]))

def check_apppack_host():
    """Four-pass differential packer (arkrtos_apppack.c) on host.

    The packer decides which bytes of an image are absolute addresses and emits
    the relocation table the loader later replays. Getting it wrong is not a
    compile error, it is an image that boots and then faults at an unrelated
    address, so the checks pin the accounting rather than "it returned 0":

      * all four table kinds (word ROM / word RAM / MOVW+MOVT ROM / RAM) are
        really recognised, counted and encoded at the right offset with the
        right kind bits -- a kind that silently stays at zero would make the
        gate green while one class goes unverified;
      * the produced image passes the loader's own check_header, computed by
        the real arkrtos_loader.c (linked in, not re-implemented here), so the
        packer and the loader cannot drift apart without this failing;
      * the header CRC covers the table and the payload, and the payload is
        restored to the nominal form A (not D) before the header is filled;
      * every parameter gate and both "refuse rather than guess" exits
        (DIFF / ENTRY / VERIFY / SIZE) fire where they should.

    Not checked here: the real four-pass link on the toolchain, the actual
    instruction stream a compiler emits, and patching at the load address --
    those need the board.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("apppack-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    link = []
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name == "apppack":
            extra = case[5] if len(case) > 5 else {}
            pick = (_markers, test, mods, defs, incs)
            link = list(extra.get("link", []))
            break
    if pick is None:
        fail("apppack-host", "case apppack missing from coverage_gate.CASES")
        return

    markers, test, mods, defs, incs = pick
    with TempDir() as td:
        exe = os.path.join(td, "apppack" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods + link)
        r = _run_check(cmd)
        if r.returncode != 0:
            tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
            fail("apppack-host", "did not build: %s" % tail[:200])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt:
            fail("apppack-host", "failed: %s" % txt.strip()[:220])
            return
        last = txt.strip().splitlines()[-1]

    ok("apppack-host",
       "four relocation kinds / header + CRC / loader check_header / refusal exits: %s"
       % last)

def check_insthost_host():
    """In-kernel installer (arkrtos_installer.c) on host.

    The installer is the "the device installs itself" entrance: it hunts the
    image magic in a serial byte stream, decides whether the frame may land,
    and only then hands it to the loader. Its failures are quiet ones, so what
    is pinned here is the decision and the accounting, not "it returned 0":

      * byte-wise magic sync (stray bytes, a header split across passes, and the
        idle-limit pass proving already collected bytes are kept, not dropped);
      * every refusal decided after the header arrived (DUP / VERSION / other
        identity errors, plus a load failure) writes NAK 0x15, drains the frame
        tail that is already on the wire, and clears the buffered header -- skip
        the last step and the pump reports the same header every task period;
      * the autostart flag decides start-or-not, and only the slot just committed
        is started;
      * fixed-slot mode: one clear before listening, no frame at all when no slot
        is configured or the slot is not blank, and exactly one image per boot;
      * the file entrance: unmounted / directory / size / short read / bad magic
        / device-registration failure each return their own error, and the
        success path really pulls the payload through the file stream.

    Stubbed: device core, tasks, partition/layout table, loader, file system,
    fault and log. So this proves the installer's decisions; flash writing,
    relocation replay and CRC re-check are the loader's business (and the
    board's).
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("insthost-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    link = []
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name == "insthost":
            extra = case[5] if len(case) > 5 else {}
            pick = (_markers, test, mods, defs, incs)
            link = list(extra.get("link", []))
            break
    if pick is None:
        fail("insthost-host", "case insthost missing from coverage_gate.CASES")
        return

    markers, test, mods, defs, incs = pick
    with TempDir() as td:
        # Not "installer.exe": Windows treats that name as an installer and
        # refuses to start it without elevation (WinError 740).
        exe = os.path.join(td, "insthost" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods + link)
        r = _run_check(cmd)
        if r.returncode != 0:
            tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
            fail("insthost-host", "did not build: %s" % tail[:200])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt:
            fail("insthost-host", "failed: %s" % txt.strip()[:220])
            return
        last = txt.strip().splitlines()[-1]

    ok("insthost-host",
       "magic sync / refusal NAK + drain / autostart / fixed slot / file entrance: %s"
       % last)

def check_paging_host():
    """Page-table isolation backend (arkrtos_paging.c) on host.

    This is the MMU half of task isolation: it audits a task's declared
    windows, switches the active space, hands out dynamic aliases, and is the
    only path by which user-supplied addresses reach kernel code. Its failures
    are the quiet kind, so what is pinned here is the decision, not "it
    returned 0":

      * the audit surface is per-task and only rebuilt when a task really
        declares ROM (rom_start == 0 must leave the surface alone, not clear
        it), with ROM/RAMX/RAM classification decided by the slot window;
      * set_app refuses NULL, refuses a privileged task, refuses an index
        outside the task table, and only an accepted call reaches the port;
      * a dynamic span is refused for size 0, any non-4K alignment (size, va
        or pa) and anything over the 16-page map budget -- and a refusal must
        not consume budget;
      * a half-failed alias run leaves the page counter where it was, and a
        half-failed unmap still gives back the pages it did release;
      * user_copy orders its checks: bad argument (-1), no window (-2),
        out-of-span (-3, write direction only accepts the RAM window) and
        space mismatch (-4, decided before any window test), and the success
        path really moves bytes in both directions.

    The 20 port functions are stubs, so this proves paging's own decisions and
    accounting. It does NOT prove TLB/CAM behaviour, real page faults or user-
    mode containment -- those are the port's and the board's business, and
    only a QEMU run (counted as "on board", never as real hardware) plus a
    real LA64 board would show them.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("paging-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    pick = None
    link = []
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name == "paging":
            extra = case[5] if len(case) > 5 else {}
            pick = (_markers, test, mods, defs, incs)
            link = list(extra.get("link", []))
            break
    if pick is None:
        fail("paging-host", "case paging missing from coverage_gate.CASES")
        return

    markers, test, mods, defs, incs = pick
    with TempDir() as td:
        exe = os.path.join(td, "paginghost" + (".exe" if os.name == "nt" else ""))
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
               + defs + incs + COMMON_INC + ["-o", exe, test] + mods + link)
        r = _run_check(cmd)
        if r.returncode != 0:
            tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
            fail("paging-host", "did not build: %s" % tail[:200])
            return
        r = _run_check([exe])
        txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or markers[0] not in txt:
            fail("paging-host", "failed: %s" % txt.strip()[:220])
            return
        last = txt.strip().splitlines()[-1]

    ok("paging-host",
       "audit surface / space switch / dynamic span budget / user_copy order: %s"
       % last)

def check_hwsafe_host():
    """HW_SAFE on/off: who gets driven at the moment a task dies, and on what.

    arkrtos_hwsafe.c is the "last words" ledger. When an owner dies, the kernel
    must still drive that owner's device to a safe state, hand the hardware to
    the next owner, or deliberately drop the ledger -- and it must never report
    a failed drive as a success. The failures that matter here are quiet:

      * a driver that answers non-zero (hardware did NOT reach safe state) being
        counted with the ones that did;
      * a death that is only marked, but never run, leaving the device live;
      * the entry that gets moved into a removed slot never being processed;
      * the monotonic counters being read as per-scenario numbers.

    Two builds are checked: HW_SAFE=1 (the real code) and HW_SAFE=0, where the
    module is an empty translation unit and only the "OFF OK" marker exists.
    NOTE: the counters are monotonic across arkrtos_hwsafe_reset() by design,
    so the test asserts increments, not absolute values.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("hwsafe-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    picks = {}
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name in ("hwsafe", "hwsafe-off"):
            picks[name] = (test, mods, defs, incs)
    for need in ("hwsafe", "hwsafe-off"):
        if need not in picks:
            fail("hwsafe-host", "case %s missing from coverage_gate.CASES" % need)
            return

    results = []
    for case_name, marker in (("hwsafe", "HWSAFE-HOST OK"),
                              ("hwsafe-off", "HWSAFE-HOST OFF OK")):
        test, mods, defs, incs = picks[case_name]
        with TempDir() as td:
            exe = os.path.join(td, case_name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
                fail("hwsafe-host",
                     "%s did not build: %s" % (case_name, tail[:200]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 or marker not in txt:
                fail("hwsafe-host",
                     "%s failed: %s" % (case_name, txt.strip()[:220]))
                return
            results.append(txt.strip().splitlines()[-1])
    ok("hwsafe-host",
       "safe-state will / hand-over / death ledger: %s | %s"
       % (results[0], results[1]))


def check_audit_host():
    """The kernel's own structural self-check, on host, at both grades.

    arkrtos_audit.c is read-only: it walks the partition table, the slot
    records and the task table and answers one question per group -- do their
    fields still satisfy the invariants the rest of the kernel assumes? The
    bug class it exists for shows up much later (scheduler picks a non-runnable
    task, pool hands out overlapping ranges), so a silent false "clean" is the
    worst possible outcome. The host case therefore builds a clean table, then
    breaks ONE field at a time and asserts exactly that group lights up with
    the right index and nothing else; then restores and asserts green again.

    Two builds are checked: USE_AUDIT=1 (the real checks; links the real
    ptable because the audit reads the actual table at SHARE_RAM_BASE) and
    USE_AUDIT=0, where every entry point must honestly answer "no check was
    done" (mask 0, last_mask 0, boot 0, name == "audit disabled") instead of
    faking a clean bill of health.

    The case also found two real divide-by-zero defects in the audit itself
    (pool_sector == 0 and pool_alloc_unit == 0): a self-check that crashes on
    the very corruption it is auditing answers nothing, so both got a
    short-circuit guard on 2026-11-22.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("audit-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    picks = {}
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name in ("audit", "audit-off"):
            picks[name] = (test, mods, defs, incs)
    for need in ("audit", "audit-off"):
        if need not in picks:
            fail("audit-host", "case %s missing from coverage_gate.CASES" % need)
            return

    results = []
    for case_name, marker in (("audit", "AUDIT-HOST OK"),
                              ("audit-off", "AUDIT-HOST OFF OK")):
        test, mods, defs, incs = picks[case_name]
        with TempDir() as td:
            exe = os.path.join(td, case_name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
                fail("audit-host",
                     "%s did not build: %s" % (case_name, tail[:200]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 or marker not in txt:
                fail("audit-host",
                     "%s failed: %s" % (case_name, txt.strip()[:220]))
                return
            results.append(txt.strip().splitlines()[-1])
    ok("audit-host",
       "structural self-check (one group broken at a time): %s | %s"
       % (results[0], results[1]))


def check_mpu_host():
    """MPU window construction, on host, at both grades.

    arkrtos_mpu.c never touches MPU registers: it decides which address
    windows a task may reach and fills the architecture-independent context
    in the TCB; the port encodes that into hardware. So the host case stubs
    the five port entry points and pins the geometry and attribute of every
    window that would reach the port, plus the fail-closed decisions:

      * a kernel task gets the kernel flash window and a chip-RAM fallback
        (shared RAM is already covered, so no separate share region);
      * an App's code window is the smallest aligned power-of-two region
        containing the image; a tight landing expands downward but never
        across the pool end (that case falls back instead);
      * a RAM window inside the pool with a bad shape (not power of two,
        misaligned, wrong size range, crossing the pool end) is REFUSED with
        the region left disabled -- never widened into chip RAM, which would
        expose every other task's stack;
      * a MiniApp's code is in RAM, so region 0 must be executable RAM (RAMX),
        not read-only flash; a data window that IS the code window must keep
        execute permission too;
      * every build clears the whole region set first, so a refused encode or
        a previous context cannot linger.

    Two builds: USE_MPU=1 (the real code) and USE_MPU=0, where the module is
    an empty translation unit and the header collapses the five entry points
    to empty macros. The OFF case proves the call sites still compile and do
    nothing; it is not a claim that any window was built.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("mpu-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    picks = {}
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name in ("mpu", "mpu-off"):
            picks[name] = (test, mods, defs, incs)
    for need in ("mpu", "mpu-off"):
        if need not in picks:
            fail("mpu-host", "case %s missing from coverage_gate.CASES" % need)
            return

    results = []
    for case_name, marker in (("mpu", "MPU-HOST OK"),
                              ("mpu-off", "MPU-HOST OFF OK")):
        test, mods, defs, incs = picks[case_name]
        with TempDir() as td:
            exe = os.path.join(td, case_name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
                fail("mpu-host",
                     "%s did not build: %s" % (case_name, tail[:200]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 or marker not in txt:
                fail("mpu-host",
                     "%s failed: %s" % (case_name, txt.strip()[:220]))
                return
            results.append(txt.strip().splitlines()[-1])
    ok("mpu-host",
       "task window geometry (fail-closed on bad shapes): %s | %s"
       % (results[0], results[1]))


def check_guard_host():
    """Watchdog gating and the heartbeat contract, on host, at three grades.

    arkrtos_guard.c decides two things and touches no register: whether the
    system is still keeping the promises it made, and whether the hardware
    should be allowed to end this run. The host case stubs the neighbours
    (tick counter, partition table, audit, crash ledger, fault ring, log,
    watchdog port) and pins the decision, not the hardware:

      * the contract is judged only on service ticks, and the judge is the
        beat age alone -- a task blocked longer than its declared period is
        still in violation, or an image that never wakes up is never caught;
      * a violation is charged once per boot (crash ledger + fault ring),
        after which only the starvation counter moves -- charging per
        evaluation would turn one broken image into hundreds of faults;
      * the watchdog is fed only when the verdict is clean; a board that did
        not ask for one, or a port that refused to arm it, still judges and
        still reports, it just never feeds;
      * a contract dies with its task, so it is never charged to whichever
        image is handed the id next.

    Three builds: WDG_ENABLE=1 (armed), WDG_ENABLE=0 (board asked for none:
    judging continues, feeding does not) and USE_KERNEL_GUARD=0, where the
    module is an empty TU and every entry point answers "disabled" instead of
    a silent success.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("guard-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    picks = {}
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name in ("guard", "guard-nowdg", "guard-off"):
            picks[name] = (test, mods, defs, incs)
    for need in ("guard", "guard-nowdg", "guard-off"):
        if need not in picks:
            fail("guard-host", "case %s missing from coverage_gate.CASES" % need)
            return

    results = []
    for case_name, marker in (("guard", "GUARD-HOST OK"),
                              ("guard-nowdg", "GUARD-HOST NOWDG OK"),
                              ("guard-off", "GUARD-HOST OFF OK")):
        test, mods, defs, incs = picks[case_name]
        with TempDir() as td:
            exe = os.path.join(td, case_name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
                fail("guard-host",
                     "%s did not build: %s" % (case_name, tail[:200]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 or marker not in txt:
                fail("guard-host",
                     "%s failed: %s" % (case_name, txt.strip()[:220]))
                return
            results.append(txt.strip().splitlines()[-1])
    ok("guard-host",
       "heartbeat contract + watchdog gating (feed only on a clean verdict): %s | %s | %s"
       % (results[0], results[1], results[2]))


def check_rt_host():
    """Real-time measurement arithmetic and bookkeeping, on host, at two grades.

    arkrtos_rt.c touches no register: every timestamp comes from the port's
    free-running cycle counter, and both wakeup scenarios go through kernel
    primitives. The host case stubs the counter, the semaphores, the task
    registration and the delay, and pins the arithmetic and the ledgers:

      * with no counter, or a counter but no clock, every row stays at n = 0
        and the microsecond conversion returns 0 -- it never falls back to
        tick counting and calls a value that is three orders of magnitude
        off a measurement;
      * cycles -> microseconds is split at the kilohertz boundary, so the
        whole-second term and the remainder both survive without a 64-bit
        divide (RV32 has no libgcc for it);
      * jitter is signed (early is negative, late is positive) and the first
        tick has no reference, so it records no sample;
      * wake.lo starts at the tick entry while switch starts at the last line
        of the tick body, so the two only subtract when the sequence numbers
        pair; a sample taken inside an open tick body is discarded and counted
        in skip_n instead of using the previous tick's end;
      * a wakeup more than one period late is a preemption by something else,
        not a wakeup delay, so it is counted in late_n and left out of the
        statistics;
      * the preemption helper is created once, and when it cannot start,
        last_error names the step; a run with fewer samples than asked is
        reported as a failure instead of being padded with zeros;
      * a critical section is measured only by the outermost exit, and an
        unbalanced exit neither samples nor underflows.

    Two builds: RTSTAT=1 (with the host-only loop budget) and RTSTAT=0, where
    the module is an empty TU -- the OFF case calls nothing and proves the
    header exposes no symbol.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC

    cc = find_cc(None)
    if not cc:
        fail("rt-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    picks = {}
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name in ("rt", "rt-off"):
            picks[name] = (test, mods, defs, incs)
    for need in ("rt", "rt-off"):
        if need not in picks:
            fail("rt-host", "case %s missing from coverage_gate.CASES" % need)
            return

    results = []
    for case_name, marker in (("rt", "RT-HOST OK"),
                              ("rt-off", "RT-HOST OFF OK")):
        test, mods, defs, incs = picks[case_name]
        with TempDir() as td:
            exe = os.path.join(td, case_name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
                fail("rt-host",
                     "%s did not build: %s" % (case_name, tail[:200]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 or marker not in txt:
                fail("rt-host",
                     "%s failed: %s" % (case_name, txt.strip()[:220]))
                return
            results.append(txt.strip().splitlines()[-1])
    ok("rt-host",
       "cycle-counter arithmetic, jitter, wake/switch pairing and ledgers: %s | %s"
       % (results[0], results[1]))

def check_sec_host():
    """Security baseline self-check (arkrtos_sec_run and its twelve items).

    The module is a read-only questionnaire: it asks "is this mechanism
    actually in the build" and returns a report; it changes nothing. Almost
    every one of the twelve items has an #if and an #else body, so no single
    configuration can walk the whole file. Four are built and run:

      * sec: everything on (CRC recheck, image sign, HW_SAFE, watchdog,
        RTSTAT) plus a strong Flash-port stub, so the PASS and FINDING sides
        of every item are exercised;
      * sec-min: all baseline mechanisms off and SEC_HOST_NO_PORT, so the
        module's own weak arkrtos_port_flash_protection answers
        ARKRTOS_SEC_ERR_NO_PORT and the item reports SKIP;
      * sec-page: the LoongArch64 shape -- MPU=0, PAGING=1, PRIV=1. Isolation
        is code 2 (page tables), not a missing MPU, and the watchdog is not
        biting, so guard.watchdog reports FINDING;
      * sec-disabled: USE_SECCHECK=0, where the module is an empty TU. The
        object is checked with nm to be free of global definitions -- "off"
        must mean zero bytes and zero symbols, not a hidden stub.

    What the case pins, beyond the item names and order:
      * the gates bitmask is recomputed independently from this build's
        macros (bit0 isolation, bit1 priv, bit2 stack check, bit3 fault
        recovery, bit4 crash log, bit5 audit; sign and hwsafe are reported in
        the upper bits but not required), so a renumbering on one side only
        goes red;
      * a slot whose magic is wrong is reported as 0x100 and is NOT handed to
        the loader, and a loader rejection comes back verbatim;
      * CRC recheck keeps three answers apart: 0 = PASS, ERR_CRC = FAIL,
        any other negative = unknown, and when nothing could be rechecked the
        item is SKIP -- a zero-checked run never reports PASS;
      * the Flash item separates "no port" (SKIP) from "read it, RDP is level
        0" (FINDING) and from "read it, RDP is on" (PASS);
      * run(NULL) returns 0 and run() returns the FAIL count, not the item
        count.

    A missing host compiler is a FAIL, not a skip.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from coverage_gate import CASES, _case, find_cc, mingw_prefix, COMMON_INC
    import shutil

    cc = find_cc(None)
    if not cc:
        fail("sec-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return

    want = (("sec", "SEC-HOST OK  "),
            ("sec-min", "SEC-HOST MIN OK  "),
            ("sec-page", "SEC-HOST PAGE OK  "),
            ("sec-disabled", "SEC-HOST DISABLED OK"))
    names = [w[0] for w in want]
    picked = {}
    for case in CASES:
        name, test, mods, defs, _markers, incs, _argv = _case(case)
        if name in names:
            picked[name] = (test, mods, defs, incs)
    for name, _marker in want:
        if name not in picked:
            fail("sec-host", "case %s missing from coverage_gate.CASES" % name)
            return

    lines = []
    with TempDir() as td:
        for name, marker in want:
            test, mods, defs, incs = picked[name]
            exe = os.path.join(td, name + (".exe" if os.name == "nt" else ""))
            cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"]
                   + defs + incs + COMMON_INC + ["-o", exe, test] + mods)
            r = _run_check(cmd)
            if r.returncode != 0:
                tail = ((r.stderr or "").strip().splitlines() or [""])[-1]
                fail("sec-host", "%s did not build: %s" % (name, tail[:200]))
                return
            r = _run_check([exe])
            txt = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 or marker not in txt:
                fail("sec-host", "%s failed: %s" % (name, txt.strip()[:220]))
                return
            lines.append(txt.strip().splitlines()[-1])

        # OFF means an empty translation unit: nm on the object must show no
        # defined global. A stub that survived the switch would still answer
        # PASS-shaped questions and defeat the point of the switch.
        off_defs = picked["sec-disabled"][2]
        obj = os.path.join(td, "sec_off.o")
        cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0"] + off_defs
               + COMMON_INC + ["-c", "-o", obj, "kernelsrc/src/arkrtos_sec.c"])
        r = _run_check(cmd)
        if r.returncode != 0:
            fail("sec-host", "OFF object did not build: %s"
                 % ((r.stderr or "").strip().splitlines() or [""])[-1][:200])
            return
        nm = os.path.join(os.path.dirname(cc), "nm.exe" if os.name == "nt" else "nm")
        if not os.path.isfile(nm):
            nm = shutil.which("nm") or "nm"
        r = _run_check([nm, obj])
        nm_txt = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0:
            fail("sec-host", "nm failed on the OFF object: %s" % nm_txt.strip()[:200])
            return
        defined = []
        for ln in nm_txt.splitlines():
            parts = ln.split()
            # 只算大写类型（T/D/B/R = 已定义的全局符号）；小写是段符号与局部符号。
            if len(parts) >= 3 and parts[-2].isupper():
                defined.append(parts[-1])
        if defined:
            fail("sec-host",
                 "USE_SECCHECK=0 object still defines symbols: %s"
                 % ", ".join(defined[:8]))
            return

    ok("sec-host",
       "14-item baseline: PASS/FAIL/FINDING/SKIP in 4 builds, OFF object has no symbols (%s)"
       % " | ".join(lines))


def check_auditlog():
    """Persistent audit ring (SEC-G05): the trail must survive a remount.

    tools/audit_log_gate.py holds the five groups: the switch defaults to OFF and
    the three config files must agree; the OFF object must export no symbol; the
    seven wired files (module + fs / installer / three shell files / linux_abi)
    must compile with the switch ON, because every wiring line sits inside an
    #if that the normal build never sees; the host cases must pass in both
    builds; and the judge itself must reject a bogus output.

    What this does NOT prove: that a real board keeps the ring across a power
    cycle. The host volume is a RAM array, and QEMU's NOR is RAM too - only real
    flash can answer that. A missing compiler is a FAIL, not a skip.
    """
    r = _run_check([sys.executable, os.path.join("tools", "audit_log_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "AUDIT-LOG-GATE OK" in txt:
        ok("auditlog", "OFF default + zero-symbol OFF object + 7 ON-path files + host cases")
    else:
        fail("auditlog", txt[-300:])

def check_rollback():
    """Rollback whitelist (SEC-G11): an explicit and bounded downgrade lane.

    tools/rollback_gate.py holds the five groups: the switch defaults to OFF and
    features.h / Kconfig / defconfig / autoconf must agree; the OFF object must
    export no symbol; the four wired files (module + loader + two shell files)
    must compile with the switch ON; the host cases must pass in three builds
    (OFF, ON, ON+audit); and the judge itself must reject a bogus output.

    What this does NOT prove: that a real downgrade works end to end on a board
    with flash and a reboot in between. The host pool is a RAM array, and the
    whitelist is RAM-only by design - the reset behaviour is the point, so it
    can only be observed on hardware.
    """
    r = _run_check([sys.executable, os.path.join("tools", "rollback_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "ROLLBACK-GATE OK" in txt:
        ok("rollback", "OFF default + zero-symbol OFF object + 4 ON-path files + host cases OFF/ON/ON+audit")
    else:
        fail("rollback", txt[-300:])

def check_selftest():
    """Component self-test (SEC-G14): does the board still work, not "did it boot".

    tools/selftest_gate.py holds the five groups: the switch defaults to OFF and
    features.h / Kconfig / defconfig / autoconf must agree; the OFF object must
    export no symbol; the module plus four wired files (init, two shell files)
    and the LA64 board hooks must compile with the switch ON; the host cases
    must pass in three builds (OFF, ON, ON+audit); and the judge itself must
    reject a bogus output.

    What this does NOT prove: that the diagnostics fire on real silicon. The
    clock check compares two readings this side of the SoC, so a common crystal
    error is invisible; the RAM item only covers the module's own window; and
    the flash item only proves "changed since the boot-time baseline", not that
    the image was ever genuine. All three are written into the known-limits doc.
    """
    r = _run_check([sys.executable, os.path.join("tools", "selftest_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "SELFTEST-GATE OK" in txt:
        ok("selftest", "OFF default + zero-symbol OFF object + 5 ON-path files + host cases OFF/ON/ON+audit")
    else:
        fail("selftest", txt[-300:])

def check_fault_inject():
    """Fault injection and tamper watch (SEC-G16, software half).

    tools/fi_gate.py holds the five groups: the switch defaults to OFF and
    features.h / Kconfig / defconfig / autoconf must agree; the OFF object must
    export no symbol; the module plus six wired files (init, task, mempool, two
    shell files) must compile with the switch ON and the softbus tier macros
    present; the host cases must pass in three builds (OFF, ON, ON+audit); the
    judge itself must reject a bogus output; and the injection sites must still
    be wired into mempool (alloc) and task (create, tick) behind the switch.

    What this does NOT prove: two things were deliberately left out of scope.
    This injects kernel *decisions* (alloc failure, create failure, a skipped
    tick), not bit flips in RAM or voltage/clock glitches -- those need hardware
    and stay with SEC-G15. And the tamper watch compares against a baseline
    taken at registration time: if the span was already forged before it was
    registered, the scan sees nothing. Closing that needs the signed-image
    chain (SEC-G12 / SEC-G09), not this module.
    """
    r = _run_check([sys.executable, os.path.join("tools", "fi_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "FI-GATE OK" in txt:
        ok("fault-inject", "OFF default + zero-symbol OFF object + 6 ON-path files + host cases OFF/ON/ON+audit + wired injection sites")
    else:
        fail("fault-inject", txt[-300:])

def check_trust_root():
    """Trust root (SEC-G12): the image key comes from somewhere, and that
    somewhere must not be "whatever this firmware was built with".

    tools/trust_gate.py holds the five groups: the switch defaults to OFF and
    features.h / Kconfig / defconfig / autoconf must agree (including the
    asymmetric-profile dependency); the OFF object must export no symbol; the
    module plus three wirings (init's boot call, loader's latch refusal, sec's
    report item) must compile with the switch ON; the host cases must pass in
    three builds (OFF, ON, ON + the sec 4-state report); and three mutations
    (latch not asserted / unreadable store reported as MATCH / a failed pin
    write reported as PASS) must all turn red.

    What this does NOT prove: that the board's OTP really is one-time
    programmable, or that the kernel itself is verified before it runs. The
    first is a silicon property; the second needs a BootROM, which is why
    arkrtos_trust.h says so in its first paragraph. Trust-on-first-use's
    first-boot window is real and is documented, not closed.
    """
    r = _run_check([sys.executable, os.path.join("tools", "trust_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "TRUST-GATE OK" in txt:
        ok("trust-root", "OFF default + zero-symbol OFF object + module & 3 wirings + host cases OFF/ON/ON+sec + 3 mutations")
    else:
        fail("trust-root", txt[-300:])

def check_gdbstub():
    """GDB RSP agent: a debugger can look at the target, but not fake a stop.

    tools/gdbstub_gate.py holds the five groups: the switch defaults to OFF and
    features.h / Kconfig / defconfig / autoconf must agree; the OFF object must
    export no symbol; the module and its host case must compile with the switch
    ON; the host cases must pass in both builds; and five mutations (hex encode
    expanded in place over its own source / an unsupported packet answered OK /
    no-ack mode never taking effect / a bad checksum acked / a register returned
    big-endian) must all turn red.

    What this does NOT prove: that a real gdb client interoperates (there is no
    gdb on the build host), or that continue/step/breakpoints work — they are
    deliberately not implemented, because faking a stop would be worse than
    answering "unsupported". The agent has no authentication: it belongs on a
    debug port only, which is why it ships off by default.
    """
    r = _run_check([sys.executable, os.path.join("tools", "gdbstub_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "GDBSTUB-GATE OK" in txt:
        ok("gdbstub", "OFF default + zero-symbol OFF object + module & host case compile ON + cases OFF/ON + 5 mutations")
    else:
        fail("gdbstub", txt[-300:])

def check_device_identity():
    """Device identity (SEC-G13): a name for the board, and a way to prove it.

    tools/devid_gate.py holds the five groups: the switch defaults to OFF and
    features.h / Kconfig / defconfig / autoconf must agree (including the
    8..32 length range); the OFF object must export no symbol; the module plus
    three wirings (init's boot call, sec's fifteenth item, the shell command)
    must compile with the switch ON; the host cases must pass in three builds
    (OFF, ON, and ON + a compile-time ARKRTOS_DEVICE_SECRET), plus two runs of
    the sec report (PASS / FINDING / SKIP x2); and four mutations (keyless
    board returns an all-zero MAC / a firmware-wide secret reported as OTP /
    a sub-minimum secret marked ready / FINDING reported as PASS) must all
    turn red.

    What this does NOT prove: that the board really has an OTP fuse bank, that
    anything was provisioned in production, or that a key can be rotated or a
    stolen device revoked.  Those are process and silicon, not kernel code;
    arkrtos_devid.h says so in its header, and docs/已知限制.md carries the
    rest.  A firmware-embedded secret is reported as BUILTIN precisely so a
    caller cannot mistake it for a per-device identity.
    """
    r = _run_check([sys.executable, os.path.join("tools", "devid_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "DEVID-GATE OK" in txt:
        ok("device-id", "OFF default + zero-symbol OFF object + module & 3 wirings + host cases OFF/ON/BUILTIN + sec 3-state report + 4 mutations")
    else:
        fail("device-id", txt[-300:])

def check_coverage_host():
    """Line coverage of the modules the host tests actually link.

    The *-host checks above answer "did the case pass". A case can pass while
    walking a third of the module, and that looks identical in this report.
    tools/coverage_gate.py rebuilds those same cases with --coverage, runs
    gcov, and compares each module's line coverage against the floor recorded
    in tools/coverage_baseline.json.

    Read the number for what it is: coverage of ONE .c per case (the one the
    host test links), not of the kernel. Most of the kernel (scheduler, SVC,
    drivers, shell) is not linked on the host at all, so whole-kernel coverage
    is far below these numbers and this gate says nothing about it. The floor
    is "not lower than when it was measured", not an industry target; when it
    drops the right move is to add a case, and only with a stated reason to
    re-pin the baseline (--update).
    """
    r = _run_check([sys.executable, os.path.join("tools", "coverage_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "COVERAGE-GATE OK" in txt:
        mods = len([l for l in txt.splitlines() if l.startswith(("ok  ", "new "))])
        ok("coverage-host", "%d host-linked module(s) at or above the pinned line-coverage floor"
           % mods)
    else:
        fail("coverage-host", "gate failed: %s" % txt.strip()[-400:])

def check_warn_host():
    """-Wall -Wextra must be silent on every host-compilable translation unit.

    This is NOT static analysis and must never be reported as such: it is the
    compiler's own semantic pass turned up from its defaults, one TU at a time,
    with no path exploration. Real path-sensitive analysis lives in the
    "analyzer" check (tools/analyzer_gate.py, GCC -fanalyzer on the RV32 target),
    which was added on 2026-11-18 once this machine's toolchain list was
    re-checked and riscv64-unknown-elf-gcc-10.2.0 turned up.

    Scope is honest and narrow: every TU the cases in coverage_gate.CASES
    compile -- 45 case configurations (test + module each) plus the SDK stdio
    test, 92 TUs in total, because a few modules are built once per
    configuration (sign, blk, dev, event, export, init, hwsafe, audit, mpu and rt
    twice; guard three times; log three
    times: default, USE_LOG_RING=0 and USE_LOG=0; timer three times: default,
    tickless and USE_TIMER=0; cfg three times: default, STACK_GUARD=1 and
    STACK_USAGE=0). Everything else in kernelsrc/ needs an
    architecture toolchain (armcc/armclang, loongarch64, riscv64) to compile at
    all, so it is out of reach here.

    It has already paid for itself: on 2026-11-17 it found p_tsk declared and
    never assigned in arkrtos_sync_release_task(), which meant a dying task was
    never removed from any waiting queue and the mutex it held was never handed
    over -- compiles clean, fails silently on a board.
    """
    r = _run_check([sys.executable, os.path.join("tools", "warn_gate.py"),
                    "--selfcheck"])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode != 0 or "selfcheck ok" not in txt:
        fail("warn-clean", "selfcheck did not see a deliberate warning: %s"
             % txt.strip()[-300:])
        return
    if "WARN-GATE selfcheck FAIL" in txt:
        fail("warn-clean", "selfcheck reported a blind gate")
        return
    r = _run_check([sys.executable, os.path.join("tools", "warn_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "WARN-GATE OK" in txt:
        head = [l for l in txt.splitlines() if l.startswith("WARN-GATE OK")]
        ok("warn-clean",
           "%s (compiler pass, not static analysis)"
           % (head[0][len("WARN-GATE OK  ("):].split("，")[0] if head else "host TU sweep"))
    else:
        fail("warn-clean", "gate failed: %s" % txt.strip()[-400:])

def check_analyzer():
    """GCC -fanalyzer over every kernel TU that compiles stand-alone.

    This is the real static analysis the debt page asked for. It runs the
    riscv64-unknown-elf-gcc-10.2.0 that ships with the Luban-Lite toolchain in
    RV32 mode (-march=rv32imac -mabi=ilp32), so pointer width matches the MCU
    targets; in RV64 mode the same code produces hundreds of
    -Wint-to-pointer-cast lines that are architecture noise, not defects.

    The gate does its own blind check: two deliberately broken probes
    (double-free, null-dereference) must each be reported, otherwise it fails
    instead of printing a clean number. Scope is printed too - TU count, skip
    list with reasons, and how many vendored diagnostics were seen and not
    judged - because "0 findings" over an unknown set is not a reading.
    """
    r = _run_check([sys.executable, os.path.join("tools", "analyzer_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "ANALYZER-GATE OK" in txt:
        head = [l for l in txt.splitlines() if l.startswith("analyzer:")]
        ok("analyzer", (head[0] if head else "kernel TU sweep") +
           " / 0 diagnostic / self-check probes fired")
    else:
        fail("analyzer", "gate failed: %s" % txt.strip()[-400:])

# ---------------------------------------------------------------- 远程档
#
# 这些检查的 gate 脚本会去驱动「只存在于开发机上的工具链」：Keil 的 armclang /
# arm-none-eabi，或者 LoongArch64 / RISC-V 交叉 gcc。Gitee Go 的 runner 上一个
# 也没有，所以 --remote 把它们列为 skip 并在总结里单独报数 —— 没跑过的检查
# 不算通过，runner 也不允许把「没工具链」说成「检查通过」。
#
# 键是 --remote 下要跳过的检查名（与打印出来的名字一致）；值只是给人看的理由。
LOCAL_TOOLCHAIN = {
    "tickless": "armclang (Keil MDK)",
    "arkmake": "arm-none-eabi-gcc",
    "rtstat": "armclang (Keil MDK)",
    "eset-syntax": "armclang (Keil MDK)",
    "waitmulti-syntax": "armclang (Keil MDK)",
    "waitq": "armclang (Keil MDK)",
    "smp": "armclang (Keil MDK)",
    "seccheck": "armclang (Keil MDK)",
    "swap": "armclang (Keil MDK)",
    "mps2-m4": "arm-none-eabi-gcc + qemu-system-arm + make",
    "paging": "LA64 / RV32 cross gcc",
    "la64-build": "LA64 cross gcc + make",
    "shell-auth": "LA64 cross gcc",
    "elf": "LA64 / RV32 cross gcc",
    "linux-abi": "LA64 / RV32 cross gcc + qemu-system-loongarch64",
    "fdt": "RV32 cross gcc + LA64 cross gcc + qemu-system-loongarch64",
    "modld": "LA64 / RV32 / LA32 cross gcc",
    "payload-sign": "LA64 cross gcc",
    "stack-guard": "LA64 / RV32 cross gcc",
    "icount-stack": "LA64 cross gcc + qemu-system-loongarch64",
    "svc-wide": "pure python (source assertions)",
    "dynlink": "LA64 cross gcc + qemu-system-loongarch64",
    "net-virtio": "LA64 cross gcc + qemu-system-loongarch64",
    "bus-line": "LA64 cross gcc + qemu-system-loongarch64",
    "realbin": "LA64 cross gcc (+ loongarch64-linux-gnu-readelf for one check)",
    "sec-crc": "armclang (Keil MDK)",
    "analyzer": "RV32 cross gcc with -fanalyzer (GCC 10+)",
}

# (打印名, 函数)。名字只用于 skip 行与自检，真正的读数由
# 各检查自己打印；一个函数打两行的情况（encoding + mixed-eol）用第一行的名字。
CHECK_TABLE = (
    ("partition-twin", check_partition_twins),
    ("single-address-source", check_single_address_source),
    ("encoding", check_encoding_eol),
    ("format", check_format_baseline),
    ("gate-live", check_gate_scripts_live),
    ("doc-index", check_doc_index),
    ("api-doc", check_api_doc_fresh),
    ("evidence-levels", check_evidence_levels),
    ("layout-check", check_layout_and_scatter),
    ("no-app-partition-include", check_no_partition_include_in_app),
    ("config-generated", check_config_generated),
    ("features-switches", check_features_switches),
    ("softbus-sync", check_softbus_sync_header),
    ("devicetree-generated", check_devicetree_generated),
    ("devicetree-dtb", check_devicetree_dtb),
    ("fdt", check_fdt),
    ("board-config-generated", check_board_config_generated),
    ("release-consistency", check_release_consistency),
    ("utest-host", check_utest_host),
    ("sdk-stdio-host", check_sdk_stdio_host),
    ("sbuf-host", check_sbuf_host),
    ("mempool-host", check_mempool_host),
    ("mq-host", check_mq_host),
    ("sync-host", check_sync_host),
    ("eset-host", check_eset_host),
    ("waitmulti-host", check_waitmulti_host),
    ("loader-host", check_loader_host),
    ("loader-accept-verify", check_loader_accept_verify),
    ("fifo-host", check_fifo_host),
    ("fault-host", check_fault_host),
    ("blk-host", check_blk_host),
    ("dev-host", check_dev_host),
    ("event-host", check_event_host),
    ("timer-host", check_timer_host),
    ("cfg-host", check_cfg_host),
    ("export-host", check_export_host),
    ("init-host", check_init_host),
    ("log-host", check_log_host),
    ("ptable-host", check_ptable_host),
    ("layout-host", check_layout_host),
    ("crash-host", check_crash_host),
    ("apprun-host", check_apprun_host),
    ("flashapp-host", check_flashapp_host),
    ("trace-host", check_trace_host),
    ("apppack-host", check_apppack_host),
    ("insthost-host", check_insthost_host),
    ("paging-host", check_paging_host),
    ("hwsafe-host", check_hwsafe_host),
    ("audit-host", check_audit_host),
    ("mpu-host", check_mpu_host),
    ("guard-host", check_guard_host),
    ("rt-host", check_rt_host),
    ("sec-host", check_sec_host),
    ("auditlog", check_auditlog),
    ("rollback", check_rollback),
    ("selftest", check_selftest),
    ("fault-inject", check_fault_inject),
    ("trust-root", check_trust_root),
    ("device-id", check_device_identity),
    ("gdbstub", check_gdbstub),
    ("coverage-host", check_coverage_host),
    ("warn-clean", check_warn_host),
    ("analyzer", check_analyzer),
    ("tickless", check_tickless),
    ("arkmake", check_arkmake_syntax),
    ("rtstat", check_rtstat_syntax),
    ("eset-syntax", check_eset_syntax),
    ("waitmulti-syntax", check_waitmulti_syntax),
    ("waitq", check_waitq),
    ("smp", check_smp),
    ("mps2-m4", check_mps2_m4),
    ("seccheck", check_seccheck_syntax),
    ("sign-host", check_sign_host),
    ("sign-ed25519", check_sign_ed25519),
    ("swap", check_swap_syntax),
    ("paging", check_paging_switch),
    ("la64-build", check_la64_build_warnings),
    ("elf", check_elf_loader),
    ("linux-abi", check_linux_abi),
    ("fork", check_fork),
    ("shell-auth", check_shell_auth),
    ("shellauth-host", check_shellauth_host),
    ("fdtconsole-host", check_fdtconsole_host),
    ("fdtres-host", check_fdtres_host),
    ("fdt-host", check_fdt_host),
    ("fdtovl-host", check_fdtovl_host),
    ("vfslfs-host", check_vfslfs_host),
    ("vfs-host", check_vfs_host),
    ("fs-host", check_fs_host),
    ("smp-sched-host", check_smp_sched_host),
    ("softbus-host", check_softbus_host),
    ("bus-crypto", check_bus_crypto),
    ("net-host", check_net_host),
    ("net-virtio", check_net_virtio),
    ("elf-host", check_elf_host),
    ("linux-abi-host", check_linux_abi_host),
    ("mini-host", check_mini_host),
    ("task-host", check_task_host),
    ("shellmisc-host", check_shell_misc_host),
    ("ushell-host", check_shell_ushell_host),
    ("shellmod-host", check_shell_mod_host),
    ("shellcfg-host", check_shell_cfg_host),
    ("shellimage-host", check_shell_image_host),
    ("shellcore-host", check_shell_core_host),
    ("shellpmap-host", check_shell_pmap_host),
    ("shelldiag-host", check_shell_diag_host),
    ("shellplat-host", check_shell_platform_host),
    ("shellfs-host", check_shell_fs_host),
    ("drvblk-host", check_drvblk_host),
    ("modld", check_modld),
    ("payload-sign", check_payload_sign),
    ("stack-guard", check_stack_guard),
    ("svc-wide", check_svc_wide),
    ("svc-args-mini", check_svc_args_mini),
    ("bus-line", check_bus_line),
    ("dynlink", check_dynlink),
    ("realbin", check_realbin),
    ("sec-crc", check_sec_crc),
    ("icount-stack", check_icount_stack),
)

def _workflow_self_consistent(txt):
    """校验 .workflow/ci.yml 的结构自洽，返回 (错误列表, 说明串)。

    这里查的是本机查得动的部分：顶层字段在不在、push 触发里有没有 master、
    stage -> step -> commands 这条路能不能走通、commands 里是不是那条
    python3 tools/ci_gate.py --remote。查不动的是 Gitee Go 平台到底认不认
    这套字段 —— 那只能在网页上第一次启用流水线时看，所以本函数只声明
    「结构自洽」，不声明「平台接受」。

    两条路径判同一组事实：有 PyYAML 就真解析，没有就退化成对扁平文本的
    token 断言。没有解析库只会让校验变粗，不会让它变绿。
    """
    toks = [
        ("version:", "顶层缺 version"),
        ("triggers:", "顶层缺 triggers"),
        ("push:", "触发里缺 push"),
        ("branches:", "触发里缺 branches"),
        ("- master", "push 分支列表里没有 master"),
        ("stages:", "顶层缺 stages"),
        ("steps:", "stage 里缺 steps"),
        ("step: build@python", "没有 build@python 步骤"),
        ("pythonVersion:", "步骤里缺 pythonVersion"),
        ("commands:", "步骤里缺 commands"),
        ("python3 tools/ci_gate.py --remote",
         "commands 里没有 python3 tools/ci_gate.py --remote"),
    ]
    bad = [why for tok, why in toks if tok not in txt]
    try:
        import yaml
    except Exception:
        return bad, "tokens only (PyYAML unavailable)"

    try:
        doc = yaml.safe_load(txt)
    except Exception as exc:
        bad.append("YAML 解析失败: %s" % exc)
        return bad, "tokens + yaml (parse failed)"
    if not isinstance(doc, dict):
        bad.append("顶层不是映射")
        return bad, "tokens + yaml"
    for key in ("version", "name", "triggers", "stages"):
        if key not in doc:
            bad.append("顶层缺 %s" % key)

    trg = doc.get("triggers")
    push = trg.get("push") if isinstance(trg, dict) else None
    branches = push.get("branches") if isinstance(push, dict) else None
    precise = branches.get("precise") if isinstance(branches, dict) else None
    if not isinstance(precise, list) or "master" not in precise:
        bad.append("triggers.push.branches.precise 里没有 master")

    cmds = []
    stages = doc.get("stages")
    if not isinstance(stages, list) or not stages:
        bad.append("stages 不是非空列表")
    else:
        for st in stages:
            stage = st.get("stage") if isinstance(st, dict) else None
            steps = stage.get("steps") if isinstance(stage, dict) else None
            if not isinstance(steps, list) or not steps:
                bad.append("某个 stage 没有 steps")
                continue
            for sp in steps:
                # Gitee Go 的写法是「- step: build@python」后同级缩进写
                # name / pythonVersion / commands，所以每个元素本身就是
                # 那个 step 的映射，键 "step" 的值是类型串。
                if not isinstance(sp, dict):
                    bad.append("某个 step 不是映射")
                    continue
                if sp.get("step") != "build@python":
                    continue
                if not sp.get("pythonVersion"):
                    bad.append("build@python 步骤没有 pythonVersion")
                got = sp.get("commands")
                if isinstance(got, list):
                    cmds.extend([c for c in got if isinstance(c, str)])
    if not any("tools/ci_gate.py" in c and "--remote" in c for c in cmds):
        bad.append("没有任何 build@python 步骤真去跑 ci_gate.py --remote")
    return bad, "tokens + yaml"


def check_remote_ci():
    """远程档本身也要有读数：工作流文件在不在、结构自不自洽、skip 清单有没有打错名字。

    两个真实的失败形态：
      1. .workflow/ 里的流水线没指向 ci_gate（或者没带 --remote），
         于是 runner 上跑的是一个更薄的项集，没人知道；
      2. LOCAL_TOOLCHAIN 里的名字写错，那个检查在 runner 上照旧去开
         armclang，然后在「没工具链」和「真失败」之间给人一个假红/假绿。
    """
    bad = []
    struct_note = "workflow missing"
    wf = os.path.join(ROOT, ".workflow", "ci.yml")
    if not os.path.isfile(wf):
        bad.append(".workflow/ci.yml 不存在")
    else:
        txt = read(wf).decode("utf-8", "replace")
        if "tools/ci_gate.py" not in txt:
            bad.append("工作流没有调用 tools/ci_gate.py")
        if "--remote" not in txt:
            bad.append("工作流没有带 --remote（runner 上会去要 Keil/交叉编译器）")
        struct_bad, struct_note = _workflow_self_consistent(txt)
        bad.extend(struct_bad)
    labels = set(name for name, _fn in CHECK_TABLE)
    strays = sorted(set(LOCAL_TOOLCHAIN) - labels)
    if strays:
        bad.append("LOCAL_TOOLCHAIN 里有检查表里不存在的名字: %s" % ", ".join(strays))
    if bad:
        fail("remote-ci", "; ".join(bad))
    else:
        ok("remote-ci",
           "workflow self-consistent (%s) and calls the gate with --remote; "
           "%d skipped check(s) all named in the table"
           % (struct_note, len(LOCAL_TOOLCHAIN)))

def main(argv):
    do_build = "--build" in argv
    remote = "--remote" in argv
    uv4 = None
    if "--uv4" in argv:
        uv4 = argv[argv.index("--uv4") + 1]
    else:
        for cand in ("D:/Keil_v5/UV4/UV4.exe", "C:/Keil_v5/UV4/UV4.exe"):
            if os.path.exists(cand):
                uv4 = cand
                break

    if remote:
        print("[remote profile] skipping checks that need a local Keil / "
              "LoongArch64 / RISC-V toolchain; skipped != passed\n")

    for name, fn in CHECK_TABLE:
        if remote and name in LOCAL_TOOLCHAIN:
            skip(name, "needs %s (not on the runner)" % LOCAL_TOOLCHAIN[name])
            continue
        fn()

    check_remote_ci()

    if do_build:
        check_builds(uv4)

    # The label used to be hardcoded, so a local full run also claimed to be
    # the remote profile (16 skips vs 0).  A reading that misnames its own
    # profile is not a reading.
    print("\n%d check(s) passed, %d failed, %d skipped%s"
          % (len(PASSED), len(FAILURES), len(SKIPPED),
             " (remote profile)" if remote else " (local profile)"))
    if SKIPPED:
        print("NOT VERIFIED on this run: %s" % ", ".join(SKIPPED))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
