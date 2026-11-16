#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""可下载模块（ARKRTOS_USE_MODLOAD）的门禁：把开关的两个方向都变成读数。

为什么需要这一道
----------------
和 elf_gate 同型的四种假绿：

  1. 默认档（关）里 `arkrtos_modld.c` / `arkrtos_exports.c` 被
     `#if (ARKRTOS_USE_MODLOAD == 1)` 整段包住，是一个空 TU，没编译器经过；
     装载器写错了，默认档照样全绿；
  2. 开关「打开了但还是空 TU」（宏拼错、`#if` 写反）——门上写着支持，
     编出来的镜像里一条模块代码都没有；
  3. **符号对账假绿**（本门禁的核心）：示例模块 `.o` 里引用的符号，如果
     有一个不在导出表里，真机上就是 `ld rc=-6`，而所有单元测试都绿；
  4. 守卫被拆：`ET_REL` 判定、重定位读写有界、不认识的重定位类型必须报错
     带类型号；任何一条被顺手删掉，都只能靠「装一个坏模块」才看得见。

判据
----
  A. 默认档：`arkrtos_autoconf.h` 里 `ARKRTOS_USE_MODLOAD` 必须是 0；
  B. LA64 正向：板级（qemu_loongarch64）已置 1，`arkrtos_modld.c` 与
     `arkrtos_exports.c` 必须编得过，**且预处理结果里真的有 `arkrtos_mod_load`**
     （防「开着却空 TU」）；
  C. 符号对账：把 `example/qemu_loongarch64/mod/modhello.c` 真编成 `.o`，
     用 nm 取未定义符号，逐个必须在 `arkrtos_exports.c` 的 `s_exports` 里；
  D. 守卫 pin：ET_REL 判定、读写有界、默认分支报错带类型号、
     `ARKRTOS_MOD_AREA_SIZE` 缺失必须 `#error`；
  E. Kconfig / 板级接线：`config ARKRTOS_USE_MODLOAD` 存在、depends 里有
     VFS 与 PRIV、help 写明「第二种模型、不是替代」，板级置 1。

不覆盖：运行期行为（真装载、真重定位、真调内核 API、`unld` 后再装）。
那些要跑 QEMU，实测读数见 docs/MMU平台可下载模块-VxWorks式.md。

用法
----
    python tools/modld_gate.py
    python tools/modld_gate.py --la64-gcc <path>

退出码 0 = 全过；1 = 有一步不成立或找不到编译器（缺编译器是 FAIL，不是跳过）。
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

LA64_FALLBACKS = [
    "D:/LoongIDE/la64-tool/bin/loongarch64-newlib-elf-gcc.exe",
]

LA64_ARCH = ["-march=loongarch64", "-mabi=lp64d", "-mstrict-align"]

COMMON = [
    "-O0",
    "-Wall",
    "-ffreestanding",
    "-fno-common",
    "-nostdlib",
    "-nostartfiles",
    "-fno-builtin",
    "-fsyntax-only",
    "-DARKRTOS_SIGN_KEY_LOCAL=1",
]

# 只为让 arkrtos_features.h 的「有签名开关就必须有密钥」那条 #error 过关。
SIGN_KEY_HEADER = """#ifndef ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_IMAGE_SIGN_KEY { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F }
#endif
"""

# 编译器自己造出来的符号，不是模块对内核的请求。
NM_ARTIFACTS = ("_GLOBAL_OFFSET_TABLE_", "__global_pointer$", "_gp", "__gp")


def _pick(cands, given, what):
    if given:
        return given
    for c in cands:
        if os.path.isfile(c):
            return c
    print("FAIL: 找不到 %s（缺编译器是 FAIL，不是跳过）" % what)
    sys.exit(1)


