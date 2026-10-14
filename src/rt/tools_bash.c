/* The bash and bash_poll tools (section 13.3), and the processes they
 * leave running.
 *
 * A bash call that runs past its yield time ends with the output so far,
 * and its process goes on in the background, named by the call's task ID
 * (unique in the session, so a name from before a restart never finds a
 * new process). When it ends, the model is told: in the next tool result
 * while it works, else in a note that starts a turn, so it need not poll.
 * bash_poll reads what it printed since, at once, or stops it.
 * Background processes die with this process; when it dies first (kill
 * -9), the next process to open the session stops them. */
#define _POSIX_C_SOURCE 200809L
#include "rt/rt_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define YIELD_S 120       /* seconds a bash call waits before it goes to the background */
#define HELD_MAX (1 << 20) /* output kept while no call reads it: the newest bytes */
#define NOTE_TAIL 8192    /* output an idle note carries per session: the newest bytes */
#define BACKGROUND_MAX 16 /* processes in the background; past it, bash calls wait */

typedef struct job job;

struct bash {
    rt_tool_kit *k; /* borrowed: the builtins' */
    job *jobs;
};

struct job {
    job *next;
    bash *b;
    uint32_t fx;      /* the call reading it; 0: in the background */
    uint32_t session; /* the bash call's task: its name in the background */
    uint32_t timer;   /* the timeout, then the SIGKILL after a stop */
    uint32_t yield;   /* when the reading call ends while it runs on */
    pt_proc *p;
    char *command;
    int out_fd, exit_fd;
    int64_t started, read_at; /* read_at: when the reading call began */
    uint32_t timeout_s;
    int status;
    bool timed_out, stopped, eof, ended;
    bool background; /* it went to the background once */
    bool noted;      /* its end is told, or need not be */
    bool noting;     /* in the idle note being written */
    bool polled;  /* the reading call is a bash_poll */
    bool told;    /* a bash_poll asked it to stop: not an error */
    pt_buf held;  /* output while in the background */
    uint64_t dropped; /* bytes cut from the front of held */
};

static job *find_job(bash *b, uint32_t fx)
{
    job *j;
    for (j = b->jobs; j && j->fx != fx; j = j->next)
        ;
    return j;
}

static uint32_t background_count(const bash *b)
{
    const job *j;
    uint32_t n = 0;
    for (j = b->jobs; j; j = j->next)
        n += !j->fx;
    return n;
}

/* Output goes to the reading call, else into held: its newest HELD_MAX
 * bytes, trimmed once it holds twice that (amortized O(n)). */
static void emit(job *j, const char *p, size_t n)
{
    pt_buf *h = &j->held;
    if (j->fx) {
        pt_rt_tool_output(j->b->k->rt, j->fx, p, n);
        return;
    }
    if (n >= HELD_MAX) {
        j->dropped += h->n + n - HELD_MAX;
        pt_buf_reset(h);
        pt_buf_put(h, p + n - HELD_MAX, HELD_MAX);
        return;
    }
    if (h->n + n > 2 * HELD_MAX) {
        size_t k = HELD_MAX - n;
        j->dropped += h->n - k;
        memmove(h->p, h->p + h->n - k, k);
        h->n = k;
    }
    pt_buf_put(h, p, n);
}

static void unwatch(job *j)
{
    pt_rt *rt = j->b->k->rt;
    if (j->out_fd >= 0)
        pt_rt_unwatch(rt, j->out_fd);
    if (j->exit_fd >= 0)
        pt_rt_unwatch(rt, j->exit_fd);
    j->out_fd = j->exit_fd = -1;
    if (j->timer)
        pt_rt_timer_cancel(rt, j->timer);
    j->timer = 0;
}

