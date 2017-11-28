#include "config.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

/*
 * This reproduces and issue if we get a signal after the pthread_atfork()
 * prepare function of socket wrapper has been called.
 *
 * The order how pthread_atfork() handlers are set up is:
 *   -> application
 *   -> preloaded libraries
 *   -> libraries
 *
 * We have a library called thread_deadlock.
 *
 * This library registers a thread_deadlock_prepare() function via
 * pthread_atfork().
 *
 * So pthread_atfork() registers the prepare function in the follow order:
 *   -> swrap_thread_prepare()
 *   -> thread_deadlock_prepare()
 *
 * In this test we fork and the swrap_thread_prepare() locks the mutex for
 * symbol binding.
 * Then thread_deadlock_prepare() is called which sends a signal to the parent
 * process of this test. The signal triggers the signal handler below.
 *
 * When we call write() in the signal handler, we will try to bind the libc symbol
 * and want to lock the symbol binding mutex. As it is already locked we run into
 * a deadlock.
 */

static void test_swrap_signal_handler(int signum)
{
	fprintf(stderr, "PID: %d, SIGNUM: %d\n", getpid(), signum);
	write(1, "DEADLOCK?\n", 10);
}

static void test_swrap_fork_pthread(void **state)
{
	pid_t pid;
	struct sigaction act = {
		.sa_handler = test_swrap_signal_handler,
		.sa_flags = 0,
	};

	(void)state; /* unused */

	sigemptyset(&act.sa_mask);
	sigaction(SIGUSR1, &act, NULL);

	pid = fork();
	assert_return_code(pid, errno);

	/* child */
	if (pid == 0) {
		exit(0);
	}

	/* parent */
	if (pid > 0) {
		pid_t child_pid;
		int wstatus = -1;

		child_pid = waitpid(-1, &wstatus, 0);
		assert_return_code(child_pid, errno);

		assert_true(WIFEXITED(wstatus));

		assert_int_equal(WEXITSTATUS(wstatus), 0);
	}
}

int main(void)
{
	int rc;

	const struct CMUnitTest swrap_tests[] = {
		cmocka_unit_test(test_swrap_fork_pthread),
	};

	rc = cmocka_run_group_tests(swrap_tests, NULL, NULL);

	return rc;
}
