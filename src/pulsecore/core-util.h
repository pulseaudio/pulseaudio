#ifndef foocoreutilhfoo
#define foocoreutilhfoo

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006-2007 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <sys/types.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#include <pulse/gccmacro.h>
#include <pulsecore/macro.h>
#include <pulsecore/socket.h>

#ifndef PACKAGE
#error "Please include config.h before including this file!"
#endif

struct timeval;

/* These resource limits are pretty new on Linux, let's define them
 * here manually, in case the kernel is newer than the glibc */
#if !defined(RLIMIT_NICE) && defined(__linux__)
#define RLIMIT_NICE 13
#endif
#if !defined(RLIMIT_RTPRIO) && defined(__linux__)
#define RLIMIT_RTPRIO 14
#endif
#if !defined(RLIMIT_RTTIME) && defined(__linux__)
#define RLIMIT_RTTIME 15
#endif

void pa_make_fd_nonblock(int fd);
void pa_make_fd_cloexec(int fd);

int pa_make_secure_dir(const char* dir, mode_t m, uid_t uid, gid_t gid);
int pa_make_secure_parent_dir(const char *fn, mode_t, uid_t uid, gid_t gid);

ssize_t pa_read(int fd, void *buf, size_t count, int *type);
ssize_t pa_write(int fd, const void *buf, size_t count, int *type);
ssize_t pa_loop_read(int fd, void*data, size_t size, int *type);
ssize_t pa_loop_write(int fd, const void*data, size_t size, int *type);

int pa_close(int fd);

void pa_check_signal_is_blocked(int sig);

char *pa_sprintf_malloc(const char *format, ...) PA_GCC_PRINTF_ATTR(1,2);
char *pa_vsprintf_malloc(const char *format, va_list ap);

char *pa_strlcpy(char *b, const char *s, size_t l);

char *pa_parent_dir(const char *fn);

int pa_make_realtime(int rtprio);
int pa_raise_priority(int nice_level);
void pa_reset_priority(void);

int pa_parse_boolean(const char *s) PA_GCC_PURE;

static inline const char *pa_yes_no(pa_bool_t b) {
    return b ? "yes" : "no";
}

static inline const char *pa_strnull(const char *x) {
    return x ? x : "(null)";
}

static inline const char *pa_strempty(const char *x) {
    return x ? x : "";
}

static inline const char *pa_strna(const char *x) {
    return x ? x : "n/a";
}

char *pa_split(const char *c, const char*delimiters, const char **state);
char *pa_split_spaces(const char *c, const char **state);

char *pa_strip_nl(char *s);
char *pa_strip(char *s);

const char *pa_sig2str(int sig) PA_GCC_PURE;

int pa_own_uid_in_group(const char *name, gid_t *gid);
int pa_uid_in_group(uid_t uid, const char *name);
gid_t pa_get_gid_of_group(const char *name);
int pa_check_in_group(gid_t g);

int pa_lock_fd(int fd, int b);

int pa_lock_lockfile(const char *fn);
int pa_unlock_lockfile(const char *fn, int fd);

char *pa_hexstr(const uint8_t* d, size_t dlength, char *s, size_t slength);
size_t pa_parsehex(const char *p, uint8_t *d, size_t dlength);

pa_bool_t pa_startswith(const char *s, const char *pfx) PA_GCC_PURE;
pa_bool_t pa_endswith(const char *s, const char *sfx) PA_GCC_PURE;

FILE *pa_open_config_file(const char *global, const char *local, const char *env, char **result);
char* pa_find_config_file(const char *global, const char *local, const char *env);

char *pa_get_runtime_dir(void);
char *pa_get_state_dir(void);
char *pa_get_home_dir_malloc(void);
char *pa_get_binary_name_malloc(void);
char *pa_runtime_path(const char *fn);
char *pa_state_path(const char *fn, pa_bool_t prepend_machine_id);

int pa_atoi(const char *s, int32_t *ret_i);
int pa_atou(const char *s, uint32_t *ret_u);
int pa_atol(const char *s, long *ret_l);
int pa_atod(const char *s, double *ret_d);

size_t pa_snprintf(char *str, size_t size, const char *format, ...);
size_t pa_vsnprintf(char *str, size_t size, const char *format, va_list ap);

