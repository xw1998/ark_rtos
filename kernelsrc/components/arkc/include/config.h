/* Ark C Compiler - config.h
 * Runtime config: stores switch states read from config file or command line.
 * Priority: command line > config file > compile-time defaults (ARKCC_FEATURE_*)
 */
#ifndef ARK_CONFIG_H
#define ARK_CONFIG_H

#include "arkcc_config.h"

typedef struct {
    /* Runtime switches (config file may override compile-time defaults) */
    int enable_vla;
    int enable_complex;
    int enable_wchar;
    int enable_trigraph;
    int enable_fpu_hw;
    int enable_dwarf;
    int enable_opt_constfold;
    int enable_opt_dce;
    int enable_opt_regalloc;
    int enable_opt_inline;   /* --autoinline / -O2 */
    int enable_opt_vectorize;/* --vectorize / -O2 */
    int opt_level;          /* 0=none, 1=basic, 2=aggressive */
    int debug_info;         /* 0=none, 1=DWARF */
    int split_sections;     /* -ffunction-sections: one section per function */
    const char *config_file; /* config file path */
    /* === 0.6.x additions: armcc 5 compatibility runtime switches === */
    int enum_is_int;          /* --enum_is_int / -fenum-is-int: force enum to int */
    int dollar_in_identifiers;/* --dollar: $ as identifier character */
    int signed_chars;         /* --signed_chars: char defaults to signed */
    int unsigned_chars;       /* --unsigned_chars: char defaults to unsigned */
    int wchar16;              /* --wchar16: wchar_t is 16-bit */
    int wchar32;              /* --wchar32: wchar_t is 32-bit */
    int dwarf_version;        /* --dwarf2=2 / --dwarf3=3; 0=default */
    const char *scatter_file; /* --scatter <file>: scatter load description file */
    const char *linker_script;/* -T/--linker-script <file>: GNU ld script (converted to scatter internally) */
    const char *fpu_type;     /* --fpu <name>: FPU type name */
} ArkccConfig;

extern ArkccConfig g_config;

/* Initialize g_config with compile-time defaults from ARKCC_FEATURE_* macros */
void config_init(void);
/* Parse arkcc.cfg (INI format, key=value under [features]/[general] sections) */
void config_load_file(const char *path);
/* Parse command-line -O0/-O1/-O2/-g/-fxxx/-fno-xxx etc.
 * Recognizes only config-related args and updates g_config; leaves other argv entries untouched.
 * argc/argv are taken directly from main. */
void config_apply_args(int argc, char **argv);
/* Debug print of current g_config contents (to stderr) */
void config_print(void);

#endif /* ARK_CONFIG_H */
