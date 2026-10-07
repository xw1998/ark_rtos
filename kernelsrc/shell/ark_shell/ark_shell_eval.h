/* ============================================================
 * ark_shell_eval.h - Command line evaluation (shell front end)
 *
 * ark_shell_execute_line() hands the raw line to ark_shell_eval_line(),
 * which turns it into work:
 *   - quote and escape aware tokenisation
 *   - command sequences   a ; b   a && b   a || b
 *   - pipelines           a | b | c
 *   - file redirection    > f   >> f   < f
 * All of this is optional (ARKRTOS_SHELL_REDIRECT); when it is compiled
 * out the operators are ordinary characters and the line is only split
 * on whitespace, exactly as before.
 *
 * The port is the other half of the contract: while a redirect is
 * active it must divert the console writes it would otherwise make into
 * ark_shell_capture_put().  Kernel log output does not go through the
 * port, so it still reaches the console during a pipeline.
 * ============================================================ */
#ifndef ARK_SHELL_EVAL_H
#define ARK_SHELL_EVAL_H

#include "ark_shell_config.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ark_shell_s;

/* ============================================================
 * ark_shell_eval_line - Parse and run one input line
 *
 * Parameters:
 *   shell - shell instance (working directory, aliases, last status)
 *   line  - NUL terminated line, modified in place (tokens are decoded
 *           by compaction, so pass the mutable editor buffer)
 * Returns: the status of the last command executed, which the caller
 *          keeps as "$?".
 * ============================================================ */
int ark_shell_eval_line(struct ark_shell_s *shell, char *line);

/* ============================================================
 * Standard input of the current pipeline / '<' redirection
 *
 * A command that consumes a stream (cat without a path argument) pulls
 * bytes from here.  Outside a pipeline the buffer is empty and getc()
 * returns -1 rather than blocking, because there is nothing to wait for.
 * ============================================================ */
int ark_shell_stdin_getc(void);
int ark_shell_stdin_ready(void);

/* ============================================================
 * Output capture, called by the port layer
 *
 * ark_shell_capture_active() is nonzero while a stage's console output is
 * being collected.  ark_shell_capture_put() returns 1 when it consumed
 * the bytes (so the port must not also write them to the UART) and 0 when
 * no capture is active (the port writes as usual).
 * ============================================================ */
int ark_shell_capture_active(void);
int ark_shell_capture_put(const unsigned char *p, unsigned int n);

#ifdef __cplusplus
}
#endif

#endif /* ARK_SHELL_EVAL_H */
