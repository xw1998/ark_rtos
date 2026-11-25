/**
* @file arkrtos_shell_image.c
* @brief 镜像与安装：info / app / drv / install。
*          这一族直接读写装载器与镜像池的账本，不碰文件系统与页表。
* @note 本文件属于内核特权态代码；由 arkrtos_shell.c 按命令族拆出，
*       只做搬运，命令逻辑与拆分前逐字相同。
*/

#include "arkrtos_shell_internal.h"
#if (ARKRTOS_USE_ROLLBACK == 1)
#include "arkrtos_rollback.h"
#endif

#if (SHELL_ENABLE == 1)

/* ============================================================
 * info：内核与分区概览
 * ============================================================ */
/* 池地图：按地址升序列出池内每个镜像的落点与跨度。
 * 线性扫「下一个更大的落点」而不是排序整个数组：记录数上限是 16，
 * 这点平方开销无所谓，却省掉一份临时副本。 */
static void sh_pool_map(const arkrtos_partition_table_t *pt)
{
    uint32 last = 0u;
    uint32 printed = 0u;
    uint32 n;

    ark_shell_printf("\r\npool map (address order)\r\n");

    for(n = 0u; (n < pt->slot_max) && (n < ARKRTOS_SLOT_ARRAY_MAX); n++)
    {
        uint32 i;
        uint32 best = 0u;
        uint32 best_idx = 0xFFFFFFFFu;

        for(i = 0u; (i < pt->slot_max) && (i < ARKRTOS_SLOT_ARRAY_MAX); i++)
        {
            uint32 base = pt->slot_base[i];

            if((pt->slot_type[i] == ARKRTOS_SLOT_FREE) || (base == 0u))
            {
                continue;
            }

            if((base < last) || ((best_idx != 0xFFFFFFFFu) && (base >= best)))
            {
                continue;
            }

            best     = base;
            best_idx = i;
        }

        if(best_idx == 0xFFFFFFFFu)
        {
            break;
        }

        ark_shell_printf("  0x%08X +%-5uB  %-4s %-8s  slot %u\r\n",
                         best, pt->slot_size[best_idx],
                         slot_type_name(pt->slot_type[best_idx]),
                         slot_state_name(pt->slot_state[best_idx]),
                         best_idx);

        last = best + 1u;
        printed++;
    }

    if(printed == 0u)
    {
        ark_shell_printf("  (pool is empty)\r\n");
    }
}

int cmd_info(int argc, char *argv[])
{
    arkrtos_partition_table_t *pt = arkrtos_ptable_get();
    uint32 used = 0u;
    uint32 total = 0u;
    uint32 frag = 0u;

    (void)argc;
    (void)argv;

    (void)arkrtos_ptable_stats(&used, &total, &frag);

    ark_shell_printf("\r\nArk RTOS kernel shell\r\n");
    ark_shell_printf("version   : " ARKRTOS_VERSION_STRING "\r\n");
    ark_shell_printf("build     : " __DATE__ " " __TIME__ "\r\n");
    ark_shell_printf("partition : ABI v%u, hw 0x%08X\r\n",
                     (uint32)pt->version, pt->hw_compat_id);
    ark_shell_printf("kernel    : flash 0x%08X +%uK, ram 0x%08X +%uK\r\n",
                     pt->kernel_base, pt->kernel_size / 1024u,
                     pt->kernel_ram_base, pt->kernel_ram_size / 1024u);
    /* 池：按 1KB 分配单元细粒度落位，尾部 pool_reserve 个扇区留作压实余量 */
    ark_shell_printf("pool      : 0x%08X +%uK (%u sectors of %uK), unit %uB, reserve %u\r\n",
                     pt->pool_base, pt->pool_size / 1024u,
                     pt->pool_units, pt->pool_sector / 1024u,
                     pt->pool_alloc_unit, pt->pool_reserve);
    ark_shell_printf("pool used : %uK of %uK, free %uK, fragments %u\r\n",
                     used / 1024u, total / 1024u,
                     (total - used) / 1024u, frag);
    ark_shell_printf("image ram : 0x%08X +%uK, block %uB..%uK, %u slots\r\n",
                     pt->image_ram_base, pt->image_ram_total / 1024u,
                     pt->image_ram_min_block, pt->image_ram_max_block / 1024u,
                     pt->slot_max);
    ark_shell_printf("tasks     : %d used / %u max\r\n",
                     (int)arkrtos_task_count, (uint32)ARKRTOS_TASK_MAX_NUM);
    ark_shell_printf("tick      : %u ms, period %u us\r\n",
                     arkrtos_kernel_get_time(), (uint32)ARKRTOS_TICK_PERIOD_US);

    sh_pool_map(pt);

    return 0;
}

