/*
 * BSD 3-Clause License
 *
 * Copyright (c) 2019-2020, Sarvi Shanmugham <sarvi@cisco.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the author nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
   Filesystem Tracker library. Passes all socket communication over
   unix domain sockets if the environment variable SOCKET_WRAPPER_DIR
   is set.
*/

#include "config.h"

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif
#ifdef HAVE_SYS_SIGNALFD_H
#include <sys/signalfd.h>
#endif
#ifdef HAVE_SYS_EVENTFD_H
#include <sys/eventfd.h>
#endif
#ifdef HAVE_SYS_TIMERFD_H
#include <sys/timerfd.h>
#endif
#include <sys/uio.h>
#include <errno.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>
#ifdef HAVE_GNU_LIB_NAMES_H
#include <gnu/lib-names.h>
#endif
#ifdef HAVE_RPC_RPC_H
#include <rpc/rpc.h>
#endif
#include <pthread.h>

enum swrap_dbglvl_e {
	SWRAP_LOG_ERROR = 0,
	SWRAP_LOG_WARN,
	SWRAP_LOG_DEBUG,
	SWRAP_LOG_TRACE
};

/* GCC have printf type attribute check. */
#ifdef HAVE_FUNCTION_ATTRIBUTE_FORMAT
#define PRINTF_ATTRIBUTE(a,b) __attribute__ ((__format__ (__printf__, a, b)))
#else
#define PRINTF_ATTRIBUTE(a,b)
#endif /* HAVE_FUNCTION_ATTRIBUTE_FORMAT */

#ifdef HAVE_CONSTRUCTOR_ATTRIBUTE
#define CONSTRUCTOR_ATTRIBUTE __attribute__ ((constructor))
#else
#define CONSTRUCTOR_ATTRIBUTE
#endif /* HAVE_CONSTRUCTOR_ATTRIBUTE */

#ifdef HAVE_DESTRUCTOR_ATTRIBUTE
#define DESTRUCTOR_ATTRIBUTE __attribute__ ((destructor))
#else
#define DESTRUCTOR_ATTRIBUTE
#endif

#ifndef FALL_THROUGH
# ifdef HAVE_FALLTHROUGH_ATTRIBUTE
#  define FALL_THROUGH __attribute__ ((fallthrough))
# else /* HAVE_FALLTHROUGH_ATTRIBUTE */
#  define FALL_THROUGH ((void)0)
# endif /* HAVE_FALLTHROUGH_ATTRIBUTE */
#endif /* FALL_THROUGH */

#ifdef HAVE_ADDRESS_SANITIZER_ATTRIBUTE
#define DO_NOT_SANITIZE_ADDRESS_ATTRIBUTE __attribute__((no_sanitize_address))
#else
#define DO_NOT_SANITIZE_ADDRESS_ATTRIBUTE
#endif

#ifdef HAVE_GCC_THREAD_LOCAL_STORAGE
# define SWRAP_THREAD __thread
#else
# define SWRAP_THREAD
#endif

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

#ifndef ZERO_STRUCT
#define ZERO_STRUCT(x) memset((char *)&(x), 0, sizeof(x))
#endif

#ifndef ZERO_STRUCTP
#define ZERO_STRUCTP(x) do { \
		if ((x) != NULL) \
			memset((char *)(x), 0, sizeof(*(x))); \
	} while(0)
#endif

#ifndef SAFE_FREE
#define SAFE_FREE(x) do { if ((x) != NULL) {free(x); (x)=NULL;} } while(0)
#endif

#ifndef discard_const
#define discard_const(ptr) ((void *)((uintptr_t)(ptr)))
#endif

#ifndef discard_const_p
#define discard_const_p(type, ptr) ((type *)discard_const(ptr))
#endif

#define UNUSED(x) (void)(x)

#ifdef IPV6_PKTINFO
# ifndef IPV6_RECVPKTINFO
#  define IPV6_RECVPKTINFO IPV6_PKTINFO
# endif /* IPV6_RECVPKTINFO */
#endif /* IPV6_PKTINFO */

/*
 * On BSD IP_PKTINFO has a different name because during
 * the time when they implemented it, there was no RFC.
 * The name for IPv6 is the same as on Linux.
 */
#ifndef IP_PKTINFO
# ifdef IP_RECVDSTADDR
#  define IP_PKTINFO IP_RECVDSTADDR
# endif
#endif