static void job_free(job *j)
{
    job **pp;
    int64_t group = pt_proc_group(j->p);
    for (pp = &j->b->jobs; *pp != j; pp = &(*pp)->next)
        ;
    *pp = j->next;
    unwatch(j);
    if (j->yield)
        pt_rt_timer_cancel(j->b->k->rt, j->yield);
    pt_proc_free(j->p); /* kills what still runs */
    pt_rt_proc_ended(j->b->k->rt, group);
    pt_buf_free(&j->held);
    free(j->command);
    free(j);
}

/* The reading call ends: with how the process ended, which frees it, or
 * with where it runs on, which leaves it in the background. */
static void job_report(job *j)
{
    pt_rt *rt = j->b->k->rt;
    uint32_t fx = j->fx;
    char note[200], details[160];
    size_t k = 0;
    pt_rt_out r;
    int64_t ms = pt_rt_now(rt) - (j->polled ? j->read_at : j->started);
    if (ms < 0)
        ms = 0;
    if (j->yield)
        pt_rt_timer_cancel(rt, j->yield);
    j->yield = 0;
    note[0] = 0;
    memset(&r, 0, sizeof r);
    if (!j->ended) {
        j->background = true;
        k += (size_t)snprintf(note, sizeof note, "\n[still running as session %u; you will be told when it ends]",
                              j->session);
        snprintf(details, sizeof details, "{\"session\":%u,\"ms\":%lld}", j->session, (long long)ms);
    } else {
        if (j->timed_out)
            k += (size_t)snprintf(note, sizeof note, "\n[timed out after %us]", j->timeout_s);
        else if (j->stopped)
            k += (size_t)snprintf(note, sizeof note, "\n[stopped]");
        else if (j->status)
            k += (size_t)snprintf(note, sizeof note, "\n[exit code %d]", j->status);
        else if (j->polled)
            k += (size_t)snprintf(note, sizeof note, "\n[ended]");
        snprintf(details, sizeof details, "{\"exit\":%d,\"ms\":%lld}", j->status, (long long)ms);
        r.is_error = j->timed_out || (j->status != 0 && !j->told);
    }
    r.note = (pt_str){note, k};
    r.details = pt_strz(details);
    j->fx = 0;
    j->polled = false;
    if (j->ended)
        job_free(j);
    pt_rt_tool_done(rt, fx, &r);
}

/* The process ended (and its output with it). */
static void job_end(job *j)
{
    pt_proc_reap(j->p, true, &j->status);
    unwatch(j);
    j->ended = true;
    if (j->fx)
        job_report(j);
}

/* Output waits: read what is there (bounded, so keys stay responsive). */
static void job_drain(job *j)
{
    bash *b = j->b;
    size_t total = 0;
    while (total < 4 * RT_TOOL_IO) {
        long got = pt_proc_read(j->p, b->k->io, RT_TOOL_IO);
        if (got < 0)
            return;
        if (got == 0) {
            j->eof = true;
            if (j->out_fd >= 0)
                pt_rt_unwatch(b->k->rt, j->out_fd);
            j->out_fd = -1;
            return;
        }
        emit(j, b->k->io, (size_t)got);
        total += (size_t)got;
    }
}

static void on_output(void *ud, pt_rt *rt)
{
    job *j = (job *)ud;
    int status;
    (void)rt;
    job_drain(j);
    /* Without a pidfd, the end of the output is the end of the call. */
    if (j->eof && (j->exit_fd < 0 || pt_proc_reap(j->p, false, &status)))
        job_end(j);
}

/* The process ended: take what output is left and end the call, even if
 * a background child still holds the pipe. */
static void on_exit_fd(void *ud, pt_rt *rt)
{
    job *j = (job *)ud;
    int status;
    (void)rt;
    if (!pt_proc_reap(j->p, false, &status))
        return;
    pt_rt_unwatch(j->b->k->rt, j->exit_fd);
    j->exit_fd = -1;
    while (!j->eof) {
        long got = pt_proc_read(j->p, j->b->k->io, RT_TOOL_IO);
        if (got <= 0)
            break;
        emit(j, j->b->k->io, (size_t)got);
    }
    job_end(j);
}

