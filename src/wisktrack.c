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
   WISK Filesystem Tracker library. Tracks all filesystem operations and logs it 
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
#include <spawn.h>
#include <uuid/uuid.h>

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

// Environment Variables
#define LD_PRELOAD "LD_PRELOAD"
#define WISK_TRACKER_UUID "WISK_TRACKER_UUID"
#define WISK_TRACKER_DEBUGLEVEL "WISK_TRACKER_DEBUGLEVEL"
#define WISK_TRACKER_PIPE "WISK_TRACKER_PIPE"
#define WISK_TRACKER_DISABLE_DEEPBIND "WISK_TRACKER_DISABLE_DEEPBIND"

char *wisk_env_vars[] = {
	LD_PRELOAD,
	WISK_TRACKER_UUID,
	WISK_TRACKER_DEBUGLEVEL,
	WISK_TRACKER_PIPE,
	WISK_TRACKER_DISABLE_DEEPBIND
};

#define VNAME(x) wisk_env_vars[x]

#define WISK_ENV_VARCOUNT (sizeof((wisk_env_vars))/sizeof((wisk_env_vars)[0]))

static char *wisk_envp[WISK_ENV_VARCOUNT];
static char *wisk_env_uuid;
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
//typedef int (*__libc_execl)(const char *path, char *const arg, ...);
//typedef int (*__libc_execlp)(const char *file, char *const arg, ...);
//typedef int (*__libc_execlpe)(const char *path, const char *arg,..., char * const envp[]);
typedef int (*__libc_execv)(const char *path, char *const argv[]);
typedef int (*__libc_execvp)(const char *file, char *const argv[]);
typedef int (*__libc_execvpe)(const char *file, char *const argv[], char *const envp[]);
typedef int (*__libc_execve)(const char *pathname, char *const argv[], char *const envp[]);
typedef int (*__libc_execveat)(int dirfd, const char *pathname, char *const argv[], char *const envp[], int flags);
typedef int (*__libc_posix_spawn)(pid_t *pid, const char *path, const posix_spawn_file_actions_t *file_actions,
		                          const posix_spawnattr_t *attrp, char *const argv[], char *const envp[]);
typedef int (*__libc_posix_spawnp)(pid_t *pid, const char *file, const posix_spawn_file_actions_t *file_actions,
                                   const posix_spawnattr_t *attrp, char *const argv[], char * const envp[]);

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
// 	WISK_SYMBOL_ENTRY(execl);
// 	WISK_SYMBOL_ENTRY(execlp);
// 	WISK_SYMBOL_ENTRY(execlpe);
	WISK_SYMBOL_ENTRY(openat);
 	WISK_SYMBOL_ENTRY(execv);
 	WISK_SYMBOL_ENTRY(execvp);
 	WISK_SYMBOL_ENTRY(execvpe);
 	WISK_SYMBOL_ENTRY(execve);
 	WISK_SYMBOL_ENTRY(execveat);
 	WISK_SYMBOL_ENTRY(posix_spawn);
 	WISK_SYMBOL_ENTRY(posix_spawnp);
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

static int libc_vexeclpe(const char *path, const char *arg, va_list ap, int argcount, char *const envp[])
{
	int i;
	char *argv[argcount+1];

	argv[0] = (char *const)arg;
	if (argcount) {
		for(i=1; i<argcount+1; i++)
			argv[i] = va_arg(ap, char *);
	}
	WISK_LOG(WISK_LOG_TRACE, "static libc_vexeclpe(%s)", path);
	wisk_bind_symbol_libc(execvpe);

	return wisk.libc.symbols._libc_execvpe.f(path, argv, envp);
}

static int libc_vexecle(const char *file, const char *arg, va_list ap, int argcount, char *const envp[])
{
	int i;
	char *argv[argcount+1];

	argv[0] = (char *const)arg;
	if (argcount) {
		for(i=1; i<argcount+1; i++)
			argv[i] = va_arg(ap, char *);
	}
	WISK_LOG(WISK_LOG_TRACE, "static libc_vexecle(%s)", file);
	wisk_bind_symbol_libc(execve);

	return wisk.libc.symbols._libc_execve.f(file, argv, envp);
}

static int libc_execv(const char *path, char *const argv[])
{
	WISK_LOG(WISK_LOG_TRACE, "static libc_execv(%s)", path);
	wisk_bind_symbol_libc(execv);

	return wisk.libc.symbols._libc_execv.f(path, argv);
}