def _text(raw):
    """内核头文件是 GBK，gcc 原样回显源里的中文，两种编码都试。"""
    if not raw:
        return ""
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def _run(cc, args):
    r = subprocess.run([cc] + args, cwd=ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return r.returncode, _text(r.stdout) + _text(r.stderr)


def _includes(board, port):
    return [
        "-Ikernelsrc/include",
        "-Iconfig",
        "-Iboard/" + board,
        "-Ikernelsrc/port/" + port,
    ]


def _read(path):
    with open(path, "rb") as f:
        return _text(f.read())


def _exports_names():
    src = _read(os.path.join(ROOT, "kernelsrc/src/arkrtos_exports.c"))
    m = re.search(r"static const arkrtos_export_t s_exports\[\] = \{(.*?)\n\};",
                  src, re.S)
    if not m:
        return None
    return set(re.findall(r'\{\s*"([^"]+)"', m.group(1)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--la64-gcc", default="")
    a = ap.parse_args()

    la64 = _pick(LA64_FALLBACKS, a.la64_gcc, "LoongArch64 交叉编译器")
    nm = os.path.join(os.path.dirname(la64),
                      os.path.basename(la64).replace("-gcc", "-nm"))

    tmp = tempfile.mkdtemp(prefix="ark_modld_")
    key_dir = os.path.join(tmp, "key")
    os.makedirs(key_dir)
    with open(os.path.join(key_dir, "arkrtos_sign_key_local.h"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(SIGN_KEY_HEADER)

    board_flags = LA64_ARCH + COMMON + [
        "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
        "-include", "arkrtos_board_config.h",
        "-I" + key_dir,
    ] + _includes("qemu_loongarch64", "loongarch/la64")

    failures = []

    # --- A. 默认档必须是 0 ------------------------------------------------
    autoconf = _read(os.path.join(ROOT, "kernelsrc/include/arkrtos_autoconf.h"))
    if "ARKRTOS_USE_MODLOAD 0" not in autoconf:
        failures.append("A 默认档：arkrtos_autoconf.h 里 ARKRTOS_USE_MODLOAD 不是 0")

    # --- B. LA64 正向（板级已开） -----------------------------------------
    for rel in ("kernelsrc/src/arkrtos_modld.c",
                "kernelsrc/src/arkrtos_exports.c"):
        rc, out = _run(la64, board_flags + [rel])
        if rc != 0:
            failures.append("B LA64 编不过 %s：\n%s" % (rel, out[-800:]))

    # 防「开着却空 TU」：预处理结果里必须真有装载器的符号。
    rc, out = _run(la64, [x for x in board_flags if x != "-fsyntax-only"] +
                   ["-E", "kernelsrc/src/arkrtos_modld.c"])
    if rc != 0:
        failures.append("B 预处理失败：\n%s" % out[-400:])
    elif "arkrtos_mod_load" not in out:
        failures.append("B 开关开着但预处理结果里没有 arkrtos_mod_load"
                        "（空 TU，宏或 #if 写错了）")

    # --- C. 符号对账（防假绿的核心） ---------------------------------------
    exports = _exports_names()
    if exports is None:
        failures.append("C 符号对账：解析不出 arkrtos_exports.c 的 s_exports 表")
    elif len(exports) == 0:
        failures.append("C 符号对账：s_exports 表是空的")
    else:
        obj = os.path.join(tmp, "modhello.o")
        rc, out = _run(la64, LA64_ARCH + [
            "-O2", "-ffreestanding", "-fno-common", "-nostdlib", "-nostartfiles",
            "-fno-builtin", "-c",
            "example/qemu_loongarch64/mod/modhello.c", "-o", obj])
        if rc != 0:
            failures.append("C 符号对账：示例模块编不过：\n%s" % out[-600:])
        elif not os.path.isfile(nm):
            failures.append("C 符号对账：找不到 %s" % nm)
        else:
            r = subprocess.run([nm, "-u", obj], cwd=ROOT,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            undef = []
            for line in _text(r.stdout).splitlines():
                parts = line.split()
                if not parts:
                    continue
                name = parts[-1]
                if name in NM_ARTIFACTS:
                    continue
                undef.append(name)
            if not undef:
                failures.append("C 符号对账：示例模块没有未定义符号 —— 它证明不了"
                                "「按名字调内核 API」，判据已过期")
            missing = [n for n in undef if n not in exports]
            if missing:
                failures.append("C 符号对账：模块引用了导出表里没有的符号 %s"
                                "（真机上就是 ld rc=-6）" % ", ".join(missing))

    # --- D. 守卫 pin -------------------------------------------------------
    modld = _read(os.path.join(ROOT, "kernelsrc/src/arkrtos_modld.c"))
    pins = [
        ("#define MOD_ET_REL                  (1u)",
         "ET_REL=1：改了它，ET_EXEC 会被当成可下载模块装进来"),
        ("if(m_rd16(s_mod_file + 16) != (uint16)MOD_ET_REL)",
         "输入必须是 ET_REL 的判定（非 ET_REL 要被拒）"),
        ("#error \"ARKRTOS_USE_MODLOAD=1 需要板级给出 ARKRTOS_MOD_AREA_SIZE",
         "缺 ARKRTOS_MOD_AREA_SIZE 必须 #error，不能默认 0 地址"),
        ("c->last_reloc = type;",
         "不认识的重定位类型必须报错并带类型号，不猜编码"),
        ("((addr < c->base) || ((addr + 4u) > (c->base + c->size))",
         "mod_read_word 必须有界（不读到模块映像外）"),
        ("((addr < c->base) || (addr >= (c->base + c->size))",
         "mod_write_word 必须有界（不写到模块映像外）"),
    ]
    for need, why in pins:
        if need not in modld:
            failures.append("D 守卫 pin：arkrtos_modld.c 里找不到 %r（%s）"
                            % (need, why))

    # --- E. Kconfig / 板级接线 --------------------------------------------
    kcfg = _read(os.path.join(ROOT, "config/kconfig/Kconfig.features"))
    if "config ARKRTOS_USE_MODLOAD" not in kcfg:
        failures.append("E Kconfig：没有 config ARKRTOS_USE_MODLOAD")
    else:
        seg = kcfg.split("config ARKRTOS_USE_MODLOAD", 1)[1]
        seg = seg.split("\nconfig ", 1)[0]
        for need, why in (
            ("ARKRTOS_USE_VFS", "依赖 VFS：模块从文件系统读"),
            ("ARKRTOS_USE_PRIV", "依赖特权级：模块跑 PLV0 需要内核态"),
            ("not a replacement", "help 必须写明它是第二种模型、不是替代 ELF"),
        ):
            if need not in seg:
                failures.append("E Kconfig：ARKRTOS_USE_MODLOAD 段里缺 %r（%s）"
                                % (need, why))

    board = _read(os.path.join(ROOT, "board/qemu_loongarch64/arkrtos_board_config.h"))
    if not re.search(r"^#define\s+ARKRTOS_USE_MODLOAD\s+1", board, re.M):
        failures.append("E 板级：qemu_loongarch64 没有把 ARKRTOS_USE_MODLOAD 置 1")
    if not re.search(r"^#define\s+ARKRTOS_MOD_AREA_SIZE\b", board, re.M):
        failures.append("E 板级：qemu_loongarch64 没有 ARKRTOS_MOD_AREA_SIZE")

    shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1

    print("MODLD-GATE OK: 默认档=0 / LA64 打开编得过(2 TU 且非空) / "
          "示例模块未定义符号全在导出表 / 守卫未拆 / Kconfig 与板级接线在")
    return 0


if __name__ == "__main__":
    sys.exit(main())