/* ============================================================
 * app：App 影像列表 / 启动 / 停止
 * @details v4 起 App 与驱动共用同一个镜像池，
 *          因此命令里的 <slot> 是“分区表记录号”，
 *          列表仅展示类型匹配的记录。
 * ============================================================ */
int cmd_app(int argc, char *argv[])
{
    arkrtos_partition_table_t *pt = arkrtos_ptable_get();
    uint32 i;
    uint32 slot;

#if (ARKRTOS_USE_APPRUN == 1)
    /* 板上直跑：app run <path> 把 arkc 编出的裸文件装进固定窗口执行。
     * 与 install 无关（不占槽位、不写 Flash），所以单独一条子命令而不是
     * install 的选项：两者的失败后果完全不同。 */
    if((argc == 3) && (strcmp(argv[1], "run") == 0))
    {
        uint32 app_task = 0u;
        int32 rc = arkrtos_apprun_run_ex(argv[2], &app_task);

        if(rc != 0)
        {
            const char *name = arkrtos_apprun_error_name(rc);

            ark_shell_printf("\r\napp run: %s failed, rc=%d%s%s\r\n",
                             argv[2], (int)rc,
                             (name != 0) ? " - " : "", (name != 0) ? name : "");
            if(rc == ARKRTOS_LOADER_ERR_SIZE)
            {
                /* 上限是窗口决定的，不是文件系统的：把它报出来，否则用户只能猜。 */
                ark_shell_printf("  image limit: %u B (code window at 0x%08X)\r\n",
                                 (unsigned)ARKRTOS_APPRUN_MAX_IMAGE,
                                 (unsigned)ARKRTOS_APPRUN_BASE);
            }
            if(rc == ARKRTOS_APPRUN_ERR_LAYOUT)
            {
                /* 说清是哪一条不满足：这份镜像可能是给别人链的，改一下
                 * scatter 就行，不是程序写错了。 */
                ark_shell_printf("  vector table: +0 must be 0x%08X (window top), "
                                 "+4 must be inside 0x%08X..0x%08X, file >= 8 B\r\n",
                                 (unsigned)(ARKRTOS_APPRUN_RAM_BASE + ARKRTOS_APPRUN_RAM_SIZE),
                                 (unsigned)ARKRTOS_APPRUN_BASE,
                                 (unsigned)(ARKRTOS_APPRUN_BASE + ARKRTOS_APPRUN_CODE_SIZE));
            }
            if(rc == ARKRTOS_APPRUN_ERR_WINDOW)
            {
                /* 池里没能按窗口大小拿到那一块：通常是上一次的 app 还没退出，
                 * 或者池表满了（'free' 看 fragment、'task' 看还在跑的程序）。 */
                ark_shell_printf("  the %u-byte window at 0x%08X is not available "
                                 "(see 'free' and 'task')\r\n",
                                 (unsigned)ARKRTOS_APPRUN_TOTAL_SIZE,
                                 (unsigned)ARKRTOS_APPRUN_BASE);
            }
            if(rc == ARKRTOS_APPRUN_ERR_PROLOGUE)
            {
                ark_shell_printf("  entry from vector table +4 does not start with 0x%04X\r\n",
                                 (unsigned)ARKRTOS_APPRUN_PROLOGUE);
            }
            if(rc == ARKRTOS_APPRUN_ERR_BUSY)
            {
                sh_out("  a previous 'app run' is still alive (see 'task')\r\n");
            }
            if(rc == ARKRTOS_LOADER_ERR_STATE)
            {
                /* 前置条件是 VFS 起来，不是 littlefs 卷挂上：镜像从 /tmp 读，
                 * 那是 VFS 的 ramfs。所以这里不能再说「去 fs mount」。 */
                sh_out("  the VFS is not up (see the boot log; 'ls /' proves it)\r\n");
            }
            if(rc == ARKRTOS_MINI_ERR_FS)
            {
                /* 读文件失败的原因不在 arkrtos_fs 那一层，别把它的错误码搬来
                 * 冒充：这里只能说清是哪一步、该去查什么。 */
                sh_out("  cannot read the image (check 'ls' and the image limit)\r\n");
            }
            return 1;
        }

        /* app_task 是起跑那一刻的任务号（app 可能已经跑完退出了，所以不能
         * 在这里回读 arkrtos_apprun_task_id）。 */
        ark_shell_printf("\r\napp run: %s started (task %u)\r\n", argv[2],
                         (unsigned)app_task);
        return 0;
    }
#endif /* ARKRTOS_USE_APPRUN */
#if (ARKRTOS_USE_FLASHAPP == 1)
    /* 阶段 3c：app flash <write <path>|run|erase|info>
     * 与 app run 的差别只有「代码从哪里取指」：run 是每次把文件读进 RAM 窗，
     * flash 是把产物烧在片内 Flash 应用区、掉电不丢。四条子命令各自一种失败
     * 后果，所以不合并成 install 的选项。 */
    if((argc >= 3) && (strcmp(argv[1], "flash") == 0))
    {
        int32 rc = 0;

        if((argc == 4) && (strcmp(argv[2], "write") == 0))
        {
            uint32 wrote = 0u;

            rc = arkrtos_flashapp_write(argv[3], &wrote);
            if(rc == 0)
            {
                ark_shell_printf("\r\napp flash write: %s, %u B at 0x%08X (verified)\r\n",
                                 argv[3], (unsigned)wrote,
                                 (unsigned)ARKRTOS_FLASHAPP_BASE);
                ark_shell_printf("  run it with 'app flash run'\r\n");
                return 0;
            }
        }
        else if((argc == 3) && (strcmp(argv[2], "run") == 0))
        {
            uint32 task = 0u;

            rc = arkrtos_flashapp_run(&task);
            if(rc == 0)
            {
                ark_shell_printf("\r\napp flash run: started (task %u, code at 0x%08X)\r\n",
                                 (unsigned)task, (unsigned)ARKRTOS_FLASHAPP_BASE);
                return 0;
            }
        }
        else if((argc == 3) && (strcmp(argv[2], "erase") == 0))
        {
            rc = arkrtos_flashapp_erase();
            if(rc == 0)
            {
                ark_shell_printf("\r\napp flash erase: 0x%08X..0x%08X cleared\r\n",
                                 (unsigned)ARKRTOS_FLASHAPP_BASE,
                                 (unsigned)ARKRTOS_FLASHAPP_END);
                return 0;
            }
        }
        else if((argc == 3) && (strcmp(argv[2], "info") == 0))
        {
            uint32 len = 0u;
            uint32 crc = 0u;
            uint32 ent = 0u;

            rc = arkrtos_flashapp_info(&len, &crc, &ent);
            if(rc == 0)
            {
                /* 显示时抹掉 Thumb 位：向量表 +4 的最低位恒 1，直接印出来会
                 * 和 'app flash run' 日志里的入口地址差一个 1（同一个入口）。 */
                ark_shell_printf("\r\nflash app: %u B, crc 0x%08X, entry 0x%08X, area 0x%08X +%uK\r\n",
                                 (unsigned)len, (unsigned)crc, (unsigned)(ent & ~1u),
                                 (unsigned)ARKRTOS_FLASHAPP_BASE,
                                 (unsigned)(ARKRTOS_FLASHAPP_SIZE / 1024u));
                ark_shell_printf("  terminal region: 0x%08X (power-cycle safe, no re-flash)\r\n",
                                 (unsigned)ARKRTOS_FLASHAPP_END);
                return 0;
            }
        }
        else
        {
            sh_out("usage: app flash [write <path> | run | erase | info]\r\n");
            return -1;
        }

        {
            const char *name = arkrtos_flashapp_error_name(rc);

            ark_shell_printf("\r\napp flash: failed, rc=%d%s%s\r\n", (int)rc,
                             (name != 0) ? " - " : "", (name != 0) ? name : "");
            if(rc == ARKRTOS_FLASHAPP_ERR_BUSY)
            {
                sh_out("  a direct-run program is alive (see 'task')\r\n");
            }
            if(rc == ARKRTOS_FLASHAPP_ERR_LAYOUT)
            {
                /* 这份镜像是给别人链的：说清要不要满足哪两条，改 scatter 即可。 */
                ark_shell_printf("  vector table: +0 must be 0x%08X (data window top, "
                                 "no separate code window), "
                                 "+4 must be inside 0x%08X..0x%08X\r\n",
                                 (unsigned)(ARKRTOS_FLASHAPP_RAM_BASE + ARKRTOS_FLASHAPP_RAM_SIZE),
                                 (unsigned)ARKRTOS_FLASHAPP_BASE,
                                 (unsigned)ARKRTOS_FLASHAPP_END);
            }
            if(rc == ARKRTOS_FLASHAPP_ERR_META)
            {
                sh_out("  nothing valid in the area: 'app flash write <path>' first\r\n");
            }
            if(rc == ARKRTOS_FLASHAPP_ERR_VERIFY)
            {
                sh_out("  read-back differs from the file: the area may now hold a partial image\r\n");
            }
            if(rc == ARKRTOS_FLASHAPP_ERR_FLASH)
            {
                sh_out("  flash erase/program failed: the area is now empty (no rollback)\r\n");
            }
            if(rc == ARKRTOS_FLASHAPP_ERR_WINDOW)
            {
                sh_out("  the data window is taken by another direct-run program (see 'free')\r\n");
            }
            if(rc == ARKRTOS_LOADER_ERR_SIZE)
            {
                ark_shell_printf("  image limit: %u B (flash app area 0x%08X)\r\n",
                                 (unsigned)ARKRTOS_FLASHAPP_MAX_IMAGE,
                                 (unsigned)ARKRTOS_FLASHAPP_BASE);
            }
            if(rc == ARKRTOS_MINI_ERR_FS)
            {
                sh_out("  cannot read the image (check 'ls' and the image limit)\r\n");
            }
            return 1;
        }
    }
#endif /* ARKRTOS_USE_FLASHAPP */


#if (ARKRTOS_USE_TASK_SWAP == 1)
    /* app swap <运行中的槽> <新槽>：A/B 双 Flash 槽的无缝升级。
     * 两个槽都必须显式给出：同时在跑的 App 可能不止一个，「谁在换」不能让命令
     * 去猜。新槽要先 `app install` 装好（LOADED）。 */
    if((argc >= 2) && (strcmp(argv[1], "swap") == 0))
    {
        uint32 old_s = 0u;
        uint32 new_s = 0u;
        int32  rc;

        if((argc < 4) || (parse_u32(argv[2], &old_s) != 0) ||
           (parse_u32(argv[3], &new_s) != 0))
        {
            sh_out("usage: app swap <running slot> <new slot>"
                   "   (install the new image first)\r\n");
            return -1;
        }

        rc = arkrtos_loader_swap(old_s, new_s);
        if(rc == 0)
        {
            ark_shell_printf("app swap: slot %u -> %u armed; it switches at the task's"
                             " next syscall (slot %u keeps its image for rollback)\r\n",
                             (unsigned)old_s, (unsigned)new_s, (unsigned)old_s);
            return 0;
        }
        ark_shell_printf("app swap: refused, rc=%d\r\n", (int)rc);
        if(rc == ARKRTOS_LOADER_ERR_STATE)
        {
            sh_out("  the old slot must be RUNNING and the new one LOADED (see 'app list')\r\n");
        }
        if(rc == ARKRTOS_SWAP_ERR_LAYOUT)
        {
            sh_out("  the two slots need the same RAM block size and the same slot type\r\n");
        }
        if(rc == ARKRTOS_LOADER_ERR_BUSY)
        {
            sh_out("  another swap is already in flight\r\n");
        }
        return 1;
    }
#endif /* ARKRTOS_USE_TASK_SWAP */


    if((argc >= 2) && (strcmp(argv[1], "start") == 0))
    {
        if((argc < 3) || (parse_u32(argv[2], &slot) != 0) ||
           (slot >= pt->slot_max) || (pt->slot_type[slot] != ARKRTOS_SLOT_APP))
        {
            sh_out("usage: app start <slot>   (slot id from 'app list')\r\n");
            return -1;
        }

        if(arkrtos_loader_start_manual(slot) > 0)
        {
            sh_out("app: started\r\n");
        }
        else
        {
            sh_out("app: start failed (see 'crash' for a held slot; state in 'app list')\r\n");
        }
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "stop") == 0))
    {
        if((argc < 3) || (parse_u32(argv[2], &slot) != 0) ||
           (slot >= pt->slot_max) || (pt->slot_type[slot] != ARKRTOS_SLOT_APP))
        {
            sh_out("usage: app stop <slot>   (slot id from 'app list')\r\n");
            return -1;
        }

        if(arkrtos_loader_stop(slot) == 0)
        {
            sh_out("app: stopped (image kept in flash)\r\n");
        }
        else
        {
            sh_out("app: stop failed (slot must be LOADED/RUNNING)\r\n");
        }
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "uninstall") == 0))
    {
        if((argc < 3) || (parse_u32(argv[2], &slot) != 0) ||
           (slot >= pt->slot_max) || (pt->slot_type[slot] != ARKRTOS_SLOT_APP))
        {
            sh_out("usage: app uninstall <slot>   (erases the image and reclaims space)\r\n");
            return -1;
        }

        {
            int32 rc;

            rc = arkrtos_loader_uninstall(slot);

            if(rc > 0)
            {
                sh_out("app: uninstalled (image invalidated, its sector was erased)\r\n");
            }
            else if(rc == 0)
            {
                sh_out("app: uninstalled (image invalidated; space returns once its sector is free)\r\n");
            }
            else
            {
                sh_out("app: uninstall failed (raw images are not managed here)\r\n");
            }
        }
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "install") == 0))
    {
        if(argc < 3)
        {
            sh_out("usage: app install <path> [slot]   (path is a .svcapp in the mounted volume)\r\n");
            return -1;
        }

        if(argc >= 4)
        {
            /* Same contract as the serial 'install [slot]': a slot only means
             * something in fixed-slot mode, where the landing address comes
             * from the device layout table instead of from the pool. */
            uint32 want;
            const arkrtos_cfg_slot_t *cs;

            if((arkrtos_ptable_get()->layout_mode != (uint32)ARKRTOS_LAYOUT_MODE_FIXED) ||
               (parse_u32(argv[3], &want) != 0) ||
               (want >= arkrtos_layout_slot_count()))
            {
                sh_out("usage: app install <path> [slot]   (a slot is only valid in fixed-slot mode)\r\n");
                return -1;
            }

            cs = arkrtos_layout_slot(want);

            if(cs == 0)
            {
                sh_out("app install: no such slot\r\n");
                return -1;
            }

            arkrtos_loader_slot_hint_set((int32)want);
            ark_shell_printf("app install: fixed slot %u -> 0x%08X\r\n",
                             (unsigned)want, (unsigned)cs->base);
        }
        else
        {
            arkrtos_loader_slot_hint_set(-1);
        }

        if(!arkrtos_fs_mounted())
        {
            /* Name the missing prerequisite: "failed, see fault" would
             * send the operator after a fault that was never recorded. */
            sh_out("app install: no file system mounted (try 'fs mount')\r\n");
            return -1;
        }

        /* No one-shot window here: the image is already on the device, so
         * the console keeps working while it is being installed. */
        ark_shell_printf("\r\napp install: %s\r\n", argv[2]);

        {
            int32 rc = arkrtos_installer_from_file(argv[2]);

            if(rc >= 0)
            {
                ark_shell_printf("app install: ok, slot %d\r\n", (int)rc);
            }
            else
            {
                sh_out("app install: failed (see 'fault'; check the path with 'fs ls')\r\n");
            }
        }

        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "list") != 0))
    {
#if (ARKRTOS_USE_TASK_SWAP == 1)
        sh_out("  app swap <running slot> <new slot>: seamless switch (A/B flash slots)\r\n");
#endif
#if (ARKRTOS_USE_APPRUN == 1)
        /* flash 子命令只在这个开关打开时才存在；usage 里出现一条敲不出来的命令
         * 比不写更糟（用户会以为是自己参数写错了）。 */
#if (ARKRTOS_USE_FLASHAPP == 1)
        sh_out("usage: app [list | install <path> [slot] | start <slot> | stop <slot> |"
               " uninstall <slot> | run <path> | flash <write <path>|run|erase|info>]\r\n");
#else
        sh_out("usage: app [list | install <path> [slot] | start <slot> | stop <slot> |"
               " uninstall <slot> | run <path>]\r\n");
#endif
#else
        sh_out("usage: app [list | install <path> [slot] | start <slot> | stop <slot> | uninstall <slot>]\r\n");
#endif
        return -1;
    }

    /* 落点是运行期分配出来的，所以列出来的是真实地址与真实占用字节数，
     * 而不是链接期的固定槽位。 */
    ark_shell_printf("\r\nid  type  state     auto  task  crash  held       base        size    ram         entry\r\n");
    for(i = 0u; (i < pt->slot_max) && (i < ARKRTOS_SLOT_ARRAY_MAX); i++)
    {
        if(pt->slot_type[i] != ARKRTOS_SLOT_APP)
        {
            continue;
        }

        ark_shell_printf(" %u   %-4s  %-8s  %-4s  %-4u  %-5u  %-9s  0x%08X  %-6u  0x%08X  0x%08X\r\n",
                         i,
                         slot_type_name(pt->slot_type[i]),
                         slot_state_name(pt->slot_state[i]),
                         (pt->slot_autostart[i] != 0u) ? "yes" : "no",
                         pt->slot_task_id[i],
                         pt->slot_crash_cnt[i],
                         arkrtos_crash_reason_name(arkrtos_crash_disabled(i)),
                         pt->slot_base[i],
                         pt->slot_size[i],
                         pt->slot_ram_base[i],
                         pt->slot_entry[i]);
    }
    return 0;
}

