/* Processes on POSIX (section 15.3): fork and exec in a new session, so
 * the command has no controlling terminal (it cannot read the TUI's keys
 * or prompt on /dev/tty) and its whole group can be signalled. On Linux a
 * pidfd tells when the process ended even while a background child keeps
 * the pipe open. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE /* sysctl */
#include "drv/drv.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <dirent.h>
#include <sys/syscall.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

extern char **environ;

struct pt_proc {
    pid_t pid;
    int out, err, in, exitfd;
    bool reaped;
    int status;
};

static bool named(const char *kv, const char *const names[])
{
    size_t i, n;
    for (i = 0; names && names[i]; i++) {
        n = strlen(names[i]);
        if (strncmp(kv, names[i], n) == 0 && kv[n] == '=')
            return true;
    }
    return false;
}

/* Before fork: async-signal-safe code only may run in the child. */
static char **make_env(const char *const unset[], const char *const add[])
{
    size_t n = 0, k = 0, i;
    char **env;
    for (i = 0; environ[i]; i++) {
        if (n == SIZE_MAX)
            return NULL;
        n++;
    }
    for (i = 0; add && add[i]; i++) {
        if (n == SIZE_MAX)
            return NULL;
        n++;
    }
    if (n > SIZE_MAX / sizeof *env - 1)
        return NULL;
    env = (char **)malloc((n + 1) * sizeof *env);
    if (!env)
        return NULL;
    for (i = 0; environ[i]; i++)
        if (!named(environ[i], unset) && !named(environ[i], (const char *const *)add))
            env[k++] = environ[i];
    for (i = 0; add && add[i]; i++)
        env[k++] = (char *)add[i];
    env[k] = NULL;
    return env;
}

/* A pipe whose ends close on exec. */
static bool cloexec_pipe(int fds[2], char *err, size_t cap)
{
#if defined(__linux__)
    if (pipe2(fds, O_CLOEXEC) != 0) {
        snprintf(err, cap, "pipe: %s", strerror(errno));
        fds[0] = fds[1] = -1;
        return false;
    }
#else
    if (pipe(fds) != 0) {
        snprintf(err, cap, "pipe: %s", strerror(errno));
        fds[0] = fds[1] = -1;
        return false;
    }
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0) {
        snprintf(err, cap, "fcntl: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        fds[0] = fds[1] = -1;
        return false;
    }
#endif
    return true;
}

static void close_pair(int fds[2])
{
    if (fds[0] >= 0)
        close(fds[0]);
    if (fds[1] >= 0)
        close(fds[1]);
    fds[0] = fds[1] = -1;
}

pt_proc *pt_proc_spawn(const char *const argv[], const char *cwd, const char *const unset[],
                       const char *const add[], const pt_proc_opts *opts, char *err, size_t cap)
{
    pt_proc *p = (pt_proc *)calloc(1, sizeof *p);
    char **env = make_env(unset, add);
    int fds[2] = {-1, -1}, efds[2] = {-1, -1}, ifds[2] = {-1, -1}, in;
    bool want_in = opts && opts->stdin_pipe, split = opts && opts->split_err;
    if (!p || !env) {
        snprintf(err, cap, "out of memory");
        goto fail;
    }
    p->exitfd = p->err = p->in = -1;
    if (!cloexec_pipe(fds, err, cap) || (split && !cloexec_pipe(efds, err, cap)) ||
        (want_in && !cloexec_pipe(ifds, err, cap)))
        goto fail;
    if (want_in) {
        struct sigaction sa;
        if (sigaction(SIGPIPE, NULL, &sa) == 0 && sa.sa_handler == SIG_DFL)
            signal(SIGPIPE, SIG_IGN);
    }
    p->pid = fork();
    if (p->pid < 0) {
        snprintf(err, cap, "fork: %s", strerror(errno));
        goto fail;
    }
    if (p->pid == 0) {
        static const int sigs[] = {SIGINT, SIGQUIT, SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU, SIGPIPE, SIGCHLD, SIGWINCH};
        sigset_t none;
        size_t i;
        setsid();
        for (i = 0; i < sizeof sigs / sizeof *sigs; i++)
            signal(sigs[i], SIG_DFL);
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        in = want_in ? ifds[0] : open("/dev/null", O_RDONLY);
        if (in < 0 || (!want_in && in > 2 && fcntl(in, F_SETFD, FD_CLOEXEC) < 0) ||
            dup2(in, 0) < 0 || dup2(fds[1], 1) < 0 || dup2(split ? efds[1] : fds[1], 2) < 0)
            _exit(127);
        /* dup2 clears FD_CLOEXEC except when source and destination match. */
        if (fcntl(0, F_SETFD, 0) < 0 || fcntl(1, F_SETFD, 0) < 0 || fcntl(2, F_SETFD, 0) < 0)
            _exit(127);
        /* The rest close on exec. */
        if (cwd && chdir(cwd) != 0) {
            static const char msg[] = "pytte: cannot enter the session's directory\n";
            ssize_t w = write(2, msg, sizeof msg - 1);
            (void)w;
            _exit(127);
        }
        execve(argv[0], (char *const *)argv, env);
        _exit(127);
    }
    close(fds[1]);
    p->out = fds[0];
    fcntl(p->out, F_SETFL, fcntl(p->out, F_GETFL) | O_NONBLOCK); /* our pipe, never a terminal */
    if (split) {
        close(efds[1]);
        p->err = efds[0];
        fcntl(p->err, F_SETFL, fcntl(p->err, F_GETFL) | O_NONBLOCK);
    }
    if (want_in) {
        close(ifds[0]);
        p->in = ifds[1];
        fcntl(p->in, F_SETFL, fcntl(p->in, F_GETFL) | O_NONBLOCK);
    }
#if defined(__linux__) && defined(SYS_pidfd_open)
    p->exitfd = (int)syscall(SYS_pidfd_open, p->pid, 0);
    if (p->exitfd >= 0)
        fcntl(p->exitfd, F_SETFD, FD_CLOEXEC);
#endif
    free(env);
    return p;
fail:
    close_pair(fds);
    close_pair(efds);
    close_pair(ifds);
    free(env);
    free(p);
    return NULL;
}

