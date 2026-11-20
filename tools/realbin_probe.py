#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把一份真实发行版 LA64 ELF 送上板并 exec，把读数原样打印出来。

为什么走串口上传而不是"编进镜像"
--------------------------------
板级的分区几何写死了镜像池的位置（见 config/arkrtos_partition.h）：

    CHIP_FLASH_BASE = 0x00200000, CHIP_FLASH_SIZE = 2 MB
    KERNEL_BASE = CHIP_FLASH_BASE + BOOT_SIZE
    IMAGE_POOL_BASE = KERNEL_BASE + KERNEL_SIZE + CONFIG_SIZE

QEMU virt 上这块 Flash 窗口正好贴到 RAM 基址 0x00400000，池子必须整个落在
窗口之内，于是 KERNEL_SIZE 只有 512 KB / 1 MB / 2 MB 三档可选，而 2 MB 会把
池子推到 RAM 基址之外（实测：抬到 2 MB 后镜像池压进内核 .bss，开机到
"[boot] paging on" 就停住）。623728 B 的 glibc 静态 hello 加 32 B 签名尾
塞不进任何一档 —— 这条通道在**当前几何**上是不通的，不是"还没调好"。

真实板子上二进制本来也不在内核镜像里，它在 Flash / SD / U 盘上。本仓已有的
对应通道就是 shell 的 `fs put`（256 B 一段、逐段 0x06 握手，见
kernelsrc/shell/arkrtos_shell_fs.c）。本工具只做三件事：

  1. 给宿主上的 ELF 补 32 B HMAC 尾签名（exec 的签名门，见 tools/sign_payload.py）
  2. 起 QEMU（TCP 串口），用 `fs put` 逐段推上去，逐段等 ACK
  3. `exec <挂载点>/<名字>`，把内核与这个二进制自己打出来的东西原样打印

判据是**读数**，不是"成功/失败"：真实二进制缺哪一道（futex / rseq / TLS TCB /
mmap），要看它自己走到哪一步。工具本身只在上传没走完时返回非 0。

用法
----
    python tools/realbin_probe.py --elf D:/tmp/realbin/hello_static.elf
    python tools/realbin_probe.py --elf ... --vfs /mnt/ram/realhello --port 4556
    python tools/realbin_probe.py --elf ... --elf-args "alpha 42" --exec-wait 15

注意
----
* QEMU 在 Windows 上打开不了含非 ASCII 的路径，所以 ELF 一律按"cwd 到
  build 目录 + 文件名"传；上传的临时签名文件也落在 build 目录下。
* 默认镜像必须按 WIN_VA 构建（真实二进制链在 0x120000000）：
      make -f example/qemu_loongarch64/Makefile clean
      make -f example/qemu_loongarch64/Makefile WIN_VA=0x120000000 -j8
