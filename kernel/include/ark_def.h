#ifndef __ARK_DEF_H__
#define __ARK_DEF_H__

#include "ark_rtos.h"

// 任务列表节点结构体
struct ark_list_node
{
    struct ark_list_node *next;              /* 指向后一个节点 */
    struct ark_list_node *prev;              /* 指向前一个节点 */
};
typedef struct ark_list_node ark_list_t;    

// 任务结构体
struct ark_task
{
    char name[NAME_MAX_LEN];
    void *sp;
    void *task;
    void *parameter;
    void *stack_addr;
    void *stack_size;
#ifdef STATIC_TASK_LIST
    ark_uint16_t ark_task_id;
#else
    ark_list_t task_list;
#endif
};
typedef struct ark_task *ark_task_t;


#endif
