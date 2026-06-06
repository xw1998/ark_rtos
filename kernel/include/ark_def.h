#ifndef __ARK_DEF_H__
#define __ARK_DEF_H__

// 任务结构体
struct ark_task
{
    void (*task)(void *arg);
    void *arg;
    uint32_t stack_size;
    uint32_t priority;
    uint32_t state;
}


#endif
