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
   Filesystem Tracker library. Tracks all filesystem operations and logs it 
   to named pipe, so that a listener to collect create a dependency tree
*/

#include "config.h"

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/un.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>

enum fstrack_dbglvl_e {
	FSTRACK_LOG_ERROR = 0,
	FSTRACK_LOG_WARN,
	FSTRACK_LOG_DEBUG,
	FSTRACK_LOG_TRACE
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

#ifdef HAVE_GCC_THREAD_LOCAL_STORAGE
# define FSTRACK_THREAD __thread
#else
# define FSTRACK_THREAD
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

/* Add new global locks here please */
# define FSTRACK_LOCK_ALL \
	fstrack_mutex_lock(&libc_symbol_binding_mutex); \

# define FSTRACK_UNLOCK_ALL \
	fstrack_mutex_unlock(&libc_symbol_binding_mutex); \

#define FS_INFO_CONTAINER(si) \
	(struct fs_info_container *)(si)

#define FSTRACK_LOCK_SI(si) do { \
	struct fs_info_container *sic = FS_INFO_CONTAINER(si); \
	fstrack_mutex_lock(&sic->meta.mutex); \
} while(0)

#define FSTRACK_UNLOCK_SI(si) do { \
	struct fs_info_container *sic = FS_INFO_CONTAINER(si); \
	fstrack_mutex_unlock(&sic->meta.mutex); \
} while(0)

int first_free;

struct fs_info
{
    FILE *tracker_socket;
};

struct fs_info_meta
{
	unsigned int refcount;
	int next_free;
	pthread_mutex_t mutex;
};

struct fs_info_container
{
	struct fs_info info;
	struct fs_info_meta meta;
};

static struct fs_info_container *sockets;

static size_t fs_info_max = 0;

/* Mutex to synchronize access to global libc.symbols */
static pthread_mutex_t libc_symbol_binding_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Mutex for syncronizing port selection during fstrack_auto_bind() */
static pthread_mutex_t autobind_start_mutex;

/* Mutex to guard the initialization of array of fs_info structures */
static pthread_mutex_t sockets_mutex;

/* Mutex to guard the socket reset in fstrack_close() and fstrack_remove_stale() */
static pthread_mutex_t socket_reset_mutex;

/* Mutex to synchronize access to first free index in fs_info array */
static pthread_mutex_t first_free_mutex;

/* Mutex to synchronize access to packet capture dump file */
static pthread_mutex_t pcap_dump_mutex;

/* Mutex for synchronizing mtu value fetch*/
static pthread_mutex_t mtu_update_mutex;

/* Function prototypes */

bool fs_tracker_enabled(void);

void fstrack_constructor(void) CONSTRUCTOR_ATTRIBUTE;
void fstrack_destructor(void) DESTRUCTOR_ATTRIBUTE;

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

static void fstrack_log(enum fstrack_dbglvl_e dbglvl, const char *func, const char *format, ...) PRINTF_ATTRIBUTE(3, 4);
# define FSTRACK_LOG(dbglvl, ...) fstrack_log((dbglvl), __func__, __VA_ARGS__)

static void fstrack_log(enum fstrack_dbglvl_e dbglvl,
		      const char *func,
		      const char *format, ...)
{
	char buffer[1024];
	va_list va;
	const char *d;
	unsigned int lvl = 0;
	const char *prefix = "SWRAP";
	const char *progname = getprogname();

	d = getenv("FS_TRACKER_DEBUGLEVEL");
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
		case FSTRACK_LOG_ERROR:
			prefix = "FSTRACK_ERROR";
			break;
		case FSTRACK_LOG_WARN:
			prefix = "FSTRACK_WARN";
			break;
		case FSTRACK_LOG_DEBUG:
			prefix = "FSTRACK_DEBUG";
			break;
		case FSTRACK_LOG_TRACE:
			prefix = "FSTRACK_TRACE";
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

#define FSTRACK_SYMBOL_ENTRY(i) \
	union { \
		__libc_##i f; \
		void *obj; \
	} _libc_##i

struct fstrack_libc_symbols {
	FSTRACK_SYMBOL_ENTRY(dup);
	FSTRACK_SYMBOL_ENTRY(dup2);
	FSTRACK_SYMBOL_ENTRY(fcntl);
	FSTRACK_SYMBOL_ENTRY(fopen);
#ifdef HAVE_FOPEN64
	FSTRACK_SYMBOL_ENTRY(fopen64);
#endif
	FSTRACK_SYMBOL_ENTRY(open);
#ifdef HAVE_OPEN64
	FSTRACK_SYMBOL_ENTRY(open64);
#endif
	FSTRACK_SYMBOL_ENTRY(openat);
};

struct swrap {
	struct {
		void *handle;
		void *socket_handle;
		struct fstrack_libc_symbols symbols;
	} libc;
};

static struct swrap swrap;

/* prototypes */
static char *fs_tracker_pipe(void);

#define LIBC_NAME "libc.so"

enum fstrack_lib {
    FSTRACK_LIBC,
    FSTRACK_LIBNSL,
    FSTRACK_LIBSOCKET,
};

static const char *fstrack_str_lib(enum fstrack_lib lib)
{
	switch (lib) {
	case FSTRACK_LIBC:
		return "libc";
	case FSTRACK_LIBNSL:
		return "libnsl";
	case FSTRACK_LIBSOCKET:
		return "libsocket";
	}

	/* Compiler would warn us about unhandled enum value if we get here */
	return "unknown";
}

static void *fstrack_load_lib_handle(enum fstrack_lib lib)
{
	int flags = RTLD_LAZY;
	void *handle = NULL;
	int i;

#ifdef RTLD_DEEPBIND
	const char *env_preload = getenv("LD_PRELOAD");
	const char *env_deepbind = getenv("FS_TRACKER_DISABLE_DEEPBIND");
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
	case FSTRACK_LIBNSL:
	case FSTRACK_LIBSOCKET:
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
	case FSTRACK_LIBC:
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
		FSTRACK_LOG(FSTRACK_LOG_ERROR,
			  "Failed to dlopen library: %s\n",
			  dlerror());
		exit(-1);
#endif
	}

	return handle;
}

