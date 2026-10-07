#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Minimal Kconfig parser / evaluator for Ark RTOS.

This is a dependency-free subset of Linux Kconfig.  It exists so the build
configuration has a single, readable, reviewable source (Kconfig) instead of
numbers scattered across headers, and so the C side can stay untouched when
Kconfig is not used at all.

Supported statements
--------------------
    mainmenu "title"
    config NAME / menuconfig NAME
        bool|tristate|int|hex|string ["prompt"] [if COND]
        prompt "..." [if COND]
        default VALUE [if COND]
        depends on EXPR
        select SYM [if COND]
        imply SYM [if COND]
        range MIN MAX [if COND]
        help / ---help---        (indented block, not interpreted)
    menu "title" ... endmenu
    choice [NAME] ... endchoice     (prompt / default / depends)
    if EXPR ... endif
    source "file"
    comment "..."
    option ... / visible if ... / optional    (parsed and ignored)

Expressions
-----------
symbols, !, &&, ||, ( ), =, !=, <, >, <=, >=, y / n / m, integers, quoted
strings.  Precedence from highest to lowest: !, comparison, &&, ||.

Symbol naming
-------------
A Kconfig symbol is named exactly like the C macro it produces (for example
ARKRTOS_USE_FS).  There is deliberately no CONFIG_ prefix and no translation
table: "one name, one meaning", and a grep for a macro finds both the Kconfig
line and the C use.

Value model
-----------
bool symbols resolve to y / n (m is accepted and normalised to y: Ark RTOS has
no modules).  int / hex resolve to a number, string to text.  A symbol whose
dependencies are not satisfied is forced to n / 0 / "".  `select` forces a
bool target to y; `imply` sets a default only.  Resolution is an iterative
fixpoint so forward references and select chains settle.
"""

import os
import re


# --------------------------------------------------------------------------
# expression tokens
# --------------------------------------------------------------------------
_TOKEN_RE = re.compile(r"""
    \s*(?:
        (?P<op>&&|\|\||!=|<=|>=|[!()=<>])
      | (?P<str>"(?:[^"\\]|\\.)*")
      | (?P<num>-?0[xX][0-9A-Fa-f]+|-?\d+)
      | (?P<id>[A-Za-z_][A-Za-z0-9_]*)
    )
