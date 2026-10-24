/*
 * Minimal test framework. Each test file is one executable:
 *
 *   #include "test.h"
 *   TEST(name) { CHECK(x); CHECK_EQ(a, b); }
 *
 * Tests register themselves from a constructor, so there is no list to keep
 * in sync. Run `./test_foo substring` to run only tests whose name matches.
 */
#ifndef TS_TEST_H
#define TS_TEST_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_MAX 256

typedef void (*test_fn)(void);

static struct {
    const char *name;
    test_fn fn;
} test_cases[TEST_MAX];
static int test_count;
static int test_failed; /* failures in the currently running test */

static void test_register(const char *name, test_fn fn) {
    if (test_count == TEST_MAX) {
        fprintf(stderr, "test.h: too many tests (raise TEST_MAX)\n");
        abort();
    }
    test_cases[test_count].name = name;
    test_cases[test_count].fn = fn;
    test_count++;
}

#define TEST(name)                                                            \
    static void test_##name(void);                                            \
    __attribute__((constructor)) static void test_register_##name(void) {     \
        test_register(#name, test_##name);                                    \
    }                                                                         \
    static void test_##name(void)

/* CHECK keeps going after a failure so one run reports everything wrong;
 * REQUIRE returns from the test (use it before dereferencing a result). */
#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__,          \
                    __LINE__, #cond);                                         \
            test_failed++;                                                    \
        }                                                                     \
    } while (0)

#define REQUIRE(cond)                                                         \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "  %s:%d: REQUIRE(%s) failed\n", __FILE__,        \
                    __LINE__, #cond);                                         \
            test_failed++;                                                    \
            return;                                                           \
        }                                                                     \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        intmax_t check_a_ = (intmax_t)(a), check_b_ = (intmax_t)(b);          \
        if (check_a_ != check_b_) {                                           \
            fprintf(stderr,                                                   \
                    "  %s:%d: CHECK_EQ(%s, %s) failed: %" PRIdMAX             \
                    " != %" PRIdMAX "\n",                                     \
                    __FILE__, __LINE__, #a, #b, check_a_, check_b_);          \
            test_failed++;                                                    \
        }                                                                     \
    } while (0)

int main(int argc, char **argv) {
    const char *filter = argc > 1 ? argv[1] : NULL;
    int ran = 0, failed = 0;
    for (int i = 0; i < test_count; i++) {
        if (filter && !strstr(test_cases[i].name, filter))
            continue;
        test_failed = 0;
        printf("[ RUN  ] %s\n", test_cases[i].name);
        fflush(stdout);
        test_cases[i].fn();
        printf("[ %s ] %s\n", test_failed ? "FAIL" : " OK ", test_cases[i].name);
        ran++;
        failed += test_failed != 0;
    }
    printf("%d/%d tests passed\n", ran - failed, ran);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

#endif /* TS_TEST_H */