/* ============================================================
 * drv：驱动镜像列表 / 启动 / 停止
 * ============================================================ */
int cmd_drv(int argc, char *argv[])
{
    arkrtos_partition_table_t *pt = arkrtos_ptable_get();
    uint32 i;
    uint32 slot;

    if((argc >= 2) && (strcmp(argv[1], "start") == 0))
    {
        if((argc < 3) || (parse_u32(argv[2], &slot) != 0) ||
           (slot >= pt->slot_max) || (pt->slot_type[slot] != ARKRTOS_SLOT_DRIVER))
        {
            sh_out("usage: drv start <slot>   (slot id from 'drv list')\r\n");
            return -1;
        }

        if(arkrtos_loader_start_driver_manual(slot) > 0)
        {
            sh_out("drv: started\r\n");
        }
        else
        {
            sh_out("drv: start failed (see 'crash' for a held slot; state in 'drv list')\r\n");
        }
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "stop") == 0))
    {
        if((argc < 3) || (parse_u32(argv[2], &slot) != 0) ||
           (slot >= pt->slot_max) || (pt->slot_type[slot] != ARKRTOS_SLOT_DRIVER))
        {
            sh_out("usage: drv stop <slot>   (slot id from 'drv list')\r\n");
            return -1;
        }

        if(arkrtos_loader_stop_driver_slot(slot) == 0)
        {
            sh_out("drv: stopped (image kept in flash)\r\n");
        }
        else
        {
            sh_out("drv: stop failed (slot must be LOADED/RUNNING)\r\n");
        }
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "uninstall") == 0))
    {
        if((argc < 3) || (parse_u32(argv[2], &slot) != 0) ||
           (slot >= pt->slot_max) || (pt->slot_type[slot] != ARKRTOS_SLOT_DRIVER))
        {
            sh_out("usage: drv uninstall <slot>   (erases the image and reclaims space)\r\n");
            return -1;
        }

        {
            int32 rc;

            rc = arkrtos_loader_uninstall(slot);

            if(rc > 0)
            {
                sh_out("drv: uninstalled (image invalidated, its sector was erased)\r\n");
            }
            else if(rc == 0)
            {
                sh_out("drv: uninstalled (image invalidated; space returns once its sector is free)\r\n");
            }
            else
            {
                sh_out("drv: uninstall failed (raw images are not managed here)\r\n");
            }
        }
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "list") != 0))
    {
        sh_out("usage: drv [list | start <slot> | stop <slot> | uninstall <slot>]\r\n");
        return -1;
    }

    ark_shell_printf("\r\nid  type  state     auto  task  crash  held       base        size    ram         entry\r\n");
    for(i = 0u; (i < pt->slot_max) && (i < ARKRTOS_SLOT_ARRAY_MAX); i++)
    {
        if(pt->slot_type[i] != ARKRTOS_SLOT_DRIVER)
        {
            continue;
        }

        ark_shell_printf(" %u   %-4s  %-8s  %-4s  %-4u  %-5u  %-9s  0x%08X  %-6u  0x%08X  0x%08X\r\n",
                         i,
                         slot_type_name(pt->slot_type[i]),
                         slot_state_name(pt->slot_state[i]),
                         (pt->slot_autostart[i] != 0u) ? "yes" : "no",
                         pt->slot_task_id[i],
                         pt->slot_crash_cnt[i],
                         arkrtos_crash_reason_name(arkrtos_crash_disabled(i)),
                         pt->slot_base[i],
                         pt->slot_size[i],
                         pt->slot_ram_base[i],
                         pt->slot_entry[i]);
    }
    return 0;
}

