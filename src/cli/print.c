/* Print mode (pytte -p, and pytte approve): run until idle, the answer on
 * stdout, tool calls, thinking and narration on stderr; or the result as
 * JSON, or every commit as a JSON line. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cli/main.h"

/* Where a message's text goes. stdout gets only answers: when it is not
 * a terminal, a message's text waits until the message is known to call
 * no tools (an answer, for stdout) or to call some (narration, for
 * stderr). */
enum { MSG_HOLD, MSG_OUT, MSG_ERR };

typedef struct ui {
    int out;
    bool tty_err, tty_out;
    bool text_nl, any_text, thinking_out;
    FILE *text_to; /* where the last text went */
    int msg;       /* MSG_*: the streaming message's text */
    int out_errno; /* stdout failed: EPIPE when its reader left */
    int runs_failed, runs_done;
    pt_buf held;          /* the message's text while MSG_HOLD */
    pt_buf result, error; /* for --output-format json */
} ui;

static volatile sig_atomic_t interrupted;

static void on_signal(int sig)
{
    (void)sig;
    interrupted = 1;
}

static bool should_stop(void *ud)
{
    (void)ud;
    return interrupted != 0;
}

/* The first stdout failure stops the run: nobody gets the answer. */
static void out_flush(ui *u)
{
    if ((fflush(stdout) != 0 || ferror(stdout)) && !u->out_errno) {
        u->out_errno = errno ? errno : EIO;
        interrupted = 1;
    }
}

static void end_text(ui *u)
{
    if (u->any_text && !u->text_nl)
        fputc('\n', u->text_to);
    u->any_text = false;
    u->text_nl = true;
    if (u->text_to == stdout)
        out_flush(u);
}

static void put_text(ui *u, FILE *f, pt_str s)
{
    if (!s.n)
        return;
    if (u->any_text && u->text_to != f)
        end_text(u);
    fwrite(s.p, 1, s.n, f);
    u->text_to = f;
    u->text_nl = s.p[s.n - 1] == '\n';
    u->any_text = true;
    if (f == stdout)
        out_flush(u);
    else
        fflush(f);
}

/* The held text goes to f, and so does the rest of the message. */
static void release(ui *u, FILE *f)
{
    put_text(u, f, pt_buf_str(&u->held));
    pt_buf_reset(&u->held);
    u->msg = f == stdout ? MSG_OUT : MSG_ERR;
}

static void on_stream(ui *u, const pt_ai_event *ev)
{
    const pt_msg *m = ev->msg;
    switch (ev->kind) {
    case PT_EV_START:
        u->msg = u->tty_out ? MSG_OUT : MSG_HOLD;
        break;
    case PT_EV_THINKING_DELTA:
        if (u->any_text)
            end_text(u);
        if (!u->thinking_out && u->tty_err)
            fputs("\x1b[2m", stderr);
        u->thinking_out = true;
        fwrite(ev->delta.p, 1, ev->delta.n, stderr);
        break;
    case PT_EV_THINKING_END:
        if (u->thinking_out)
            fputs(u->tty_err ? "\x1b[0m\n" : "\n", stderr);
        u->thinking_out = false;
        break;
    case PT_EV_TEXT_DELTA:
        if (u->msg == MSG_HOLD) {
            pt_buf_puts(&u->held, ev->delta);
            if (!u->held.oom)
                break;
            /* Out of memory: the text goes out now, as if it were the answer.
             * The buffer keeps what it had; a fresh one forgets the failure. */
            release(u, stdout);
            pt_buf_free(&u->held);
            pt_buf_init(&u->held, pt_sys_allocator());
        }
        put_text(u, u->msg == MSG_ERR ? stderr : stdout, ev->delta);
        break;
    case PT_EV_TOOLCALL_START:
        if (u->msg == MSG_HOLD)
            release(u, stderr);
        break;
    case PT_EV_TOOLCALL_END: {
        const pt_block *b = &m->blocks[ev->index];
        end_text(u);
        fprintf(stderr, "%s-> %.*s %.*s%s\n", u->tty_err ? "\x1b[36m" : "", (int)b->name.n, b->name.p,
                (int)b->args.n, b->args.p, u->tty_err ? "\x1b[0m" : "");
        break;
    }
    case PT_EV_DONE:
    case PT_EV_ERROR:
        if (u->thinking_out)
            fputs(u->tty_err ? "\x1b[0m\n" : "\n", stderr);
        u->thinking_out = false;
        /* A cut-off message is no answer: a retry or the error follows. */
        if (u->msg == MSG_HOLD)
            release(u, ev->kind == PT_EV_DONE ? stdout : stderr);
        end_text(u);
        break;
    default:
        break;
    }
}