static int libc_execvp(const char *file, char *const argv[])
{
	WISK_LOG(WISK_LOG_TRACE, "static libc_execvp(%s)", file);
	wisk_bind_symbol_libc(execvp);

	return wisk.libc.symbols._libc_execvp.f(file, argv);
}

static int libc_execvpe(const char *file, char *const argv[], char *const envp[])
{
	WISK_LOG(WISK_LOG_TRACE, "static libc_execvpe(%s)", file);
	wisk_bind_symbol_libc(execvpe);

	return wisk.libc.symbols._libc_execvpe.f(file, argv, envp);
}

static int libc_execve(const char *pathname, char *const argv[], char *const envp[])
{
	WISK_LOG(WISK_LOG_TRACE, "static libc_execve(%s)", pathname);
	wisk_bind_symbol_libc(execve);

	return wisk.libc.symbols._libc_execve.f(pathname, argv, envp);
}

static int libc_execveat(int dirfd, const char *pathname, char *const argv[], char *const envp[], int flags)
{
	WISK_LOG(WISK_LOG_TRACE, "static libc_execveat(%s)", pathname);
	wisk_bind_symbol_libc(execveat);

	return wisk.libc.symbols._libc_execveat.f(dirfd, pathname, argv, envp, flags);
}

static int libc_posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *file_actions,
		                    const posix_spawnattr_t *attrp, char *const argv[], char *const envp[])
{
	WISK_LOG(WISK_LOG_TRACE, "static libc_posix_spawn(%s)", path);
	wisk_bind_symbol_libc(posix_spawn);

	return wisk.libc.symbols._libc_posix_spawn.f(pid, path, file_actions, attrp, argv, envp);
}

static int libc_posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *file_actions,
                             const posix_spawnattr_t *attrp, char *const argv[], char * const envp[])