static void *_fstrack_bind_symbol(enum fstrack_lib lib, const char *fn_name)
{
	void *handle;
	void *func;

	handle = fstrack_load_lib_handle(lib);

	func = dlsym(handle, fn_name);
	if (func == NULL) {
		FSTRACK_LOG(FSTRACK_LOG_ERROR,
			  "Failed to find %s: %s\n",
			  fn_name,
			  dlerror());
		exit(-1);
	}

	FSTRACK_LOG(FSTRACK_LOG_TRACE,
		  "Loaded %s from %s",
		  fn_name,
		  fstrack_str_lib(lib));

	return func;
}

static void fstrack_mutex_lock(pthread_mutex_t *mutex)
{
	int ret;

	ret = pthread_mutex_lock(mutex);
	if (ret != 0) {
		FSTRACK_LOG(FSTRACK_LOG_ERROR, "Couldn't lock pthread mutex - %s",
			  strerror(ret));
	}
}

static void fstrack_mutex_unlock(pthread_mutex_t *mutex)
{
	int ret;

	ret = pthread_mutex_unlock(mutex);
	if (ret != 0) {
		FSTRACK_LOG(FSTRACK_LOG_ERROR, "Couldn't unlock pthread mutex - %s",
			  strerror(ret));
	}
}

/*
 * These macros have a thread race condition on purpose!
 *
 * This is an optimization to avoid locking each time we check if the symbol is
 * bound.
 */