/* Add new global locks here please */
# define SWRAP_LOCK_ALL \
	swrap_mutex_lock(&libc_symbol_binding_mutex); \

# define SWRAP_UNLOCK_ALL \
	swrap_mutex_unlock(&libc_symbol_binding_mutex); \

#define SOCKET_INFO_CONTAINER(si) \
	(struct socket_info_container *)(si)

#define SWRAP_LOCK_SI(si) do { \
	struct socket_info_container *sic = SOCKET_INFO_CONTAINER(si); \
	swrap_mutex_lock(&sic->meta.mutex); \
} while(0)

#define SWRAP_UNLOCK_SI(si) do { \
	struct socket_info_container *sic = SOCKET_INFO_CONTAINER(si); \
	swrap_mutex_unlock(&sic->meta.mutex); \
} while(0)

#if defined(HAVE_GETTIMEOFDAY_TZ) || defined(HAVE_GETTIMEOFDAY_TZ_VOID)
#define swrapGetTimeOfDay(tval) gettimeofday(tval,NULL)
#else
#define swrapGetTimeOfDay(tval)	gettimeofday(tval)
#endif

/* we need to use a very terse format here as IRIX 6.4 silently
   truncates names to 16 chars, so if we use a longer name then we
   can't tell which port a packet came from with recvfrom()

   with this format we have 8 chars left for the directory name
*/
#define SOCKET_FORMAT "%c%02X%04X"
#define SOCKET_TYPE_CHAR_TCP		'T'
#define SOCKET_TYPE_CHAR_UDP		'U'
#define SOCKET_TYPE_CHAR_TCP_V6		'X'
#define SOCKET_TYPE_CHAR_UDP_V6		'Y'

/*
 * Set the packet MTU to 1500 bytes for stream sockets to make it it easier to
 * format PCAP capture files (as the caller will simply continue from here).
 */
#define SOCKET_WRAPPER_MTU_DEFAULT 1500
#define SOCKET_WRAPPER_MTU_MIN     512
#define SOCKET_WRAPPER_MTU_MAX     32768

#define SOCKET_MAX_SOCKETS 1024

/*
 * Maximum number of socket_info structures that can
 * be used. Can be overriden by the environment variable
 * SOCKET_WRAPPER_MAX_SOCKETS.
 */
#define SOCKET_WRAPPER_MAX_SOCKETS_DEFAULT 65535

#define SOCKET_WRAPPER_MAX_SOCKETS_LIMIT 262140

/* This limit is to avoid broadcast sendto() needing to stat too many
 * files.  It may be raised (with a performance cost) to up to 254
 * without changing the format above */
#define MAX_WRAPPED_INTERFACES 64

struct swrap_address {
	socklen_t sa_socklen;
	union {
		struct sockaddr s;
		struct sockaddr_in in;
#ifdef HAVE_IPV6
		struct sockaddr_in6 in6;
#endif
		struct sockaddr_un un;
		struct sockaddr_storage ss;
	} sa;
};

int first_free;

struct socket_info
{
	int family;
	int type;
	int protocol;
	int bound;
	int bcast;
	int is_server;
	int connected;
	int defer_connect;
	int pktinfo;
	int tcp_nodelay;

	/* The unix path so we can unlink it on close() */
	struct sockaddr_un un_addr;

	struct swrap_address bindname;
	struct swrap_address myname;
	struct swrap_address peername;

	struct {
		unsigned long pck_snd;
		unsigned long pck_rcv;
	} io;
};

struct socket_info_meta
{
	unsigned int refcount;
	int next_free;
	pthread_mutex_t mutex;
};

struct socket_info_container
{
	struct socket_info info;
	struct socket_info_meta meta;
};

static struct socket_info_container *sockets;

static size_t socket_info_max = 0;

/*
 * Allocate the socket array always on the limit value. We want it to be
 * at least bigger than the default so if we reach the limit we can
 * still deal with duplicate fds pointing to the same socket_info.
 */
static size_t socket_fds_max = SOCKET_WRAPPER_MAX_SOCKETS_LIMIT;

/* Hash table to map fds to corresponding socket_info index */
static int *socket_fds_idx;