{
	WISK_LOG(WISK_LOG_TRACE, "static libc_posix_spawnp(%s)", file);
	wisk_bind_symbol_libc(posix_spawnp);

	return wisk.libc.symbols._libc_posix_spawnp.f(pid, file, file_actions, attrp, argv, envp);
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
// 	wisk_bind_symbol_libc(execl);
// 	wisk_bind_symbol_libc(execlp);
// 	wisk_bind_symbol_libc(execlpe);
 	wisk_bind_symbol_libc(execv);
 	wisk_bind_symbol_libc(execvp);
 	wisk_bind_symbol_libc(execvpe);
 	wisk_bind_symbol_libc(execve);
 	wisk_bind_symbol_libc(execveat);
 	wisk_bind_symbol_libc(posix_spawn);
 	wisk_bind_symbol_libc(posix_spawnp);
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
    char *s;
    int len;
    s = getenv(var);
    if (s) {
        len = strlen(var)+strlen(s) + 2;
        wisk_envp[*count] = malloc(len);
        snprintf(wisk_envp[*count], len, "%s=%s", var, s);
        if (strcmp(var, WISK_TRACKER_UUID)==0) {
            wisk_env_uuid = wisk_envp[*count] + strlen(WISK_TRACKER_UUID) + 2;
        }
        (*count)++;
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
	for(wisk_env_count=0, i=0; i< WISK_ENV_VARCOUNT; i++)
        wisk_env_add(wisk_env_vars[i], &wisk_env_count);
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

void wisk_report_write(const char *fname)
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

void wisk_report_read(const char *fname)
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
        wisk_report_read(name);
        wisk_report_write(name);
    } else if ((mode[0] == 'w') || (mode[0] == 'a')) {
        wisk_report_write(name);
    } else {
        wisk_report_read(name);
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
    if (strlen(mode)==2) {
        wisk_report_read(name);
        wisk_report_write(name);
    } else if ((mode[0] == 'w') || (mode[0] == 'a')) {
        wisk_report_write(name);
    } else {
        wisk_report_read(name);
    }
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
        wisk_report_read(pathname);
        wisk_report_write(pathname);
    } else if (flags & O_WRONLY) {
        wisk_report_write(pathname);
    } else if (flags & O_RDONLY) {
        wisk_report_read(pathname);
    } else {
        wisk_report_read(pathname);
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
    if ((flags & O_WRONLY) && (flags & O_RDONLY)) {
        wisk_report_read(pathname);
        wisk_report_write(pathname);
    } else if (flags & O_WRONLY) {
        wisk_report_write(pathname);
    } else if (flags & O_RDONLY) {
        wisk_report_read(pathname);
    } else {
        wisk_report_read(pathname);
    }
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

static int wisk_isenv(const char *env)
{
	int i;
	for(i=0; i< WISK_ENV_VARCOUNT; i++)
		if (envcmp(env, wisk_env_vars[i]))
			return true;
	return false;
}

static int wisk_getvarcount(char *const envp[])
{
    int envc, i;

    for (i=0, envc=1; envp[i]; i++) {
      if (wisk_isenv(envp[i])) {
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
    int envi=0, i;
    uuid_t uuid;

	uuid_generate(uuid);
	uuid_unparse(uuid, wisk_env_uuid);
	for(envi=0; envi < wisk_env_count; envi++)
		nenvp[envi] = wisk_envp[envi];
    for (i=0; envp[i]; i++) {
      if (wisk_isenv(envp[i]))
          continue;
      nenvp[envi++] = envp[i];
    }
    nenvp[envi++] = envp[i];
	for(i=0; nenvp[i]; i++) {
		WISK_LOG(WISK_LOG_TRACE, "Environment %d: %s", i, nenvp[i]);
	}
}

void  wisk_report_command(const char *pathname, char *const argv[], char *const envp[])
{
    int i, msglen;
    char msgbuffer[BUFFER_SIZE];

    msglen = snprintf(msgbuffer, BUFFER_SIZE, "%s: calls %s\n", fs_tracker_uuid, wisk_env_uuid);
    write(fs_tracker_pipe, msgbuffer, msglen); 
    msglen = snprintf(msgbuffer, BUFFER_SIZE, "%s: command-path %s\n", wisk_env_uuid, pathname);
    write(fs_tracker_pipe, msgbuffer, msglen); 
    msglen = snprintf(msgbuffer, BUFFER_SIZE, "%s: command [", wisk_env_uuid);
    for(i=0; argv[i]; i++)
        msglen += snprintf(msgbuffer+msglen, BUFFER_SIZE-msglen, "\"%s\" ", argv[i]);
    msglen += snprintf(msgbuffer+msglen, BUFFER_SIZE-msglen, "]\n");
    write(fs_tracker_pipe, msgbuffer, msglen); 
    msglen = snprintf(msgbuffer, BUFFER_SIZE, "%s: environment [", wisk_env_uuid);
    for(i=wisk_env_count; envp[i]; i++)
        msglen += snprintf(msgbuffer+msglen, BUFFER_SIZE-msglen, "\"%s\", ", envp[i]);
    msglen += snprintf(msgbuffer+msglen, BUFFER_SIZE-msglen, "]\n");
    write(fs_tracker_pipe, msgbuffer, msglen); 
}

static int wisk_vexecl(const char *file, const char *arg, va_list ap, int argcount)
{
    char *nenvp[wisk_getvarcount(environ) + wisk_env_count + 1];

    WISK_LOG(WISK_LOG_TRACE, "wisk_vexecl(%s)", file);
	if (fs_tracker_enabled()) {
        wisk_loadenv(environ, nenvp);
//        wisk_report_command(file, arg, nenvp);
	    return libc_vexecle(file, arg, ap, argcount, nenvp);
    } else {
	    return libc_vexecle(file, arg, ap, argcount, environ);
    }
}

int execl(const char *file, const char *arg, ...)
{
	va_list ap;
	int argcount=0, rv;
	if (arg) {
	    va_start(ap, arg);
		for(argcount=1; va_arg(ap, char *) != NULL; argcount++);
		va_end(ap);
	}
    WISK_LOG(WISK_LOG_TRACE, "execl(%s)", file);
    va_start(ap, arg);
    rv = wisk_vexecl(file, arg, ap, argcount);
    va_end(ap);
    return rv;
}

static int wisk_vexeclp(const char *file, const char *arg, va_list ap, int argcount)
{
    char *nenvp[wisk_getvarcount(environ) + wisk_env_count + 1];

    WISK_LOG(WISK_LOG_TRACE, "wisk_vexeclp(%s)", file);
	if (fs_tracker_enabled()) {
        wisk_loadenv(environ, nenvp);
//        wisk_report_command(file, arg, nenvp);
	    return libc_vexeclpe(file, arg, ap, argcount, nenvp);
    } else {
	    return libc_vexeclpe(file, arg, ap, argcount, environ);
    }
}

int execlp(const char *file, const char *arg, ...)
{
	va_list ap;
	int argcount=0, rv;
	if (arg) {
	    va_start(ap, arg);
		for(argcount=1; va_arg(ap, char *) != NULL; argcount++);
		va_end(ap);
	}
    WISK_LOG(WISK_LOG_TRACE, "execlp(%s)", file);
    va_start(ap, arg);
    rv = wisk_vexeclp(file, arg, ap, argcount);
    va_end(ap);
    return rv;
}

static int wisk_vexeclpe(const char *file, const char *arg, va_list ap, int argcount, char *const envp[])
{
    WISK_LOG(WISK_LOG_TRACE, "wisk_vexeclpe(%s)", file);
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(envp) + wisk_env_count + 1];
        wisk_loadenv(envp, nenvp);
//        wisk_report_command(file, argv, nenvp);
	    return libc_vexeclpe(file, arg, ap, argcount, nenvp);
    } else
	    return libc_vexeclpe(file, arg, ap, argcount, envp);
}

int execlpe(const char *file, const char *arg, ...)
{
	va_list ap;
	int argcount=0, rv;
	char **envp;

	if (arg) {
	    va_start(ap, arg);
		for(argcount=1; va_arg(ap, char *) != NULL; argcount++);
		envp = (char **)va_arg(ap, char *);
		va_end(ap);
	}
    WISK_LOG(WISK_LOG_TRACE, "execlpe(%s)", file);
    va_start(ap, arg);
    rv = wisk_vexeclpe(file, arg, ap, argcount, envp);
    va_end(ap);
    return rv;
}

static int wisk_execv(const char *path, char *const argv[])
{
    WISK_LOG(WISK_LOG_TRACE, "wisk_execv(%s)", path);
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(environ) + wisk_env_count + 1];
        wisk_loadenv(environ, nenvp);
        wisk_report_command(path, argv, nenvp);
	    return libc_execvpe(path, argv, nenvp);
    } else
	    return libc_execvpe(path, argv, environ);
}

