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
#include <sys/timeb.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <errno.h>
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
#include <linux/limits.h>


enum wisk_dbglvl_e {
	WISK_LOG_ERROR = 0,
	WISK_LOG_WARN,
	WISK_LOG_INFO,
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
#define UUID_SIZE 36

#define LD_PRELOAD_SEPARATOR ' '
#define LD_LIBRARY_PATH_SEPARATOR ':'
// Environment Variables
#define LD_PRELOAD "LD_PRELOAD"
#define LD_LIBRARY_PATH "LD_LIBRARY_PATH"
#define WISK_TRACKER_PID "WISK_TRACKER_PID"
#define WISK_TRACKER_UUID "WISK_TRACKER_UUID"
#define WISK_TRACKER_PUUID "WISK_TRACKER_PUUID"
#define WISK_TRACKER_DEBUGLEVEL "WISK_TRACKER_DEBUGLEVEL"
#define WISK_TRACKER_DEBUGLOG "WISK_TRACKER_DEBUGLOG"
#define WISK_TRACKER_DEBUGLOG_FD "WISK_TRACKER_DEBUGLOG_FD"
#define WISK_TRACKER_PIPE "WISK_TRACKER_PIPE"
#define WISK_TRACKER_PIPE_FD "WISK_TRACKER_PIPE_FD"
#define WISK_TRACKER_DISABLE_DEEPBIND "WISK_TRACKER_DISABLE_DEEPBIND"
#define WISK_TRACKER_EVENTFILTER "WISK_TRACKER_EVENTFILTER"

char *wisk_env_vars[] = {
	LD_PRELOAD,
	LD_LIBRARY_PATH,
	WISK_TRACKER_PID,
	WISK_TRACKER_UUID,
	WISK_TRACKER_PUUID,
	WISK_TRACKER_DEBUGLEVEL,
	WISK_TRACKER_DEBUGLOG,
	WISK_TRACKER_DEBUGLOG_FD,
	WISK_TRACKER_PIPE,
	WISK_TRACKER_PIPE_FD,
	WISK_TRACKER_DISABLE_DEEPBIND,
	WISK_TRACKER_EVENTFILTER
};

typedef struct random_uuid_ {
    int i1;
    int i2;
    int i3;
    int i4;
} random_uuid_t;

char *ldload[] = {
		"libwisktrack.so",
		NULL
};
char *preldload[] = {
		"libwisktrack.so",
		NULL
};
char *postldload[] = {
//		"libwisktrack.so",
		NULL
};

enum wisk_eventfilter_e {
	WISK_TRACK_WRITES = 0,
	WISK_TRACK_READS,
	WISK_TRACK_LINKS,
	WISK_TRACK_CHMODS,
	WISK_TRACK_PROCESS
};

#define WISK_TRACK_EVENT(x) (fs_tracker_eventfilter & (1<<(x)))

#define VNAME(x) wisk_env_vars[x]

#define WISK_ENV_VARCOUNT (sizeof((wisk_env_vars))/sizeof((wisk_env_vars)[0]))

static char *wisk_envp[WISK_ENV_VARCOUNT];
// static char *wisk_env_uuid;
static int wisk_env_count=0;
static pid_t fs_tracker_pid = -1;
static int fs_tracker_pipe = -1;
static int fs_tracker_debuglog = -1;
static char fs_tracker_uuid[UUID_SIZE+1];
static char fs_tracker_puuid[UUID_SIZE+1];
static int fs_tracker_eventfilter=0xFFFFFFFF;

/* Mutex to synchronize access to global libc.symbols */
static pthread_mutex_t libc_symbol_binding_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Mutex to guard the initialization of array of fs_info structures */
static pthread_mutex_t fs_tracker_pipe_mutex;

/* Function prototypes */

bool fs_tracker_enabled(void);

char** saved_argv;
int saved_argc;

void wisk_constructor(int argc, char** argv) CONSTRUCTOR_ATTRIBUTE;
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

int fd_is_valid(int fd)
{
    return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

static void wisk_log(enum wisk_dbglvl_e dbglvl, const char *func, const char *format, ...) PRINTF_ATTRIBUTE(3, 4);
# define WISK_LOG(dbglvl, ...) wisk_log((dbglvl), __func__, __VA_ARGS__)

static void wisk_log(enum wisk_dbglvl_e dbglvl,
		      const char *func,
		      const char *format, ...)
{
	char buffer[1024];
	va_list va;
	const char *d;
	int fdout=2;
	unsigned int lvl = 0;
	const char *prefix = "WISK";
	const char *progname = getprogname();

	if (fs_tracker_debuglog != -1 && fs_tracker_debuglog != 2) {
		fdout = fs_tracker_debuglog;
	} else {
		d = getenv(WISK_TRACKER_DEBUGLEVEL);
		if (d != NULL) {
			lvl = atoi(d);
		}

		if (lvl < dbglvl) {
			return;
		}
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
		case WISK_LOG_INFO:
			prefix = "WISK_INFO";
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

	dprintf(fdout,
		"%s[%s (%u:%d:%d:%d)] - %s: %s\n",
		prefix,
		progname,
		(unsigned int)getpid(), fdout, fs_tracker_debuglog, fs_tracker_pipe,
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
typedef FILE *(*__libc_popen)(const char *command, const char *type);
typedef int (*__libc_symlink)(const char *target, const char *linkpath);
typedef int (*__libc_symlinkat)(const char *target, int newdirfd, const char *linkpath);
typedef int (*__libc_link)(const char *oldpath, const char *newpath);
typedef int (*__libc_linkat)(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags);
typedef int (*__libc_unlink)(const char *pathname);
typedef int (*__libc_unlinkat)(int dirfd, const char *pathname, int flags);
typedef int (*__libc_chmod)(__const char *__file, __mode_t __mode);
typedef int (*__libc_fchmod)(int __fd, __mode_t __mode);
typedef int (*__libc_fchmodat)(int __fd, __const char *__file, __mode_t __mode, int flags);

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
 	WISK_SYMBOL_ENTRY(popen);
 	WISK_SYMBOL_ENTRY(symlink);
 	WISK_SYMBOL_ENTRY(symlinkat);
 	WISK_SYMBOL_ENTRY(link);
 	WISK_SYMBOL_ENTRY(linkat);
	WISK_SYMBOL_ENTRY(unlink);
	WISK_SYMBOL_ENTRY(unlinkat);
	WISK_SYMBOL_ENTRY(chmod);
	WISK_SYMBOL_ENTRY(fchmod);
	WISK_SYMBOL_ENTRY(fchmodat);
};

struct wisk {
	struct {
		void *handle;
		void *socket_handle;
		struct wisk_libc_symbols symbols;
	} libc;
};

static struct wisk wisk;

/* LIBC fopen and open for internale use */
__libc_fopen internal_fopen = NULL;
__libc_open internal_open = NULL;


/* prototypes */
static char *fs_tracker_pipe_getpath(void);

#define LIBC_NAME "libc.so"

enum wisk_lib {
    WISK_NONE,
    WISK_LIBC,
    WISK_LIBNSL,
    WISK_LIBSOCKET,
};

static const char *wisk_str_lib(enum wisk_lib lib)
{
	switch (lib) {
	case WISK_NONE:
		return "rtld_next";
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

static void *wisk_load_lib_handle(enum wisk_lib lib, bool nocache)
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
		if(!nocache)
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

			if(!nocache)
				wisk.libc.socket_handle = handle;
		}
		break;
#endif
	case WISK_LIBC:
		if(!nocache)
			handle = wisk.libc.handle;
#ifdef LIBC_SO
		if (handle == NULL) {
			handle = dlopen(LIBC_SO, flags);

			if(!nocache)
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

			if(!nocache)
				wisk.libc.handle = handle;
		}
		break;
	}

	if (handle == NULL) {
#ifdef RTLD_NEXT

		handle = wisk.libc.socket_handle = RTLD_NEXT;
		if(!nocache)
			wisk.libc.handle = handle;
#else
		WISK_LOG(WISK_LOG_ERROR,
			  "Failed to dlopen library: %s\n",
			  dlerror());
		exit(-1);
#endif
	}

	return handle;
}

static void *_wisk_bind_symbol(enum wisk_lib lib, const char *fn_name, bool nocache)
{
	void *handle;
	void *func;

	handle = wisk_load_lib_handle(lib, nocache);

	func = dlsym(handle, fn_name);
	if (func == NULL) {
		WISK_LOG(WISK_LOG_ERROR,
			  "Failed to find %s: %s\n",
			  fn_name,
			  dlerror());
		exit(-1);
	}

	WISK_LOG(WISK_LOG_TRACE,
		  "Loaded %s(%p) from %s",
		  fn_name, func,
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
				_wisk_bind_symbol(WISK_NONE, #sym_name, false); \
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
//	WISK_LOG(WISK_LOG_TRACE, "static libc_vexeclpe(%s)", path);
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
//	WISK_LOG(WISK_LOG_TRACE, "static libc_vexecle(%s)", file);
	wisk_bind_symbol_libc(execve);

	return wisk.libc.symbols._libc_execve.f(file, argv, envp);
}

static int libc_execv(const char *path, char *const argv[])
{
//	WISK_LOG(WISK_LOG_TRACE, "static libc_execv(%s)", path);
	wisk_bind_symbol_libc(execv);

	return wisk.libc.symbols._libc_execv.f(path, argv);
}

static int libc_execvp(const char *file, char *const argv[])
{
//	WISK_LOG(WISK_LOG_TRACE, "static libc_execvp(%s)", file);
	wisk_bind_symbol_libc(execvp);

	return wisk.libc.symbols._libc_execvp.f(file, argv);
}

static int libc_execvpe(const char *file, char *const argv[], char *const envp[])
{
//	WISK_LOG(WISK_LOG_TRACE, "static libc_execvpe(%s)", file);
	wisk_bind_symbol_libc(execvpe);

	return wisk.libc.symbols._libc_execvpe.f(file, argv, envp);
}

static int libc_execve(const char *pathname, char *const argv[], char *const envp[])
{
//	WISK_LOG(WISK_LOG_TRACE, "static libc_execve(%s)", pathname);
	wisk_bind_symbol_libc(execve);

	return wisk.libc.symbols._libc_execve.f(pathname, argv, envp);
}

static int libc_execveat(int dirfd, const char *pathname, char *const argv[], char *const envp[], int flags)
{
//	WISK_LOG(WISK_LOG_TRACE, "static libc_execveat(%s)", pathname);
	wisk_bind_symbol_libc(execveat);

	return wisk.libc.symbols._libc_execveat.f(dirfd, pathname, argv, envp, flags);
}

static int libc_posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *file_actions,
		                    const posix_spawnattr_t *attrp, char *const argv[], char *const envp[])
{
//	WISK_LOG(WISK_LOG_TRACE, "static libc_posix_spawn(%s)", path);
	wisk_bind_symbol_libc(posix_spawn);

	return wisk.libc.symbols._libc_posix_spawn.f(pid, path, file_actions, attrp, argv, envp);
}

static int libc_posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *file_actions,
                             const posix_spawnattr_t *attrp, char *const argv[], char * const envp[])

{
//	WISK_LOG(WISK_LOG_TRACE, "static libc_posix_spawnp(%s)", file);
	wisk_bind_symbol_libc(posix_spawnp);

	return wisk.libc.symbols._libc_posix_spawnp.f(pid, file, file_actions, attrp, argv, envp);
}

static FILE *libc_popen(const char *command, const char *type)
{
//	WISK_LOG(WISK_LOG_TRACE, "static libc_popen(%s)", command);
	wisk_bind_symbol_libc(popen);

	return wisk.libc.symbols._libc_popen.f(command, type);
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
//	WISK_LOG(WISK_LOG_TRACE, "static libc_fopen64(%s, %s)", name, mode);
	wisk_bind_symbol_libc(fopen64);

	return wisk.libc.symbols._libc_fopen64.f(name, mode);
}
#endif /* HAVE_FOPEN64 */

static int libc_vopen(const char *pathname, int flags, va_list ap)
{
	int mode = 0;
	int fd;

//	WISK_LOG(WISK_LOG_TRACE, "static libc_vopen: %s", pathname);
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

//	WISK_LOG(WISK_LOG_TRACE, "static libc_open: %s", pathname);
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

//	WISK_LOG(WISK_LOG_TRACE, "static libc_vopen64(%s, %d)", pathname, flags);
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

//	WISK_LOG(WISK_LOG_TRACE, "static libc_vopenat(%d, %s, %d)", dirfd, path, flags);
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

static int libc_symlink(const char *target, const char *linkpath)
{
//	WISK_LOG(WISK_LOG_TRACE, "static libc_symlink(%s, %s)", target, linkpath);
	wisk_bind_symbol_libc(symlink);

	return wisk.libc.symbols._libc_symlink.f(target, linkpath);
}

static int libc_symlinkat(const char *target, int newdirfd, const char *linkpath)
{
//	WISK_LOG(WISK_LOG_TRACE, "static libc_symlink(%s, %d, %s)", target, newdirfd, linkpath) ;
	wisk_bind_symbol_libc(symlinkat);

	return wisk.libc.symbols._libc_symlinkat.f(target, newdirfd, linkpath);
}

static int libc_link(const char *oldpath, const char *newpath)
{
//	WISK_LOG(WISK_LOG_TRACE, "static libc_link(%s, %s)", oldpath, newpath);
	wisk_bind_symbol_libc(link);

	return wisk.libc.symbols._libc_link.f(oldpath, newpath);
}

static int libc_linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags)
{
//	WISK_LOG(WISK_LOG_TRACE, "static libc_linkat(%d, %s, %d, %s, %d)", olddirfd, oldpath, newdirfd, newpath, flags) ;
	wisk_bind_symbol_libc(linkat);

	return wisk.libc.symbols._libc_linkat.f(olddirfd, oldpath, newdirfd, newpath, flags);
}

static int libc_unlink(const char *pathname)
{
//	WISK_LOG(WISK_LOG_TRACE, "static libc_unlink(%s)", pathname);
	wisk_bind_symbol_libc(unlink);

	return wisk.libc.symbols._libc_unlink.f(pathname);
}

static int libc_unlinkat(int dirfd, const char *pathname, int flags)
{
//	WISK_LOG(WISK_LOG_TRACE, "static libc_unlinkat(%d, %s, %d)", dirfd, pathname, flags) ;
	wisk_bind_symbol_libc(unlinkat);

	return wisk.libc.symbols._libc_unlinkat.f(dirfd, pathname, flags);
}


static int libc_chmod(__const char *__file, __mode_t __mode)
{
//	WISK_LOG(WISK_LOG_TRACE, "static libc_chmod(%s, %d)", pathname, mode) ;
	wisk_bind_symbol_libc(chmod);

	return wisk.libc.symbols._libc_chmod.f(__file, __mode);
}

static int libc_fchmod(int __fd, __mode_t __mode)
{
//	WISK_LOG(WISK_LOG_TRACE, "static libc_fchmod(%d, %d)", fd, mode) ;
	wisk_bind_symbol_libc(fchmod);

	return wisk.libc.symbols._libc_fchmod.f(__fd, __mode);
}

static int libc_fchmodat(int __fd, __const char *__file, __mode_t __mode, int flags)
{
//	WISK_LOG(WISK_LOG_TRACE, "static libc_fchmodat(%d, %s, %d)", dirfd, pathname, mode) ;
	wisk_bind_symbol_libc(fchmodat);

	return wisk.libc.symbols._libc_fchmodat.f(__fd, __file, __mode, flags);
}


/* DO NOT call this function during library initialization! */
static void wisk_bind_symbol_all(void)
{
    internal_fopen = (__libc_fopen)_wisk_bind_symbol(WISK_LIBC, "fopen", true);
    internal_open = (__libc_open)_wisk_bind_symbol(WISK_LIBC, "open", true);

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
// 	wisk_bind_symbol_libc(execveat);
 	wisk_bind_symbol_libc(posix_spawn);
 	wisk_bind_symbol_libc(posix_spawnp);
 	wisk_bind_symbol_libc(symlink);
 	wisk_bind_symbol_libc(symlinkat);
 	wisk_bind_symbol_libc(link);
 	wisk_bind_symbol_libc(linkat);
	wisk_bind_symbol_libc(unlink);
	wisk_bind_symbol_libc(unlinkat);
	wisk_bind_symbol_libc(chmod);
	wisk_bind_symbol_libc(fchmod);
	wisk_bind_symbol_libc(fchmodat);
}

/*********************************************************
 * SWRAP HELPER FUNCTIONS
 *********************************************************/

char* ifnotabsolute(char *retbuf, const char *fname)
{
	char buf[PATH_MAX];

	if (fname[0] == '/')
		return (char*)fname;
	else {
		getcwd(buf, PATH_MAX);
		snprintf(retbuf, PATH_MAX, "%s/%s", buf, fname);
		return retbuf;
	}
}

static inline void escapedcharcopy(char **d, char **s)
{
    int j;
    char esc_char[]= { '\a','\b','\f','\n','\r','\t','\v','\\', '"'};
    char essc_str[]= {  'a', 'b', 'f', 'n', 'r', 't', 'v','\\', '"'};

    for(j=0; j< 9 ;j++){
        if( **s==esc_char[j] ){
          *(*d)++ = '\\';
          *(*d)++ = essc_str[j];
          *(*d) = '\0';
          (*s)++;
          break;
        }
    }
    if(j == 9 ) {
        *(*d)++ = *(*s)++;
        *(*d) = '\0';
    }
}

static inline void flushbuffer(char *msgbuffer, char **trackdest, int *trackcont)
{
//	msgbuffer[BUFFER_SIZE-1] = '\0';
    *(*trackdest)++ = '\n';
    **trackdest='\0';
    WISK_LOG(WISK_LOG_TRACE, "%d: %.*s", *trackdest - msgbuffer, *trackdest - msgbuffer, msgbuffer);
	write(fs_tracker_pipe, msgbuffer, *trackdest - msgbuffer);
    *trackdest = msgbuffer;
    *msgbuffer='\0';
    if (trackcont)
        *trackcont = true;
}


// msgbuffer - to use for reporting opersation
// uuid - associated with operation
// operation - string
// valuestr - to copy over
// idx - < 0 means, it is a single string, if >=0 it is a list, and this is the idx
// trackdest - current ptr within msgbuffer where the next character can be copied
// trackcont - Are we in operation start or continuation mode
static void wisk_report_operation(char *msgbuffer, char const *uuid, char const *operation, char *valuestr, int idx, char **trackdest, int *trackcont)
{
    char *src, *ldest;
    int lcont=false;

//    WISK_LOG(WISK_LOG_TRACE, "%s: %s", uuid, operation);
    if (trackcont == NULL) {
        trackcont = &lcont;
    }
    if (trackdest == NULL) {
        ldest = msgbuffer;
        trackdest = &ldest;
    }
    if (*trackdest >= msgbuffer+BUFFER_SIZE-10) { // Enough space to copy a char+'\0' OR escaped char + '\0'
        flushbuffer(msgbuffer, trackdest, trackcont);
    }
    if (*trackdest==msgbuffer) { // If beginning of Buffer
        *trackdest = msgbuffer+snprintf(*trackdest, BUFFER_SIZE, "%s %s ", uuid, operation);
        if (*trackcont) {
            *(*trackdest)++ = '*';
            *(*trackdest) = '\0';
        }
    }
	if (idx > 0) {
        *(*trackdest)++ = ',';
        *(*trackdest)++ = ' ';
	}
    *(*trackdest)++ = '"';
    *(*trackdest) = '\0';
    for(src=valuestr; *src; ) {
        if (*trackdest >= msgbuffer+BUFFER_SIZE-10) { // Enough space to copy a char+'\0' OR escaped char + '\0'
            flushbuffer(msgbuffer, trackdest, trackcont);
        }
        if (*trackdest==msgbuffer) { // If beginning of Buffer
            *trackdest = msgbuffer+snprintf(*trackdest, BUFFER_SIZE, "%s %s ", uuid, operation);
            if (*trackcont) {
                *(*trackdest)++ = '*';
                *(*trackdest) = '\0';
            }
            continue;
        }
        escapedcharcopy(trackdest, &src);
	}
    *(*trackdest)++ = '"';
    *(*trackdest) = '\0';
    if (idx < 0) // If idx < 0, its not a list, nothing more flush it
        flushbuffer(msgbuffer, trackdest, trackcont);
}

static wisk_report_operationlist(char *msgbuffer, char const *uuid, char const *operation, char *listp[])
{
	char *dest = msgbuffer;
    int cont=0, idx;

//    WISK_LOG(WISK_LOG_TRACE, "%s: %s", uuid, operation);
    dest = msgbuffer+snprintf(dest, BUFFER_SIZE, "%s %s [", uuid, operation);
	for(idx=0; listp[idx]; idx++) {
        WISK_LOG(WISK_LOG_TRACE, "%s: ['%s']", operation, listp[idx]);
	    wisk_report_operation(msgbuffer, uuid, operation, listp[idx], idx, &dest, &cont);
	}
    *dest++ = ']';
    flushbuffer(msgbuffer, &dest, &cont);
}


void wisk_report_link(const char *target, const char *linkpath)
{
    char msgbuffer[BUFFER_SIZE];
    char *listp[] = {NULL, NULL, NULL};
    char tbuf[PATH_MAX], lbuf[PATH_MAX];
    int msglen;

    if (fs_tracker_pipe < 0)
    	return;
	if (fs_tracker_enabled() && WISK_TRACK_EVENT(WISK_TRACK_LINKS)) {
        listp[0] = ifnotabsolute(tbuf, target);
        listp[1] = ifnotabsolute(lbuf, linkpath);
        wisk_report_operationlist(msgbuffer, fs_tracker_uuid, "LINKS", listp);
    } else {
        WISK_LOG(WISK_LOG_TRACE, "LINKS %s %s", target, linkpath);
	}
}

void wisk_report_unlink(const char *pathname)
{
    char msgbuffer[BUFFER_SIZE];
    char buf[PATH_MAX], lbuf[PATH_MAX];
    int msglen;

    if (fs_tracker_pipe < 0)
        return;
    if (fs_tracker_enabled() && WISK_TRACK_EVENT(WISK_TRACK_LINKS)) {
        wisk_report_operation(msgbuffer, fs_tracker_uuid, "UNLINK", ifnotabsolute(buf, pathname), -1, NULL, NULL);
    } else {
        WISK_LOG(WISK_LOG_TRACE, "UNLINK %s", pathname);
    }
}

void wisk_report_chmod(const char *pathname)
{
    char msgbuffer[BUFFER_SIZE];
    char buf[PATH_MAX], lbuf[PATH_MAX];
    int msglen;

    if (fs_tracker_pipe < 0)
        return;
    if (fs_tracker_enabled() && WISK_TRACK_EVENT(WISK_TRACK_CHMODS)) {
        wisk_report_operation(msgbuffer, fs_tracker_uuid, "CHMOD", ifnotabsolute(buf, pathname), -1, NULL, NULL);
    } else {
        WISK_LOG(WISK_LOG_TRACE, "CHMOD %s", pathname);
    }
}

void wisk_report_write(const char *fname)
{
    char msgbuffer[BUFFER_SIZE];
    char buf[PATH_MAX];
    int msglen;

    if (fs_tracker_pipe < 0)
    	return;
	if (fs_tracker_enabled() && WISK_TRACK_EVENT(WISK_TRACK_WRITES)) {
        wisk_report_operation(msgbuffer, fs_tracker_uuid, "WRITES", ifnotabsolute(buf, fname), -1, NULL, NULL);
    } else {
        WISK_LOG(WISK_LOG_TRACE, "WRITES %s", fname);
	}
}

void wisk_report_read(const char *fname)
{
    char msgbuffer[BUFFER_SIZE];
    char buf[PATH_MAX];
    int msglen;

    if (fs_tracker_pipe < 0)
    	return;
	if (fs_tracker_enabled() && WISK_TRACK_EVENT(WISK_TRACK_READS)) {
        wisk_report_operation(msgbuffer, fs_tracker_uuid, "READS", ifnotabsolute(buf, fname), -1, NULL, NULL);
    } else {
        WISK_LOG(WISK_LOG_TRACE, "READS %s", fname);
	}
}

void wisk_report_unknown(const char *fname, const char *mode)
{
    char msgbuffer[BUFFER_SIZE];
    char buf[PATH_MAX];
    int msglen;

	if (fs_tracker_enabled() && (fs_tracker_pipe >=0) && WISK_TRACK_EVENT(WISK_TRACK_READS)) {
        wisk_report_operation(msgbuffer, fs_tracker_uuid, "READS-UNKNOWN", (char*)fname, -1, NULL, NULL);
    } else {
        WISK_LOG(WISK_LOG_TRACE, "READS %s", fname);
	}
}

static void  wisk_report_command()
{
    int i, msglen, envcount, count;
    char curprog[PATH_MAX];
	char curpath[PATH_MAX];
	char pidstr[PATH_MAX];
	char ppidstr[PATH_MAX];
    char msgbuffer[BUFFER_SIZE];
    char envstr[BUFFER_SIZE];

    if (fs_tracker_pipe < 0)
    	return;
    if (!WISK_TRACK_EVENT(WISK_TRACK_PROCESS))
    	return;
    i = readlink("/proc/self/exe", curprog, sizeof(curprog)-1);
    if (i == PATH_MAX) {
      curprog[PATH_MAX-1] = '\0';
    } else if (i == -1) {
    	WISK_LOG(WISK_LOG_ERROR, "Falling back to ArgV, %d: %s", errno, strerror(errno));
    	strncpy(curprog, saved_argv[0], PATH_MAX);
    } else {
    	curprog[i] = '\0';
    }
    WISK_LOG(WISK_LOG_TRACE, "%d: %s", i, curprog);

	getcwd(curpath, PATH_MAX);
    WISK_LOG(WISK_LOG_TRACE, "%s CALLS %s PID=%d PPID=%d)",fs_tracker_puuid, fs_tracker_uuid, getpid(), getppid());
    wisk_report_operation(msgbuffer, fs_tracker_puuid, "CALLS", fs_tracker_uuid, -1, NULL, NULL);
    snprintf(pidstr, PATH_MAX, "%d", getpid());
    wisk_report_operation(msgbuffer, fs_tracker_uuid, "PID", pidstr, -1, NULL, NULL);
    snprintf(ppidstr, PATH_MAX, "%d", getppid());
    wisk_report_operation(msgbuffer, fs_tracker_uuid, "PPID", ppidstr, -1, NULL, NULL);
    wisk_report_operation(msgbuffer, fs_tracker_uuid, "WORKING_DIRECTORY", curpath, -1, NULL, NULL);
    wisk_report_operation(msgbuffer, fs_tracker_uuid, "COMMAND_PATH", curprog, -1, NULL, NULL);
    wisk_report_operationlist(msgbuffer, fs_tracker_uuid, "COMMAND", saved_argv);
    wisk_report_operationlist(msgbuffer, fs_tracker_uuid, "ENVIRONMENT", environ);
}

static void  wisk_report_commandcomplete()
{
    int count;
    char pbuf[PATH_MAX], ppbuf[PATH_MAX], procpbuf[PATH_MAX];
    char msgbuffer[BUFFER_SIZE];

    if (fs_tracker_pipe < 0)
    	return;
    if (!WISK_TRACK_EVENT(WISK_TRACK_PROCESS))
    	return;
    for(count=0; saved_argv[count]; count++); 
    char *listp[count+4];
    snprintf(procpbuf, PATH_MAX, "%d", fs_tracker_pid);
    snprintf(pbuf, PATH_MAX, "%d", getpid());
    snprintf(ppbuf, PATH_MAX, "%d", getppid());
    listp[0] = procpbuf;
    listp[1] = pbuf;
    listp[2] = ppbuf;
    for(count=3; saved_argv[count-3]; count++)
        listp[count] = saved_argv[count-3];
    listp[count] = saved_argv[count-3];
    if(getpid() == fs_tracker_pid || getppid() == 1) {
        wisk_report_operationlist(msgbuffer, fs_tracker_uuid, "COMPLETE", listp);
    }
    else {
    	WISK_LOG(WISK_LOG_TRACE, "%s COMPLETE_THREAD [\"%d\" \"%d\" \"%d\" \"%s\" \"%s\" \"%s\" \"%s\" %s]",
    			fs_tracker_uuid, fs_tracker_pid, getpid(), getppid(), listp[3], count>4?listp[4]:"", count>5?listp[5]:"", count>6?listp[6]:"", count>7?"...":"");
//        wisk_report_operationlist(msgbuffer, fs_tracker_uuid, "COMPLETE_THREAD", listp);

    }
}


static int generate_uniqueid(char *str)
{
    FILE *randfile;
    random_uuid_t random_uuid;

//    struct timespec epochtime;
//	unsigned int i1, i2, i3, i4;
//    clock_gettime(CLOCK_REALTIME, &epochtime);
//	srandom(epochtime.tv_sec);
//	i1 = random();
//	srandom(epochtime.tv_nsec);
//	i2 = random();
//	srandom(getpid());
//	i3 = random();
//	srandom(getppid());
//	i4 = random();
//    getrandom(&random_uuid, sizeof(random_uuid), 0);
//
//	struct timeb ctime;
//	ftime(&ctime);
//    srandom(ctime.millitm);
//    i1 = random();
//    i2 = random();
//    srandom(getpid());
//    i3 = random();
//    i4 = random();
//    snprintf(str, UUID_SIZE, "%08x-%08x-%08x-%08x", i1, i2, i3, i4);

    randfile = internal_fopen("/dev/urandom", "rb");
    WISK_LOG(WISK_LOG_TRACE, "Randomize Device File: %p", randfile);
    fread(&random_uuid, sizeof(random_uuid), 1, randfile);
    fclose(randfile);
    snprintf(str, UUID_SIZE, "%08x-%08x-%08x-%08x", random_uuid.i1, random_uuid.i2, random_uuid.i3, random_uuid.i4);
    WISK_LOG(WISK_LOG_TRACE, "Random UniqeID(%s)", str);
//	WISK_LOG(WISK_LOG_TRACE, "Seconds: %d, Nanoseconds: %d, PID: %d, PPID: %d, UniqeID(%s)", epochtime.tv_sec, epochtime.tv_nsec, getpid(), getppid(), str);
//    WISK_LOG(WISK_LOG_TRACE, "PID: %d, UniqeID(%s), with %d", getpid(), str, millisecond);
}

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

static void debug_log_wiskenv(char *msg, char *const envp[])
{
	int i, count;
	for(i=0, count=0; envp[i]; i++)
	    count++;
	WISK_LOG(WISK_LOG_TRACE, "%s Count: %d", msg?msg:"", count);
	for(i=0; envp[i]; i++) {
		WISK_LOG(WISK_LOG_TRACE, "\t%d: %s", i, envp[i]);
	}

}

static void wisk_env_update(char *var, char *value, int *count, bool update)
{
    char *s;
    int len;
	int i;

	if (*count == 0) {
		WISK_LOG(WISK_LOG_TRACE, "Initializing");
		for(i=0; i< WISK_ENV_VARCOUNT; i++)
			wisk_envp[i] = NULL;
	}
	for(i=0; i< *count; i++)
		if (envcmp(var, wisk_env_vars[i]))
			break;
	if (strncmp(var, WISK_TRACKER_UUID, strlen(WISK_TRACKER_UUID)) == 0)
		value = fs_tracker_uuid;
	if (value == NULL)
		value = getenv(var);
	if (value == NULL) {
//	    WISK_LOG(WISK_LOG_TRACE, "Nothing to %s WISK Environment %s = %s", update?"Update":"Add", var, value);
		return;
	}
	if (i != *count && !update) {
//	    WISK_LOG(WISK_LOG_TRACE, "Do not %s existing WISK Environment %s = %s", update?"Update":"Add",var, value);
		return;
	}
	len = strlen(var)+strlen(value) + 2;
	if (wisk_envp[i] !=  NULL) {
	    SAFE_FREE(wisk_envp[i]);
	}
	wisk_envp[i] = malloc(len);
	wisk_envp[i][0] ='\0';
	WISK_LOG(WISK_LOG_TRACE, "WISK Environment %s=%s, len=%d", var, value, len);
    snprintf(wisk_envp[i], len, "%s=%s", var, value);
//	WISK_LOG(WISK_LOG_TRACE, "WISK Environment %s", wisk_envp[i]);

	if (i==*count) {
		*count = i+1;
//		WISK_LOG(WISK_LOG_TRACE, "%s New WISK Environment %d: %s, count=%d", update?"Update":"Add",i, wisk_envp[i], *count);
	} else {
//		WISK_LOG(WISK_LOG_TRACE, "%s Existing WISK Environment %d: %s", update?"Update":"Add", i, wisk_envp[i]);
	}
//    for(i=0; i<wisk_env_count; i++) {
//	    WISK_LOG(WISK_LOG_TRACE, "WISK_ENV[%d: %s]", i, wisk_envp[i]);
//    }
}

static int wisk_getvarcount(char *const envp[])
{
    int envc, i;

    for (i=0, envc=1; envp[i]; i++) {
      if (wisk_isenv(envp[i])) {
//          WISK_LOG(WISK_LOG_TRACE, "Skipping Environment %d: %s", i, envp[i]);
          continue;
        }
//      WISK_LOG(WISK_LOG_TRACE, "Environment %d: %s", i, envp[i]);
      envc++;
    }
//    WISK_LOG(WISK_LOG_TRACE, "Var Count: %d",  envc);
    return envc;
}

static void wisk_path_append(char *dest, char *src, char separator, char **include, char **exclude)
{
    char *starts, *s, *d, **inc, **exc, *envdest=dest, *envsrc=src, *pinclude=NULL, *pexclude=NULL;
    char sepstr[2];
    int len, first=0, inexlen;
    
    sprintf(sepstr, "%c", separator);
	WISK_LOG(WISK_LOG_TRACE, "dest:%s, src:%s, include=%s%s%s, exclude=%s%s%s", dest, src,
			include?"{":"", include?include[0]:NULL, include?",..}":"",
			exclude?"{":"", exclude?exclude[0]:NULL, exclude?",..}":"");
    for(starts=envsrc, len=0; *envsrc && *envsrc != '='; envsrc++, len++);
    envsrc++;
    if (*envdest  == '\0') {
        strncat(envdest, starts, len+1);
    }
    for(; *envdest && *envdest != '='; envdest++);
    envdest++;
    if (*envdest == '\0')
        first=1;
    for(starts=envsrc, s=envsrc, len=0; ; s++) {
        if (*starts == '\0')
            break;
        if (*starts == separator) {
            starts++;
            s=starts;
            len=1;
            continue;
        }
        if (*s != separator && *s) {
            len++;
            continue;
        }
        for(d=envdest; *d; d++) {
            if ((strncmp(d, starts, len) == 0) && (*(d+len) == '\0' || *(d+len) == separator)) {
                len=0;
                break;
            }
        }
        if (len && include) {
        	pinclude = include[0];
//        	WISK_LOG(WISK_LOG_TRACE, "including: %s....", include[0]);
        	for(inc=include; *inc; inc += 1) {
            	inexlen=strlen(*inc);
            	if (strncmp(starts, *inc, (len <= inexlen?len:inexlen))==0) {
            		break;
        	    }
        	}
        	if(!*inc) {
//	            WISK_LOG(WISK_LOG_TRACE, "Skipping by Include-Only: %.*s", len, starts);
        		len=0;
        	}
        }
        if (len && exclude) {
        	pexclude = exclude[0];
//        	WISK_LOG(WISK_LOG_TRACE, "excluding: %s....", exclude[0]);
        	for(exc=exclude; *exc; exc += 1) {
				inexlen=strlen(*exc);
//				WISK_LOG(WISK_LOG_TRACE, "Exclude Comparing: %.*s, %s, %d, %d, %d", len, starts, *exc, len, inexlen, (len <= inexlen?len:inexlen));
				if (strncmp(starts, *exc, (len <= inexlen?len:inexlen))==0) {
//		            WISK_LOG(WISK_LOG_TRACE, "Skipping by Exclude-Only: %.*s", len, starts);
					len=0;
					break;
				}
        	}
        }
        if (len) {
            WISK_LOG(WISK_LOG_TRACE, "Adding: %.*s, include=%s, exclude=%s", len, starts, pinclude, pexclude);
            if (!first)
               strncat(envdest, sepstr, 1);
            first=0;
            strncat(envdest, starts, len);
        }
        starts=s;
        len=1;
    }
	WISK_LOG(WISK_LOG_TRACE, "Return %s", dest);
}

static void wisk_loadenv(char *const envp[], char *nenvp[], char *ld_library_path, char *ld_preload)
{
    int envi=0, i;
    char *wisk_ld_path=NULL;
    char *wisk_ld_preload=NULL;

//	WISK_LOG(WISK_LOG_TRACE, "WISK_ENV_COUNT: %d", wisk_env_count);
    ld_library_path[0] = '\0';
    ld_preload[0] = '\0';
	for(envi=0; envi < wisk_env_count; envi++) {
//	  WISK_LOG(WISK_LOG_TRACE, "Adding WISK_ENV: %s", wisk_envp[envi]);
      if (envcmp(wisk_envp[envi], "LD_LIBRARY_PATH")) {
		nenvp[envi] = ld_library_path;
        wisk_ld_path = wisk_envp[envi];
      }
      else if (envcmp(wisk_envp[envi], "LD_PRELOAD")) {
//      	WISK_LOG(WISK_LOG_TRACE, "Saved WISK ENV LDLOAD: %s", wisk_envp[envi]);
		nenvp[envi] = ld_preload;
        wisk_ld_preload = wisk_envp[envi];
        wisk_path_append(ld_preload, wisk_ld_preload, LD_PRELOAD_SEPARATOR, preldload, NULL);
      }
      else
		nenvp[envi] = wisk_envp[envi];
//      WISK_LOG(WISK_LOG_TRACE, "Added WISK_ENV: %s", nenvp[envi]);
	}
//	WISK_LOG(WISK_LOG_TRACE, "Added by PRE LDLOAD: %s", ld_preload);
    for (i=0; envp[i]; i++) {
//      WISK_LOG(WISK_LOG_TRACE, "Adding ENV: %s", envp[i]);
      if (envcmp(envp[i], "LD_LIBRARY_PATH")) {
    	  nenvp[envi] = ld_library_path;
          wisk_path_append(ld_library_path, envp[i], LD_LIBRARY_PATH_SEPARATOR, NULL, NULL);
          continue;
      } 
      else if (envcmp(envp[i], "LD_PRELOAD")) {
//    	  WISK_LOG(WISK_LOG_TRACE, "Current Original ENV LDLOAD: %s", envp[i]);
    	  nenvp[envi] = ld_preload;
          wisk_path_append(ld_preload, envp[i], LD_PRELOAD_SEPARATOR, NULL, ldload);
          continue;
      } 
      else if (wisk_isenv(envp[i]))
          continue;
      nenvp[envi++] = envp[i];
//      WISK_LOG(WISK_LOG_TRACE, "Added ENV: %s", nenvp[envi-1]);
    }
//    WISK_LOG(WISK_LOG_TRACE, "Added from Original LDLOAD: %s", ld_preload);
    if (wisk_ld_path) 
        wisk_path_append(ld_library_path, wisk_ld_path, LD_LIBRARY_PATH_SEPARATOR, NULL, NULL);
    if (wisk_ld_preload)
        wisk_path_append(ld_preload, wisk_ld_preload, LD_PRELOAD_SEPARATOR, postldload, NULL);
    nenvp[envi++] = envp[i];
//	WISK_LOG(WISK_LOG_TRACE, "Added by POST LDLOAD: %s", ld_preload);
	debug_log_wiskenv("WISK Loaded Environment", nenvp);
}


static char *fs_tracker_pipe_getpath(void)
{
	char *wisk_pipe_path = NULL;
	char *s = getenv(WISK_TRACKER_PIPE);

	if (s == NULL) {
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

void logging_init(void)
{
	char *d = NULL;
	char value[PATH_MAX];

	WISK_LOG(WISK_LOG_TRACE, "Init");
	if (fs_tracker_debuglog == -1) {
		d = getenv(WISK_TRACKER_DEBUGLOG_FD);
		WISK_LOG(WISK_LOG_TRACE, "%s=%s", WISK_TRACKER_DEBUGLOG_FD, d);
		if (d && fd_is_valid(atoi(d))) {
			fs_tracker_debuglog = atoi(d);
		}
	}
	if (fs_tracker_debuglog == -1) {
		d = getenv(WISK_TRACKER_DEBUGLOG);
		WISK_LOG(WISK_LOG_TRACE, "Init: %s=%s", WISK_TRACKER_DEBUGLOG, d);
		if (d) {
			if (internal_open) {
				WISK_LOG(WISK_LOG_TRACE, "Tracker Recieve Pipe Real open(%s), UUID=%s", d, fs_tracker_uuid);
				fs_tracker_debuglog = internal_open(d, O_WRONLY|O_APPEND);
			} else {
				WISK_LOG(WISK_LOG_ERROR, "Tracker Recieve Pipe: Local open(%s), UUID=%s, PUUID=%s(Internal open not initialized)",
						d, fs_tracker_uuid, fs_tracker_puuid);
//				fs_tracker_debuglog = libc_open(d, O_WRONLY|O_APPEND);
			}
			snprintf(value, PATH_MAX, "%d", fs_tracker_debuglog);
			wisk_env_update(WISK_TRACKER_DEBUGLOG_FD, value, &wisk_env_count, true);
		}
	}
	if (fs_tracker_debuglog == -1) {
		WISK_LOG(WISK_LOG_ERROR, "File System Tracker Pipe %s cannot be opened for write\n", fs_tracker_debuglog);
	}
	WISK_LOG(WISK_LOG_TRACE, "Init done");
}

static void fs_tracker_init_pipe(char *fs_tracker_pipe_path)
{
	char *uuidstr, *puuidstr, *d, value[PATH_MAX];
	int ret, i;

	wisk_mutex_lock(&fs_tracker_pipe_mutex);
	wisk_bind_symbol_all();
	logging_init();
    generate_uniqueid(fs_tracker_uuid);
	uuidstr = getenv(WISK_TRACKER_UUID);
    if (uuidstr) {
        strncpy(fs_tracker_puuid, uuidstr, UUID_SIZE);
    } else {
        strncpy(fs_tracker_puuid, "XXXXXXXX-XXXXXXXX-XXXXXXXX-XXXXXXXX", UUID_SIZE);
    }

	if (fs_tracker_pipe == -1) {
		d = getenv(WISK_TRACKER_PIPE_FD);
		WISK_LOG(WISK_LOG_TRACE, "%s=%s", WISK_TRACKER_PIPE_FD, d);
		if (d && fd_is_valid(atoi(d))) {
			fs_tracker_pipe = atoi(d);
		}
	}
	if (fs_tracker_pipe == -1) {
		if (internal_open) {
			WISK_LOG(WISK_LOG_TRACE, "Tracker Recieve Pipe Real open(%s), UUID=%s", fs_tracker_pipe_path, fs_tracker_uuid);
			fs_tracker_pipe = internal_open(fs_tracker_pipe_path, O_WRONLY|O_APPEND);
		} else {
			WISK_LOG(WISK_LOG_ERROR, "Tracker Recieve Pipe: Local open(%s), UUID=%s, PUUID=%s(Internal open not initialized)",
					fs_tracker_pipe_path, fs_tracker_uuid, fs_tracker_puuid);
		}
		snprintf(value, PATH_MAX, "%d", fs_tracker_pipe);
		wisk_env_update(WISK_TRACKER_PIPE_FD, value, &wisk_env_count, true);
	}
	if (fs_tracker_pipe == -1) {
		WISK_LOG(WISK_LOG_ERROR, "File System Tracker Pipe %s cannot be opened for write\n", fs_tracker_pipe_path);
	}
	d = getenv(WISK_TRACKER_EVENTFILTER);
	if (d != NULL) {
		fs_tracker_eventfilter = atoi(d);
		WISK_LOG(WISK_LOG_TRACE, "File System Tracker Event Filter: %s, 0x%X\n", d, fs_tracker_eventfilter);
	}
	for(i=0; i< WISK_ENV_VARCOUNT; i++)
		wisk_env_update(wisk_env_vars[i], NULL, &wisk_env_count, false);
	fs_tracker_pid = getpid();

//    for(i=0; i<wisk_env_count; i++) {
//	    WISK_LOG(WISK_LOG_TRACE, "WISK_ENV[%d: %s]", i, wisk_envp[i]);
//    }
//    WISK_LOG(WISK_LOG_TRACE, "WISK_ENV_COUNT: %d", wisk_env_count);
    debug_log_wiskenv("WISK Environment", wisk_envp);
    wisk_report_command();

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

	// This needs to be done last for some reason. screws up thinigs otherwise. Thread safety?
	// Cant handle being called from the constructor in clones
	setenv(WISK_TRACKER_UUID, fs_tracker_uuid, 1);
	return true;
}

/****************************************************************************
 *   FOPEN
 ***************************************************************************/

#ifdef INTERCEPT_FOPEN
static FILE *wisk_fopen(const char *name, const char *mode)
{
	FILE *fp;
    WISK_LOG(WISK_LOG_TRACE, "wisk_fopen(%s, %s)", name, mode);
    fp = libc_fopen(name, mode);
    if (fp == NULL) return fp;
    if ((mode[0] == 'w') || (mode[0] == 'a')) {
        wisk_report_write(name);
        if (mode[1] == '+')
        	wisk_report_read(name);
    } else if ((mode[0] == 'r')) {
        wisk_report_read(name);
        if (mode[1] == '+')
        	wisk_report_write(name);
    } else {
        wisk_report_unknown(name, mode);
    }
    WISK_LOG(WISK_LOG_TRACE, "wisk_fopen(%s, %s)->%p", name, mode, fp);
	return fp;
}

FILE *fopen(const char *name, const char *mode)
{
    WISK_LOG(WISK_LOG_TRACE, "fopen(%s, %s)", name, mode);
	return wisk_fopen(name, mode);
}
#endif

/****************************************************************************
 *   FOPEN64
 ***************************************************************************/

#ifdef INTERCEPT_FOPEN64
#ifdef HAVE_FOPEN64
static FILE *wisk_fopen64(const char *name, const char *mode)
{
	FILE *fp;
    WISK_LOG(WISK_LOG_TRACE, "wisk_fopen64(%s, %s)", name, mode);
	fp = libc_fopen64(name, mode);
	if (fp == NULL) return fp;
    if ((mode[0] == 'w') || (mode[0] == 'a')) {
        wisk_report_write(name);
        if (mode[1] == '+')
        	wisk_report_read(name);
    } else if ((mode[0] == 'r')) {
        wisk_report_read(name);
        if (mode[1] == '+')
        	wisk_report_write(name);
    } else {
        wisk_report_unknown(name, mode);
    }
	return fp;
}

FILE *fopen64(const char *name, const char *mode)
{
    WISK_LOG(WISK_LOG_TRACE, "fopen64(%s, %s)", name, mode);
	return wisk_fopen64(name, mode);
}
#endif /* HAVE_FOPEN64 */
#endif

/****************************************************************************
 *   OPEN
 ***************************************************************************/
#ifdef INTERCEPT_OPEN
static int wisk_vopen(const char *pathname, int flags, va_list ap)
{
    int fd;
    WISK_LOG(WISK_LOG_TRACE, "wisk_vopen(%s, %d)", pathname, flags);
	fd = libc_vopen(pathname, flags, ap);
    if (fd == -1) return fd;
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
	return fd;
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
#endif

/****************************************************************************
 *   OPEN64
 ***************************************************************************/

#ifdef INTERCEPT_OPEN64
#ifdef HAVE_OPEN64
static int wisk_vopen64(const char *pathname, int flags, va_list ap)
{
	int ret;

    WISK_LOG(WISK_LOG_TRACE, "wisk_vopen64(%s, %d)", pathname, flags);
	ret = libc_vopen64(pathname, flags, ap);
    if (ret == -1) return ret;
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
#endif

/****************************************************************************
 *   OPENAT
 ***************************************************************************/

#ifdef INTERCEPT_OPENAT
static int wisk_vopenat(int dirfd, const char *path, int flags, va_list ap)
{
	int ret;

    WISK_LOG(WISK_LOG_TRACE, "wisk_vopenat(%d, %s, %d)", dirfd, path, flags);
	ret = libc_vopenat(dirfd, path, flags, ap);
	if (ret == -1) return ret;
    if ((flags & O_WRONLY) && (flags & O_RDONLY)) {
        wisk_report_read(path);
        wisk_report_write(path);
    } else if (flags & O_WRONLY) {
        wisk_report_write(path);
    } else if (flags & O_RDONLY) {
        wisk_report_read(path);
    } else {
        wisk_report_read(path);
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
#endif

/****************************************************************************
 *   EXECVE
 ***************************************************************************/

#ifdef INTERCEPT_EXECL
static int wisk_vexecl(const char *file, const char *arg, va_list ap, int argcount)
{
    WISK_LOG(WISK_LOG_TRACE, "wisk_vexecl(%s)", file);
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(environ) + wisk_env_count + 1];
        char ld_library_path[PATH_MAX];
        char ld_preload[PATH_MAX];
        wisk_loadenv(environ, nenvp, ld_library_path, ld_preload);
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
#endif

#ifdef INTERCEPT_EXECLE
static int wisk_vexecle(const char *file, const char *arg, va_list ap, int argcount, char *const envp[])
{

    WISK_LOG(WISK_LOG_TRACE, "wisk_vexecle(%s)", file);
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(envp) + wisk_env_count + 1];
        char ld_library_path[PATH_MAX];
        char ld_preload[PATH_MAX];
        wisk_loadenv(envp, nenvp, ld_library_path, ld_preload);
//        wisk_report_command(file, arg, nenvp);
	    return libc_vexecle(file, arg, ap, argcount, nenvp);
    } else {
	    return libc_vexecle(file, arg, ap, argcount, envp);
    }
}

int execle(const char *file, const char *arg, ...)
{
	va_list ap;
	char **envp;
	int argcount=0, rv;

	va_start(ap, arg);
	for(argcount=1; va_arg(ap, char *) != NULL; argcount++);
	envp = (char **)va_arg(ap, char *);
	va_end(ap);

    WISK_LOG(WISK_LOG_TRACE, "execle(%s)", file);
    va_start(ap, arg);
    rv = wisk_vexecle(file, arg, ap, argcount, envp);
    va_end(ap);
    return rv;
}
#endif


#ifdef INTERCEPT_EXECLP
static int wisk_vexeclp(const char *file, const char *arg, va_list ap, int argcount)
{
    WISK_LOG(WISK_LOG_TRACE, "wisk_vexeclp(%s)", file);
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(environ) + wisk_env_count + 1];
        char ld_library_path[PATH_MAX];
        char ld_preload[PATH_MAX];
        wisk_loadenv(environ, nenvp, ld_library_path, ld_preload);
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
#endif

#ifdef INTERCEPT_EXECLPE
static int wisk_vexeclpe(const char *file, const char *arg, va_list ap, int argcount, char *const envp[])
{
    WISK_LOG(WISK_LOG_TRACE, "wisk_vexeclpe(%s)", file);
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(envp) + wisk_env_count + 1];
        char ld_library_path[PATH_MAX];
        char ld_preload[PATH_MAX];
        wisk_loadenv(envp, nenvp, ld_library_path, ld_preload);
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

	va_start(ap, arg);
	for(argcount=1; va_arg(ap, char *) != NULL; argcount++);
	envp = (char **)va_arg(ap, char *);
	va_end(ap);

	WISK_LOG(WISK_LOG_TRACE, "execlpe(%s)", file);
    va_start(ap, arg);
    rv = wisk_vexeclpe(file, arg, ap, argcount, envp);
    va_end(ap);
    return rv;
}
#endif

#ifdef INTERCEPT_EXECV
static int wisk_execv(const char *path, char *const argv[])
{
    WISK_LOG(WISK_LOG_TRACE, "wisk_execv(%s)", path);
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(environ) + wisk_env_count + 1];
        char ld_library_path[PATH_MAX];
        char ld_preload[PATH_MAX];
        wisk_loadenv(environ, nenvp, ld_library_path, ld_preload);
//        wisk_report_command(path, argv, nenvp);
	    return libc_execvpe(path, argv, nenvp);
    } else
	    return libc_execvpe(path, argv, environ);
}

int execv(const char *path, char *const argv[])
{
    WISK_LOG(WISK_LOG_TRACE, "execv(%s)", path);
	return wisk_execv(path, argv);
}
#endif

#ifdef INTERCEPT_EXECVP
static int wisk_execvp(const char *file, char *const argv[])
{
    WISK_LOG(WISK_LOG_TRACE, "wisk_execvp(%s)", file);
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(environ) + wisk_env_count + 1];
        char ld_library_path[PATH_MAX];
        char ld_preload[PATH_MAX];
        wisk_loadenv(environ, nenvp, ld_library_path, ld_preload);
//        wisk_report_command(file, argv, nenvp);
	    return libc_execvpe(file, argv, nenvp);
    } else
	    return libc_execvpe(file, argv, environ);
}

int execvp(const char *file, char *const argv[])
{
    WISK_LOG(WISK_LOG_TRACE, "execvp(%s)", file);
	return wisk_execvp(file, argv);
}
#endif

#ifdef INTERCEPT_EXECVPE
static int wisk_execvpe(const char *file, char *const argv[], char *const envp[])
{
    WISK_LOG(WISK_LOG_TRACE, "wisk_execvpe(%s)", file);
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(envp) + wisk_env_count + 1];
        char ld_library_path[PATH_MAX];
        char ld_preload[PATH_MAX];
        wisk_loadenv(envp, nenvp, ld_library_path, ld_preload);
//        wisk_report_command(file, argv, nenvp);
	    return libc_execvpe(file, argv, nenvp);
    } else
	    return libc_execvpe(file, argv, envp);
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
    WISK_LOG(WISK_LOG_TRACE, "execvpe(%s)", file);
	return wisk_execvpe(file, argv, envp);
}
#endif

