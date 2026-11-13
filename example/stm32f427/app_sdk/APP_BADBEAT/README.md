# APP_BADBEAT：故意违反心跳合同的应用（负向用例）

它和 `APP_BAD` 是一对：那个测「越权被拦」，这个测「违约被记账」。

```c
printf("BEAT_BAD: declare heartbeat 1000 ms, then stay silent 3000 ms per round\n");
(void)arkrtos_heartbeat(1000u);          /* 我说：我最久隔 1 s 报一次到 */

for(;;)
{
    arkrtos_task_wait(3000u);            /* 实际：每轮沉默 3 s */
    printf("BEAT_BAD: woke after 3000 ms while promising 1000 ms\n");
}
```

它**不是死循环、也不崩**：每轮照样醒、照样打印，只是报到太晚。这正是
[崩溃恢复与镜像版本把关](../../../../docs/崩溃恢复与镜像版本把关.md) §1.5 那句话的
可执行形态——**判定只看报到的间隔，不看任务当时在干什么**。

## 怎么用

```bash
# 1) 打包（工程里 BeforeMake 钩子会按 --dev-slot 生成 dev 用 .sct）
py -3 tools/pack_app.py \
    --project example/stm32f427/app_sdk/APP_BADBEAT/MDK-ARM/beat_bad.uvprojx \
    --type app --version 1.0.0 --name beat_bad --dev-slot 0 \
    --out build/beat_bad.svcapp
# → 2692 B（负载 2408 B、重定位 7 项 ROM6/RAM1、自启）

# 2) 上板：装进槽 0
MSYS_NO_PATHCONV=1 py -3 tools/fs_put.py --port COM21 --path /beat.svcapp build/beat_bad.svcapp
# 串口：fs mount / app stop 0 / app install /beat.svcapp 0

# 3) 读数（判据只有内核自己的账）
guard        # contracts: 1 declared, 1 violating now；task 4 period=1000 beat_ms≈2700
crash        # slot 0 app faults=... last=heartbeat hold=heartbeat
crash        # 再 reboot 两次，faults 到 3 后 hold=heartbeat，app list 显示 INVALID / auto=no
```

## 板上实测（2026-11-13，F427 + DAPLink）

装好当次：

```
app list: 0  app  RUNNING  yes  4  1  none  0x08060000 ...
guard   : contracts: 1 declared, 1 violating now
          4  0  1000  2713  2713  2713  17  wait
```

`reboot` 之后（这是第三次违约）**开机日志自己就把结论打出来**：

```
[    0.003] <3>CRASH:311 slot 0 held: heartbeat, 3 faults, last at tick 2200 (retry with 'app start 0' or 'drv start 0')
```

```
crash: slot 0  app  3  heartbeat  heartbeat  2200
app list: 0  app  INVALID  no  0  3  heartbeat  ...
```

人工重试 `app start 0`：`crash/held` 清零、应用重新跑起来（`auto` 仍是 `no`）。
**同一时刻守卫立刻停止喂狗**：

```
[   10.400] <3>GUARD:348 not feeding: verdict 0x2 (audit 0x0, violating 1)
```

这条是顺带拿到的：违约还在时看门狗不再被喂，4 s 后硬件复位——"违约到一定程度
机器自己重启"不是文档里的承诺，是这条读数。

## 两个容易踩的地方

- **违约要攒几次才禁**：`arkrtos_crash_fault()` 对心跳违约每次开机最多记一条
  （见 `arkrtos_guard.c` 里那段注释），所以从装上到 `hold=heartbeat` 要经过
  `fault_restart_max` 次开机。只 reboot 一次看到 `faults=1` 是正常的。
- **手工 `app start` 会清账**：它是"显式重试"，`crash`/`held` 归零；要重现禁用
  就再次 reboot。
