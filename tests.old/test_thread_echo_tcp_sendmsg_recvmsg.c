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

static void *thread_worker1(void *arg)
{
	struct torture_address addr = {
		.sa_socklen = sizeof(struct sockaddr_storage),
	};
	char send_buf[64] = {0};
	char recv_buf[64] = {0};
	ssize_t ret;
	int rc;
	int i;
	int s;

	(void) arg; /* unused */

	s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	assert_int_not_equal(s, -1);

	addr.sa.in.sin_family = AF_INET;
	addr.sa.in.sin_port = htons(torture_server_port());

	rc = inet_pton(AF_INET,
		       torture_server_address(AF_INET),
		       &addr.sa.in.sin_addr);
	assert_int_equal(rc, 1);

	rc = connect(s, &addr.sa.s, addr.sa_socklen);
	assert_return_code(rc, errno);

	for (i = 0; i < 10; i++) {
		struct torture_address reply_addr = {
			.sa_socklen = sizeof(struct sockaddr_storage),
		};
		struct msghdr s_msg = {
			.msg_namelen = 0,
		};
		struct msghdr r_msg = {
			.msg_namelen = 0,
		};
		struct iovec s_iov;
		struct iovec r_iov;

		snprintf(send_buf, sizeof(send_buf), "packet.%d", i);

		/* This should be ignored */
		rc = inet_pton(AF_INET,
			       "127.0.0.1",
			       &addr.sa.in.sin_addr);
		assert_int_equal(rc, 1);

		s_msg.msg_name = &addr.sa.s;
		s_msg.msg_namelen = addr.sa_socklen;

		s_iov.iov_base = send_buf;
		s_iov.iov_len = sizeof(send_buf);

		s_msg.msg_iov = &s_iov;
		s_msg.msg_iovlen = 1;

		ret = sendmsg(s, &s_msg, 0);
		assert_int_not_equal(ret, -1);

		r_msg.msg_name = &reply_addr.sa.s;
		r_msg.msg_namelen = reply_addr.sa_socklen;

		r_iov.iov_base = recv_buf;
		r_iov.iov_len = sizeof(recv_buf);

		r_msg.msg_iov = &r_iov;
		r_msg.msg_iovlen = 1;

		ret = recvmsg(s, &r_msg, 0);
		assert_int_not_equal(ret, -1);

		assert_int_equal(r_msg.msg_namelen, 0);

		assert_memory_equal(send_buf, recv_buf, sizeof(send_buf));
	}

	close(s);
	return NULL;
}

static void *thread_worker2(void *arg)
{
	struct torture_address send_addr = {
		.sa_socklen = sizeof(struct sockaddr_storage),
	};
	struct msghdr s_msg = {
		.msg_namelen = 0,
	};
	struct msghdr r_msg = {
		.msg_namelen = 0,
	};
	struct iovec iov;
	char payload[] = "PACKET";
	ssize_t ret;
	int rc;
	int s;

	(void)arg; /* unused */

	s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	assert_int_not_equal(s, -1);

	send_addr.sa.in.sin_family = AF_INET;
	send_addr.sa.in.sin_port = htons(torture_server_port());

	rc = inet_pton(AF_INET,
		       torture_server_address(AF_INET),
		       &send_addr.sa.in.sin_addr);
	assert_int_equal(rc, 1);

	rc = connect(s, &send_addr.sa.s, send_addr.sa_socklen);
	assert_return_code(rc, errno);

	/* msg_name = NULL */

	iov.iov_base = (void *)payload;
	iov.iov_len = sizeof(payload);

	s_msg.msg_iov = &iov;
	s_msg.msg_iovlen = 1;

	ret = sendmsg(s, &s_msg, 0);
	assert_int_not_equal(ret, -1);

	/* msg_name = NULL */

	memset(payload, 0, sizeof(payload));

	r_msg.msg_iov = &iov;
	r_msg.msg_iovlen = 1;

	ret = recvmsg(s, &r_msg, 0);
	assert_int_not_equal(ret, -1);

	assert_int_equal(r_msg.msg_namelen, 0);
	assert_null(r_msg.msg_name);

	close(s);
	return NULL;
}

static void test_sendmsg_recvmsg_ipv4(void **state)
{
	pthread_attr_t pthread_custom_attr;
	pthread_t threads[NUM_THREADS];
	int i;

	(void) state; /* unused */

	pthread_attr_init(&pthread_custom_attr);

	for (i = 0; i < NUM_THREADS; i++) {
		pthread_create(&threads[i],
			       &pthread_custom_attr,
			       thread_worker1,
			       NULL);
	}

	for (i = 0; i < NUM_THREADS; i++) {
		pthread_join(threads[i], NULL);
	}

	pthread_attr_destroy(&pthread_custom_attr);
}

static void test_sendmsg_recvmsg_ipv4_null(void **state)
{
	pthread_attr_t pthread_custom_attr;
	pthread_t threads[NUM_THREADS];
	int i;

	(void) state; /* unused */

	pthread_attr_init(&pthread_custom_attr);

	for (i = 0; i < NUM_THREADS; i++) {
		pthread_create(&threads[i],
			       &pthread_custom_attr,
			       thread_worker2,
			       NULL);
	}

	for (i = 0; i < NUM_THREADS; i++) {
		pthread_join(threads[i], NULL);
	}

	pthread_attr_destroy(&pthread_custom_attr);
}


int main(void) {
	int rc;

	const struct CMUnitTest sendmsg_tests[] = {
		cmocka_unit_test_setup_teardown(test_sendmsg_recvmsg_ipv4,
						setup_echo_srv_tcp_ipv4,
						teardown),
		cmocka_unit_test_setup_teardown(test_sendmsg_recvmsg_ipv4_null,
						setup_echo_srv_tcp_ipv4,
						teardown),
	};

	rc = cmocka_run_group_tests(sendmsg_tests, NULL, NULL);

	return rc;
}
