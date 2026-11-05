# 本文件由 tools/gen_gcc_sources.py 的同构流程派生；H743 工程没有 Keil 工程可读，
# 因此以 example/stm32f427 的清单为基线，去掉 lwip / ark_softbus / mdk_trace，
# 换掉板级目录与 port，并只保留 H743 实际用到的 HAL 模块。
# 与 Keil 工程同一文件集这条约束在 H7 上暂不适用（H7 只走 GCC）。

SRCS := \
  example/stm32h743/kernel/ARKRTOS_TEST/Drivers/CMSIS/Device/ST/STM32H7xx/Source/Templates/gcc/startup_stm32h743xx.s \
  example/stm32h743/kernel/ARKRTOS_TEST/Core/Src/system_stm32h7xx.c \
  example/stm32h743/kernel/ARKRTOS_TEST/Core/Src/main.c \
  example/stm32h743/kernel/ARKRTOS_TEST/Core/Src/gpio.c \
  example/stm32h743/kernel/ARKRTOS_TEST/Core/Src/stm32h7xx_it.c \
  example/stm32h743/kernel/ARKRTOS_TEST/Core/Src/stm32h7xx_hal_msp.c \
  kernelsrc/src/arkrtos_vfs.c \
  kernelsrc/src/arkrtos_vfs_lfs.c \
  kernelsrc/components/ark_vfs/src/ark_vfs_core.c \
  kernelsrc/components/ark_vfs/src/ark_vfs_ramfs.c \
  kernelsrc/components/ark_vfs/port/ark_vfs_port_arkrtos.c \
  kernelsrc/components/ark_vfs/port/ark_vfs_devfs.c \
  kernelsrc/components/ark_vfs/port/ark_vfs_procfs.c \
  kernelsrc/src/arkrtos_task.c \
  kernelsrc/src/arkrtos_blk.c \
  kernelsrc/src/arkrtos_fs.c \
  kernelsrc/src/arkrtos_audit.c \
  kernelsrc/src/arkrtos_guard.c \
  kernelsrc/src/arkrtos_crash.c \
  kernelsrc/src/arkrtos_sign.c \
  kernelsrc/fs/littlefs/lfs.c \
  kernelsrc/fs/littlefs/lfs_util.c \
  kernelsrc/src/arkrtos_trace.c \
  kernelsrc/components/mdk_trace/mdk_trace.c \
  kernelsrc/components/mdk_trace/mdk_trace_swd.c \
  kernelsrc/components/mdk_trace/mdk_trace_arkrtos.c \
  kernelsrc/src/arkrtos_dev.c \
  kernelsrc/src/arkrtos_event.c \
  kernelsrc/src/arkrtos_fifo.c \
  kernelsrc/src/arkrtos_cfg.c \
  kernelsrc/src/arkrtos_sync.c \
  kernelsrc/src/arkrtos_mq.c \
  kernelsrc/src/arkrtos_mempool.c \
  kernelsrc/src/arkrtos_net.c \
  kernelsrc/src/arkrtos_timer.c \
  kernelsrc/src/arkrtos_fault.c \
  kernelsrc/src/arkrtos_hwsafe.c \
  kernelsrc/src/arkrtos_init.c \
  kernelsrc/src/arkrtos_mpu.c \
  kernelsrc/src/arkrtos_ptable.c \
  kernelsrc/src/arkrtos_layout.c \
  kernelsrc/src/arkrtos_loader.c \
  kernelsrc/src/arkrtos_mini.c \
  kernelsrc/src/arkrtos_apprun.c \
  kernelsrc/src/arkrtos_installer.c \
  kernelsrc/src/arkrtos_log.c \
  kernelsrc/src/arkrtos_utest.c \
  kernelsrc/src/arkrtos_utest_cases.c \
  kernelsrc/shell/platform_arkrtos.c \
  kernelsrc/shell/arkrtos_shell.c \
  kernelsrc/shell/ark_shell/ark_shell.c \
  kernelsrc/shell/ark_shell/ark_shell_commands.c \
  kernelsrc/shell/ark_shell/ark_shell_eval.c \
  kernelsrc/shell/ark_shell/ark_shell_history.c \
  kernelsrc/shell/ark_shell/ark_shell_line_editor.c \
  kernelsrc/shell/ark_shell/ark_shell_autocomplete.c \
  kernelsrc/port/arm/cortex-m7/arkrtos_context_gcc.s \
  kernelsrc/port/arm/cortex-m7/arkrtos_port.c \
  board/stm32h743/arkrtos_board.c \
  board/stm32h743/arkrtos_dt.c \
  board/stm32h743/drvuart.c \
  board/stm32h743/drvled.c \
  board/stm32h743/drvflash.c \
  board/stm32h743/drvwdg.c \
  board/stm32h743/drvblk.c \
  board/stm32h743/drvfmc.c \
  example/stm32h743/kernel/ARKRTOS_TEST/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal.c \
  example/stm32h743/kernel/ARKRTOS_TEST/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_cortex.c \
  example/stm32h743/kernel/ARKRTOS_TEST/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc.c \
  example/stm32h743/kernel/ARKRTOS_TEST/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc_ex.c \
  example/stm32h743/kernel/ARKRTOS_TEST/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_gpio.c \
  example/stm32h743/kernel/ARKRTOS_TEST/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr.c \
  example/stm32h743/kernel/ARKRTOS_TEST/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr_ex.c \
  example/stm32h743/kernel/ARKRTOS_TEST/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dma.c \
  example/stm32h743/kernel/ARKRTOS_TEST/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dma_ex.c \
  example/stm32h743/kernel/ARKRTOS_TEST/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash.c \
  example/stm32h743/kernel/ARKRTOS_TEST/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash_ex.c \
  example/stm32h743/kernel/ARKRTOS_TEST/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_uart.c \
  example/stm32h743/kernel/ARKRTOS_TEST/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_exti.c \

