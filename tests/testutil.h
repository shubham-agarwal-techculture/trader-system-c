#ifndef TESTUTIL_H
#define TESTUTIL_H

#include "trading/core.h"

#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*test_fn)(void);

typedef struct {
    const char *name;
    test_fn fn;
} test_case;

/* Each test_*.c file exports `const test_case <suite>[]` terminated by
 * {NULL, NULL}. test_main aggregates them. */

extern jmp_buf g_test_jmp;
extern int g_test_failures;
extern const char *g_test_current;

void test_fail(const char *file, int line, const char *expr);
void test_run_suite(const char *suite, const test_case *tests);
int test_summary(void);

#define ASSERT(cond)                                                        \
    do {                                                                    \
        if (!(cond)) {                                                      \
            test_fail(__FILE__, __LINE__, #cond);                           \
        }                                                                   \
    } while (0)

#define ASSERT_EQ_I(a, b)                                                   \
    do {                                                                    \
        long long _a = (long long)(a), _b = (long long)(b);                 \
        if (_a != _b) {                                                     \
            fprintf(stderr, "  expected %lld got %lld\n", _b, _a);          \
            test_fail(__FILE__, __LINE__, #a " == " #b);                    \
        }                                                                   \
    } while (0)

#define ASSERT_EQ_D(a, b, eps)                                              \
    do {                                                                    \
        double _a = (double)(a), _b = (double)(b);                          \
        double _d = _a - _b; _d = _d < 0 ? -_d : _d;                        \
        if (_d > (eps)) {                                                   \
            fprintf(stderr, "  expected %.10g got %.10g\n", _b, _a);        \
            test_fail(__FILE__, __LINE__, #a " ~= " #b);                    \
        }                                                                   \
    } while (0)

#define ASSERT_STR(a, b)                                                    \
    do {                                                                    \
        const char *_a = (a), *_b = (b);                                    \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                             \
            fprintf(stderr, "  expected '%s' got '%s'\n",                   \
                    _b ? _b : "(null)", _a ? _a : "(null)");                \
            test_fail(__FILE__, __LINE__, #a " == " #b);                    \
        }                                                                   \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* TESTUTIL_H */
