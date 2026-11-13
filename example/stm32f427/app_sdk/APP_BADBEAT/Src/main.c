/**
* @file  main.c
* @brief 故意违反心跳合同的应用（负向用例）。
* @details 应用自己声明一个心跳窗口（`arkrtos_heartbeat(1000)` = "我最久隔 1 s
*          报一次到"），然后每轮睡 3 s——比自己的承诺还久。守卫按违约记一条
*          `REASON_HEARTBEAT`（每次开机最多一条），到上限（布局里的
*          `fault_restart_max`）之后槽位被保持为禁用，除非有人手动 `app start`。
*
*          它和 `APP_BAD` 是一对：那个测"越权被拦"，这个测"违约被记账"。
*          判据只看内核自己的账（`guard` 的 violating 列、`crash` 的
*          held=heartbeat），不看灯、也不看打印的先后。
*
*          注意：违约不等于死循环。应用每轮照样醒、照样打印，只是报到太晚——
*          这正是文档 §1.5 说的"判定只看报到间隔，不看任务当时在干什么"。
*/
#include <stdio.h>
#include "arkrtos.h"

int main(void)
{
    printf("BEAT_BAD: declare heartbeat 1000 ms, then stay silent 3000 ms per round\n");
    (void)arkrtos_heartbeat(1000u);

    for(;;)
    {
        arkrtos_task_wait(3000u);            /* 3x the declared window: contract broken */
        printf("BEAT_BAD: woke after 3000 ms while promising 1000 ms\n");
    }

    return 0;
}
