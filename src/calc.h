#ifndef CALC_H
#define CALC_H

// Evaluate expression and return result
double calc_evaluate(const char *expr, char **error);

// Format result as string
char *calc_format_result(double result);

#endif // CALC_H
