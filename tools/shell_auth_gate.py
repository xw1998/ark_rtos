#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""控制台登录（ARKRTOS_USE_SHELL_AUTH / SEC-G01）的门禁。

为什么需要这一道
----------------
这个开关打开以后走的是「先验口令、再执行命令」，它有三类假绿是默
认档看不出来的：

  1. 门钩没接上：`ark_shell_set_gate()` 定义了但没人调，或者门钩挂在
     `ark_shell_find_command()` **之后**——后者还留了一条旁路：未知词
     会掉到 exec 回退里，门钩等于没拦住。默认档里这些代码一行都不编，
     全绿。
  2. 守卫只是注释：开档却没给盐/摘要，编出来的镜像「没人能登录」；
     或者给了个通用默认口令，「谁都能登录」。两种都是假绿。
  3. 关档不干净：开关关掉以后还留着符号、还生成凭据头、还加 -D，
     那「默认档零变化」就只是说说。

这里把三类都做成读数：
  A. 开关三处（features.h 缺省 0 / Kconfig 两项 / Makefile 只在 EXTRA_CFLAGS
     显式开档时生成凭据头并加 -D）。
  B. 关档零符号（真编 .o 用 nm 看）：`arkrtos_shell_auth.c` 关档编出来是空
     TU；`ark_shell.c` 关档没有 `ark_shell_set_gate`；`ark_shell_eval.c` 关档
     不引用 `arkrtos_shell_auth_gate`。开档三样都在。
  C. LA64 正向：`arkrtos_shell_auth.c` / `ark_shell.c` / `ark_shell_eval.c`
     带开关真编得过。
  D. 三条 #error 真会触发，且各自点名：无盐 / 有盐无摘要 / 没有 shell。
  E. 常量时间比对：32 字节循环 XOR 累加，循环里不许有提前 return。
  F. 门钩位置：`ark_shell_eval.c` 里 gate 调用必须在 `ark_shell_find_command`
     之前；放行名单只有 login / logout / help。
  G. 失败上限与硬锁：`s_auth_fails >= ARKRTOS_SHELL_AUTH_TRIES` 之后置
     `s_auth_locked_hard`，且硬锁后正确口令也被拒。
  H. 命令注册：login / logout 在 AUTH 条件里注册进命令表，容量预算 +2。
  I. 接线：开机即锁（init 置 locked=1）、shell 任务里调 init + set_gate 并
     在横幅后打印「console: locked」。
  J. 凭据自洽：Makefile 里那份演示盐/摘要必须与 tools/shell_auth_cred.py
     现算的一致（防止改了盐忘了改摘要）。
  K. 凭据头不入库：.gitignore 覆盖 `*_shell_auth_local.h`，且没有任何被
     跟踪的同名文件。

E–K 每条都自带**防假绿自检**：把源串故意改坏一次，对应 pin 必须报错；
报不出错就说明这条 pin 是空转的。

不覆盖：QEMU / 板上「登录前后命令真的被拒 / 真的能跑」的行为读数（那是
实跑，见 docs/QEMU模拟器移植与验证.md 的 §5.27），也不覆盖「口令有多强」
（那是运维选口令的事）。

用法
----
    python tools/shell_auth_gate.py
    python tools/shell_auth_gate.py --la64-gcc <path>

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
]

INCLUDES = [
    "-Ikernelsrc/include",
    "-Iconfig",
    "-Iboard/qemu_loongarch64",
    "-Ikernelsrc/port/loongarch/la64",
    "-Ikernelsrc/shell",
    "-Ikernelsrc/shell/ark_shell",
]

# 与 example/qemu_loongarch64/Makefile 里的演示凭据同值。门禁用自己的副本，
# 不去读构建目录（构建目录可能还没生成，那就成了「先有鸡还是先有蛋」）。
DEMO_SALT = "61726b2d72746f732d636f6e736f6c65"          # "ark-rtos-console"
DEMO_PASSWORD = "arkrtos-demo"
DEMO_DIGEST = ("04E5F69B6F84B7ABD75F71A9A89CDA92"
               "12D76556278BD260E288149A932D8716")

