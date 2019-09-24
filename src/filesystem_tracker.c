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

// Environment Variables
#define LD_PRELOAD "LD_PRELOAD"
#define WISK_TRACKER_UUID "WISK_TRACKER_UUID"
#define WISK_TRACKER_DEBUGLEVEL "WISK_TRACKER_DEBUGLEVEL"
#define WISK_TRACKER_PIPE "WISK_TRACKER_PIPE"
#define WISK_TRACKER_DISABLE_DEEPBIND "WISK_TRACKER_DISABLE_DEEPBIND"

enum wisk_dbglvl_e {
	WISK_LOG_ERROR = 0,
	WISK_LOG_WARN,
	WISK_LOG_DEBUG,
	WISK_LOG_TRACE
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
# define WISK_THREAD __thread
#else
# define WISK_THREAD
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
# define WISK_LOCK_ALL \
	wisk_mutex_lock(&libc_symbol_binding_mutex); \

# define WISK_UNLOCK_ALL \
	wisk_mutex_unlock(&libc_symbol_binding_mutex); \

#define BUFFER_SIZE 4096
#define UUID_SIZE 50

#define WISK_VAR_COUNT 4
static char *wisk_envp[WISK_VAR_COUNT];
static int wisk_env_count = 0;
static int fs_tracker_pipe = -1;
static char fs_tracker_uuid[UUID_SIZE];
static char writebuffer[BUFFER_SIZE];

/* Mutex to synchronize access to global libc.symbols */
static pthread_mutex_t libc_symbol_binding_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Mutex to guard the initialization of array of fs_info structures */
static pthread_mutex_t fs_tracker_pipe_mutex;

/* Function prototypes */

bool fs_tracker_enabled(void);

void wisk_constructor(void) CONSTRUCTOR_ATTRIBUTE;
void wisk_destructor(void) DESTRUCTOR_ATTRIBUTE;

#ifndef HAVE_GETPROGNAME
static const char *getprogname(void)
{
#if defined(HAVE_PROGRAM_INVOCATION_SHORT_NAME)
	return program_invocation_short_name;
#elif defined(HAVE_GETEXECNAME)
	return getexecname();
#else
	return program_invocation_short_name;
#endif /* HAVE_PROGRAM_INVOCATION_SHORT_NAME */
}
#endif /* HAVE_GETPROGNAME */

static void wisk_log(enum wisk_dbglvl_e dbglvl, const char *func, const char *format, ...) PRINTF_ATTRIBUTE(3, 4);
# define WISK_LOG(dbglvl, ...) wisk_log((dbglvl), __func__, __VA_ARGS__)

static void wisk_log(enum wisk_dbglvl_e dbglvl,
		      const char *func,
		      const char *format, ...)
{
	char buffer[1024];
	va_list va;
	const char *d;
	unsigned int lvl = 0;
	const char *prefix = "WISK";
	const char *progname = getprogname();

	d = getenv(WISK_TRACKER_DEBUGLEVEL);
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
		case WISK_LOG_ERROR:
			prefix = "WISK_ERROR";
			break;
		case WISK_LOG_WARN:
			prefix = "WISK_WARN";
			break;
		case WISK_LOG_DEBUG:
			prefix = "WISK_DEBUG";
			break;
		case WISK_LOG_TRACE:
			prefix = "WISK_TRACE";
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
 * WISK LOADING LIBC FUNCTIONS
 *********************************************************/

#include <dlfcn.h>

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
typedef int (*__libc_execve)(const char *pathname, char *const argv[], char *const envp[]);
typedef int (*__libc_execveat)(int dirfd, const char *pathname, char *const argv[], char *const envp[], int flags);

#define WISK_SYMBOL_ENTRY(i) \
	union { \
		__libc_##i f; \
		void *obj; \
	} _libc_##i

struct wisk_libc_symbols {
	WISK_SYMBOL_ENTRY(fcntl);
	WISK_SYMBOL_ENTRY(fopen);
#ifdef HAVE_FOPEN64
	WISK_SYMBOL_ENTRY(fopen64);
#endif
	WISK_SYMBOL_ENTRY(open);
#ifdef HAVE_OPEN64
	WISK_SYMBOL_ENTRY(open64);
#endif
	WISK_SYMBOL_ENTRY(openat);
 	WISK_SYMBOL_ENTRY(execve);
 	WISK_SYMBOL_ENTRY(execveat);
};

struct wisk {
	struct {
		void *handle;
		void *socket_handle;
		struct wisk_libc_symbols symbols;
	} libc;
};

static struct wisk wisk;

/* prototypes */
static char *fs_tracker_pipe_getpath(void);

#define LIBC_NAME "libc.so"

enum wisk_lib {
    WISK_LIBC,
    WISK_LIBNSL,
    WISK_LIBSOCKET,
};

static const char *wisk_str_lib(enum wisk_lib lib)
{
	switch (lib) {
	case WISK_LIBC:
		return "libc";
	case WISK_LIBNSL:
		return "libnsl";
	case WISK_LIBSOCKET:
		return "libsocket";
	}

	/* Compiler would warn us about unhandled enum value if we get here */
	return "unknown";
}

static void *wisk_load_lib_handle(enum wisk_lib lib)
{
	int flags = RTLD_LAZY;
	void *handle = NULL;
	int i;

#ifdef RTLD_DEEPBIND
	const char *env_preload = getenv(LD_PRELOAD);
	const char *env_deepbind = getenv(WISK_TRACKER_DISABLE_DEEPBIND);
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
	case WISK_LIBNSL:
	case WISK_LIBSOCKET:
#ifdef HAVE_LIBSOCKET
		handle = wisk.libc.socket_handle;
		if (handle == NULL) {
			for (i = 10; i >= 0; i--) {
				char soname[256] = {0};

				snprintf(soname, sizeof(soname), "libsocket.so.%d", i);
				handle = dlopen(soname, flags);
				if (handle != NULL) {
					break;
				}
			}

			wisk.libc.socket_handle = handle;
		}
		break;
#endif
	case WISK_LIBC:
		handle = wisk.libc.handle;
#ifdef LIBC_SO
		if (handle == NULL) {
			handle = dlopen(LIBC_SO, flags);

			wisk.libc.handle = handle;
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

			wisk.libc.handle = handle;
		}
		break;
	}

	if (handle == NULL) {
#ifdef RTLD_NEXT
		handle = wisk.libc.handle = wisk.libc.socket_handle = RTLD_NEXT;
#else
		WISK_LOG(WISK_LOG_ERROR,
			  "Failed to dlopen library: %s\n",
			  dlerror());
		exit(-1);
#endif
	}

	return handle;
}

static void *_wisk_bind_symbol(enum wisk_lib lib, const char *fn_name)
{
	void *handle;
	void *func;

	handle = wisk_load_lib_handle(lib);

	func = dlsym(handle, fn_name);
	if (func == NULL) {
		WISK_LOG(WISK_LOG_ERROR,
			  "Failed to find %s: %s\n",
			  fn_name,
			  dlerror());
		exit(-1);
	}

	WISK_LOG(WISK_LOG_TRACE,
		  "Loaded %s from %s",
		  fn_name,
		  wisk_str_lib(lib));

	return func;
}

static void wisk_mutex_lock(pthread_mutex_t *mutex)
{
	int ret;

	ret = pthread_mutex_lock(mutex);
	if (ret != 0) {
		WISK_LOG(WISK_LOG_ERROR, "Couldn't lock pthread mutex - %s",
			  strerror(ret));
	}
}

static void wisk_mutex_unlock(pthread_mutex_t *mutex)
{
	int ret;

	ret = pthread_mutex_unlock(mutex);
	if (ret != 0) {
		WISK_LOG(WISK_LOG_ERROR, "Couldn't unlock pthread mutex - %s",
			  strerror(ret));
	}
}

/*
 * These macros have a thread race condition on purpose!
 *
 * This is an optimization to avoid locking each time we check if the symbol is
 * bound.
 */
#define wisk_bind_symbol_libc(sym_name) \
	if (wisk.libc.symbols._libc_##sym_name.obj == NULL) { \
		wisk_mutex_lock(&libc_symbol_binding_mutex); \
		if (wisk.libc.symbols._libc_##sym_name.obj == NULL) { \
			wisk.libc.symbols._libc_##sym_name.obj = \
				_wisk_bind_symbol(WISK_LIBC, #sym_name); \
		} \
		wisk_mutex_unlock(&libc_symbol_binding_mutex); \
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

static int libc_execve(const char *pathname, char *const argv[], char *const envp[])
{
    int i;

	WISK_LOG(WISK_LOG_TRACE, "static libc_execve(%s)", pathname);
	wisk_bind_symbol_libc(execve);

	return wisk.libc.symbols._libc_execve.f(pathname, argv, envp);
}

static int libc_execveat(int dirfd, const char *pathname, char *const argv[], char *const envp[], int flags)
{
    int i;

	WISK_LOG(WISK_LOG_TRACE, "static libc_execveat(%s)", pathname);
	wisk_bind_symbol_libc(execveat);

	return wisk.libc.symbols._libc_execveat.f(dirfd, pathname, argv, envp, flags);
}

static FILE *libc_fopen(const char *name, const char *mode)
{
	WISK_LOG(WISK_LOG_TRACE, "static libc_fopen(%s, %s)", name, mode);
	wisk_bind_symbol_libc(fopen);

	return wisk.libc.symbols._libc_fopen.f(name, mode);
}

#ifdef HAVE_FOPEN64
static FILE *libc_fopen64(const char *name, const char *mode)
{
	WISK_LOG(WISK_LOG_TRACE, "static libc_fopen64(%s, %s)", name, mode);
	wisk_bind_symbol_libc(fopen64);

	return wisk.libc.symbols._libc_fopen64.f(name, mode);
}
#endif /* HAVE_FOPEN64 */

static int libc_vopen(const char *pathname, int flags, va_list ap)
{
	int mode = 0;
	int fd;

	WISK_LOG(WISK_LOG_TRACE, "static libc_vopen: %s", pathname);
	wisk_bind_symbol_libc(open);

	if (flags & O_CREAT) {
		mode = va_arg(ap, int);
	}
	fd = wisk.libc.symbols._libc_open.f(pathname, flags, (mode_t)mode);

	return fd;
}

static int libc_open(const char *pathname, int flags, ...)
{
	va_list ap;
	int fd;

	WISK_LOG(WISK_LOG_TRACE, "static libc_open: %s", pathname);
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

	WISK_LOG(WISK_LOG_TRACE, "static libc_vopen64(%s, %d)", pathname, flags);
	wisk_bind_symbol_libc(open64);

	if (flags & O_CREAT) {
		mode = va_arg(ap, int);
	}
	fd = wisk.libc.symbols._libc_open64.f(pathname, flags, (mode_t)mode);

	return fd;
}
#endif /* HAVE_OPEN64 */

static int libc_vopenat(int dirfd, const char *path, int flags, va_list ap)
{
	int mode = 0;
	int fd;

	WISK_LOG(WISK_LOG_TRACE, "static libc_vopenat(%d, %s, %d)", dirfd, path, flags);
	wisk_bind_symbol_libc(openat);

	if (flags & O_CREAT) {
		mode = va_arg(ap, int);
	}
	fd = wisk.libc.symbols._libc_openat.f(dirfd,
					       path,
					       flags,
					       (mode_t)mode);

	return fd;
}

/* DO NOT call this function during library initialization! */
static void wisk_bind_symbol_all(void)
{
	wisk_bind_symbol_libc(fopen);
#ifdef HAVE_FOPEN64
	wisk_bind_symbol_libc(fopen64);
#endif
	wisk_bind_symbol_libc(open);
#ifdef HAVE_OPEN64
	wisk_bind_symbol_libc(open64);
#endif
	wisk_bind_symbol_libc(openat);
 	wisk_bind_symbol_libc(execve);
 	wisk_bind_symbol_libc(execveat);
}

/*********************************************************
 * SWRAP HELPER FUNCTIONS
 *********************************************************/

static char *fs_tracker_pipe_getpath(void)
{
	char *wisk_pipe_path = NULL;
	char *s = getenv(WISK_TRACKER_PIPE);

	if (s == NULL) {
		WISK_LOG(WISK_LOG_WARN, "WISK_TRACKER_PIPE not set\n");
		return NULL;
	}

	wisk_pipe_path = realpath(s, NULL);
	if (wisk_pipe_path == NULL) {
		WISK_LOG(WISK_LOG_ERROR,
			  "Unable to resolve fs_wrapper pipe path: %s", s);
		return NULL;
	}

	return wisk_pipe_path;
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

static void wisk_env_add(char *var, int *count)
{
    char *s = getenv(var);
    int len;
    if (s) {
        len = strlen(var)+strlen(s) + 2;
        wisk_envp[*count] = malloc(len);
        snprintf(wisk_envp[*count], len, "%s=%s", var, s);
        *count++;
    }
}

static void fs_tracker_init_pipe(char *fs_tracker_pipe_path)
{
	char *uuid;
	int ret, i;

	wisk_mutex_lock(&fs_tracker_pipe_mutex);
	uuid = getenv(WISK_TRACKER_UUID);
    if (uuid) {
        strncpy(fs_tracker_uuid, uuid, UUID_SIZE);
    } else {
        strncpy(fs_tracker_uuid, "UNDEFINED_UUID", UUID_SIZE);
    }

    if (wisk.libc.symbols._libc_open.f) {
	    WISK_LOG(WISK_LOG_TRACE, "Tracker Recieve Pipe Real open(%s), UUID=%s", fs_tracker_pipe_path, fs_tracker_uuid);
        fs_tracker_pipe = wisk.libc.symbols._libc_open.f(fs_tracker_pipe_path, O_WRONLY);
    } else {
	    WISK_LOG(WISK_LOG_TRACE, "Tracker Recieve Pipe: Local open(%s), UUID=%s", fs_tracker_pipe_path, fs_tracker_uuid);
        fs_tracker_pipe = libc_open(fs_tracker_pipe_path, O_WRONLY);
    }
	if (fs_tracker_pipe < 0) {
		WISK_LOG(WISK_LOG_ERROR, "File System Tracker Pipe %s cannot be opened for write\n", fs_tracker_pipe_path);
	}
    wisk_env_count=0;
    wisk_env_add(LD_PRELOAD, &wisk_env_count);
    wisk_env_add(WISK_TRACKER_PIPE, &wisk_env_count);
    wisk_env_add(WISK_TRACKER_DEBUGLEVEL, &wisk_env_count);
    wisk_env_add(WISK_TRACKER_UUID, &wisk_env_count);
    for(i=0; i<wisk_env_count; i++) {
	    WISK_LOG(WISK_LOG_TRACE, "WISK_ENV[%s]", wisk_envp[i]);
    }

done:
	wisk_mutex_unlock(&fs_tracker_pipe_mutex);
}

bool fs_tracker_enabled(void)
{
	char *s;

	if (fs_tracker_pipe >= 0) {
		return true;
	}
	s = fs_tracker_pipe_getpath();
	if (s == NULL) {
		return false;
	}

	fs_tracker_init_pipe(s);

	SAFE_FREE(s);

	WISK_LOG(WISK_LOG_TRACE, "File System Tracker Enabled\n\n");

	return true;
}

void wisk_write(const char *fname)
{
    char msgbuffer[BUFFER_SIZE];
    int msglen;

	if (fs_tracker_enabled()) {
        msglen = snprintf(msgbuffer, BUFFER_SIZE, "%s: Writes %s\n", fs_tracker_uuid, fname);
        write(fs_tracker_pipe, msgbuffer, msglen); 
    } else {
        WISK_LOG(WISK_LOG_TRACE, "Writes %s", fname);
	}
}

void wisk_read(const char *fname)
{
    char msgbuffer[BUFFER_SIZE];
    int msglen;

	if (fs_tracker_enabled()) {
        msglen = snprintf(msgbuffer, BUFFER_SIZE, "%s: Reads %s\n", fs_tracker_uuid, fname);
        write(fs_tracker_pipe, msgbuffer, msglen); 
    } else {
        WISK_LOG(WISK_LOG_TRACE, "Reads %s", fname);
	}
}

/****************************************************************************
 *   FOPEN
 ***************************************************************************/

static FILE *wisk_fopen(const char *name, const char *mode)
{
    WISK_LOG(WISK_LOG_TRACE, "wisk_fopen(%s, %s)", name, mode);
    if (strlen(mode)==2) {
        wisk_read(name);
        wisk_write(name);
    } else if ((mode[0] == 'w') || (mode[0] == 'a')) {
        wisk_write(name);
    } else {
        wisk_read(name);
    }
	return libc_fopen(name, mode);
}

FILE *fopen(const char *name, const char *mode)
{
    WISK_LOG(WISK_LOG_TRACE, "fopen(%s, %s)", name, mode);
	return wisk_fopen(name, mode);
}

/****************************************************************************
 *   FOPEN64
 ***************************************************************************/

#ifdef HAVE_FOPEN64
static FILE *wisk_fopen64(const char *name, const char *mode)
{
	FILE *fp;

    WISK_LOG(WISK_LOG_TRACE, "wisk_fopen64(%s, %s)", name, mode);
	fp = libc_fopen64(name, mode);
	if (fp != NULL) {
		int fd = fileno(fp);
	}

	return fp;
}

FILE *fopen64(const char *name, const char *mode)
{
    WISK_LOG(WISK_LOG_TRACE, "fopen64(%s, %s)", name, mode);
	return wisk_fopen64(name, mode);
}
#endif /* HAVE_FOPEN64 */

/****************************************************************************
 *   OPEN
 ***************************************************************************/

static int wisk_vopen(const char *pathname, int flags, va_list ap)
{
	int ret;

    WISK_LOG(WISK_LOG_TRACE, "wisk_vopen(%s, %d)", pathname, flags);
    if ((flags & O_WRONLY) && (flags & O_RDONLY)) {
        wisk_read(pathname);
        wisk_write(pathname);
    } else if (flags & O_WRONLY) {
        wisk_write(pathname);
    } else if (flags & O_RDONLY) {
        wisk_read(pathname);
    } else {
        wisk_read(pathname);
    }
	ret = libc_vopen(pathname, flags, ap);
	if (ret != -1) {
	}
	return ret;
}

int open(const char *pathname, int flags, ...)
{
	va_list ap;
	int fd;

    WISK_LOG(WISK_LOG_TRACE, "open(%s, %d)", pathname, flags);
	va_start(ap, flags);
	fd = wisk_vopen(pathname, flags, ap);
	va_end(ap);

	return fd;
}

/****************************************************************************
 *   OPEN64
 ***************************************************************************/

#ifdef HAVE_OPEN64
static int wisk_vopen64(const char *pathname, int flags, va_list ap)
{
	int ret;

    WISK_LOG(WISK_LOG_TRACE, "wisk_vopen64(%s, %d)", pathname, flags);
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

    WISK_LOG(WISK_LOG_TRACE, "open64(%s, %d)", pathname, flags);
	va_start(ap, flags);
	fd = wisk_vopen64(pathname, flags, ap);
	va_end(ap);

	return fd;
}
#endif /* HAVE_OPEN64 */

/****************************************************************************
 *   OPENAT
 ***************************************************************************/

static int wisk_vopenat(int dirfd, const char *path, int flags, va_list ap)
{
	int ret;

    WISK_LOG(WISK_LOG_TRACE, "wisk_vopenat(%d, %s, %d)", dirfd, path, flags);
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

    WISK_LOG(WISK_LOG_TRACE, "openat(%d, %s, %d)", dirfd, path, flags);
	va_start(ap, flags);
	fd = wisk_vopenat(dirfd, path, flags, ap);
	va_end(ap);

	return fd;
}

/****************************************************************************
 *   EXECVE
 ***************************************************************************/

static int envcmp(const char *env, const char *var)
{
    int len;
    len = strlen(var);
    return (strncmp(env, var, len) == 0 && env[len] == '=');
}

static int wisk_getvarcount(char *const envp[])
{
    int envc, i;

    for (i=0, envc=1; envp[i]; i++) {
      if (envcmp(envp[i], LD_PRELOAD) ||
          envcmp(envp[i], WISK_TRACKER_PIPE) ||
          envcmp(envp[i], WISK_TRACKER_DEBUGLEVEL) ||
          envcmp(envp[i], WISK_TRACKER_UUID)) {
          WISK_LOG(WISK_LOG_TRACE, "Skipping Environment %d: %s", i, envp[i]);
          continue;
        }
      WISK_LOG(WISK_LOG_TRACE, "Environment %d: %s", i, envp[i]);
      envc++;
    }
    WISK_LOG(WISK_LOG_TRACE, "Var Count: %d",  envc);
    return envc;
}

static void wisk_loadenv(char *const envp[], char *nenvp[])
{
    int envi, i;

    for(envi=0; envi < wisk_env_count; envi++)
        nenvp[envi] = wisk_envp[envi];
    for (i=0; envp[i]; i++) {
      if (envcmp(envp[i], LD_PRELOAD) ||
          envcmp(envp[i], WISK_TRACKER_PIPE) ||
          envcmp(envp[i], WISK_TRACKER_DEBUGLEVEL) ||
          envcmp(envp[i], WISK_TRACKER_UUID))
          continue;
      nenvp[envi++] = envp[i];
    }
    nenvp[envi++] = envp[i];
    for(i=0; nenvp[i]; i++) {
        WISK_LOG(WISK_LOG_TRACE, "Environment %d: %s", i, nenvp[i]);
    }
    WISK_LOG(WISK_LOG_TRACE, "Environment %d: %s", i, nenvp[i]);
}

static int wisk_execve(const char *pathname, char *const argv[], char *const envp[])
{
    int i;
    WISK_LOG(WISK_LOG_TRACE, "wisk_execve(%s)", pathname);
    /* Avoid dynamic memory allocation due two main issues:
       1. The function should be async-signal-safe and a running on a signal
          handler with a fail outcome might lead to malloc bad state.
       2. It might be used in a vfork/clone(VFORK) scenario where using
          malloc also might lead to internal bad state.  */
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(envp) + wisk_env_count + 1];
        wisk_loadenv(envp, nenvp);
	    return libc_execve(pathname, argv, nenvp);
    } else
	    return libc_execve(pathname, argv, envp);
}

int execve(const char *pathname, char *const argv[], char *const envp[])
{
    WISK_LOG(WISK_LOG_TRACE, "execve(%s)", pathname);
	return wisk_execve(pathname, argv, envp);
}

static int wisk_execveat(int dirfd, const char *pathname, char *const argv[], char *const envp[], int flags)
{
    WISK_LOG(WISK_LOG_TRACE, "wisk_execveat(%s)", pathname);
	return libc_execveat(dirfd, pathname, argv, envp, flags);
}

int execveat(int dirfd, const char *pathname, char *const argv[], char *const envp[], int flags)
{
    WISK_LOG(WISK_LOG_TRACE, "wisk_execveat(%s)", pathname);
	return wisk_execveat(dirfd, pathname, argv, envp, flags);
}

/****************************
 * Thread safe code
 ***************************/
static void wisk_thread_prepare(void)
{
	WISK_LOG(WISK_LOG_TRACE, "wisk_thread_prepare: ");
	/*
	 * This function should only be called here!!
	 *
	 * We bind all symobls to avoid deadlocks of the fork is
	 * interrupted by a signal handler using a symbol of this
	 * library.
	 */
	wisk_bind_symbol_all();

	WISK_LOCK_ALL;
}

static void wisk_thread_parent(void)
{
	WISK_LOG(WISK_LOG_TRACE, "wisk_thread_parent: ");
	WISK_UNLOCK_ALL;
}

static void wisk_thread_child(void)
{
	WISK_LOG(WISK_LOG_TRACE, "wisk_thread_child: ");
	WISK_UNLOCK_ALL;
}

/****************************
 * CONSTRUCTOR
 ***************************/
void wisk_constructor(void)
{
	int ret;

	WISK_LOG(WISK_LOG_TRACE, "Constructor ");
	/*
	* If we hold a lock and the application forks, then the child
	* is not able to unlock the mutex and we are in a deadlock.
	* This should prevent such deadlocks.
	*/
	pthread_atfork(&wisk_thread_prepare,
		       &wisk_thread_parent,
		       &wisk_thread_child);

	ret = fs_tracker_init_mutex(&fs_tracker_pipe_mutex);
	if (ret != 0) {
		WISK_LOG(WISK_LOG_ERROR,
			  "Failed to initialize pthread mutex");
		exit(-1);
	}
	fs_tracker_enabled();
}

/****************************
 * DESTRUCTOR
 ***************************/

/*
 * This function is called when the library is unloaded and makes sure that
 * sockets get closed and the unix file for the socket are unlinked.
 */
void wisk_destructor(void)
{
	size_t i;

	if (wisk.libc.handle != NULL) {
		dlclose(wisk.libc.handle);
	}
	if (wisk.libc.socket_handle) {
		dlclose(wisk.libc.socket_handle);
	}
	WISK_LOG(WISK_LOG_TRACE, "Destructor ");
}
