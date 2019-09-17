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

static int setup_echo_srv_udp_ipv4(void **state)
{
	torture_setup_echo_srv_udp_ipv4(state);

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
	ssize_t ret;
	int rc;
	int i;
	int s;

	(void) arg; /* unused */

	s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	assert_int_not_equal(s, -1);

	addr.sa.in.sin_family = AF_INET;
	addr.sa.in.sin_port = htons(torture_server_port());

	rc = inet_pton(AF_INET,
		       torture_server_address(AF_INET),
		       &addr.sa.in.sin_addr);
	assert_int_equal(rc, 1);

	rc = connect(s, &addr.sa.s, addr.sa_socklen);
	assert_int_equal(rc, 0);

	for (i = 0; i < 10; i++) {
		char send_buf[64] = {0};
		char recv_buf[64] = {0};

		snprintf(send_buf, sizeof(send_buf), "packet.%d", i);

		ret = send(s,
			   send_buf,
			   sizeof(send_buf),
			   0);
		assert_int_not_equal(ret, -1);

		ret = recv(s,
			   recv_buf,
			   sizeof(recv_buf),
			   0);
		assert_int_not_equal(ret, -1);

		assert_memory_equal(send_buf, recv_buf, sizeof(send_buf));
	}

	close(s);
	return NULL;
}

static void test_send_recv_ipv4(void **state)
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

	const struct CMUnitTest send_tests[] = {
		cmocka_unit_test_setup_teardown(test_send_recv_ipv4,
						setup_echo_srv_udp_ipv4,
						teardown),
	};

	rc = cmocka_run_group_tests(send_tests, NULL, NULL);

	return rc;
}
