#include "calc.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

// Get next token
static Token lexer_next_token(Lexer *lex) {
    Token tok = {TOKEN_EOF, 0, NULL};
    
    lexer_skip_ws(lex);
    
    char c = lexer_peek(lex);
    if (c == '\0') {
        tok.type = TOKEN_EOF;
        return tok;
    }
    
    // Number (including hex 0x, octal 0o, binary 0b)
    if (c == '0' && (lex->expr[lex->pos + 1] == 'x' || lex->expr[lex->pos + 1] == 'X' ||
                      lex->expr[lex->pos + 1] == 'o' || lex->expr[lex->pos + 1] == 'O' ||
                      lex->expr[lex->pos + 1] == 'b' || lex->expr[lex->pos + 1] == 'B')) {
        tok.type = TOKEN_NUMBER;
        lex->pos += 2;  // skip 0x/0o/0b
        double value = 0;
        char base = lex->expr[lex->pos - 1];
        base = (base >= 'a' && base <= 'z') ? base - 32 : base;  // uppercase
        
        if (base == 'X') {
            // Hexadecimal
            while (lex->pos < lex->len) {
                char c2 = lex->expr[lex->pos];
                if (c2 >= '0' && c2 <= '9') { value = value * 16 + (c2 - '0'); lex->pos++; }
                else if (c2 >= 'a' && c2 <= 'f') { value = value * 16 + (c2 - 'a' + 10); lex->pos++; }
                else if (c2 >= 'A' && c2 <= 'F') { value = value * 16 + (c2 - 'A' + 10); lex->pos++; }
                else break;
            }
        } else if (base == 'O') {
            // Octal
            while (lex->pos < lex->len && lex->expr[lex->pos] >= '0' && lex->expr[lex->pos] <= '7') {
                value = value * 8 + (lex->expr[lex->pos] - '0');
                lex->pos++;
            }
        } else if (base == 'B') {
            // Binary
            while (lex->pos < lex->len && (lex->expr[lex->pos] == '0' || lex->expr[lex->pos] == '1')) {
                value = value * 2 + (lex->expr[lex->pos] - '0');
                lex->pos++;
            }
        }
        tok.number = value;
        return tok;
    }
    
    // Regular number (may have % suffix for percentage)
    if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)lex->expr[lex->pos + 1]))) {
        tok.type = TOKEN_NUMBER;
        char *end;
        tok.number = strtod(lex->expr + lex->pos, &end);
        lex->pos = end - lex->expr;
        // Handle percentage suffix -- only if not a modulo operator.
        // Distinguish: 50% (percentage) vs 10%3 (modulo) vs 25%+25% (percentage)
        while (lex->pos < lex->len && isspace((unsigned char)lex->expr[lex->pos])) lex->pos++;
        if (lex->pos < lex->len && lex->expr[lex->pos] == '%') {
            size_t peek = lex->pos + 1;
            while (peek < lex->len && isspace((unsigned char)lex->expr[peek])) peek++;
            char next = (peek < lex->len) ? lex->expr[peek] : '\0';

            int is_percentage = 0;
            if (next == '\0') {
                is_percentage = 1;  // EOF
            } else if (next == '+' || next == '-' || next == '*' || next == '/' ||
                       next == '^' || next == ')' || next == ',') {
                is_percentage = 1;  // followed by operator
            }
            // Otherwise it's modulo (digit, '(', letter) -- don't consume '%'

            if (is_percentage) {
                lex->pos++;  // consume '%'
                tok.number /= 100.0;
            }
        }
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
    Lexer lex;
    Token current;
} Parser;

// Init parser
static void parser_init(Parser *p, const char *expr) {
    lexer_init(&p->lex, expr);
    p->current = lexer_next_token(&p->lex);
}

// Advance to next token
static void parser_next(Parser *p) {
    token_free(&p->current);
    p->current = lexer_next_token(&p->lex);
}

// Get variable value
static double get_var(const char *name) {
    if (strcmp(name, "pi") == 0) return M_PI;
    if (strcmp(name, "e") == 0) return 2.718281828459045;
    if (strcmp(name, "phi") == 0) return 1.618033988749895;
    if (strcmp(name, "tau") == 0) return 2 * M_PI;
    if (strcmp(name, "inf") == 0) return INFINITY;
    if (strcmp(name, "nan") == 0) return NAN;
    return 0;
}

// Evaluate function
static double eval_func(const char *name, double *args, int n) {
    
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
        int64_t ni = (int64_t)args[0];
        if (ni < 0) return NAN;
        if (ni == 0 || ni == 1) return 1;
        double result = 1;
        for (int64_t i = 2; i <= ni; i++) result *= i;
        return result;
    }
    
    // Degree conversion
    if (strcmp(name, "deg") == 0 && n >= 1) return args[0] * 180 / M_PI;
    if (strcmp(name, "rad") == 0 && n >= 1) return args[0] * M_PI / 180;
    
    return NAN;
}

