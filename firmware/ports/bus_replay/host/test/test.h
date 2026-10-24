/*
 * A minimal test harness. CHECK() records a failure with its location and carries on;
 * test_result() is the program's exit code: 0 if every check passed.
 */

#ifndef TEST_H
#define TEST_H

#include <ftw.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>


static unsigned int test_checks;
static unsigned int test_failures;
static char test_tmp[512];

#define CHECK(cond) test_check((cond), __FILE__, __LINE__, #cond)


static inline void test_check(bool ok, const char *file, int line, const char *cond)
{
    test_checks++;
    if (!ok) {
        test_failures++;
        fprintf(stderr, "%s:%d: check failed: %s\n", file, line, cond);
    }
}


/*
 * A new, empty directory for the test's files, under $TMPDIR or /tmp. test_result() removes
 * it if every check passed, and keeps it otherwise.
 */
static inline const char *test_dir(void)
{
    const char *tmp = getenv("TMPDIR");

    snprintf(test_tmp, sizeof(test_tmp), "%s/bus-replay-test-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(test_tmp)) {
        perror(test_tmp);
        exit(1);
    }

    return test_tmp;
}


// The binary under test: $BUS_REPLAY (set by make test), else build/bus-replay.
static inline const char *test_bin(void)
{
    const char *bin = getenv("BUS_REPLAY");

    return bin && *bin ? bin : "build/bus-replay";
}


static inline int test_remove(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;

    return remove(path);
}


static inline int test_result(const char *file)
{
    fprintf(stderr, "%s: %u checks, %u failed\n", file, test_checks, test_failures);
    if (test_tmp[0] && test_failures) {
        fprintf(stderr, "%s: files kept in %s\n", file, test_tmp);
    } else if (test_tmp[0]) {
        nftw(test_tmp, test_remove, 16, FTW_DEPTH | FTW_PHYS);
    }

    return test_failures ? 1 : 0;
}

#endif