static void on_kill(void *ud, pt_rt *rt)
{
    job *j = (job *)ud;
    (void)rt;
    j->timer = 0;
    pt_proc_stop(j->p, true);
}

/* SIGTERM to the group, and SIGKILL two seconds later. */
static void stop_job(job *j)
{
    pt_rt *rt = j->b->k->rt;
    pt_proc_stop(j->p, false);
    if (j->timer)
        pt_rt_timer_cancel(rt, j->timer);
    j->timer = pt_rt_timer(rt, pt_rt_now(rt) + 2000, on_kill, j);
    if (!j->timer)
        pt_proc_stop(j->p, true); /* Preserve the kill guarantee on timer OOM. */
}

static void on_timeout(void *ud, pt_rt *rt)
{
    job *j = (job *)ud;
    (void)rt;
    j->timer = 0;
    j->timed_out = true;
    stop_job(j);
}

/* The reading call has waited long enough: it ends, the process runs on. */
static void on_yield(void *ud, pt_rt *rt)
{
    job *j = (job *)ud;
    (void)rt;
    j->yield = 0;
    if (j->fx && !j->ended)
        job_report(j);
}

static const char *shell(void)
{
    static const char *const ok[] = {"/bash", "/zsh", "/sh", "/dash", "/ksh"};
    const char *sh = getenv("SHELL");
    size_t i, n;
    if (sh && *sh == '/')
        for (i = 0; i < sizeof ok / sizeof *ok; i++) {
            n = strlen(ok[i]);
            if (strlen(sh) >= n && strcmp(sh + strlen(sh) - n, ok[i]) == 0)
                return sh;
        }
    return "/bin/sh";
}

static void t_bash(void *ud, pt_rt *rt, const pt_fx *fx)
{
    bash *b = (bash *)ud;
    const pt_json *args = pt_rt_kit_args(b->k, fx);
    const char *cmd = pt_rt_kit_str(b->k, args, "command"), *argv[4], *add[4];
    int64_t timeout = pt_json_geti(args, "timeout", 0), yield = pt_json_geti(args, "yield", YIELD_S);
    pt_str cwd = pt_session_cwd(pt_rt_agent(rt));
    char err[300], call_id[200], attempt[40];
    job *j;
    if (!cmd) {
        pt_rt_kit_done(rt, fx->id, PT_S("command is required"), true);
        return;
    }
    if (!(j = (job *)calloc(1, sizeof *j)) || !(j->command = (char *)malloc(strlen(cmd) + 1))) {
        free(j);
        pt_rt_kit_done(rt, fx->id, PT_S("out of memory"), true);
        return;
    }
    memcpy(j->command, cmd, strlen(cmd) + 1);
    argv[0] = shell();
    argv[1] = "-c";
    argv[2] = cmd;
    argv[3] = NULL;
    snprintf(call_id, sizeof call_id, "PYTTE_CALL_ID=%.*s", (int)(fx->call_id.n > 150 ? 150 : fx->call_id.n),
             fx->call_id.p);
    snprintf(attempt, sizeof attempt, "PYTTE_ATTEMPT=%u", fx->attempt);
    add[0] = call_id;
    add[1] = attempt;
    add[2] = "PYTTE_TOOL=bash";
    add[3] = NULL;
    j->p = pt_proc_spawn(argv, cwd.n ? pt_arena_cstr(&b->k->ar, cwd) : NULL, b->k->unset, add, NULL, err, sizeof err);
    if (!j->p) {
        free(j->command);
        free(j);
        pt_rt_kit_done(rt, fx->id, pt_strz(err), true);
        return;
    }
    pt_rt_proc_started(rt, j->p);
    j->b = b;
    j->fx = fx->id;
    j->session = fx->task;
    j->started = j->read_at = pt_rt_now(rt);
    j->out_fd = pt_proc_out_fd(j->p);
    j->exit_fd = pt_proc_exit_fd(j->p);
    pt_buf_init(&j->held, pt_sys_allocator());
    j->next = b->jobs;
    b->jobs = j;
    if (!pt_rt_watch(rt, j->out_fd, on_output, j) ||
        (j->exit_fd >= 0 && !pt_rt_watch(rt, j->exit_fd, on_exit_fd, j))) {
        j->stopped = true;
        pt_proc_stop(j->p, true);
        job_end(j);
        return;
    }
    if (timeout > 0) {
        j->timeout_s = timeout > 86400 ? 86400 : (uint32_t)timeout;
        j->timer = pt_rt_timer(rt, j->started + (int64_t)j->timeout_s * 1000, on_timeout, j);
        if (!j->timer) {
            j->timed_out = true;
            j->stopped = true;
            pt_proc_stop(j->p, true);
            job_end(j);
            return;
        }
    }
    if (yield > 0 && background_count(b) < BACKGROUND_MAX)
        j->yield = pt_rt_timer(rt, j->started + (yield > 86400 ? 86400 : yield) * 1000, on_yield, j);
}