SIGN_KEY_HEADER = """#ifndef ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_IMAGE_SIGN_KEY { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F }
#endif
"""

AUTH_HEADER = """#ifndef ARKRTOS_SHELL_AUTH_LOCAL_H
#define ARKRTOS_SHELL_AUTH_LOCAL_H
#define ARKRTOS_SHELL_AUTH_SALT { 0x61, 0x72, 0x6B, 0x2D, 0x72, 0x74, 0x6F, 0x73, 0x2D, 0x63, 0x6F, 0x6E, 0x73, 0x6F, 0x6C, 0x65 }
#define ARKRTOS_SHELL_AUTH_DIGEST { 0x04, 0xE5, 0xF6, 0x9B, 0x6F, 0x84, 0xB7, 0xAB, 0xD7, 0x5F, 0x71, 0xA9, 0xA8, 0x9C, 0xDA, 0x92, 0x12, 0xD7, 0x65, 0x56, 0x27, 0x8B, 0xD2, 0x60, 0xE2, 0x88, 0x14, 0x9A, 0x93, 0x2D, 0x87, 0x16 }
#endif
"""


def _pick(cands, given, what):
    if given:
        return given
    for c in cands:
        if os.path.isfile(c):
            return c
    print("FAIL: 找不到 %s（缺编译器是 FAIL，不是跳过）" % what)
    sys.exit(1)


def _text(raw):
    """内核头文件是 GBK，gcc 把源里的中文原样回显，所以两种编码都要试。"""
    if not raw:
        return ""
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def _run(cmd):
    r = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    return r.returncode, _text(r.stdout) + _text(r.stderr)


def _src(rel):
    with open(os.path.join(ROOT, rel), "rb") as f:
        # 归一化行尾：pin 是拿源码串做判据的，CRLF 会让每条 anchor 都对不上。
        return f.read().decode("utf-8", "replace").replace("\r\n", "\n")


def _body(text, sig):
    """从签名的第一个 '{' 起按大括号配平截出函数体；找不到返回空串。"""
    i = text.find(sig)
    if i < 0:
        return ""
    j = text.find("{", i)
    if j < 0:
        return ""
    depth = 0
    for k in range(j, len(text)):
        if text[k] == "{":
            depth += 1
        elif text[k] == "}":
            depth -= 1
            if depth == 0:
                return text[j:k + 1]
    return ""


# --------------------------------------------------------------- 源码 pin
#
# 每个 pin 都只吃「源码文本」，返回问题列表；这样同一批函数既能检查工作
# 区，也能检查被故意改坏的副本（自检）。

