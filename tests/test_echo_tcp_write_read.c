#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "config.h"
#include "torture.h"

#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

static void setup_echo_srv_tcp_ipv4(void **state)
{
	torture_setup_echo_srv_tcp_ipv4(state);
}

static void setup_echo_srv_tcp_ipv6(void **state)
{
	torture_setup_echo_srv_tcp_ipv6(state);
}

static void teardown(void **state)
{
	torture_teardown_echo_srv(state);
}

static void test_write_read_ipv4(void **state)
{
	struct sockaddr_in sin;
	socklen_t slen = sizeof(struct sockaddr_in);
	ssize_t ret;
	int rc;
	int i;
	int s;

	(void) state; /* unused */

	s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	assert_int_not_equal(s, -1);

	ZERO_STRUCT(sin);
	sin.sin_family = AF_INET;
	sin.sin_port = htons(torture_server_port());

	rc = inet_pton(sin.sin_family,
		       torture_server_address(AF_INET),
		       &sin.sin_addr);
	assert_int_equal(rc, 1);

	rc = connect(s, (struct sockaddr *)&sin, slen);
	assert_int_equal(rc, 0);

	for (i = 0; i < 10; i++) {
		char send_buf[64] = {0};
		char recv_buf[64] = {0};

		snprintf(send_buf, sizeof(send_buf), "packet.%d", i);

		ret = write(s,
			    send_buf,
			    sizeof(send_buf));
		assert_int_not_equal(ret, -1);

		ret = read(s,
			   recv_buf,
			   sizeof(recv_buf));
		assert_int_not_equal(ret, -1);

		assert_memory_equal(send_buf, recv_buf, sizeof(send_buf));
	}

	close(s);
}

#ifdef HAVE_IPV6
static void test_write_read_ipv6(void **state)
{
	struct sockaddr_in6 sin6;
	socklen_t slen = sizeof(struct sockaddr_in6);
	ssize_t ret;
	int rc;
	int i;
	int s;

	(void) state; /* unused */

	s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	assert_int_not_equal(s, -1);

	ZERO_STRUCT(sin6);
	sin6.sin6_family = AF_INET6;
	sin6.sin6_port = htons(torture_server_port());

	rc = inet_pton(AF_INET6,
		       torture_server_address(AF_INET6),
		       &sin6.sin6_addr);
	assert_int_equal(rc, 1);

	rc = connect(s, (struct sockaddr *)&sin6, slen);
	assert_int_equal(rc, 0);

	for (i = 0; i < 10; i++) {
		char send_buf[64] = {0};
		char recv_buf[64] = {0};

		snprintf(send_buf, sizeof(send_buf), "packet.%d", i);

		ret = write(s,
			    send_buf,
			    sizeof(send_buf));
		assert_int_not_equal(ret, -1);

		ret = read(s,
			   recv_buf,
			   sizeof(recv_buf));
		assert_int_not_equal(ret, -1);

		assert_memory_equal(send_buf, recv_buf, sizeof(send_buf));
	}

	close(s);
}
#endif

int main(void) {
	int rc;

	const UnitTest tests[] = {
		unit_test_setup_teardown(test_write_read_ipv4, setup_echo_srv_tcp_ipv4, teardown),
#ifdef HAVE_IPV6
		unit_test_setup_teardown(test_write_read_ipv6, setup_echo_srv_tcp_ipv6, teardown),
#endif
	};

	rc = run_tests(tests);

	return rc;
}