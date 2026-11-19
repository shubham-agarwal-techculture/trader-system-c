#include "testutil.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

jmp_buf g_test_jmp;
int g_test_failures = 0;
const char *g_test_current = "";

void test_fail(const char *file, int line, const char *expr)
{
    fprintf(stderr, "FAIL %s:%d in %s: %s\n", file, line,
            g_test_current ? g_test_current : "?", expr);
    g_test_failures++;
    longjmp(g_test_jmp, 1);
}

void test_run_suite(const char *suite, const test_case *tests)
{
    printf("[%s]\n", suite);
    for (size_t i = 0; tests[i].name; ++i) {
        g_test_current = tests[i].name;
        if (setjmp(g_test_jmp) == 0) {
            tests[i].fn();
            printf("  PASS %s\n", tests[i].name);
        } else {
            printf("  FAIL %s\n", tests[i].name);
        }
    }
}

int test_summary(void)
{
    if (g_test_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d ASSERTION FAILURE(S)\n", g_test_failures);
    return 1;
}
