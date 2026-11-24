#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""真实以太网（QEMU virtio-net / ARKRTOS_USE_NET_VIRTIO）的门禁。

为什么需要这一道
----------------
「ping 通了」这句话在这个工程里有四种假绿形态，每一种都编得过、都能打出一
份看起来对的串口日志：

  1. 回环冒充真实链路：lwIP 本来就有 loopif 127.0.0.1，raw pcb 收到自己发出
     的包也能让回调跑起来 —— 那不是网卡，是协议栈把包喂回了自己；
  2. 帧没出去但日志写了「reply」：回调里打印的是我们想看到的话，不是对端
     真的回了；
  3. 开关没接线：Kconfig 有一项、features.h 有 #error，但源文件没被 #if 包
     住 —— 默认档悄悄多出一份代码（而且默认档整个镜像会跟着变）；
  4. 收包投错了线程：LWIP_TCPIP_CORE_LOCKING=0 时协议栈只在 tcpip 线程里跑。
     在轮询任务里直接 netif.input 会同时改 ARP 表与 netif 状态，实测表现为
     「跑几百毫秒后整机停住、raw pcb 一个包都收不到」—— 它长得像网卡坏。

所以判据全部落在**外部可核对**的读数上：QEMU 的 filter-dump 抓到的 pcap
（帧真的出了芯片又回来了，且 MAC/IP 是我们设的），加上串口上逐条出现的那几
行；再加一档「把网卡拿掉必须失败」的反假绿，以及一份解析器变异自检。

判据（全过才 0）
----------------
  A. 默认关：arkrtos_autoconf.h 里 ARKRTOS_USE_NET_VIRTIO 必须是 0；
     Kconfig.net 有 config ARKRTOS_USE_NET_VIRTIO；features.h 有依赖 #error；
     Makefile 里 lwip / drvnet / net_demo 三个源都在开关块里（不是无条件编）。
  B. 开档编得过：LA64 真板级 + `-DARKRTOS_USE_NET_VIRTIO=1
     -DARKRTOS_NET_DEMO=1` 全量构建出一个 elf。
  C. QEMU 真读数（带 `-netdev user` + virtio-net-pci + filter-dump）：
     串口上逐条出现 `[net] virtio-net: found at pci 00:01.0` / netif up /
     seq=1..3 的 `reply from 10.0.2.2 ... data="@ABC..."` / `ping: 3/3 ok` /
     `stats tx=5 txdrop=0 rx=4 rxdrop=0 last_len=60` / `demo: done`；
     并且 pcap 里四类帧齐全 —— ARP request（我们 → 广播）、ARP reply（对端 →
     我们）、ICMP echo request（type 8）与 echo reply（type 0）各三条，id 都是
     16722、seq 各是 1/2/3，源/目的 MAC 与固件里那份一致。**只看串口不算数**：
     没有 pcap 这一条，回环网卡也能让日志一模一样。
  D. 无网卡必须失败（反假绿）：`-net none` 时串口必须打
     `[net] virtio-net: no device on pci bus 0`、不许出现 `netif up`、三次
     `send failed`、最终 `ping: 0/3 FAIL`。这一档就是「日志不是编的」的证明。
  E. 解析器变异自检：把合成的坏日志 / 坏 pcap 喂给同一组判据（去掉一条
     reply、把 3/3 改成 2/3、改 payload 字节、去掉 found 行、拿掉 ARP reply、
     把 echo reply 的 seq 改掉、换掉源 MAC、清空帧表、无网卡档里插一条
     netif up），十种坏法都必须判红。

覆盖：PCIe 枚举与 virtio-net 现代接口 bringup、ARP 解析、ICMP echo 往返、
真帧进出（pcap 侧）、收包投递到 tcpip 线程、无网卡时按失败处理。
不覆盖：真实 PHY / MDIO / RMII（本板是 PCIe 虚拟网卡）、多播/广播收包、
TCP 连接级流量、DHCP。