int pt_proc_out_fd(const pt_proc *p)
{
    return p->out;
}

int pt_proc_exit_fd(const pt_proc *p)
{
    return p->reaped ? -1 : p->exitfd;
}

int pt_proc_err_fd(const pt_proc *p)
{
    return p->err;
}

int pt_proc_in_fd(const pt_proc *p)
{
    return p->in;
}

static long read_fd(int fd, void *buf, size_t cap)
{
    ssize_t r;
    if (fd < 0)
        return 0;
    do
        r = read(fd, buf, cap);
    while (r < 0 && errno == EINTR);
    if (r < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : 0;
    return (long)r;
}

long pt_proc_read(pt_proc *p, void *buf, size_t cap)
{
    return read_fd(p->out, buf, cap);
}

long pt_proc_read_err(pt_proc *p, void *buf, size_t cap)
{
    return read_fd(p->err, buf, cap);
}

long pt_proc_write(pt_proc *p, const void *buf, size_t n)
{
    ssize_t r;
    if (p->in < 0)
        return -1;
    if (!n)
        return 0;
    do
        r = write(p->in, buf, n);
    while (r < 0 && errno == EINTR);
    if (r < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    return (long)r;
}

void pt_proc_close_in(pt_proc *p)
{
    if (p->in >= 0)
        close(p->in);
    p->in = -1;
}

bool pt_proc_reap(pt_proc *p, bool wait, int *status)
{
    int st;
    pid_t r;
    if (!p->reaped) {
        do
            r = waitpid(p->pid, &st, wait ? 0 : WNOHANG);
        while (r < 0 && errno == EINTR);
        if (r == 0)
            return false;
        p->reaped = true;
        p->status = r < 0 ? 127 : WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
        if (p->exitfd >= 0) {
            close(p->exitfd);
            p->exitfd = -1;
        }
    }
    *status = p->status;
    return true;
}

void pt_proc_stop(pt_proc *p, bool hard)
{
    /* The group: setsid made pid its leader. Once reaped, the ID may
     * belong to someone else. */
    if (!p->reaped)
        kill(-p->pid, hard ? SIGKILL : SIGTERM);
}

void pt_proc_free(pt_proc *p)
{
    int st;
    if (!p)
        return;
    if (!p->reaped) {
        pt_proc_stop(p, true);
        pt_proc_reap(p, true, &st);
    }
    if (p->out >= 0)
        close(p->out);
    if (p->err >= 0)
        close(p->err);
    pt_proc_close_in(p);
    free(p);
}

/* ---- what outlives a crash ----
 * A mark is "<pid> <boot> <start>": the group's ID, which is its leader's
 * pid (setsid), and when that leader started, so that a later process
 * that gets the same pid is never taken for it. */

#ifdef __linux__
/* From /proc/<pid>/stat, after the command: the group, the session and
 * the start time (clock ticks since boot). */
static bool linux_stat(long pid, long *pgrp, long *sid, unsigned long long *start)
{
    char path[64], buf[1024], *p;
    ssize_t n;
    int fd, field;
    snprintf(path, sizeof path, "/proc/%ld/stat", pid);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return false;
    do
        n = read(fd, buf, sizeof buf - 1);
    while (n < 0 && errno == EINTR);
    close(fd);
    if (n <= 0)
        return false;
    buf[n] = 0;
    if (!(p = (strrchr)(buf, ')'))) /* the function: glibc's macro is C11 */
        return false;
    p++;
    for (field = 0; field <= 19; field++) { /* state, ppid, pgrp, session, ... starttime */
        unsigned long long v;
        char *end;
        while (*p == ' ')
            p++;
        if (!*p)
            return false;
        if (field == 0) {
            while (*p && *p != ' ')
                p++;
            continue;
        }
        v = strtoull(p, &end, 10);
        if (end == p)
            return false;
        p = end;
        if (field == 2)
            *pgrp = (long)v;
        else if (field == 3)
            *sid = (long)v;
        else if (field == 19)
            *start = v;
    }
    return true;
}

static bool start_of(long pid, char *out, size_t cap)
{
    long pgrp, sid;
    unsigned long long start;
    if (!linux_stat(pid, &pgrp, &sid, &start))
        return false;
    snprintf(out, cap, "%llu", start);
    return true;
}

/* The leader is gone but its group may not be: a member of its session
 * that started no earlier than it did. The kernel does not hand out a pid
 * while a group or session still uses it as its ID. */
static bool group_lives(long pid, const char *start)
{
    unsigned long long since = strtoull(start, NULL, 10);
    DIR *d = opendir("/proc");
    struct dirent *e;
    bool found = false;
    if (!d)
        return false;
    while (!found && (e = readdir(d))) {
        long pgrp, sid, member;
        unsigned long long at;
        char *end;
        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue;
        member = strtol(e->d_name, &end, 10);
        if (*end || !linux_stat(member, &pgrp, &sid, &at))
            continue;
        found = pgrp == pid && sid == pid && at >= since;
    }
    closedir(d);
    return found;
}
#elif defined(__APPLE__)
static bool start_of(long pid, char *out, size_t cap)
{
    struct kinfo_proc kp;
    size_t len = sizeof kp;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid};
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0 || len == 0)
        return false;
    snprintf(out, cap, "%ld.%06ld", (long)kp.kp_proc.p_starttime.tv_sec, (long)kp.kp_proc.p_starttime.tv_usec);
    return true;
}

