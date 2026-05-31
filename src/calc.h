#ifndef CALC_H
#define CALC_H

// Evaluate expression and return result
double calc_evaluate(const char *expr, char **error);

// Format result as string
char *calc_format_result(double result);

// Format result with error message
char *calc_format_result_with_error(double result, const char *error);

#endif // CALC_H