用法
----
    py -3 tools/net_virtio_gate.py
    py -3 tools/net_virtio_gate.py --keep
    py -3 tools/net_virtio_gate.py --qemu <path> --la64-gcc <path>

退出码 0 = 全过；1 = 任一判据不成立 / 缺工具链与 QEMU（缺工具是 FAIL，不是跳过）。
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

MAKEFILE = "example/qemu_loongarch64/Makefile"
BUILD_DIR = os.path.join(ROOT, "build", "qemu_loongarch64")
ELF_NAME = "ark_qemu_la64.elf"

NET_FLAGS = ["-DARKRTOS_USE_NET_VIRTIO=1", "-DARKRTOS_NET_DEMO=1"]

LA64_FALLBACKS = [
    "D:/LoongIDE/la64-tool/bin/loongarch64-newlib-elf-gcc.exe",
]
QEMU_CANDIDATES = (
    "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe",
)

OUR_MAC = "52:54:00:12:34:56"
ICMP_ID = 16722                       # 固件里写死的 id（"AR"）
ECHO_DATA = "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"

FAILS = []
PASSES = []


def ok(name, detail):
    PASSES.append(name)
    print("PASS  %-20s %s" % (name, detail))


def bad(name, detail):
    FAILS.append(name)
    print("FAIL  %-20s %s" % (name, detail))


def read_text(path):
    with open(path, "rb") as f:
        raw = f.read()
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def read_text_bytes(raw):
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def _pick(cands, given, what):
    if given:
        if os.path.isfile(given):
            return given
        bad("toolchain", "%s 不存在：%s" % (what, given))
        return None
    for c in cands:
        if os.path.isfile(c):
            return c
    bad("toolchain", "找不到 %s（缺工具是 FAIL，不是跳过）" % what)
    return None


# ---------------------------------------------------------------- A 默认关

def check_default_off():
    ac = read_text(os.path.join(ROOT, "kernelsrc/include/arkrtos_autoconf.h"))
    m = re.search(r"^#define\s+ARKRTOS_USE_NET_VIRTIO\s+(\d+)\s*$", ac, re.M)
    if not m:
        bad("default-off", "autoconf.h 里没有 ARKRTOS_USE_NET_VIRTIO（开关没进 Kconfig 生成物）")
    elif m.group(1) != "0":
        bad("default-off", "默认档 ARKRTOS_USE_NET_VIRTIO=%s（必须是 0）" % m.group(1))
    else:
        ok("default-off", "arkrtos_autoconf.h: ARKRTOS_USE_NET_VIRTIO 0")

    # 只有网卡本身进 Kconfig；ARKRTOS_NET_DEMO 是示例程序里的一个 -D（不进
    # 内核配置），所以这里查的是「它的依赖 #error 在不在」，不是「Kconfig 有没有」。
    kn = read_text(os.path.join(ROOT, "config/kconfig/Kconfig.net"))
    if "config ARKRTOS_USE_NET_VIRTIO" not in kn:
        bad("kconfig-wired", "Kconfig.net 里没有 config ARKRTOS_USE_NET_VIRTIO")
    elif not re.search(r"\bdepends on ARKRTOS_USE_LWIP\b", kn.split("config ARKRTOS_USE_NET_VIRTIO", 1)[1]):
        bad("kconfig-wired", "Kconfig.net 的 ARKRTOS_USE_NET_VIRTIO 没有 depends on ARKRTOS_USE_LWIP")
    else:
        demo = read_text(os.path.join(ROOT, "example/qemu_loongarch64/net_demo.c"))
        if "ARKRTOS_NET_DEMO=1 requires ARKRTOS_USE_NET_VIRTIO=1" not in demo:
            bad("kconfig-wired", "net_demo.c 里没有 ARKRTOS_NET_DEMO 依赖网卡的 #error")
        else:
            ok("kconfig-wired", "Kconfig 有网卡开关且依赖 lwIP；演示档有自己的依赖 #error")

    fh = read_text(os.path.join(ROOT, "kernelsrc/include/arkrtos_features.h"))
    if "ARKRTOS_USE_NET_VIRTIO=1 requires" not in fh:
        bad("features-guard", "features.h 里没有 ARKRTOS_USE_NET_VIRTIO 的依赖 #error")
    else:
        ok("features-guard", "features.h 有依赖 #error（开关组合错了编不过）")

    mk = read_text(os.path.join(ROOT, MAKEFILE))
    need = ["drvnet.c", "lwip_netif_link.c", "net_demo.c"]
    miss = [n for n in need if n not in mk]
    if miss:
        bad("makefile-wired", "Makefile 里缺 %s" % "、".join(miss))
    elif "ARKRTOS_USE_NET_VIRTIO=1" not in mk:
        bad("makefile-wired", "Makefile 里没有用 ARKRTOS_USE_NET_VIRTIO=1 做条件（源文件会被无条件编进默认档）")
    else:
        ok("makefile-wired", "三个源都在开关块里（默认档不编）")


