# MPOOL_DEMO：把固定块内存池开给 App 用（SVC 0x20）

这份例程是 [固定块内存池](../../../../docs/固定块内存池.md) §七里那条「非特权通道」
的判据实现：**一个 `.svcapp` 经 SVC 0x20 分配/释放并打印块地址**。

它跑在 App 自己的非特权任务里，池的内存区就是 App 自己的一段静态数组
（`static uint8_t s_pool[32 * 16]`）。所有 `arkrtos_mpool_*` 调用都过 SVC 边界，
所以它同时验证三件事：

| 段落 | 覆盖 |
|------|------|
| 创建与几何 | `create` 拿到句柄；`query` 报出的名字、块大小、块数、空闲数、峰值都对 |
| 发放 | 连拿 3 块：非空、**在调用者数组内**、按指针宽度对齐、互不相同、可读可写 |
| 记账 | `free_count` 逐次递减，`alloc_ok` / `peak_used` 跟着走 |
| 释放与复用 | `free` 返回 0；**刚还回去的那块是下一块被发出的**（LIFO 空闲链） |
| 拒绝不动状态 | 外来指针释放返回 -1，`free_count` 不变、`free_bad` 只 +1 |
| 边界校验 | 重名 / 块小于指针宽度（2 B）/ 块未按 4 对齐（30 B）/ 块数 0 / 名字超长 / 池内存区不在调用者 RAM（拿 `main` 的入口地址当样本）—— 六类全部被拒 |
| 用尽 | 4 块的池发完 4 块，第 5 次拿到空指针，`alloc_fail` +1，还完恢复 4 块空闲 |
| 生命周期 | `delete` 后 `alloc` 返回空、`query`/`free` 返回负值；**名字可被重新占用** |

最后两行是 `MPOOL: N pass, M fail` 与 `MPOOL: ALL PASS` / `MPOOL: FAILED`，
`main` 的返回值跟着 `fail` 走。

## 实测（STM32F427，2026-11-13）

```
--- mpool demo (SVC 0x20) ---
pool base=20010050 block=32 count=16
create -> 0
query  name=demo block=32 count=16 free=16 peak=0
alloc  a=20010050 b=20010070 c=20010090
query  free=13 alloc_ok=3 peak=3
free(foreign) -> -1
query  free=13 free_bad=1
ptr width=4 byte(s)
create demo2 -> 1
query  free=0 alloc_ok=4 alloc_fail=1
MPOOL: 33 pass, 0 fail
MPOOL: ALL PASS
```

体积：`Code=4456 RO-data=1636 RW-data=0 ZI-data=4944`；打包后
`build/mpool_demo.svcapp` 6688 B（负载 6092 B），重定位表 85 项，
RAM 声明 8192 B（RW/ZI 720 + 栈 4096 = 4816）。

> **口径提醒**：ARM 上指针宽度是 **4** 字节，所以「块太小」要取 2、
> 「块未对齐」要取 30；`block_size=4` 与 `20` 都是合法值。例程第一版把这两条
> 判据写反，板端给出的两个 FAIL 是判据的错，不是内核的错。

## 编译、打包、上板

```bash
# 1. 编译（0 err；两条 nonnull 比较 warning 来自 AC6 头文件的属性）
env -u PYTHONHOME -u PYTHONPATH "D:/Keil_v5/UV4/UV4.exe" -b \
    example/stm32f427/app_sdk/MPOOL_DEMO/MDK-ARM/mpool_demo.uvprojx -j0 -o C:/tmp/mpool.log

# 2. 打包（四遍差分链接，打包期就证明镜像可重定位）
env -u PYTHONHOME -u PYTHONPATH py -3 tools/pack_app.py \
    --project example/stm32f427/app_sdk/MPOOL_DEMO/MDK-ARM/mpool_demo.uvprojx \
    --type app --name MPOOL_DEMO --version 1.0.0 --out build/mpool_demo.svcapp

# 3. 送进设备 NOR 卷
MSYS_NO_PATHCONV=1 env -u PYTHONHOME -u PYTHONPATH py -3 tools/fs_put.py \
    --port COM21 --path /mpool_demo.svcapp build/mpool_demo.svcapp

# 4. 装到槽位 0 并运行（槽位被占则先 app uninstall <slot>）
#    app install /mpool_demo.svcapp 0
```

镜像带自启标志，`app install` 成功后立刻跑完一轮并把读数打到控制台；
任务结束即退场，不存在常驻循环。

## 顺手修掉的一个 SDK 缺陷

编这个例程时发现 `kernelsrc/sdk/app_sdk/arkrtos_stdio.c` 在 **AC6 下编不过**：
AC6 的 `<stdio.h>` 把 `putchar` 定义成宏（`#define putchar(c) putc(c, stdout)`），
SDK 里那句 `int putchar(int c)` 被宏展开成语法错误，整个工程
`3 Error(s)`（`HELLO_APP` 同样编不过，只是它此前只走过 GCC 那条链）。
修法是在定义前 `#ifdef putchar / #undef putchar / #endif`，源码其余部分一字未改。
