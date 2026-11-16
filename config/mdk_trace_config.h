/**
* @brief mdk_trace instrumentation config - STM32F427 / Ark RTOS
* @details Picked up because the project defines MDK_TRACE_USE_CONFIG_FILE;
*          AC5 (armcc V5.06) does not answer __has_include, so without that
*          define this file would be silently skipped and the component would
*          fall back to its ITM/SWO default - it would still compile, but
*          nothing would ever leave the chip.
*
*          This board is wired to a DAPLink over SWD only (no SWO pin), so the
*          backend is swd: events are compressed on target into a RAM ring and
*          the host drains it incrementally with mdkdebug trace_swd_read. The
*          unread part of the ring is never overwritten, so a recording that
*          keeps up loses nothing.
*
*          Time granularity is a run time decision, not a build one: the host
*          writes TS_SHIFT / DT_UNIT / FLAGS.TS_OFF into the control block and
*          asks for a fresh recording. The clock below is only used to convert
*          the target side cycle count into microseconds.
*
*          Everything else keeps the component defaults
*          (kernelsrc/components/mdk_trace/mdk_trace_config_default.h):
*            MDK_TRACE_SWD_BYTES         8192   ring bytes = static RAM cost
*            MDK_TRACE_SWD_TS_SHIFT      0      full DWT resolution
*            MDK_TRACE_SWD_CLEAR_ON_INIT 1      clear the ring on power up
*/

#ifndef MDK_TRACE_CONFIG_H
#define MDK_TRACE_CONFIG_H

/* Master switch. 0 compiles the whole component down to empty functions. */
#define MDK_TRACE_ENABLE              1

/* Backend.
 *   ARM (Cortex-M, SWD 调试器在位) —— 压缩字节环，主机按片抽取，录音不掉。
 *   其它内核（本仓库的 QEMU RISC-V / LoongArch 目标）—— 没有 SWD 可抽，
 *   改用定长记录环 BUFF：环在 .bss 里，主机按符号 mdk_trace_buff_blob
 *   一次性读整窗；板子上自己只能答「记了多少 / 丢了多少 / 转过圈没有」。
 * 两边的内核门面（kernelsrc/src/arkrtos_trace.c）都读后端的计数器，
 * 所以换后端不改任何调用点。 */
#if defined(__arm__) || defined(__ARM_ARCH) || defined(__ARMCC_VERSION) || \
    defined(__ICCARM__) || defined(__TI_ARM__)
#define MDK_TRACE_BACKEND_SWD         1
#else
#define MDK_TRACE_BACKEND_BUFF        1
#endif

/* RISC-V 上的时间戳来源。
 *   0 = mcycle（MDK_TRACE_USE_DWT 决定是否开），数的是核被时钟驱动的周期数；
 *   1 = rdtime（CSR 0xC01），读 mtime，与内核节拍同源。
 * 模拟器（QEMU）里 mcycle 与真实时间无关，所以 RV32 目标用 rdtime；
 * 真核上两者都可，按板子时钟树选，并把 MDK_TRACE_CPU_HZ 填成这条时钟的频率。 */
#ifndef MDK_TRACE_USE_RDTIME
#define MDK_TRACE_USE_RDTIME          0
#endif

/* Kernel automation hooks: the kernel reports its own switches, task
 * lifecycle and object waits/wakes (see mdk_trace_arkrtos.h). With this at 0
 * the firmware still links and still traces, but the timeline carries only
 * what application code records by hand - no kernel events at all. */
#define MDK_TRACE_ARKRTOS_HOOKS         1

/* Core clock, derived from SystemClock_Config() in
 * example/stm32f427/kernel/ARKRTOS_TEST/Core/Src/main.c:
 *   HSI 16 MHz / PLLM 16 = 1 MHz, x PLLN 192 = 192 MHz VCO, / PLLP 2 = 96 MHz.
 * FLASH_LATENCY_3 in the same function is the wait state count for a
 * 90..100 MHz SYSCLK on voltage scale 1, which agrees with 96 MHz.
 *
 * Note this is NOT board/stm32f427/arkrtos_board_config.h's
 * ARKRTOS_SYSTEM_CLOCK_HZ (168 MHz). That macro describes the part's nominal
 * maximum rather than this project's HSI-fed PLL, and nothing in the kernel
 * reads it today - but the difference matters here, because this value ends
 * up in the host's microsecond conversion. Do not copy the 168.
 */
#ifndef MDK_TRACE_CPU_HZ
#define MDK_TRACE_CPU_HZ              96000000
#endif

/* Fault snapshot: extra pc/lr/sp/xpsr/hfsr/bfar in the fault path (ARM 上由
 * 板级的四入口 thunk 提供 EXC_RETURN/MSP/PSP；其它内核这条帧捕获不参与
 * 编译，见 mdk_trace.c 的 MDK_TRACE_FAULT_FRAME 门）。
 * The F427 board now wraps its four fault vectors (board/stm32f427/
 * arkrtos_board.c) with an AC5 assembly thunk that passes EXC_RETURN / MSP /
 * PSP to mdk_trace_fault_capture() before the C body runs, so there is a real
 * producer behind this switch - same wiring F401 already had. */
#define MDK_TRACE_FAULT_FRAME         1

#endif /* MDK_TRACE_CONFIG_H */
