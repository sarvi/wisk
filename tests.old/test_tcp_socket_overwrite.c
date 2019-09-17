#include "torture.h"

#include <cmocka.h>
#include <unistd.h>
#include <arpa/inet.h>
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

static void test_tcp_socket_overwrite(void **state)
{
	struct torture_address addr_in = {
		.sa_socklen = sizeof(struct sockaddr_in),
		.sa.in = (struct sockaddr_in) {
			.sin_family = AF_INET,
		},
	};

	int s, dup_s, new_s, rc;

	(void) state; /* unused */

	s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	assert_int_not_equal(s, -1);

	dup_s = dup(s);
	assert_int_not_equal(dup_s, -1);

	close(dup_s);

	new_s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	assert_int_not_equal(new_s, -1);

	close(new_s);

	rc = inet_pton(AF_INET, "127.0.0.20", &addr_in.sa.in.sin_addr);
	assert_int_equal(rc, 1);

	/* bind should fail during socklen check if old socket info
	 * is overwritten by new socket info */
	rc = bind(s, &addr_in.sa.s, addr_in.sa_socklen);
	assert_return_code(rc, errno);

	close(s);
}

int main(void) {
	int rc;

	const struct CMUnitTest tcp_socket_overwrite_tests[] = {
		cmocka_unit_test(test_tcp_socket_overwrite),
	};

	rc = cmocka_run_group_tests(tcp_socket_overwrite_tests,
				    setup, teardown);

	return rc;
}