#ifdef INTERCEPT_EXECVE
static int wisk_execve(const char *pathname, char *const argv[], char *const envp[])
{
    WISK_LOG(WISK_LOG_TRACE, "wisk_execve(%s)", pathname);
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(envp) + wisk_env_count + 1];
        char ld_library_path[PATH_MAX];
        char ld_preload[PATH_MAX];
        wisk_loadenv(envp, nenvp, ld_library_path, ld_preload);
//        wisk_report_command(pathname, argv, nenvp);
	    return libc_execve(pathname, argv, nenvp);
    } else
	    return libc_execve(pathname, argv, envp);
}

int execve(const char *pathname, char *const argv[], char *const envp[])
{
    WISK_LOG(WISK_LOG_TRACE, "execve(%s)", pathname);
	return wisk_execve(pathname, argv, envp);
}
#endif

#ifdef INTERCEPT_EXECVEAT
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
#endif

#ifdef INTERCEPT_POSIX_SPAWN
static int wisk_posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *file_actions,
		                    const posix_spawnattr_t *attrp, char *const argv[], char *const envp[])
{
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(envp) + wisk_env_count + 1];
        char ld_library_path[PATH_MAX];
        char ld_preload[PATH_MAX];
        wisk_loadenv(envp, nenvp, ld_library_path, ld_preload);