/* Abort: a bash call stops its process; a bash_poll only stops reading,
 * and the process runs on in the background. */
static void bash_cancel(void *ud, pt_rt *rt, uint32_t fx)
{
    bash *b = (bash *)ud;
    job *j = find_job(b, fx);
    if (!j)
        return;
    if (j->polled) {
        job_report(j);
        return;
    }
    j->stopped = true;
    if (rt->closing) { /* exiting: no time for a grace period */
        pt_proc_stop(j->p, true);
        job_end(j);
    } else {
        stop_job(j);
    }
}

static void t_bash_poll(void *ud, pt_rt *rt, const pt_fx *fx)
{
    bash *b = (bash *)ud;
    const pt_json *args = pt_rt_kit_args(b->k, fx);
    int64_t session = pt_json_geti(args, "session", 0);
    bool stop = pt_json_truthy(pt_json_get(args, "stop"));
    char msg[200];
    size_t k = 0; /* held bytes before the first whole line */
    job *j;
    for (j = b->jobs; j && (int64_t)j->session != session; j = j->next)
        ;
    if (!j) {
        snprintf(msg, sizeof msg,
                 "No session %lld: it ended and was read already, or pytte restarted since.", (long long)session);
        pt_rt_kit_done(rt, fx->id, pt_strz(msg), true);
        return;
    }
    if (j->fx) {
        snprintf(msg, sizeof msg, "Another call is reading session %lld.", (long long)session);
        pt_rt_kit_done(rt, fx->id, pt_strz(msg), true);
        return;
    }
    j->fx = fx->id;
    j->polled = true;
    j->read_at = pt_rt_now(rt);
    if (j->dropped) { /* from the first whole line */
        const char *nl = (const char *)memchr(j->held.p, '\n', j->held.n);
        k = nl ? (size_t)(nl + 1 - j->held.p) : 0;
        snprintf(msg, sizeof msg, "[%llu earlier bytes dropped]\n", (unsigned long long)(j->dropped + k));
        pt_rt_tool_output(rt, fx->id, msg, strlen(msg));
        j->dropped = 0;
    }
    if (j->held.n > k)
        pt_rt_tool_output(rt, fx->id, j->held.p + k, j->held.n - k);
    pt_buf_reset(&j->held);
    if (stop && !j->ended) { /* ends within two seconds */
        j->stopped = j->told = true;
        stop_job(j);
    } else {
        job_report(j);
    }
}

/* "session N ended with exit code S: command" */
static void put_ended(pt_buf *t, const job *j)
{
    char line[400];
    snprintf(line, sizeof line, "session %u ended with %s %d: %.300s%s", j->session,
             j->timed_out ? "a timeout, exit code" : "exit code", j->status, j->command,
             strlen(j->command) > 300 ? "…" : "");
    pt_buf_putz(t, line);
}