# ---------------------------------------------------------------- B 开档构建

def build_image():
    log = os.path.join(tempfile.gettempdir(), "ark_net_build.log")
    env = dict(os.environ)
    env["EXTRA_CFLAGS"] = " ".join(NET_FLAGS)
    with open(log, "wb") as f:
        # 先 clean：make 不跟踪 EXTRA_CFLAGS，上一档留下的 .o 会让这一次只做
        # 链接 —— 镜像里根本没有网卡，日志却照样有，这是最像真的假绿。
        r = subprocess.run(["make", "-f", MAKEFILE, "clean"], cwd=ROOT, env=env,
                           stdout=f, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            return False, read_text(log)[-1200:]
        r = subprocess.run(["make", "-f", MAKEFILE, "-j8"], cwd=ROOT, env=env,
                           stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        return False, read_text(log)[-1500:]
    elf = os.path.join(BUILD_DIR, ELF_NAME)
    if not os.path.isfile(elf):
        return False, "构建成功但没有 %s" % ELF_NAME
    return True, ""


def restore_default_build():
    """把构建目录恢复成默认档。

    本门禁为读现场证据会 clean + 带 EXTRA_CFLAGS 重编；不恢复的话，后面任何
    一次 `make` 都会复用这些 .o（make 不跟踪 EXTRA_CFLAGS），默认档就悄悄变
    成网卡档 —— 下一个人看到的是「默认档也有 [net] 读数」。
    """
    log = os.path.join(tempfile.gettempdir(), "ark_net_restore.log")
    with open(log, "wb") as f:
        r = subprocess.run(["make", "-f", MAKEFILE, "clean"], cwd=ROOT,
                           stdout=f, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            return False, read_text(log)[-600:]
        r = subprocess.run(["make", "-f", MAKEFILE, "-j8"], cwd=ROOT,
                           stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        return False, read_text(log)[-1200:]
    return True, ""


# ---------------------------------------------------------------- C/D QEMU

def run_qemu(qemu, with_nic, pcap_path, seconds, workdir):
    """跑一次 QEMU，串口走管道（file 后端在 kill 时会丢最后一个不满 4 KB 的块）。"""
    cmd = [qemu, "-machine", "virt", "-kernel", ELF_NAME,
           "-nographic", "-monitor", "none"]
    if with_nic:
        cmd += ["-netdev", "user,id=n0",
                "-device", "virtio-net-pci-non-transitional,netdev=n0"]
        if pcap_path:
            cmd += ["-object", "filter-dump,id=f0,netdev=n0,file=" + pcap_path]
    else:
        # 注意：不给 -device 是不够的 —— QEMU 的 virt machine 有默认 NIC，
        # 早期那一版「--no-device」假对照里驱动照样 found at 00:01.0。
        cmd += ["-net", "none"]

    p = subprocess.Popen(cmd, cwd=workdir, stdin=subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    chunks = []

    def drain():
        while True:
            b = p.stdout.read(1024)
            if not b:
                break
            chunks.append(b)

    th = threading.Thread(target=drain)
    th.daemon = True
    th.start()

    marker = b"[net] demo: done"
    deadline = time.time() + seconds
    while time.time() < deadline:
        if marker in b"".join(chunks):
            break
        time.sleep(0.05)

    time.sleep(0.4)                     # 让最后几行落进管道
    p.kill()
    p.wait()
    th.join(timeout=5)
    data = b"".join(chunks)
    if not data:
        return None
    return read_text_bytes(data)


# ---------------------------------------------------------------- pcap 解析

def read_pcap(path):
    """返回 [(frame_bytes, ...)]；坏文件抛 ValueError（变异自检用它）。"""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 24:
        raise ValueError("pcap 头不足 24 字节")
    magic = struct.unpack("<I", data[:4])[0]
    if magic == 0xA1B2C3D4:
        endian = "<"
    elif magic == 0xD4C3B2A1:
        endian = ">"
    else:
        raise ValueError("pcap magic 0x%08x 不认识" % magic)
    off = 24
    frames = []
    while off + 16 <= len(data):
        _ts, _tu, incl, _orig = struct.unpack(endian + "IIII", data[off:off + 16])
        off += 16
        if incl > 262144 or off + incl > len(data):
            raise ValueError("pcap 记录长度越界")
        frames.append(data[off:off + incl])
        off += incl
    return frames


def _mac_str(b):
    return ":".join("%02x" % x for x in b)


def classify(frame):
    """把一帧拆成判据需要的字段；认不出就返回 None。"""
    if len(frame) < 14:
        return None
    dst = _mac_str(frame[0:6])
    src = _mac_str(frame[6:12])
    etype = struct.unpack(">H", frame[12:14])[0]
    if etype == 0x0806 and len(frame) >= 42:
        op = struct.unpack(">H", frame[20:22])[0]
        return {"kind": "arp", "op": op, "src": src, "dst": dst}
    if etype == 0x0800 and len(frame) >= 34:
        ip = frame[14:]
        ihl = (ip[0] & 0x0F) * 4
        if ihl < 20 or len(ip) < ihl + 8:
            return None
        proto = ip[9]
        if proto != 1:
            return {"kind": "ip", "proto": proto, "src": src, "dst": dst}
        ic = ip[ihl:]
        return {"kind": "icmp", "type": ic[0], "code": ic[1],
                "id": struct.unpack(">H", ic[4:6])[0],
                "seq": struct.unpack(">H", ic[6:8])[0],
                "len": len(ic),
                "data": bytes(ic[8:8 + 32]).decode("latin-1"),
                "src": src, "dst": dst}
    return {"kind": "other", "etype": etype, "src": src, "dst": dst}


def judge_pcap(frames):
    """返回问题列表；空列表 = 通过。"""
    problems = []
    if not frames:
        return ["pcap 里一帧都没有（网卡没发出任何东西）"]
    items = []
    for f in frames:
        c = classify(f)
        if c is None:
            problems.append("有认不出的帧（长度不足）")
            continue
        items.append(c)

    reqs = [c for c in items if c["kind"] == "arp" and c["op"] == 1]
    reps = [c for c in items if c["kind"] == "arp" and c["op"] == 2]
    if not reqs:
        problems.append("pcap 里没有 ARP request（对端 MAC 没解析过）")
    elif not any(c["src"] == OUR_MAC and c["dst"] == "ff:ff:ff:ff:ff:ff" for c in reqs):
        problems.append("ARP request 的源不是 %s 或目的不是广播" % OUR_MAC)
    if not reps:
        problems.append("pcap 里没有 ARP reply（没人回答我们，链路没通）")
    elif not any(c["dst"] == OUR_MAC and c["src"] != OUR_MAC for c in reps):
        problems.append("ARP reply 的目的 MAC 不是 %s" % OUR_MAC)

    for seq in (1, 2, 3):
        e8 = [c for c in items if c["kind"] == "icmp" and c["type"] == 8
              and c["id"] == ICMP_ID and c["seq"] == seq]
        e0 = [c for c in items if c["kind"] == "icmp" and c["type"] == 0
              and c["id"] == ICMP_ID and c["seq"] == seq]
        if not e8:
            problems.append("pcap 里没有 seq=%d 的 echo request（type 8）" % seq)
        elif not any(c["src"] == OUR_MAC and c["data"] == ECHO_DATA for c in e8):
            problems.append("seq=%d 的 echo request 源 MAC 不对或负载不是回显串" % seq)
        if not e0:
            problems.append("pcap 里没有 seq=%d 的 echo reply（type 0）" % seq)
        elif not any(c["dst"] == OUR_MAC and c["src"] != OUR_MAC for c in e0):
            problems.append("seq=%d 的 echo reply 目的 MAC 不是 %s（回环冒充？）"
                            % (seq, OUR_MAC))
    return problems


# ---------------------------------------------------------------- 串口判据

# 注意：串口行不是一帧一行就有边界 —— 内核日志会与 FS/驱动输出叠在同一行
# （实测 "\[FS\] blk ram0 found=1\[net\] send failed seq=1"），netif up 那行
# 前面还带 ANSI 颜色与 "NET:276 " 前缀。所以一律用 re.search 而不是 ^...$。
RE_FOUND = re.compile(r"\[net\] virtio-net: found at pci 00:01\.0")
RE_UP = re.compile(r"netif up: virtio-net ip 10\.0\.2\.15/24 gw 10\.0\.2\.2")
RE_PING = re.compile(r"\[net\] ping: 3/3 ok")
RE_STATS = re.compile(r"\[net\] stats tx=5 txdrop=0 rx=4 rxdrop=0 last_len=60")
RE_DONE = re.compile(r"\[net\] demo: done")
RE_NO_DEV = re.compile(r"\[net\] virtio-net: no device on pci bus 0")
RE_PING_FAIL = re.compile(r"\[net\] ping: 0/3 FAIL")
RE_SEND_FAIL = re.compile(r"\[net\] send failed seq=(\d)")
RE_REPLY = re.compile(r"\[net\] reply from")


def _reply_line(seq):
    return '[net] reply from 10.0.2.2 seq=%d len=60 data="%s"' % (seq, ECHO_DATA)


def judge_log(text):
    problems = []
    lines = [ln.rstrip() for ln in text.replace("\r\n", "\n").split("\n")]
    joined = "\n".join(lines)

    if not any(RE_FOUND.search(ln) for ln in lines):
        problems.append("缺 `[net] virtio-net: found at pci 00:01.0`（PCIe 上没认到设备）")
    if not any(RE_UP.search(ln) for ln in lines):
        problems.append("缺 netif up 行（lwIP 那张真网卡没建起来）")
    for seq in (1, 2, 3):
        if _reply_line(seq) not in joined:
            problems.append("缺 seq=%d 的 reply from 行" % seq)
    if not any(RE_PING.search(ln) for ln in lines):
        problems.append("缺 `[net] ping: 3/3 ok`")
    if not any(RE_STATS.search(ln) for ln in lines):
        problems.append("缺 `[net] stats tx=5 ... last_len=60`（读数与帧数对不上）")
    if not any(RE_DONE.search(ln) for ln in lines):
        problems.append("缺 `[net] demo: done`")
    if any(RE_SEND_FAIL.search(ln) for ln in lines):
        problems.append("出现了 send failed（发送路径报错，不该有回包）")
    return problems


def judge_nonic_log(text):
    problems = []
    lines = [ln.rstrip() for ln in text.replace("\r\n", "\n").split("\n")]

    if not any(RE_NO_DEV.search(ln) for ln in lines):
        problems.append("无网卡档没有报 `no device on pci bus 0`")
    if any(RE_UP.search(ln) for ln in lines):
        problems.append("无网卡档居然建起了 netif（QEMU 默认 NIC 没被去掉？）")
    sent = sorted(int(m.group(1)) for ln in lines for m in [RE_SEND_FAIL.search(ln)] if m)
    if sent != [1, 2, 3]:
        problems.append("无网卡档 send failed seq 序列 %s != [1, 2, 3]" % sent)
    if not any(RE_PING_FAIL.search(ln) for ln in lines):
        problems.append("无网卡档没有 `[net] ping: 0/3 FAIL`")
    if any(RE_REPLY.search(ln) for ln in lines):
        problems.append("无网卡档居然有 reply from 行")
    return problems


# ---------------------------------------------------------------- E 变异自检

def mutation_self_check(good_log, good_frames, nonic_log):
    cases = []

    def expect_red(name, problems):
        cases.append((name, problems))

    lines = good_log.replace("\r\n", "\n").split("\n")
    # 1) 去掉 seq=2 的 reply
    m = [ln for ln in lines if _reply_line(2) not in ln]
    expect_red("日志少一条 reply", judge_log("\n".join(m)))
    # 2) 3/3 改成 2/3
    m = [ln.replace("ping: 3/3 ok", "ping: 2/3 ok") for ln in lines]
    expect_red("ping 计数被改", judge_log("\n".join(m)))
    # 3) 改 payload 第一个字节
    m = [ln.replace('data="@ABC', 'data="#ABC') for ln in lines]
    expect_red("回显负载被改", judge_log("\n".join(m)))
    # 4) 去掉 found 行
    m = [ln for ln in lines if not RE_FOUND.search(ln)]
    expect_red("日志少 found 行", judge_log("\n".join(m)))

    # 5) 拿掉 ARP reply
    m = [f for f in good_frames if (classify(f) or {}).get("op") != 2]
    expect_red("pcap 少 ARP reply", judge_pcap(m))
    # 6) 把 seq=3 的 echo reply 改成 seq=4
    m = []
    for f in good_frames:
        c = classify(f)
        if c and c["kind"] == "icmp" and c["type"] == 0 and c["seq"] == 3:
            # 直接改 ip 头之后的 ICMP seq：定位到 ICMP 起点再写两个字节
            b = bytearray(f)
            ihl = (b[14] & 0x0F) * 4
            base = 14 + ihl
            b[base + 6:base + 8] = struct.pack(">H", 4)
            f = bytes(b)
        m.append(f)
    expect_red("pcap 里 reply 的 seq 被改", judge_pcap(m))
    # 7) 换掉 echo request 的源 MAC
    m = []
    for f in good_frames:
        c = classify(f)
        if c and c["kind"] == "icmp" and c["type"] == 8:
            b = bytearray(f)
            b[6:12] = bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0x01])
            f = bytes(b)
        m.append(f)
    expect_red("pcap 里源 MAC 被换", judge_pcap(m))
    # 8) 空帧表
    expect_red("pcap 一帧都没有", judge_pcap([]))

    # 9) 无网卡档里插一条 netif up（模拟默认 NIC 又回来了）
    m = nonic_log.replace("\r\n", "\n").replace(
        "[net] virtio-net: no device on pci bus 0",
        "[net] virtio-net: no device on pci bus 0\n"
        "[net] netif up: virtio-net ip 10.0.2.15/24 gw 10.0.2.2")
    expect_red("无网卡档冒出 netif up", judge_nonic_log(m))
    # 10) 无网卡档把 0/3 改成 3/3
    m = nonic_log.replace("ping: 0/3 FAIL", "ping: 3/3 ok")
    expect_red("无网卡档计数被改", judge_nonic_log(m))

    green = [name for name, probs in cases if not probs]
    if green:
        bad("self-check", "%d 种坏输入被判绿：%s" % (len(green), "、".join(green)))
    else:
        ok("self-check", "%d 种坏日志/坏 pcap 全部判红" % len(cases))


# ---------------------------------------------------------------- main

def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--la64-gcc", default=None)
    ap.add_argument("--keep", action="store_true",
                    help="保留本次的串口日志与 pcap（排查用）")
    ap.add_argument("--seconds", type=float, default=60.0)
    args = ap.parse_args(argv)

    qemu = _pick(QEMU_CANDIDATES, args.qemu, "qemu-system-loongarch64")
    gcc = _pick(LA64_FALLBACKS, args.la64_gcc, "loongarch64-newlib-elf-gcc")
    if qemu is None or gcc is None:
        print("\nNET-VIRTIO-GATE FAIL (missing toolchain)")
        return 1

    work = os.path.join(tempfile.gettempdir(), "ark_net_gate")
    if not os.path.isdir(work):
        os.makedirs(work)
    log1 = os.path.join(work, "net_run.log")
    log2 = os.path.join(work, "net_nonic.log")
    pcap = os.path.join(work, "net_run.pcap")

    check_default_off()

    built, why = build_image()
    if not built:
        bad("build-on", "LA64 网卡档构建失败：%s" % why)
    else:
        ok("build-on", "LA64 + -DARKRTOS_USE_NET_VIRTIO=1 -DARKRTOS_NET_DEMO=1 全量构建通过")

    if built:
        if os.path.isfile(pcap):
            os.remove(pcap)
        text1 = run_qemu(qemu, True, pcap, args.seconds, BUILD_DIR)
        with open(log1, "wb") as f:
            f.write((text1 or "").encode("utf-8"))
        if text1 is None:
            bad("qemu-run", "QEMU 没有产出任何串口输出")
        else:
            probs = judge_log(text1)
            if probs:
                bad("qemu-run", "; ".join(probs))
            else:
                ok("qemu-run", "串口六类读数齐全（found / netif up / 三条 reply / 3-3 ok / stats / done）")

            if not os.path.isfile(pcap):
                bad("pcap", "filter-dump 没产出 pcap")
            else:
                try:
                    frames = read_pcap(pcap)
                except ValueError as e:
                    frames = []
                    bad("pcap", "读 pcap 失败：%s" % e)
                else:
                    probs = judge_pcap(frames)
                    if probs:
                        bad("pcap", "; ".join(probs))
                    else:
                        ok("pcap", "%d 帧：ARP req/reply + 三条 echo 往返，MAC/%s id=%d 全对"
                           % (len(frames), OUR_MAC, ICMP_ID))

                    # 变异自检（用真实产物做基准）
                    if text1 is not None:
                        nonic_text = run_qemu(qemu, False, None, args.seconds, BUILD_DIR)
                        with open(log2, "wb") as f:
                            f.write((nonic_text or "").encode("utf-8"))
                        if nonic_text is None:
                            bad("no-nic", "无网卡档没有串口输出")
                        else:
                            probs = judge_nonic_log(nonic_text)
                            if probs:
                                bad("no-nic", "; ".join(probs))
                            else:
                                ok("no-nic", "-net none 时 no device / 三次 send failed / 0-3 FAIL 全对")
                            mutation_self_check(text1, frames, nonic_text)

    if not args.keep:
        for p in (log1, log2, pcap):
            if os.path.isfile(p):
                os.remove(p)

    restored, why = restore_default_build()
    if not restored:
        bad("restore", "收尾没能把构建目录恢复成默认档：%s" % why)
    else:
        ok("restore", "构建目录已恢复默认档（不留网卡档 .o）")

    print("")
    if FAILS:
        print("NET-VIRTIO-GATE FAIL: %d 项不成立" % len(FAILS))
        return 1
    print("NET-VIRTIO-GATE OK: %d 项全过" % len(PASSES))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))