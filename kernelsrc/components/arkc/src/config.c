/* Ark C Compiler - config
 * Runtime configuration: merge defaults / config file / command-line switches into g_config.
 *
 * Priority: command-line > config file > compile-time defaults (ARKCC_FEATURE_*).
 *   - config_init            : fill g_config with macro defaults.
 *   - config_load_file(path) : parse INI-style arkcc.cfg, override defaults.
 *   - config_apply_args      : scan argv for -O/-g/-fxxx/-fno-xxx,
 *                              override file settings (highest priority).
 */
#include "config.h"
#include "platform.h"
#include "flash_fs.h"
#include "diag.h"

ArkccConfig g_config;

/* Convert string value ("0"/"1"/"true"/"false" etc.) to int 0/1.
 * Returns 0 on parse failure. */
static int parse_bool(const char *s) {
    if (!s || !*s) return 0;
    if (s[0] == '0' && s[1] == 0) return 0;
    if (s[0] == '1' && s[1] == 0) return 1;
    if ((s[0] == 't' || s[0] == 'T') &&
        (s[1] == 'r' || s[1] == 'R') &&
        (s[2] == 'u' || s[2] == 'U') &&
        (s[3] == 'e' || s[3] == 'E') &&
        s[4] == 0) return 1;
    if ((s[0] == 'f' || s[0] == 'F') &&
        (s[1] == 'a' || s[1] == 'A') &&
        (s[2] == 'l' || s[2] == 'L') &&
        (s[3] == 's' || s[3] == 'S') &&
        (s[4] == 'e' || s[4] == 'E') &&
        s[5] == 0) return 0;
    /* Non-zero digits are treated as true */
    if (s[0] >= '0' && s[0] <= '9') return 1;
    return 0;
}

static int parse_int(const char *s, int *out) {
    int sign = 1; int v = 0; int seen = 0;
    if (!s || !*s) return 0;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; seen = 1; }
    if (!seen) return 0;
    *out = sign * v;
    return 1;
}

void config_init(void) {
    g_config.enable_vla           = ARKCC_FEATURE_VLA;
    g_config.enable_complex       = ARKCC_FEATURE_COMPLEX;
    g_config.enable_wchar         = ARKCC_FEATURE_WCHAR;
    g_config.enable_trigraph      = ARKCC_FEATURE_TRIGRAPH;
    g_config.enable_fpu_hw        = ARKCC_FEATURE_FPU_HW;
    g_config.enable_dwarf         = ARKCC_FEATURE_DWARF;
    g_config.enable_opt_constfold = ARKCC_FEATURE_OPT_CONSTFOLD;
    g_config.enable_opt_dce       = ARKCC_FEATURE_OPT_DCE;
    g_config.enable_opt_regalloc  = ARKCC_FEATURE_OPT_REGALLOC;
    g_config.enable_opt_inline    = ARKCC_FEATURE_OPT_INLINE;
    g_config.enable_opt_vectorize = ARKCC_FEATURE_OPT_VECTORIZE;
    g_config.opt_level            = 0;
    g_config.debug_info           = 0;
    g_config.split_sections       = 0;
    g_config.config_file          = 0;
    /* 0.6.x: armcc 5 compatible runtime switch defaults */
    g_config.enum_is_int          = 0;
    g_config.dollar_in_identifiers= 1;  /* armcc supports $ in identifiers by default */
    g_config.signed_chars         = 0;
    g_config.unsigned_chars       = 0;
    g_config.wchar16              = 0;
    g_config.wchar32              = 0;
    g_config.dwarf_version        = 0;
    g_config.scatter_file         = 0;
    g_config.fpu_type             = 0;
}

/* Apply "key=value" to g_config. section is the current INI section name ("features"/"general").
 * Unknown keys are silently ignored - the config file may contain items not yet recognized. */
