/* pytte-ai: talk to one model through the AI layer. Exists to prove the
 * providers work, and to record fixtures, before the agent exists. */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "drv/drv.h"

static const char usage[] =
    "usage: pytte-ai [options] MODEL [PROMPT]\n"
    "\n"
    "MODEL is provider/model-id, e.g. anthropic/claude-opus-5-5, openai/gpt-5,\n"
    "openrouter/<vendor>/<model>, faux/faux. PROMPT is read from stdin when\n"
    "omitted.\n"
    "\n"
    "  --system TEXT       system prompt\n"
    "  --tools FILE        JSON list of tools: [{\"name\", \"description\", \"parameters\"}]\n"
    "  --reasoning LEVEL   off, minimal, low, medium, high, xhigh, max\n"
    "  --max-tokens N      default: the model's maximum that fits the context\n"
    "  --temperature X\n"
    "  --no-cache          do not ask for prompt caching\n"
    "  --events            print unified events as JSON lines\n"
    "  --record FILE.sse   save each raw response as a test fixture\n"
    "  --turns N           answer tool calls with canned results, up to N requests\n"
    "  --script FILE       faux provider script (tests/faux/*.json)\n"
    "  --base-url URL      talk to another server (a proxy, a local test server)\n"
    "  --api ID            use another API: anthropic-messages, openai-completions,\n"
    "                      openai-responses\n"
    "  --list              list known providers and models\n";

typedef struct opts {
    const char *model, *prompt, *system, *tools, *record, *script, *temperature, *base_url, *api;
    pt_reasoning reasoning;
    uint32_t max_tokens;
    long turns;
    bool events, no_cache, list;
} opts;

static volatile sig_atomic_t interrupted;

static void on_sigint(int sig)
{
    (void)sig;
    interrupted = 1;
}

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "pytte-ai: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

static void out_str(FILE *f, pt_str s)
{
    fwrite(s.p, 1, s.n, f);
}

/* ---- events ---- */

typedef struct ui {
    bool events, tty_err;
    bool text_nl; /* last text output ended with a newline */
    bool any_text;
    bool thinking_out; /* thinking text was printed for the current block */
    pt_buf thinking;   /* what was printed of it */
    pt_buf line;
} ui;

static void emit_json_event(ui *u, const pt_ai_event *ev)
{
    pt_jw w;
    const pt_msg *m = ev->msg;
    const pt_block *b = ev->index < m->nblocks ? &m->blocks[ev->index] : NULL;
    pt_buf_reset(&u->line);
    pt_jw_init(&w, &u->line);
    pt_jw_obj(&w);
    pt_jw_key(&w, "type");
    pt_jw_strz(&w, pt_ev_name(ev->kind));
    switch (ev->kind) {
    case PT_EV_START:
        pt_jw_key(&w, "provider");
        pt_jw_str(&w, m->provider);
        pt_jw_key(&w, "model");
        pt_jw_str(&w, m->model);
        break;
    case PT_EV_DONE:
    case PT_EV_ERROR:
        pt_jw_key(&w, "message");
        pt_msg_write_json(&w, m);
        break;
    default:
        pt_jw_key(&w, "index");
        pt_jw_uint(&w, ev->index);
        if (ev->delta.n) {
            pt_jw_key(&w, "delta");
            pt_jw_str(&w, ev->delta);
        }
        if (b && ev->kind == PT_EV_TOOLCALL_START) {
            pt_jw_key(&w, "id");
            pt_jw_str(&w, b->id);
            pt_jw_key(&w, "name");
            pt_jw_str(&w, b->name);
        }
        if (b && ev->kind == PT_EV_TEXT_END) {
            pt_jw_key(&w, "text");
            pt_jw_str(&w, b->text);
        }
        if (b && ev->kind == PT_EV_THINKING_END) {
            pt_jw_key(&w, "thinking");
            pt_jw_str(&w, b->text);
            pt_jw_key(&w, "signature");
            pt_jw_str(&w, b->signature);
            if (b->redacted) {
                pt_jw_key(&w, "redacted");
                pt_jw_bool(&w, true);
            }
        }
        if (b && ev->kind == PT_EV_TOOLCALL_END) {
            pt_jw_key(&w, "arguments");
            pt_jw_str(&w, b->args);
        }
    }
    pt_jw_obj_end(&w);
    pt_buf_putc(&u->line, '\n');
    out_str(stdout, pt_buf_str(&u->line));
    fflush(stdout);
}

