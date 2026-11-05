/* arkcc - diagnostic message table (generated)
 *
 * DO NOT EDIT: regenerate with `python tools/gen_diag_table.py`.
 * Generated from 37 frontend + 7 driver messages.
 */
#include "diag_msg_table.h"

typedef struct {
    int         code;
    int         d_tag;  /* printed as "#<code>-D" at any severity, else 0 */
    const char *text;
} MsgEntry;

/* Sorted by code: lookup below relies on it to stop early. */
static const MsgEntry k_frontend[] = {
    { 4, 0, "out of memory" },
    { 5, 0, "cannot open %s input file %sq2: %s3" },
    { 6, 0, "comment unclosed at end of file" },
    { 7, 0, "unrecognized token" },
    { 8, 0, "missing closing quote" },
    { 9, 0, "nested comment is not allowed" },
    { 11, 1, "unrecognized preprocessing directive" },
    { 12, 0, "parsing restarts here after previous syntax error" },
    { 13, 0, "expected a file name" },
    { 17, 0, "expected a \"]\"" },
    { 18, 0, "expected a \")\"" },
    { 20, 0, "identifier %sq is undefined" },
    { 28, 0, "expression must have a constant value" },
    { 29, 0, "expected an expression" },
    { 35, 0, "#error directive: %s" },
    { 36, 0, "the #if for this directive is missing" },
    { 37, 0, "the #endif for this directive is missing" },
    { 38, 0, "directive is not allowed -- an #else has already appeared" },
    { 39, 0, "division by zero" },
    { 40, 0, "expected an identifier" },
    { 65, 0, "expected a \";\"" },
    { 67, 0, "expected a \"}\"" },
    { 100, 0, "duplicate parameter name" },
    { 101, 0, "%sq has already been declared in the current scope" },
    { 117, 0, "non-void %n should return a value" },
    { 118, 0, "a void function may not return a value" },
    { 137, 0, "expression must be a modifiable lvalue" },
    { 140, 0, "too many arguments in function call" },
    { 148, 0, "%n has already been initialized" },
    { 165, 0, "too few arguments in function call" },
    { 174, 1, "expression has no effect" },
    { 177, 1, "%n was declared but never referenced" },
    { 179, 0, "right operand of \"%%\" is zero" },
    { 223, 1, "function %sq declared implicitly" },
    { 247, 0, "%n has already been defined" },
    { 992, 0, "invalid macro definition: %s" },
    { 1215, 1, "#warning directive: %s" },
};

typedef struct {
    const char *code;
    char        severity;
    const char *text;
} DriverEntry;

static const DriverEntry k_driver[] = {
    { "C3900U", 'U', "Unrecognized option '%s%s'." },
    { "C3901U", 'U', "Missing argument for option '%s'." },
    { "C3903U", 'U', "Argument '%s' not permitted for option '%s'." },
    { "C4056E", 'E', "bad option '%s'" },
    { "C4065E", 'E', "type of input file '%s' unknown\n" },
    { "C4079E", 'E', "%s command with no effect\n" },
    { "C4486W", 'W', "option '-%c' causes input file '%s' to be ignored\n" },
};

/* Local string compare: this file must build in the freestanding STM32
 * profile, where no libc is linked in. */
static int same(const char *a, const char *b) {
    int i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

int diag_msg_has_d_tag(int code) {
    int i;
    for (i = 0; i < (int)(sizeof(k_frontend) / sizeof(k_frontend[0])); i++) {
        if (k_frontend[i].code == code) return k_frontend[i].d_tag;
        if (k_frontend[i].code > code) break;
    }
    return 0;
}

const char *diag_msg_lookup(int code) {
    int i;
    for (i = 0; i < (int)(sizeof(k_frontend) / sizeof(k_frontend[0])); i++) {
        if (k_frontend[i].code == code) return k_frontend[i].text;
        if (k_frontend[i].code > code) break;   /* sorted: no later match */
    }
    return 0;
}

static const DriverEntry *driver_find(const char *code) {
    int i;
    if (!code) return 0;
    for (i = 0; i < (int)(sizeof(k_driver) / sizeof(k_driver[0])); i++)
        if (same(k_driver[i].code, code)) return &k_driver[i];
    return 0;
}

const char *diag_driver_lookup(const char *code) {
    const DriverEntry *e = driver_find(code);
    return e ? e->text : 0;
}

char diag_driver_severity(const char *code) {
    const DriverEntry *e = driver_find(code);
    return e ? e->severity : 0;
}
