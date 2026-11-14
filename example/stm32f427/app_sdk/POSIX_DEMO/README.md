# POSIX_DEMO：一份未修改的 POSIX 程序，直接编成 Ark RTOS App

这份示例要证明一句话：**把一个为 Linux / Windows 写的控制台程序搬到 Ark RTOS 上，改的是 include 列表，不是程序结构。**

`Src/main.c` 里没有 `arkrtos.h`、没有控制台设备名、没有地址、没有分区号和槽位号。它只用
`<stdio.h>` / `<string.h>` / `<stdlib.h>` / `<ctype.h>` 加 `<unistd.h>` / `<pthread.h>` / `<semaphore.h>`。
同一份源码在 Linux 上 `cc main.c -pthread` 也能编。

## 编译、打包、安装

App 走的是**镜像安装**路径（不是把 axf 直接烧到某个地址），一共三步。

```bash
# 1) 编译：BeforeMake 先跑 gen_app_sct.py 生成散列文件，再链接出 axf
#    （UV4 成功也常返回退出码 1，看日志里的 "Program Size:" / "0 Error(s)"）
env -u PYTHONHOME -u PYTHONPATH "D:/Keil_v5/UV4/UV4.exe" -r \
    example/stm32f427/app_sdk/POSIX_DEMO/MDK-ARM/posix_demo.uvprojx \
    -o "C:\\tmp\\posix_demo.log"

# 2) 打包成 .svcapp（四遍差分链接，含镜像头、重定位表与逐字节自检）
env -u PYTHONHOME -u PYTHONPATH py -3 tools/pack_app.py \
    --project example/stm32f427/app_sdk/POSIX_DEMO/MDK-ARM/posix_demo.uvprojx \
    --type app --ram-size 16384 \
    --out build/POSIX_DEMO/POSIX_DEMO.svcapp

# 3) 目标板：控制台里开安装窗口，再把镜像发过去
#    install 2
#    <等设备回 "send the file now"，然后从同一个串口发 .svcapp>
#    tools/send_image.py 只能发文件、不会自己开 install 窗口，
#    所以要么先手动敲 install，要么用脚本共用同一个串口。
```

安装成功后设备回 `INSTALL: ok`，应用自动起跑。

**不要用 pyocd / UV4 -f 把 axf 直接烧到槽基址**：开发散列把这些 App 链接在 `0x080C0000`（不是槽基址），
而且裸 axf 没有镜像头和重定位表。烧进去的现象是**任务在跑、但一行都不打印**——看起来像串口坏了，
实际是镜像从未被加载器认可。

## 两处不能少的东西

| 位置 | 为什么 |
|------|--------|
| BeforeMake 钩子 `--heap-size 2048` | `main.c` 用了 `malloc`。散列默认不划库堆区，不给就会在链接期报 `L6915E: Heap was used, but no heap region was defined` |
| BeforeMake 钩子 `--ram-size 16384` | POSIX 兼容层自己有静态占用（`arkrtos_pth_stacks[]` 线程栈表 + 堆 arena）。实测 `arkrtos_posix.o` ZI 3904，加本程序 RW/ZI 4348、App 主栈 4096、库堆 2048 ≈ 10.5 KB，声明 16 KB 留余量 |
| 源文件里**没有** `arkrtos_app_main.c` | 它自带强 `int main(void)`，与用户 `main` 一起会产出两个 `__ARM_use_no_argv` 定义，链接期 `L6200E` |
| `.uvoptx` 的 `<TargetName>` 必须是 `POSIX_DEMO` | 它是 Keil 下载/调试入口的索引。复制别的工程当模板忘了改，调试器会以 `Internal DLL Error` / `Flash Download failed - Target DLL has been cancelled` 失败——现象像硬件坏了，实际只是这一个字段 |

### RAM 窗口：镜像声明的 ram_size 不能超过槽登记的上限

固定槽模式（`mode = fixed`）下，加载器把槽配置里的 `ram_size` 当作**上限**：

```c
/* kernelsrc/src/arkrtos_loader.c : arkrtos_loader_reserve_fixed() */
if((img_ram > slots[i].ram_size) && slot_pinned)   return ARKRTOS_LOADER_ERR_SIZE; /* -4 */
```

本示例第一次安装就撞在这里——镜像声明 16384，而板上槽 2 登记的是 8192（`0x2000`）：

```
INSTALL:262 rejected: err -4, type 1, payload 10532 B, reloc 56
```

正确做法是**改槽配置**（把该槽的 `ram_size` 提到 `0x4000` 并追加一条新记录），
不是去裁 POSIX 静态表把自己压进 8 KB：

```bash
env -u PYTHONHOME -u PYTHONPATH py -3 tools/arkrtos_layout.py build --seq <新序号>
env -u PYTHONHOME -u PYTHONPATH py -3 tools/arkrtos_cfg.py write --bin <生成的文件> --reboot
```

配置区是 256 条 512 B 记录的**追加写**：`cfg load: ok, record N` 里的 N 就是当前活动记录索引，
旧记录不擦、可回退。注意 `cfg show` **不打印每槽 ram_size**，要读记录得先让 CPU 停下再看 Flash，
否则读回来的字是随机的（详见 `docs/Ark RTOS应用安装与调试指南.md` §1.3）。

## 验证结论

**上板验证过**（STM32F427 + DAPLink，控制台 COM21 / 115200 8N1）。板上实测段落：

```
POSIX demo: ...
rwlock  : PASS (0 violations)
barrier : PASS (arrived=2 serial=1 bad=0)
done    : entering heartbeat
heartbeat   (每 5 秒一行)
```

覆盖的是：`printf` 真从控制台出来；`pthread_create` / `pthread_join` / `sem_wait` / `qsort` / `malloc` /
`sleep` 按 POSIX 语义工作；`pthread_rwlock_*` 的「读锁叠加—放掉后可写—写者持有时读者被拒—destroy」单任务状态机；
`pthread_barrier_*` 的 3 参与者同步（恰好 1 个 `PTHREAD_BARRIER_SERIAL_THREAD`）。
安装后在 `app list` 里它是 RUNNING 的那个实例（`install 2` 落固定槽 2 → base `0x080A0000`、ram `0x20014000`）。

**未覆盖**（不要读成读写锁/屏障已完备）：

- 多读者**真并发重叠**——读写锁自检是单任务顺序走的，没有两个读者同时持锁的用例；
- 写者优先（不饿死）——当前实现不保证写者排队优先；
- 屏障在多次复用、超时路径、参与者提前退出下的行为。

完整输出与逐条结论见 `docs/POSIX与Windows兼容层说明.md` 第 10 节。