static void on_event(void *ud, const pt_event *ev)
{
    ui *u = (ui *)ud;
    if (ev->kind == PT_E_RUN_DONE) {
        bool ok = pt_str_eqz(ev->outcome, "completed");
        ok ? u->runs_done++ : u->runs_failed++;
        pt_buf_reset(&u->error);
        if (!ok)
            pt_buf_puts(&u->error, ev->error);
    }
    if (u->out == OUT_JSON) {
        if (ev->kind == PT_E_STREAM && ev->ai->kind == PT_EV_START)
            pt_buf_reset(&u->result);
        if (ev->kind == PT_E_STREAM && ev->ai->kind == PT_EV_TEXT_DELTA)
            pt_buf_puts(&u->result, ev->ai->delta);
        return;
    }
    if (u->out == OUT_STREAM) {
        if (ev->kind == PT_E_COMMITTED) {
            fwrite(ev->line.p, 1, ev->line.n, stdout);
            fputc('\n', stdout);
            out_flush(u);
        }
        return;
    }
    switch (ev->kind) {
    case PT_E_STREAM:
        on_stream(u, ev->ai);
        break;
    case PT_E_RETRY: {
        int64_t wait = ev->until - pt_sys_now_ms();
        fprintf(stderr, "%sretrying in %lld.%llds: %.*s%s\n", u->tty_err ? "\x1b[33m" : "",
                (long long)(wait > 0 ? wait / 1000 : 0), (long long)(wait > 0 ? wait % 1000 / 100 : 0),
                (int)ev->error.n, ev->error.p, u->tty_err ? "\x1b[0m" : "");
        break;
    }
    case PT_E_RUN_DONE:
        if (!pt_str_eqz(ev->outcome, "completed"))
            fprintf(stderr, "%serror:%s %.*s\n", u->tty_err ? "\x1b[31m" : "", u->tty_err ? "\x1b[0m" : "",
                    (int)ev->error.n, ev->error.p);
        break;
    default:
        break;
    }
}

static void print_json(ui *u, const char *id, const char *status)
{
    pt_buf b;
    pt_jw w;
    pt_buf_init(&b, pt_sys_allocator());
    pt_jw_init(&w, &b);
    pt_jw_obj(&w);
    pt_jw_key(&w, "session");
    pt_jw_strz(&w, id);
    pt_jw_key(&w, "status");
    pt_jw_strz(&w, status);
    pt_jw_key(&w, "result");
    pt_jw_str(&w, pt_buf_str(&u->result));
    if (u->error.n) {
        pt_jw_key(&w, "error");
        pt_jw_str(&w, pt_buf_str(&u->error));
    }
    pt_jw_obj_end(&w);
    pt_buf_putc(&b, '\n');
    fwrite(b.p, 1, b.n, stdout);
    out_flush(u);
    pt_buf_free(&b);
}

#define STDIN_MAX ((size_t)64 << 20)

/* All of stdin into b. False, with why in err, when it cannot be read
 * whole: a prompt is never sent cut short. Says so on stderr when stdin
 * stays silent, so an idle pipe is not a mystery hang. */
