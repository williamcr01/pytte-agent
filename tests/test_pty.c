/* The real pytte binary in a pseudo-terminal (section 22.5), on the faux
 * provider, with its output fed to the strict emulator.
 *
 * pytte runs under a tiny job-control "shell" (a session leader that gives
 * it the foreground and continues it after Ctrl-Z), so suspending works as
 * it does under a real shell.
 *
 *   PYTTE_BIN   the binary (default build/pytte) */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "t.h"
#include "vt.c"
#include "drv/drv.h"

typedef struct pty {
    int fd;
    pid_t leader, pid; /* the shell, and pytte */
    vt *v;
    bool eof;
    int status;
    pt_buf scratch;
    /* With watch: the cursor's row in each finished frame, and whether it
     * ever went up (the input jumped). */
    bool watch, rose;
    int row;
} pty;

static char state_dir[256], config_dir[256], self_bin[4096];
static bool has(pty *p, const char *text);
static const char *bin;

static void put(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return;
        p += w;
        n -= (size_t)w;
    }
}

/* The session leader: runs pytte as the foreground job, reports its pid,
 * continues it when it stops, and exits with its status. */
static void leader(const char *slave, char *const argv[], int info)
{
    int fd, go[2], st;
    pid_t gc;
    char c = 0;
    setsid();
    fd = open(slave, O_RDWR);
    if (fd < 0)
        _exit(99);
    ioctl(fd, TIOCSCTTY, 0);
    signal(SIGTTOU, SIG_IGN);
    if (pipe(go) != 0)
        _exit(99);
    gc = fork();
    if (gc == 0) {
        setpgid(0, 0);
        close(go[1]);
        if (read(go[0], &c, 1) != 1)
            _exit(99);
        signal(SIGTTOU, SIG_DFL);
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2)
            close(fd);
        close(info);
        execv(argv[0], argv);
        _exit(98);
    }
    setpgid(gc, gc);
    tcsetpgrp(fd, gc);
    put(go[1], "g", 1);
    put(info, (const char *)&gc, sizeof gc);
    close(info);
    for (;;) {
        if (waitpid(gc, &st, WUNTRACED) < 0)
            _exit(97);
        if (WIFSTOPPED(st)) {
            struct timespec ts = {0, 300 * 1000000L};
            tcsetpgrp(fd, getpgrp());
            put(fd, "[stopped]\n", 10);
            nanosleep(&ts, NULL);
            tcsetpgrp(fd, gc);
            kill(-gc, SIGCONT);
            continue;
        }
        _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
    }
}

static int proc_fd_probe(void)
{
    struct stat out, other;
    int fd;
    if (fstat(STDOUT_FILENO, &out) != 0)
        return 2;
    for (fd = 3; fd < 4096; fd++)
        if (fcntl(fd, F_GETFD) >= 0 && fstat(fd, &other) == 0 &&
            out.st_dev == other.st_dev && out.st_ino == other.st_ino)
            return 1; /* an original pipe end survived exec */
    return 0;
}

static void test_identity_zero_capacity(void)
{
    char host = 'h', boot = 'b';
    int64_t pid = 0;
    pt_sys_identity(&host, 0, &boot, 0, &pid);
    CHECK(host == 'h' && boot == 'b');
    CHECK(pid == (int64_t)getpid());
    pt_sys_identity(NULL, 0, NULL, 0, NULL);
}

static void test_proc_cloexec(void)
{
    char err[300];
    const char *argv[] = {self_bin, "--proc-fd-probe", NULL};
    pt_proc *p;
    int status = -1;
    p = pt_proc_spawn(argv, NULL, NULL, NULL, NULL, err, sizeof err);
    CHECK(p != NULL);
    if (!p) {
        fprintf(stderr, "    process spawn: %s\n", err);
        return;
    }
    CHECK(pt_proc_out_fd(p) > 2);
    CHECK(pt_proc_reap(p, true, &status));
    CHECK(status == 0); /* the executed child must not inherit its pipe reader */
    pt_proc_free(p);
}

typedef struct faux_probe {
    unsigned heads, bodies, dones;
    bool error;
} faux_probe;

static void faux_head(void *ud, int status, pt_str retry)
{
    faux_probe *p = (faux_probe *)ud;
    (void)status;
    (void)retry;
    p->heads++;
}

static void faux_body(void *ud, const char *bytes, size_t n)
{
    faux_probe *p = (faux_probe *)ud;
    (void)bytes;
    if (n)
        p->bodies++;
}

static void faux_done(void *ud, const char *error)
{
    faux_probe *p = (faux_probe *)ud;
    p->dones++;
    p->error = error != NULL;
}

static void test_faux_delay(void)
{
    pt_http_driver *d = pt_http_faux_new(PT_S(
        "{\"chunk\":1,\"delay_ms\":1000,\"turns\":[{\"content\":[{\"type\":\"text\",\"text\":\"x\"}]}]}"));
    pt_http_req req;
    pt_http_sink sink;
    faux_probe seen;
    pt_waitfd fd;
    void *handle;
    int wake[2];
    memset(&req, 0, sizeof req);
    req.body = PT_S("{}");
    memset(&seen, 0, sizeof seen);
    CHECK(d != NULL);
    if (!d)
        return;
    sink = (pt_http_sink){&seen, faux_head, faux_body, faux_done, NULL};
    handle = d->start(d, &req, &sink);
    CHECK(handle != NULL);
    if (!handle) {
        d->free(d);
        return;
    }
    CHECK(d->pump(d, 0, NULL, 0) == 1);
    CHECK(seen.heads == 0 && seen.bodies == 0 && seen.dones == 0);
    if (pipe(wake) != 0) {
        d->cancel(d, handle);
        d->free(d);
        CHECK(false);
        return;
    }
    CHECK(write(wake[1], "w", 1) == 1);
    fd = (pt_waitfd){wake[0], false};
    CHECK(d->pump(d, 1000, &fd, 1) == 1 && fd.ready);
    CHECK(seen.heads == 0 && seen.bodies == 0 && seen.dones == 0);
    pt_sys_sleep_ms(1020);
    CHECK(d->pump(d, 0, NULL, 0) == 1);
    CHECK(seen.heads == 1 && seen.bodies == 1 && seen.dones == 0);
    d->cancel(d, handle);
    d->free(d);
    close(wake[0]); close(wake[1]);
}

static void test_faux_cut(void)
{
    pt_http_driver *d;
    pt_http_req req;
    pt_http_sink sink;
    faux_probe seen;
    CHECK(pt_http_faux_new((pt_str){"", SIZE_MAX}) == NULL);
    d = pt_http_faux_new(PT_S(
        "{\"turns\":[{\"cut\":1,\"content\":[{\"type\":\"text\",\"text\":\"x\"}]}]}"));
    memset(&req, 0, sizeof req);
    req.body = PT_S("{}");
    memset(&seen, 0, sizeof seen);
    CHECK(d != NULL);
    if (!d)
        return;
    sink = (pt_http_sink){&seen, faux_head, faux_body, faux_done, NULL};
    CHECK(d->start(d, &req, &sink) != NULL);
    CHECK(d->pump(d, 0, NULL, 0) == 0);
    CHECK(seen.heads == 1 && seen.bodies == 1 && seen.dones == 1 && seen.error);
    d->free(d);
}

static void pty_start(pty *p, int rows, int cols, const char *const *args)
{
    char *argv[32];
    struct winsize ws;
    int info[2], n = 0;
    const char *slave;
    memset(p, 0, sizeof *p);
    pt_buf_init(&p->scratch, T_AL);
    argv[n++] = (char *)bin;
    while (*args && n < 30)
        argv[n++] = (char *)*args++;
    argv[n] = NULL;
    p->fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (p->fd < 0 || grantpt(p->fd) != 0 || unlockpt(p->fd) != 0 || !(slave = ptsname(p->fd))) {
        perror("pty");
        exit(1);
    }
    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ioctl(p->fd, TIOCSWINSZ, &ws);
    p->v = vt_new(rows, cols, true);
    p->v->answer = true;
    p->v->kitty_ok = true;
    if (pipe(info) != 0)
        exit(1);
    fflush(NULL);
    p->leader = fork();
    if (p->leader == 0) {
        close(p->fd);
        close(info[0]);
        leader(slave, argv, info[1]);
    }
    close(info[1]);
    if (read(info[0], &p->pid, sizeof p->pid) != (ssize_t)sizeof p->pid)
        p->pid = -1;
    close(info[0]);
}

/* Feed bytes to the emulator a frame at a time (a frame ends with the end
 * of a synchronized update), checking where each leaves the cursor. */
