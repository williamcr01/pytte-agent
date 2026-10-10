/* Processes on POSIX (section 15.3): fork and exec in a new session, so
 * the command has no controlling terminal (it cannot read the TUI's keys
 * or prompt on /dev/tty) and its whole group can be signalled. On Linux a
 * pidfd tells when the process ended even while a background child keeps
 * the pipe open. */
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
    int out, exitfd;
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
    for (i = 0; environ[i]; i++)
        n++;
    for (i = 0; add && add[i]; i++)
        n++;
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

pt_proc *pt_proc_spawn(const char *const argv[], const char *cwd, const char *const unset[],
                       const char *const add[], char *err, size_t cap)
{
    pt_proc *p = (pt_proc *)calloc(1, sizeof *p);
    char **env = make_env(unset, add);
    int fds[2] = {-1, -1}, devnull;
    if (!p || !env) {
        snprintf(err, cap, "out of memory");
        goto fail;
    }
    p->exitfd = -1;
    if (pipe(fds) != 0) {
        snprintf(err, cap, "pipe: %s", strerror(errno));
        goto fail;
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
        devnull = open("/dev/null", O_RDONLY);
        if (devnull < 0 || dup2(devnull, 0) < 0 || dup2(fds[1], 1) < 0 || dup2(fds[1], 2) < 0)
            _exit(127);
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
    fcntl(p->out, F_SETFD, FD_CLOEXEC);
    fcntl(p->out, F_SETFL, fcntl(p->out, F_GETFL) | O_NONBLOCK); /* our pipe, never a terminal */
#if defined(__linux__) && defined(SYS_pidfd_open)
    p->exitfd = (int)syscall(SYS_pidfd_open, p->pid, 0);
    if (p->exitfd >= 0)
        fcntl(p->exitfd, F_SETFD, FD_CLOEXEC);
#endif
    free(env);
    return p;
fail:
    if (fds[0] >= 0) {
        close(fds[0]);
        close(fds[1]);
    }
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

long pt_proc_read(pt_proc *p, void *buf, size_t cap)
{
    ssize_t r;
    if (p->out < 0)
        return 0;
    do
        r = read(p->out, buf, cap);
    while (r < 0 && errno == EINTR);
    if (r < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : 0;
    return (long)r;
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
    free(p);
}
