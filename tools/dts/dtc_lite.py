#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dtc_lite - a small Device Tree compiler for Ark RTOS.

It reads a DTS subset and emits a self-contained C file (one per board) that
defines a flat device table plus the query API declared in
kernelsrc/include/arkrtos_dt.h.

Why a subset instead of a full dtc
----------------------------------
Ark RTOS boards need exactly one thing from the tree: a list of devices, each
with a compatible string, a register window and a handful of properties, that a
driver can look up at run time.  There is no MMU, no address translation, no
interrupt controller hierarchy to describe.  This compiler therefore supports
what that needs and rejects the rest loudly:

  /dts-v1/;                       version marker (accepted and ignored)
  /include/ "file"  or  #include "file"
  / { ... };                      root
  name[@unit] { ... };            node
  label: name { ... };            labelled node
  &label { ... };                 overlay: merge properties into a node
  /delete-node/ name;             remove a child
  /delete-property/ name;         remove a property
  prop = "str", "str2";           string property
  prop = <0x1234 5 &other>;       cell property (a &ref becomes a phandle)
  prop = [00 11 22];              byte string property
  /memreserve/ ...                accepted and ignored

Unknown directives raise, so a typo cannot silently produce an empty tree.

Nodes are emitted for *every* node except the root, with `is_device` set when a
`compatible` property is present.  Consumers that bind drivers use
arkrtos_dt_find()/arkrtos_dt_find_next(); structural nodes are still present so
that a phandle reference always resolves.
"""

import argparse
import os
import re
import sys


class DtsError(Exception):
    pass


# --------------------------------------------------------------------------
# tokenizer
# --------------------------------------------------------------------------
_TOKEN_RE = re.compile(r"""
    (?P<ws>\s+)
  | (?P<lc>//[^\n]*)
  | (?P<bc>/\*.*?\*/)
  | (?P<str>"(?:[^"\\]|\\.)*")
  | (?P<num>-?(?:0[xX][0-9A-Fa-f]+|\d+))
  | (?P<id>[A-Za-z_#][A-Za-z0-9_\-#]*)
  | (?P<punct>[]{};=<>[&:,@/])
""", re.VERBOSE | re.DOTALL)


def tokenize(text, where):
    toks = []
    pos = 0
    line = 1
    while pos < len(text):
        m = _TOKEN_RE.match(text, pos)
        if not m:
            raise DtsError("%s:%d: cannot tokenize %r" % (where, line, text[pos:pos + 18]))
        pos = m.end()
        line += text[m.start():m.end()].count("\n")
        if m.group("ws") or m.group("lc") or m.group("bc"):
            continue
        if m.group("str"):
            toks.append(("str", _unquote(m.group("str")), line))
        elif m.group("num"):
            toks.append(("num", m.group("num"), line))
        elif m.group("id"):
            toks.append(("id", m.group("id"), line))
        else:
            toks.append(("punct", m.group("punct"), line))
    return toks


def _unquote(s):
    return s[1:-1].replace('\\"', '"').replace("\\n", "\n").replace("\\\\", "\\")


# --------------------------------------------------------------------------
# model
# --------------------------------------------------------------------------
STR, CELLS, BYTES = "str", "cells", "bytes"


class Node(object):
    def __init__(self, name, unit=None, parent=None):
        self.name = name
        self.unit = unit or ""
        self.parent = parent
        self.props = {}          # name -> (kind, [values])
        self.prop_order = []
        self.children = []
        self.labels = []
        self.phandle = 0
        self.index = -1

    @property
    def full_name(self):
        return self.name + ("@" + self.unit if self.unit else "")

    def set_prop(self, name, kind, values):
        if name not in self.props:
            self.prop_order.append(name)
        self.props[name] = (kind, values)

    def get(self, name):
        return self.props.get(name)

    def child(self, name, unit=None):
        for c in self.children:
            if c.name == name and (unit is None or c.unit == unit):
                return c
        return None


class Parser(object):
    def __init__(self, base_dir):
        self.base_dir = base_dir
        self.labels = {}         # label -> Node
        self.root = Node("/")
        self.toks = []
        self.pos = 0

    # -- helpers ----------------------------------------------------------
    def _peek(self, k=0):
        i = self.pos + k
        return self.toks[i] if i < len(self.toks) else None

    def _next(self):
        t = self._peek()
        if t is None:
            raise DtsError("unexpected end of file")
        self.pos += 1
        return t

    def _is(self, ch, k=0):
        t = self._peek(k)
        return t is not None and t[0] == "punct" and t[1] == ch

    def _expect_punct(self, ch):
        t = self._next()
        if t[0] != "punct" or t[1] != ch:
            raise DtsError("line %d: expected %r, got %r" % (t[2], ch, t[1]))
        return t

    # -- entry ------------------------------------------------------------
    def parse_file(self, path):
        path = os.path.abspath(path)
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        toks = tokenize(text, os.path.basename(path))
        saved = self.toks, self.pos
        self.toks, self.pos = toks, 0
        self._parse_body(self.root)
        self.toks, self.pos = saved

    def parse_text(self, text, where="<text>"):
        saved = self.toks, self.pos
        self.toks, self.pos = tokenize(text, where), 0
        self._parse_body(self.root)
        self.toks, self.pos = saved

    # -- body -------------------------------------------------------------
    def _parse_body(self, node):
        while True:
            t = self._peek()
            if t is None:
                return
            if self._is("}"):
                return
            if t[0] == "punct" and t[1] == "/":
                self._parse_directive(node)
                continue
            if t[0] == "id" and t[1] == "#include":
                self._parse_c_include()
                continue
            if t[0] == "punct" and t[1] == "&":
                self._parse_overlay()
                continue
            self._parse_member(node)

    def _parse_c_include(self):
        t = self._next()                                  # '#include'
        f = self._next()
        if f[0] != "str":
            raise DtsError("line %d: #include needs a quoted file" % f[2])
        self.parse_file(os.path.join(self.base_dir, f[1]))

    def _parse_directive(self, node):
        self._next()                       # '/'
        if self._is("{"):
            # a bare `/ { ... };` defines the root node
            self._next()
            self._parse_body(self.root)
            self._expect_punct("}")
            if self._is(";"):
                self._next()
            return
        t = self._next()
        if t[0] != "id":
            raise DtsError("line %d: bad directive /%s" % (t[2], t[1]))
        what = t[1]
        # these keywords are written with a trailing slash: /include/, /memreserve/
        if self._is("/"):
            self._next()
        if what in ("dts-v1", "plugin"):
            self._expect_punct(";")
            return
        if what == "include":
            f = self._next()
            if f[0] != "str":
                raise DtsError("line %d: /include/ needs a quoted file" % f[2])
            self.parse_file(os.path.join(self.base_dir, f[1]))
            return
        if what == "memreserve":
            while self._peek() is not None and not self._is(";"):
                self._next()
            self._expect_punct(";")
            return
        if what in ("delete-node", "delete-property"):
            name, unit = self._parse_name()
            self._expect_punct(";")
            if what == "delete-node":
                c = node.child(name, unit or None)
                if c is not None:
                    node.children.remove(c)
            else:
                node.props.pop(name, None)
                if name in node.prop_order:
                    node.prop_order.remove(name)
            return
        raise DtsError("line %d: unsupported directive /%s/" % (t[2], what))

    def _parse_name(self):
        name = self._next()
        if name[0] != "id":
            raise DtsError("line %d: expected a name, got %r" % (name[2], name[1]))
        unit = None
        if self._is("@"):
            self._next()
            # A unit address is hexadecimal without a 0x prefix, so the
            # tokenizer splits e.g. 40020c00 into a number and an id.  Glue
            # every following number/id back together; only the register
            # window (reg) is ever looked at, the unit is just a label.
            parts = []
            while self._peek() is not None and self._peek()[0] in ("id", "num"):
                parts.append(str(self._next()[1]))
            if not parts:
                raise DtsError("line %d: bad unit address" % name[2])
            unit = "".join(parts)
        return name[1], unit

    def _parse_member(self, node):
        # optional label prefix:  label: name { }
        t = self._peek()
        label = None
        if t[0] == "id" and self._is(":", 1):
            label = t[1]
            self._next()
            self._next()
        name, unit = self._parse_name()
        if self._is("{"):
            self._next()
            child = Node(name, unit, node)
            node.children.append(child)
            if label:
                child.labels.append(label)
                self.labels[label] = child
            self._parse_body(child)
            self._expect_punct("}")
            if self._is(";"):
                self._next()
            return
        if self._is("=") or self._is(";"):
            self._parse_prop(node, name)
            return
        raise DtsError("line %d: expected '{' or '=' or ';' after %s" % (self._peek()[2], name))

    def _parse_prop(self, node, name):
        if self._is(";"):
            self._next()
            node.set_prop(name, BYTES, [])
            return
        self._expect_punct("=")
        kind = None
        values = []
        while True:
            k, v = self._parse_value()
            if kind is None:
                kind = k
            elif k != kind:
                raise DtsError("property %s mixes value kinds (%s and %s)" % (name, kind, k))
            values.extend(v)
            if self._is(","):
                self._next()
                continue
            break
        self._expect_punct(";")
        node.set_prop(name, kind, values)

    def _parse_value(self):
        t = self._next()
        if t[0] == "str":
            return STR, [t[1]]
        if t[0] == "num":
            return CELLS, [_num(t[1])]
        if t[0] == "punct" and t[1] == "&":
            lab = self._next()
            return CELLS, [("ref", lab[1])]
        if t[0] == "punct" and t[1] == "<":
            cells = []
            while self._peek() is not None and not self._is(">"):
                e = self._next()
                if e[0] == "num":
                    cells.append(_num(e[1]))
                elif e[0] == "punct" and e[1] == "&":
                    lab = self._next()
                    cells.append(("ref", lab[1]))
                else:
                    raise DtsError("line %d: bad cell %r" % (e[2], e[1]))
            self._expect_punct(">")
            return CELLS, cells
        if t[0] == "punct" and t[1] == "[":
            by = []
            while self._peek() is not None and not self._is("]"):
                e = self._next()
                if e[0] not in ("num", "id"):
                    raise DtsError("line %d: bad byte %r" % (e[2], e[1]))
                by.append(_num(e[1]))
            self._expect_punct("]")
            return BYTES, by
        raise DtsError("line %d: bad property value %r" % (t[2], t[1]))

    def _parse_overlay(self):
        self._expect_punct("&")
        lab = self._next()
        if lab[0] != "id" or lab[1] not in self.labels:
            raise DtsError("line %d: overlay on unknown label &%s" % (lab[2], lab[1]))
        target = self.labels[lab[1]]
        self._expect_punct("{")
        self._parse_body(target)
        self._expect_punct("}")
        if self._is(";"):
            self._next()


def _num(s):
    return int(s, 0)


# --------------------------------------------------------------------------
# phandles + flatten
# --------------------------------------------------------------------------
def assign_phandles(root):
    """Every labelled node gets a phandle; refs in cells become that number."""
    n = [0]

    def walk(node):
        for c in node.children:
            walk(c)
            if c.labels:
                n[0] += 1
                c.phandle = n[0]

    walk(root)
    # resolve
    def resolve(node):
        for name in node.prop_order:
            kind, vals = node.props[name]
            if kind != CELLS:
                continue
            out = []
            for v in vals:
                if isinstance(v, tuple) and v[0] == "ref":
                    tn = _find_label(root, v[1])
                    if tn is None:
                        raise DtsError("reference to unknown label &%s" % v[1])
                    out.append(tn.phandle)
                else:
                    out.append(v)
            node.props[name] = (CELLS, out)
        for c in node.children:
            resolve(c)

    resolve(root)


def _find_label(root, label):
    stack = [root]
    while stack:
        n = stack.pop()
        if label in n.labels:
            return n
        stack.extend(n.children)
    return None


def flatten(root):
    out = []

    def walk(node, parent_idx):
        for c in node.children:
            c.index = len(out)
            out.append((c, parent_idx))
            walk(c, c.index)

    walk(root, 0xffff)
    return out


# --------------------------------------------------------------------------
# code generation
# --------------------------------------------------------------------------
def _cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def gen_c(root, board_name, source):
    nodes = flatten(root)
    model = root.get("model")
    model_str = model[1][0] if model else board_name

    lines = []
    a = lines.append
    a("/* Auto-generated by tools/dts/dtc_lite.py - DO NOT EDIT.")
    a(" *")
    a(" * Source : %s" % source)
    a(" * Board  : %s" % board_name)
    a(" * Nodes  : %d" % len(nodes))
    a(" *")
    a(" * Regenerate with:")
    a(" *   py -3 tools/dts/dtc_lite.py --dts dts/%s.dts \\" % board_name)
    a(" *       --out board/%s/arkrtos_dt.c" % board_name)
    a(" */")
    a("")
    a('#include "arkrtos_dt.h"')
    a("")

    # string pools
    for i, (node, _p) in enumerate(nodes):
        strs = []
        for name in node.prop_order:
            kind, vals = node.props[name]
            if kind == STR:
                strs.extend(vals)
        if strs:
            a("static const char *const dt_s%d[] = {%s, 0};"
              % (i, ", ".join(_cstr(s) for s in strs)))
    if any(any(n.props[p][0] == STR for p in n.prop_order) for n, _ in nodes):
        a("")

    # u32 pools
    for i, (node, _p) in enumerate(nodes):
        for j, name in enumerate(node.prop_order):
            kind, vals = node.props[name]
            if kind == CELLS:
                a("static const uint32 dt_u%d_%d[] = {%s};"
                  % (i, j, ", ".join("0x%xu" % (v & 0xffffffff) for v in vals)))
    a("")

    # property tables
    for i, (node, _p) in enumerate(nodes):
        if not node.prop_order:
            continue
        entries = []
        si = 0
        for j, name in enumerate(node.prop_order):
            kind, vals = node.props[name]
            if kind == STR:
                entries.append("{ %s, ARKRTOS_DT_PROP_STR, %d, 0, dt_s%d + %d }"
                               % (_cstr(name), len(vals), i, si))
                si += len(vals)
            elif kind == CELLS:
                entries.append("{ %s, ARKRTOS_DT_PROP_U32, %d, dt_u%d_%d, 0 }"
                               % (_cstr(name), len(vals), i, j))
            else:
                entries.append("{ %s, ARKRTOS_DT_PROP_BYTES, %d, 0, 0 }"
                               % (_cstr(name), vals[0] if vals else 0))
        a("static const arkrtos_dt_prop_t dt_p%d[] = {" % i)
        for e in entries:
            a("    %s," % e)
        a("};")
    a("")

    # node table
    a("static const arkrtos_dt_node_t dt_nodes[] = {")
    for i, (node, parent) in enumerate(nodes):
        comp = node.get("compatible")
        comp_str = _cstr(comp[1][0]) if (comp and comp[1]) else "0"
        props_ptr = ("dt_p%d" % i) if node.prop_order else "0"
        status = node.get("status")
        okay = 1 if (status is None or (status[1] and status[1][0] == "okay")) else 0
        is_dev = 1 if comp else 0
        a("    { %s, %s, 0x%xu, 0x%xu, %d, %d, %d, %d, %d, %s },"
          % (_cstr(node.full_name), comp_str,
             _reg_base(node), _reg_size(node), node.phandle,
             parent if parent != 0xffff else 65535, len(node.prop_order),
             okay, is_dev, props_ptr))
    a("};")
    a("")
    a("static const char dt_model[] = %s;" % _cstr(model_str))
    a("")
    _gen_api(a)
    return "\n".join(lines) + "\n"


def _reg_first(node):
    reg = node.get("reg")
    if not reg or reg[0] != CELLS or not reg[1]:
        return None
    return reg[1]


def _reg_base(node):
    r = _reg_first(node)
    return r[0] if r else 0


def _reg_size(node):
    r = _reg_first(node)
    return r[1] if (r and len(r) > 1) else 0


def _gen_api(a):
    a("int arkrtos_dt_str_eq(const char *a, const char *b)")
    a("{")
    a("    if((a == 0) || (b == 0))")
    a("    {")
    a("        return 0;")
    a("    }")
    a("    while((*a != 0) && (*a == *b))")
    a("    {")
    a("        a++;")
    a("        b++;")
    a("    }")
    a("    return (*a == *b) ? 1 : 0;")
    a("}")
    a("")
    a("uint32 arkrtos_dt_count(void)")
    a("{")
    a("    return (uint32)(sizeof(dt_nodes) / sizeof(dt_nodes[0]));")
    a("}")
    a("")
    a("const arkrtos_dt_node_t *arkrtos_dt_node(uint32 idx)")
    a("{")
    a("    if(idx >= arkrtos_dt_count())")
    a("    {")
    a("        return 0;")
    a("    }")
    a("    return &dt_nodes[idx];")
    a("}")
    a("")
    a("const arkrtos_dt_node_t *arkrtos_dt_find(const char *compatible)")
    a("{")
    a("    return arkrtos_dt_find_next(compatible, 0);")
    a("}")
    a("")
    a("const arkrtos_dt_node_t *arkrtos_dt_find_next(const char *compatible,")
    a("                                             const arkrtos_dt_node_t *from)")
    a("{")
    a("    uint32 i = 0u;")
    a("    if(compatible == 0)")
    a("    {")
    a("        return 0;")
    a("    }")
    a("    if(from != 0)")
    a("    {")
    a("        i = (uint32)(from - dt_nodes) + 1u;")
    a("    }")
    a("    for(; i < arkrtos_dt_count(); i++)")
    a("    {")
    a("        if((dt_nodes[i].is_device != 0u) && (dt_nodes[i].status_okay != 0u) &&")
    a("           (dt_nodes[i].compatible != 0) &&")
    a("           (arkrtos_dt_str_eq(dt_nodes[i].compatible, compatible) != 0))")
    a("        {")
    a("            return &dt_nodes[i];")
    a("        }")
    a("    }")
    a("    return 0;")
    a("}")
    a("")
    a("const char *arkrtos_dt_model(void)")
    a("{")
    a("    return dt_model;")
    a("}")
    a("")
    a("const arkrtos_dt_prop_t *arkrtos_dt_prop(const arkrtos_dt_node_t *node,")
    a("                                          const char *name)")
    a("{")
    a("    uint16 i;")
    a("    if((node == 0) || (node->props == 0))")
    a("    {")
    a("        return 0;")
    a("    }")
    a("    for(i = 0u; i < node->n_props; i++)")
    a("    {")
    a("        if(arkrtos_dt_str_eq(node->props[i].name, name) != 0)")
    a("        {")
    a("            return &node->props[i];")
    a("        }")
    a("    }")
    a("    return 0;")
    a("}")
    a("")
    a("int32 arkrtos_dt_prop_u32(const arkrtos_dt_node_t *node, const char *name,")
    a("                          uint32 idx, uint32 *out)")
    a("{")
    a("    const arkrtos_dt_prop_t *p = arkrtos_dt_prop(node, name);")
    a("    if((p == 0) || (p->kind != ARKRTOS_DT_PROP_U32) || (p->u32 == 0) ||")
    a("       (idx >= p->count))")
    a("    {")
    a("        return -1;")
    a("    }")
    a("    if(out != 0)")
    a("    {")
    a("        *out = p->u32[idx];")
    a("    }")
    a("    return 0;")
    a("}")
    a("")
    a("const char *arkrtos_dt_prop_str(const arkrtos_dt_node_t *node,")
    a("                                const char *name)")
    a("{")
    a("    const arkrtos_dt_prop_t *p = arkrtos_dt_prop(node, name);")
    a("    if((p == 0) || (p->kind != ARKRTOS_DT_PROP_STR) || (p->str == 0))")
    a("    {")
    a("        return 0;")
    a("    }")
    a("    return p->str[0];")
    a("}")


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------
def build(dts_path):
    base = os.path.dirname(os.path.abspath(dts_path))
    p = Parser(base)
    p.parse_file(dts_path)
    assign_phandles(p.root)
    return p.root


def main(argv):
    ap = argparse.ArgumentParser(description="Ark RTOS Device Tree compiler")
    ap.add_argument("--dts", required=True)
    ap.add_argument("--out")
    ap.add_argument("--board", help="board name (default: dts file stem)")
    ap.add_argument("--dump", action="store_true", help="print the tree, no code")
    ap.add_argument("--check", action="store_true",
                    help="fail if --out differs from the generated text")
    args = ap.parse_args(argv)

    root = build(args.dts)
    board = args.board or os.path.splitext(os.path.basename(args.dts))[0]

    if args.dump:
        for i, (n, parent) in enumerate(flatten(root)):
            comp = n.get("compatible")
            print("%3d %-28s parent=%-4s compat=%s reg=0x%x/0x%x phandle=%d" % (
                i, n.full_name, parent, comp[1][0] if comp else "-",
                _reg_base(n), _reg_size(n), n.phandle))
        return 0

    src = os.path.relpath(args.dts).replace("\\", "/")
    text = gen_c(root, board, src)

    if not args.out:
        sys.stdout.write(text)
        return 0
    if args.check:
        old = ""
        if os.path.exists(args.out):
            old = open(args.out, "r", encoding="utf-8").read()
        if old == text:
            print("dts: %s is up to date" % args.out)
            return 0
        print("dts: %s is STALE - regenerate it" % args.out, file=sys.stderr)
        return 1
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("dtc_lite: %s -> %s" % (args.dts, args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