static void on_event(void *ud, const pt_ai_event *ev)
{
    ui *u = (ui *)ud;
    const pt_msg *m = ev->msg;
    if (u->events) {
        emit_json_event(u, ev);
        return;
    }
    switch (ev->kind) {
    case PT_EV_THINKING_DELTA:
        if (!u->thinking_out && u->tty_err)
            fputs("\x1b[2m", stderr);
        u->thinking_out = true;
        out_str(stderr, ev->delta);
        pt_buf_puts(&u->thinking, ev->delta);
        break;
    case PT_EV_THINKING_END:
        /* OpenAI sometimes rewrites a reasoning summary mid-stream; then the
         * final text differs from what was printed. Show the final one. */
        if (u->thinking_out && !pt_str_eq(pt_buf_str(&u->thinking), m->blocks[ev->index].text)) {
            fputs("\n[revised]\n", stderr);
            out_str(stderr, m->blocks[ev->index].text);
        }
        pt_buf_reset(&u->thinking);
        if (m->blocks[ev->index].redacted) {
            fputs(u->tty_err ? "\x1b[2m[redacted thinking]" : "[redacted thinking]", stderr);
            u->thinking_out = true;
        }
        if (u->thinking_out) /* empty thinking (no summary) prints nothing */
            fputs(u->tty_err ? "\x1b[0m\n" : "\n", stderr);
        u->thinking_out = false;
        break;
    case PT_EV_TEXT_DELTA:
        out_str(stdout, ev->delta);
        if (ev->delta.n)
            u->text_nl = ev->delta.p[ev->delta.n - 1] == '\n';
        u->any_text = true;
        fflush(stdout);
        break;
    case PT_EV_TOOLCALL_END: {
        const pt_block *b = &m->blocks[ev->index];
        if (u->any_text && !u->text_nl) {
            fputc('\n', stdout);
            u->text_nl = true;
        }
        fflush(stdout);
        fprintf(stderr, "%s-> %.*s %.*s%s\n", u->tty_err ? "\x1b[36m" : "", (int)b->name.n, b->name.p,
                (int)b->args.n, b->args.p, u->tty_err ? "\x1b[0m" : "");
        break;
    }
    case PT_EV_DONE:
    case PT_EV_ERROR:
        if (u->any_text && !u->text_nl)
            fputc('\n', stdout);
        u->any_text = false;
        fflush(stdout);
        break;
    default:
        break;
    }
}

/* ---- one request ---- */

typedef struct call {
    pt_ai_stream *s;
    pt_buf rec; /* raw response, when recording */
    bool recording;
    bool http_done; /* the driver is finished with this request */
    int status;
    char retry_after[32];
} call;

static int64_t now(void)
{
    return pt_sys_now_ms();
}

static void sink_head(void *ud, int status, pt_str retry_after)
{
    call *c = (call *)ud;
    c->status = status;
    if (retry_after.n < sizeof c->retry_after) {
        memcpy(c->retry_after, retry_after.p, retry_after.n);
        c->retry_after[retry_after.n] = 0;
    }
    pt_ai_stream_head(c->s, status, retry_after);
}

static void sink_body(void *ud, const char *p, size_t n)
{
    call *c = (call *)ud;
    if (c->recording)
        pt_buf_put(&c->rec, p, n);
    pt_ai_stream_body(c->s, p, n, now());
}

static void sink_done(void *ud, const char *error)
{
    call *c = (call *)ud;
    c->http_done = true;
    pt_ai_stream_end(c->s, error, now());
}

/* Never let a credential into a fixture. */
static void redact(pt_buf *b, pt_str secret)
{
    size_t i;
    if (secret.n < 8)
        return;
    for (i = 0; i + secret.n <= b->n; i++)
        if (memcmp(b->p + i, secret.p, secret.n) == 0)
            memset(b->p + i, 'X', secret.n);
}

