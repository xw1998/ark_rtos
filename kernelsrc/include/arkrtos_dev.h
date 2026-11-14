/**
* @brief Ark RTOS 设备驱动框架（内核内部）
* @details 提供统一的设备驱动接口和动态驱动注册机制
*          此文件仅供内核内部使用，应用程序应使用 arkrtos.h
*/

#ifndef __ARKRTOS_DEV_H__
#define __ARKRTOS_DEV_H__

#include "arkrtos_def.h"
#include "arkrtos_config.h"
/* 返回值约定：0 成功，负值失败。原先失败处一律写裸 -1/-2，读代码看不出
 * 「为什么失败」；下面这些名字把原因写进代码，**值不得单独修改**
 * （多个名字共用同一个值是有意的，分名只为表意）。 */
#define ARKRTOS_DEV_ERR_PARAM    (-1)   /* 名字/描述符/输出缓冲为空或长度非法 */
#define ARKRTOS_DEV_ERR_ARG      (-2)   /* 注册时名字或驱动指针为空（历史值是 -2，不得改成 -1） */
#define ARKRTOS_DEV_ERR_EXIST    (-2)   /* 同名设备已注册（历史值是 -2，不得改成 -1） */
#define ARKRTOS_DEV_ERR_NOTFOUND (-1)   /* 设备表里没有这个名字 / 驱动缺对应入口 */
#define ARKRTOS_DEV_ERR_HANDLE   (-1)   /* 句柄不是设备（标志位/槽位下标不对） */
#define ARKRTOS_DEV_ERR_STATE    (-1)   /* 槽位已注销 / 设备当前没有打开实例 */
#define ARKRTOS_DEV_ERR_NOIMPL   (-1)   /* 驱动没有实现 read/write/ctrl 中的这一个 */
#define ARKRTOS_DEV_ERR_NOSLOT   (-1)   /* 设备表已满，注册不进去 */

typedef struct {
    uint32 block_size;
} arkrtos_dev_hdr_t;

typedef arkrtos_dev_hdr_t *(*arkrtos_drv_open_func)(uint32 dev_id, uint32 param);
typedef int32            (*arkrtos_drv_close_func)(arkrtos_dev_hdr_t *obj);
typedef int32            (*arkrtos_drv_rw_func)(arkrtos_dev_hdr_t *obj, uint8 *pdata, int32 len);
typedef int32            (*arkrtos_drv_ioctl_func)(arkrtos_dev_hdr_t *obj, uint32 code, uint32 value);

typedef struct {
    arkrtos_drv_open_func    drv_open;
    arkrtos_drv_close_func   drv_close;
    arkrtos_drv_rw_func      drv_read;
    arkrtos_drv_rw_func      drv_write;
    arkrtos_drv_ioctl_func   drv_ctrl;
} arkrtos_dev_drv_t;

typedef struct {
    char dev_name[8];
    arkrtos_dev_drv_t *drv;
    uint32 dev_num;
} arkrtos_dev_desc_t;

void   arkrtos_dev_module_init(void);
int32  arkrtos_dev_register(const char *name, arkrtos_dev_drv_t *drv, uint32 dev_num);
int32  arkrtos_dev_unregister(const char *name);
int32  arkrtos_dev_get_count(void);

/* 按设备表槽位索引取设备名，供 devfs 枚举用。
 * idx  为槽位索引(0 ~ ARKRTOS_DEV_MAX_NUM-1)，与 open/close 的 handle 同一套编号。
 * 空槽位或参数非法返回 -1，且 out[0]=0（不填空值）。
 * 成功写出返回 0。 */
int32  arkrtos_dev_name_at(uint32 idx, char *out, uint32 max);

int32  arkrtos_dev_open_internal(char *name, uint32 param);
int32  arkrtos_dev_close_internal(int32 handle);
int32  arkrtos_dev_read_internal(int32 handle, uint8 *pdata, int32 len);
int32  arkrtos_dev_write_internal(int32 handle, uint8 *pdata, int32 len);
int32  arkrtos_dev_ctrl_internal(int32 handle, uint32 code, uint32 value);

#if ARKRTOS_HW_SAFE
/* Locate the device instance and run its drv_ctrl. Used by the App-crash
 * hardware-safe ("will") path: the kernel only holds the instance pointer,
 * never the handle (the handle belongs to a task that is already dead).
 * Returns the driver's own return value; -1 when the instance is unknown or
 * the driver has no ctrl entry. */
int32  arkrtos_dev_ctrl_obj(arkrtos_dev_hdr_t *obj, uint32 code, uint32 value);
#endif

void   arkrtos_dev_board_init(void);

#endif
