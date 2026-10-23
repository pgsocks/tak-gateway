#include <gtest/gtest.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	testing::InitGoogleTest(&argc, argv);
	int result = RUN_ALL_TESTS();

	if (!GTEST_FLAG_GET(list_tests) &&
	    testing::UnitTest::GetInstance()->total_test_count() == 0) {
		fprintf(stderr, "Unit suite contains no cases. "
			"Add public API tests before running this suite.\n");
		return 1;
	}
	return result;
}
