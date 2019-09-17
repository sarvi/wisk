#include "torture.h"

#include <cmocka.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int setup(void **state)
{
	torture_setup_socket_dir(state);

	return 0;
}

static int teardown(void **state)
{
	torture_teardown_socket_dir(state);

	return 0;
}

static void test_fcntl_dupfd_existing_open_fd(void **state)
{
	int s, dup_s;

	(void) state; /* unused */

	s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	assert_return_code(s, errno);

	dup_s = fcntl(s, F_DUPFD, 100);
	assert_int_equal(dup_s, 100);

	close(s);
	close(dup_s);
}

static void test_fcntl_getfd_existing_open_fd(void **state)
{
	int s, rc, flags;

	(void) state; /* unused */

	s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	assert_return_code(s, errno);

	rc = fcntl(s, F_SETFD, FD_CLOEXEC);
	assert_int_equal(rc, 0);

	flags = fcntl(s, F_GETFD);
	assert_int_equal(flags, FD_CLOEXEC);

	close(s);
}

int main(void) {
	int rc;

	const struct CMUnitTest tcp_fcntl_dupfd_tests[] = {
		cmocka_unit_test(test_fcntl_dupfd_existing_open_fd),
		cmocka_unit_test(test_fcntl_getfd_existing_open_fd),
	};

	rc = cmocka_run_group_tests(tcp_fcntl_dupfd_tests, setup, teardown);

	return rc;
}
