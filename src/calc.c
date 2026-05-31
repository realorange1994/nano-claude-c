#include "calc.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

// Token types
typedef enum {
    TOKEN_NUMBER,
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_MUL,
    TOKEN_DIV,
    TOKEN_MOD,
    TOKEN_POW,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_COMMA,
    TOKEN_IDENT,
    TOKEN_EOF
} TokenType;

// Token
typedef struct {
    TokenType type;
    double number;
    char *ident;
} Token;

// Lexer state
typedef struct {
    const char *expr;
    size_t pos;
    size_t len;
} Lexer;

// Context for variables and functions
typedef struct {
    double vars[8];           // pi, e, phi, tau, inf, nan, 2 unused
    const char *var_names[8];
} CalcContext;

// Function pointer type
typedef double (*CalcFunc)(double *args, int n);

// Built-in functions
typedef struct {
    const char *name;
    CalcFunc func;
    int min_args;
    int max_args;
} CalcBuiltinFunc;

// Create lexer
static void lexer_init(Lexer *lex, const char *expr) {
    lex->expr = expr;
    lex->pos = 0;
    lex->len = strlen(expr);
}

// Skip whitespace
static void lexer_skip_ws(Lexer *lex) {
    while (lex->pos < lex->len && isspace((unsigned char)lex->expr[lex->pos])) {
        lex->pos++;
    }
}

// Peek current char
static char lexer_peek(Lexer *lex) {
    if (lex->pos >= lex->len) return '\0';
    return lex->expr[lex->pos];
}

// Consume current char
static char lexer_next(Lexer *lex) {
    if (lex->pos >= lex->len) return '\0';
    return lex->expr[lex->pos++];
}

// Get next token
static Token lexer_next_token(Lexer *lex) {
    Token tok = {TOKEN_EOF, 0, NULL};
    
    lexer_skip_ws(lex);
    
    char c = lexer_peek(lex);
    if (c == '\0') {
        tok.type = TOKEN_EOF;
        return tok;
    }
    
    // Number
    if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)lex->expr[lex->pos + 1]))) {
        tok.type = TOKEN_NUMBER;
        char *end;
        tok.number = strtod(lex->expr + lex->pos, &end);
        lex->pos = end - lex->expr;
        return tok;
    }
    
    // Identifier or keyword
    if (isalpha((unsigned char)c) || c == '_') {
        tok.type = TOKEN_IDENT;
        size_t start = lex->pos;
        while (lex->pos < lex->len && (isalnum((unsigned char)lex->expr[lex->pos]) || lex->expr[lex->pos] == '_')) {
            lex->pos++;
        }
        size_t len = lex->pos - start;
        tok.ident = malloc(len + 1);
        strncpy(tok.ident, lex->expr + start, len);
        tok.ident[len] = '\0';
        return tok;
    }
    
    // Operators
    switch (c) {
        case '+': tok.type = TOKEN_PLUS; lex->pos++; return tok;
        case '-': tok.type = TOKEN_MINUS; lex->pos++; return tok;
        case '*': tok.type = TOKEN_MUL; lex->pos++; return tok;
        case '/': tok.type = TOKEN_DIV; lex->pos++; return tok;
        case '%': tok.type = TOKEN_MOD; lex->pos++; return tok;
        case '^': tok.type = TOKEN_POW; lex->pos++; return tok;
        case '(': tok.type = TOKEN_LPAREN; lex->pos++; return tok;
        case ')': tok.type = TOKEN_RPAREN; lex->pos++; return tok;
        case ',': tok.type = TOKEN_COMMA; lex->pos++; return tok;
    }
    
    // Unknown char - skip it
    lex->pos++;
    return lexer_next_token(lex);
}

// Free token
static void token_free(Token *tok) {
    if (tok->ident) {
        free(tok->ident);
        tok->ident = NULL;
    }
}

// Parser state
typedef struct {
    Lexer *lex;
    Token current;
    CalcContext *ctx;
} Parser;

// Init parser
static void parser_init(Parser *p, const char *expr, CalcContext *ctx) {
    lexer_init(&p->lex, expr);
    p->ctx = ctx;
    p->current = lexer_next_token(&p->lex);
}

// Advance to next token
static void parser_next(Parser *p) {
    token_free(&p->current);
    p->current = lexer_next_token(&p->lex);
}