# Text editor component.  Upstream arkvim (gitee xw19981010/arkvim) is copied
# into kernelsrc/components/arkvim/src unchanged; this repository only adds the
# three wiring files: arkvim_port_arkrtos.c (transport), arkvim_fileio_vfs.c
# (file backend), arkvim_shell_cmd.c (the "vim" command).  With USE_ARKVIM=0
# none of it is compiled, so "off means no change" is enforced by the build,
# not by #if in the sources.  arkvim_fileio_posix.c is the PC backend and is
# deliberately not in this list.
ifeq ($(USE_ARKVIM),1)
SRCS += \
  kernelsrc/components/arkvim/src/arkvim.c \
  kernelsrc/components/arkvim/src/arkvim_fileio.c \
  kernelsrc/components/arkvim/src/arkvim_gap_buffer.c \
  kernelsrc/components/arkvim/src/arkvim_line_table.c \
  kernelsrc/components/arkvim/src/arkvim_modes.c \
  kernelsrc/components/arkvim/src/arkvim_port.c \
  kernelsrc/components/arkvim/src/arkvim_renderer.c \
  kernelsrc/components/arkvim/src/arkvim_syntax.c \
  kernelsrc/components/arkvim/src/arkvim_terminal.c \
  kernelsrc/components/arkvim/src/arkvim_undo.c \
  kernelsrc/components/arkvim/arkvim_port_arkrtos.c \
  kernelsrc/components/arkvim/arkvim_fileio_vfs.c \
  kernelsrc/components/arkvim/arkvim_shell_cmd.c
endif

# Ark C compiler component.  Upstream arkc is copied into
# kernelsrc/components/arkc/{src,include} by tools/sync_arkc.py; this
# repository only adds arkc_port_arkrtos.c (platform + file hooks),
# arkc_port.h (the contract the command layer uses) and arkc_shell_cmd.c
# (the "cc" command).  Not from upstream: src/main.c (host driver, has its
# own main()), port/ (port_stm32 provides malloc/putchar/
# __user_initial_stackheap, which collide with newlib) and the c / x86_64 /
# riscv32 backends.  With USE_ARKC=0 none of this is compiled at all.
#
# These objects are built with CFLAGS_ARKC (see Makefile), not CFLAGS:
# arkc ships include/platform.h, and the shell ships shell/platform.h.
ifeq ($(USE_ARKC),1)
SRCS += \
  kernelsrc/components/arkc/src/ast.c \
  kernelsrc/components/arkc/src/backend/arm/backend_arm.c \
  kernelsrc/components/arkc/src/backend/registry.c \
  kernelsrc/components/arkc/src/codegen_common.c \
  kernelsrc/components/arkc/src/compiler.c \
  kernelsrc/components/arkc/src/config.c \
  kernelsrc/components/arkc/src/cpu.c \
  kernelsrc/components/arkc/src/diag.c \
  kernelsrc/components/arkc/src/diag_msg_table.c \
  kernelsrc/components/arkc/src/ir.c \
  kernelsrc/components/arkc/src/irgen.c \
  kernelsrc/components/arkc/src/lexer.c \
  kernelsrc/components/arkc/src/memory.c \
  kernelsrc/components/arkc/src/opt.c \
  kernelsrc/components/arkc/src/parser.c \
  kernelsrc/components/arkc/src/preproc.c \
  kernelsrc/components/arkc/src/scatter.c \
  kernelsrc/components/arkc/src/sema.c \
  kernelsrc/components/arkc/src/symtab.c \
  kernelsrc/components/arkc/src/type.c \
  kernelsrc/components/arkc/arkc_port_arkrtos.c \
  kernelsrc/components/arkc/arkc_shell_cmd.c
endif