static bool unread_end(const job *j)
{
    return j->background && j->ended && !j->noted && !j->fx;
}

/* What an idle note shows of held: all of it, or its newest NOTE_TAIL
 * bytes from a whole line (else a whole character). cut: some is left
 * out, so the session stays for bash_poll to read whole. */
static pt_str note_tail(const job *j, bool *cut)
{
    pt_str s = pt_buf_str(&j->held);
    const char *nl;
    *cut = j->dropped || s.n > NOTE_TAIL;
    if (!*cut)
        return s;
    if (s.n > NOTE_TAIL) {
        s.p += s.n - NOTE_TAIL;
        s.n = NOTE_TAIL;
    }
    if ((nl = (const char *)memchr(s.p, '\n', s.n)) != NULL && nl + 1 < s.p + s.n) {
        s.n -= (size_t)(nl + 1 - s.p);
        s.p = nl + 1;
    }
    while (s.n && ((unsigned char)*s.p & 0xC0) == 0x80) {
        s.p++;
        s.n--;
    }
    return s;
}

/* Idle, with background commands ended and unread: submit a note that
 * starts a turn, with what they printed, so the model carries on without
 * the user asking or a poll. It is an input like any other, committed
 * before it runs, so a crash cannot lose or repeat it. A session shown
 * whole is read and gone; one cut short stays for bash_poll. */
bool pt_rt_bash_idle(bash *b)
{
    pt_buf *t = &b->k->out;
    pt_input in;
    char line[200], req[48];
    job *j, *next;
    uint32_t n = 0, first = 0;
    bool ok, cut;
    pt_buf_reset(t);
    pt_buf_putz(t, "[pytte] Background commands you started have ended (the user did not write this "
                   "message):\n");
    for (j = b->jobs; j; j = j->next) {
        if (!unread_end(j))
            continue;
        j->noted = j->noting = true;
        if (!n++ || j->session < first)
            first = j->session;
        pt_buf_putz(t, "- ");
        put_ended(t, j);
        pt_buf_putc(t, '\n');
    }
    if (!n)
        return false;
    for (j = b->jobs; j; j = j->next) {
        pt_str tail;
        if (!j->noting)
            continue;
        tail = note_tail(j, &cut);
        if (!tail.n && !cut) {
            snprintf(line, sizeof line, "\nSession %u printed nothing.\n", j->session);
            pt_buf_putz(t, line);
            continue;
        }
        if (cut)
            snprintf(line, sizeof line,
                     "\nSession %u printed (its last %zu bytes; bash_poll reads what came before):\n<output>\n",
                     j->session, tail.n);
        else
            snprintf(line, sizeof line, "\nSession %u printed:\n<output>\n", j->session);
        pt_buf_putz(t, line);
        pt_buf_puts(t, tail);
        if (tail.n && tail.p[tail.n - 1] != '\n')
            pt_buf_putc(t, '\n');
        pt_buf_putz(t, "</output>\n");
    }
    pt_buf_putz(t, "\nCarry on with what they were for. If nothing is left to do, say so in a sentence.");
    snprintf(req, sizeof req, "background:%u", first);
    memset(&in, 0, sizeof in);
    in.text = pt_buf_str(t);
    in.req = pt_strz(req);
    in.note = true;
    ok = !t->oom && pt_submit(pt_rt_agent(b->k->rt), &in);
    for (j = b->jobs; j; j = next) {
        next = j->next;
        if (!j->noting)
            continue;
        j->noting = false;
        note_tail(j, &cut);
        if (ok && !cut)
            job_free(j);
    }
    return ok;
}

void pt_rt_bash_ended(bash *b, pt_buf *out)
{
    job *j;
    for (j = b->jobs; j; j = j->next) {
        if (!unread_end(j))
            continue;
        j->noted = true;
        pt_buf_putz(out, "\n[pytte: background ");
        put_ended(out, j);
        pt_buf_putz(out, "; bash_poll reads its output]");
    }
}