/* Mutex to synchronize access to global libc.symbols */
static pthread_mutex_t libc_symbol_binding_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Mutex for syncronizing port selection during swrap_auto_bind() */
static pthread_mutex_t autobind_start_mutex;

/* Mutex to guard the initialization of array of socket_info structures */
static pthread_mutex_t sockets_mutex;

/* Mutex to guard the socket reset in swrap_close() and swrap_remove_stale() */
static pthread_mutex_t socket_reset_mutex;

/* Mutex to synchronize access to first free index in socket_info array */
static pthread_mutex_t first_free_mutex;

/* Mutex to synchronize access to packet capture dump file */
static pthread_mutex_t pcap_dump_mutex;

/* Mutex for synchronizing mtu value fetch*/
static pthread_mutex_t mtu_update_mutex;

/* Function prototypes */

bool socket_wrapper_enabled(void);

void swrap_constructor(void) CONSTRUCTOR_ATTRIBUTE;
void swrap_destructor(void) DESTRUCTOR_ATTRIBUTE;

#ifndef HAVE_GETPROGNAME
static const char *getprogname(void)
{
#if defined(HAVE_PROGRAM_INVOCATION_SHORT_NAME)
	return program_invocation_short_name;
#elif defined(HAVE_GETEXECNAME)
	return getexecname();
#else
	return NULL;
#endif /* HAVE_PROGRAM_INVOCATION_SHORT_NAME */
}
#endif /* HAVE_GETPROGNAME */

static void swrap_log(enum swrap_dbglvl_e dbglvl, const char *func, const char *format, ...) PRINTF_ATTRIBUTE(3, 4);
# define SWRAP_LOG(dbglvl, ...) swrap_log((dbglvl), __func__, __VA_ARGS__)

static void swrap_log(enum swrap_dbglvl_e dbglvl,
		      const char *func,
		      const char *format, ...)
{
	char buffer[1024];
	va_list va;
	const char *d;
	unsigned int lvl = 0;
	const char *prefix = "SWRAP";
	const char *progname = getprogname();

	d = getenv("SOCKET_WRAPPER_DEBUGLEVEL");
	if (d != NULL) {
		lvl = atoi(d);
	}

	if (lvl < dbglvl) {
		return;
	}

	va_start(va, format);
	vsnprintf(buffer, sizeof(buffer), format, va);
	va_end(va);

	switch (dbglvl) {
		case SWRAP_LOG_ERROR:
			prefix = "SWRAP_ERROR";
			break;
		case SWRAP_LOG_WARN:
			prefix = "SWRAP_WARN";
			break;
		case SWRAP_LOG_DEBUG:
			prefix = "SWRAP_DEBUG";
			break;
		case SWRAP_LOG_TRACE:
			prefix = "SWRAP_TRACE";
			break;
	}

	if (progname == NULL) {
		progname = "<unknown>";
	}

	fprintf(stderr,
		"%s[%s (%u)] - %s: %s\n",
		prefix,
		progname,
		(unsigned int)getpid(),
		func,
		buffer);
}

/*********************************************************
 * SWRAP LOADING LIBC FUNCTIONS
 *********************************************************/

#include <dlfcn.h>

typedef int (*__libc_close)(int fd);
typedef int (*__libc_dup)(int fd);
typedef int (*__libc_dup2)(int oldfd, int newfd);
typedef int (*__libc_fcntl)(int fd, int cmd, ...);
typedef FILE *(*__libc_fopen)(const char *name, const char *mode);
#ifdef HAVE_FOPEN64
typedef FILE *(*__libc_fopen64)(const char *name, const char *mode);
#endif
typedef int (*__libc_open)(const char *pathname, int flags, ...);
#ifdef HAVE_OPEN64
typedef int (*__libc_open64)(const char *pathname, int flags, ...);
#endif /* HAVE_OPEN64 */
typedef int (*__libc_openat)(int dirfd, const char *path, int flags, ...);
typedef int (*__libc_pipe)(int pipefd[2]);

#define SWRAP_SYMBOL_ENTRY(i) \
	union { \
		__libc_##i f; \
		void *obj; \
	} _libc_##i