char *pa_truncate_utf8(char *c, size_t l);

int pa_match(const char *expr, const char *v);

char *pa_getcwd(void);
char *pa_make_path_absolute(const char *p);
pa_bool_t pa_is_path_absolute(const char *p);

void *pa_will_need(const void *p, size_t l);

static inline int pa_is_power_of_two(unsigned n) {
    return !(n & (n - 1));
}

static inline unsigned pa_ulog2(unsigned n) {

    if (n <= 1)
        return 0;

#if __GNUC__ >= 4 || (__GNUC__ == 3 && __GNUC_MINOR__ >= 4)
    return 8U * (unsigned) sizeof(unsigned) - (unsigned) __builtin_clz(n) - 1;
#else
{
    unsigned r = 0;

    for (;;) {
        n = n >> 1;

        if (!n)
            return r;

        r++;
    }
}
#endif
}

static inline unsigned pa_make_power_of_two(unsigned n) {

    if (pa_is_power_of_two(n))
        return n;

    return 1U << (pa_ulog2(n) + 1);
}

void pa_close_pipe(int fds[2]);

char *pa_readlink(const char *p);

int pa_close_all(int except_fd, ...);
int pa_close_allv(const int except_fds[]);
int pa_unblock_sigs(int except, ...);
int pa_unblock_sigsv(const int except[]);
int pa_reset_sigs(int except, ...);
int pa_reset_sigsv(const int except[]);

void pa_set_env(const char *key, const char *value);
void pa_set_env_and_record(const char *key, const char *value);
void pa_unset_env_recorded(void);

pa_bool_t pa_in_system_mode(void);

#define pa_streq(a,b) (!strcmp((a),(b)))
pa_bool_t pa_str_in_list_spaces(const char *needle, const char *haystack);

char *pa_get_host_name_malloc(void);
char *pa_get_user_name_malloc(void);

char *pa_machine_id(void);
char *pa_session_id(void);
char *pa_uname_string(void);

#ifdef HAVE_VALGRIND_MEMCHECK_H
pa_bool_t pa_in_valgrind(void);
#else
static inline pa_bool_t pa_in_valgrind(void) {
    return FALSE;
}
#endif

unsigned pa_gcd(unsigned a, unsigned b);
void pa_reduce(unsigned *num, unsigned *den);

unsigned pa_ncpus(void);

char *pa_replace(const char*s, const char*a, const char *b);

/* Escapes p by inserting backslashes in front of backslashes. chars is a
 * regular (i.e. NULL-terminated) string containing additional characters that
 * should be escaped. chars can be NULL. The caller has to free the returned
 * string. */
char *pa_escape(const char *p, const char *chars);

/* Does regular backslash unescaping. Returns the argument p. */
char *pa_unescape(char *p);

char *pa_realpath(const char *path);

void pa_disable_sigpipe(void);

void pa_xfreev(void**a);

static inline void pa_xstrfreev(char **a) {
    pa_xfreev((void**) a);
}

char **pa_split_spaces_strv(const char *s);

char* pa_maybe_prefix_path(const char *path, const char *prefix);

/* Returns size of the specified pipe or 4096 on failure */
size_t pa_pipe_buf(int fd);

void pa_reset_personality(void);

/* We abuse __OPTIMIZE__ as a check whether we are a debug build
 * or not. If we are and are run from the build tree then we
 * override the search path to point to our build tree */
#if defined(__linux__) && !defined(__OPTIMIZE__)
pa_bool_t pa_run_from_build_tree(void);
#else
static inline pa_bool_t pa_run_from_build_tree(void) {return FALSE;}
#endif

const char *pa_get_temp_dir(void);

int pa_open_cloexec(const char *fn, int flags, mode_t mode);
int pa_socket_cloexec(int domain, int type, int protocol);
int pa_pipe_cloexec(int pipefd[2]);
int pa_accept_cloexec(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
FILE* pa_fopen_cloexec(const char *path, const char *mode);

void pa_nullify_stdfds(void);

char *pa_read_line_from_file(const char *fn);
pa_bool_t pa_running_in_vm(void);

#ifdef OS_IS_WIN32
char *pa_win32_get_toplevel(HANDLE handle);
#endif

#endif