bool pt_rt_bash_running(const bash *b)
{
    const job *j;
    for (j = b->jobs; j; j = j->next)
        if (j->background && !j->ended)
            return true;
    return false;
}

uint32_t pt_rt_bash_backgrounds(const bash *b, pt_rt_background *out, uint32_t max)
{
    const job *j;
    uint32_t n = 0;
    for (j = b->jobs; j; j = j->next) {
        if (!j->background)
            continue;
        if (n < max) {
            out[n].session = j->session;
            out[n].command = j->command;
            out[n].started = j->started;
            out[n].ended = j->ended;
            out[n].status = j->status;
        }
        n++;
    }
    return n;
}

uint32_t pt_rt_bash_stop_all(bash *b)
{
    job *j;
    uint32_t n = 0;
    for (j = b->jobs; j; j = j->next)
        if (j->background && !j->ended && !j->stopped) {
            j->stopped = j->told = j->noted = true; /* the user stopped it: no note */
            stop_job(j);
            n++;
        }
    return n;
}

/* ---- the tools ---- */

static const char bash_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"The command line\"},"
    "\"timeout\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Stop it after this many seconds (default: never)\"},"
    "\"yield\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"Return after this many seconds if it still runs, "
    "leaving it in the background (default 120; 0: wait for it to end)\"}},"
    "\"required\":[\"command\"],\"additionalProperties\":false}";
static const char bash_poll_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"session\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"The session a bash call left running\"},"
    "\"stop\":{\"type\":\"boolean\",\"description\":\"Stop it (SIGTERM, then SIGKILL after 2s)\"}},"
    "\"required\":[\"session\"],\"additionalProperties\":false}";

bash *pt_rt_bash_new(rt_tool_kit *k)
{
    bash *b = (bash *)calloc(1, sizeof *b);
    if (b)
        b->k = k;
    return b;
}

void pt_rt_bash_free(bash *b)
{
    if (!b)
        return;
    while (b->jobs) { /* a reading call gets its result; the background just goes */
        job *j = b->jobs;
        j->stopped = true;
        pt_proc_stop(j->p, true);
        if (j->fx)
            job_end(j);
        else
            job_free(j);
    }
    free(b);
}

void pt_rt_bash_tools(bash *b, pt_rt_tool t[2])
{
    memset(t, 0, 2 * sizeof *t);
    t[0].def.source = t[1].def.source = PT_S("builtin");
    t[0].ud = t[1].ud = b;
    t[0].def.name = PT_S("bash");
    t[0].def.description = PT_S(
        "Run a command line in the user's shell, in the session's directory, and return its output "
        "(stdout and stderr together). Commands start in the working directory. A cd or export inside "
        "one call doesn't carry over to the next. stdin is empty, so nothing can prompt. A command still running after yield seconds keeps running "
        "in the background as a session; pass a small yield to start a server or a watcher. You are "
        "told when a session ends, with what it printed, so do not poll to wait for one: work on, "
        "or end your turn saying what you wait for, and you will be woken when it ends.");
    t[0].def.schema = PT_S(bash_schema);
    t[0].def.parallel = true;
    t[0].run = t_bash;
    t[0].cancel = bash_cancel;
    t[1].def.name = PT_S("bash_poll");
    t[1].def.description = PT_S(
        "Read what a background bash session printed since it was last read, at once, and whether "
        "it still runs; or stop it. Use it to check on progress or to read output the end notice "
        "left out. It does not wait: you are told when a session ends.");
    t[1].def.schema = PT_S(bash_poll_schema);
    t[1].def.replay = PT_REPLAY_SAFE;
    t[1].def.parallel = true;
    t[1].no_ask = true; /* it can stop a session: not read-only */
    t[1].run = t_bash_poll;
    t[1].cancel = bash_cancel;
}
