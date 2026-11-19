/**
* @file arkrtos_dt_blob.c
* @brief 把编进镜像的标准设备树 blob 交给启动代码
* @details MMU 档的形态：镜像里放一块 .dtb（tools/dts/dtc_lite.py 的第二个
*          出口），启动时解析。这块数据由构建系统生成：
*
*            dts/qemu_loongarch64.dts
*              --(dtc_lite.py --format dtb)--> build/.../arkrtos.dtb
*              --(elf_blob.py)--> build/.../arkrtos_dt_blob.h
*
*          数组只是"把文件送进镜像"的手段，不是解析器的输入格式——解析器
*          读的是标准 DTB 字节流，与 Linux 上那份完全同构。
*
*          开关关掉时本文件不产生任何符号（连数组都不编进来）。
*
* @note 本模块属于内核特权态代码，只读。
* @author xw
*/

#include "arkrtos_config.h"

#if (ARKRTOS_USE_FDT == 1)

#include "arkrtos_board.h"
#include "arkrtos_dt_blob.h"

const void *arkrtos_board_dtb(uint32 *size)
{
    if(size != 0)
    {
        *size = (uint32)ARK_QEMU_DTB_LEN;
    }
    return (const void *)ark_qemu_dtb;
}

#endif /* ARKRTOS_USE_FDT */