//        wisk_report_command(path, argv, nenvp);
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
#endif

#ifdef INTERCEPT_POSIX_SPAWNP
static int wisk_posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *file_actions,
		                    const posix_spawnattr_t *attrp, char *const argv[], char *const envp[])
{
	if (fs_tracker_enabled()) {
        char *nenvp[wisk_getvarcount(envp) + wisk_env_count + 1];
        char ld_library_path[PATH_MAX];
        char ld_preload[PATH_MAX];
        wisk_loadenv(envp, nenvp, ld_library_path, ld_preload);
//        wisk_report_command(file, argv, nenvp);
	    return libc_posix_spawnp(pid, file, file_actions, attrp, argv, nenvp);
    } else
	    return libc_posix_spawnp(pid, file, file_actions, attrp, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *file_actions,
		                    const posix_spawnattr_t *attrp, char *const argv[], char *const envp[])
{
    WISK_LOG(WISK_LOG_TRACE, "posix_spawnp(%s)", file);
	return wisk_posix_spawnp(pid, file, file_actions, attrp, argv, envp);
}
#endif

#ifdef INTERCEPT_POPEN
static FILE *wisk_popen(const char *command, const char *type)
{
	if (fs_tracker_enabled()) {
	    return libc_popen(command, type);
    } else
	    return libc_popen(command, type);
}

FILE *popen(const char *command, const char *type)
{
    WISK_LOG(WISK_LOG_TRACE, "popen(%s)", command);
	return wisk_popen(command, type);
}
#endif

#ifdef INTERCEPT_SYMLINK
static int wisk_symlink(const char *target, const char *linkpath)
{
	if (fs_tracker_enabled()) {
		wisk_report_link(target, linkpath);
	    return libc_symlink(target, linkpath);
    } else
	    return libc_symlink(target, linkpath);
}

int symlink(const char *target, const char *linkpath)
{
    WISK_LOG(WISK_LOG_TRACE, "symlink(%s, %s)", target, linkpath);
	return wisk_symlink(target, linkpath);
}
#endif


#ifdef INTERCEPT_LINKAT
static int wisk_symlinkat(const char *target, int newdirfd, const char *linkpath)
{
	if (fs_tracker_enabled()) {
		wisk_report_link(target, linkpath);
	    return libc_symlinkat(target, newdirfd, linkpath);
    } else
	    return libc_symlinkat(target, newdirfd, linkpath);
}

int symlinkat(const char *target, int newdirfd, const char *linkpath)
{
    WISK_LOG(WISK_LOG_TRACE, "symlinkat(%s, %d, %s)", target, newdirfd, linkpath);
	return wisk_symlinkat(target, newdirfd, linkpath);
}
#endif


#ifdef INTERCEPT_LINK
static int wisk_link(const char *oldpath, const char *newpath)
{
	if (fs_tracker_enabled()) {
		wisk_report_link(oldpath, newpath);
	    return libc_link(oldpath, newpath);
    } else
	    return libc_link(oldpath, newpath);
}

int link(const char *oldpath, const char *newpath)
{
    WISK_LOG(WISK_LOG_TRACE, "link(%s, %s)", oldpath, newpath);
	return wisk_link(oldpath, newpath);
}
#endif

#ifdef INTERCEPT_LINKAT
static int wisk_linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags)
{
	if (fs_tracker_enabled()) {
		wisk_report_link(oldpath, newpath);
	    return libc_linkat(olddirfd, oldpath, newdirfd, newpath, flags);
    } else
	    return libc_linkat(olddirfd, oldpath, newdirfd, newpath, flags);
}

