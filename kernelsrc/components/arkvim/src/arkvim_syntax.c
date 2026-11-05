/* ============================================================
 * arkvim_syntax.c - Minimal C Syntax Highlighter
 *
 * A single-pass scanner. It recognises:
 *   line comments (//)    block comments (span lines)
 *   "strings" 'chars'       with backslash escapes
 *   numbers (0x.., digits)
 *   C keywords
 *   punctuation operators
 * ============================================================ */
#include "arkvim_syntax.h"
#include <string.h>

static const char *const c_keywords[] = {
    "auto","break","case","char","const","continue","default","do",
    "double","else","enum","extern","float","for","goto","if","inline",
    "int","long","register","return","short","signed","sizeof","static",
    "struct","switch","typedef","union","unsigned","void","volatile",
    "while","_Bool","_Complex","_Imaginary","restrict",
    NULL
};

static bool is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident(char c)
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_keyword(const char *p, int len)
{
    int i;
    for (i = 0; c_keywords[i] != NULL; i++) {
        if ((int)strlen(c_keywords[i]) == len &&
            memcmp(c_keywords[i], p, (size_t)len) == 0) {
            return true;
        }
    }
    return false;
}

void syntax_highlight_line(const char *line, int len,
                           uint8_t *out_attr, bool *in_block)
{
    int i = 0;

    /* Continue a block comment from the previous line. */
    if (*in_block) {
        while (i < len) {
            out_attr[i] = ARKVIM_ATTR_COMMENT;
            if (line[i] == '*' && i + 1 < len && line[i + 1] == '/') {
                out_attr[i + 1] = ARKVIM_ATTR_COMMENT;
                i += 2;
                *in_block = false;
                break;
            }
            i++;
        }
    }

    while (i < len) {
        char c = line[i];

        /* whitespace */
        if (c == ' ' || c == '\t') {
            out_attr[i++] = ARKVIM_ATTR_NORMAL;
            continue;
        }

        /* line comment */
        if (c == '/' && i + 1 < len && line[i + 1] == '/') {
            while (i < len) {
                out_attr[i++] = ARKVIM_ATTR_COMMENT;
            }
            return;
        }

        /* block comment start */
        if (c == '/' && i + 1 < len && line[i + 1] == '*') {
            out_attr[i++] = ARKVIM_ATTR_COMMENT;
            out_attr[i++] = ARKVIM_ATTR_COMMENT;
            while (i < len) {
                if (line[i] == '*' && i + 1 < len && line[i + 1] == '/') {
                    out_attr[i++] = ARKVIM_ATTR_COMMENT;
                    out_attr[i++] = ARKVIM_ATTR_COMMENT;
                    *in_block = false;
                    break;
                }
                out_attr[i++] = ARKVIM_ATTR_COMMENT;
            }
            if (i >= len) {
                *in_block = true;   /* runs to next line */
            }
            continue;
        }

        /* string literal */
        if (c == '"') {
            out_attr[i++] = ARKVIM_ATTR_STRING;
            while (i < len) {
                if (line[i] == '\\' && i + 1 < len) {
                    out_attr[i++] = ARKVIM_ATTR_STRING;
                    out_attr[i++] = ARKVIM_ATTR_STRING;
                    continue;
                }
                if (line[i] == '"') {
                    out_attr[i++] = ARKVIM_ATTR_STRING;
                    break;
                }
                out_attr[i++] = ARKVIM_ATTR_STRING;
            }
            continue;
        }

        /* char literal */
        if (c == '\'') {
            out_attr[i++] = ARKVIM_ATTR_STRING;
            while (i < len) {
                if (line[i] == '\\' && i + 1 < len) {
                    out_attr[i++] = ARKVIM_ATTR_STRING;
                    out_attr[i++] = ARKVIM_ATTR_STRING;
                    continue;
                }
                if (line[i] == '\'') {
                    out_attr[i++] = ARKVIM_ATTR_STRING;
                    break;
                }
                out_attr[i++] = ARKVIM_ATTR_STRING;
            }
            continue;
        }

        /* preprocessor directive (#...) */
        if (c == '#') {
            while (i < len) {
                out_attr[i++] = ARKVIM_ATTR_KEYWORD;
            }
            return;
        }

        /* number */
        if (is_digit(c) || (c == '.' && i + 1 < len && is_digit(line[i + 1]))) {
            out_attr[i++] = ARKVIM_ATTR_NUMBER;
            while (i < len) {
                char h = line[i];
                if (is_digit(h) || h == '.' || h == 'x' || h == 'X' ||
                    (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F') ||
                    h == 'u' || h == 'U' || h == 'l' || h == 'L') {
                    out_attr[i++] = ARKVIM_ATTR_NUMBER;
                } else {
                    break;
                }
            }
            continue;
        }

        /* identifier / keyword */
        if (is_ident_start(c)) {
            int start = i;
            while (i < len && is_ident(line[i])) {
                i++;
            }
            {
                uint8_t a = is_keyword(&line[start], i - start)
                            ? ARKVIM_ATTR_KEYWORD : ARKVIM_ATTR_NORMAL;
                int k;
                for (k = start; k < i; k++) {
                    out_attr[k] = a;
                }
            }
            continue;
        }

        /* operators / punctuation */
        out_attr[i++] = ARKVIM_ATTR_OPERATOR;
    }
}
