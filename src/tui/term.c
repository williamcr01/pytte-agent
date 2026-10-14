/* The terminal (section 16.2.10): raw mode, the startup queries, signals
 * through a self-pipe, and restore on every exit path a process can see.
 * kill -9 is not one: after it the shell gets the terminal as we left it.
 *
 * The terminal is never set O_NONBLOCK (R8): its open file description is
 * shared with the shell. Reads happen after poll() said there is input. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE /* TIOCGWINSZ */
#include "tui/tui.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#include "drv/drv.h"

struct tui_term {
    int in, out;
    bool raw, paste, kitty, gone;
    bool kitty_was; /* pushed before a suspend */
    struct termios saved;
    int sig[2]; /* the self-pipe */
    int trace;  /* PYTTE_TUI_TRACE, or -1 */
    pt_buf tb;
};

/* For the signal handlers, which can only touch these. */
static int g_sigw = -1, g_out = -1, g_in = -1;
static struct sigaction g_old_pipe[sizeof((int[]){SIGWINCH, SIGTSTP, SIGCONT, SIGINT, SIGTERM, SIGQUIT, SIGHUP}) / sizeof(int)];
static struct sigaction g_old_fatal[sizeof((int[]){SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE}) / sizeof(int)];
static struct sigaction g_old_sigpipe;
static bool g_handlers_saved;
static volatile sig_atomic_t g_raw;
static struct termios g_saved;
static char g_restore[64];
static volatile sig_atomic_t g_restore_n;

static const char restore_modes[] = "\x1b[?2004l\x1b[?2026l\x1b[?25h\x1b[0m";

static void set_restore(bool kitty)
{
    size_t n = 0;
    g_restore_n = 0;
    if (kitty) {
        memcpy(g_restore, "\x1b[<u", 4);
        n = 4;
    }
    memcpy(g_restore + n, restore_modes, sizeof restore_modes - 1);
    g_restore_n = (sig_atomic_t)(n + sizeof restore_modes - 1);
}

static void put_all(int fd, const char *p, size_t n)
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

/* ---- signals ---- */

static void on_signal(int sig)
{
    int e = errno;
    unsigned char c = (unsigned char)sig;
    if (g_sigw >= 0)
        (void)!write(g_sigw, &c, 1);
    errno = e;
}