struct swrap_libc_symbols {
	SWRAP_SYMBOL_ENTRY(close);
	SWRAP_SYMBOL_ENTRY(dup);
	SWRAP_SYMBOL_ENTRY(dup2);
	SWRAP_SYMBOL_ENTRY(fcntl);
	SWRAP_SYMBOL_ENTRY(fopen);
#ifdef HAVE_FOPEN64
	SWRAP_SYMBOL_ENTRY(fopen64);
#endif
	SWRAP_SYMBOL_ENTRY(open);
#ifdef HAVE_OPEN64
	SWRAP_SYMBOL_ENTRY(open64);
#endif
	SWRAP_SYMBOL_ENTRY(openat);
	SWRAP_SYMBOL_ENTRY(pipe);
};

struct swrap {
	struct {
		void *handle;
		void *socket_handle;
		struct swrap_libc_symbols symbols;
	} libc;
};

static struct swrap swrap;

/* prototypes */
static char *socket_wrapper_dir(void);

#define LIBC_NAME "libc.so"

enum swrap_lib {
    SWRAP_LIBC,
    SWRAP_LIBNSL,
    SWRAP_LIBSOCKET,
};

static const char *swrap_str_lib(enum swrap_lib lib)
{
	switch (lib) {
	case SWRAP_LIBC:
		return "libc";
	case SWRAP_LIBNSL:
		return "libnsl";
	case SWRAP_LIBSOCKET:
		return "libsocket";
	}

	/* Compiler would warn us about unhandled enum value if we get here */
	return "unknown";
}

static void *swrap_load_lib_handle(enum swrap_lib lib)
{
	int flags = RTLD_LAZY;
	void *handle = NULL;
	int i;

#ifdef RTLD_DEEPBIND
	const char *env_preload = getenv("LD_PRELOAD");
	const char *env_deepbind = getenv("SOCKET_WRAPPER_DISABLE_DEEPBIND");
	bool enable_deepbind = true;

	/* Don't do a deepbind if we run with libasan */
	if (env_preload != NULL && strlen(env_preload) < 1024) {
		const char *p = strstr(env_preload, "libasan.so");
		if (p != NULL) {
			enable_deepbind = false;
		}
	}

	if (env_deepbind != NULL && strlen(env_deepbind) >= 1) {
		enable_deepbind = false;
	}

	if (enable_deepbind) {
		flags |= RTLD_DEEPBIND;
	}
#endif

	switch (lib) {
	case SWRAP_LIBNSL:
	case SWRAP_LIBSOCKET:
#ifdef HAVE_LIBSOCKET
		handle = swrap.libc.socket_handle;
		if (handle == NULL) {
			for (i = 10; i >= 0; i--) {
				char soname[256] = {0};

				snprintf(soname, sizeof(soname), "libsocket.so.%d", i);
				handle = dlopen(soname, flags);
				if (handle != NULL) {
					break;
				}
			}

			swrap.libc.socket_handle = handle;
		}
		break;
#endif
	case SWRAP_LIBC:
		handle = swrap.libc.handle;
#ifdef LIBC_SO
		if (handle == NULL) {
			handle = dlopen(LIBC_SO, flags);

			swrap.libc.handle = handle;
		}
#endif
		if (handle == NULL) {
			for (i = 10; i >= 0; i--) {
				char soname[256] = {0};

				snprintf(soname, sizeof(soname), "libc.so.%d", i);
				handle = dlopen(soname, flags);
				if (handle != NULL) {
					break;
				}
			}

			swrap.libc.handle = handle;
		}
		break;
	}

	if (handle == NULL) {
#ifdef RTLD_NEXT
		handle = swrap.libc.handle = swrap.libc.socket_handle = RTLD_NEXT;
#else
		SWRAP_LOG(SWRAP_LOG_ERROR,
			  "Failed to dlopen library: %s\n",
			  dlerror());
		exit(-1);
#endif
	}

	return handle;
}

static void *_swrap_bind_symbol(enum swrap_lib lib, const char *fn_name)
{
	void *handle;
	void *func;

	handle = swrap_load_lib_handle(lib);

	func = dlsym(handle, fn_name);
	if (func == NULL) {
		SWRAP_LOG(SWRAP_LOG_ERROR,
			  "Failed to find %s: %s\n",
			  fn_name,
			  dlerror());
		exit(-1);
	}

	SWRAP_LOG(SWRAP_LOG_TRACE,
		  "Loaded %s from %s",
		  fn_name,
		  swrap_str_lib(lib));

	return func;
}

