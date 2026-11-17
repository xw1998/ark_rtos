/**
* @file arkrtos_shell_auth.h
* @brief Console login for the kernel shell (SEC-G01).
* @details Four entry points:
*            arkrtos_shell_auth_init()   - reset the state (boot starts locked)
*            arkrtos_shell_auth_gate()   - the shell's gate hook: 0 lets the
*                                          word through, -1 refuses it and has
*                                          already printed why
*            arkrtos_shell_auth_locked() - 1 while the console is locked
*            arkrtos_shell_auth_cmd_login / _cmd_logout - the two commands
*          With ARKRTOS_USE_SHELL_AUTH off every entry point is a macro, so a
*          default build carries no bytes, no symbols and no gate: the shell
*          behaves exactly as it did before this file existed.
* @author xw
*/
#ifndef __ARKRTOS_SHELL_AUTH_H__
#define __ARKRTOS_SHELL_AUTH_H__

#include "arkrtos_features.h"

#if (defined(__cplusplus))
extern "C" {
#endif

#if (ARKRTOS_USE_SHELL_AUTH == 1)

/** @brief Reset to "locked, no failures".  Called once from the shell task. */
void arkrtos_shell_auth_init(void);

/** @brief Gate hook: 0 = allowed, -1 = refused (reason already printed). */
int arkrtos_shell_auth_gate(const char *name);

/** @brief 1 while the console refuses everything but login/logout/help. */
int arkrtos_shell_auth_locked(void);

/** @brief `login <password>` */
int arkrtos_shell_auth_cmd_login(int argc, char **argv);

/** @brief `logout` */
int arkrtos_shell_auth_cmd_logout(int argc, char **argv);

#else

#define arkrtos_shell_auth_init()               ((void)0)
#define arkrtos_shell_auth_gate(name)           (0)
#define arkrtos_shell_auth_locked()             (0)

#endif /* ARKRTOS_USE_SHELL_AUTH */

#if (defined(__cplusplus))
}
#endif

#endif /* __ARKRTOS_SHELL_AUTH_H__ */