static bool read_stdin(pt_buf *b, char *err, size_t cap)
{
    struct stat st;
    struct pollfd pfd;
    if (fstat(STDIN_FILENO, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
        (uint64_t)st.st_size <= STDIN_MAX)
        pt_buf_reserve(b, (size_t)st.st_size + 1);
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 3000) == 0)
        fputs("pytte: waiting for stdin (--no-stdin runs without it)\n", stderr);
    for (;;) {
        ssize_t got;
        if (b->n == b->cap && !pt_buf_reserve(b, 65536)) {
            snprintf(err, cap, "out of memory reading stdin");
            return false;
        }
        got = read(STDIN_FILENO, b->p + b->n, b->cap - b->n);
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            snprintf(err, cap, "cannot read stdin: %s", strerror(errno));
            return false;
        }
        if (got == 0)
            return true;
        b->n += (size_t)got;
        if (b->n > STDIN_MAX) {
            snprintf(err, cap, "stdin is over %u MiB", (unsigned)(STDIN_MAX >> 20));
            return false;
        }
    }
}

/* Tool calls waiting for approval, with the command that answers each. */
static void print_asks(cli_session *s)
{
    pt_ask_view v[8];
    pt_arena ar;
    uint32_t n = pt_asks(s->a, v, 8), i;
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    for (i = 0; i < n && i < 8; i++) {
        pt_str gist = pt_rt_gist(v[i].args, &ar);
        fprintf(stderr, "pytte: %.*s wants approval: %.*s\n", (int)v[i].name.n, v[i].name.p,
                (int)(gist.n > 300 ? 300 : gist.n), gist.p);
        fprintf(stderr, "  answer with: pytte approve %s %u yes|no\n", s->id, v[i].task);
    }
    pt_arena_free(&ar);
}