static void apply_kv(const char *section, const char *key, const char *val) {
    if (!key || !val) return;
    if (ark_streq(section, "features")) {
        if (ark_streq(key, "vla"))            g_config.enable_vla           = parse_bool(val);
        else if (ark_streq(key, "complex"))   g_config.enable_complex       = parse_bool(val);
        else if (ark_streq(key, "wchar"))     g_config.enable_wchar         = parse_bool(val);
        else if (ark_streq(key, "trigraph"))  g_config.enable_trigraph      = parse_bool(val);
        else if (ark_streq(key, "fpu_hw"))    g_config.enable_fpu_hw        = parse_bool(val);
        else if (ark_streq(key, "dwarf"))     g_config.enable_dwarf         = parse_bool(val);
        else if (ark_streq(key, "opt_constfold")) g_config.enable_opt_constfold = parse_bool(val);
        else if (ark_streq(key, "opt_dce"))   g_config.enable_opt_dce       = parse_bool(val);
        else if (ark_streq(key, "opt_regalloc")) g_config.enable_opt_regalloc = parse_bool(val);
        else if (ark_streq(key, "opt_inline"))    g_config.enable_opt_inline    = parse_bool(val);
        else if (ark_streq(key, "opt_vectorize")) g_config.enable_opt_vectorize = parse_bool(val);
    } else if (ark_streq(section, "general")) {
        if (ark_streq(key, "opt_level")) {
            int v = 0;
            if (parse_int(val, &v)) {
                if (v < 0) v = 0;
                if (v > 2) v = 2;
                g_config.opt_level = v;
            }
        } else if (ark_streq(key, "debug_info")) {
            g_config.debug_info = parse_bool(val) ? 1 : 0;
        } else if (ark_streq(key, "split_sections") || ark_streq(key, "function-sections")) {
            g_config.split_sections = parse_bool(val) ? 1 : 0;
        }
    }
}

void config_load_file(const char *path) {
    static char cfg_buf[4096];
    char section[64] = "";
    int len;
    int i;
    char *line_start;
    if (!path) return;
    /* Read the entire config file at once via fs_read_file (host=stdio, STM32=W25Q Flash).
     * Silently fall back to compile-time defaults on failure. */
    len = fs_read_file(path, cfg_buf, (int)sizeof(cfg_buf) - 1);
    if (len < 0) {
        diag_tool(DIAG_SEV_WARNING, path,
                  "cannot open config file, compile-time defaults are used",
                  0,0,0,0,0,0);
        return;
    }
    cfg_buf[len] = 0;
    g_config.config_file = path;
    /* Parse line by line: split by \n manually. Handle CR/LF too. */
    line_start = cfg_buf;
    for (i = 0; i <= len; i++) {
        if (i == len || cfg_buf[i] == '\n') {
            char *p = line_start;
            char *key;
            char *val;
            char *eq;
            char *end;
            /* Truncate current line */
            if (i < len) cfg_buf[i] = 0;
            /* Strip trailing \r */
            {
                int ll = (int)(&cfg_buf[i] - p);
                while (ll > 0 && (p[ll-1] == '\r')) { p[ll-1] = 0; ll--; }
            }
            /* Skip leading whitespace */
            while (*p == ' ' || *p == '\t') p++;
            /* Empty line / comment */
            if (*p == 0 || *p == ';' || *p == '#') {
                line_start = &cfg_buf[i + 1];
                continue;
            }
            /* Section name [section] */
            if (*p == '[') {
                char *q = p + 1;
                char *e = q;
                while (*e && *e != ']') e++;
                *e = 0;
                {
                    int k = 0;
                    while (q[k] && k < (int)sizeof(section) - 1) { section[k] = q[k]; k++; }
                    section[k] = 0;
                }
                line_start = &cfg_buf[i + 1];
                continue;
            }
            /* key=value */
            eq = p;
            while (*eq && *eq != '=') eq++;
            if (*eq != '=') {
                line_start = &cfg_buf[i + 1];
                continue;
            }
            *eq = 0;
            /* Strip trailing whitespace from key */
            end = eq;
            while (end > p && (end[-1] == ' ' || end[-1] == '\t')) { end--; *end = 0; }
            key = p;
            val = eq + 1;
            /* Skip leading whitespace in val */
            while (*val == ' ' || *val == '\t') val++;
            /* Strip trailing whitespace from val */
            {
                char *ve = val;
                while (*ve) ve++;
                while (ve > val && (ve[-1] == ' ' || ve[-1] == '\t')) { ve--; *ve = 0; }
            }
            apply_kv(section, key, val);
            line_start = &cfg_buf[i + 1];
        }
    }
}

