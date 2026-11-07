/* Ark C Compiler - ast
 * AST node construction and destruction.
 */
#include "ast.h"
#include "memory.h"

AstNode *ast_new(AstNodeKind kind, int line) {
    AstNode *n = (AstNode *)mem_pool_alloc(sizeof(AstNode));
    n->kind = kind;
    n->type = 0;
    n->line = line;
    n->col = 0;
    n->fval = 0.0;
    return n;
}

AstNode *ast_num(long long val, int line) {
    AstNode *n = ast_new(AST_NUM, line);
    n->type = type_int();
    n->u.num_val = val;
    return n;
}

AstNode *ast_str(const char *content, int line) {
    AstNode *n = ast_new(AST_STR, line);
    n->type = type_pointer(type_char());
    n->u.name = mem_strdup(content ? content : "");
    return n;
}

AstNode *ast_var(const char *name, int line) {
    AstNode *n = ast_new(AST_VAR, line);
    n->u.name = mem_strdup(name);
    return n;
}

AstNode *ast_binary(int op, AstNode *left, AstNode *right, int line) {
    AstNode *n = ast_new(AST_BINARY, line);
    n->u.binary.op = op;
    n->u.binary.left = left;
    n->u.binary.right = right;
    return n;
}

AstNode *ast_unary(int op, AstNode *operand, int line) {
    AstNode *n = ast_new(AST_UNARY, line);
    n->u.unary.op = op;
    n->u.unary.operand = operand;
    n->u.unary.is_postfix = 0;
    return n;
}

AstNode *ast_assign(int op, AstNode *left, AstNode *right, int line) {
    AstNode *n = ast_new(AST_ASSIGN, line);
    n->u.binary.op = op;
    n->u.binary.left = left;
    n->u.binary.right = right;
    return n;
}

AstNode *ast_if(AstNode *cond, AstNode *then_stmt, AstNode *else_stmt, int line) {
    AstNode *n = ast_new(AST_IF, line);
    n->u.if_stmt.cond = cond;
    n->u.if_stmt.then_stmt = then_stmt;
    n->u.if_stmt.else_stmt = else_stmt;
    return n;
}

AstNode *ast_while(AstNode *cond, AstNode *body, int line) {
    AstNode *n = ast_new(AST_WHILE, line);
    n->u.while_stmt.cond = cond;
    n->u.while_stmt.body = body;
    return n;
}

AstNode *ast_for(AstNode *init, AstNode *cond, AstNode *step, AstNode *body, int line) {
    AstNode *n = ast_new(AST_FOR, line);
    n->u.for_stmt.init = init;
    n->u.for_stmt.cond = cond;
    n->u.for_stmt.step = step;
    n->u.for_stmt.body = body;
    return n;
}

AstNode *ast_block(int line) {
    AstNode *n = ast_new(AST_BLOCK, line);
    n->u.block.stmts = 0;
    n->u.block.stmt_count = 0;
    n->u.block.stmt_capacity = 0;
    n->u.block.is_decl_list = 0;
    return n;
}

void block_add_stmt(AstNode *block, AstNode *stmt) {
    int count = block->u.block.stmt_count;
    if (count >= block->u.block.stmt_capacity) {
        int nc = block->u.block.stmt_capacity ? block->u.block.stmt_capacity * 2 : 8;
        block->u.block.stmts = (AstNode **)mem_realloc(
            block->u.block.stmts,
            block->u.block.stmt_capacity * (int)sizeof(AstNode *),
            nc * (int)sizeof(AstNode *));
        block->u.block.stmt_capacity = nc;
    }
    block->u.block.stmts[count] = stmt;
    block->u.block.stmt_count = count + 1;
}

AstNode *ast_func_def(char *name, Type *ret_type, AstNode **params, int param_count, AstNode *body, int line) {
    AstNode *n = ast_new(AST_FUNC_DEF, line);
    n->u.func.name = name;
    n->u.func.ret_type = ret_type;
    n->u.func.params = params;
    n->u.func.param_count = param_count;
    n->u.func.body = body;
    n->u.func.is_external = 0;
    n->u.func.is_static = 0;
    n->u.func.is_variadic = 0;
    /* Negative sentinels per include/ast.h.  The pool is zero-filled, and 0
     * is a *valid* __smc/__global_reg number: a constructor that leaves these
     * alone silently claims "this is SMC #0" / "lives in r0".  Every caller in
     * parser.c overwrites them, but the block-scope prototype added for
     * C99 6.2.2p5 did not, and src/compiler.c's "extern int f(void);"
     * consequently compiled into SMC #0. */
    n->u.func.smc_num = -1;
    n->u.func.global_reg = -1;
    return n;
}

AstNode *ast_var_decl(char *name, Type *var_type, AstNode *init, int line) {
    AstNode *n = ast_new(AST_VAR_DECL, line);
    n->u.var.name = name;
    n->u.var.var_type = var_type;
    n->u.var.init = init;
    n->u.var.is_global = 0;
    n->u.var.is_static = 0;
    n->u.var.is_extern = 0;
    return n;
}

AstNode *ast_func_call(char *name, AstNode **args, int arg_count, int line) {
    AstNode *n = ast_new(AST_FUNC_CALL, line);
    n->u.call.name = name;
    n->u.call.args = args;
    n->u.call.arg_count = arg_count;
    n->u.call.callee = 0;
    return n;
}

AstNode *ast_array_access(AstNode *array, AstNode *index, int line) {
    AstNode *n = ast_new(AST_ARRAY_ACCESS, line);
    n->u.array_access.array = array;
    n->u.array_access.index = index;
    return n;
}

AstNode *ast_member(AstNode *obj, const char *member, int is_arrow, int line) {
    AstNode *n = ast_new(AST_MEMBER, line);
    n->u.member.obj = obj;
    n->u.member.member = mem_strdup(member);
    n->u.member.is_arrow = is_arrow;
    n->u.member.offset = 0;
    return n;
}

AstNode *ast_cast(Type *target, AstNode *expr, int line) {
    AstNode *n = ast_new(AST_CAST, line);
    n->u.unary.operand = expr;
    n->u.unary.op = 0;
    n->type = target;
    return n;
}

AstNode *ast_sizeof_val(int bytes, int line) {
    AstNode *n = ast_new(AST_SIZEOF, line);
    n->u.num_val = bytes;
    n->type = type_int();
    return n;
}
