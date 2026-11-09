/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @details        : Ark RTOS 内核入口（STM32H743XI）。
  *                   由 CubeMX 骨架改写：时钟/GPIO 初始化保留，MPU 交给
  *                   内核（arkrtos_mpu.c），调度与中断向量交给板级层。
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* 注意：本文件的板级 LED 任务是内核侧任务，直接调用 _internal 接口，
 * 不走 SVC 用户态封装（arkrtos_dev_open 等）。用户态封装只属于 App/驱动 SDK。 */
/* USER CODE BEGIN Includes */
#include "arkrtos.h"
#include "arkrtos_hal.h"
#include "arkrtos_cfg.h"
#include "arkrtos_event.h"
#include "arkrtos_dev.h"
#include "arkrtos_sync.h"
#include "arkrtos_task.h"
#include "arkrtos_mpu.h"
#include "arkrtos_config.h"
#include "arkrtos_ptable.h"
#include "arkrtos_loader.h"
#include "arkrtos_installer.h"
#include "arkrtos_shell.h"
#include "arkrtos_log.h"
#include "arkrtos_vfs.h"
#include "arkrtos_mq.h"
#include "arkrtos_timer.h"
#include "arkrtos_fault.h"
#include "arkrtos_init.h"
#include "arkrtos_partition.h"
#include "arkrtos_layout.h"
#include "arkrtos_audit.h"
#include "arkrtos_guard.h"
#include "arkrtos_crash.h"
#include "drvfmc.h"          /* board SDRAM state: the VFS file buffers live there */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
#define ARKRTOS_IDLE_STACK_WORDS    512
#define ARKRTOS_LED_STACK_WORDS     1024

/* 注意：外部分区固件（驱动 / App）不再在这里静态注册任务。
 * 它们的入口地址与栈由内核加载器按 config/stm32h743/arkrtos_partition.h 推导。 */

static uint32 arkrtos_idle_stack[ARKRTOS_IDLE_STACK_WORDS];
static uint32 led_task_stack[ARKRTOS_LED_STACK_WORDS];

