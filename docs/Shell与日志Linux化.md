# Shell 与日志：往 Linux 操作习惯上靠

> 目标：让从 Linux 过来的人**不用重新学一套**——提示符、路径、命令名、输出措辞、
> 日志形态都照 Linux 来。
>
> 验证分档：**编译通过 / 主机测试通过 / 上板验证过**。本文所列截止当前为**编译通过**
> （AC6 `-fsyntax-only` rc=0）；上板验证状态见文末。

## 1. 提示符与工作目录

提示符是完整的 bash 形态：

```
ark@arkrtos:<cwd>$
```

- `cwd` 是当前工作目录；等于根 `/` 时显示 `~`（与 bash 的 `$HOME` 行为一致）。
- 启动即给出提示符，不再用裸 `ark> `。

```
ark@arkrtos:~$ pwd
/
ark@arkrtos:~$ cd /dev
ark@arkrtos:/dev$ ls
BLED  led0  led1  led2  ttyS0
ark@arkrtos:/dev$ cd ..
ark@arkrtos:~$ 
```

**路径解析**：`ls` / `cat` / `rm` / `cd` 的参数按当前目录解析，支持绝对路径、相对路径、
`.`、`..`、`~`、`~/x`。规范化按 Linux 规则（`//` 合并、`.` 去掉、`..` 消化前面的段），
越过根报错而非悄悄折回根。

`cd` 只在该路径**存在且是目录**时才改变当前目录，否则报错并保持原目录不变。

## 2. 命令面

### 2.1 新增的 Linux 风格命令

| 命令 | 说明 |
|------|------|
| `pwd` | 打印当前工作目录 |
| `cd [dir]` | 切换工作目录 |
| `ls [-l] [-a] [path]` | 列目录；`-l` 附类型字符与大小，`-a` 追加 `.` / `..` |
| `cat <path>` / `rm <path>` | 打印 / 删除文件 |
| `mount` / `umount` | 挂载点列出与增删 |
| `df` | 挂载点与卷用量 |
| `ps` | 任务与镜像概览 |
| `free` | 内存概览（内核 RAM 窗口 + 镜像池） |
| `uname [-a]` | 系统与构建信息 |
| `history` | 历史命令 |
| `which <cmd>` | 别名解析到真实命令 |
| `man <cmd>` | 等于 `help <cmd>` |
| `dmesg [-c]` | 打印内核日志环，`-c` 打印后清空 |

### 2.2 别名

| 别名 | 真实命令 |
|------|----------|
| `ll` | `ls -l` |
| `la` | `ls -a` |
| `dir` | `ls` |
| `cls` | `clear` |
| `halt` / `poweroff` | `reboot` |

`which ll` 会回答 `ll: alias for 'ls -l'`。

### 2.3 退出码与单项帮助

- **`$?`**：打印上一条命令的返回码（Linux 口径：0 = 成功，非 0 = 失败；未知命令 127）。
- **`cmd -h` / `cmd --help`**：打印该命令的用法，等价 `help <cmd>` / `man <cmd>`。
- **错误文案**按 Linux 措辞：`<name>: command not found`、`<cmd>: too many arguments`、
  `<path>: No such file or directory`。

### 2.4 诚实口径（不编造）

内核不记的账，一律留空或打 `-`，**不猜一个像样的答案**：

- `ls -l` 只给类型字符 + 大小 + 名字；**没有 owner / 时间戳**字段（VFS 不提供）。
- `ps` **没有 TTY / CPU 时间**列（内核无此记账）。
- `free` 报的是分区表 RAM 段与镜像池；内核**没有堆**，`used` / `free` 打 `n/a`。
- `df` 对未知容量打 `-`。

## 3. 日志：dmesg 风格

每行形如：

```
[    1.234] <6>VFS:347 namespace up: / (ramfs) with /tmp /etc /home /mnt /media, /dev (devfs)
```

| 字段 | 含义 |
|------|------|
| `[  秒.毫秒]` | 自启动以来的时刻 |
| `<n>` | syslog 优先级：`<3>` err、`<4>` warn、`<6>` info、`<7>` debug |
| `标签:行号` | 模块名 + **产生该日志的源码行**，便于直接定位 |

- 运行期级别仍用 `log [0..4]` 调（0=off…4=debug）。
- **历史日志**由日志环承载：`dmesg` 打印（最旧→最新），`dmesg -c` 打印后清空。
- 日志环**默认开**，容量 `ARKRTOS_LOG_RING_SIZE`（默认 4096 B）；关断时 `dmesg` 报容量 0。
- 环满覆盖最旧一条；dump 分两段回调时**不保证整行连续**，按段读取即可。

## 4. 设备与命名空间

设备节点一律 Linux 风格：

| 旧名 | 新名 |
|------|------|
| `COM1` | `ttyS0` |
| `LED` | `led0` |
| `LED2` | `led1` |
| `LED3` | `led2` |
| `nor0` | 不变（本就是 Linux 风格） |

`/dev` 由设备注册表镜像而来，所以 `ls /dev` 与 `arkrtos_dev_open()` 看到的是**同一批名字**。

根命名空间是 Linux 的标准子集：

```
/            易失根（ramfs，掉电即失）
/dev         设备注册表镜像（devfs）
/tmp         临时数据（根 ramfs 上的普通目录）
/etc         配置（hostname 等）
/home        用户目录
/mnt/<名字>  持久卷（littlefs），由上层显式挂
/media       可移除介质挂载点（预留）
```

> `/tmp` / `/etc` / `/home` / `/mnt` / `/media` 是**同一棵 ramfs 上的普通目录**，
> 不是独立挂载点——所以 `mount` / `df` 把它们算在 `/` 里，与没有专门 `/tmp` 挂载的系统一致。

## 5. 配置开关

| 宏 | 默认 | 说明 |
|----|------|------|
| `ARKRTOS_SHELL_PROMPT_LINUX` | y | 1 = linux 形态提示符；0 = 回退到固定的备用提示符 |
| `ARKRTOS_USE_LOG_RING` | y | 日志环总开关 |
| `ARKRTOS_LOG_RING_SIZE` | 4096 | 日志环容量（字节），依赖 `ARKRTOS_USE_LOG_RING`；**RAM 紧张的工程可在自己工程的 Misc Controls 里覆盖**，见下 |
| `SHELL_DEV_NAME` | `"ttyS0"` | 控制台串口设备名 |
| `INSTALLER_DEV_NAME` | `"ttyS0"` | 镜像接收设备名 |

### 5.1 F401 的环尺寸覆盖

F401 内核 RAM 窗口只有 56 KB，本轮的 cwd/新命令/日志环加起来超出执行区约 3 KB，
链接报 `L6406E: No space in execution regions`。F401 工程在自己的 **Misc Controls**
里加了：

```
-DARKRTOS_LOG_RING_SIZE=512
```

生成的 `arkrtos_autoconf.h` 用 `#ifndef` 包住这个宏，所以命令行覆盖生效，而 F427 仍用
Kconfig 的默认 4096。覆盖后 F401 链接通过（RAM 余 ~480 B、Flash 余 ~14 KB）。

## 6. 验证状态

- **编译通过**：内核源文件 AC6 `armclang -fsyntax-only` rc=0（含合法裁剪变体）。
- **上板验证过**：待补（F427 / COM3 实机见《内核Shell控制台使用说明.md》§4.3）。