#define fstrack_bind_symbol_libc(sym_name) \
	if (swrap.libc.symbols._libc_##sym_name.obj == NULL) { \
		fstrack_mutex_lock(&libc_symbol_binding_mutex); \
		if (swrap.libc.symbols._libc_##sym_name.obj == NULL) { \
			swrap.libc.symbols._libc_##sym_name.obj = \
				_fstrack_bind_symbol(FSTRACK_LIBC, #sym_name); \
		} \
		fstrack_mutex_unlock(&libc_symbol_binding_mutex); \
	}

#define fstrack_bind_symbol_libsocket(sym_name) \
	if (swrap.libc.symbols._libc_##sym_name.obj == NULL) { \
		fstrack_mutex_lock(&libc_symbol_binding_mutex); \
		if (swrap.libc.symbols._libc_##sym_name.obj == NULL) { \
			swrap.libc.symbols._libc_##sym_name.obj = \
				_fstrack_bind_symbol(FSTRACK_LIBSOCKET, #sym_name); \
		} \
		fstrack_mutex_unlock(&libc_symbol_binding_mutex); \
	}

#define fstrack_bind_symbol_libnsl(sym_name) \
	if (swrap.libc.symbols._libc_##sym_name.obj == NULL) { \
		fstrack_mutex_lock(&libc_symbol_binding_mutex); \
		if (swrap.libc.symbols._libc_##sym_name.obj == NULL) { \
			swrap.libc.symbols._libc_##sym_name.obj = \
				_fstrack_bind_symbol(FSTRACK_LIBNSL, #sym_name); \
		} \
		fstrack_mutex_unlock(&libc_symbol_binding_mutex); \
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

static int libc_dup(int fd)
{
	fstrack_bind_symbol_libc(dup);

	return swrap.libc.symbols._libc_dup.f(fd);
}

static int libc_dup2(int oldfd, int newfd)
{
	fstrack_bind_symbol_libc(dup2);

	return swrap.libc.symbols._libc_dup2.f(oldfd, newfd);
}

static FILE *libc_fopen(const char *name, const char *mode)
{
	fstrack_bind_symbol_libc(fopen);

	return swrap.libc.symbols._libc_fopen.f(name, mode);
}

#ifdef HAVE_FOPEN64
static FILE *libc_fopen64(const char *name, const char *mode)
{
	fstrack_bind_symbol_libc(fopen64);

	return swrap.libc.symbols._libc_fopen64.f(name, mode);
}
#endif /* HAVE_FOPEN64 */

static int libc_vopen(const char *pathname, int flags, va_list ap)
{
	int mode = 0;
	int fd;

	fstrack_bind_symbol_libc(open);

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

	fstrack_bind_symbol_libc(open64);

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

	fstrack_bind_symbol_libc(openat);

	if (flags & O_CREAT) {
		mode = va_arg(ap, int);
	}
	fd = swrap.libc.symbols._libc_openat.f(dirfd,
					       path,
					       flags,
					       (mode_t)mode);

	return fd;
}

/* DO NOT call this function during library initialization! */
static void fstrack_bind_symbol_all(void)
{
	fstrack_bind_symbol_libc(dup);
	fstrack_bind_symbol_libc(dup2);
	fstrack_bind_symbol_libc(fopen);
#ifdef HAVE_FOPEN64
	fstrack_bind_symbol_libc(fopen64);
#endif
	fstrack_bind_symbol_libc(open);
#ifdef HAVE_OPEN64
	fstrack_bind_symbol_libc(open64);
#endif
	fstrack_bind_symbol_libc(openat);
}

/*********************************************************
 * SWRAP HELPER FUNCTIONS
 *********************************************************/

static void fstrack_inc_refcount(struct fs_info *si)
{
	struct fs_info_container *sic = FS_INFO_CONTAINER(si);

	sic->meta.refcount += 1;
}

static void fstrack_dec_refcount(struct fs_info *si)
{
	struct fs_info_container *sic = FS_INFO_CONTAINER(si);

	sic->meta.refcount -= 1;
}

static char *fs_tracker_pipe(void)
{
	char *fstrack_pipe = NULL;
	char *s = getenv("FS_TRACKER_PIPE");

    printf("Hello World from the library\n");
	if (s == NULL) {
		FSTRACK_LOG(FSTRACK_LOG_WARN, "FS_TRACKER_PIPE not set\n");
		return NULL;
	}

	fstrack_pipe = realpath(s, NULL);
	if (fstrack_pipe == NULL) {
		FSTRACK_LOG(FSTRACK_LOG_ERROR,
			  "Unable to resolve fs_wrapper pipe path: %s",
			  strerror(errno));
		return NULL;
	}

	FSTRACK_LOG(FSTRACK_LOG_TRACE, "fs_tracker_pipe: %s", fstrack_pipe);
	return fstrack_pipe;
}

static int fs_tracker_init_mutex(pthread_mutex_t *m)
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

static void fs_tracker_init_sockets(void)
{
	size_t max_sockets;
	size_t i;
	int ret;

	fstrack_mutex_lock(&sockets_mutex);

	if (sockets != NULL) {
		fstrack_mutex_unlock(&sockets_mutex);
		return;
	}


	fstrack_mutex_lock(&first_free_mutex);

	ret = fs_tracker_init_mutex(&autobind_start_mutex);
	if (ret != 0) {
		FSTRACK_LOG(FSTRACK_LOG_ERROR,
			  "Failed to initialize pthread mutex");
		goto done;
	}

	ret = fs_tracker_init_mutex(&pcap_dump_mutex);
	if (ret != 0) {
		FSTRACK_LOG(FSTRACK_LOG_ERROR,
			  "Failed to initialize pthread mutex");
		goto done;
	}

	ret = fs_tracker_init_mutex(&mtu_update_mutex);
	if (ret != 0) {
		FSTRACK_LOG(FSTRACK_LOG_ERROR,
			  "Failed to initialize pthread mutex");
		goto done;
	}

done:
	fstrack_mutex_unlock(&first_free_mutex);
	fstrack_mutex_unlock(&sockets_mutex);
	if (ret != 0) {
		exit(-1);
	}
}

bool fs_tracker_enabled(void)
{
	char *s = fs_tracker_pipe();

	if (s == NULL) {
		return false;
	}

	SAFE_FREE(s);

	fs_tracker_init_sockets();

	return true;
}

/****************************************************************************
 *   FOPEN
 ***************************************************************************/

static FILE *fstrack_fopen(const char *name, const char *mode)
{
	FILE *fp;

	if (!fs_tracker_enabled()) {
        FSTRACK_LOG(FSTRACK_LOG_TRACE, "FS_TRACKER_PIPE not set\n");
	}
	return libc_fopen(name, mode);

	return fp;
}

FILE *fopen(const char *name, const char *mode)
{
	return fstrack_fopen(name, mode);
}

/****************************************************************************
 *   FOPEN64
 ***************************************************************************/

#ifdef HAVE_FOPEN64
static FILE *fstrack_fopen64(const char *name, const char *mode)
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
	return fstrack_fopen64(name, mode);
}
#endif /* HAVE_FOPEN64 */

/****************************************************************************
 *   OPEN
 ***************************************************************************/

static int fstrack_vopen(const char *pathname, int flags, va_list ap)
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
	fd = fstrack_vopen(pathname, flags, ap);
	va_end(ap);

	return fd;
}

/****************************************************************************
 *   OPEN64
 ***************************************************************************/

#ifdef HAVE_OPEN64
static int fstrack_vopen64(const char *pathname, int flags, va_list ap)
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
	fd = fstrack_vopen64(pathname, flags, ap);
	va_end(ap);

	return fd;
}
#endif /* HAVE_OPEN64 */

/****************************************************************************
 *   OPENAT
 ***************************************************************************/

static int fstrack_vopenat(int dirfd, const char *path, int flags, va_list ap)
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
	fd = fstrack_vopenat(dirfd, path, flags, ap);
	va_end(ap);

	return fd;
}

