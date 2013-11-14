#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void setup(void **state)
{
	char test_tmpdir[256];
	const char *p;

	(void) state; /* unused */

	snprintf(test_tmpdir, sizeof(test_tmpdir), "/tmp/test_socket_wrapper_XXXXXX");

	p = mkdtemp(test_tmpdir);
	assert_non_null(p);

	setenv("SOCKET_WRAPPER_DIR", p, 1);
	setenv("SOCKET_WRAPPER_DEFAULT_IFACE", "11", 1);
}

static void teardown(void **state)
{
	char remove_cmd[256] = {0};
	const char *swrap_dir = getenv("SOCKET_WRAPPER_DIR");
	int rc;

	(void) state; /* unused */

	if (swrap_dir != NULL) {
		snprintf(remove_cmd, sizeof(remove_cmd), "rm -rf %s", swrap_dir);
	}

	rc = system(remove_cmd);
	if (rc < 0) {
		fprintf(stderr, "%s failed: %s", remove_cmd, strerror(errno));
	}
}

#if 0
static void test_socket_wrapper_dir(void **state)
{
	backup_env();

	setenv("SOCKET_WRAPPER_DIR", "foo", 1);
	assert_string_equal(socket_wrapper_dir(), "foo");
	setenv("SOCKET_WRAPPER_DIR", "./foo", 1);
	assert_string_equal(socket_wrapper_dir(), "foo");
	unsetenv("SOCKET_WRAPPER_DIR");
	assert_non_null(socket_wrapper_dir());

	restore_env();
}
#endif

static void test_swrap_ioctl(void **state)
{
	int fd;
	int rc;

	(void) state; /* unused */

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	assert_int_not_equal(fd, -1);

	rc = ioctl(fd, FIONBIO);
	assert_int_equal(rc, 0);
}

static void test_swrap_socket(void **state)
{
	int rc;

	(void) state; /* unused */

	rc = socket(1337, 1337, 0);
	assert_int_equal(rc, -1);
	assert_int_equal(errno, EAFNOSUPPORT);

	rc = socket(AF_INET, 1337, 0);
	assert_int_equal(rc, -1);
	assert_int_equal(errno, EPROTONOSUPPORT);

	rc = socket(AF_INET, SOCK_DGRAM, 10);
	assert_int_equal(rc, -1);
	assert_int_equal(errno, EPROTONOSUPPORT);
}

#if 0
unsigned int socket_wrapper_default_iface(void);
static bool test_socket_wrapper_default_iface(struct torture_context *tctx)
{
	backup_env();
	unsetenv("SOCKET_WRAPPER_DEFAULT_IFACE");
	torture_assert_int_equal(tctx, socket_wrapper_default_iface(), 1, "unset");
	setenv("SOCKET_WRAPPER_DEFAULT_IFACE", "2", 1);
	torture_assert_int_equal(tctx, socket_wrapper_default_iface(), 2, "unset");
	setenv("SOCKET_WRAPPER_DEFAULT_IFACE", "bla", 1);
	torture_assert_int_equal(tctx, socket_wrapper_default_iface(), 1, "unset");
	restore_env();
	return true;
}
#endif

int main(void) {
	int rc;

	const UnitTest tests[] = {
		unit_test_setup_teardown(test_swrap_socket, setup, teardown),
		unit_test_setup_teardown(test_swrap_ioctl, setup, teardown),
	};

	rc = run_tests(tests);

	return rc;
}
