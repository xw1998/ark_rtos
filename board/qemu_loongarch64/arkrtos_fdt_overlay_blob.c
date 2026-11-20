/**
* @file arkrtos_fdt_overlay_blob.c
* @brief 把编进镜像的 overlay demo fragment 交给 shell
* @details overlay 的 fragment 在被合并之前也是一块普通 dtb，所以它的
*          "进镜像"方式与整棵树完全一样：
*
*            tools/dts/overlay/demo.dts
*              --(dtc_lite.py --format dtb)--> build/.../arkrtos_fdt_overlay_demo.dtb
*              --(elf_blob.py)--> build/.../arkrtos_fdt_overlay_demo_blob.h
*
*          它只服务于 `fdt overlay` 这条命令（演示 + QEMU 读数），不参与
*          启动流程；镜像里有没有它，内核启动行为完全一样。
*
*          开关关掉时本文件不产生任何符号。
*
* @note 本模块属于内核特权态代码，只读。
* @author xw
*/

#include "arkrtos_config.h"

#if (ARKRTOS_USE_FDT_OVERLAY == 1)

#include "arkrtos_board.h"
#include "arkrtos_fdt_overlay_demo_blob.h"

const void *arkrtos_board_fdt_overlay_demo(uint32 *size)
{
    if(size != 0)
    {
        *size = (uint32)ARK_OVERLAY_DEMO_DTB_LEN;
    }
    return (const void *)ark_overlay_demo_dtb;
}

#endif /* ARKRTOS_USE_FDT_OVERLAY */
