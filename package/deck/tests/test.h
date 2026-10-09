// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * test.h - the assert kit the host tests share. One test binary per file, so
 * the counters can be file-static.
 */
#ifndef EP122_TESTS_TEST_H
#define EP122_TESTS_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int         t_fail;
static int         t_checks;
static const char *t_case = "";

#define T_CASE(name) (t_case = (name))

#define T_FAILED(fmt, ...) \
    (t_fail++, printf("  FAIL %s:%d [%s] " fmt "\n", __FILE__, __LINE__, t_case, __VA_ARGS__))

#define CHECK(cond) do { \
        t_checks++; \
        if (!(cond)) T_FAILED("%s", #cond); \
    } while (0)

#define CHECK_INT(got, want) do { \
        long g_ = (long)(got), w_ = (long)(want); \
        t_checks++; \
        if (g_ != w_) T_FAILED("%s: got %ld, want %ld", #got, g_, w_); \
    } while (0)

#define CHECK_U32(got, want) do { \
        unsigned long g_ = (unsigned long)(got), w_ = (unsigned long)(want); \
        t_checks++; \
        if (g_ != w_) T_FAILED("%s: got 0x%08lx, want 0x%08lx", #got, g_, w_); \
    } while (0)

/* Doubles compare within a tolerance; exact cases pass 0. */
#define CHECK_NEAR(got, want, tol) do { \
        double g_ = (got), w_ = (want), d_ = g_ - w_; \
        t_checks++; \
        if (d_ < 0) d_ = -d_; \
        if (!(d_ <= (tol))) \
            T_FAILED("%s: got %.9f, want %.9f", #got, g_, w_); \
    } while (0)

#define CHECK_STR(got, want) do { \
        const char *g_ = (got), *w_ = (want); \
        t_checks++; \
        if (g_ == NULL || w_ == NULL || strcmp(g_, w_) != 0) \
            T_FAILED("%s: got \"%s\", want \"%s\"", #got, g_ ? g_ : "(null)", w_ ? w_ : "(null)"); \
    } while (0)

static int t_done(const char *suite)
{
    printf("%-16s %7d checks  %s\n", suite, t_checks, t_fail ? "FAILED" : "ok");
    return t_fail != 0;
}

#endif /* EP122_TESTS_TEST_H */