int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags)
{
    WISK_LOG(WISK_LOG_TRACE, "linkat(%d, %s, %d, %s, %d)", olddirfd, oldpath, newdirfd, newpath, flags);
	return wisk_linkat(olddirfd, oldpath, newdirfd, newpath, flags);
}
#endif

#ifdef INTERCEPT_UNLINK
static int wisk_unlink(const char *pathname)
{
	if (fs_tracker_enabled()) {
		wisk_report_unlink(pathname);
	    return libc_unlink(pathname);
    } else
	    return libc_unlink(pathname);
}

int unlink(const char *pathname)
{
    WISK_LOG(WISK_LOG_TRACE, "unlink(%s)", pathname);
	return wisk_unlink(pathname);
}
#endif

#ifdef INTERCEPT_UNLINKAT
static int wisk_unlinkat(int dirfd, const char *pathname, int flags)
{
	if (fs_tracker_enabled()) {
		wisk_report_unlink(pathname);
	    return libc_unlinkat(dirfd, pathname, flags);
    } else
	    return libc_unlinkat(dirfd, pathname, flags);
}

int unlinkat(int dirfd, const char *pathname, int flags)
{
    WISK_LOG(WISK_LOG_TRACE, "unlinkat(%d, %s, %d)", dirfd, pathname, flags);
	return wisk_unlinkat(dirfd, pathname, flags);
}
#endif