def pin_a(feats, kconf, mk):
    bad = []
    if "#define ARKRTOS_USE_SHELL_AUTH          0" not in feats:
        bad.append("A: arkrtos_features.h 里 ARKRTOS_USE_SHELL_AUTH 的缺省不是 0")
    if "#define ARKRTOS_SHELL_AUTH_TRIES        5" not in feats:
        bad.append("A: arkrtos_features.h 里 ARKRTOS_SHELL_AUTH_TRIES 的缺省不是 5")
    if "config ARKRTOS_USE_SHELL_AUTH" not in kconf:
        bad.append("A: Kconfig.features 里没有 ARKRTOS_USE_SHELL_AUTH")
    def block(name):
        i = kconf.find("config " + name)
        if i < 0:
            return ""
        j = kconf.find("\nconfig ", i + 1)
        return kconf[i:] if j < 0 else kconf[i:j]

    if "depends on ARKRTOS_USE_SHELL\n" not in block("ARKRTOS_USE_SHELL_AUTH"):
        bad.append("A: Kconfig 里 ARKRTOS_USE_SHELL_AUTH 没有 depends on ARKRTOS_USE_SHELL")
    tries = block("ARKRTOS_SHELL_AUTH_TRIES")
    if not tries:
        bad.append("A: Kconfig 里没有 ARKRTOS_SHELL_AUTH_TRIES")
    else:
        if "depends on ARKRTOS_USE_SHELL_AUTH" not in tries:
            bad.append("A: Kconfig 里 TRIES 没有 depends on ARKRTOS_USE_SHELL_AUTH")
        if "range 1 100" not in tries:
            bad.append("A: Kconfig 里 TRIES 没有 range 1 100")
    if "ARKRTOS_USE_SHELL_AUTH=1,$(EXTRA_CFLAGS)" not in mk:
        bad.append("A: Makefile 没有按 EXTRA_CFLAGS 判断开档（ARK_AUTH_ON）")
    if "ARK_AUTH_FLAGS := -DARKRTOS_SHELL_AUTH_LOCAL=1" not in mk:
        bad.append("A: Makefile 开档时没有加 -DARKRTOS_SHELL_AUTH_LOCAL=1")
    if "$(OBJS): $(SHELL_AUTH_HDR)" not in mk:
        bad.append("A: Makefile 没有让目标文件依赖凭据头（会用到上一次的旧头）")
    return bad


def pin_e(auth):
    bad = []
    body = _body(auth, "static int auth_digest_equal(")
    if not body:
        bad.append("E: 找不到 auth_digest_equal 的定义")
        return bad
    if "for(i = 0u; i < n; i++)" not in body:
        bad.append("E: digest 比对不是 32 字节整段循环（长度就是信息）")
    if "diff |=" not in body:
        bad.append("E: digest 比对没有用 XOR 累加（会退化成逐字节提前返回）")
    loop = body[body.find("for("):]
    if "return" in loop.split("diff |=", 1)[0]:
        bad.append("E: digest 比对的循环里出现了提前 return")
    if "ARKRTOS_SHA256_LEN" not in _body(auth, "int arkrtos_shell_auth_cmd_login("):
        bad.append("E: login 没有按 SHA-256 全长比对")
    return bad


def pin_f(eval_src, auth):
    bad = []
    g = eval_src.find("shell->gate_fn(xargv[0])")
    c = eval_src.find("cmd = ark_shell_find_command(xargv[0]);")
    if g < 0:
        bad.append("F: ark_shell_eval.c 里没有门钩调用")
    if c < 0:
        bad.append("F: ark_shell_eval.c 里找不到 ark_shell_find_command 调用，判据已过期")
    if g >= 0 and c >= 0 and g > c:
        bad.append("F: 门钩挂在查找之后（exec 回退那条旁路没被拦住）")
    exempt = _body(auth, "static int auth_exempt(")
    for n in ("login", "logout", "help"):
        if '"%s"' % n not in exempt:
            bad.append("F: 放行名单里缺 %s" % n)
    for line in exempt.splitlines():
        m = re.findall(r'auth_streq\(name, "([a-z]+)"\)', line)
        for word in m:
            if word not in ("login", "logout", "help"):
                bad.append("F: 放行名单多放了 %s" % word)
    return bad


def pin_g(auth):
    bad = []
    login = _body(auth, "int arkrtos_shell_auth_cmd_login(")
    if "s_auth_fails >= ARKRTOS_SHELL_AUTH_TRIES" not in login:
        bad.append("G: 失败计数没有对着 ARKRTOS_SHELL_AUTH_TRIES 判上限")
    if "s_auth_locked_hard = 1" not in login:
        bad.append("G: 到了上限没有置硬锁")
    hard = login.find("if(s_auth_locked_hard != 0)")
    cmp_pw = login.find("auth_digest_equal(")
    if hard < 0 or cmp_pw < 0 or hard > cmp_pw:
        bad.append("G: 硬锁判断不在口令比对之前（锁死以后还能试）")
    gate = _body(auth, "int arkrtos_shell_auth_gate(")
    if "(s_auth_locked_hard != 0)" not in gate:
        bad.append("G: 门钩没有单独区分硬锁")
    return bad


