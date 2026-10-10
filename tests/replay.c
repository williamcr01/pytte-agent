/* Replay a PYTTE_TUI_TRACE file into the test emulator (section 22.5) and
 * print what the terminal would show: the scrollback and screen as lines,
 * then the emulator's verdict.
 *
 *   build/replay TRACE [--no-reflow] [--old-widths] [--sticky] */
#include "t.h"
#include "vt.c"
#include "drv/drv.h"

int main(int argc, char **argv)
{
    size_t n, at = 0;
    char *src;
    bool reflow = true, old = false, sticky = false;
    int i;
    vt *v = NULL;
    pt_arena ar;
    pt_buf out, bytes;
    if (argc < 2) {
        fprintf(stderr, "usage: replay TRACE [--no-reflow] [--old-widths]\n");
        return 2;
    }
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--no-reflow") == 0)
            reflow = false;
        else if (strcmp(argv[i], "--old-widths") == 0)
            old = true;
        else if (strcmp(argv[i], "--sticky") == 0)
            sticky = true;
    }
    if (!(src = pt_sys_read_file(argv[1], &n))) {
        perror(argv[1]);
        return 1;
    }
    pt_arena_init(&ar, T_AL, 0);
    pt_buf_init(&out, T_AL);
    pt_buf_init(&bytes, T_AL);
    while (at < n) {
        size_t e = at;
        pt_json *j;
        while (e < n && src[e] != '\n')
            e++;
        pt_arena_reset(&ar);
        j = pt_json_parse((pt_str){src + at, e - at}, &ar, NULL);
        at = e + 1;
        if (!j)
            continue;
        if (pt_json_get(j, "size")) {
            int r = 0, c = 0;
            pt_str s = pt_json_gets(j, "size", &ar);
            char tmp[32];
            snprintf(tmp, sizeof tmp, "%.*s", (int)(s.n < 31 ? s.n : 31), s.p);
            sscanf(tmp, "%d %d", &r, &c);
            if (!v) {
                v = vt_new(r, c, reflow);
                if (old)
                    v->width = vt_width_old;
                v->sticky = sticky;
            } else if (r != v->rows || c != v->cols) {
                vt_resize(v, r, c);
            }
        } else if (v && pt_json_get(j, "w")) {
            const pt_json *w = pt_json_get(j, "w");
            pt_buf_reset(&bytes);
            pt_json_unescape((pt_str){w->raw.p + 1, w->raw.n - 2}, &bytes);
            for (i = 0; (size_t)i < bytes.n; i++) { /* the tty's OPOST: \n is CR LF */
                if (bytes.p[i] == '\n')
                    vt_write(v, "\r", 1);
                vt_write(v, bytes.p + i, 1);
            }
        }
    }
    if (!v) {
        fprintf(stderr, "no size in the trace\n");
        return 1;
    }
    vt_lines(v, v->rows, &out);
    fwrite(out.p, 1, out.n, stdout);
    printf("---- cursor row %d col %d; %s\n", v->r, v->c, v->err[0] ? v->err : "no errors");
    (void)t_done;
    (void)vt_row_text;
    vt_free(v);
    pt_buf_free(&out);
    pt_buf_free(&bytes);
    pt_arena_free(&ar);
    free(src);
    return 0;
}