#ifdef INTERCEPT_CHMOD
static int wisk_chmod(__const char *__file, __mode_t __mode)
{
	if (fs_tracker_enabled()) {
		wisk_report_chmod(__file);
	    return libc_chmod(__file, __mode);
    } else
	    return libc_chmod(__file, __mode);
}

int chmod(__const char *__file, __mode_t __mode)
{
    WISK_LOG(WISK_LOG_TRACE, "chmod(%s, %d)", __file, __mode);
	return wisk_chmod(__file, __mode);
}
#endif

#ifdef INTERCEPT_FCHMOD
static int wisk_fchmod(int __fd, __mode_t __mode)
{
	int i;
	char fdstr[PATH_MAX];
	char fdpath[PATH_MAX];

	if (fs_tracker_enabled()) {
		sprintf(fdstr, "/proc/self/fd/%d", __fd);
		i = readlink(fdstr, fdpath, sizeof(fdpath)-1);
		if (i == PATH_MAX) {
			fdpath[PATH_MAX-1] = '\0';
	    } else if (i == -1) {
	        WISK_LOG(WISK_LOG_ERROR, "Falling back to ArgV, %d: %s", errno, strerror(errno));
            strncpy(fdpath, "FAILED_FILE_PATH", PATH_MAX);
	    } else {
            fdpath[i] = '\0';
	    }
		wisk_report_chmod(fdpath);
	    return libc_fchmod(__fd, __mode);
    } else
	    return libc_fchmod(__fd, __mode);
}