// Get variable value
static double get_var(const char *name, CalcContext *ctx) {
    if (strcmp(name, "pi") == 0) return 3.141592653589793;
    if (strcmp(name, "e") == 0) return 2.718281828459045;
    if (strcmp(name, "phi") == 0) return 1.618033988749895;
    if (strcmp(name, "tau") == 0) return 6.283185307179586;
    if (strcmp(name, "inf") == 0) return INFINITY;
    if (strcmp(name, "nan") == 0) return NAN;
    return 0;
}

// Evaluate function
static double eval_func(const char *name, double *args, int n, CalcContext *ctx) {
    (void)ctx;  // unused
    
    // Trigonometric
    if (strcmp(name, "sin") == 0 && n >= 1) return sin(args[0]);
    if (strcmp(name, "cos") == 0 && n >= 1) return cos(args[0]);
    if (strcmp(name, "tan") == 0 && n >= 1) return tan(args[0]);
    if (strcmp(name, "asin") == 0 && n >= 1) return asin(args[0]);
    if (strcmp(name, "acos") == 0 && n >= 1) return acos(args[0]);
    if (strcmp(name, "atan") == 0 && n >= 1) return atan(args[0]);
    if (strcmp(name, "atan2") == 0 && n >= 2) return atan2(args[0], args[1]);
    
    // Hyperbolic
    if (strcmp(name, "sinh") == 0 && n >= 1) return sinh(args[0]);
    if (strcmp(name, "cosh") == 0 && n >= 1) return cosh(args[0]);
    if (strcmp(name, "tanh") == 0 && n >= 1) return tanh(args[0]);
    if (strcmp(name, "asinh") == 0 && n >= 1) return asinh(args[0]);
    if (strcmp(name, "acosh") == 0 && n >= 1) return acosh(args[0]);
    if (strcmp(name, "atanh") == 0 && n >= 1) return atanh(args[0]);
    
    // Root and power
    if (strcmp(name, "sqrt") == 0 && n >= 1) return sqrt(args[0]);
    if (strcmp(name, "cbrt") == 0 && n >= 1) return cbrt(args[0]);
    if (strcmp(name, "pow") == 0 && n >= 2) return pow(args[0], args[1]);
    
    // Logarithmic
    if (strcmp(name, "log") == 0) {
        if (n == 1) return log(args[0]);
        if (n >= 2) return log(args[0]) / log(args[1]);
    }
    if (strcmp(name, "ln") == 0 && n >= 1) return log(args[0]);
    if (strcmp(name, "log2") == 0 && n >= 1) return log2(args[0]);
    if (strcmp(name, "log10") == 0 && n >= 1) return log10(args[0]);
    
    // Exponential and absolute
    if (strcmp(name, "exp") == 0 && n >= 1) return exp(args[0]);
    if (strcmp(name, "abs") == 0 && n >= 1) return fabs(args[0]);
    
    // Rounding
    if (strcmp(name, "floor") == 0 && n >= 1) return floor(args[0]);
    if (strcmp(name, "ceil") == 0 && n >= 1) return ceil(args[0]);
    if (strcmp(name, "round") == 0 && n >= 1) return round(args[0]);
    
    // Min and Max
    if (strcmp(name, "min") == 0 && n >= 1) {
        double m = args[0];
        for (int i = 1; i < n; i++) if (args[i] < m) m = args[i];
        return m;
    }
    if (strcmp(name, "max") == 0 && n >= 1) {
        double m = args[0];
        for (int i = 1; i < n; i++) if (args[i] > m) m = args[i];
        return m;
    }
    
    // Utility
    if (strcmp(name, "sign") == 0 && n >= 1) {
        if (args[0] > 0) return 1;
        if (args[0] < 0) return -1;
        return 0;
    }
    
    if (strcmp(name, "fact") == 0 && n >= 1) {
        int64_t n = (int64_t)args[0];
        if (n < 0) return NAN;
        if (n == 0 || n == 1) return 1;
        double result = 1;
        for (int64_t i = 2; i <= n; i++) result *= i;
        return result;
    }
    
    // Degree conversion
    if (strcmp(name, "deg") == 0 && n >= 1) return args[0] * 180 / M_PI;
    if (strcmp(name, "rad") == 0 && n >= 1) return args[0] * M_PI / 180;
    
    return NAN;
}

// Forward declarations for parsing functions
static double parse_expr(Parser *p);
static double parse_term(Parser *p);
static double parse_power(Parser *p);
static double parse_unary(Parser *p);
static double parse_primary(Parser *p);

