/*
 * Checks that the test build and harness work.
 */

#include <stdint.h>

#include "test/test.h"


int main(void)
{
    CHECK(sizeof(uint64_t) == 8);
    CHECK(1 + 1 == 2);

    return test_result(__FILE__);
}