int fchmod(int __fd, __mode_t __mode)
{
    WISK_LOG(WISK_LOG_TRACE, "fchmod(%d, %d)", __fd, __mode);
	return wisk_fchmod(__fd, __mode);
}
#endif

#ifdef INTERCEPT_FCHMODAT
static int wisk_fchmodat(int __fd, __const char *__file, __mode_t __mode, int flags)
{
	if (fs_tracker_enabled()) {
		wisk_report_chmod(__file);
	    return libc_fchmodat(__fd, __file, __mode, flags);
    } else
	    return libc_fchmodat(__fd, __file, __mode, flags);
}

int fchmodat(int __fd, __const char *__file, __mode_t __mode, int flags)
{
    WISK_LOG(WISK_LOG_TRACE, "fchmodat(%d, %s, %d)", __fd, __file, __mode, flags);
	return wisk_fchmodat(__fd, __file, __mode, flags);
}
#endif


/****************************
 * Thread safe code
 ***************************/
static void wisk_thread_prepare(void)
{
//	WISK_LOG(WISK_LOG_TRACE, "wisk_thread_prepare: ");
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
//	WISK_LOG(WISK_LOG_TRACE, "wisk_thread_parent: ");
	WISK_UNLOCK_ALL;
}

static void wisk_thread_child(void)
{
//	WISK_LOG(WISK_LOG_TRACE, "wisk_thread_child: ");
	WISK_UNLOCK_ALL;
}

/****************************
 * CONSTRUCTOR
 ***************************/
void wisk_constructor(int argc, char** argv)
{
	int ret;

	saved_argc = argc;
	saved_argv = argv;
//	logging_init();
	WISK_LOG(WISK_LOG_TRACE, "Constructor(%d, %s)", argc, argv[0]);
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
	if (fs_tracker_enabled())
    	wisk_report_commandcomplete();
	if (wisk.libc.handle != NULL && wisk.libc.handle != RTLD_NEXT) {
		dlclose(wisk.libc.handle);
	}
	if (wisk.libc.socket_handle && wisk.libc.socket_handle != RTLD_NEXT) {
		dlclose(wisk.libc.socket_handle);
	}
	WISK_LOG(WISK_LOG_TRACE, "Destructor ");
}
