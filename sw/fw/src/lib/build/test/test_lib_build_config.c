#include "lib_build_config.h"
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static bool isHex(char c)
{
    return (((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f')));
}

// [test->fw~obs_identity_001~1]
static void test_identity_format(void)
{
    const char * const id = LIB_BUILD_IDENTITY;
    const size_t len = strlen(id);

    // "cccccccccccc" (clean) or "cccccccccccc+dddddddd" (dirty tree).
    TEST_ASSERT_TRUE((len == 12U) || (len == 21U));
    for (size_t i = 0U; i < 12U; i++)
    {
        TEST_ASSERT_TRUE(isHex(id[i]));
    }
    if (len == 21U)
    {
        TEST_ASSERT_EQUAL_CHAR('+', id[12]);
        for (size_t i = 13U; i < 21U; i++)
        {
            TEST_ASSERT_TRUE(isHex(id[i]));
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_identity_format);
    return UNITY_END();
}
