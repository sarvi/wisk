#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <pthread.h>

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

#define NUM_THREADS 10

static int setup_echo_srv_tcp_ipv4(void **state)
{
	torture_setup_echo_srv_tcp_ipv4(state);

	return 0;
}

static int teardown(void **state)
{
	torture_teardown_echo_srv(state);

	return 0;
}

static void *thread_worker(void *arg)
{
	struct torture_address addr = {
		.sa_socklen = sizeof(struct sockaddr_in),
	};
	int rc;
	int s;

	(void) arg; /* unused */

	s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	assert_int_not_equal(s, -1);

	addr.sa.in.sin_family = AF_INET;
	addr.sa.in.sin_port = htons(torture_server_port());

	rc = inet_pton(addr.sa.in.sin_family,
		       torture_server_address(AF_INET),
		       &addr.sa.in.sin_addr);
	assert_int_equal(rc, 1);

	rc = connect(s, &addr.sa.s, addr.sa_socklen);
	assert_return_code(rc, errno);

	close(s);
	return NULL;
}

static void test_connect_ipv4(void **state)
{
	pthread_attr_t pthread_custom_attr;
	pthread_t threads[NUM_THREADS];
	int i;

	(void) state; /* unused */

	pthread_attr_init(&pthread_custom_attr);

	for (i = 0; i < NUM_THREADS; i++) {
		pthread_create(&threads[i],
			       &pthread_custom_attr,
			       thread_worker,
			       NULL);
	}

	for (i = 0; i < NUM_THREADS; i++) {
		pthread_join(threads[i], NULL);
	}

	pthread_attr_destroy(&pthread_custom_attr);
}

int main(void) {
	int rc;

	const struct CMUnitTest tcp_connect_tests[] = {
		cmocka_unit_test(test_connect_ipv4),
	};

	rc = cmocka_run_group_tests(tcp_connect_tests,
				    setup_echo_srv_tcp_ipv4,
				    teardown);

	return rc;
}
