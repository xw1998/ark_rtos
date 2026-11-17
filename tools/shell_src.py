#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""shell 源文件清单（门禁统一入口）。

内核 shell 已按命令族拆成 kernelsrc/shell/arkrtos_shell*.c。门禁里凡是要
「读 Shell 源码做 pin」或「把 Shell 编进档」的地方，都必须按这份清单取并集：
只读 arkrtos_shell.c 的话，命令一搬家判据就一条也匹配不上——仍然全绿，
但那只是空转（假绿），不是覆盖。
"""
import os

SUBDIR = ("kernelsrc", "shell")
PREFIX = "arkrtos_shell"
SUFFIX = ".c"


def rel_list(root):
    """返回 Shell 源文件的仓库相对路径（已排序）。"""
    d = os.path.join(root, *SUBDIR)
    names = [n for n in os.listdir(d)
             if n.startswith(PREFIX) and n.endswith(SUFFIX)]
    return sorted("kernelsrc/shell/" + n for n in names)
