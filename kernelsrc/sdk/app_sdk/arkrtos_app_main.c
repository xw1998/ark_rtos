/**
* @brief Ark RTOS App SDK - 应用程序入口
*/

#include "arkrtos.h"

ARKRTOS_WEAK void AppMain(void)
{
}

int main(void)
{
    AppMain();
    while(1)
    {
        arkrtos_task_wait(1000);
    }
}