static void on_fatal(int sig)
{
    if (g_out >= 0 && g_raw) {
        (void)!write(g_out, g_restore, (size_t)g_restore_n);
        if (g_in >= 0)
            tcsetattr(g_in, TCSANOW, &g_saved);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void handle(int sig, void (*fn)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(sig, &sa, NULL);
}

static const int fatal_sigs[] = {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE};
static const int pipe_sigs[] = {SIGWINCH, SIGTSTP, SIGCONT, SIGINT, SIGTERM, SIGQUIT, SIGHUP};

static void trace(tui_term *t, const char *what, pt_str s)
{
    if (t->trace < 0)
        return;
    pt_buf_reset(&t->tb);
    pt_buf_putz(&t->tb, "{\"t\":");
    pt_buf_i64(&t->tb, pt_sys_now_ms());
    pt_buf_putz(&t->tb, ",\"");
    pt_buf_putz(&t->tb, what);
    pt_buf_putz(&t->tb, "\":");
    pt_json_quote(&t->tb, s);
    pt_buf_putz(&t->tb, "}\n");
    put_all(t->trace, t->tb.p, t->tb.n);
}

tui_term *tui_term_new(void)
{
    tui_term *t;
    const char *tf = getenv("PYTTE_TUI_TRACE");
    size_t i;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return NULL;
    if (!(t = (tui_term *)calloc(1, sizeof *t)))
        return NULL;
    t->in = STDIN_FILENO;
    t->out = STDOUT_FILENO;
    t->trace = -1;
    t->sig[0] = t->sig[1] = -1;
    pt_buf_init(&t->tb, pt_sys_allocator());
    if (pipe(t->sig) == 0) {
        for (i = 0; i < 2; i++) {
            fcntl(t->sig[i], F_SETFD, FD_CLOEXEC);
            fcntl(t->sig[i], F_SETFL, O_NONBLOCK); /* our own pipe, not the terminal */
        }
        g_sigw = t->sig[1];
    }
    if (tf && *tf)
        t->trace = open(tf, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    g_out = t->out;
    g_in = t->in;
    sigaction(SIGPIPE, NULL, &g_old_sigpipe);
    for (i = 0; i < sizeof pipe_sigs / sizeof pipe_sigs[0]; i++) {
        sigaction(pipe_sigs[i], NULL, &g_old_pipe[i]);
        handle(pipe_sigs[i], on_signal);
    }
    for (i = 0; i < sizeof fatal_sigs / sizeof fatal_sigs[0]; i++) {
        sigaction(fatal_sigs[i], NULL, &g_old_fatal[i]);
        handle(fatal_sigs[i], on_fatal);
    }
    g_handlers_saved = true;
    signal(SIGPIPE, SIG_IGN);
    set_restore(false);
    return t;
}

static bool raw_on(tui_term *t)
{
    struct termios r = t->saved;
    r.c_lflag &= ~(tcflag_t)(ECHO | ICANON | ISIG | IEXTEN);
    r.c_iflag &= ~(tcflag_t)(IXON | ICRNL | INLCR | IGNCR | ISTRIP | BRKINT | INPCK);
    r.c_cflag |= CS8;
    r.c_cc[VMIN] = 1;
    r.c_cc[VTIME] = 0;
    /* OPOST stays on: a stray \n from anywhere cannot staircase the screen */
    if (tcsetattr(t->in, TCSANOW, &r) != 0)
        return false;
    t->raw = true;
    g_raw = 1;
    return true;
}

bool tui_term_start(tui_term *t, bool kitty)
{
    if (tcgetattr(t->in, &t->saved) != 0)
        return false;
    g_saved = t->saved;
    if (!raw_on(t))
        return false;
    tui_term_write(t, kitty ? PT_S("\x1b[6n\x1b[?u\x1b[c\x1b[?2004h") : PT_S("\x1b[6n\x1b[c\x1b[?2004h"));
    t->paste = true;
    return true;
}

void tui_term_kitty(tui_term *t, bool push)
{
    if (push == t->kitty)
        return;
    tui_term_write(t, push ? PT_S("\x1b[>1u") : PT_S("\x1b[<u"));
    t->kitty = push;
    set_restore(push);
}

int tui_term_in(const tui_term *t)
{
    return t->in;
}

int tui_term_sigfd(const tui_term *t)
{
    return t->sig[0];
}

int tui_term_signals(tui_term *t, int *last)
{
    unsigned char buf[64];
    int bits = 0;
    ssize_t n, i;
    while ((n = read(t->sig[0], buf, sizeof buf)) > 0)
        for (i = 0; i < n; i++) {
            int b = buf[i] == SIGWINCH  ? TS_WINCH
                    : buf[i] == SIGTSTP ? TS_TSTP
                    : buf[i] == SIGCONT ? TS_CONT
                    : buf[i] == SIGHUP  ? TS_HUP
                                        : TS_QUIT;
            bits |= b;
            if (b == TS_QUIT && last)
                *last = buf[i];
        }
    return bits;
}

long tui_term_read(tui_term *t, char *buf, size_t cap)
{
    ssize_t n = read(t->in, buf, cap);
    if (n < 0)
        return errno == EINTR || errno == EAGAIN ? -1 : 0;
    if (n > 0)
        trace(t, "r", (pt_str){buf, (size_t)n});
    return (long)n;
}

int tui_term_write(tui_term *t, pt_str s)
{
    int64_t t0;
    const char *p = s.p;
    size_t n = s.n;
    if (t->gone || !n)
        return 0;
    trace(t, "w", s);
    t0 = pt_sys_now_ms();
    while (n) {
        ssize_t w = write(t->out, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && errno == EAGAIN) { /* someone else set O_NONBLOCK */
            struct pollfd pf;
            pf.fd = t->out;
            pf.events = POLLOUT;
            poll(&pf, 1, 100);
            continue;
        }
        if (w <= 0) {
            t->gone = true;
            break;
        }
        p += w;
        n -= (size_t)w;
    }
    return (int)(pt_sys_now_ms() - t0);
}

void tui_term_size(tui_term *t, int *rows, int *cols)
{
    struct winsize ws;
    *rows = 24;
    *cols = 80;
    if (ioctl(t->out, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    }
    if (t->trace >= 0) {
        char b[48];
        snprintf(b, sizeof b, "%d %d", *rows, *cols);
        trace(t, "size", pt_strz(b));
    }
}

void tui_term_gone(tui_term *t)
{
    t->gone = true;
    g_raw = 0;
}

void tui_term_suspend(tui_term *t)
{
    t->kitty_was = t->kitty;
    tui_term_kitty(t, false);
    tui_term_write(t, pt_strz(restore_modes));
    t->paste = false;
    if (t->raw && !t->gone)
        tcsetattr(t->in, TCSADRAIN, &t->saved);
    t->raw = false;
    g_raw = 0;
}

void tui_term_resume(tui_term *t)
{
    if (t->gone)
        return;
    raw_on(t);
    tui_term_write(t, PT_S("\x1b[?2004h"));
    t->paste = true;
    tui_term_kitty(t, t->kitty_was);
}

void tui_term_stop_self(void)
{
    signal(SIGTSTP, SIG_DFL);
    raise(SIGTSTP);
    handle(SIGTSTP, on_signal); /* continued */
}

void tui_term_free(tui_term *t)
{
    size_t i;
    if (!t)
        return;
    if (t->raw)
        tui_term_suspend(t);
    g_sigw = -1;
    g_out = -1;
    g_in = -1;
    if (g_handlers_saved) {
        sigaction(SIGPIPE, &g_old_sigpipe, NULL);
        for (i = 0; i < sizeof pipe_sigs / sizeof pipe_sigs[0]; i++)
            sigaction(pipe_sigs[i], &g_old_pipe[i], NULL);
        for (i = 0; i < sizeof fatal_sigs / sizeof fatal_sigs[0]; i++)
            sigaction(fatal_sigs[i], &g_old_fatal[i], NULL);
        g_handlers_saved = false;
    }
    for (i = 0; i < 2; i++)
        if (t->sig[i] >= 0)
            close(t->sig[i]);
    if (t->trace >= 0)
        close(t->trace);
    pt_buf_free(&t->tb);
    free(t);
}