static void record(const opts *o, const pt_ai_target *t, long turn, const call *c, const pt_http_req *req,
                   pt_str key)
{
    char path[4096], reqpath[4096];
    size_t l = strlen(o->record);
    int base = (int)(l > 4 && strcmp(o->record + l - 4, ".sse") == 0 ? l - 4 : l);
    pt_buf out, rq;
    pt_jw w;
    if (turn == 0)
        snprintf(path, sizeof path, "%.*s.sse", base, o->record);
    else
        snprintf(path, sizeof path, "%.*s-%ld.sse", base, o->record, turn + 1);
    snprintf(reqpath, sizeof reqpath, "%.*s.request.json", (int)strlen(path) - 4, path);
    pt_buf_init(&out, pt_sys_allocator());
    pt_jw_init(&w, &out);
    pt_jw_obj(&w);
    pt_jw_key(&w, "model"); /* resolved, so a replay picks the same model */
    pt_buf_init(&rq, pt_sys_allocator());
    pt_buf_putz(&rq, t->provider->id);
    pt_buf_putc(&rq, '/');
    pt_buf_putz(&rq, t->model.id);
    pt_jw_str(&w, pt_buf_str(&rq));
    pt_buf_reset(&rq);
    pt_jw_key(&w, "status");
    pt_jw_int(&w, c->status);
    pt_jw_key(&w, "retry_after");
    pt_jw_strz(&w, c->retry_after);
    pt_jw_key(&w, "recorded_ms");
    pt_jw_int(&w, now());
    pt_jw_obj_end(&w);
    pt_buf_putc(&out, '\n');
    pt_buf_puts(&out, pt_buf_str(&c->rec));
    redact(&out, key);
    pt_buf_puts(&rq, req->body);
    redact(&rq, key);
    if (!pt_sys_write_file(path, out.p, out.n) || !pt_sys_write_file(reqpath, rq.p, rq.n))
        fprintf(stderr, "pytte-ai: could not write %s\n", path);
    else
        fprintf(stderr, "recorded %s\n", path);
    pt_buf_free(&out);
    pt_buf_free(&rq);
}

static void summary(const ui *u, const pt_ai_target *t, const pt_msg *m, const pt_ai_error *e)
{
    const pt_cost *c = &t->model.cost;
    const char *dim = u->tty_err ? "\x1b[2m" : "", *reset = u->tty_err ? "\x1b[0m" : "";
    if (u->events)
        return;
    if (m->stop == PT_STOP_ERROR || m->stop == PT_STOP_ABORTED) {
        fprintf(stderr, "%serror%s (%s", u->tty_err ? "\x1b[31m" : "", reset, pt_err_class_name(e->cls));
        if (e->status && e->status != 200)
            fprintf(stderr, ", HTTP %d", e->status);
        if (e->retry_after_ms >= 0)
            fprintf(stderr, ", retry after %lld ms", (long long)e->retry_after_ms);
        fprintf(stderr, "): %.*s\n", (int)m->error.n, m->error.p);
    }
    fprintf(stderr, "%s[%s", dim, pt_stop_name(m->stop));
    if (m->raw_stop.n && !pt_str_eqz(m->raw_stop, pt_stop_name(m->stop)))
        fprintf(stderr, " (%.*s)", (int)m->raw_stop.n, m->raw_stop.p);
    if (m->response_model.n)
        fprintf(stderr, " | %.*s", (int)m->response_model.n, m->response_model.p);
    fprintf(stderr, " | in %llu out %llu", (unsigned long long)m->usage.input,
            (unsigned long long)m->usage.output);
    if (m->usage.cache_read || m->usage.cache_write)
        fprintf(stderr, " cache r%llu w%llu", (unsigned long long)m->usage.cache_read,
                (unsigned long long)m->usage.cache_write);
    if (m->usage.reasoning)
        fprintf(stderr, " reasoning %llu", (unsigned long long)m->usage.reasoning);
    fprintf(stderr, " | %lld.%01llds", (long long)(m->duration_ms / 1000),
            (long long)(m->duration_ms % 1000 / 100));
    if (c->input || c->output) {
        unsigned long long micro =
            (unsigned long long)((m->usage.input * c->input + m->usage.output * c->output +
                                  m->usage.cache_read * c->cache_read +
                                  m->usage.cache_write * c->cache_write) /
                                 1000000);
        fprintf(stderr, " | $%llu.%06llu", micro / 1000000, micro % 1000000);
    }
    fprintf(stderr, "]%s\n", reset);
}

/* ---- setup ---- */

