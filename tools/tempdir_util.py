#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""TempDir: 带清理重试的临时目录，给门禁脚本用。

为什么需要它
------------
门禁都在临时目录里编 exe、跑 exe、然后删目录。本机杀毒软件会短暂锁住刚执行
过的 .exe，Windows 上 rmtree 随即抛 WinError 5（拒绝访问），
tempfile.TemporaryDirectory.__exit__ 把它原样抛出来，整条门禁就以一个与判定
无关的异常收场——2026-11-22 实测两次，分别死在 ci_gate 的 timer-host 与
event-host，前面几十项检查已经打印出来，结论却作废。

清理失败只说明临时目录留下了垃圾，不说明被检查的东西有问题。所以这里：
  * 先重试（5 次 × 250 ms，足够杀毒扫描松开句柄）；
  * 仍然失败就告警并继续，让门禁给出它真正的判定，退出码不受清理影响。
这不是把错误藏起来：目录路径与最后一次异常都写进 stderr，能查能复现。

用法
----
    from tempdir_util import TempDir
    with TempDir() as td:
        ...

构造参数与 tempfile.TemporaryDirectory 完全一致。
"""
import sys
import tempfile
import time

RETRIES = 5
DELAY = 0.25


class TempDir(tempfile.TemporaryDirectory):
    """TemporaryDirectory + 清理重试；最终失败只告警（stderr），不抛异常。"""

    def cleanup(self):
        last = None
        for _ in range(RETRIES):
            try:
                tempfile.TemporaryDirectory.cleanup(self)
                return
            except OSError as exc:   # PermissionError / NotADirectoryError 都是子类
                last = exc
                time.sleep(DELAY)
        sys.stderr.write("warning: temp dir left behind: %s (%s)\n"
                         % (self.name, last))
