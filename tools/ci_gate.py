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

ALLOWED_ENCODINGS = ("utf-8", "gbk")

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

    Works on bytes, not text: these files are GBK/UTF-8 mixed, so decoding with
    'replace' before scanning can shift what is left.  Handles // line comments,
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
    """Every source decodes as one allowed encoding; no blob mixes CRLF and LF.

    Mixed endings are a failure, not a note: they are what turned a real
    153-line change into a 5739-line diff that nobody could review.

    Scanned on the index blob (see stored_bytes) - the working tree is
    autocrlf-rewritten and says nothing about what would be committed.
    """
    bad, mixed = [], []
    for rel, path in own_sources():
        raw = stored_bytes(rel, path)
        if not raw:
            continue
        decoded = None
        for enc in ALLOWED_ENCODINGS:
            try:
                raw.decode(enc)
                decoded = enc
                break
            except UnicodeDecodeError:
                continue
        if decoded is None:
            bad.append(rel)
            continue
        crlf = raw.count(b"\r\n")
        lf = raw.count(b"\n") - crlf
        if crlf and lf:
            mixed.append("%s (crlf=%d lf=%d)" % (rel, crlf, lf))
    if bad:
        fail("encoding", "%d file(s) decode as neither utf-8 nor gbk: %s"
             % (len(bad), ", ".join(bad[:5])))
    else:
        ok("encoding", "all own sources decode")
    if mixed:
        fail("mixed-eol", "%d file(s): %s" % (len(mixed), ", ".join(mixed[:6])))
    else:
        ok("mixed-eol", "no file mixes CRLF and LF")


# ---------------------------------------------------------------- check 3b
def check_format_baseline():
    """格式基线（.editorconfig 里声明的四条）必须真被判到，且门禁自己不失明。

    这几条是纯字节判断（无制表符 / 无行尾空白 / 文件以换行结尾 / 无 UTF-8 BOM），
    不解码、不转码，所以对仓库里 22 个 GBK 旧文件同样成立 —— 这也是选了这四条、
    而不是上 clang-format 的原因：这台机器没有 clang-format，网络也装不了，而整
    文件重排版会把 GBK 字节改坏。

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
    else:
        ok("doc-index", "all %d docs pages are indexed" % len(pages))


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
        cmd += ["-std=gnu11", "-DARKRTOS_USE_MQ=1",
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
    seven affected translation units; a missing armclang is a FAIL, not a
    skip.  Syntax only -- whether the hand-off is actually seamless still needs
    a board (docs/无缝升级-双缓冲热切换.md §6).
    """
    r = _run_check([sys.executable, os.path.join("tools", "swap_syntax_gate.py")])
    txt = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode == 0 and "SWAP-GATE OK" in txt:
        ok("swap", "ON-path compile verified (7 files)")
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
        ok("elf", "default off + LA64 ON compiles (2 TUs) + 2 negative cases refused")
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

    This is the only "look at the code without running it" step this machine
    can actually do: there is no cppcheck / MISRA / clang-tidy, and GCC 4.8 has
    no -fanalyzer. It is NOT static analysis, and must never be reported as
    "static analysis done" -- it is the compiler's own semantic pass turned up
    from its defaults.

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
    "paging": "LA64 / RV32 cross gcc",
    "shell-auth": "LA64 cross gcc",
    "elf": "LA64 / RV32 cross gcc",
    "modld": "LA64 / RV32 / LA32 cross gcc",
    "payload-sign": "LA64 cross gcc",
    "stack-guard": "LA64 / RV32 cross gcc",
    "sec-crc": "armclang (Keil MDK)",
}

# (打印名, 函数)。名字只用于 skip 行与自检，真正的读数由
# 各检查自己打印；一个函数打两行的情况（encoding + mixed-eol）用第一行的名字。
CHECK_TABLE = (
    ("partition-twin", check_partition_twins),
    ("single-address-source", check_single_address_source),
    ("encoding", check_encoding_eol),
    ("format", check_format_baseline),
    ("doc-index", check_doc_index),
    ("api-doc", check_api_doc_fresh),
    ("layout-check", check_layout_and_scatter),
    ("no-app-partition-include", check_no_partition_include_in_app),
    ("config-generated", check_config_generated),
    ("features-switches", check_features_switches),
    ("softbus-sync", check_softbus_sync_header),
    ("devicetree-generated", check_devicetree_generated),
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
    ("tickless", check_tickless),
    ("arkmake", check_arkmake_syntax),
    ("rtstat", check_rtstat_syntax),
    ("eset-syntax", check_eset_syntax),
    ("waitmulti-syntax", check_waitmulti_syntax),
    ("waitq", check_waitq),
    ("smp", check_smp),
    ("seccheck", check_seccheck_syntax),
    ("sign-host", check_sign_host),
    ("swap", check_swap_syntax),
    ("paging", check_paging_switch),
    ("elf", check_elf_loader),
    ("shell-auth", check_shell_auth),
    ("modld", check_modld),
    ("payload-sign", check_payload_sign),
    ("stack-guard", check_stack_guard),
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

    print("\n%d check(s) passed, %d failed, %d skipped (remote profile)"
          % (len(PASSED), len(FAILURES), len(SKIPPED)))
    if SKIPPED:
        print("NOT VERIFIED on this run: %s" % ", ".join(SKIPPED))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
