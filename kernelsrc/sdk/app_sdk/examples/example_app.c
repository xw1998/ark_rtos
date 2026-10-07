/**
* @brief Ark RTOS App SDK - 示例应用程序
*/

#include "arkrtos.h"

void AppMain(void)
{
    int32 com1 = arkrtos_dev_open("ttyS0", 0);
    int32 led  = arkrtos_dev_open("led0", 0);
    int32 evt  = arkrtos_event_create("my_event");
    uint8 buf[64];
    uint32 tick = 0;

    while(1)
    {
        arkrtos_dev_write(led, (void *)&tick, 4);

        if(com1 >= 0)
        {
            int32 len = arkrtos_dev_read(com1, buf, 64);
            if(len > 0)
            {
                arkrtos_dev_write(com1, buf, len);
            }
        }

        tick = arkrtos_get_time_ms();

        arkrtos_task_wait(500);
    }
}