def pin_h(shell):
    bad = []
    reg = 'ARK_SHELL_CMD("login"'
    i = shell.find(reg)
    if i < 0:
        bad.append("H: shell 没有注册 login 命令")
    else:
        guard = shell.rfind("#if (ARKRTOS_USE_SHELL_AUTH == 1)", 0, i)
        endl = shell.rfind("#endif", 0, i)
        if guard < 0 or endl > guard:
            bad.append("H: login 的注册不在 ARKRTOS_USE_SHELL_AUTH 条件里")
    if 'ARK_SHELL_CMD("logout"' not in shell:
        bad.append("H: shell 没有注册 logout 命令")
    if "((ARKRTOS_USE_SHELL_AUTH == 1) ? 2 : 0)" not in shell:
        bad.append("H: 命令表容量预算没有算上 login/logout 两条")
    return bad


def pin_i(auth, shell):
    bad = []
    init = _body(auth, "void arkrtos_shell_auth_init(")
    if "s_auth_locked      = 1" not in init:
        bad.append("I: init 没有把控制台置成锁定（开机即锁）")
    task = _body(shell, "static void arkrtos_shell_task(")
    if "arkrtos_shell_auth_init();" not in task:
        bad.append("I: shell 任务没有初始化登录状态")
    if "ark_shell_set_gate(&g_shell, arkrtos_shell_auth_gate)" not in task:
        bad.append("I: shell 任务没有把门钩挂上去（门钩定义了也没人用）")
    if "console: locked" not in task:
        bad.append("I: 开机横幅后没有告诉运维「控制台已锁」")
    return bad


def pin_j(mk):
    bad = []
    m = re.search(r"ARKRTOS_SHELL_AUTH_SALT \{([^}]*)\}", mk)
    d = re.search(r"ARKRTOS_SHELL_AUTH_DIGEST \{([^}]*)\}", mk)
    if not m or not d:
        bad.append("J: Makefile 里找不到演示盐/摘要")
        return bad
    salt = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
    dig = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", d.group(1)))
    if salt.hex() != DEMO_SALT:
        bad.append("J: Makefile 的演示盐变了（%s），门禁里那份要一起改" % salt.hex())
    if dig.hex().upper() != DEMO_DIGEST:
        bad.append("J: Makefile 的演示摘要是 %s，与门禁里那份不符"
                   % dig.hex().upper())
    return bad