/* ============================================================
 * install：打开一次性安装窗口
 *
 * 窗口期间本任务不跑 ark_shell_run()，串口读权全部交给安装器；
 * 因此不存在「两个读者把镜像字节流随机分掉」的问题。
 * ============================================================ */
int cmd_install(int argc, char *argv[])
{
    int32 slot;
    int32 dev;

    if(argc >= 2)
    {
        uint32 want;
        const arkrtos_cfg_slot_t *cs;

        if((arkrtos_ptable_get()->layout_mode != (uint32)ARKRTOS_LAYOUT_MODE_FIXED) ||
           (parse_u32(argv[1], &want) != 0) ||
           (want >= arkrtos_layout_slot_count()))
        {
            sh_out("usage: install [slot]   (a slot number is only valid in fixed-slot mode)\r\n");
            return -1;
        }

        cs = arkrtos_layout_slot(want);

        if(cs == 0)
        {
            sh_out("install: no such slot\r\n");
            return -1;
        }

        /* Overwrite install: the fixed slot may still hold the previous
         * image, so clear it before receiving the frame. It has to happen
         * here, not in the middle of the frame: an erase stalls the CPU for
         * about a second, the RX interrupt cannot run, and the relocation
         * table the host already sent is lost to a hardware overrun (the
         * symptom is err -4 with the table never written). So: clear first,
         * while the host is still waiting for its banner, then say "send".
         * The loader's own blank check then skips its erase. */
        {
            /* 清槽就是擦掉这个槽的 Flash。若还有镜像正从这里取指运行，擦完
             * 它取到的是 0xFF（未定义指令）——板上实测：跟着三条 USGFAULT，
             * 撞到崩溃上限后整个槽被禁用，而操作者只看到 install 成功，
             * 上一个应用“无缘无故”被停。无头安装器（arkrtos_installer.c 的
             * arkrtos_installer_fixed_prepare）早就会先停后擦，这里当初漏了。*/
            arkrtos_partition_table_t *pt = arkrtos_ptable_get();
            uint32 j;

            for(j = 0u; (j < pt->slot_max) && (j < ARKRTOS_SLOT_ARRAY_MAX); j++)
            {
                if(pt->slot_type[j] == ARKRTOS_SLOT_FREE)
                {
                    continue;
                }
                if(!((cs->base < (pt->slot_base[j] + pt->slot_size[j])) &&
                     (pt->slot_base[j] < (cs->base + cs->size))))
                {
                    continue;       /* 与要清的槽没有交集 */
                }
                if(pt->slot_state[j] != ARKRTOS_APP_SLOT_RUNNING)
                {
                    continue;       /* 没有东西在从这个槽执行 */
                }

                ark_shell_printf("install: slot %u still running (id %u), stopping it before the erase\r\n",
                                 (unsigned)want, (unsigned)j);

                if(arkrtos_loader_stop((uint32)j) != 0)
                {
                    sh_out("install: cannot stop the running image, refusing to erase its flash\r\n");
                    return -1;
                }
            }
        }

        if(arkrtos_loader_clear_fixed_slot((int32)want) != 0)
        {
            sh_out("install: cannot clear the slot (flash erase failed)\r\n");
            return -1;
        }

        arkrtos_loader_slot_hint_set((int32)want);
        ark_shell_printf("install: fixed slot %u -> 0x%08X\r\n",
                         (unsigned)want, (unsigned)cs->base);
    }
    else if(arkrtos_ptable_get()->layout_mode == (uint32)ARKRTOS_LAYOUT_MODE_FIXED)
    {
        /* In fixed-slot mode the landing place is the operator's decision. A
         * bare command would leave the kernel guessing which slot to clear (it
         * only learns the image type from the frame), so refuse instead of
         * picking one. */
        sh_out("usage: install <slot>   (fixed-slot mode needs an explicit slot)\r\n");
        return -1;
    }
    else
    {
        arkrtos_loader_slot_hint_set(-1);
    }

    sh_out("\r\ninstall: waiting for one .svcapp image on " SHELL_DEV_NAME "\r\n");
    sh_out("install: send the file now; console input is ignored while waiting\r\n");

    dev = arkrtos_shell_uart_open();

    slot = arkrtos_installer_run_once(dev, (uint32)SHELL_INSTALL_TIMEOUT_MS);

    /* Drop whatever arrived after the frame was already decided. A host that
     * sends header + relocation table as one burst still has bytes on the
     * wire when the header alone is rejected (compat id, size...); anything
     * the installer did not consume would be replayed to the shell as a
     * command line - seen in practice as "Command not found: )8PX`hpdrv".
     * The window is over here, so nothing legitimate is in flight. */
    {
        uint8 sink[32];
        uint32 spins = 0u;

        while((spins < 64u) &&
              (arkrtos_dev_read_internal(dev, sink, (int32)sizeof(sink)) > 0))
        {
            spins++;
        }
    }

    if(slot >= 0)
    {
        ark_shell_printf("install: ok, slot %d\r\n", (int)slot);
    }
    else
    {
        sh_out("install: failed or timed out (see 'fault')\r\n");
    }
    return 0;
}