/* Command line recognizes the following arguments and updates g_config:
 *   -O0/-O1/-O2/-O3  : opt_level
 *   -g               : debug_info=1
 *   -fno-<feature>   : disable a feature
 *   -f<feature>      : enable a feature
 *   -config <file>   : config file path (recorded only, not loaded here; main handles it)
 *
 * Feature names: vla/complex/wchar/trigraph/fpu/fpu_hw/dwarf/opt-constfold/
 *               opt-dce/opt-regalloc/constfold/dce/regalloc
 * Unrecognized arguments are left untouched (main still handles -o/-target/-I etc.).
 *
 * 0.6.x adds armcc 5 style --xxx options (most silently accepted):
 *   --enum_is_int / -fenum-is-int / --dollar / --signed_chars / --unsigned_chars
 *   --wchar16 / --wchar32 / --dwarf2 / --dwarf3 / --scatter <file> / --fpu <name>
 *   --thumb / --arm / --remarks / --autoinline / --no_autoinline
 *   --vectorize / --no_vectorize / --split_sections / --library_type
 *   --locale / --multibyte_chars / --data_reorder / --diag_style
 *   --remove_unneeded_entities
 *   --cpp : error "C++ not supported"
 */
void config_apply_args(int argc, char **argv) {
    int i;
    if (!argv) return;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!a || a[0] != '-') continue;
        if (a[0] == '-' && a[1] == 'O' && a[2] >= '0' && a[2] <= '9' && a[3] == 0) {
            g_config.opt_level = a[2] - '0';
            if (g_config.opt_level > 2) g_config.opt_level = 2;
            /* -O2 auto-enables inlining and vectorization */
            if (g_config.opt_level >= 2) {
                g_config.enable_opt_inline    = 1;
                g_config.enable_opt_vectorize = 1;
            }
            continue;
        }
        if (ark_streq(a, "-g")) {
            g_config.debug_info = 1;
            /* The ARM emitter gates .debug_info/.debug_abbrev/.debug_line on
             * dwarf_version >= 2, so plain -g used to set a flag that nothing
             * read and produced no debug sections at all. Map -g onto DWARF 2
             * (the oldest supported level); an explicit --dwarf2/--dwarf3 seen
             * later on the command line still wins. */
            if (g_config.dwarf_version < 2) g_config.dwarf_version = 2;
            continue;
        }
        if (ark_streq(a, "-config") && i + 1 < argc) {
            /* Record path only; loading is done by main before calling config_apply_args */
            g_config.config_file = argv[i + 1];
            i++;
            continue;
        }
        /* --cpp: arkcc does not support C++; report error explicitly */
        if (ark_streq(a, "--cpp")) {
            /* Counted as an error, so the driver's exit code is non-zero:
             * telling the user "not supported" and then exiting 0 is the same
             * silent-wrong-answer failure the diagnostic layer exists to stop. */
            diag_tool(DIAG_SEV_ERROR, "arkcc",
                      "C++ (--cpp) is not supported", 0,0,0,0,0,0);
            continue;
        }
        /* 0.6.x: armcc 5 style --xxx options */
        if (ark_streq(a, "--enum_is_int") || ark_streq(a, "-fenum-is-int")) {
            g_config.enum_is_int = 1;
            continue;
        }
        if (ark_streq(a, "--dollar")) {
            g_config.dollar_in_identifiers = 1;
            continue;
        }
        if (ark_streq(a, "--signed_chars")) {
            g_config.signed_chars = 1;
            g_config.unsigned_chars = 0;
            continue;
        }
        if (ark_streq(a, "--unsigned_chars")) {
            g_config.unsigned_chars = 1;
            g_config.signed_chars = 0;
            continue;
        }
        if (ark_streq(a, "--wchar16")) {
            g_config.wchar16 = 1;
            g_config.wchar32 = 0;
            continue;
        }
        if (ark_streq(a, "--wchar32")) {
            g_config.wchar32 = 1;
            g_config.wchar16 = 0;
            continue;
        }
        if (ark_streq(a, "--dwarf2")) { g_config.dwarf_version = 2; continue; }
        if (ark_streq(a, "--dwarf3")) { g_config.dwarf_version = 3; continue; }
        if (ark_streq(a, "--scatter") && i + 1 < argc) {
            g_config.scatter_file = argv[i + 1];
            i++;
            continue;
        }
        /* -T/--linker-script: GNU ld style. Stored separately so the backend
         * can parse it via linker_script_parse_file and the -S mode can do
         * .ld -> .sct conversion. */
        if ((ark_streq(a, "-T") || ark_streq(a, "--linker-script") ||
             ark_streq(a, "--script")) && i + 1 < argc) {
            g_config.linker_script = argv[i + 1];
            i++;
            continue;
        }
        if (ark_streq(a, "--fpu") && i + 1 < argc) {
            const char *name = argv[i + 1];
            g_config.fpu_type = name;
            /* Simplified: none -> disable FPU; others -> enable FPU */
            if (ark_streq(name, "none")) g_config.enable_fpu_hw = 0;
            else                          g_config.enable_fpu_hw = 1;
            i++;
            continue;
        }
        /* Silently accepted armcc 5 options (no effect on code generation) */
        if (ark_streq(a, "--thumb") || ark_streq(a, "--arm") ||
            ark_streq(a, "--remarks") ||
            ark_streq(a, "--library_type") || ark_streq(a, "--locale") ||
            ark_streq(a, "--multibyte_chars") || ark_streq(a, "--data_reorder") ||
            ark_streq(a, "--diag_style") ||
            ark_streq(a, "--remove_unneeded_entities")) {
            continue;
        }
        /* --autoinline / --no_autoinline: enable/disable auto inlining */
        if (ark_streq(a, "--autoinline")) {
            g_config.enable_opt_inline = 1;
            continue;
        }
        if (ark_streq(a, "--no_autoinline")) {
            g_config.enable_opt_inline = 0;
            continue;
        }
        /* --vectorize / --no_vectorize: enable/disable auto vectorization */
        if (ark_streq(a, "--vectorize")) {
            g_config.enable_opt_vectorize = 1;
            continue;
        }
        if (ark_streq(a, "--no_vectorize")) {
            g_config.enable_opt_vectorize = 0;
            continue;
        }
        /* -fno-xxx / -fxxx */
        if (a[0] == '-' && a[1] == 'f') {
            int enable;
            const char *name;
            if (a[2] == 'n' && a[3] == 'o' && a[4] == '-') {
                enable = 0;
                name = a + 5;
            } else {
                enable = 1;
                name = a + 2;
            }
            if (!*name) continue;
            if      (ark_streq(name, "vla"))            g_config.enable_vla           = enable;
            else if (ark_streq(name, "complex"))        g_config.enable_complex       = enable;
            else if (ark_streq(name, "wchar"))          g_config.enable_wchar         = enable;
            else if (ark_streq(name, "trigraph"))       g_config.enable_trigraph      = enable;
            else if (ark_streq(name, "fpu") || ark_streq(name, "fpu_hw") || ark_streq(name, "fpu-hw"))
                g_config.enable_fpu_hw                  = enable;
            else if (ark_streq(name, "dwarf"))          g_config.enable_dwarf         = enable;
            else if (ark_streq(name, "opt-constfold") || ark_streq(name, "constfold"))
                g_config.enable_opt_constfold           = enable;
            else if (ark_streq(name, "opt-dce") || ark_streq(name, "dce"))
                g_config.enable_opt_dce                 = enable;
            else if (ark_streq(name, "opt-regalloc") || ark_streq(name, "regalloc"))
                g_config.enable_opt_regalloc            = enable;
            else if (ark_streq(name, "opt-inline") || ark_streq(name, "autoinline"))
                g_config.enable_opt_inline              = enable;
            else if (ark_streq(name, "opt-vectorize") || ark_streq(name, "vectorize"))
                g_config.enable_opt_vectorize           = enable;
            else if (ark_streq(name, "function-sections") || ark_streq(name, "split_sections"))
                g_config.split_sections                 = enable;
            else if (ark_streq(name, "enum-is-int"))   g_config.enum_is_int          = enable;
            else if (ark_streq(name, "dollar"))        g_config.dollar_in_identifiers= enable;
            /* Unrecognized -fxxx: ignore silently (avoid conflict with gcc-style -fxxx) */
        }
    }
}

void config_print(void) {
    ark_log("[arkcc] config:\n");
    ark_log("  vla=%d complex=%d wchar=%d trigraph=%d\n",
            g_config.enable_vla, g_config.enable_complex,
            g_config.enable_wchar, g_config.enable_trigraph);
    ark_log("  fpu_hw=%d dwarf=%d\n",
            g_config.enable_fpu_hw, g_config.enable_dwarf);
    ark_log("  opt_constfold=%d opt_dce=%d opt_regalloc=%d\n",
            g_config.enable_opt_constfold, g_config.enable_opt_dce,
            g_config.enable_opt_regalloc);
    ark_log("  opt_inline=%d opt_vectorize=%d\n",
            g_config.enable_opt_inline, g_config.enable_opt_vectorize);
    ark_log("  opt_level=%d debug_info=%d split_sections=%d\n",
            g_config.opt_level, g_config.debug_info, g_config.split_sections);
    ark_log("  config_file=%s\n",
            g_config.config_file ? g_config.config_file : "(none)");
}