// Parse expression: + -
static double parse_expr(Parser *p) {
    double left = parse_term(p);
    
    while (p->current.type == TOKEN_PLUS || p->current.type == TOKEN_MINUS) {
        int op = p->current.type;
        parser_next(p);
        double right = parse_term(p);
        if (op == TOKEN_PLUS) left += right;
        else left -= right;
    }
    
    return left;
}

// Parse term: * / %
static double parse_term(Parser *p) {
    double left = parse_power(p);
    
    while (p->current.type == TOKEN_MUL || p->current.type == TOKEN_DIV || 
           p->current.type == TOKEN_MOD) {
        int op = p->current.type;
        parser_next(p);
        double right = parse_power(p);
        if (op == TOKEN_MUL) left *= right;
        else if (op == TOKEN_DIV) {
            if (right == 0) return NAN;
            left /= right;
        } else {
            left = fmod(left, right);
        }
    }
    
    return left;
}

// Parse power: ^ (right associative)
static double parse_power(Parser *p) {
    double base = parse_unary(p);
    
    if (p->current.type == TOKEN_POW) {
        parser_next(p);
        double exp = parse_power(p);  // Right associative
        return pow(base, exp);
    }
    
    return base;
}

// Parse unary: - + !
static double parse_unary(Parser *p) {
    if (p->current.type == TOKEN_MINUS) {
        parser_next(p);
        return -parse_unary(p);
    }
    if (p->current.type == TOKEN_PLUS) {
        parser_next(p);
        return parse_unary(p);
    }
    
    return parse_primary(p);
}

// Parse primary: number, identifier, parenthesized expression, function call
static double parse_primary(Parser *p) {
    TokenType type = p->current.type;
    
    // Number
    if (type == TOKEN_NUMBER) {
        double val = p->current.number;
        parser_next(p);
        return val;
    }
    
    // Identifier or function call
    if (type == TOKEN_IDENT) {
        char *name = strdup(p->current.ident);
        parser_next(p);
        
        double result;
        
        // Check for function call
        if (p->current.type == TOKEN_LPAREN) {
            parser_next(p);  // consume (
            
            // Parse arguments
            double args[16];
            int n = 0;
            
            if (p->current.type != TOKEN_RPAREN) {
                args[n++] = parse_expr(p);
                while (p->current.type == TOKEN_COMMA && n < 16) {
                    parser_next(p);
                    args[n++] = parse_expr(p);
                }
            }
            
            if (p->current.type != TOKEN_RPAREN) {
                free(name);
                return NAN;
            }
            parser_next(p);  // consume )
            
            result = eval_func(name, args, n, p->ctx);
        } else {
            // Variable
            result = get_var(name, p->ctx);
        }
        
        free(name);
        return result;
    }
    
    // Parenthesized expression
    if (type == TOKEN_LPAREN) {
        parser_next(p);
        double val = parse_expr(p);
        if (p->current.type != TOKEN_RPAREN) {
            return NAN;
        }
        parser_next(p);
        return val;
    }
    
    // Implicit multiplication: 2pi, 3(4)
    if (type == TOKEN_EOF || type == TOKEN_RPAREN) {
        return 1;  // For implicit multiplication at end
    }
    
    return NAN;
}

// Main evaluation function
double calc_evaluate(const char *expr, char **error) {
    if (!expr || !*expr) {
        if (error) *error = strdup("empty expression");
        return NAN;
    }
    
    CalcContext ctx;
    Parser parser;
    
    parser_init(&parser, expr, &ctx);
    
    double result = parse_expr(&parser);
    
    // Check for trailing tokens (syntax error)
    if (parser.current.type != TOKEN_EOF) {
        token_free(&parser.current);
        if (error) *error = strdup("unexpected token");
        return NAN;
    }
    
    token_free(&parser.current);
    
    return result;
}

// Format result as string
char *calc_format_result(double result) {
    char *str = malloc(64);
    if (!str) return NULL;
    
    if (isnan(result)) {
        strcpy(str, "NaN");
    } else if (isinf(result)) {
        strcpy(str, result > 0 ? "Infinity" : "-Infinity");
    } else {
        // Use high precision, but trim trailing zeros
        snprintf(str, 64, "%.15g", result);
    }
    
    return str;
}

// Format result with error
char *calc_format_result_with_error(double result, const char *error) {
    char *str;
    if (error) {
        str = malloc(strlen(error) + 64);
        if (str) {
            char *result_str = calc_format_result(result);
            snprintf(str, strlen(error) + 64, "%s (result: %s)", error, result_str);
            free(result_str);
        }
    } else {
        str = calc_format_result(result);
    }
    return str;
}