// Forward declarations for parsing functions
static double parse_expr(Parser *p);
static double parse_factor(Parser *p);
static double parse_power(Parser *p);
static double parse_unary(Parser *p);
static double parse_primary(Parser *p);

// Parse expression: + -
static double parse_expr(Parser *p) {
    double left = parse_factor(p);
    
    while (p->current.type == TOKEN_PLUS || p->current.type == TOKEN_MINUS) {
        int op = p->current.type;
        parser_next(p);
        double right = parse_factor(p);
        if (op == TOKEN_PLUS) left += right;
        else left -= right;
    }
    
    return left;
}

// Parse factor: * / % and implicit multiplication
static double parse_factor(Parser *p) {
    double left = parse_power(p);

    while (1) {
        // Handle factorial operator (!)
        // Note: We detect '!' by checking the char directly since it's not a token
        while (lexer_peek(&p->lex) == '!') {
            p->lex.pos++;  // consume '!'
            // Compute factorial
            int64_t n = (int64_t)left;
            if (n < 0) { left = NAN; break; }
            if (n == 0 || n == 1) { left = 1; }
            else {
                double result = 1;
                for (int64_t i = 2; i <= n; i++) result *= i;
                left = result;
            }
        }
        
        // Explicit multiplication/division/modulo
        if (p->current.type == TOKEN_MUL) {
            parser_next(p);
            left *= parse_power(p);
        } else if (p->current.type == TOKEN_DIV) {
            parser_next(p);
            double right = parse_power(p);
            if (right == 0) return NAN;
            left /= right;
        } else if (p->current.type == TOKEN_MOD) {
            parser_next(p);
            double right = parse_power(p);
            if (right == 0) { left = NAN; break; }
            // Python-style modulo: always has same sign as divisor
            left = fmod(left, right);
            if (left < 0) left += right;
        }
        // Implicit multiplication: number followed by ( or identifier
        else if (p->current.type == TOKEN_LPAREN) {
            parser_next(p);
            double right = parse_expr(p);
            if (p->current.type != TOKEN_RPAREN) return NAN;
            parser_next(p);
            left *= right;
        }
        // Implicit multiplication: number followed by identifier (e.g., 2pi)
        else if (p->current.type == TOKEN_IDENT) {
            char *name = strdup(p->current.ident);
            parser_next(p);
            double right;
            
            // Check if this is a function or variable
            if (p->current.type == TOKEN_LPAREN) {
                
                // If eval_func returns 0 with 0 args, it's not a function (or is a function that returns 0)
                // We need a better way to check if it's a function...
                // For now, check if it's a known variable first
                double var_val = get_var(name);
                int is_variable = (strcmp(name, "pi") == 0 || strcmp(name, "e") == 0 || 
                                   strcmp(name, "phi") == 0 || strcmp(name, "tau") == 0 ||
                                   strcmp(name, "inf") == 0 || strcmp(name, "nan") == 0);
                
                if (is_variable) {
                    // It's a variable followed by (expr), treat as implicit multiplication
                    // First consume the (
                    parser_next(p);
                    double mul_expr = parse_expr(p);
                    if (p->current.type != TOKEN_RPAREN) {
                        free(name);
                        return NAN;
                    }
                    parser_next(p);  // consume )
                    right = var_val * mul_expr;
                } else {
                    // It's a function call
                    parser_next(p);
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
                    parser_next(p);
                    right = eval_func(name, args, n);
                }
            } else {
                // Simple variable
                right = get_var(name);
            }
            free(name);
            left *= right;
        }
        // Implicit multiplication: closing paren followed by ( or identifier
        else {
            break;
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

// Parse unary: - + 
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
            // Check if it's a known variable (pi, e, phi, tau) followed by implicit multiplication
            double var_val = get_var(name);
            int is_known_var = (strcmp(name, "pi") == 0 || strcmp(name, "e") == 0 || 
                               strcmp(name, "phi") == 0 || strcmp(name, "tau") == 0 ||
                               strcmp(name, "inf") == 0 || strcmp(name, "nan") == 0);
            
            if (is_known_var) {
                // pi(3) means pi * (3), not a function call
                // Consume the (
                parser_next(p);
                double mul_expr = parse_expr(p);
                if (p->current.type != TOKEN_RPAREN) {
                    free(name);
                    return NAN;
                }
                parser_next(p);  // consume )
                result = var_val * mul_expr;
            } else {
                // It's a function call
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
                
                result = eval_func(name, args, n);
            }
        } else {
            // Variable
            result = get_var(name);
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
    
    return NAN;
}

// Main evaluation function
double calc_evaluate(const char *expr, char **error) {
    if (!expr || !*expr) {
        if (error) *error = strdup("empty expression");
        return NAN;
    }
    
    Parser parser;

    parser_init(&parser, expr);
    
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

