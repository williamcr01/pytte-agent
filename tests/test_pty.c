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
} pty;

static char state_dir[256], config_dir[256];
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

/* Until no run is active (the activity line is gone). */
static bool wait_idle(pty *p, int ms)
{
    int64_t end = pt_sys_now_ms() + ms;
    pump(p, 50);
    while (has(p, "to interrupt")) {
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
    CHECK(wait_for(&p, "┃ hi there", 2000));
    keys(&p, "\r");
    CHECK(wait_for(&p, "Hello from the faux provider! Nothing here touched the network.", 5000));
    CHECK(wait_for(&p, "✻ thought", 2000));
    CHECK(wait_idle(&p, 3000));
    CHECK(wait_for(&p, "network.\n\nWorked for ", 1000)); /* the spinner's time, kept */
    CHECK(wait_for(&p, "\nfaux · ", 1000) && wait_for(&p, "/100k\n", 1000)); /* context used, of the window */
    keys(&p, "\x1b[100;5u"); /* Ctrl-D, kitty style */
    CHECK(finish(&p, 3000) == 0);
    CHECK(has(&p, "┃ hi there"));
    CHECK(has(&p, "\n\nto continue this session: pytte -r 20")); /* the session's id */
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
        CHECK(wait_for(&p, "┃ hi there\n", 3000));
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
    CHECK(wait_for(&p, "┃ list the files", 3000));
    CHECK(wait_for(&p, "(interrupted)", 3000));
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
    CHECK(wait_for(&p, "┃ draft text", 2000));
    keys(&p, "\x1b[122;5u"); /* Ctrl-Z */
    CHECK(wait_for(&p, "[stopped]", 2000));
    CHECK(p.v->kitty == 0 && !p.v->paste); /* restored while stopped */
    CHECK(wait_for(&p, "┃ draft text", 3000)); /* continued: drawn again */
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
    CHECK(wait_for(&p, "esc esc to interrupt", 3000));
    for (i = 0; i < 40; i++) {
        memset(&ws, 0, sizeof ws);
        ws.ws_row = 24;
        ws.ws_col = (unsigned short)(i % 2 ? 80 - (i * 3) % 30 : 40 + (i * 7) % 40);
        vt_resize(p.v, ws.ws_row, ws.ws_col);
        ioctl(p.fd, TIOCSWINSZ, &ws);
        pump(&p, 25 + (i * 13) % 30);
    }
    CHECK(wait_idle(&p, 10000));
    CHECK(count(&p, "esc esc to interrupt") == 0);
    CHECK(count(&p, "The user wants a listing") == 0); /* the live tail of the thinking */
    CHECK(count(&p, "┃ list the files") == 1);
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
    CHECK(wait_for(&p, "queued: and then?", 2000));
    CHECK(wait_for(&p, "No tools are", 5000));
    /* the call shows the gist of its arguments; its result is on a rail */
    CHECK(has(&p, "● bash ls\n│ This tool is not available"));
    keys(&p, "\x1b[27u");
    CHECK(wait_for(&p, "esc again to interrupt", 1000));
    keys(&p, "\x1b[27u");
    CHECK(wait_for(&p, "(interrupted)", 2000));
    /* the queued input goes next; the script's last turn answers it */
    CHECK(wait_for(&p, "┃ and then?", 3000));
    CHECK(wait_for(&p, "and resume.\n", 8000));
    CHECK(wait_idle(&p, 3000));
    CHECK(!has(&p, "queued:"));
    keys(&p, "\x1b[100;5u");
    CHECK(finish(&p, 3000) == 0);
    modes_restored(&p);
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
    keys(&p, "/");
    CHECK(wait_for(&p, "▸ /model     set the model", 1000)); /* descriptions in a column */
    CHECK(wait_for(&p, "enter run · esc close · 1/9", 1000)); /* the keys, and where you are */
    keys(&p, "\x1b[A"); /* up from the top wraps to the end, the window follows */
    CHECK(wait_for(&p, "▸ /quit", 1000));
    CHECK(wait_for(&p, "9/9", 1000));
    keys(&p, "\x1b[B");
    CHECK(wait_for(&p, "▸ /model", 1000));
    keys(&p, "th\t");
    CHECK(wait_for(&p, "┃ /thinking ", 1000));
    CHECK(wait_for(&p, "collapsed", 1000));
    keys(&p, "full\r");
    CHECK(wait_for(&p, "\nswitched thinking: full\n", 1000));
    CHECK(!has(&p, "┃ /thinking")); /* a command is not echoed, only what it did */
    snprintf(path, sizeof path, "%s/pytte/config.json", config_dir);
    cfg = pt_sys_read_file(path, &n);
    CHECK(cfg && strstr(cfg, "\"tui\": {\"thinking\": \"full\"}"));
    free(cfg);
    keys(&p, "/effort high\r");
    CHECK(wait_for(&p, "switched effort: high", 1000));
    keys(&p, "/model faux/faux\x1b");
    pump(&p, 100);
    keys(&p, "\r");
    CHECK(wait_for(&p, "\nswitched model: faux/faux\n", 1000));
    CHECK(wait_for(&p, "faux · high", 1000));
    keys(&p, "/model nosuch/provider\r");
    CHECK(wait_for(&p, "nosuch/provider: ", 1000));
    /* a model is always named: the menu lists no bare providers */
    keys(&p, "/model anthropic");
    CHECK(wait_for(&p, "▸ anthropic/claude-fable-5 ", 1000));
    CHECK(!has(&p, "anthropic  Anthropic"));
    keys(&p, "\x1b"); /* Esc closes the menu, so Enter sends what was typed */
    pump(&p, 100);
    keys(&p, "\r");
    CHECK(wait_for(&p, "anthropic: no model given", 1000));
    keys(&p, "/settings model anthropic\x1b");
    pump(&p, 100);
    keys(&p, "\r");
    CHECK(wait_for(&p, "\n\nanthropic: no model given", 1000));
    CHECK(!has(&p, "saved model")); /* refused: nothing was saved */
    keys(&p, "/status\r");
    CHECK(wait_for(&p, "kitty keys on", 1000));
    keys(&p, "/help\r");
    CHECK(wait_for(&p, "esc esc interrupt", 1000));
    keys(&p, "/nope\r");
    CHECK(wait_for(&p, "unknown command /nope", 1000));
    /* thinking is shown in full now */
    keys(&p, "hi\r");
    CHECK(wait_for(&p, "A greeting. Answer briefly.", 5000));
    CHECK(wait_idle(&p, 5000));
    keys(&p, "/quit\r");
    CHECK(finish(&p, 3000) == 0);
    CHECK(has(&p, "A greeting. Answer briefly.\nHello from the faux provider!")); /* in the scrollback */
    modes_restored(&p);
    pty_free(&p);
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
    CHECK(wait_for(&p, "┃ first question", 1000));
    keys(&p, "\x03"); /* Ctrl-C clears it */
    keys(&p, "/settings pager cat\r");
    CHECK(wait_for(&p, "saved pager: cat", 1000));
    keys(&p, "\x0f"); /* Ctrl-O: the thinking in full, through cat */
    CHECK(wait_for(&p, "A greeting. Answer briefly.", 2000));
    CHECK(wait_for(&p, "┃ ", 1000));
    keys(&p, "unsent words");
    pump(&p, 1300); /* saved at most once a second */
    kill(p.pid, SIGTERM);
    CHECK(finish(&p, 3000) == 128 + SIGTERM);
    modes_restored(&p);
    pty_free(&p);
    pty_start(&p, 24, 80, again);
    CHECK(wait_for(&p, "┃ unsent words", 3000));
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
    CHECK(wait_for(&p, "┃ remaining draft", 1000));
    /* A late CPR is dropped; late kitty negotiation still enables keys. */
    keys(&p, "\x1b[1;1R\x1b[?0u\x1b[?1;2c");
    CHECK(p.v->kitty == 1 && has(&p, "┃ remaining draft"));
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
    CHECK(wait_for(&p, "┃ remaining draft", 3000));
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
    CHECK(wait_for(&p, "┃ still typing", 1000));
    fd = open(path, O_WRONLY);
    CHECK(fd >= 0);
    if (fd >= 0) {
        put(fd, original, sizeof original - 1);
        close(fd);
    }
    CHECK(wait_for(&p, "saved thinking: hidden", 3000));
    keys(&p, "\x03\x14\x14\x04"); /* Rapid changes are flushed on quit. */
    CHECK(finish(&p, 3000) == 0);
    modes_restored(&p);
    pty_free(&p);
    text = pt_sys_read_file(path, &n);
    CHECK(text && pt_str_contains((pt_str){text, n}, PT_S("\"untouched\":42")));
    CHECK(text && pt_str_contains((pt_str){text, n}, PT_S("\"thinking\":\"full\"")));
    free(text);
}

int main(void)
{
    char tmpl[] = "/tmp/pytte-pty-XXXXXX";
    char cmd[300];
    const char *dir;
    bin = getenv("PYTTE_BIN") ? getenv("PYTTE_BIN") : "build/pytte";
    if (access(bin, X_OK) != 0) {
        fprintf(stderr, "test_pty: %s not built; skipped\n", bin);
        return 0;
    }
    if (!(dir = mkdtemp(tmpl)))
        return 1;
    snprintf(state_dir, sizeof state_dir, "%s/state", dir);
    snprintf(config_dir, sizeof config_dir, "%s/config", dir);
    setenv("XDG_STATE_HOME", state_dir, 1);
    setenv("XDG_CONFIG_HOME", config_dir, 1);
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
    RUN(test_resize_while_streaming);
    RUN(test_commands);
    RUN(test_history_pager_draft);
    RUN(test_sigterm);
    RUN(test_early_input);
    RUN(test_blocked_settings);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (!t_failed && system(cmd) != 0)
        fprintf(stderr, "cannot remove %s\n", dir);
    return t_done("test_pty");
}