static void parse_args(int argc, char **argv, opts *o)
{
    int i;
    memset(o, 0, sizeof *o);
    o->turns = 1;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define TAKES(name) (strcmp(a, name) == 0 && (v || (die("missing value for", name), 0)) && ++i)
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            fputs(usage, stdout);
            exit(0);
        } else if (strcmp(a, "--events") == 0) {
            o->events = true;
        } else if (strcmp(a, "--no-cache") == 0) {
            o->no_cache = true;
        } else if (strcmp(a, "--list") == 0) {
            o->list = true;
        } else if (TAKES("--system")) {
            o->system = v;
        } else if (TAKES("--tools")) {
            o->tools = v;
        } else if (TAKES("--record")) {
            o->record = v;
        } else if (TAKES("--script")) {
            o->script = v;
        } else if (TAKES("--api")) {
            o->api = v;
        } else if (TAKES("--base-url")) {
            o->base_url = v;
        } else if (TAKES("--temperature")) {
            o->temperature = v;
        } else if (TAKES("--reasoning")) {
            if (!pt_reasoning_parse(pt_strz(v), &o->reasoning))
                die("unknown reasoning level", v);
        } else if (TAKES("--max-tokens")) {
            uint64_t n;
            if (!pt_str_to_u64(pt_strz(v), &n) || n == 0 || n > UINT32_MAX)
                die("bad --max-tokens", v);
            o->max_tokens = (uint32_t)n;
        } else if (TAKES("--turns")) {
            uint64_t n;
            if (!pt_str_to_u64(pt_strz(v), &n) || n == 0 || n > 100000)
                die("bad --turns", v);
            o->turns = (long)n;
        } else if (a[0] == '-' && a[1]) {
            die("unknown option", a);
        } else if (!o->model) {
            o->model = a;
        } else if (!o->prompt) {
            o->prompt = a;
        } else {
            die("too many arguments", a);
        }
#undef TAKES
    }
}

static void list_models(void)
{
    const pt_ai_catalog *cat = pt_ai_builtin_catalog();
    uint32_t i, j;
    for (i = 0; i < cat->nproviders; i++) {
        const pt_provider *p = &cat->providers[i];
        const char *key = p->key_env[0];
        printf("%s (%s, %s)%s%s%s\n", p->id, p->name, p->api, key ? "  key: " : "", key ? key : "",
               key ? (getenv(key) && *getenv(key) ? " (set)" : " (not set)") : "");
        for (j = 0; j < cat->nmodels; j++) {
            const pt_model_entry *m = &cat->models[j];
            char ref[128];
            if (m->provider != i)
                continue;
            snprintf(ref, sizeof ref, "%s/%s", p->id, m->id);
            printf("  %-30s %8lu ctx %7lu out%s\n", ref, (unsigned long)m->context_window,
                   (unsigned long)m->max_tokens, m->reasoning ? "  reasoning" : "");
        }
        if (strcmp(p->id, "openrouter") == 0)
            printf("  %-30s any OpenRouter model ID\n", "openrouter/<vendor>/<model>");
    }
}

/* Tool definitions from a JSON file; the file stays loaded for the run. */
static pt_tool *load_tools(const char *path, uint32_t *n, pt_arena *a)
{
    size_t len;
    char *src = pt_sys_read_file(path, &len);
    pt_json_err err;
    pt_json *v;
    const pt_json *c;
    pt_tool *tools;
    uint32_t i = 0;
    if (!src)
        die("cannot read tools file", path);
    v = pt_json_parse((pt_str){src, len}, a, &err);
    if (!v || v->type != PT_J_ARR) {
        fprintf(stderr, "pytte-ai: %s: %s at byte %zu\n", path, v ? "expected a JSON array" : err.msg, err.pos);
        exit(1);
    }
    tools = (pt_tool *)pt_arena_zalloc(a, sizeof(pt_tool) * (v->len ? v->len : 1));
    if (!tools) {
        free(src);
        fprintf(stderr, "pytte-ai: out of memory loading tools from %s\n", path);
        exit(1);
    }
    for (c = v->child; c; c = c->next, i++) {
        const pt_json *schema = pt_json_get(c, "parameters");
        if (!schema)
            schema = pt_json_get(c, "input_schema");
        tools[i].name = pt_json_gets(c, "name", a);
        tools[i].description = pt_json_gets(c, "description", a);
        tools[i].schema = schema ? schema->raw : PT_STR_NULL;
        if (!tools[i].name.n)
            die("a tool has no name in", path);
    }
    *n = i;
    return tools;
}

