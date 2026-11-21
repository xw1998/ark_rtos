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
        fail("fdt", txt[-320:])

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
    import tempfile
    cc = _find_host_cc()
    if not cc:
        fail("utest-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with tempfile.TemporaryDirectory() as td:
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

def check_sdk_stdio_host():
    """The SDK's printf shim must produce exactly the right bytes.

    arkrtos_stdio.c is what makes `#include <stdio.h>` + `printf("Hello, World!\\n")`
    work in an App or a mini program without any toolchain retarget.  Its failure
    mode is silent: the program prints a *different* sentence, which nobody
    notices on a board but a host byte comparison catches immediately.
    A missing host compiler is a FAIL, not a skip.
    """
    import tempfile
    cc = _find_host_cc()
    if not cc:
        fail("sdk-stdio-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with tempfile.TemporaryDirectory() as td:
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
            ok("sdk-stdio-host", "printf/puts/putchar byte-exact on host")
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
    import tempfile
    cc = _find_host_cc()
    if not cc:
        fail("sbuf-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with tempfile.TemporaryDirectory() as td:
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
    import tempfile
    cc = _find_host_cc()
    if not cc:
        fail("mempool-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with tempfile.TemporaryDirectory() as td:
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
    import tempfile
    cc = _find_host_cc()
    if not cc:
        fail("mq-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with tempfile.TemporaryDirectory() as td:
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
    import tempfile
    cc = _find_host_cc()
    if not cc:
        fail("sync-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with tempfile.TemporaryDirectory() as td:
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
    import tempfile
    cc = _find_host_cc()
    if not cc:
        fail("loader-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with tempfile.TemporaryDirectory() as td:
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
    import tempfile
    cc = _find_host_cc()
    if not cc:
        fail("fifo-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with tempfile.TemporaryDirectory() as td:
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
    import tempfile
    cc = _find_host_cc()
    if not cc:
        fail("fault-host", "no host C compiler (set ARKRTOS_HOST_CC)")
        return
    with tempfile.TemporaryDirectory() as td:
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

    不覆盖：动态链接（PT_INTERP）、fork/execve、信号、socket、futex、真 mmap
    语义——这些是**不支持**，不是欠验，见 docs/Linux可执行文件兼容-系统调用ABI.md。
    """
    r = _run_check([sys.executable, os.path.join("tools", "linux_abi_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "LINUX-ABI-GATE OK" in txt:
        ok("linux-abi",
           "default off zero symbols / LA64 ON 5 TU compile / 15 syscall numbers "
           "match asm-generic / LA64 instr!=0 is Ark, RV32 0x10..0x22 is Ark / "
           "QEMU 14 checks + exit 7 / parser self-check 4/4")
    else:
        fail("linux-abi", txt[-320:])


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

    Scope is honest and narrow: the 10 host test files, the 10 kernel modules
    they link, and the SDK stdio test. Everything else in kernelsrc/ needs an
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
        ok("warn-clean",
           "21 host-compilable TU(s) build with zero -Wall -Wextra -Wshadow warnings (compiler pass, not static analysis)")
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
    "shell-auth": "LA64 cross gcc",
    "elf": "LA64 / RV32 cross gcc",
    "linux-abi": "LA64 / RV32 cross gcc + qemu-system-loongarch64",
    "fdt": "RV32 cross gcc + LA64 cross gcc + qemu-system-loongarch64",
    "modld": "LA64 / RV32 / LA32 cross gcc",
    "payload-sign": "LA64 cross gcc",
    "stack-guard": "LA64 / RV32 cross gcc",
    "svc-wide": "pure python (source assertions)",
    "dynlink": "LA64 cross gcc + qemu-system-loongarch64",
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
    ("fifo-host", check_fifo_host),
    ("fault-host", check_fault_host),
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
    ("swap", check_swap_syntax),
    ("paging", check_paging_switch),
    ("elf", check_elf_loader),
    ("linux-abi", check_linux_abi),
    ("shell-auth", check_shell_auth),
    ("modld", check_modld),
    ("payload-sign", check_payload_sign),
    ("stack-guard", check_stack_guard),
    ("svc-wide", check_svc_wide),
    ("svc-args-mini", check_svc_args_mini),
    ("dynlink", check_dynlink),
    ("realbin", check_realbin),
    ("sec-crc", check_sec_crc),
)

def check_remote_ci():
    """远程档本身也要有读数：工作流文件在不在、skip 清单有没有打错名字。

    两个真实的失败形态：
      1. .workflow/ 里的流水线没指向 ci_gate（或者没带 --remote），
         于是 runner 上跑的是一个更薄的项集，没人知道；
      2. LOCAL_TOOLCHAIN 里的名字写错，那个检查在 runner 上照旧去开
         armclang，然后在「没工具链」和「真失败」之间给人一个假红/假绿。
    """
    bad = []
    wf = os.path.join(ROOT, ".workflow", "ci.yml")
    if not os.path.isfile(wf):
        bad.append(".workflow/ci.yml 不存在")
    else:
        txt = read(wf).decode("utf-8", "replace")
        if "tools/ci_gate.py" not in txt:
            bad.append("工作流没有调用 tools/ci_gate.py")
        if "--remote" not in txt:
            bad.append("工作流没有带 --remote（runner 上会去要 Keil/交叉编译器）")
    labels = set(name for name, _fn in CHECK_TABLE)
    strays = sorted(set(LOCAL_TOOLCHAIN) - labels)
    if strays:
        bad.append("LOCAL_TOOLCHAIN 里有检查表里不存在的名字: %s" % ", ".join(strays))
    if bad:
        fail("remote-ci", "; ".join(bad))
    else:
        ok("remote-ci",
           "workflow calls the gate with --remote; %d skipped check(s) all named in the table"
           % len(LOCAL_TOOLCHAIN))

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