def pin_k(gi, tracked):
    bad = []
    if "*_shell_auth_local.h" not in gi:
        bad.append("K: .gitignore 没有忽略凭据头（*_shell_auth_local.h）")
    hits = [t for t in tracked if t.endswith("_shell_auth_local.h")]
    if hits:
        bad.append("K: 凭据头已经被跟踪: %s" % ", ".join(hits))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--la64-gcc", default="")
    a = ap.parse_args()

    la64 = _pick(LA64_FALLBACKS, a.la64_gcc, "LoongArch64 交叉编译器")
    nm = os.path.join(os.path.dirname(la64), "loongarch64-newlib-elf-nm.exe")
    if not os.path.isfile(nm):
        nm = "loongarch64-newlib-elf-nm"

    tmp = tempfile.mkdtemp(prefix="ark_shell_auth_")
    inc = os.path.join(tmp, "inc")
    os.makedirs(inc)
    for name, body in (("arkrtos_sign_key_local.h", SIGN_KEY_HEADER),
                       ("arkrtos_shell_auth_local.h", AUTH_HEADER)):
        with open(os.path.join(inc, name), "w", encoding="utf-8",
                  newline="\n") as f:
            f.write(body)

    base = LA64_ARCH + COMMON + [
        "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
        "-include", "arkrtos_board_config.h",
        "-DARKRTOS_SIGN_KEY_LOCAL=1",
        "-I" + inc,
    ] + INCLUDES
    on = base + ["-DSHELL_ENABLE=1", "-DARKRTOS_USE_SHELL_AUTH=1",
                 "-DARKRTOS_SHELL_AUTH_LOCAL=1"]
    off = base + ["-DSHELL_ENABLE=1", "-DARKRTOS_USE_SHELL_AUTH=0"]

    failures = []

    src_auth = "kernelsrc/src/arkrtos_shell_auth.c"
    src_eval = "kernelsrc/shell/ark_shell/ark_shell_eval.c"
    src_core = "kernelsrc/shell/ark_shell/ark_shell.c"
    src_shell = "kernelsrc/shell/arkrtos_shell.c"

    # --- C. LA64 正向：三处带开关真编 -------------------------------
    for rel in (src_auth, src_core, src_eval):
        rc, out = _run([la64] + on + [rel])
        if rc != 0:
            failures.append("C LA64 正向：%s 带 AUTH=1 编不过：\n%s"
                            % (rel, out[-600:]))

    # --- B. 关档零符号 / 开档有符号（真编 .o + nm）-------------------
    def compile_obj(rel, flags, tag):
        obj = os.path.join(tmp, tag + ".o")
        args = [f for f in flags if f != "-fsyntax-only"] + \
            ["-c", "-o", obj, rel]
        rc, out = _run([la64] + args)
        return rc, obj, out

    def symbols(obj):
        rc, out = _run([nm, obj])
        if rc != 0:
            return None
        return out

    rc, obj, out = compile_obj(src_auth, off, "auth_off")
    if rc != 0:
        failures.append("B 关档：%s 关档编不过：\n%s" % (src_auth, out[-400:]))
    else:
        syms = symbols(obj)
        if syms is None:
            failures.append("B 关档：nm 读不了 %s" % obj)
        elif "arkrtos_shell_auth" in syms:
            failures.append("B 关档：关档镜像里仍有 arkrtos_shell_auth_* 符号")

    checks = [
        ("ark_shell_set_gate", src_core, "core"),
    ]
    for sym, rel, tag in checks:
        rc, obj, out = compile_obj(rel, off, tag + "_off")
        if rc != 0:
            failures.append("B 关档：%s 关档编不过：\n%s" % (rel, out[-400:]))
        else:
            syms = symbols(obj)
            if syms is not None and sym in syms:
                failures.append("B 关档：%s 关档仍带 %s（关档不干净）" % (rel, sym))
        rc, obj, out = compile_obj(rel, on, tag + "_on")
        if rc != 0:
            failures.append("B 开档：%s 带 AUTH=1 编不过：\n%s" % (rel, out[-400:]))
        else:
            syms = symbols(obj)
            if syms is None or sym not in syms:
                failures.append("B 开档：%s 带 AUTH=1 没有 %s（开关没接上）"
                                % (rel, sym))

    # eval 的门钩是「函数指针调用」，目标文件里不会出现 arkrtos_shell_auth_gate
    # 这个符号（谁接到这个指针上是 arkrtos_shell.c 的事），所以在这里用预处理
    # 出来的源码判断：开档必须留下调用点，关档必须一点不剩。
    def preprocess(flags):
        rc, out = _run([la64] + flags + ["-E", src_eval])
        return rc, out

    rc, pp_on = preprocess(on)
    if rc != 0:
        failures.append("B 开档：%s 预处理失败" % src_eval)
    elif "gate_fn(xargv[0])" not in pp_on:
        failures.append("B 开档：预处理后门钩调用点不见了（开关没接上）")
    rc, pp_off = preprocess(off)
    if rc != 0:
        failures.append("B 关档：%s 预处理失败" % src_eval)
    elif "gate_fn(xargv[0])" in pp_off:
        failures.append("B 关档：关档预处理后仍有门钩调用点（关档不干净）")

    # --- D. 三条 #error 真会触发且各自点名 ---------------------------
    neg = [
        ("无盐", ["-DSHELL_ENABLE=1", "-DARKRTOS_USE_SHELL_AUTH=1"],
         "ARKRTOS_SHELL_AUTH_SALT"),
        ("有盐无摘要",
         ["-DSHELL_ENABLE=1", "-DARKRTOS_USE_SHELL_AUTH=1",
          "-DARKRTOS_SHELL_AUTH_SALT={0x61}"],
         "ARKRTOS_SHELL_AUTH_DIGEST"),
        ("没有 shell",
         ["-DSHELL_ENABLE=0", "-DARKRTOS_USE_SHELL_AUTH=1",
          "-DARKRTOS_SHELL_AUTH_SALT={0x61}",
          "-DARKRTOS_SHELL_AUTH_DIGEST={0x04}"],
         "SHELL_ENABLE"),
    ]
    for tag, extra, want in neg:
        rc, out = _run([la64] + base + extra + [src_auth])
        if rc == 0:
            failures.append("D 负向(%s)：缺条件竟然编过了（守卫是装饰）" % tag)
        elif want not in out:
            failures.append("D 负向(%s)：编不过但不是那条 #error：\n%s"
                            % (tag, out[-400:]))

    # --- 源码 pin ----------------------------------------------------
    feats = _src("kernelsrc/include/arkrtos_features.h")
    kconf = _src("config/kconfig/Kconfig.features")
    mk = _src("example/qemu_loongarch64/Makefile")
    auth = _src(src_auth)
    evalsrc = _src(src_eval)
    shell = _src(src_shell)
    gi = _src(".gitignore")
    rc, out = _run([git_exe(), "ls-files"])
    tracked = out.splitlines() if rc == 0 else []

    pins = [
        ("A", pin_a(feats, kconf, mk)),
        ("E", pin_e(auth)),
        ("F", pin_f(evalsrc, auth)),
        ("G", pin_g(auth)),
        ("H", pin_h(shell)),
        ("I", pin_i(auth, shell)),
        ("J", pin_j(mk)),
        ("K", pin_k(gi, tracked)),
    ]
    for tag, bad in pins:
        for b in bad:
            failures.append(b)

    # --- J 第二半：用 tools/shell_auth_cred.py 现算一遍 ----------------
    r = subprocess.run([sys.executable,
                        os.path.join("tools", "shell_auth_cred.py"),
                        "--salt", DEMO_SALT,
                        "--password", DEMO_PASSWORD,
                        "--check", DEMO_DIGEST],
                       cwd=ROOT, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if r.returncode != 0:
        failures.append("J: tools/shell_auth_cred.py 复算演示凭据不匹配：%s"
                        % _text(r.stdout + r.stderr).strip()[-300:])
    r = subprocess.run([sys.executable,
                        os.path.join("tools", "shell_auth_cred.py"),
                        "--salt", DEMO_SALT,
                        "--password", "not-the-password",
                        "--check", DEMO_DIGEST],
                       cwd=ROOT, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if r.returncode == 0:
        failures.append("J: 凭据脚本对错口令也报 match（比对本身是假绿）")

    # --- 防假绿自检：把源改坏，对应 pin 必须报错 ----------------------
    def mutated(text, old, new, count=1):
        return text.replace(old, new, count) if old in text else text

    selfchecks = [
        ("A-开关", pin_a(mutated(feats, "#define ARKRTOS_USE_SHELL_AUTH          0",
                                 "#define ARKRTOS_USE_SHELL_AUTH          1"),
                         kconf, mk)),
        ("A-Kconfig", pin_a(feats,
                            mutated(kconf, "\tdepends on ARKRTOS_USE_SHELL\n",
                                    "\tdepends on ARKRTOS_USE_SHELL_AUTH\n"),
                            mk)),
        ("A-Makefile", pin_a(feats, kconf,
                             mutated(mk, "ARK_AUTH_FLAGS := -DARKRTOS_SHELL_AUTH_LOCAL=1",
                                     "ARK_AUTH_FLAGS :="))),
        ("E", pin_e(mutated(auth, "        diff |= (uint32)(a[i] ^ b[i]);",
                            "        if(a[i] != b[i]) { return 0; }"))),
        ("E-长度", pin_e(mutated(auth, "for(i = 0u; i < n; i++)",
                                 "for(i = 0u; i < 4u; i++)"))),
        ("F-名单", pin_f(evalsrc, mutated(auth, 'auth_streq(name, "help") != 0',
                                          'auth_streq(name, "reboot") != 0'))),
        ("G-上限", pin_g(mutated(auth, "s_auth_fails >= ARKRTOS_SHELL_AUTH_TRIES",
                                 "s_auth_fails >= 99"))),
        ("G-硬锁", pin_g(mutated(auth, "s_auth_locked_hard = 1", "s_auth_locked_hard = 0"))),
        ("H-注册", pin_h(mutated(shell, 'ARK_SHELL_CMD("login"', 'ARK_SHELL_CMD("signin"'))),
        ("H-预算", pin_h(mutated(shell, "((ARKRTOS_USE_SHELL_AUTH == 1) ? 2 : 0)",
                                 "((ARKRTOS_USE_SHELL_AUTH == 1) ? 0 : 0)"))),
        ("I-门钩", pin_i(auth, mutated(shell, "ark_shell_set_gate(&g_shell, arkrtos_shell_auth_gate)",
                                       ""))),
        ("I-开机即锁", pin_i(mutated(auth, "\n    s_auth_locked      = 1;",
                                     "\n    s_auth_locked      = 0;"), shell)),
        ("J", pin_j(mutated(mk, "0x04, 0xE5, 0xF6", "0x04, 0xE5, 0xF7"))),
        ("K", pin_k(mutated(gi, "*_shell_auth_local.h", "*_sign_key_local.h"), tracked)),
    ]
    # F-位置那一条要真的把门钩搬到查找之后：上面那种「加注释」改不动位置，
    # 所以单独做一次顺序对调。
    gate_block = """#if ARK_SHELL_ENABLE_GATE
    /* Console gate: before the lookup, so it covers the command table and the
     * exec fallback below alike.  126 is "could not execute". */
    if ((shell->gate_fn != NULL) && (shell->gate_fn(xargv[0]) != 0)) {
        eval_set_status(shell, 126);
        return 126;
    }
#endif

"""
    if gate_block in evalsrc:
        moved = evalsrc.replace(gate_block, "", 1)
        moved = moved.replace("    cmd = ark_shell_find_command(xargv[0]);",
                              "    cmd = ark_shell_find_command(xargv[0]);\n" +
                              gate_block, 1)
        selfchecks.append(("F-顺序", pin_f(moved, auth)))
    else:
        failures.append("F: 门钩那段的原文对不上，位置自检做不了（判据已过期）")

    for tag, bad in selfchecks:
        if not bad:
            failures.append("自检 %s：把源改坏后这条 pin 仍然通过（pin 是空转的）"
                            % tag)

    shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1

    print("SHELL-AUTH-GATE OK: 开关三处 / 关档零符号(nm) / LA64 正向 3 TU / "
          "三条 #error 负向被拒并点名 / 常量时间比对 E pin / 门钩位置与放行名单 F pin / "
          "失败上限与硬锁 G pin / 命令注册与预算 H pin / 开机接线 I pin / "
          "演示凭据自洽 J pin / 凭据头不入库 K pin —— 全部自带防假绿自检")
    return 0


def git_exe():
    for cand in ("git", "git.exe"):
        for p in os.environ.get("PATH", "").split(os.pathsep):
            f = os.path.join(p, cand)
            if os.path.isfile(f):
                return f
    return "git"


if __name__ == "__main__":
    sys.exit(main())