/* 灯设备写入的互斥锁（单任务版本也保留：后面加任务时不必改结构） */
static int32 g_led_mutex = -1;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static void arkrtos_register_tasks(void);
static void arkrtos_kernel_init(void);
static void arkrtos_start_idle(void);
static void led_blink_task(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* 注意：CubeMX 生成的 MPU_Config() 不在这里调用。
   * H743 的 MPU 由内核 arkrtos_mpu.c 独占（任务窗口 + PRIVDEFENA），
   * 两处都配会互相覆盖，且 CubeMX 那份带 SubRegionDisable=0x87 的
   * 4GB 全禁区域会让内核的任务窗口之外全部不可访问。 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock: HSI 64MHz -> PLL1 M4/N60/P2 = 480MHz */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  /* USER CODE BEGIN 2 */

  arkrtos_port_board_init();

  arkrtos_cfg_load();

  arkrtos_register_tasks();

  arkrtos_port_irq_init();

  arkrtos_kernel_init();

  arkrtos_start_idle();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  * @note  HSI 64MHz 由 PLL1 分出 480MHz 的 SYSCLK：M=4 -> 16MHz VCO 输入，
  *        N=60 -> 960MHz VCO，P=2 -> 480MHz。VOS0 是 480MHz 的前提，
  *        FLASH_LATENCY_4 对应 240MHz 的 HCLK（AHB /2）。
  *        参数来自同一块板子上已验证过的 CubeMX 工程，不是估算。
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 60;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

static void led_blink_task(void)
{
    int32 led = arkrtos_dev_open_internal("led0", 0);
    uint8 on  = 1;

    while(1)
    {
        /* 加锁后写设备：len>0 点亮、len==0 熄灭（约定见板级 drvled） */
        arkrtos_mtx_lock_internal(g_led_mutex, -1);   /* -1 = 永久等待 */
        arkrtos_dev_write_internal(led, &on, 1);      /* 点亮 */
        arkrtos_mtx_unlock_internal(g_led_mutex);
        arkrtos_task_wait_internal(1000);

        arkrtos_mtx_lock_internal(g_led_mutex, -1);   /* -1 = 永久等待 */
        arkrtos_dev_write_internal(led, &on, 0);      /* 熄灭 */
        arkrtos_mtx_unlock_internal(g_led_mutex);
        arkrtos_task_wait_internal(1000);
    }
}

static void arkrtos_register_tasks(void)
{
    arkrtos_task_t *p_task;

    led_task_stack[0] = ARKRTOS_STACK_END_FLAG_VAL;

    p_task = &arkrtos_task_table[arkrtos_task_count];
    p_task->ram_start   = (uint32)led_task_stack;
    p_task->ram_size    = sizeof(led_task_stack);
    p_task->stack_size  = sizeof(led_task_stack);
    p_task->rom_start   = 0;
    p_task->rom_size    = 0;
    p_task->period      = ARKRTOS_MS_TO_TICK(1000);
    p_task->priority    = 10;
    p_task->base_priority = 10;         /* 继承撤销的恢复依据：缺了会被提到优先级 0 */
    p_task->shm_attri   = 0;
    p_task->status      = ARKRTOS_TASK_READY;
    p_task->period_time = p_task->period;
    p_task->wait_time   = 0;
    p_task->tim_tick    = 0;
    p_task->touch_tick  = 0;

    /* A hand-built TCB skips arkrtos_task_register(), so it also skips the two
     * things that function finishes for us: the MPU windows (kernel task
     * defaults - kernel flash plus the whole chip RAM) and "this task is
     * kernel code, therefore privileged". Leaving both at their .bss values
     * produces an unprivileged task with no MPU region at all, and with
     * PRIVDEFENA that means its very first stack access faults. */
    p_task->is_priv = 1u;
    arkrtos_mpu_build_task(p_task);

    arkrtos_task_stack_init(p_task, led_blink_task,
                          led_task_stack, sizeof(led_task_stack));

    arkrtos_task_count++;
    arkrtos_ready_add(arkrtos_task_count - 1);   /* 手工登记 TCB 也必须挂就绪链 */
}

static void arkrtos_kernel_init(void)
{
    /* 顺序与内核默认入口 kernelsrc/src/arkrtos_init.c 一致：
     * 先初始化各内核模块，再认定/启动外部映像，最后注册安装任务。
     * （缺模块初始化或顺序颠倒会导致内核无法正常启动） */
    arkrtos_kernel_module_init();

    /* Log first: everything after this point can leave a trace on the
     * console, which is the only cheap way to see where a boot stops. */
    arkrtos_log_init();
    ARKRTOS_LOGI("BOOT", "Ark RTOS kernel starting on STM32H743, build %s %s",
               __DATE__, __TIME__);

    /* 内核内置 LED 任务用的互斥量 */
    g_led_mutex = arkrtos_mtx_create_internal("ledmtx");

    /* ---- 分区表与外部映像 ---- */
    arkrtos_ptable_init();

    /* Cross-reset crash bookkeeping. init() only validates or starts the
     * journal; it must run before anything can fault. */
    arkrtos_crash_init();

    /* Device-side layout config (install mode + fixed slot table) must be
     * resolved before the pool is scanned. */
    arkrtos_layout_init();

    /* Debug-attach window: the device-side configuration may ask for a
     * boot delay. Split into short chunks so a long delay does not ride on
     * one enormous busy loop. */
    {
        uint32 boot_delay = arkrtos_layout_boot_delay_ms();

        if(boot_delay != 0u)
        {
            uint32 left = boot_delay;

            ARKRTOS_LOGI("BOOT", "boot delay %u ms before autostart (debugger attach window)",
                       boot_delay);
            while(left > 0u)
            {
                uint32 chunk = (left > 10u) ? 10u : left;

                arkrtos_port_delay_us(chunk * 1000u);
                left -= chunk;
            }
        }
    }

#if (ARKRTOS_USE_VFS == 1)
    /* Linux-style path namespace: / is a volatile scratch area, /dev mirrors
     * the device registry, and volumes live under /mnt/<name>. */
    {
        int32 vfs_rc;
        int    skip_vfs = 0;

#if (ARKRTOS_USE_SDRAM == 1)
        /* The ramfs nodes live in the external RAM window, so VFS may only come
         * up once the window is proven.  "Proven" is an explicit step: bring-up
         * in board init programs the controller but never touches the window,
         * because a bus error there happens before the fault handler and the
         * console exist and the board would simply go silent.
         *
         * ARKRTOS_SDRAM_VERIFY_AT_BOOT=1 asks for the proof here, where a
         * failure is at least reported (PANIC BUS with the address) instead of
         * swallowed.  Leave it at 0 on hardware whose memory path is not yet
         * known good, so the board still reaches a prompt and "sdram info /
         * regs / fsm" can be asked what is wrong.
         *
         * 默认改 1（2026-11-07）：这块 FK743M5-XIH6 的内存通路已经过验证
         * （缺的是 FMC_BCR1.FMCEN，见 drvfmc.c），而 "跳过 VFS" 的代价比
         * 想象的大得多——没有 VFS 就没有 /tmp，`mini run` 连源文件都读不到，
         * 而控制台上只看到 `ls: no mount`，看不出根因在 SDRAM。 换一块未验证
         * 的板时把它设回 0 再看 "sdram regs/fsm"。 */
#ifndef ARKRTOS_SDRAM_VERIFY_AT_BOOT
#define ARKRTOS_SDRAM_VERIFY_AT_BOOT 1
#endif
#if (ARKRTOS_SDRAM_VERIFY_AT_BOOT == 1)
        (void)arkrtos_sdram_verify();
#endif
        if(arkrtos_sdram_ready() != 1)
        {
            ARKRTOS_LOGE("BOOT", "SDRAM configured=%d verified=0, skipping VFS: "
                       "its file buffers live there (run 'sdram verify')",
                       (int)arkrtos_sdram_configured());
            skip_vfs = 1;
        }
#endif

        if(skip_vfs == 0)
        {
            vfs_rc = arkrtos_vfs_init();
            if(vfs_rc != 0)
            {
                ARKRTOS_LOGE("BOOT", "arkrtos_vfs_init failed: %d (%s)", (int)vfs_rc,
                           arkrtos_vfs_error_name(vfs_rc));
            }
        }

#if (ARKRTOS_AUTOMOUNT_NOR == 1)
        /* 板上的 fstab：NOR 卷挂到 /mnt/nor，控制台一起来就能
         * "cd /mnt/nor && make"，不用先敲 fs mount + mount。
         *
         * 挂不上不拦启动：卷可能是空片（没 format），也可能这块板根本没焊
         * NOR。那两种都要能进控制台自己去问（`nor info` / `mount`），所以这里
         * 只报一行、把个子命令级的失败留给现场，不在这里假装成功。
         *
         * 顺序：vfs_init 之后（先有 /mnt 这层命名空间），且只在 VFS 没被跳过
         * 时才有意义——SDRAM 没验过时 VFS 起不来，挂载点也就不存在。 */
        if((skip_vfs == 0) && (vfs_rc == 0))
        {
            int32 mrc = arkrtos_vfs_mount_volume(ARKRTOS_FS_DEV_NAME,
                                                 (uint32)ARKRTOS_FS_BASE,
                                                 (uint32)ARKRTOS_FS_SIZE,
                                                 ARKRTOS_AUTOMOUNT_NOR_POINT);

            if(mrc == 0)
            {
                ARKRTOS_LOGI("BOOT", "%s mounted at %s",
                           ARKRTOS_FS_DEV_NAME, ARKRTOS_AUTOMOUNT_NOR_POINT);
            }
            else
            {
                ARKRTOS_LOGE("BOOT", "cannot mount %s at %s: %d (%s); "
                           "the console still comes up, run 'fs mount' / 'nor info'",
                           ARKRTOS_FS_DEV_NAME, ARKRTOS_AUTOMOUNT_NOR_POINT,
                           (int)mrc, arkrtos_vfs_error_name(mrc));

                /* The common first-boot state: the chip is blank (or was
                 * formatted for something else).  Autostart must not format -
                 * that would erase a volume because a mount happened to fail -
                 * so name the situation and leave the decision to the
                 * console. */
                if(arkrtos_fs_err_is_unformatted(arkrtos_fs_last_error()) != 0u)
                {
                    ARKRTOS_LOGE("BOOT", "%s holds no littlefs volume (blank or foreign); "
                               "nothing was written.  Run 'fs format %s' on purpose "
                               "to create one",
                               ARKRTOS_FS_DEV_NAME, ARKRTOS_FS_DEV_NAME);
                }
            }
        }
#endif /* ARKRTOS_AUTOMOUNT_NOR */
    }
#endif /* ARKRTOS_USE_VFS */

    /* Power-on recovery. reclaim() MUST run after the pool scan, not before:
     * it decides what is garbage from the slot table, and right after
     * arkrtos_ptable_init() that table is empty. */
    (void)arkrtos_loader_scan_driver();
    (void)arkrtos_loader_reclaim();

    arkrtos_crash_scan_apply();

    if(arkrtos_loader_scan_driver() > 0u)
    {
        arkrtos_loader_start_autostart_driver();
    }

    if(arkrtos_loader_scan() > 0u)
    {
        arkrtos_loader_start_autostart();
    }

    #if (SHELL_ENABLE == 1)
    /* Console enabled: the shell task owns the serial port, so the resident
     * installer task is not registered here. */
    arkrtos_shell_init();
    #else
    /* No console: the resident installer task receives images by itself. */
    arkrtos_installer_init();
    #endif

    /* Structure self-audit: the table is populated and the images are
     * registered, so this is the first moment the invariants mean anything. */
    (void)arkrtos_audit_boot();

    /* Guard last: the watchdog is armed here (when the board asks for
     * it) and from this point on the kernel decides whether it gets fed. */
    arkrtos_guard_init();

    /* One line that answers the two questions asked most often on the
     * console: how much room is left for images, and how many tasks the
     * application set has already consumed. */
    {
        uint32 pool_largest = 0u;
        uint32 pool_free    = arkrtos_loader_pool_free(&pool_largest);

        ARKRTOS_LOGI("POOL", "free %u B (largest run %u B), tasks %d/%u",
                   pool_free, pool_largest,
                   (int)arkrtos_task_count, (uint32)ARKRTOS_TASK_MAX_NUM);
    }
}

static void arkrtos_start_idle(void)
{
    uint32 psp = (uint32)(&arkrtos_idle_stack[ARKRTOS_IDLE_STACK_WORDS]);
    psp &= ~0x7u;

    #if (ARKRTOS_USE_MPU == 1)
    arkrtos_port_set_idle_mpu((uint32)arkrtos_start_idle,
                            (uint32)arkrtos_idle_stack,
                            sizeof(arkrtos_idle_stack));
    #endif

    /* Start the idle context privileged: the switch path applies each
     * task's own privilege on the first switch, and the idle task itself
     * is kernel code. */
    arkrtos_port_enter_idle(psp, 1u);

    arkrtos_port_start_timer(ARKRTOS_TICK_PERIOD_US);

    /* The idle PSP context now exists, so the scheduler may switch: this
     * is what starts the first registered task. */
    arkrtos_port_switch_enable();

    while(1)
    {
        ARKRTOS_WFI();
    }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: line number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
