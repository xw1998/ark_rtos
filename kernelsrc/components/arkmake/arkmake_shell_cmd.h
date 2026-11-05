/* ============================================================
 * arkmake_shell_cmd.h - "make" 命令进 shell 的两个入口
 *
 * 与 arkvim / arkc 同一套约定：组件不认识 shell 的存在，shell 只多一次
 * 调用、多一个命令槽。装不下时 arkmake_shell_register 原样返回 idx，
 * 调用方的哨兵照常落位，所以"表满"不会变成越界写。
 * ============================================================ */
#ifndef ARKMAKE_SHELL_CMD_H
#define ARKMAKE_SHELL_CMD_H

/* 在 g_cmd_table[idx] 装上 "make"，返回下一个空位。 */
int arkmake_shell_register(int idx);

/* 命令本体。单独开出来是为了让自检能直接调它（不经 shell 的行编辑器），
 * 也让 help 之外的地方有机会复用同一份参数解析。 */
int cmd_make(int argc, char *argv[]);

#endif /* ARKMAKE_SHELL_CMD_H */