static void watch_frames(pty *p, const char *buf, size_t n)
{
    static const char end[] = "\x1b[?2026l";
    size_t at = 0;
    while (at < n) {
        size_t k = at;
        while (k < n && !(n - k >= sizeof end - 1 && memcmp(buf + k, end, sizeof end - 1) == 0))
            k++;
        k = k < n ? k + sizeof end - 1 : n;
        vt_write(p->v, buf + at, k - at);
        at = k;
        if (p->v->sync || p->v->hidden)
            continue;
        if (p->v->r < p->row && !p->rose) {
            p->rose = true;
            has(p, "");
            fprintf(stderr, "    the input went up from row %d to %d; the screen:\n%s\n", p->row, p->v->r,
                    p->scratch.p);
        }
        p->row = p->v->r;
    }
}

/* Feed what pytte wrote to the emulator, and the emulator's replies back,
 * for up to ms. */
static void pump(pty *p, int ms)
{
    int64_t end = pt_sys_now_ms() + ms;
    char buf[8192];
    while (!p->eof) {
        struct pollfd pf;
        int64_t left = end - pt_sys_now_ms();
        ssize_t n;
        pf.fd = p->fd;
        pf.events = POLLIN;
        if (poll(&pf, 1, left > 0 ? (int)left : 0) <= 0)
            break;
        n = read(p->fd, buf, sizeof buf);
        if (n <= 0) { /* EIO: every slave fd is closed */
            p->eof = true;
            break;
        }
        if (p->watch)
            watch_frames(p, buf, (size_t)n);
        else
            vt_write(p->v, buf, (size_t)n);
        if (p->v->replies.n) {
            put(p->fd, p->v->replies.p, p->v->replies.n);
            pt_buf_reset(&p->v->replies);
        }
    }
}

static bool has(pty *p, const char *text)
{
    vt_lines(p->v, p->v->rows, &p->scratch);
    pt_buf_putc(&p->scratch, 0);
    return strstr(p->scratch.p, text) != NULL;
}

/* How long to wait for what follows a write that is synced to disk (a
 * saved setting or key): quick when idle, seconds on a busy disk. */
#define DURABLE_MS 10000

static bool wait_for(pty *p, const char *text, int ms)
{
    int64_t end = pt_sys_now_ms() + ms;
    while (!has(p, text)) {
        if (pt_sys_now_ms() > end || p->eof) {
            fprintf(stderr, "    waiting for \"%s\" timed out; the screen:\n%s\n", text, p->scratch.p);
            return false;
        }
        pump(p, 20);
    }
    return true;
}

/* Like wait_for, on the screen with each word-wrapped line whole again:
 * pytte continues a line on the next row after its hanging indent (two
 * spaces), and the row break becomes the space it was. For a line whose
 * wrap depends on the machine, such as one holding a path. */
static bool wait_for_joined(pty *p, const char *text, int ms)
{
    int64_t end = pt_sys_now_ms() + ms;
    pt_buf j;
    bool found = false;
    pt_buf_init(&j, pt_sys_allocator());
    for (;;) {
        size_t i;
        vt_lines(p->v, p->v->rows, &p->scratch);
        pt_buf_reset(&j);
        for (i = 0; i < p->scratch.n; i++) {
            const char *c = p->scratch.p + i;
            if (*c == '\n' && i + 3 < p->scratch.n && c[1] == ' ' && c[2] == ' ' && c[3] != ' ' && c[3] != '\n') {
                pt_buf_putc(&j, ' ');
                i += 2;
            } else {
                pt_buf_putc(&j, *c);
            }
        }
        pt_buf_putc(&j, 0);
        if ((found = strstr(j.p, text) != NULL))
            break;
        if (pt_sys_now_ms() > end || p->eof) {
            fprintf(stderr, "    waiting for \"%s\" (rows joined) timed out; the screen:\n%s\n", text, j.p);
            break;
        }
        pump(p, 20);
    }
    pt_buf_free(&j);
    return found;
}

/* Until no run is active (the activity line is gone). */
/* A run is active: the activity row above the input shows its clock. Its
 * dot blinks, so the row starts with "• " or two spaces. An input that
 * waits (for the session to open, or for its MCP fetches) is work still
 * to come: its row says "queued · " or "fetching · ". */
static bool busy(pty *p)
{
    static const char *const what[] = {"working · ", "thinking · ", "writing · ", "compacting · ",
                                       "waiting for you · ", "running ", "retrying in ", "queued · ",
                                       "fetching · "};
    char row[64];
    size_t i;
    for (i = 0; i < sizeof what / sizeof what[0]; i++) {
        snprintf(row, sizeof row, "\n• %s", what[i]);
        if (has(p, row))
            return true;
        snprintf(row, sizeof row, "\n  %s", what[i]);
        if (has(p, row))
            return true;
    }
    return false;
}

static bool wait_idle(pty *p, int ms)
{
    int64_t end = pt_sys_now_ms() + ms;
    pump(p, 50);
    while (busy(p)) {
        if (pt_sys_now_ms() > end || p->eof) {
            fprintf(stderr, "    still running; the screen:\n%s\n", p->scratch.p);
            return false;
        }
        pump(p, 20);
    }
    return true;
}

static void keys(pty *p, const char *s)
{
    put(p->fd, s, strlen(s));
    pump(p, 30);
}

