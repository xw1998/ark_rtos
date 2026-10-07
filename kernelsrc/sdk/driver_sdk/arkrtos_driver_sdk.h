/**
* @brief Ark RTOS Driver SDK - 驱动开发接口
* @details 驱动有两种部署形态，注册/注销/计数接口相同，
*          驱动代码不需要区分自己是哪种形态：
*          由宏 ARKRTOS_DRV_USER_MODE 选择：
*          - 未定义（默认）：内核态模式，随内核一起编译，直接调用 arkrtos_dev_register 等
*          - 已定义：用户态模式，编译为独立固件，经 SVC 0x14 与内核交互
*          用户态模式下任务/事件接口由 arkrtos_drv_oslib.c 提供 SVC 封装。
*          本头文件不依赖任何 MCU 头文件，SDK 可独立使用。
*/

#ifndef __ARKRTOS_DRIVER_SDK_H__
#define __ARKRTOS_DRIVER_SDK_H__

#include "arkrtos_types.h"
#include "arkrtos_ulog.h"
#include "arkrtos_svc_call.h"

#define ARKRTOS_DEV_CTRL_OPEN          (0x0001)
#define ARKRTOS_DEV_CTRL_CLOSE         (0x0002)
#define ARKRTOS_DEV_CTRL_SET_BAUD      (0x0010)
#define ARKRTOS_DEV_CTRL_SET_MODE      (0x0011)
#define ARKRTOS_DEV_CTRL_GET_STATUS    (0x0020)
#define ARKRTOS_DEV_CTRL_RESET         (0x0030)

#define ARKRTOS_DRV_OK                 (0)
#define ARKRTOS_DRV_ERROR              (-1)
#define ARKRTOS_DRV_BUSY               (-2)
#define ARKRTOS_DRV_TIMEOUT            (-3)
#define ARKRTOS_DRV_INVALID_PARAM      (-4)

typedef struct {
    uint32 block_size;
} arkrtos_dev_hdr_t;

typedef arkrtos_dev_hdr_t *(*arkrtos_drv_open_func)(uint32 dev_id, uint32 param);
typedef int32            (*arkrtos_drv_close_func)(arkrtos_dev_hdr_t *obj);
typedef int32            (*arkrtos_drv_read_func)(arkrtos_dev_hdr_t *obj, uint8 *pdata, int32 len);
typedef int32            (*arkrtos_drv_write_func)(arkrtos_dev_hdr_t *obj, uint8 *pdata, int32 len);
typedef int32            (*arkrtos_drv_ioctl_func)(arkrtos_dev_hdr_t *obj, uint32 code, uint32 value);

typedef struct {
    arkrtos_drv_open_func    drv_open;
    arkrtos_drv_close_func   drv_close;
    arkrtos_drv_read_func    drv_read;
    arkrtos_drv_write_func   drv_write;
    arkrtos_drv_ioctl_func   drv_ctrl;
} arkrtos_dev_drv_t;

int32 arkrtos_drv_register(const char *name, arkrtos_dev_drv_t *drv, uint32 dev_num);
int32 arkrtos_drv_unregister(const char *name);
int32 arkrtos_drv_get_count(void);

/**
* @brief Hardware safe-state ("will") callback a driver declares for a device
* @param obj   the device instance that was opened (driver private struct; hdr first)
* @param owner the (already dead) task that owned it; for logging only
* @return 0 = brought to its safe state; non-zero = the driver reports failure
*
* @warning Runs from the task-exit / fault-recovery path, possibly with
*          interrupts masked. Must be non-blocking (bounded register / GPIO
*          writes only - no wait, no delay, no SVC, no filesystem or Flash)
*          and idempotent. See docs/App崩溃硬件安全与遗言机制.md.
*/
typedef int32 (*arkrtos_hwsafe_fn)(void *obj, uint32 owner);

int32 arkrtos_drv_hwsafe_register(const char *dev_name, arkrtos_hwsafe_fn fn);
int32 arkrtos_drv_hwsafe_unregister(const char *dev_name);

#ifdef ARKRTOS_DRV_USER_MODE

#define ARKRTOS_SVC_DRV_MGR           (0x14)

ARKRTOS_SVC_DECL_1(int32, ARKRTOS_SVC_DRV_MGR, arkrtos_call_drv_mgr, uint32 *);

/* 以下任务/事件接口由 arkrtos_drv_oslib.c 提供 SVC 封装 */
void   arkrtos_task_wait(uint32 ms);
void   arkrtos_task_wait_period(void);
void   arkrtos_task_delay(uint32 us);
uint32 arkrtos_get_time_ms(void);
int32  arkrtos_event_create(char *name);
int32  arkrtos_event_wait(int32 handle, int32 timeout);

/* Wait results, same values as the kernel headers (arkrtos_def.h).  The
 * driver SDK does not include them, so they are repeated here behind
 * the usual guards. */
#ifndef ARKRTOS_SYNC_OK
#define ARKRTOS_SYNC_OK             (0)
#endif
#ifndef ARKRTOS_SYNC_ERR_TIMEOUT
#define ARKRTOS_SYNC_ERR_TIMEOUT    (-2)
#endif
#ifndef ARKRTOS_SYNC_ERR_WOULDBLOCK
#define ARKRTOS_SYNC_ERR_WOULDBLOCK (-4)
#endif
void   arkrtos_event_set(int32 handle);

#else

extern int32 arkrtos_dev_register(const char *name, arkrtos_dev_drv_t *drv, uint32 dev_num);
extern int32 arkrtos_dev_unregister(const char *name);
extern int32 arkrtos_dev_get_count(void);
extern int32 arkrtos_hwsafe_register(const char *dev_name, arkrtos_hwsafe_fn fn);
extern int32 arkrtos_hwsafe_unregister(const char *dev_name);

int32 arkrtos_drv_hwsafe_register(const char *dev_name, arkrtos_hwsafe_fn fn)
{
    return arkrtos_hwsafe_register(dev_name, fn);
}

int32 arkrtos_drv_hwsafe_unregister(const char *dev_name)
{
    return arkrtos_hwsafe_unregister(dev_name);
}

#endif

#endif
