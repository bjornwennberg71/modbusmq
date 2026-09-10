//////////////////////////////////////////////////////////////////////////////
//
// bjornwennberg71@gmail.com
//
// test_common.h
//
// Minimal assertion helpers shared by the test programs. No framework, no
// dependencies: a test program counts its checks, prints the ones that failed,
// and returns non-zero if any did. run_tests.sh aggregates the exit codes.
//
#ifndef test_common_h_
#define test_common_h_

#include <stdio.h>
#include <string.h>
#include <math.h>

static int tests_checked = 0;
static int tests_failed  = 0;

#define CHECK(cond, what)                                               \
    do {                                                                \
        tests_checked++;                                                \
        if (!(cond))                                                    \
        {                                                               \
            tests_failed++;                                             \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));   \
        }                                                               \
    } while(0)

#define CHECK_INT(got, want, what)                                      \
    do {                                                                \
        long g_ = (long)(got), w_ = (long)(want);                       \
        tests_checked++;                                                \
        if (g_ != w_)                                                   \
        {                                                               \
            tests_failed++;                                             \
            printf("  FAIL %s:%d: %s: got %ld, want %ld\n",             \
                   __FILE__, __LINE__, (what), g_, w_);                 \
        }                                                               \
    } while(0)

#define CHECK_FLT(got, want, eps, what)                                 \
    do {                                                                \
        double g_ = (double)(got), w_ = (double)(want);                 \
        tests_checked++;                                                \
        if (fabs(g_ - w_) > (eps))                                      \
        {                                                               \
            tests_failed++;                                             \
            printf("  FAIL %s:%d: %s: got %g, want %g\n",               \
                   __FILE__, __LINE__, (what), g_, w_);                 \
        }                                                               \
    } while(0)

#define CHECK_STR(got, want, what)                                      \
    do {                                                                \
        const char *g_ = (got), *w_ = (want);                           \
        tests_checked++;                                                \
        if (!g_ || !w_ || strcmp(g_, w_) != 0)                          \
        {                                                               \
            tests_failed++;                                             \
            printf("  FAIL %s:%d: %s: got \"%s\", want \"%s\"\n",       \
                   __FILE__, __LINE__, (what), g_ ? g_ : "(null)",      \
                   w_ ? w_ : "(null)");                                 \
        }                                                               \
    } while(0)

//
// @brief prints the tally and gives main() its exit code
//
// @return 0 when every check passed, 1 otherwise
//
static int
test_summary(const char *name)
{
    if (tests_failed == 0)
    {
        printf("  ok: %d checks passed [%s]\n", tests_checked, name);
        return 0;
    }

    printf("  FAILED: %d of %d checks [%s]\n", tests_failed, tests_checked, name);
    return 1;
}

#endif // test_common_h_