static void swrap_mutex_lock(pthread_mutex_t *mutex)
{
	int ret;

	ret = pthread_mutex_lock(mutex);
	if (ret != 0) {
		SWRAP_LOG(SWRAP_LOG_ERROR, "Couldn't lock pthread mutex - %s",
			  strerror(ret));
	}
}

static void swrap_mutex_unlock(pthread_mutex_t *mutex)
{
	int ret;

	ret = pthread_mutex_unlock(mutex);
	if (ret != 0) {
		SWRAP_LOG(SWRAP_LOG_ERROR, "Couldn't unlock pthread mutex - %s",
			  strerror(ret));
	}
}

/*
 * These macros have a thread race condition on purpose!
 *
 * This is an optimization to avoid locking each time we check if the symbol is
 * bound.
 */
#define swrap_bind_symbol_libc(sym_name) \
	if (swrap.libc.symbols._libc_##sym_name.obj == NULL) { \
		swrap_mutex_lock(&libc_symbol_binding_mutex); \
		if (swrap.libc.symbols._libc_##sym_name.obj == NULL) { \
			swrap.libc.symbols._libc_##sym_name.obj = \
				_swrap_bind_symbol(SWRAP_LIBC, #sym_name); \
		} \
		swrap_mutex_unlock(&libc_symbol_binding_mutex); \
	}

#define swrap_bind_symbol_libsocket(sym_name) \
	if (swrap.libc.symbols._libc_##sym_name.obj == NULL) { \
		swrap_mutex_lock(&libc_symbol_binding_mutex); \
		if (swrap.libc.symbols._libc_##sym_name.obj == NULL) { \
			swrap.libc.symbols._libc_##sym_name.obj = \
				_swrap_bind_symbol(SWRAP_LIBSOCKET, #sym_name); \
		} \
		swrap_mutex_unlock(&libc_symbol_binding_mutex); \
	}

#define swrap_bind_symbol_libnsl(sym_name) \
	if (swrap.libc.symbols._libc_##sym_name.obj == NULL) { \
		swrap_mutex_lock(&libc_symbol_binding_mutex); \
		if (swrap.libc.symbols._libc_##sym_name.obj == NULL) { \
			swrap.libc.symbols._libc_##sym_name.obj = \
				_swrap_bind_symbol(SWRAP_LIBNSL, #sym_name); \
		} \
		swrap_mutex_unlock(&libc_symbol_binding_mutex); \
	}

/****************************************************************************
 *                               IMPORTANT
 ****************************************************************************
 *
 * Functions especially from libc need to be loaded individually, you can't
 * load all at once or gdb will segfault at startup. The same applies to
 * valgrind and has probably something todo with with the linker.  So we need
 * load each function at the point it is called the first time.
 *
 ****************************************************************************/

static int libc_close(int fd)
{
	swrap_bind_symbol_libc(close);

	return swrap.libc.symbols._libc_close.f(fd);
}

static int libc_dup(int fd)
{
	swrap_bind_symbol_libc(dup);

	return swrap.libc.symbols._libc_dup.f(fd);
}

static int libc_dup2(int oldfd, int newfd)
{
	swrap_bind_symbol_libc(dup2);

	return swrap.libc.symbols._libc_dup2.f(oldfd, newfd);
}

static FILE *libc_fopen(const char *name, const char *mode)
{
	swrap_bind_symbol_libc(fopen);

	return swrap.libc.symbols._libc_fopen.f(name, mode);
}

#ifdef HAVE_FOPEN64
static FILE *libc_fopen64(const char *name, const char *mode)
{
	swrap_bind_symbol_libc(fopen64);

	return swrap.libc.symbols._libc_fopen64.f(name, mode);
}
#endif /* HAVE_FOPEN64 */

static int libc_vopen(const char *pathname, int flags, va_list ap)
{
	int mode = 0;
	int fd;

	swrap_bind_symbol_libc(open);

	if (flags & O_CREAT) {
		mode = va_arg(ap, int);
	}
	fd = swrap.libc.symbols._libc_open.f(pathname, flags, (mode_t)mode);

	return fd;
}

static int libc_open(const char *pathname, int flags, ...)
{
	va_list ap;
	int fd;

	va_start(ap, flags);
	fd = libc_vopen(pathname, flags, ap);
	va_end(ap);

	return fd;
}

#ifdef HAVE_OPEN64
static int libc_vopen64(const char *pathname, int flags, va_list ap)
{
	int mode = 0;
	int fd;

	swrap_bind_symbol_libc(open64);

	if (flags & O_CREAT) {
		mode = va_arg(ap, int);
	}
	fd = swrap.libc.symbols._libc_open64.f(pathname, flags, (mode_t)mode);

	return fd;
}
#endif /* HAVE_OPEN64 */

static int libc_vopenat(int dirfd, const char *path, int flags, va_list ap)
{
	int mode = 0;
	int fd;

	swrap_bind_symbol_libc(openat);

	if (flags & O_CREAT) {
		mode = va_arg(ap, int);
	}
	fd = swrap.libc.symbols._libc_openat.f(dirfd,
					       path,
					       flags,
					       (mode_t)mode);

	return fd;
}

#if 0
static int libc_openat(int dirfd, const char *path, int flags, ...)
{
	va_list ap;
	int fd;

	va_start(ap, flags);
	fd = libc_vopenat(dirfd, path, flags, ap);
	va_end(ap);

	return fd;
}
#endif

static int libc_pipe(int pipefd[2])
{
	swrap_bind_symbol_libsocket(pipe);

	return swrap.libc.symbols._libc_pipe.f(pipefd);
}

/* DO NOT call this function during library initialization! */
static void swrap_bind_symbol_all(void)
{
	swrap_bind_symbol_libc(close);
	swrap_bind_symbol_libc(dup);
	swrap_bind_symbol_libc(dup2);
	swrap_bind_symbol_libc(fopen);
#ifdef HAVE_FOPEN64
	swrap_bind_symbol_libc(fopen64);
#endif
	swrap_bind_symbol_libc(open);
#ifdef HAVE_OPEN64
	swrap_bind_symbol_libc(open64);
#endif
	swrap_bind_symbol_libc(openat);
}

/*********************************************************
 * SWRAP HELPER FUNCTIONS
 *********************************************************/

static void swrap_inc_refcount(struct socket_info *si)
{
	struct socket_info_container *sic = SOCKET_INFO_CONTAINER(si);

	sic->meta.refcount += 1;
}

static void swrap_dec_refcount(struct socket_info *si)
{
	struct socket_info_container *sic = SOCKET_INFO_CONTAINER(si);

	sic->meta.refcount -= 1;
}

static char *socket_wrapper_dir(void)
{
	char *swrap_dir = NULL;
	char *s = getenv("SOCKET_WRAPPER_DIR");

    printf("Hello World from the librar\n");
	if (s == NULL) {
		SWRAP_LOG(SWRAP_LOG_WARN, "SOCKET_WRAPPER_DIR not set\n");
		return NULL;
	}

	swrap_dir = realpath(s, NULL);
	if (swrap_dir == NULL) {
		SWRAP_LOG(SWRAP_LOG_ERROR,
			  "Unable to resolve socket_wrapper dir path: %s",
			  strerror(errno));
		return NULL;
	}

	SWRAP_LOG(SWRAP_LOG_TRACE, "socket_wrapper_dir: %s", swrap_dir);
	return swrap_dir;
}

static int socket_wrapper_init_mutex(pthread_mutex_t *m)
{
	pthread_mutexattr_t ma;
	int ret;

	ret = pthread_mutexattr_init(&ma);
	if (ret != 0) {
		return ret;
	}

	ret = pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_ERRORCHECK);
	if (ret != 0) {
		goto done;
	}

	ret = pthread_mutex_init(m, &ma);

done:
	pthread_mutexattr_destroy(&ma);

	return ret;
}

static size_t socket_wrapper_max_sockets(void)
{
	const char *s;
	size_t tmp;
	char *endp;

	if (socket_info_max != 0) {
		return socket_info_max;
	}

	socket_info_max = SOCKET_WRAPPER_MAX_SOCKETS_DEFAULT;

	s = getenv("SOCKET_WRAPPER_MAX_SOCKETS");
	if (s == NULL || s[0] == '\0') {
		goto done;
	}

	tmp = strtoul(s, &endp, 10);
	if (s == endp) {
		goto done;
	}
	if (tmp == 0) {
		tmp = SOCKET_WRAPPER_MAX_SOCKETS_DEFAULT;
		SWRAP_LOG(SWRAP_LOG_ERROR,
			  "Invalid number of sockets specified, "
			  "using default (%zu)",
			  tmp);
	}

	if (tmp > SOCKET_WRAPPER_MAX_SOCKETS_LIMIT) {
		tmp = SOCKET_WRAPPER_MAX_SOCKETS_LIMIT;
		SWRAP_LOG(SWRAP_LOG_ERROR,
			  "Invalid number of sockets specified, "
			  "using maximum (%zu).",
			  tmp);
	}

	socket_info_max = tmp;

done:
	return socket_info_max;
}

static void socket_wrapper_init_sockets(void)
{
	size_t max_sockets;
	size_t i;
	int ret;

	swrap_mutex_lock(&sockets_mutex);

	if (sockets != NULL) {
		swrap_mutex_unlock(&sockets_mutex);
		return;
	}


	swrap_mutex_lock(&first_free_mutex);

	ret = socket_wrapper_init_mutex(&autobind_start_mutex);
	if (ret != 0) {
		SWRAP_LOG(SWRAP_LOG_ERROR,
			  "Failed to initialize pthread mutex");
		goto done;
	}

	ret = socket_wrapper_init_mutex(&pcap_dump_mutex);
	if (ret != 0) {
		SWRAP_LOG(SWRAP_LOG_ERROR,
			  "Failed to initialize pthread mutex");
		goto done;
	}

	ret = socket_wrapper_init_mutex(&mtu_update_mutex);
	if (ret != 0) {
		SWRAP_LOG(SWRAP_LOG_ERROR,
			  "Failed to initialize pthread mutex");
		goto done;
	}

done:
	swrap_mutex_unlock(&first_free_mutex);
	swrap_mutex_unlock(&sockets_mutex);
	if (ret != 0) {
		exit(-1);
	}
}

bool socket_wrapper_enabled(void)
{
	char *s = socket_wrapper_dir();

	if (s == NULL) {
		return false;
	}

	SAFE_FREE(s);

	socket_wrapper_init_sockets();

	return true;
}

/****************************************************************************
 *   FOPEN
 ***************************************************************************/

static FILE *swrap_fopen(const char *name, const char *mode)
{
	FILE *fp;

	if (!socket_wrapper_enabled()) {
        SWRAP_LOG(SWRAP_LOG_TRACE, "SOCKET_WRAPPER_DIR not set\n");
	}
	return libc_fopen(name, mode);

	return fp;
}

FILE *fopen(const char *name, const char *mode)
{
	return swrap_fopen(name, mode);
}

/****************************************************************************
 *   FOPEN64
 ***************************************************************************/

#ifdef HAVE_FOPEN64
static FILE *swrap_fopen64(const char *name, const char *mode)
{
	FILE *fp;

	fp = libc_fopen64(name, mode);
	if (fp != NULL) {
		int fd = fileno(fp);
	}

	return fp;
}

FILE *fopen64(const char *name, const char *mode)
{
	return swrap_fopen64(name, mode);
}
#endif /* HAVE_FOPEN64 */

/****************************************************************************
 *   OPEN
 ***************************************************************************/

static int swrap_vopen(const char *pathname, int flags, va_list ap)
{
	int ret;

	ret = libc_vopen(pathname, flags, ap);
	if (ret != -1) {
	}
	return ret;
}

int open(const char *pathname, int flags, ...)
{
	va_list ap;
	int fd;

	va_start(ap, flags);
	fd = swrap_vopen(pathname, flags, ap);
	va_end(ap);

	return fd;
}

/****************************************************************************
 *   OPEN64
 ***************************************************************************/

#ifdef HAVE_OPEN64
static int swrap_vopen64(const char *pathname, int flags, va_list ap)
{
	int ret;

	ret = libc_vopen64(pathname, flags, ap);
	if (ret != -1) {
		/*
		 * There are methods for closing descriptors (libc-internal code
		 * paths, direct syscalls) which close descriptors in ways that
		 * we can't intercept, so try to recover when we notice that
		 * that's happened
		 */
	}
	return ret;
}

int open64(const char *pathname, int flags, ...)
{
	va_list ap;
	int fd;

	va_start(ap, flags);
	fd = swrap_vopen64(pathname, flags, ap);
	va_end(ap);

	return fd;
}
#endif /* HAVE_OPEN64 */

/****************************************************************************
 *   OPENAT
 ***************************************************************************/

static int swrap_vopenat(int dirfd, const char *path, int flags, va_list ap)
{
	int ret;

	ret = libc_vopenat(dirfd, path, flags, ap);
	if (ret != -1) {
		/*
		 * There are methods for closing descriptors (libc-internal code
		 * paths, direct syscalls) which close descriptors in ways that
		 * we can't intercept, so try to recover when we notice that
		 * that's happened
		 */
	}

	return ret;
}

int openat(int dirfd, const char *path, int flags, ...)
{
	va_list ap;
	int fd;

	va_start(ap, flags);
	fd = swrap_vopenat(dirfd, path, flags, ap);
	va_end(ap);

	return fd;
}

/****************************
 * DUP
 ***************************/

static int swrap_dup(int fd)
{
	struct socket_info *si;
	int dup_fd, idx;

	dup_fd = libc_dup(fd);
	if (dup_fd == -1) {
		int saved_errno = errno;
		errno = saved_errno;
		return -1;
	}

	SWRAP_LOCK_SI(si);

	swrap_inc_refcount(si);

	SWRAP_UNLOCK_SI(si);

	return dup_fd;
}

int dup(int fd)
{
	return swrap_dup(fd);
}

/****************************
 * DUP2
 ***************************/

static int swrap_dup2(int fd, int newfd)
{
	struct socket_info *si;
	int dup_fd, idx;

	dup_fd = libc_dup2(fd, newfd);
	if (dup_fd == -1) {
		int saved_errno = errno;
		errno = saved_errno;
		return -1;
	}

	SWRAP_LOCK_SI(si);

	swrap_inc_refcount(si);

	SWRAP_UNLOCK_SI(si);

	return dup_fd;
}

int dup2(int fd, int newfd)
{
	return swrap_dup2(fd, newfd);
}

static void swrap_thread_prepare(void)
{
	/*
	 * This function should only be called here!!
	 *
	 * We bind all symobls to avoid deadlocks of the fork is
	 * interrupted by a signal handler using a symbol of this
	 * library.
	 */
	swrap_bind_symbol_all();

	SWRAP_LOCK_ALL;
}

static void swrap_thread_parent(void)
{
	SWRAP_UNLOCK_ALL;
}

static void swrap_thread_child(void)
{
	SWRAP_UNLOCK_ALL;
}

/****************************
 * CONSTRUCTOR
 ***************************/
void swrap_constructor(void)
{
	int ret;

	/*
	* If we hold a lock and the application forks, then the child
	* is not able to unlock the mutex and we are in a deadlock.
	* This should prevent such deadlocks.
	*/
	pthread_atfork(&swrap_thread_prepare,
		       &swrap_thread_parent,
		       &swrap_thread_child);

	ret = socket_wrapper_init_mutex(&sockets_mutex);
	if (ret != 0) {
		SWRAP_LOG(SWRAP_LOG_ERROR,
			  "Failed to initialize pthread mutex");
		exit(-1);
	}

	ret = socket_wrapper_init_mutex(&socket_reset_mutex);
	if (ret != 0) {
		SWRAP_LOG(SWRAP_LOG_ERROR,
			  "Failed to initialize pthread mutex");
		exit(-1);
	}

	ret = socket_wrapper_init_mutex(&first_free_mutex);
	if (ret != 0) {
		SWRAP_LOG(SWRAP_LOG_ERROR,
			  "Failed to initialize pthread mutex");
		exit(-1);
	}
}

/****************************
 * DESTRUCTOR
 ***************************/

/*
 * This function is called when the library is unloaded and makes sure that
 * sockets get closed and the unix file for the socket are unlinked.
 */
void swrap_destructor(void)
{
	size_t i;

	if (socket_fds_idx != NULL) {
		for (i = 0; i < socket_fds_max; ++i) {
			if (socket_fds_idx[i] != -1) {
				swrap_close(i);
			}
		}
		SAFE_FREE(socket_fds_idx);
	}

	SAFE_FREE(sockets);

	if (swrap.libc.handle != NULL) {
		dlclose(swrap.libc.handle);
	}
	if (swrap.libc.socket_handle) {
		dlclose(swrap.libc.socket_handle);
	}
}