"""

import argparse
import os
import shlex
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build", "qemu_loongarch64")
ELF_NAME = "ark_qemu_la64.elf"
QEMU_CANDIDATES = [
    "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe",
]

FS_PUT_CHUNK = 256
FS_PUT_ACK = 0x06
FS_PUT_NAK = 0x15


def pick_qemu(explicit):
    cands = ([explicit] if explicit else []) + QEMU_CANDIDATES
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return ""


class Console(object):
    """串口收发：后台线程一直读，正文按字节留在 self.raw 里。"""

    def __init__(self, sock):
        self.s = sock
        self.raw = bytearray()
        self.lock = threading.Lock()
        self.stop = False
        self.t = threading.Thread(target=self._drain)
        self.t.daemon = True
        self.t.start()

    def _drain(self):
        while not self.stop:
            try:
                d = self.s.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                return
            if not d:
                return
            with self.lock:
                self.raw += d

    def send(self, data):
        self.s.sendall(data)

    def text(self):
        with self.lock:
            return self.raw.decode("utf-8", "replace")

    def wait_text(self, needle, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if needle in self.text():
                return True
            time.sleep(0.02)
        return False

    def mark(self):
        with self.lock:
            return len(self.raw)

    def since(self, mark):
        with self.lock:
            return self.raw[mark:].decode("utf-8", "replace")

    def wait_byte(self, want, seconds):
        """等一个流控字节（0x06/0x15），回 True/False。"""
        end = time.time() + seconds
        while time.time() < end:
            with self.lock:
                i = self.raw.find(bytes([want]))
                if i >= 0:
                    del self.raw[:i + 1]      # 连同它之前的内容一起吃掉
                    return True
            time.sleep(0.002)
        return False

    def drop_ack_bytes(self):
        """把已经收到的流控字节从正文里剔掉，免得干扰后面的文本判读。"""
        with self.lock:
            self.raw = bytearray(b for b in self.raw
                                 if b not in (FS_PUT_ACK, FS_PUT_NAK))


def run(a):
    qemu = pick_qemu(a.qemu)
    if not qemu:
        print("FAIL: 找不到 qemu-system-loongarch64")
        return 1
    src = os.path.abspath(a.elf)
    if not os.path.isfile(src):
        print("FAIL: 找不到 %s" % src)
        return 1
    if not os.path.isfile(os.path.join(BUILD, ELF_NAME)):
        print("FAIL: 缺 %s（先按 WIN_VA 构建）" % os.path.join(BUILD, ELF_NAME))
        return 1

    # 1) 签名：exec 的签名门要求末尾 32 B HMAC；sign 是幂等的。
    signed = os.path.join(BUILD, "realbin.signed.elf")
    keyhdr = os.path.join(BUILD, "arkrtos_sign_key_local.h")
    cmd = [sys.executable, os.path.join(ROOT, "tools", "sign_payload.py"),
           "sign", src, "--out", signed]
    if os.path.isfile(keyhdr):
        cmd += ["--key-header", keyhdr]
    r = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    if r.returncode != 0:
        print("FAIL: 签名失败\n" + r.stdout.decode("utf-8", "replace"))
        return 1

    blob = open(signed, "rb").read()
    print("[host] %s -> %s (%d B, 已签名)" % (src, signed, len(blob)))

    # 2) 起 QEMU，串口走 TCP（-nographic 的 mon:stdio 会吃掉管道输入）
    argv = [qemu, "-machine", "virt", "-kernel", ELF_NAME,
            "-display", "none", "-monitor", "none",
            "-serial", "tcp:127.0.0.1:%d,server=on,wait=off" % a.port]
    # 额外参数只为诊断：取证时用 -d int -D <文件> 把异常流落盘，
    # 平时一个字节都不加（默认空串）。
    if a.qemu_extra:
        argv += shlex.split(a.qemu_extra)
    p = subprocess.Popen(argv, cwd=BUILD,
                         stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    sock = None
    try:
        for _ in range(80):
            if p.poll() is not None:
                print("FAIL: QEMU 立刻退出了（rc=%d）" % p.returncode)
                return 1
            try:
                sock = socket.create_connection(("127.0.0.1", a.port), timeout=1)
                break
            except OSError:
                time.sleep(0.25)
        if sock is None:
            print("FAIL: 连不上 127.0.0.1:%d" % a.port)
            return 1
        sock.settimeout(0.5)
        con = Console(sock)

        # 3) 等 shell 提示符
        ok = False
        end = time.time() + a.boot
        while time.time() < end:
            con.send(b"\r")
            if con.wait_text("@arkrtos", 1.0):
                ok = True
                break
        if not ok:
            print("FAIL: %.0f s 内没等到提示符；末尾输出：\n%s"
                  % (a.boot, con.text()[-1500:]))
            return 1

        # 4) 上传
        #    命令行本身要逐字符敲：一次性灌 20 多个字节时，控制台正在被
        #    [A]/[B] 心跳行占用，实测会被吃掉字符（argv 变成 "alhello"，
        #    于是 fs_check_path 判「路径不以 / 开头」）。敲慢一点，再核对
        #    回显与横幅，两边都对上才开始推数据。
        banner = "fs put: /%s, %d bytes" % (a.name, len(blob))
        opened = False
        for attempt in range(3):
            m = con.mark()
            for ch in ("fs put /%s %d\r" % (a.name, len(blob))):
                con.send(ch.encode())
                time.sleep(0.004)
            if con.wait_text(banner, 8.0):
                opened = True
                break
            print("[host] 第 %d 次没进上传窗口，重试；回显：\n%s"
                  % (attempt + 1, con.since(m)[-400:]))
            time.sleep(1.0)
        if not opened:
            print("FAIL: fs put 没有进入上传窗口")
            return 1
        print("[host] 上传窗口已打开，%d 段 × %d B"
              % ((len(blob) + FS_PUT_CHUNK - 1) // FS_PUT_CHUNK, FS_PUT_CHUNK))

        t0 = time.time()
        sent = 0
        while sent < len(blob):
            piece = blob[sent:sent + FS_PUT_CHUNK]
            con.send(piece)
            if not con.wait_byte(FS_PUT_ACK, a.chunk_timeout):
                print("FAIL: 第 %d 段（偏移 %d）没等到 ACK" % (sent // FS_PUT_CHUNK, sent))
                return 1
            sent += len(piece)
            if (sent // FS_PUT_CHUNK) % 400 == 0:
                print("[host] %d / %d B  %.1f s"
                      % (sent, len(blob), time.time() - t0))
        dt = time.time() - t0
        con.drop_ack_bytes()
        print("[host] 上传完成 %d B，用时 %.1f s（%.0f B/s）"
              % (sent, dt, sent / dt if dt > 0 else 0.0))

        if not con.wait_text("fs put: ok", 10.0):
            print("FAIL: 没看到 fs put: ok；输出：\n%s" % con.text()[-1500:])
            return 1

        # 5) 目录里核实大小，再 exec
        m = con.mark()
        con.send(("ls %s\r" % a.vfs_dir).encode())
        time.sleep(1.0)
        print("\n$ ls %s\n%s" % (a.vfs_dir, con.since(m).rstrip()))

        m = con.mark()
        line = "exec %s %s\r" % (a.vfs, a.elf_args)
        con.send(line.encode())
        print("\n$ " + line.strip())
        t_end = time.time() + a.exec_wait
        while time.time() < t_end:
            time.sleep(0.2)
            # 任务跑完会打 exit code，看到就不再等满
            if "exit code" in con.since(m):
                time.sleep(0.5)
                break
        print(con.since(m).rstrip())
        print("\n==== 上面就是全部读数（QEMU，不是真机） ====")
        return 0
    finally:
        try:
            sock.close()
        except Exception:
            pass
        p.terminate()
        try:
            p.wait(timeout=5)
        except Exception:
            p.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", required=True, help="宿主上的真实 LA64 ELF")
    ap.add_argument("--name", default="realhello", help="板上的文件名（不带目录）")
    ap.add_argument("--vfs-dir", default="/mnt/ram", help="挂载点（VFS 路径）")
    ap.add_argument("--vfs", default="", help="exec 用的完整路径，默认 <dir>/<name>")
    ap.add_argument("--elf-args", default="", help="传给二进制的参数串")
    ap.add_argument("--port", type=int, default=4555)
    ap.add_argument("--boot", type=float, default=20.0, help="等提示符的秒数")
    ap.add_argument("--chunk-timeout", type=float, default=6.0)
    ap.add_argument("--exec-wait", type=float, default=12.0)
    ap.add_argument("--qemu", default="")
    ap.add_argument("--qemu-extra", default="",
                    help="追加给 QEMU 的参数（诊断用，会切词；默认空）")
    a = ap.parse_args()
    if not a.vfs:
        a.vfs = a.vfs_dir.rstrip("/") + "/" + a.name
    return run(a)


if __name__ == "__main__":
    sys.exit(main())