#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""发布一致性检查：版本头 / 中英 README / 发布说明 / 最新 tag 必须是一回事。

为什么需要它
------------
`kernelsrc/include/arkrtos_version.h` 的注释从第一天就写着「tag 与 README 徽章
必须和这三个数字一起改」，但没有任何东西在盯——结果是 readme 写着
「版本以 git tag 为唯一记法」而仓库一个 tag 都没有，那句承诺空了很久。

这里把四件事绑在一次检查里：

  1. version.h 的三个数字能解析（唯一事实源）；
  2. `readme.md` 与 `readme_en.md` 的徽章都含 `release-<ver>`；
  3. 两份 README 的「当前版本」段都含 `**<ver>**`（只有徽章对、正文没改也算漂移）；
  4. `docs/发行版与版本策略.md` 在，且 `docs/Release-<ver>.md` 在并含「未验证」一节；
  5. 若仓库已有 tag，最新 tag 必须以 `v<ver>` 开头。

第 5 条是关键：它同时挡住「改了版本号忘了打标签」和「打了标签又悄悄改版本号」。
第一次发版前仓库没有任何 tag，这时第 5 条打印「not tagged yet」而不是失败——
故意的：先把版本与文档改对，再打标签，顺序不能反。

不覆盖什么：tag 是否**已推到远端**（要网络）、tag 的签名、Gitee Release 页面
是否创建。这三件在发版动作清单里，靠人做。

用法
----
    py -3 tools/release_check.py
    py -3 tools/release_check.py --root <仓库根>     # 临时树里跑

退出码 0 = 全过；1 = 有一项不一致。
"""

import argparse
import os
import re
import subprocess
import sys

VER_HDR = "kernelsrc/include/arkrtos_version.h"
README_CN = "readme.md"
README_EN = "readme_en.md"
POLICY = "docs/发行版与版本策略.md"
NOTES_FMT = "docs/Release-%s.md"
# 预览版的发布说明按 tag 命名（docs/Release-0.0.3-preview.1.md），因为 -preview.N
# 只活在 tag 上、不进 version.h；所以发布说明既接受按版本号命名，也接受
# 「版本号 + '-' 开头后缀」的写法，两者只能中一个，多个候选视为歧义。
def find_notes(root, ver):
    import glob
    cands = [d for d in [NOTES_FMT % ver]
             if os.path.exists(os.path.join(root, d))]
    cands += sorted(os.path.relpath(p, root).replace(os.sep, "/")
                    for p in glob.glob(os.path.join(root, NOTES_FMT % (ver + "-*")).replace("/", os.sep)))
    return cands


def read_text(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


def parse_version(root):
    """返回 (major, minor, patch)；解析不出来就抛 ValueError。"""
    txt = read_text(os.path.join(root, VER_HDR))
    vals = {}
    for key in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(r"^#define\s+ARKRTOS_VERSION_%s\s+(\d+)\b" % key,
                      txt, re.M)
        if not m:
            raise ValueError("cannot parse ARKRTOS_VERSION_%s from %s" % (key, VER_HDR))
        vals[key] = int(m.group(1))
    return vals["MAJOR"], vals["MINOR"], vals["PATCH"]


def latest_tag(root):
    """仓库里语义版本最大的 tag，没有就返回 None。"""
    try:
        r = subprocess.run(["git", "-C", root, "tag", "-l", "--sort=-v:refname"],
                           capture_output=True, encoding="utf-8", errors="replace")
    except OSError:
        return None
    if r.returncode != 0:
        return None
    for line in (r.stdout or "").splitlines():
        line = line.strip()
        if line:
            return line
    return None


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    args = ap.parse_args()
    root = args.root

    problems = []

    try:
        major, minor, patch = parse_version(root)
    except ValueError as e:
        print("FAIL version-header            %s" % e)
        return 1
    ver = "%d.%d.%d" % (major, minor, patch)

    # 1) 中英 README 的徽章
    #    shields 的写法是 release-0.0.3--preview.1（`-` 用 `--` 转义），所以这里
    #    只要求出现 release-<ver>，后缀交给 tag 与发布说明去承载。
    for name in (README_CN, README_EN):
        path = os.path.join(root, name)
        if not os.path.exists(path):
            problems.append("%s is missing" % name)
            continue
        txt = read_text(path)
        if ("release-%s" % ver) not in txt:
            problems.append("%s badge does not carry release-%s" % (name, ver))
        if ("**%s**" % ver) not in txt:
            problems.append("%s current-version line does not carry **%s**" % (name, ver))

    # 2) 策略页与发布说明
    if not os.path.exists(os.path.join(root, POLICY)):
        problems.append("%s is missing (release policy)" % POLICY)
    notes = find_notes(root, ver)
    if not notes:
        problems.append("%s is missing (release notes for %s)" % (NOTES_FMT % ver, ver))
    elif len(notes) > 1:
        problems.append("ambiguous release notes for %s: %s"
                        % (ver, ", ".join(notes)))
    else:
        notes_path = os.path.join(root, notes[0])
        notes = notes[0]
        # 发布说明必须自报边界：没有「未验证」这一节就退化成公告，预览版靠它守诚实度。
        if "未验证" not in read_text(notes_path):
            problems.append("%s has no 未验证 section" % notes)

    # 3) 最新 tag 与版本头一致
    tag = latest_tag(root)
    if tag is None:
        print("note  latest-tag               none yet (first release: version and docs first, tag last)")
    elif not tag.startswith("v%s" % ver):
        problems.append("latest tag %s does not match version %s (v%s...)" % (tag, ver, ver))

    if problems:
        for p in problems:
            print("FAIL release-consistency       %s" % p)
        return 1

    print("RELEASE-CHECK OK  version %s%s" % (ver, ("  tag %s" % tag) if tag else "  (not tagged yet)"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
