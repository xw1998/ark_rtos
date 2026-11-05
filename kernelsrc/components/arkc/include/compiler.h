/* Ark C Compiler - compiler.h */
#ifndef ARK_COMPILER_H
#define ARK_COMPILER_H

#include "common.h"

typedef struct {
    const char *input_path;
    const char *output_path;  /* NULL = derive default */
    int         emit_asm;     /* -S */
    int         verbose;
    const char *target;       /* -target <name>; NULL = default */
    const char *include_dirs[16];
    int         include_dir_count;
    int         list_backends;
    /* === 0.3.0 additions === */
    int         compile_only;     /* -c: generate object file only */
    int         preprocess_only;  /* -E: output preprocessor result only */
    int         link_mode;        /* -link: link multiple objects */
    int         vector_table;     /* --vector-table: reserve ARM_VT_SIZE at the
                                   * front of the image and emit a Cortex-M
                                   * vector table there (no image shift) */
    const char *link_inputs[32];  /* -link inputs */
    int         link_input_count;
    int         opt_level;        /* -O0/-O1/-O2: applied by src/opt.c (range-fold + constant fold) */
    int         debug_info;       /* -g: emits DWARF .debug_info/.debug_abbrev/.debug_line */
    int         no_unused_warn;   /* -Wno-unused */
    /* === 0.6.0 additions: armcc 5 compatibility === */
    const char *cpu;            /* --cpu/-mcpu target CPU name (e.g. Cortex-M4) */
    int         gen_depend;     /* -MMD/--depend: generate dependency file */
    const char *depend_file;    /* -MF dependency file output path */
    int         split_sections; /* -ffunction-sections/--split_sections: one section per function */
    /* === 0.7.0 additions: scatter/linker script support === */
    const char *scatter_file;  /* --scatter <file>: ARM scatter .sct file */
    const char *linker_script; /* -T <file>: GNU ld linker script .ld file */
    /* === 0.7.1 additions: -D command-line macro defines === */
    const char *pre_defines[64]; /* -DNAME or -DNAME=VALUE entries */
    int         pre_define_count;
    /* === 0.8.0 additions: MDK project auto-discovery === */
    const char *uvprojx;  /* --uvprojx <file>: MDK .uvprojx to auto-extract -I/-D */
} ArkOptions;

int ark_compile(const ArkOptions *opts);
int ark_compile_and_run(const char *path);

#endif /* ARK_COMPILER_H */