/****************************
 * DUP
 ***************************/

static int fstrack_dup(int fd)
{
	struct fs_info *si;
	int dup_fd, idx;

	dup_fd = libc_dup(fd);
	if (dup_fd == -1) {
		int saved_errno = errno;
		errno = saved_errno;
		return -1;
	}

	FSTRACK_LOCK_SI(si);

	fstrack_inc_refcount(si);

	FSTRACK_UNLOCK_SI(si);

	return dup_fd;
}

int dup(int fd)
{
	return fstrack_dup(fd);
}

/****************************
 * DUP2
 ***************************/

static int fstrack_dup2(int fd, int newfd)
{
	struct fs_info *si;
	int dup_fd, idx;

	dup_fd = libc_dup2(fd, newfd);
	if (dup_fd == -1) {
		int saved_errno = errno;
		errno = saved_errno;
		return -1;
	}

	FSTRACK_LOCK_SI(si);

	fstrack_inc_refcount(si);

	FSTRACK_UNLOCK_SI(si);

	return dup_fd;
}

int dup2(int fd, int newfd)
{
	return fstrack_dup2(fd, newfd);
}

static void fstrack_thread_prepare(void)
{
	/*
	 * This function should only be called here!!
	 *
	 * We bind all symobls to avoid deadlocks of the fork is
	 * interrupted by a signal handler using a symbol of this
	 * library.
	 */
	fstrack_bind_symbol_all();

	FSTRACK_LOCK_ALL;
}

static void fstrack_thread_parent(void)
{
	FSTRACK_UNLOCK_ALL;
}

static void fstrack_thread_child(void)
{
	FSTRACK_UNLOCK_ALL;
}

/****************************
 * CONSTRUCTOR
 ***************************/
void fstrack_constructor(void)
{
	int ret;

	/*
	* If we hold a lock and the application forks, then the child
	* is not able to unlock the mutex and we are in a deadlock.
	* This should prevent such deadlocks.
	*/
	pthread_atfork(&fstrack_thread_prepare,
		       &fstrack_thread_parent,
		       &fstrack_thread_child);

	ret = fs_tracker_init_mutex(&sockets_mutex);
	if (ret != 0) {
		FSTRACK_LOG(FSTRACK_LOG_ERROR,
			  "Failed to initialize pthread mutex");
		exit(-1);
	}

	ret = fs_tracker_init_mutex(&socket_reset_mutex);
	if (ret != 0) {
		FSTRACK_LOG(FSTRACK_LOG_ERROR,
			  "Failed to initialize pthread mutex");
		exit(-1);
	}

	ret = fs_tracker_init_mutex(&first_free_mutex);
	if (ret != 0) {
		FSTRACK_LOG(FSTRACK_LOG_ERROR,
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
void fstrack_destructor(void)
{
	size_t i;

	SAFE_FREE(sockets);

	if (swrap.libc.handle != NULL) {
		dlclose(swrap.libc.handle);
	}
	if (swrap.libc.socket_handle) {
		dlclose(swrap.libc.socket_handle);
	}
}
