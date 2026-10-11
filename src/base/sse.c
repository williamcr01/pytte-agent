#include "pytte_base.h"

void pt_sse_init(pt_sse *s, pt_allocator al)
{
    pt_buf_init(&s->line, al);
    pt_buf_init(&s->data, al);
    pt_buf_init(&s->event, al);
    pt_buf_init(&s->id, al);
    s->cr = false;
    s->has_data = false;
}

void pt_sse_free(pt_sse *s)
{
    pt_buf_free(&s->line);
    pt_buf_free(&s->data);
    pt_buf_free(&s->event);
    pt_buf_free(&s->id);
}

static int dispatch(pt_sse *s, pt_sse_cb cb, void *ud)
{
    int r = 0;
    if (s->line.oom || s->data.oom || s->event.oom || s->id.oom)
        return -1;
    if (s->has_data) {
        pt_sse_event ev;
        ev.event = pt_buf_str(&s->event);
        ev.data = pt_buf_str(&s->data);
        ev.id = pt_buf_str(&s->id);
        r = cb(ud, &ev);
    }
    pt_buf_reset(&s->data);
    pt_buf_reset(&s->event);
    s->has_data = false;
    return r;
}

static int on_line(pt_sse *s, pt_str l, pt_sse_cb cb, void *ud)
{
    pt_str field, value = {"", 0};
    long colon;
    if (l.n == 0)
        return dispatch(s, cb, ud);
    if (l.p[0] == ':')
        return 0; /* comment */
    colon = pt_str_find(l, ':');
    field = l;
    if (colon >= 0) {
        field.n = (size_t)colon;
        value.p = l.p + colon + 1;
        value.n = l.n - (size_t)colon - 1;
        if (value.n && value.p[0] == ' ') {
            value.p++;
            value.n--;
        }
    }
    if (pt_str_eqz(field, "data")) {
        if (s->has_data)
            pt_buf_putc(&s->data, '\n');
        pt_buf_puts(&s->data, value);
        s->has_data = true;
    } else if (pt_str_eqz(field, "event")) {
        pt_buf_reset(&s->event);
        pt_buf_puts(&s->event, value);
    } else if (pt_str_eqz(field, "id")) {
        if (pt_str_find(value, '\0') < 0) {
            pt_buf_reset(&s->id);
            pt_buf_puts(&s->id, value);
        }
    }
    /* "retry" and unknown fields are ignored */
    return 0;
}

int pt_sse_feed(pt_sse *s, const char *p, size_t n, pt_sse_cb cb, void *ud)
{
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        int r;
        if (s->cr) {
            s->cr = false;
            if (p[i] == '\n') {
                i++;
                continue;
            }
        }
        while (j < n && p[j] != '\r' && p[j] != '\n')
            j++;
        if (j == n) { /* incomplete line: keep it for the next chunk */
            pt_buf_put(&s->line, p + i, n - i);
            return s->line.oom ? -1 : 0;
        }
        if (s->line.n) {
            pt_buf_put(&s->line, p + i, j - i);
            r = on_line(s, pt_buf_str(&s->line), cb, ud);
            pt_buf_reset(&s->line);
        } else {
            pt_str l;
            l.p = p + i;
            l.n = j - i;
            r = on_line(s, l, cb, ud);
        }
        if (p[j] == '\r')
            s->cr = true;
        i = j + 1;
        if (s->line.oom || s->data.oom || s->event.oom || s->id.oom)
            return -1;
        if (r)
            return r;
    }
    return 0;
}

int pt_sse_finish(pt_sse *s, pt_sse_cb cb, void *ud)
{
    int r = 0;
    s->cr = false;
    if (s->line.oom || s->data.oom || s->event.oom || s->id.oom)
        return -1;
    if (s->line.n) {
        r = on_line(s, pt_buf_str(&s->line), cb, ud);
        pt_buf_reset(&s->line);
        if (r)
            return r;
    }
    return dispatch(s, cb, ud);
}
