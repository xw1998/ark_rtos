/* Ark C Compiler - parser.h */
#ifndef ARK_PARSER_H
#define ARK_PARSER_H

#include "common.h"
#include "lexer.h"
#include "ast.h"

typedef struct {
    Lexer *lex;
    int    error_count;
    /* Where the declarator's name token was. parse_declarator() records it so
     * that declaration nodes can carry a column for diagnostics (a caret has
     * to land under the identifier armcc points at), without threading a new
     * out-parameter through every caller. 0 = this declarator had no name. */
    int    name_line;
    int    name_col;
} Parser;

AstNode *parse_program(Lexer *lex);

#endif /* ARK_PARSER_H */
