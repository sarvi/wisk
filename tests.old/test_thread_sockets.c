#include "config.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <pthread.h>

#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <grp.h>

#define NUM_THREADS 10

static void *thread_worker(void *arg)
{
	int i;

	(void) arg; /* unused */

	for (i = 0; i < 1000; i++) {
		int s;

		s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		assert_return_code(s, errno);

		close(s);
	}

	return NULL;
}

static void test_threads_socket(void **state)
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

	const struct CMUnitTest thread_tests[] = {
		cmocka_unit_test(test_threads_socket),
	};

	rc = cmocka_run_group_tests(thread_tests, NULL, NULL);

	return rc;
}