static int finish(pty *p, int ms)
{
    int64_t end = pt_sys_now_ms() + ms;
    int st = -1;
    while (waitpid(p->leader, &st, WNOHANG) == 0) {
        if (pt_sys_now_ms() > end) {
            kill(p->pid, SIGKILL);
            kill(p->leader, SIGKILL);
            waitpid(p->leader, &st, 0);
            fprintf(stderr, "    pytte did not exit\n");
            break;
        }
        pump(p, 20);
    }
    pump(p, 100); /* the last bytes, the watchdog's */
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void modes_restored(pty *p)
{
    CHECK(p->v->kitty == 0);
    CHECK(!p->v->hidden);
    CHECK(!p->v->sync);
    if (p->v->err[0])
        fprintf(stderr, "    emulator: %s\n", p->v->err);
    CHECK(!p->v->err[0]);
}

static void pty_free(pty *p)
{
    if (getenv("PTY_DUMP")) { /* the transcript and screen, for looking at */
        vt_lines(p->v, p->v->rows, &p->scratch);
        fprintf(stderr, "----\n%.*s----\n", (int)p->scratch.n, p->scratch.p);
    }
    close(p->fd);
    vt_free(p->v);
    pt_buf_free(&p->scratch);
}

/* The newest session's log. */
static char *last_log(size_t *n)
{
    char path[600], cmd[700];
    FILE *f;
    snprintf(cmd, sizeof cmd, "ls -d %s/pytte/sessions/*/ | tail -1", state_dir);
    if (!(f = popen(cmd, "r")) || !fgets(path, sizeof path - 20, f))
        return NULL;
    pclose(f);
    path[strcspn(path, "\n")] = 0;
    strcat(path, "log.jsonl");
    return pt_sys_read_file(path, n);
}

static void test_chat(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/hello.json", NULL};
    pty_start(&p, 24, 80, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    CHECK(p.v->kitty == 1); /* the query was answered: flags pushed */
    CHECK(p.v->paste);
    keys(&p, "hi there");
    CHECK(wait_for(&p, "› hi there", 2000));
    keys(&p, "\r");
    CHECK(wait_for(&p, "Hello from the faux provider! Nothing here touched the network.", 5000));
    CHECK(wait_for(&p, "∴ thought", 2000)); /* hidden by default: only that it happened */
    CHECK(wait_idle(&p, 3000));
    CHECK(!has(&p, "A greeting."));
    CHECK(wait_for(&p, "network.\n\n· worked for ", 1000)); /* the spinner's time, kept */
    CHECK(wait_for(&p, "  ask · faux · ", 1000) && wait_for(&p, "/100k\n", 1000)); /* context used, of the window */
    keys(&p, "\x1b[100;5u"); /* Ctrl-D, kitty style */
    CHECK(finish(&p, 3000) == 0);
    CHECK(has(&p, "› hi there"));
    CHECK(has(&p, "\n\n· to continue this session: pytte -r 20")); /* the session's id */
    modes_restored(&p);
    CHECK(!p.v->paste);
    {
        /* the id it printed opens that session again */
        char id[64] = "";
        const char *at = strstr(p.scratch.p, "pytte -r "), *again[] = {"-r", id, "--script",
                                                                       "tests/faux/hello.json", NULL};
        if (at)
            sscanf(at + 9, "%63s", id);
        pty_free(&p);
        CHECK(id[0] != 0);
        pty_start(&p, 24, 80, again);
        CHECK(wait_for(&p, "› hi there\n", 3000));
        keys(&p, "\x04");
        CHECK(finish(&p, 3000) == 0);
        pty_free(&p);
    }
    /* a session where nothing was said is not kept, so there is nothing to continue */
    pty_start(&p, 24, 80, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    CHECK(!has(&p, "to continue this session"));
    pty_free(&p);
}

static void test_kill9(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/slow.json", NULL};
    const char *again[] = {"-c", "--script", "tests/faux/slow.json", NULL};
    size_t n;
    char *log;
    pty_start(&p, 24, 80, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    keys(&p, "list the files\r");
    CHECK(wait_for(&p, "No tools are available", 5000));
    kill(p.pid, SIGKILL);
    CHECK(finish(&p, 3000) == 137);
    CHECK(p.v->kitty == 0); /* the watchdog popped the flags */
    CHECK(!p.v->paste);
    modes_restored(&p);
    pty_free(&p);
    /* the transcript is the same after pytte -c, with the partial marked */
    pty_start(&p, 24, 80, again);
    CHECK(wait_for(&p, "› list the files", 3000));
    CHECK(wait_for(&p, "· interrupted", 3000));
    CHECK(wait_for(&p, "and resume.\n", 8000));
    CHECK(wait_idle(&p, 3000));
    keys(&p, "\x04"); /* Ctrl-D, legacy */
    pump(&p, 100);
    CHECK(finish(&p, 3000) == 0 || (keys(&p, "\x04"), finish(&p, 3000) == 0));
    modes_restored(&p);
    log = last_log(&n);
    CHECK(log && strstr(log, "\"event\":\"close\""));
    free(log);
    pty_free(&p);
}

static void test_suspend_and_signals(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/hello.json", NULL};
    struct winsize ws;
    int i;
    pty_start(&p, 24, 80, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    keys(&p, "draft text");
    CHECK(wait_for(&p, "› draft text", 2000));
    keys(&p, "\x1b[122;5u"); /* Ctrl-Z */
    CHECK(wait_for(&p, "[stopped]", 2000));
    CHECK(p.v->kitty == 0 && !p.v->paste); /* restored while stopped */
    CHECK(wait_for(&p, "› draft text", 3000)); /* continued: drawn again */
    pump(&p, 100);
    CHECK(p.v->kitty == 1 && p.v->paste);
    /* a resize storm */
    for (i = 0; i < 12; i++) {
        memset(&ws, 0, sizeof ws);
        ws.ws_row = (unsigned short)(16 + i % 5);
        ws.ws_col = (unsigned short)(50 + (i * 7) % 40);
        vt_resize(p.v, ws.ws_row, ws.ws_col);
        ioctl(p.fd, TIOCSWINSZ, &ws);
        pump(&p, 15);
    }
    pump(&p, 300);
    vt_row_text(p.v, p.v->r, &p.scratch);
    pt_buf_putc(&p.scratch, 0);
    CHECK(strstr(p.scratch.p, "draft text") != NULL);
    /* SIGHUP: the terminal is gone; the session is closed anyway */
    kill(p.pid, SIGHUP);
    CHECK(finish(&p, 3000) == 129);
    {
        size_t n;
        char *log = last_log(&n);
        CHECK(log && strstr(log, "\"event\":\"close\""));
        free(log);
    }
    if (p.v->err[0])
        fprintf(stderr, "    emulator: %s\n", p.v->err);
    CHECK(!p.v->err[0]);
    pty_free(&p);
}

static int count(pty *p, const char *text)
{
    const char *at;
    int n = 0;
    vt_lines(p->v, p->v->rows, &p->scratch);
    pt_buf_putc(&p->scratch, 0);
    for (at = p->scratch.p; (at = strstr(at, text)) != NULL; at += strlen(text))
        n++;
    return n;
}

/* Resizes while an answer streams leave nothing behind: the terminal has
 * rewrapped at once, so nothing may be painted with the old size while
 * the resize waits out its debounce. */
static void test_resize_while_streaming(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/slow.json", NULL};
    struct winsize ws;
    int i;
    pty_start(&p, 24, 80, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    keys(&p, "list the files\r");
    CHECK(wait_for(&p, "Let me look", 3000));
    for (i = 0; i < 40; i++) {
        memset(&ws, 0, sizeof ws);
        ws.ws_row = 24;
        ws.ws_col = (unsigned short)(i % 2 ? 80 - (i * 3) % 30 : 40 + (i * 7) % 40);
        vt_resize(p.v, ws.ws_row, ws.ws_col);
        ioctl(p.fd, TIOCSWINSZ, &ws);
        pump(&p, 25 + (i * 13) % 30);
    }
    CHECK(wait_idle(&p, 10000));
    CHECK(count(&p, "The user wants a listing") == 0); /* hidden thinking is never drawn, not even its live tail */
    CHECK(count(&p, "› list the files") == 1);
    CHECK(count(&p, "Let me look at the files first.") == 1);
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    modes_restored(&p);
    pty_free(&p);
}

/* Esc Esc interrupts; input sent meanwhile is queued, then answered. */
static void test_interrupt_and_queue(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/slow.json", NULL};
    pty_start(&p, 24, 80, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    keys(&p, "list the files\r");
    CHECK(wait_for(&p, "Let me look at the files first.\n", 5000));
    keys(&p, "and then?\r");
    CHECK(wait_for(&p, "queued · and then?", 2000));
    CHECK(wait_for(&p, "No tools are", 5000));
    /* the call shows the gist of its arguments, then how it ended */
    CHECK(wait_for(&p, "✗ bash  ls · failed\n        Invalid arguments: $: missing required property command", 1000));
    keys(&p, "\x1b[27u");
    CHECK(wait_for(&p, "esc again to interrupt", 1000));
    keys(&p, "\x1b[27u");
    CHECK(wait_for(&p, "· interrupted", 2000));
    /* the queued input goes next; the script's last turn answers it */
    CHECK(wait_for(&p, "› and then?", 3000));
    CHECK(wait_for(&p, "and resume.\n", 8000));
    CHECK(wait_idle(&p, 3000));
    CHECK(!has(&p, "queued ·"));
    keys(&p, "\x1b[100;5u");
    CHECK(finish(&p, 3000) == 0);
    modes_restored(&p);
    pty_free(&p);
}

/* The input stays where it is while a run comes and goes: rows above it
 * (the open group, the live tail, queued inputs, the activity) leave
 * blank rows behind until the transcript fills them, so the cursor's row
 * never goes up. On a fresh screen and on a full one. */
static void test_steady_input(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/steady.json", "--permission-mode", "auto", NULL};
    int full;
    for (full = 0; full < 2; full++) {
        pty_start(&p, 24, 80, args);
        CHECK(wait_for(&p, "/help for commands", 3000));
        if (full) { /* answered at once: no run */
            int i;
            for (i = 0; i < 12; i++)
                keys(&p, "/status\r");
            CHECK(wait_idle(&p, 3000));
        }
        pump(&p, 100);
        p.watch = true;
        p.row = p.v->r;
        keys(&p, "build and test\r");
        CHECK(wait_for(&p, "· 6 lines\n", 5000)); /* the running call, in the band */
        keys(&p, "and then?\r");
        CHECK(wait_for(&p, "queued · and then?", 2000));
        CHECK(wait_for(&p, "› and then?", 10000));
        CHECK(wait_idle(&p, 8000));
        CHECK(!p.rose);
        p.watch = false;
        keys(&p, "\x04");
        CHECK(finish(&p, 3000) == 0);
        pty_free(&p);
    }
}

/* Calls in a row form a group (section 16.2): a row for each round of
 * calls, those of one tool after its name once; a failure keeps its last
 * lines, an edit shows its diff. The open group, with the running calls
 * a row for each round, is in the band. */
static void test_tool_rendering(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/render.json", "--permission-mode", "auto", NULL};
    const char *again[] = {"-m", "faux/faux", "--script", "tests/faux/render.json", "-c", NULL};
    remove("build/test/render.c");
    pty_start(&p, 30, 160, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    keys(&p, "go\r");
    CHECK(wait_for(&p, /* the band: the commands run at once, and join their row as they end */
                   "✗ bash  echo 31, echo one; echo two; echo 'band.c:88: error'; echo 'make: Error 1'; exit 2 · exit 2\n",
                   3000));
    CHECK(wait_for(&p, /* the running call last, by the spinner, one row: how much it printed, not what */
                   " bash  echo started; sleep 1; echo finished · 1 line\n", 3000));
    CHECK(!has(&p, "        started"));
    CHECK(wait_for(&p, "Fixed.", 5000));
    CHECK(has(&p, "  Looking.\n\n"
                  "✗ bash  echo 31, echo one; echo two; echo 'band.c:88: error'; echo 'make: Error 1'; exit 2 · exit 2, "
                  "echo started; sleep 1; echo finished · 1s\n"
                  "        two\n"
                  "        band.c:88: error\n"
                  "        make: Error 1\n"
                  "✓ write build/test/render.c +2\n"
                  "✓ read  build/test/render.c · 2 lines ▪ edit build/test/render.c +1 -1\n"
                  "\n"
                  "  Fixed."));
    CHECK(!has(&p, "        one"));
    CHECK(!has(&p, "[exit code 2]"));
    CHECK(wait_idle(&p, 3000));
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    modes_restored(&p);
    pty_free(&p);
    /* resumed, the log gives the same group, once */
    pty_start(&p, 30, 160, again);
    CHECK(wait_for(&p, "▪ edit build/test/render.c +1 -1\n", 3000));
    CHECK(wait_for(&p, "faux · ", 3000));
    pump(&p, 300);
    CHECK(!has(&p, "Fixed.\n\n› go"));
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    pty_free(&p);
    remove("build/test/render.c");
}

/* A row shows a path under the session's directory from there; a call
 * after the join mark has one space after its name, not the column. */
static void test_short_paths(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "paths.json", "--permission-mode", "auto", NULL};
    char cwd[4096];
    FILE *f;
    CHECK(getcwd(cwd, sizeof cwd) != NULL);
    f = fopen("paths.json", "w");
    CHECK(f != NULL);
    if (!f)
        return;
    fprintf(f, "{\"turns\": [{\"content\": ["
               "{\"type\": \"tool_call\", \"name\": \"ls\", \"args\": {\"path\": \"%s\"}},"
               "{\"type\": \"tool_call\", \"name\": \"bash\", \"args\": {\"command\": \"true\"}},"
               "{\"type\": \"tool_call\", \"name\": \"ls\", \"args\": {\"path\": \"%s/tests\"}}"
               "]}, {\"content\": [{\"type\": \"text\", \"text\": \"Listed.\"}]}]}\n",
            cwd, cwd);
    fclose(f);
    pty_start(&p, 30, 100, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    keys(&p, "go\r");
    CHECK(wait_for(&p, "Listed.", 5000));
    /* one round, which ends in any order */
    CHECK(has(&p, "ls    .") || has(&p, "ls .") || has(&p, ", ."));
    CHECK(has(&p, "ls    tests") || has(&p, "ls tests") || has(&p, ", tests"));
    CHECK(has(&p, "▪ bash true") || has(&p, "▪ ls "));
    CHECK(!has(&p, "▪ ls  ") && !has(&p, "▪ bash  "));
    CHECK(!has(&p, cwd));
    CHECK(wait_idle(&p, 3000));
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    pty_free(&p);
    remove("paths.json");
}

/* A background command that ends while the agent is idle: pytte tells
 * the model in a note of its own, with what it printed, and the model
 * carries on without a poll. */
static void test_background_note(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/notify.json", "--permission-mode", "auto", NULL};
    char *log;
    size_t n;
    pty_start(&p, 30, 100, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    keys(&p, "go\r");
    CHECK(wait_for(&p, "It runs in the background.", 5000));
    CHECK(wait_for(&p, "It printed built.", 8000));
    CHECK(has(&p, "It runs in the background.\n\n"
                  "· worked for "));
    CHECK(has(&p, "· worked for 1s\n\n"
                  "· session 9 ended with exit code 0: sleep 2; echo built; echo '- not a session'\n\n"
                  "  It printed built."));
    CHECK(!has(&p, "[pytte]"));          /* the note is not shown as typed */
    CHECK(!has(&p, "· not a session")); /* nor its output */
    CHECK(wait_idle(&p, 3000));
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    pty_free(&p);
    /* the log: a user entry from pytte, with the output */
    log = last_log(&n);
    CHECK(log && strstr(log, "\"from\":\"pytte\"") != NULL);
    CHECK(log && strstr(log, "<output>\\nbuilt\\n- not a session\\n</output>") != NULL);
    free(log);
}

/* A command past its yield runs on in the background; polls name it by
 * session, and say whether it still runs. */
static void test_background(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/background.json", "--permission-mode", "auto", NULL};
    pty_start(&p, 30, 100, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    keys(&p, "go\r");
    CHECK(wait_for(&p, "· 1 in background (/ps)", 3000)); /* the footer */
    CHECK(wait_for(&p, "s · ↑ ", 3000) && has(&p, " ↓ ")); /* the run's tokens, beside its clock */
    CHECK(wait_for(&p, "Done.", 20000));
    /* a poll returns at once; the end is told in the next result */
    CHECK(has(&p, "Starting it.\n\n"
                  "✓ bash  echo first; sleep 2; echo second; exit 3 · still running as session 9\n"
                  "✓ poll  session 9 · still running\n"
                  "✓ bash  sleep 2.5"));
    CHECK(has(&p, "✗ poll  session 9 · exit 3\n"
                  "        second\n"
                  "✗ poll  session 9 · failed ▪ bash echo up; sleep 100 · still running as session 23\n"));
    /* one round, which ends in either order */
    CHECK(has(&p, "✓ poll  session 23 (stop) ▪ bash echo left; sleep 100 · still running as session 28\n") ||
          has(&p, "✓ bash  echo left; sleep 100 · still running as session 28 ▪ poll session 23 (stop)\n"));
    CHECK(!has(&p, "[still running"));
    CHECK(wait_idle(&p, 3000));
    /* one left running: the footer counts it, /ps lists it, /stop ends it */
    CHECK(wait_for(&p, "· 1 in background (/ps)", 2000));
    keys(&p, "/ps\r");
    CHECK(wait_for(&p, "session 28 · running ", 2000));
    CHECK(has(&p, "· echo left; sleep 100"));
    keys(&p, "/stop\r");
    CHECK(wait_for(&p, "stopping 1 background command\n", 2000));
    pump(&p, 300);
    CHECK(!has(&p, "in background (/ps)"));
    keys(&p, "/ps\r");
    CHECK(wait_for(&p, "session 28 · ended (exit 143), not read yet · echo left; sleep 100", 2000));
    keys(&p, "/stop\r");
    CHECK(wait_for(&p, "no background commands running", 2000));
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    pty_free(&p);
}

/* A tool call that may change things waits for approval: 1 runs it,
 * esc denies the next one. */
static void test_approval(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/tools.json", NULL};
    pty_start(&p, 24, 80, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    keys(&p, "go\r");
    CHECK(wait_for(&p, "  › 1  yes", 3000));
    CHECK(has(&p, "? bash  echo hi-from-bash; echo second-line"));
    CHECK(has(&p, "2  yes, and allow bash \"echo\" this session"));
    CHECK(has(&p, "esc no"));
    keys(&p, "1");
    CHECK(wait_for(&p, "✓ bash  echo hi-from-bash; echo second-line\n", 3000));
    CHECK(wait_for(&p, "? bash  touch should-not-exist", 3000));
    CHECK(wait_for(&p, "3  no (esc)", 3000));
    keys(&p, "\x1b[27u");
    CHECK(wait_for(&p, "✓ bash  echo hi-from-bash; echo second-line\n"
                       "✗ bash  touch should-not-exist · denied\n",
                   3000));
    CHECK(wait_for(&p, "All done with tools.", 3000));
    CHECK(wait_idle(&p, 3000));
    CHECK(access("should-not-exist", F_OK) != 0);
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    modes_restored(&p);
    pty_free(&p);
}

/* A small context window: the session compacts and goes on; /usage and
 * /compact. */
static void test_compaction(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/long.json", "--context-window", "500", NULL};
    pty_start(&p, 24, 100, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    keys(&p, "q1\r");
    CHECK(wait_for(&p, "One. Lorem", 3000));
    CHECK(wait_idle(&p, 3000));
    keys(&p, "/compact\r");
    CHECK(wait_for(&p, "compacted: the model continues from a summary (ctrl+o shows it)", 3000));
    CHECK(wait_idle(&p, 3000));
    keys(&p, "/compact\r");
    CHECK(wait_for(&p, "nothing to compact yet", 3000));
    keys(&p, "q2\r");
    CHECK(wait_for(&p, "Two. Duis", 3000));
    CHECK(wait_idle(&p, 3000));
    keys(&p, "q3\r");
    CHECK(wait_for(&p, "Three. Sed", 3000));
    CHECK(wait_idle(&p, 3000));
    keys(&p, "q4\r");
    CHECK(wait_for(&p, "Four. Nemo", 5000));
    CHECK(wait_idle(&p, 3000));
    keys(&p, "q5\r");
    CHECK(wait_for(&p, "Five. Neque", 5000));
    CHECK(wait_idle(&p, 3000));
    if (count(&p, "compacted: the model") != 2) /* the second time by itself */
        fprintf(stderr, "%s\n", p.scratch.p);
    CHECK(count(&p, "compacted: the model") == 2);
    keys(&p, "/usage\r");
    CHECK(wait_for(&p, "faux/faux: ", 3000));
    CHECK(has(&p, "requests"));
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    modes_restored(&p);
    pty_free(&p);
}

/* Skills: the $ menu, /skill: in the slash menu, a skill shown by name. */
static void test_skills(void)
{
    pty p;
    int i;
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/hello.json", "--context-window", "100000", NULL};
    char path[300];
    static const char skill[] = "---\nname: garden\ndescription: reads the soil\n---\nLook at the soil.\n";
    snprintf(path, sizeof path, "%s/pytte/skills/garden", config_dir);
    CHECK(mkdir(config_dir, 0700) == 0 || errno == EEXIST);
    snprintf(path, sizeof path, "%s/pytte", config_dir);
    CHECK(mkdir(path, 0700) == 0 || errno == EEXIST);
    snprintf(path, sizeof path, "%s/pytte/skills", config_dir);
    CHECK(mkdir(path, 0700) == 0 || errno == EEXIST);
    snprintf(path, sizeof path, "%s/pytte/skills/garden", config_dir);
    CHECK(mkdir(path, 0700) == 0 || errno == EEXIST);
    snprintf(path, sizeof path, "%s/pytte/skills/garden/SKILL.md", config_dir);
    CHECK(pt_sys_write_file(path, skill, sizeof skill - 1));
    pty_start(&p, 24, 80, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    /* before any answer, the context holds the system prompt, tools and skills */
    for (i = 0; i < 50 && (!has(&p, "/100k") || has(&p, " 0/100k")); i++)
        pump(&p, 20); /* 0 only until the session is open */
    CHECK(has(&p, "/100k") && !has(&p, " 0/100k"));
    keys(&p, "check $ga");
    CHECK(wait_for(&p, "  › $garden  reads the soil", 1000));
    keys(&p, "\r"); /* picks the skill; the message goes on */
    CHECK(wait_for(&p, "› check $garden \n", 1000));
    keys(&p, "now\r");
    CHECK(wait_for(&p, "› skill garden\n› check $garden now", 3000));
    CHECK(wait_for(&p, "Hello from the faux", 3000));
    CHECK(!has(&p, "Look at the soil")); /* the instructions are the model's */
    CHECK(wait_idle(&p, 3000));
    keys(&p, "/sk");
    CHECK(wait_for(&p, "  › /skill:garden  reads the soil", 1000));
    keys(&p, "\t");
    CHECK(wait_for(&p, "› /skill:garden \n", 1000));
    keys(&p, "go\r");
    CHECK(wait_for(&p, "› skill garden\n› go", 3000));
    CHECK(wait_idle(&p, 3000));
    keys(&p, "/skill:nope x\r");
    CHECK(wait_for(&p, "no skill named nope", 1000));
    keys(&p, "/settings skills off\r");
    CHECK(wait_for(&p, "skills: off", 1000));
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    pty_free(&p);
}

/* The slash menu, /model, /effort, /thinking (saved), /status, /help. */
static void test_commands(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/hello.json", NULL};
    char path[300];
    size_t n;
    char *cfg;
    pty_start(&p, 24, 80, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    CHECK(wait_for(&p, "faux · medium", 1000));
    keys(&p, "/");
    CHECK(wait_for(&p, "  › /model     switch the model", 1000)); /* descriptions in a column */
    CHECK(wait_for(&p, "enter run · esc close · 1/18", 1000)); /* the keys, and where you are */
    keys(&p, "\x1b[A"); /* up from the top wraps to the end, the window follows */
    CHECK(wait_for(&p, "  › /quit", 1000));
    CHECK(wait_for(&p, "18/18", 1000));
    keys(&p, "\x1b[B");
    CHECK(wait_for(&p, "  › /model", 1000));
    keys(&p, "th\t");
    CHECK(wait_for(&p, "› /thinking ", 1000));
    CHECK(wait_for(&p, "hidden", 1000));
    keys(&p, "full\r");
    CHECK(wait_for(&p, "\n· saved thinking: full\n", DURABLE_MS)); /* written by a worker */
    CHECK(!has(&p, "› /thinking")); /* a command is not echoed, only what it did */
    keys(&p, "/thinking some\r");
    CHECK(wait_for(&p, "not a value for thinking: some (hidden, full)", 1000));
    snprintf(path, sizeof path, "%s/pytte/config.json", config_dir);
    cfg = pt_sys_read_file(path, &n);
    CHECK(cfg && strstr(cfg, "\"tui\": {\n    \"thinking\": \"full\"\n  }"));
    free(cfg);
    keys(&p, "/effort high\r");
    CHECK(wait_for(&p, "switched effort: high", 1000));
    keys(&p, "/effort default\r");
    CHECK(wait_for(&p, "not a value for effort: default (off, minimal, low, medium, high, xhigh, max)", 1000));
    CHECK(has(&p, "faux · high"));
    keys(&p, "/settings effort default\r");
    CHECK(wait_for(&p, "not a value for effort: default (off,", 1000));
    keys(&p, "/settings reasoning max\r"); /* the config key names it too */
    CHECK(wait_for(&p, "saved effort: max", DURABLE_MS));
    CHECK(!has(&p, "saving")); /* one line: when it is saved */
    CHECK(wait_for(&p, "faux · max", 1000));
    keys(&p, "/settings\r\x1b[B\x1b[B\x1b[B\x1b[C");
    CHECK(wait_for(&p, "←→ or enter change · esc close", 1000));
    CHECK(wait_for(&p, "saved effort: off", DURABLE_MS)); /* max wraps straight to off */
    keys(&p, "\x1b[D");
    keys(&p, "\x1b");
    pump(&p, 100);
    CHECK(wait_for(&p, "faux · max", 1000)); /* off wraps straight to max */
    keys(&p, "/settings effort low\r");
    CHECK(wait_for(&p, "saved effort: low", DURABLE_MS));
    CHECK(wait_for(&p, "faux · low", 1000));
    keys(&p, "/effort high\r");
    CHECK(wait_for(&p, "faux · high", 1000));
    keys(&p, "/model faux/faux\x1b");
    pump(&p, 100);
    keys(&p, "\r");
    CHECK(wait_for(&p, "\n· switched model: faux/faux\n", 1000));
    CHECK(wait_for(&p, "faux · high", 1000));
    keys(&p, "/model nosuch/provider\r");
    CHECK(wait_for(&p, "nosuch/provider: ", 1000));
    /* a model is always named: the menu lists no bare providers */
    keys(&p, "/model anthropic");
    CHECK(wait_for(&p, "  › anthropic/claude-fable-5 ", 1000));
    CHECK(!has(&p, "anthropic  Anthropic"));
    keys(&p, "\x1b"); /* Esc closes the menu, so Enter sends what was typed */
    pump(&p, 100);
    keys(&p, "\r");
    CHECK(wait_for(&p, "anthropic: no model given", 1000));
    keys(&p, "/settings model anthropic\x1b");
    pump(&p, 100);
    keys(&p, "\r");
    CHECK(wait_for(&p, "\n\n✗ anthropic: no model given", 1000));
    CHECK(!has(&p, "saved model")); /* refused: nothing was saved */
    /* the default model switches this session too, as the default effort does */
    keys(&p, "/settings model anthropic/claude-fable-5\x1b");
    pump(&p, 100);
    keys(&p, "\r");
    CHECK(wait_for(&p, "saved model: anthropic/claude-fable-5", DURABLE_MS));
    CHECK(wait_for(&p, "claude-fable-5 · high", 1000));
    keys(&p, "/settings model faux/faux\x1b");
    pump(&p, 100);
    keys(&p, "\r");
    CHECK(wait_for(&p, "saved model: faux/faux", DURABLE_MS));
    CHECK(wait_for(&p, "faux · high", 1000));
    keys(&p, "/settings compact-model none\r");
    CHECK(wait_for(&p, "saved compact-model: none", DURABLE_MS));
    keys(&p, "/status\r");
    CHECK(wait_for(&p, "kitty keys on", 1000));
    keys(&p, "/help\r");
    CHECK(wait_for(&p, "esc esc interrupt", 1000));
    keys(&p, "/nope\r");
    CHECK(wait_for(&p, "unknown command /nope", 1000));
    /* /login: the key is masked, saved to auth.json, and kept out of the history */
    keys(&p, "/login ");
    CHECK(wait_for(&p, "sign in with a subscription (ChatGPT)", 1000)); /* api or oauth */
    keys(&p, "api anthr\t");
    CHECK(wait_for(&p, "› /login api anthropic ", 1000));
    keys(&p, "sk-test-123");
    CHECK(wait_for(&p, "› /login api anthropic ***********", 1000));
    CHECK(!has(&p, "sk-test"));
    keys(&p, "\r");
    CHECK(wait_for(&p, "saved the Anthropic key to ", DURABLE_MS));
    snprintf(path, sizeof path, "%s/pytte/auth.json", config_dir);
    cfg = pt_sys_read_file(path, &n);
    CHECK(cfg && strstr(cfg, "{\n  \"anthropic\": {\n    \"key\": \"sk-test-123\"\n  }\n}\n"));
    free(cfg);
    snprintf(path, sizeof path, "%s/pytte/history", state_dir);
    cfg = pt_sys_read_file(path, &n);
    CHECK(!cfg || !strstr(cfg, "sk-test"));
    free(cfg);
    keys(&p, "/login api\x1b");
    pump(&p, 100);
    keys(&p, "\r");
    CHECK(wait_for(&p, "anthropic: saved in auth.json", 1000));
    keys(&p, "/login oauth\x1b");
    pump(&p, 100);
    keys(&p, "\r");
    CHECK(wait_for(&p, "openai-codex: not logged in", 1000));
    /* thinking is shown in full now */
    keys(&p, "hi\r");
    CHECK(wait_for(&p, "A greeting. Answer briefly.", 5000));
    CHECK(wait_idle(&p, 5000));
    keys(&p, "/quit\r");
    CHECK(finish(&p, 3000) == 0);
    CHECK(has(&p, "A greeting. Answer briefly.\n  Hello from the faux provider!")); /* in the scrollback */
    modes_restored(&p);
    pty_free(&p);
    pty_start(&p, 24, 80, args);
    CHECK(wait_for(&p, "faux · low", 3000)); /* saved settings survive the session override */
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    modes_restored(&p);
    pty_free(&p);
}

/* The tools the first request of the session p shows had, as its log's
 * settled materials record them: whether tool is among them. */
static bool first_request_has(pty *p, const char *tool)
{
    char dir[4300], *log, *line, *end, want[160];
    const char *at;
    size_t n;
    bool found;
    keys(p, "/status\r");
    if (!wait_for(p, "\n  store /", 1000) || !(at = strstr(p->scratch.p, "\n  store /")))
        return false;
    sscanf(at + strlen("\n  store "), "%4095s", dir);
    strcat(dir, "/log.jsonl");
    if (!(log = pt_sys_read_file(dir, &n)))
        return false;
    snprintf(want, sizeof want, "\"name\":\"%s\"", tool);
    line = strstr(log, "\"t\":\"materials\"");
    end = line ? strchr(line, '\n') : NULL;
    if (end)
        *end = 0;
    found = line && strstr(line, want);
    CHECK(line != NULL); /* settled at the first request */
    free(log);
    return found;
}

/* /tools: a menu of the tools, on or off; web_fetch starts off, bash_poll
 * goes with bash. */
static void test_tools(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/hello.json", NULL};
    char path[300], *cfg;
    size_t n;
    snprintf(path, sizeof path, "%s/pytte/config.json", config_dir);
    unlink(path);
    pty_start(&p, 30, 100, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    keys(&p, "/tools\r");
    CHECK(wait_for(&p, "↑↓ select · space turns on or off · esc close · 7 of 8 on", 1000));
    CHECK(wait_for(&p, "● read ", 1000));
    CHECK(has(&p, "○ web_fetch"));
    CHECK(!has(&p, "bash_poll")); /* listed as bash */
    keys(&p, "\x1b[A "); /* up from the top: the last, web_fetch */
    CHECK(wait_for(&p, "\n· saved web_fetch: on\n", DURABLE_MS)); /* nothing sent yet: the first request gets it */
    CHECK(wait_for(&p, "8 of 8 on", 1000));
    CHECK(wait_for(&p, "● web_fetch", 1000));
    cfg = pt_sys_read_file(path, &n);
    CHECK(cfg && strstr(cfg, "\"tools\": {\n    \"web_fetch\": true\n  }"));
    free(cfg);
    keys(&p, "\x1b");
    pump(&p, 100);
    CHECK(!has(&p, "space turns on or off"));
    keys(&p, "/tools bash off\r"); /* the command, before the first prompt too */
    CHECK(wait_for(&p, "\n· saved bash: off\n", DURABLE_MS));
    keys(&p, "hi\r");
    CHECK(wait_for(&p, "Hello from the faux provider!", 5000));
    CHECK(wait_idle(&p, 5000));
    /* what the first request had: web_fetch, and no bash nor bash_poll */
    CHECK(first_request_has(&p, "web_fetch"));
    CHECK(first_request_has(&p, "read"));
    CHECK(!first_request_has(&p, "bash"));
    CHECK(!first_request_has(&p, "bash_poll"));
    keys(&p, "/tools bash on\r"); /* after it: from the next session */
    CHECK(wait_for(&p, "saved bash: on (the model gets it at the next session or /compact)", DURABLE_MS));
    keys(&p, "/tools web_fetch off\r");
    CHECK(wait_for(&p, "saved web_fetch: off (refused now; the model forgets it at the next session or /compact)",
                   DURABLE_MS));
    keys(&p, "/tools web_fetch\r");
    CHECK(wait_for(&p, "web_fetch: off (user)", 1000));
    keys(&p, "/tools read maybe\r");
    CHECK(wait_for(&p, "not a value for read: maybe (on, off)", 1000));
    keys(&p, "/tools nope on\r");
    CHECK(wait_for(&p, "no tool named nope; /tools lists them", 1000));
    keys(&p, "/tools bash_poll off\r");
    CHECK(wait_for(&p, "bash_poll goes with bash", 1000));
    keys(&p, "/tools we");
    CHECK(wait_for(&p, "  › web_fetch  off", 1000)); /* the state beside each */
    keys(&p, "\x15"); /* clear the draft */
    /* /settings shows how many are on, and opens the menu */
    keys(&p, "/settings\r\x1b[A");
    CHECK(wait_for(&p, "tools         7 of 8 on", 1000));
    CHECK(wait_for(&p, "enter open · esc close", 1000));
    keys(&p, "\r");
    CHECK(wait_for(&p, "space turns on or off", 1000));
    keys(&p, "\x1b"); /* alone: before ctrl-d, it would read as alt */
    pump(&p, 100);
    CHECK(!has(&p, "space turns on or off"));
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    modes_restored(&p);
    pty_free(&p);

    /* A session kept empty and resumed has sent nothing either: a tool
     * turned on there, even by a command typed while it opens, reaches its
     * first request. */
    pty_start(&p, 30, 100, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    keys(&p, "/quit\r");
    CHECK(finish(&p, 3000) == 0);
    pty_free(&p);
    {
        const char *again[] = {"-c", "--script", "tests/faux/hello.json", NULL};
        pty_start(&p, 30, 100, again);
    }
    CHECK(wait_for(&p, "/help for commands", 3000)); /* the terminal is raw: Enter is Enter */
    keys(&p, "/tools web_fetch on\r"); /* queued if the session still opens */
    CHECK(wait_for(&p, "\n· saved web_fetch: on\n", DURABLE_MS));
    keys(&p, "/settings\r\x1b[A\r"); /* and the menu from /settings */
    CHECK(wait_for(&p, "space turns on or off · esc close · 8 of 8 on", 1000));
    keys(&p, "\x1b");
    pump(&p, 100);
    keys(&p, "hi\r");
    CHECK(wait_for(&p, "Hello from the faux provider!", 5000));
    CHECK(wait_idle(&p, 5000));
    CHECK(first_request_has(&p, "web_fetch"));
    CHECK(first_request_has(&p, "bash"));
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    modes_restored(&p);
    pty_free(&p);
    unlink(path);
}

/* With no model chosen, sessions opened in the TUI get theirs as at start:
 * a new one the configured default, a resumed one its own. */
static void test_session_model(void)
{
    pty p;
    const char *args[] = {"--script", "tests/faux/hello.json", NULL};
    const char *again[] = {"-c", "--script", "tests/faux/hello.json", NULL};
    static const char fable[] = "{\"model\": \"anthropic/claude-fable-5\"}\n";
    static const char faux[] = "{\"model\": \"faux/faux\"}\n";
    char path[400], first[64] = "", cmd[100], *at;
    int fd;
    snprintf(path, sizeof path, "%s/pytte/config.json", config_dir);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    if (fd >= 0) {
        put(fd, faux, sizeof faux - 1);
        close(fd);
    }
    pty_start(&p, 24, 80, args);
    CHECK(wait_for(&p, "ask · faux · ", 3000)); /* the configured default */
    keys(&p, "hi\r");
    CHECK(wait_for(&p, "Hello from the faux provider!", 5000));
    CHECK(wait_idle(&p, 5000));
    keys(&p, "/status\r");
    CHECK(wait_for(&p, "· session ", 1000));
    if ((at = strstr(p.scratch.p, "· session ")))
        sscanf(at + strlen("· session "), "%63[^,]", first);
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    pty_free(&p);
    fd = open(path, O_WRONLY | O_TRUNC);
    CHECK(fd >= 0);
    if (fd >= 0) {
        put(fd, fable, sizeof fable - 1);
        close(fd);
    }
    pty_start(&p, 24, 80, again);
    CHECK(wait_for(&p, "ask · faux · ", 3000)); /* resumed: its own model */
    keys(&p, "/new\r");
    CHECK(wait_for(&p, "ask · claude-fable-5 · ", 3000)); /* new: the default */
    snprintf(cmd, sizeof cmd, "/resume %s\r", first);
    keys(&p, cmd);
    CHECK(wait_for(&p, "ask · faux · ", 3000)); /* its own again, not the new one's */
    keys(&p, "/new\r");
    CHECK(wait_for(&p, "ask · claude-fable-5 · ", 3000));
    keys(&p, "/model faux/faux\x1b");
    pump(&p, 100);
    keys(&p, "\r");
    CHECK(wait_for(&p, "switched model: faux/faux", 1000));
    keys(&p, "/new\r"); /* a chosen model goes on */
    CHECK(wait_for(&p, "ask · faux · ", 3000));
    CHECK(!has(&p, "switched model"));
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    modes_restored(&p);
    pty_free(&p);
    unlink(path);
}

/* The first prompt names a session; /fork continues a copy of it; /resume
 * finds sessions by name; /wt starts a session in a git worktree. */
static void test_fork_worktree(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/hello.json", NULL};
    char cwd[4096], path[4400], script[4200], first[64] = "", *at;
    const char *in_repo[] = {"-m", "faux/faux", "--script", script, NULL};
    const char *launch[] = {"-w", "launched", "-m", "faux/faux", "--script", script, NULL};
    size_t n;
    char *name;
    pty_start(&p, 30, 100, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    keys(&p, "/fork\r");
    CHECK(wait_for(&p, "nothing to fork yet", 1000));
    keys(&p, "  hi fork test\r");
    CHECK(wait_for(&p, "Hello from the faux provider!", 5000));
    CHECK(wait_idle(&p, 5000));
    keys(&p, "say more\r");
    CHECK(wait_idle(&p, 5000));
    keys(&p, "/status\r");
    CHECK(wait_for(&p, "\n· name hi fork test\n", 1000)); /* the first prompt's, trimmed */
    if ((at = strstr(p.scratch.p, "session 2")))
        sscanf(at + 8, "%63[^,]", first);
    snprintf(path, sizeof path, "%s/pytte/sessions/%s/name", state_dir, first);
    name = pt_sys_read_file(path, &n);
    CHECK(name && strcmp(name, "hi fork test\n") == 0);
    free(name);
    keys(&p, "/fork\r");
    snprintf(path, sizeof path, "forked from %s", first);
    CHECK(wait_for(&p, path, 3000));
    CHECK(wait_for(&p, "› hi fork test", 1000)); /* the conversation came along */
    keys(&p, "/status\r");
    CHECK(wait_for(&p, "· name hi fork test\n· directory ", 1000));
    keys(&p, "and in the fork\r");
    CHECK(wait_idle(&p, 5000));
    /* the original is found by its name, not only its id */
    keys(&p, "/resume FORK T");
    snprintf(path, sizeof path, "  › %s  hi fork test", first);
    CHECK(wait_for(&p, path, 3000));
    keys(&p, "\r");
    CHECK(wait_for(&p, "› say more", 3000));
    CHECK(!has(&p, "› and in the fork")); /* only in the copy */
    /* outside a repository, /wt says so and the session stays */
    keys(&p, "/wt\r");
    CHECK(wait_for(&p, "not in a git repository", 3000));
    keys(&p, "/wt ../x\r");
    CHECK(wait_for(&p, "not a worktree name: ../x", 1000));
    keys(&p, "/status\r");
    snprintf(path, sizeof path, "· session %s,", first);
    CHECK(wait_for(&p, path, 3000));
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    modes_restored(&p);
    pty_free(&p);

    /* in a repository: a worktree under .pytte/worktrees, on its own branch */
    CHECK(getcwd(cwd, sizeof cwd) != NULL);
    CHECK(system("mkdir repo && cd repo && git init -q && git -c user.name=t -c user.email=t@t "
                 "commit -q --allow-empty -m init") == 0);
    snprintf(script, sizeof script, "%s/tests/faux/hello.json", cwd); /* the session moves */
    CHECK(chdir("repo") == 0);
    pty_start(&p, 30, 100, in_repo);
    CHECK(wait_for(&p, "/help for commands", 3000));
    keys(&p, "/worktree feature\r");
    snprintf(path, sizeof path, "made worktree %s/repo/.pytte/worktrees/feature on branch worktree-feature", cwd);
    CHECK(wait_for(&p, path, 5000));
    keys(&p, "/status\r");
    snprintf(path, sizeof path, "· directory %s/repo/.pytte/worktrees/feature\n", cwd);
    CHECK(wait_for(&p, path, 1000));
    keys(&p, "hi fork test\r");
    CHECK(wait_idle(&p, 5000));
    keys(&p, "/new\r");
    pump(&p, 300);
    keys(&p, "/wt feature\r"); /* again: the same worktree */
    snprintf(path, sizeof path, "in worktree %s/repo/.pytte/worktrees/feature", cwd);
    CHECK(wait_for(&p, path, 5000));
    keys(&p, "/resume ");
    CHECK(wait_for(&p, "feature · hi fork test", 3000)); /* a worktree's session says where it is */
    keys(&p, "\x1b\x7f\x7f\x7f\x7f\x7f\x7f\x7f\x7f"); /* close the menu, empty the draft */
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    pty_free(&p);
    /* -w: start in one */
    pty_start(&p, 30, 100, launch);
    snprintf(path, sizeof path, "made worktree %s/repo/.pytte/worktrees/launched on branch worktree-launched", cwd);
    CHECK(wait_for(&p, path, 5000));
    keys(&p, "/new\r"); /* this open only: the next session is not another worktree */
    pump(&p, 300);
    keys(&p, "/status\r");
    snprintf(path, sizeof path, "· directory %s/repo/.pytte/worktrees/launched\n", cwd);
    CHECK(wait_for(&p, path, 1000));
    CHECK(!has(&p, "in worktree"));
    keys(&p, "\x04");
    CHECK(finish(&p, 3000) == 0);
    pty_free(&p);
    CHECK(chdir(cwd) == 0);
    CHECK(system("cd repo/.pytte/worktrees/feature && test \"$(git branch --show-current)\" = worktree-feature") == 0);
    CHECK(system("cd repo && test -z \"$(git status --porcelain)\"") == 0); /* the worktrees are ignored */
}

/* ↑ recalls history; Ctrl-O shows the full view in the pager; an unsent
 * draft survives quitting. */
static void test_history_pager_draft(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/hello.json", NULL};
    const char *again[] = {"-c", "--script", "tests/faux/hello.json", NULL};
    pty_start(&p, 24, 80, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    keys(&p, "first question\r");
    CHECK(wait_idle(&p, 5000));
    keys(&p, "\x1b[A");
    CHECK(wait_for(&p, "› first question", 1000));
    keys(&p, "\x03"); /* Ctrl-C clears it */
    keys(&p, "/settings pager cat\r");
    CHECK(wait_for(&p, "saved pager: cat", DURABLE_MS));
    keys(&p, "\x0f"); /* Ctrl-O: the thinking in full, through cat */
    CHECK(wait_for(&p, "A greeting. Answer briefly.", 2000));
    CHECK(wait_for(&p, "› ", 1000));
    keys(&p, "unsent words");
    pump(&p, 1300); /* saved at most once a second */
    kill(p.pid, SIGTERM);
    CHECK(finish(&p, 3000) == 128 + SIGTERM);
    modes_restored(&p);
    pty_free(&p);
    pty_start(&p, 24, 80, again);
    CHECK(wait_for(&p, "› unsent words", 3000));
    keys(&p, "\x03\x03");
    pump(&p, 100);
    keys(&p, "\x03");
    CHECK(finish(&p, 3000) == 0);
    modes_restored(&p);
    pty_free(&p);
}

static void test_sigterm(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/hello.json", NULL};
    pty_start(&p, 24, 80, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    keys(&p, "hello\r");
    CHECK(wait_for(&p, "Nothing here to", 5000));
    kill(p.pid, SIGTERM); /* mid-stream: what was shown stays */
    CHECK(finish(&p, 3000) == 128 + SIGTERM);
    CHECK(has(&p, "Nothing here to"));
    CHECK(has(&p, "(detached)"));
    modes_restored(&p);
    CHECK(!p.v->paste);
    pty_free(&p);
}

/* Missing/late query replies never gate input. A blocked setup preserves
 * both a submitted prompt and the draft when the user quits immediately. */
static void test_early_input(void)
{
    pty p;
    char fifo[400];
    const char *args[] = {"-m", "faux/faux", "--script", fifo, NULL};
    const char *again[] = {"-c", "--script", "tests/faux/hello.json", NULL};
    static const char script[] = "{\"turns\":[{\"content\":[{\"type\":\"text\",\"text\":\"hello\"}]}]}";
    char *log;
    size_t n;
    int fd;
    snprintf(fifo, sizeof fifo, "%s/open-script", state_dir);
    CHECK(mkfifo(fifo, 0600) == 0);
    pty_start(&p, 24, 80, args);
    p.v->answer = false;
    CHECK(wait_for(&p, "/help for commands", 3000));
    CHECK(p.v->kitty == 0);
    keys(&p, "early prompt\rremaining draft");
    CHECK(wait_for(&p, "› remaining draft", 1000));
    CHECK(has(&p, "  queued · early prompt")); /* sent while the session opens: shown waiting */
    /* A late CPR is dropped; late kitty negotiation still enables keys. */
    keys(&p, "\x1b[1;1R\x1b[?0u\x1b[?1;2c");
    CHECK(p.v->kitty == 1 && has(&p, "› remaining draft"));
    kill(p.pid, SIGTERM);
    fd = open(fifo, O_WRONLY);
    CHECK(fd >= 0);
    if (fd >= 0) {
        put(fd, script, sizeof script - 1);
        close(fd);
    }
    CHECK(finish(&p, 3000) == 128 + SIGTERM);
    modes_restored(&p);
    pty_free(&p);
    unlink(fifo);
    log = last_log(&n);
    CHECK(log && pt_str_contains((pt_str){log, n}, PT_S("early prompt")));
    CHECK(log && pt_str_contains((pt_str){log, n}, PT_S("\"event\":\"close\"")));
    free(log);
    pty_start(&p, 24, 80, again);
    CHECK(wait_for(&p, "› remaining draft", 3000));
    CHECK(wait_idle(&p, 5000));
    keys(&p, "\x03\x04");
    CHECK(finish(&p, 3000) == 0);
    modes_restored(&p);
    pty_free(&p);
}

static void test_blocked_settings(void)
{
    pty p;
    char path[400];
    const char *args[] = {"-m", "faux/faux", "--script", "tests/faux/hello.json", NULL};
    static const char original[] = "{\"untouched\":42,\"tui\":{\"thinking\":\"full\"}}\n";
    char *text;
    size_t n;
    int fd;
    pty_start(&p, 24, 80, args);
    CHECK(wait_for(&p, "/help for commands", 3000));
    /* Hold a settings read after startup, so it cannot hide behind setup. */
    snprintf(path, sizeof path, "%s/pytte/config.json", config_dir);
    unlink(path);
    CHECK(mkfifo(path, 0600) == 0);
    keys(&p, "/settings thinking hidden\rstill typing");
    CHECK(wait_for(&p, "› still typing", 1000));
    fd = open(path, O_WRONLY);
    CHECK(fd >= 0);
    if (fd >= 0) {
        put(fd, original, sizeof original - 1);
        close(fd);
    }
    CHECK(wait_for(&p, "saved thinking: hidden", DURABLE_MS));
    keys(&p, "\x03\x14\x14\x14\x04"); /* Rapid changes are flushed on quit. */
    CHECK(finish(&p, 3000) == 0);
    modes_restored(&p);
    pty_free(&p);
    text = pt_sys_read_file(path, &n);
    CHECK(text && pt_str_contains((pt_str){text, n}, PT_S("\"untouched\": 42")));
    CHECK(text && pt_str_contains((pt_str){text, n}, PT_S("\"thinking\": \"full\"")));
    free(text);
}

/* MCP in the TUI: a project's server waits for trust, /mcp trust starts
 * it, its prompt is a slash command, its resource an @-mention. The
 * server is test_mcp, next to this program. */
static void test_mcp(void)
{
    pty p;
    const char *args[] = {"-m", "faux/faux", "--script", "../tests/faux/hello.json", NULL};
    char server[4200], json[8600], *slash, *log;
    size_t n;
    snprintf(server, sizeof server, "%s", self_bin);
    if (!(slash = strrchr(server, '/')))
        return;
    strcpy(slash + 1, "test_mcp");
    if (access(server, X_OK) != 0) {
        fprintf(stderr, "    %s not built; skipped\n", server);
        return;
    }
    CHECK(mkdir("mcpproj", 0700) == 0 || errno == EEXIST);
    snprintf(json, sizeof json, "{\"mcpServers\": {\"t\": {\"command\": \"%s\", \"args\": [\"--mcp-server\", \"ok\"]}}}\n",
             server);
    CHECK(pt_sys_write_file("mcpproj/.mcp.json", json, strlen(json)));
    CHECK(chdir("mcpproj") == 0);
    pty_start(&p, 30, 100, args);
    CHECK(wait_for(&p, "not trusted, so they do not run: t (.mcp.json", 3000));
    keys(&p, "/mcp\r");
    CHECK(wait_for(&p, "t · not trusted (.mcp.json)", 2000));
    keys(&p, "/mcp trust\r");
    CHECK(wait_for(&p, "trusted this project's MCP servers: starting them", 2000));
    CHECK(wait_for(&p, "mcp t: ready: 7 tools", 5000));
    /* its prompt, from the menu */
    keys(&p, "/t:");
    CHECK(wait_for(&p, "/t:review  Review a file · file, focus?", 2000));
    keys(&p, "\r"); /* it needs a file: completed, not sent */
    CHECK(wait_for(&p, "› /t:review \n", 1000));
    keys(&p, "a.c\r");
    CHECK(wait_for(&p, "› Please review a.c", 3000));
    CHECK(wait_for(&p, "Hello from the faux", 3000));
    CHECK(wait_idle(&p, 3000));
    keys(&p, "/t:review\r"); /* completed again */
    CHECK(wait_for(&p, "› /t:review \n", 1000));
    keys(&p, "\x1b"); /* no menu: sent as it is */
    pump(&p, 100);
    keys(&p, "\r");
    CHECK(wait_for(&p, "/t:review needs file", 2000));
    /* a resource, mentioned */
    keys(&p, "look at @t:n");
    CHECK(wait_for(&p, "@t:note://one  one", 2000));
    keys(&p, "\r"); /* picked; the message goes on */
    CHECK(wait_for(&p, "› look at @t:note://one \n", 1000));
    keys(&p, "please\r");
    CHECK(wait_for(&p, "› look at @t:note://one please\n› resource t:note://one", 3000));
    CHECK(!has(&p, "contents of note://one")); /* the model's to read */
    CHECK(wait_idle(&p, 3000));
    keys(&p, "/mcp\r");
    /* the line holds test_mcp's path: where it wraps depends on the checkout */
    CHECK(wait_for_joined(&p, "t · ready · ", 2000));
    CHECK(wait_for_joined(&p, "--mcp-server ok · 7 tools, 1 prompts, 1 resources", 2000));
    keys(&p, "\x04");
    CHECK(finish(&p, 5000) == 0);
    modes_restored(&p);
    pty_free(&p);
    log = last_log(&n);
    CHECK(log && strstr(log, "<resource server=\\\"t\\\" uri=\\\"note://one\\\">\\ncontents of note://one"));
    free(log);
    CHECK(chdir("..") == 0);
}

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/pytte-pty-XXXXXX";
    char cmd[4200], repo[4096];
    static char bin_path[4096];
    const char *dir;
    if (argc == 2 && strcmp(argv[1], "--proc-fd-probe") == 0)
        return proc_fd_probe();
    if (!realpath(argv[0], self_bin))
        return 1;
    RUN(test_identity_zero_capacity);
    RUN(test_proc_cloexec);
    RUN(test_faux_delay);
    RUN(test_faux_cut);
    bin = getenv("PYTTE_BIN") ? getenv("PYTTE_BIN") : "build/pytte";
    if (access(bin, X_OK) != 0) {
        fprintf(stderr, "test_pty: %s not built; skipped\n", bin);
        return t_done("test_pty");
    }
    if (!(dir = mkdtemp(tmpl)) || !realpath(bin, bin_path) || !getcwd(repo, sizeof repo))
        return 1;
    /* Run from outside any repository, so this one's AGENTS.md and
     * CLAUDE.md stay out of the system prompt; tests/ is linked in. */
    bin = bin_path;
    snprintf(cmd, sizeof cmd, "%s/tests", repo);
    if (chdir(dir) != 0 || symlink(cmd, "tests") != 0)
        return 1;
    snprintf(state_dir, sizeof state_dir, "%s/state", dir);
    snprintf(config_dir, sizeof config_dir, "%s/config", dir);
    setenv("XDG_STATE_HOME", state_dir, 1);
    setenv("XDG_CONFIG_HOME", config_dir, 1);
    setenv("HOME", dir, 1); /* none of the user's ~/.agents skills */
    setenv("TERM", "xterm-256color", 1);
    setenv("LANG", "C.UTF-8", 1);
    unsetenv("LC_ALL");
    unsetenv("NO_COLOR");
    unsetenv("PYTTE_MODEL");
    unsetenv("PYTTE_EFFORT");
    unsetenv("XTERM_VERSION");
    RUN(test_chat);
    RUN(test_kill9);
    RUN(test_suspend_and_signals);
    RUN(test_interrupt_and_queue);
    RUN(test_steady_input);
    RUN(test_approval);
    RUN(test_tool_rendering);
    RUN(test_short_paths);
    RUN(test_background);
    RUN(test_background_note);
    RUN(test_compaction);
    RUN(test_resize_while_streaming);
    RUN(test_commands);
    RUN(test_skills);
    RUN(test_session_model);
    RUN(test_tools);
    RUN(test_fork_worktree);
    RUN(test_history_pager_draft);
    RUN(test_sigterm);
    RUN(test_early_input);
    RUN(test_blocked_settings);
    RUN(test_mcp);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (!t_failed && system(cmd) != 0)
        fprintf(stderr, "cannot remove %s\n", dir);
    return t_done("test_pty");
}