static pt_str read_stdin(void)
{
    pt_buf b;
    char tmp[4096];
    size_t got;
    pt_buf_init(&b, pt_sys_allocator());
    while ((got = fread(tmp, 1, sizeof tmp, stdin)) > 0)
        pt_buf_put(&b, tmp, got);
    return pt_str_trim(pt_buf_str(&b)); /* leaked on purpose: lives for the run */
}

/* The canned result for a tool call in --turns mode: echo returns its text
 * argument; anything else reports what it was called with. */
static pt_str canned_result(const pt_block *b, pt_arena *a)
{
    pt_json *args = pt_json_parse(b->args, a, NULL);
    pt_buf out;
    pt_str r;
    if (pt_str_eqz(b->name, "echo") && pt_json_is(pt_json_get(args, "text"), PT_J_STR))
        return pt_json_gets(args, "text", a);
    pt_buf_init(&out, a->al);
    pt_buf_putz(&out, "(canned result) ");
    pt_buf_puts(&out, b->name);
    pt_buf_putz(&out, " ran with ");
    pt_buf_puts(&out, b->args);
    r = pt_arena_dup(a, pt_buf_str(&out));
    pt_buf_free(&out);
    return r;
}

static bool has_models(const pt_ai_catalog *cat, const char *provider)
{
    uint32_t i;
    for (i = 0; i < cat->nmodels; i++)
        if (strcmp(cat->providers[cat->models[i].provider].id, provider) == 0)
            return true;
    return false;
}