int pytte_print(opts *o, const char *id, cli_config *cfg)
{
    cli_session s;
    ui u;
    pt_buf in_buf;
    pt_str prompt = PT_STR_NULL, data = PT_STR_NULL;
    char err[700];
    const char *status = "completed";
    int rc;
    memset(&u, 0, sizeof u);
    u.out = o->out;
    u.tty_err = isatty(STDERR_FILENO);
    u.tty_out = isatty(STDOUT_FILENO);
    u.text_nl = true;
    pt_buf_init(&u.held, pt_sys_allocator());
    pt_buf_init(&u.result, pt_sys_allocator());
    pt_buf_init(&u.error, pt_sys_allocator());
    pt_buf_init(&in_buf, pt_sys_allocator());
    /* Piped stdin is the prompt, or data that goes with it. */
    if (!o->cmd_approve && !o->no_stdin && !isatty(STDIN_FILENO)) {
        if (!read_stdin(&in_buf, err, sizeof err))
            die(err, NULL);
        data = pt_str_trim(pt_buf_str(&in_buf));
    }
    if (o->prompt)
        prompt = pt_strz(o->prompt);
    else {
        prompt = data;
        data = PT_STR_NULL;
    }
    if (!id && !prompt.n && !o->cmd_approve)
        die("no prompt (give one as an argument or on stdin)", NULL);
    apply_config(o, cfg, id == NULL);
    effort_note(o->c.model, o->c.reasoning);
    o->c.stop = should_stop;
    load_mcp(o, false);
    if ((rc = cli_session_open(&s, &o->c, id, on_event, &u, err, sizeof err)) != 0) {
        fprintf(stderr, "pytte: %s\n", err);
        return rc;
    }
    if (o->cmd_approve) {
        uint64_t task = 0;
        if (!pt_str_to_u64(pt_strz(o->task), &task) || task > UINT32_MAX ||
            !pt_approve(s.a, (uint32_t)task, strcmp(o->answer, "yes") == 0, PT_STR_NULL)) {
            fprintf(stderr, "pytte: task %s: %s\n", o->task, task ? pt_error(s.a) : "not a task number");
            cli_session_close(&s);
            return 1;
        }
    }
    if (prompt.n) {
        pt_input in;
        pt_buf text, joined;
        char in_dir[4200];
        pt_buf_init(&text, pt_sys_allocator());
        pt_buf_init(&joined, pt_sys_allocator());
        memset(&in, 0, sizeof in);
        in.text = prompt;
        switch (pt_rt_expand_skills(s.rt, prompt, &text, err, sizeof err)) {
        case -1:
            fprintf(stderr, "pytte: %s\n", err);
            snprintf(in_dir, sizeof in_dir, "%s", s.dir);
            cli_session_close(&s);
            if (!id) /* new, and nothing in it */
                cli_session_discard(in_dir);
            return 1;
        case 1:
            in.text = pt_buf_str(&text);
            break;
        }
        if (data.n) { /* the data first, then what to do with it */
            pt_buf_reserve(&joined, data.n + in.text.n + 32);
            pt_buf_putz(&joined, "<stdin>\n");
            pt_buf_puts(&joined, data);
            pt_buf_putz(&joined, "\n</stdin>\n\n");
            pt_buf_puts(&joined, in.text);
            in.text = pt_buf_str(&joined);
        }
        if (joined.oom || !pt_submit(s.a, &in)) {
            char dir[4200];
            fprintf(stderr, "pytte: %s\n", joined.oom ? "out of memory" : pt_error(s.a));
            snprintf(dir, sizeof dir, "%s", s.dir);
            cli_session_close(&s);
            if (!id)
                cli_session_discard(dir); /* discard a newly-created empty session */
            pt_buf_free(&text);
            pt_buf_free(&joined);
            return 1;
        }
        pt_buf_free(&text);
        pt_buf_free(&joined);
        cli_session_name_set(s.dir, prompt);
    }
    if (o->out == OUT_TEXT)
        fprintf(stderr, "%ssession %s%s\n", u.tty_err ? "\x1b[2m" : "", s.id, u.tty_err ? "\x1b[0m" : "");
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    { /* the terminal or ssh connection went away; unless nohup (or trap '' HUP) said to run on */
        struct sigaction hup;
        if (sigaction(SIGHUP, NULL, &hup) != 0 || hup.sa_handler != SIG_IGN)
            signal(SIGHUP, on_signal);
    }
    signal(SIGPIPE, SIG_IGN); /* a closed stdout is an EPIPE out_flush sees */
    switch (pt_rt_run(s.rt)) {
    case PT_RT_IDLE:
        end_text(&u);
        cli_session_close(&s);
        rc = u.runs_failed ? 10 : 0;
        status = u.runs_failed ? "failed" : "completed";
        break;
    case PT_RT_STOPPED:
        end_text(&u);
        fprintf(stderr, "pytte: detached; continue with: pytte -r %s\n", s.id);
        cli_session_close(&s);
        rc = 130;
        status = "detached";
        break;
    case PT_RT_FATAL:
        end_text(&u);
        fprintf(stderr, "pytte: %s\n", pt_error(s.a));
        pt_buf_reset(&u.error);
        pt_buf_putz(&u.error, pt_error(s.a));
        if (pt_fatal(s.a) == PT_FATAL_CONFIG) {
            cli_session_close(&s); /* nothing is broken: a clean exit */
            rc = 1;
        } else {
            cli_session_free(&s);
            rc = 20;
        }
        status = "error";
        fprintf(stderr, "pytte: the session is safe up to its last commit; continue with: pytte -r %s\n", s.id);
        break;
    case PT_RT_ASK:
        end_text(&u);
        print_asks(&s);
        cli_session_close(&s);
        rc = 3;
        status = "waiting";
        break;
    default: /* STUCK; BUSY is not returned by pt_rt_run */
        fprintf(stderr, "pytte: internal error: the agent is stuck\n");
        cli_session_free(&s);
        rc = 20;
        status = "error";
        break;
    }
    if (o->out == OUT_JSON)
        print_json(&u, s.id, status);
    if (u.out_errno && (rc == 0 || rc == 130)) {
        /* An EPIPE is quiet, as a SIGPIPE would be; other failures are not. */
        if (u.out_errno != EPIPE)
            fprintf(stderr, "pytte: cannot write to stdout: %s\n", strerror(u.out_errno));
        rc = u.out_errno == EPIPE ? 141 : 10;
    }
    pt_buf_free(&u.held);
    pt_buf_free(&u.result);
    pt_buf_free(&u.error);
    pt_buf_free(&in_buf);
    return rc;
}