/* ============================================================
 * rollback：回滚白名单的武装 / 查看 / 清空（SEC-G11）
 *
 * 这里只做搬运：锚点（image_size + crc32）由模块自己从池里那份已提交副本读，
 * 所以操作员抄的只有 id 与 version —— 名字不构成凭据，字节才构成凭据。
 * ============================================================ */
#if (ARKRTOS_USE_ROLLBACK == 1)
int cmd_rollback(int argc, char *argv[])
{
    arkrtos_partition_table_t *pt = arkrtos_ptable_get();
    arkrtos_rollback_entry_t rb[ARKRTOS_ROLLBACK_ENTRIES];
    uint32 n;
    uint32 idx;

    if((argc >= 2) && (strcmp(argv[1], "clear") == 0))
    {
        (void)arkrtos_rollback_clear();
        sh_out("rollback: whitelist cleared (arming again requires a fresh command)\r\n");
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "arm") == 0))
    {
        uint32 image_id;
        uint32 version;
        uint32 uses = 0u;           /* 0 = ARKRTOS_ROLLBACK_USES */
        int32 rc;

        if((argc < 4) ||
           (parse_u32(argv[2], &image_id) != 0) ||
           (parse_u32(argv[3], &version) != 0) ||
           ((argc >= 5) && (parse_u32(argv[4], &uses) != 0)))
        {
            sh_out("usage: rollback arm <image_id> <version> [uses]\r\n");
            sh_out("       id and version come from the installed-copy table above\r\n");
            return -1;
        }

        rc = arkrtos_rollback_arm(image_id, version, uses);

        if(rc == ARKRTOS_ROLLBACK_OK)
        {
            ark_shell_printf("rollback: armed id %u version %u (%u admission(s))\r\n",
                             (unsigned)image_id, (unsigned)version,
                             (unsigned)((uses != 0u) ? uses : (uint32)ARKRTOS_ROLLBACK_USES));
            return 0;
        }
        if(rc == ARKRTOS_ROLLBACK_ERR_NOTFOUND)
        {
            sh_out("rollback: no LOADED/RUNNING copy of that id+version; see the table above\r\n");
        }
        else if(rc == ARKRTOS_ROLLBACK_ERR_FULL)
        {
            sh_out("rollback: whitelist is full; 'rollback clear' first\r\n");
        }
        else
        {
            sh_out("rollback: bad arguments (id and version must be non-zero)\r\n");
        }
        return -1;
    }

    /* 默认 / list：先列可作锚点的副本，再列已武装的条目。 */
    ark_shell_printf("\r\ninstalled copies (records only, not written)\r\n");
    ark_shell_printf("slot  type    state     version  image_id    size    crc32\r\n");
    for(idx = 0u; (idx < pt->slot_max) && (idx < ARKRTOS_SLOT_ARRAY_MAX); idx++)
    {
        const arkrtos_app_header_t *p_hdr;
        uint32 base = pt->slot_base[idx];

        if(base == 0u)
        {
            continue;
        }
        if((pt->slot_state[idx] != ARKRTOS_APP_SLOT_LOADED) &&
           (pt->slot_state[idx] != ARKRTOS_APP_SLOT_RUNNING))
        {
            continue;
        }
        if(*(const volatile uint32 *)base != (uint32)ARKRTOS_APP_MAGIC)
        {
            continue;
        }

        p_hdr = (const arkrtos_app_header_t *)base;
        ark_shell_printf(" %-4u  %-4s  %-8s  %-7u  0x%08X  %-6u  0x%08X\r\n",
                         idx,
                         slot_type_name(pt->slot_type[idx]),
                         slot_state_name(pt->slot_state[idx]),
                         p_hdr->version,
                         p_hdr->image_id,
                         p_hdr->image_size,
                         p_hdr->crc32);
    }

    n = arkrtos_rollback_list(rb, (uint32)ARKRTOS_ROLLBACK_ENTRIES);

    ark_shell_printf("\r\narmed rollback targets (%u of %u), admitted since reset: %u\r\n",
                     (unsigned)n, (unsigned)ARKRTOS_ROLLBACK_ENTRIES,
                     (unsigned)arkrtos_rollback_permits());
    if(n == 0u)
    {
        sh_out("  (none: the install path still refuses every downgrade)\r\n");
    }
    for(idx = 0u; idx < n; idx++)
    {
        ark_shell_printf("  id 0x%08X  version %-7u  size %-6u  crc 0x%08X  budget %u  used %u\r\n",
                         rb[idx].image_id, rb[idx].version, rb[idx].image_size,
                         rb[idx].crc32, rb[idx].budget, rb[idx].used);
    }
    return 0;
}
#endif /* ARKRTOS_USE_ROLLBACK */

#endif /* SHELL_ENABLE */