int main(int argc, char **argv)
{
    opts o;
    pt_allocator al = pt_sys_allocator();
    pt_arena session;
    pt_ai_target t;
    pt_provider provider;
    pt_credentials cred;
    pt_http_driver *http;
    pt_msg *msgs = NULL;
    uint32_t nmsgs = 0, cap = 0, ntools = 0;
    pt_tool *tools = NULL;
    const char *why, *which;
    ui u;
    long turn;
    int rc = 0;

    parse_args(argc, argv, &o);
    if (o.list) {
        list_models();
        return 0;
    }
    if (!o.model) {
        fputs(usage, stderr);
        return 1;
    }
    pt_arena_init(&session, al, 0);
    if (!pt_ai_resolve(pt_ai_builtin_catalog(), pt_strz(o.model), &t, &session, &why))
        die(why, o.model);
    if (o.api && !pt_ai_retarget(&t, pt_strz(o.api)))
        die("unknown API", o.api);
    if (o.base_url) {
        provider = *t.provider;
        provider.base_url = o.base_url;
        t.provider = &provider;
    }
    /* Only worth a note where the catalog lists the provider's models;
     * OpenRouter's are never listed. */
    if (!t.known && has_models(pt_ai_builtin_catalog(), t.provider->id))
        fprintf(stderr, "pytte-ai: note: %s is not in the catalog; using %s defaults\n", t.model.id,
                t.provider->id);
    if (!pt_ai_find_key(t.provider, pt_sys_env, NULL, &cred, &which)) {
        if (t.provider->auth == PT_AUTH_OAUTH)
            fprintf(stderr, "pytte-ai: %s needs a login, which only pytte has\n", t.provider->id);
        else
            fprintf(stderr, "pytte-ai: no API key for %s: set %s\n", t.provider->id, t.provider->key_env[0]);
        return 1;
    }
    if (strcmp(t.provider->id, "faux") == 0) {
        size_t n;
        char *script;
        if (!o.script)
            die("the faux provider needs --script FILE", NULL);
        script = pt_sys_read_file(o.script, &n);
        if (!script)
            die("cannot read script", o.script);
        http = pt_http_faux_new((pt_str){script, n});
        free(script);
    } else {
        http = pt_http_curl_new();
    }
    if (!http)
        die("cannot start the HTTP driver", NULL);
    if (o.tools)
        tools = load_tools(o.tools, &ntools, &session);
    if (!o.prompt && isatty(STDIN_FILENO))
        die("no prompt (give one as an argument or on stdin)", NULL);

    cap = 16;
    msgs = (pt_msg *)calloc(cap, sizeof *msgs);
    if (!msgs) {
        fputs("pytte-ai: out of memory allocating messages\n", stderr);
        http->free(http);
        pt_arena_free(&session);
        return 1;
    }
    if (o.system) {
        msgs[nmsgs].role = PT_ROLE_SYSTEM;
        msgs[nmsgs++].text = pt_strz(o.system);
    }
    msgs[nmsgs].role = PT_ROLE_USER;
    msgs[nmsgs++].text = o.prompt ? pt_strz(o.prompt) : read_stdin();

    memset(&u, 0, sizeof u);
    u.events = o.events;
    u.tty_err = isatty(STDERR_FILENO);
    pt_buf_init(&u.line, al);
    pt_buf_init(&u.thinking, al);
    signal(SIGINT, on_sigint);

    for (turn = 0; turn < o.turns; turn++) {
        pt_arena scratch;
        pt_ai_context ctx;
        pt_ai_opts ao;
        pt_http_req req;
        call c;
        void *h;
        pt_msg m;
        pt_ai_error e;
        uint32_t i;

        pt_arena_init(&scratch, al, 0);
        ctx.msgs = msgs;
        ctx.nmsgs = nmsgs;
        ctx.tools = tools;
        ctx.ntools = ntools;
        memset(&ao, 0, sizeof ao);
        ao.max_tokens = o.max_tokens;
        ao.reasoning = o.reasoning;
        ao.no_cache = o.no_cache;
        ao.temperature = pt_strz(o.temperature);
        if (!pt_ai_build(&t, &ctx, &ao, &cred, &req, &scratch))
            die("could not build the request", NULL);

        memset(&c, 0, sizeof c);
        c.recording = o.record != NULL;
        pt_buf_init(&c.rec, al);
        c.s = pt_ai_stream_new(&t, &session, al, on_event, &u, now());
        if (!c.s)
            die("out of memory", NULL);
        {
            pt_http_sink sink;
            sink.ud = &c;
            sink.head = sink_head;
            sink.body = sink_body;
            sink.done = sink_done;
            sink.header = NULL;
            h = http->start(http, &req, &sink);
        }
        if (!h) {
            c.http_done = true;
            pt_ai_stream_end(c.s, "could not start the request", now());
        }
        while (!pt_ai_stream_finished(c.s)) {
            if (interrupted) {
                pt_ai_stream_abort(c.s, now());
                break;
            }
            if (http->pump(http, 100, NULL, 0) < 0)
                pt_ai_stream_end(c.s, "HTTP driver failure", now());
        }
        /* The stream can finish (on the final event) before the transfer
         * does. Drop the transfer, or its completion would land on the next
         * turn's call. */
        if (!c.http_done)
            http->cancel(http, h);
        m = *pt_ai_stream_message(c.s);
        e = *pt_ai_stream_error(c.s);
        pt_ai_stream_free(c.s);
        summary(&u, &t, &m, &e);
        if (o.record && !interrupted)
            record(&o, &t, turn, &c, &req, cred.api_key);
        pt_buf_free(&c.rec);
        pt_arena_free(&scratch);

        if (nmsgs + m.nblocks + 1 >= cap) {
            uint32_t new_cap = (nmsgs + m.nblocks + 1) * 2;
            pt_msg *new_msgs = (pt_msg *)realloc(msgs, new_cap * sizeof *msgs);
            if (!new_msgs) {
                fputs("pytte-ai: out of memory growing messages\n", stderr);
                http->free(http);
                pt_buf_free(&u.line);
                pt_buf_free(&u.thinking);
                free(msgs);
                pt_arena_free(&session);
                return 1;
            }
            msgs = new_msgs;
            cap = new_cap;
        }
        msgs[nmsgs++] = m;
        if (m.stop == PT_STOP_ERROR || m.stop == PT_STOP_ABORTED) {
            rc = interrupted ? 130 : 2;
            break;
        }
        if (m.stop != PT_STOP_TOOL_USE || turn + 1 >= o.turns)
            break;
        for (i = 0; i < m.nblocks; i++) {
            pt_msg *r;
            if (m.blocks[i].kind != PT_BLOCK_TOOL_CALL)
                continue;
            r = &msgs[nmsgs++];
            memset(r, 0, sizeof *r);
            r->role = PT_ROLE_TOOL_RESULT;
            r->call_id = m.blocks[i].id;
            r->name = m.blocks[i].name;
            r->text = canned_result(&m.blocks[i], &session);
            if (!o.events)
                fprintf(stderr, "%s<- %.*s%s\n", u.tty_err ? "\x1b[36m" : "", (int)r->text.n, r->text.p,
                        u.tty_err ? "\x1b[0m" : "");
        }
    }

    http->free(http);
    pt_buf_free(&u.line);
    pt_buf_free(&u.thinking);
    free(msgs);
    pt_arena_free(&session);
    return rc;
}