int execv(const char *path, char *const argv[])
{
    WISK_LOG(WISK_LOG_TRACE, "execv(%s)", path);
	return wisk_execv(path, argv);
}

static int wisk_execvp(const char *file, char *const argv[])
{
    WISK_LOG(WISK_LOG_TRACE, "wisk_execvp(%s)", file);
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(environ) + wisk_env_count + 1];
        wisk_loadenv(environ, nenvp);
        wisk_report_command(file, argv, nenvp);
	    return libc_execvpe(file, argv, nenvp);
    } else
	    return libc_execvpe(file, argv, environ);
}

int execvp(const char *file, char *const argv[])
{
    WISK_LOG(WISK_LOG_TRACE, "execvp(%s)", file);
	return wisk_execvp(file, argv);
}

static int wisk_execvpe(const char *file, char *const argv[], char *const envp[])
{
    WISK_LOG(WISK_LOG_TRACE, "wisk_execvpe(%s)", file);
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(envp) + wisk_env_count + 1];
        wisk_loadenv(envp, nenvp);
        wisk_report_command(file, argv, nenvp);
	    return libc_execvpe(file, argv, nenvp);
    } else
	    return libc_execvpe(file, argv, envp);
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
    WISK_LOG(WISK_LOG_TRACE, "execvpe(%s)", file);
	return wisk_execvpe(file, argv, envp);
}

static int wisk_execve(const char *pathname, char *const argv[], char *const envp[])
{
    WISK_LOG(WISK_LOG_TRACE, "wisk_execve(%s)", pathname);
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(envp) + wisk_env_count + 1];
        wisk_loadenv(envp, nenvp);
        wisk_report_command(pathname, argv, nenvp);
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

static int wisk_posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *file_actions,
		                    const posix_spawnattr_t *attrp, char *const argv[], char *const envp[])
{
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(envp) + wisk_env_count + 1];
        wisk_loadenv(envp, nenvp);
        wisk_report_command(path, argv, nenvp);
	    return libc_posix_spawn(pid, path, file_actions, attrp, argv, nenvp);
    } else
	    return libc_posix_spawn(pid, path, file_actions, attrp, argv, envp);
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *file_actions,
		                    const posix_spawnattr_t *attrp, char *const argv[], char *const envp[])
{
    WISK_LOG(WISK_LOG_TRACE, "posix_spawn(%s)", path);
	return wisk_posix_spawn(pid, path, file_actions, attrp, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *file_actions,
		                    const posix_spawnattr_t *attrp, char *const argv[], char *const envp[])
{
    WISK_LOG(WISK_LOG_TRACE, "posix_spawnp(%s)", file);
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(envp) + wisk_env_count + 1];
        wisk_loadenv(envp, nenvp);
        wisk_report_command(file, argv, nenvp);
	    return libc_posix_spawnp(pid, file, file_actions, attrp, argv, nenvp);
    } else
	    return libc_posix_spawnp(pid, file, file_actions, attrp, argv, envp);
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