static bool group_lives(long pid, const char *start)
{
    (void)pid;
    (void)start;
    return false; /* only a leader that still runs is known for sure */
}
#else
static bool start_of(long pid, char *out, size_t cap)
{
    (void)pid;
    (void)out;
    (void)cap;
    return false;
}

static bool group_lives(long pid, const char *start)
{
    (void)pid;
    (void)start;
    return false;
}
#endif

/* The boot ID, or "-" where there is none (the start time is absolute). */
static void boot_of(char *out, size_t cap)
{
    pt_sys_identity(NULL, 0, out, cap, NULL);
    if (!out[0])
        snprintf(out, cap, "-");
}

int64_t pt_proc_group(const pt_proc *p)
{
    return (int64_t)p->pid;
}

size_t pt_proc_mark(const pt_proc *p, char *out, size_t cap)
{
    char boot[64], start[32];
    int n;
    if (p->reaped || !start_of((long)p->pid, start, sizeof start))
        return 0;
    boot_of(boot, sizeof boot);
    n = snprintf(out, cap, "%ld %s %s", (long)p->pid, boot, start);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

bool pt_proc_stop_marked(pt_str mark)
{
    char text[160], boot[64], start[32], now[64], cur[32];
    long pid;
    if (!mark.n || mark.n >= sizeof text)
        return false;
    memcpy(text, mark.p, mark.n);
    text[mark.n] = 0;
    if (sscanf(text, "%ld %63s %31s", &pid, boot, start) != 3 || pid <= 1 || (long)(pid_t)pid != pid)
        return false;
    boot_of(now, sizeof now);
    if (strcmp(boot, now) != 0)
        return false; /* rebooted since: nothing of it runs */
    if (start_of(pid, cur, sizeof cur)) {
        if (strcmp(cur, start) != 0)
            return false; /* the pid is another process's now */
    } else if (!group_lives(pid, start)) {
        return false;
    }
    return kill(-(pid_t)pid, SIGKILL) == 0;
}