""", re.VERBOSE)

_KEYWORDS = {"y": "lit", "n": "lit", "m": "lit"}


class KconfigError(Exception):
    pass


def _tokenize(text):
    out = []
    pos = 0
    while pos < len(text):
        m = _TOKEN_RE.match(text, pos)
        if not m:
            if text[pos:].strip() == "":
                break
            raise KconfigError("bad expression near %r" % text[pos:pos + 20])
        pos = m.end()
        if m.group("op"):
            out.append(("op", m.group("op")))
        elif m.group("str"):
            out.append(("lit", _unquote(m.group("str"))))
        elif m.group("num"):
            out.append(("lit", m.group("num")))
        else:
            name = m.group("id")
            if name in _KEYWORDS:
                out.append(("lit", name))
            else:
                out.append(("sym", name))
    return out


def _unquote(s):
    return s[1:-1].replace('\\"', '"').replace("\\\\", "\\")


def parse_expr(text):
    """Parse an expression string into a tuple tree."""
    toks = _tokenize(text)
    idx = [0]

    def peek():
        return toks[idx[0]] if idx[0] < len(toks) else None

    def take():
        t = peek()
        idx[0] += 1
        return t

    def p_or():
        node = p_and()
        while True:
            t = peek()
            if t == ("op", "||"):
                take()
                node = ("or", node, p_and())
            else:
                return node

    def p_and():
        node = p_not()
        while True:
            t = peek()
            if t == ("op", "&&"):
                take()
                node = ("and", node, p_not())
            else:
                return node

    def p_not():
        t = peek()
        if t == ("op", "!"):
            take()
            return ("not", p_not())
        return p_cmp()

    def p_cmp():
        node = p_atom()
        t = peek()
        if t and t[0] == "op" and t[1] in ("=", "!=", "<", ">", "<=", ">="):
            op = take()[1]
            return ("cmp", op, node, p_atom())
        return node

    def p_atom():
        t = take()
        if t is None:
            raise KconfigError("unexpected end of expression")
        if t[0] == "op" and t[1] == "(":
            node = p_or()
            if take() != ("op", ")"):
                raise KconfigError("missing ')'")
            return node
        if t[0] in ("sym", "lit"):
            return (t[0], t[1])
        raise KconfigError("unexpected token %r" % (t,))

    if not toks:
        return None
    node = p_or()
    if idx[0] != len(toks):
        raise KconfigError("trailing tokens in expression: %r" % (toks[idx[0]:],))
    return node


# --------------------------------------------------------------------------
# model
# --------------------------------------------------------------------------
BOOL, INT, HEX, STRING = "bool", "int", "hex", "string"


class Symbol(object):
    def __init__(self, name):
        self.name = name
        self.type = None
        self.prompts = []          # (text, cond)
        self.defaults = []         # (value_text, cond)
        self.depends = []          # expr nodes
        self.selects = []          # (name, cond)
        self.implies = []          # (name, cond)
        self.ranges = []           # (lo, hi, cond)
        self.help = None
        self.choice = None
        self.menu = None           # Menu that owns the prompt
        self.order = 0
        self._enclosing_deps = []   # conditions inherited from a choice

    @property
    def visible(self):
        return bool(self.prompts)


class Choice(object):
    def __init__(self, name):
        self.name = name
        self.type = None
        self.prompts = []          # (text, cond) - prompt(s) shown for the group
        self.defaults = []         # (symbol_name, cond)
        self.depends = []
        self.members = []
        self.menu = None
        self.order = 0

    @property
    def prompt(self):
        return self.prompts[0][0] if self.prompts else None


class Menu(object):
    def __init__(self, title, depends, parent):
        self.title = title
        self.depends = list(depends)
        self.parent = parent
        self.children = []
        self.order = 0


class Kconfig(object):
    def __init__(self, path=None):
        self.symbols = {}
        self.choices = []
        self.menus = []
        self.mainmenu = None
        self.root = Menu("", [], None)
        self.menus.append(self.root)
        self.path = path

    def symbol(self, name):
        s = self.symbols.get(name)
        if s is None:
            s = Symbol(name)
            s.order = len(self.symbols)
            self.symbols[name] = s
        return s


# --------------------------------------------------------------------------
# parser
# --------------------------------------------------------------------------
class _Parser(object):
    def __init__(self, kc):
        self.kc = kc
        self.if_stack = []         # expr nodes from `if`
        self.menu_stack = [kc.root]
        self.cur = None            # current Symbol
        self.active_choice = None  # Choice currently open, if any
        self.help_indent = None

    # -- helpers ----------------------------------------------------------
    def _flat_depends(self, local):
        """Combine local depends with every enclosing if / menu condition."""
        deps = list(self.if_stack)
        m = self.menu_stack[-1]
        while m is not None:
            deps.extend(m.depends)
            m = m.parent
        deps.extend(local)
        return deps

    def _set_type(self, sym_or_choice, typ, prompt, cond):
        if sym_or_choice.type is not None and sym_or_choice.type != typ:
            raise KconfigError("conflicting type for %s" % sym_or_choice.name)
        sym_or_choice.type = typ
        if prompt is not None:
            sym_or_choice.prompts.append((prompt, cond))

    # -- main loop --------------------------------------------------------
    def parse_file(self, path):
        path = os.path.abspath(path)
        with open(path, "r", encoding="utf-8") as f:
            raw = f.read()
        # join continuation lines
        lines = []
        buf = ""
        for ln in raw.splitlines():
            if ln.rstrip().endswith("\\"):
                buf += ln.rstrip()[:-1] + " "
                continue
            lines.append(buf + ln)
            buf = ""
        if buf:
            lines.append(buf)

        base = os.path.dirname(path)
        i = 0
        while i < len(lines):
            raw_line = lines[i]
            i += 1
            stripped = raw_line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            indent = len(raw_line) - len(raw_line.lstrip())
            # help block: everything indented deeper than the `help` keyword
            if self.help_indent is not None:
                if indent > self.help_indent or not stripped:
                    continue
                self.help_indent = None
            self._statement(stripped, base, lines, i, indent)
            # _statement may consume help lines by returning new index
            i = self._consumed_index(i, lines)

    def _consumed_index(self, i, lines):
        if self.help_indent is None:
            return i
        # skip the indented help block
        while i < len(lines):
            ln = lines[i]
            if ln.strip() and (len(ln) - len(ln.lstrip())) <= self.help_indent:
                break
            i += 1
        self.help_indent = None
        return i

    def _statement(self, s, base, lines, i, indent):
        word = re.split(r"[\s]+", s, 1)[0]

        if word == "mainmenu":
            self.kc.mainmenu = _strip_quotes(s[len("mainmenu"):].strip())
            return
        if word in ("config", "menuconfig"):
            name = s[len(word):].strip()
            self.cur = self.kc.symbol(name)
            if self.active_choice is not None:
                self.active_choice.members.append(name)
                self.cur._enclosing_deps = list(self.active_choice.depends)
                self.cur.choice = self.active_choice
            return
        if word == "menu":
            title = _strip_quotes(s[len("menu"):].strip())
            m = Menu(title, self._flat_depends([]), self.menu_stack[-1])
            m.order = len(self.kc.menus)
            self.kc.menus.append(m)
            self.menu_stack[-1].children.append(m)
            self.menu_stack.append(m)
            self.cur = None
            return
        if word == "endmenu":
            if len(self.menu_stack) > 1:
                self.menu_stack.pop()
            return
        if word == "choice":
            name = s[len("choice"):].strip()
            c = Choice(name or ("CHOICE_%d" % len(self.kc.choices)))
            c.depends = self._flat_depends([])
            c.menu = self.menu_stack[-1]
            c.order = len(self.kc.choices)
            self.kc.choices.append(c)
            self.active_choice = c
            self.cur = None
            return
        if word == "endchoice":
            self.active_choice = None
            self.cur = None
            return
        if word == "if":
            node = parse_expr(s[len("if"):].strip())
            self.if_stack.append(node)
            return
        if word == "endif":
            if self.if_stack:
                self.if_stack.pop()
            return
        if word == "source":
            arg = s[len("source"):].strip()
            arg = _strip_quotes(arg)
            self.parse_file(os.path.join(base, arg))
            return
        if word == "comment":
            return
        if word == "help" or word == "---help---":
            self.help_indent = indent
            return

        # -- properties (inside config / choice) -------------------------
        if self.cur is None and self.active_choice is None:
            # silently accept `option`, `visible if`, `optional`, etc.
            return

        target = self.cur if self.cur is not None else self.active_choice
        if word in (BOOL, "tristate", INT, HEX, STRING):
            typ = BOOL if word == "tristate" else word
            prompt, cond = _split_prompt_cond(s[len(word):].strip())
            self._set_type(target, typ, prompt, cond)
            return
        if word == "prompt":
            prompt, cond = _split_prompt_cond(s[len("prompt"):].strip())
            self._set_type(target, BOOL if target.type is None else target.type,
                           prompt, cond)
            return
        if word == "def_bool" or word == "def_tristate":
            val, cond = _split_value_cond(s[len(word):].strip())
            target.type = target.type or BOOL
            target.defaults.append((val, cond))
            return
        if word == "default":
            val, cond = _split_value_cond(s[len("default"):].strip())
            if isinstance(target, Choice):
                target.defaults.append((_strip_quotes(val), cond))
            else:
                target.defaults.append((_strip_quotes(val), cond))
            return
        if word == "depends":
            node = parse_expr(s[len("depends on"):].strip())
            target.depends.append(node)
            return
        if word == "select" or word == "imply":
            rest = s[len(word):].strip()
            val, cond = _split_value_cond(rest)
            name = val.strip()
            if isinstance(target, Symbol):
                (target.selects if word == "select" else target.implies).append(
                    (name, cond))
            return
        if word == "range":
            rest = s[len("range"):].strip()
            head, cond = _split_if(rest)
            parts = head.split()
            if len(parts) >= 2 and isinstance(target, Symbol):
                target.ranges.append((parts[0], parts[1], cond))
            return
        if word == "option" or word == "visible" or word == "optional":
            return
        # anything else: ignore, but keep a note in help
        return


def _strip_quotes(s):
    s = s.strip()
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
        return s[1:-1]
    return s


def _split_if(text):
    """Split '<head> if <cond>' into (head, cond_expr_or_None)."""
    m = re.search(r"\s+if\s+", text)
    if not m:
        return text.strip(), None
    return text[:m.start()].strip(), parse_expr(text[m.end():].strip())


def _split_prompt_cond(text):
    head, cond = _split_if(text)
    return (_strip_quotes(head) if head else None), cond


def _split_value_cond(text):
    return _split_if(text)


# --------------------------------------------------------------------------
# evaluation
# --------------------------------------------------------------------------
class Config(object):
    """Holds resolved values and answers dependency / expression questions."""

    def __init__(self, kc):
        self.kc = kc
        self.values = {}          # name -> str
        self.user = {}            # name -> str (from .config)
        self.forced = {}          # name -> str (from select)
        self._choice_pick = {}    # choice name -> member name
        self._active_memo = {}

    # -- basic accessors --------------------------------------------------
    def raw(self, name):
        if name in self.values:
            return self.values[name]
        s = self.kc.symbols.get(name)
        if s is None:
            return "n"
        if s.type in (INT, HEX):
            return "0"
        if s.type == STRING:
            return ""
        return "n"

    def truthy(self, name):
        return self.raw(name) not in ("n", "", "0")

    # -- dependency / expression evaluation -------------------------------
    def deps_ok(self, deps):
        for d in deps:
            if d is None:
                continue
            if not self.eval_bool(d):
                return False
        return True

    def eval_bool(self, node):
        t = node[0]
        if t == "sym":
            return self.truthy(node[1])
        if t == "lit":
            return node[1] not in ("n", "0", "")
        if t == "not":
            return not self.eval_bool(node[1])
        if t == "and":
            return self.eval_bool(node[1]) and self.eval_bool(node[2])
        if t == "or":
            return self.eval_bool(node[1]) or self.eval_bool(node[2])
        if t == "cmp":
            return self._cmp(node[1], self.eval_value(node[2]),
                             self.eval_value(node[3]))
        raise KconfigError("cannot evaluate %r" % (node,))

    def eval_value(self, node):
        if node[0] == "sym":
            return self.raw(node[1])
        if node[0] == "lit":
            return node[1]
        return "y" if self.eval_bool(node) else "n"

    @staticmethod
    def _cmp(op, a, b):
        def num(x):
            try:
                return int(x, 0)
            except (TypeError, ValueError):
                return None
        na, nb = num(a), num(b)
        if na is not None and nb is not None:
            a, b = na, nb
        if op == "=":
            return a == b
        if op == "!=":
            return a != b
        if op == "<":
            return a < b
        if op == ">":
            return a > b
        if op == "<=":
            return a <= b
        if op == ">=":
            return a >= b
        raise KconfigError("bad comparison %r" % op)

    # -- per-symbol resolution --------------------------------------------
    def symbol_active(self, s):
        """True if the symbol's own dependencies (incl. enclosing) are met."""
        deps = list(s.depends)
        if s.choice is not None:
            deps = list(getattr(s, "_enclosing_deps", [])) + deps
        return self.deps_ok(deps)

    def _choice_defaults(self):
        """Which member of each choice wins when .config does not say."""
        out = {}
        for c in self.kc.choices:
            if not self.deps_ok(c.depends):
                continue
            picked = None
            for m in c.members:
                if self.user.get(m) == "y":
                    picked = m
                    break
            if picked is None:
                for name, cond in c.defaults:
                    if name in c.members and (cond is None or self.eval_bool(cond)):
                        picked = name
                        break
            out[c.name] = picked
        return out

    def _compute(self, s):
        active = self.symbol_active(s)
        typ = s.type or BOOL
        if s.choice is not None:
            if not self.deps_ok(s.choice.depends):
                return "n"
            if s.name in self.user:
                return self.user[s.name]
            return "y" if self._choice_pick.get(s.choice.name) == s.name else "n"
        if not active:
            return "n" if typ == BOOL else ("0" if typ in (INT, HEX) else "")
        # user override wins when the symbol is visible
        if s.name in self.user:
            return self.user[s.name]
        # select forces a bool target high
        if typ == BOOL and self.forced.get(s.name) == "y":
            return "y"
        # first satisfied default
        for val, cond in s.defaults:
            if cond is None or self.eval_bool(cond):
                return val
        return "n" if typ == BOOL else ("0" if typ in (INT, HEX) else "")

    def _apply_ranges(self, s, val):
        for lo, hi, cond in s.ranges:
            if cond is not None and not self.eval_bool(cond):
                continue
            try:
                v, a, b = int(val, 0), int(lo, 0), int(hi, 0)
            except ValueError:
                continue
            if v < a:
                return lo
            if v > b:
                return hi
        return val

    def resolve(self, rounds=64):
        for s in self.kc.symbols.values():
            if s.type is None:
                s.type = BOOL
        for _ in range(rounds):
            changed = False
            self._choice_pick = self._choice_defaults()
            # select propagation
            self.forced = {}
            for s in self.kc.symbols.values():
                if self.truthy(s.name):
                    for target, cond in s.selects:
                        if cond is None or self.eval_bool(cond):
                            if self.forced.get(target) != "y":
                                self.forced[target] = "y"
                                changed = True
            for name, s in self.kc.symbols.items():
                new = self._compute(s)
                if s.type in (INT, HEX):
                    new = self._apply_ranges(s, new)
                if self.values.get(name) != new:
                    self.values[name] = new
                    changed = True
            if not changed:
                break
        return self.values


# --------------------------------------------------------------------------
# load / generate
# --------------------------------------------------------------------------
def load(kconfig_path):
    kc = Kconfig(kconfig_path)
    p = _Parser(kc)
    p.parse_file(kconfig_path)
    # attach choice membership: a config declared inside a choice block
    # is linked by the parser via cur_choice handling below.
    _link_choices(kc)
    return kc


def _link_choices(kc):
    # the parser records members as it walks; rebuild from stored links
    for c in kc.choices:
        for name in c.members:
            s = kc.symbols.get(name)
            if s is not None:
                s.choice = c
                s._enclosing_deps = c.depends


def parse_config(path):
    """Parse a .config file into an ordered dict of assignments."""
    out = {}
    if not path or not os.path.exists(path):
        return out
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            m = re.match(r"^(?:CONFIG_)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$", line)
            if not m:
                continue
            key, val = m.group(1), m.group(2).strip()
            if len(val) >= 2 and val[0] == '"' and val[-1] == '"':
                val = val[1:-1]
            out[key] = val
    return out
