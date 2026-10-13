/* Processes on POSIX (section 15.3): fork and exec in a new session, so
 * the command has no controlling terminal (it cannot read the TUI's keys
 * or prompt on /dev/tty) and its whole group can be signalled. On Linux a
 * pidfd tells when the process ended even while a background child keeps
 * the pipe open. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
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
#include <sys/syscall.h>
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
