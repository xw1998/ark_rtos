#ifndef __ARK_VFS_H__
#define __ARK_VFS_H__

#include "ark_rtos.h"

//vfs½á¹¹Ìå
struct ark_vfs_block
{
    uint32_t id;
    uint32_t type;
    uint32_t size;
    uint32_t offset;
}

#endif