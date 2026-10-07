#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Interactive editor for the Ark RTOS Kconfig.

A plain line-based console front end (no curses): it works the same over SSH,
in CI scripts and from a Windows console, and it can be driven from a file of
commands, which is how it stays testable.

Commands
--------
    help                     this text
    list [SUBSTR]            every visible symbol and its current value
    menu                     the menu / comment structure
    show SYMBOL              one symbol with type, help and resolved value
    set SYMBOL VALUE         assign (refused if its dependencies are unmet)
    unset SYMBOL             drop the assignment (fall back to the default)
    save --defconfig FILE    write a defconfig
    save --config FILE       write a .config
    gen OUT                  generate an autoconf.h
    reload                   re-read the input file
    quit

Refusing an unreachable assignment is deliberate: setting FOO=y while its
`depends on` is unmet would produce a value the kernel never sees, and a
silent no-op is exactly the "authoritative wrong answer" this project bans.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kconfig as kc        # noqa: E402
import genconfig as gc      # noqa: E402


class Session(object):
    def __init__(self, kconf, start=None):
        self.kconf = kconf
        self.cfg = kc.Config(kconf)
        self.cfg.user = kc.parse_config(start) if start else {}
        self.cfg.resolve()

    def refresh(self):
        self.cfg.user = {k: v for k, v in self.cfg.user.items()}
        self.cfg.resolve()

    # -- commands ---------------------------------------------------------
    def do_list(self, arg):
        for name in sorted(self.cfg.values):
            s = self.kconf.symbols.get(name)
            if s is not None and not s.visible:
                continue
            if arg and arg not in name:
                continue
            print("  %-34s %s" % (name, self.cfg.values[name]))

    def do_menu(self, _arg):
        for m in self.kconf.menus:
            if m is self.kconf.root:
                continue
            depth = 0
            p = m.parent
            while p is not None and p is not self.kconf.root:
                depth += 1
                p = p.parent
            print("%s[%s]" % ("  " * depth, m.title))
            for name, s in sorted(self.kconf.symbols.items()):
                if s.menu is m and s.visible:
                    print("%s  %s" % ("  " * depth, name))

    def do_show(self, arg):
        s = self.kconf.symbols.get(arg.strip())
        if s is None:
            print("  no such symbol: %s" % arg.strip())
            return
        print("  %s  (%s) = %s" % (arg.strip(), s.type, self.cfg.values.get(arg.strip())))
        for d in s.defaults:
            print("    default %s" % (d[0],))
        for dep in s.depends:
            print("    depends on %r" % (dep,))
        if s.help:
            print("    help: %s" % s.help.strip().splitlines()[0])

    def do_set(self, arg):
        parts = arg.split(None, 1)
        if len(parts) != 2:
            print("  usage: set SYMBOL VALUE")
            return
        name, val = parts[0], parts[1].strip()
        s = self.kconf.symbols.get(name)
        if s is None:
            print("  no such symbol: %s" % name)
            return
        if not self.cfg.symbol_active(s):
            print("  REFUSED: %s dependencies are not satisfied; enable them first" % name)
            return
        self.cfg.user[name] = val
        self.cfg.resolve()
        print("  %s = %s" % (name, self.cfg.values.get(name)))

    def do_unset(self, arg):
        name = arg.strip()
        self.cfg.user.pop(name, None)
        self.cfg.resolve()
        print("  %s = %s (default)" % (name, self.cfg.values.get(name)))

    def do_save(self, arg):
        parts = arg.split(None, 1)
        if len(parts) != 2:
            print("  usage: save --defconfig FILE | save --config FILE")
            return
        kind, path = parts[0], parts[1].strip()
        if kind == "--defconfig":
            text = gc.render_defconfig(self.kconf, self.cfg.values)
        else:
            text = "\n".join("%s=%s" % (k, self.cfg.values[k])
                             for k in sorted(self.cfg.values)) + "\n"
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("  saved %s" % path)

    def do_gen(self, arg):
        path = arg.strip()
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(gc.render(self.kconf, self.cfg.values, "menuconfig"))
        print("  generated %s" % path)


HELP = __doc__.split("Commands", 1)[1]


def run(sess, lines):
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        cmd, _, arg = line.partition(" ")
        if cmd in ("quit", "exit", "q"):
            return False
        if cmd == "help":
            print(HELP.strip())
        elif cmd == "list":
            sess.do_list(arg.strip())
        elif cmd == "menu":
            sess.do_menu(arg.strip())
        elif cmd == "show":
            sess.do_show(arg)
        elif cmd == "set":
            sess.do_set(arg)
        elif cmd == "unset":
            sess.do_unset(arg)
        elif cmd == "save":
            sess.do_save(arg)
        elif cmd == "gen":
            sess.do_gen(arg)
        elif cmd == "reload":
            pass
        else:
            print("  unknown command: %s (try 'help')" % cmd)
    return True


def main(argv):
    ap = argparse.ArgumentParser(description="Ark RTOS Kconfig editor")
    ap.add_argument("--kconfig", default="Kconfig")
    ap.add_argument("--config")
    ap.add_argument("--script", help="file of commands, then exit")
    args = ap.parse_args(argv)

    kconf = kc.load(args.kconfig)
    sess = Session(kconf, args.config)

    print("Ark RTOS menuconfig - type 'help' for commands")
    if args.script:
        with open(args.script, "r", encoding="utf-8") as f:
            run(sess, f)
        return 0

    while True:
        try:
            line = input("menuconfig> ")
        except EOFError:
            break
        if not run(sess, [line]):
            break
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
