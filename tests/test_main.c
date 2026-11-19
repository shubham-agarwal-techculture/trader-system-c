#include "testutil.h"

#include <stdio.h>

extern const test_case core_tests[];
extern const test_case oms_tests[];
extern const test_case rms_tests[];
extern const test_case ems_tests[];
extern const test_case journal_tests[];
extern const test_case pipeline_tests[];

int main(void)
{
    test_run_suite("core", core_tests);
    test_run_suite("oms", oms_tests);
    test_run_suite("rms", rms_tests);
    test_run_suite("ems", ems_tests);
    test_run_suite("journal", journal_tests);
    test_run_suite("pipeline", pipeline_tests);
    return test_summary();
}
