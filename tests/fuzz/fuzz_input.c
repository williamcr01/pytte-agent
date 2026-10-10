/* Cached incremental wrapping must equal a fresh full layout after edits. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "tui/tui.h"

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

static void *allocate(void *ud, void *p, size_t old, size_t n)
{
    (void)ud; (void)old;
    if (!n) { free(p); return NULL; }
    return realloc(p, n);
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    pt_allocator al = {allocate, NULL};
    tui_input cached, fresh;
    size_t i;
    int width = 76, show = 8;
    tui_input_init(&cached, al);
    tui_input_init(&fresh, al);
    for (i = 0; i < size; i++) {
        tui_ev ev;
        int j;
        unsigned op = data[i] % 15;
        memset(&ev, 0, sizeof ev);
        ev.kind = TE_KEY;
        fresh.layout_valid = false;
        if (op < 4 && cached.text.n < 16384) {
            static const char *const text[] = {"word ", "\n", "é", "👩‍💻🇸🇪界"};
            tui_input_insert(&cached, pt_strz(text[op]));
            tui_input_insert(&fresh, pt_strz(text[op]));
        } else if (op == 4 && cached.text.n && cached.text.n < 16384) {
            size_t end = tui_next_cluster(pt_buf_str(&cached.text), 0);
            tui_input_insert(&cached, (pt_str){cached.text.p, end});
            tui_input_insert(&fresh, (pt_str){fresh.text.p, end});
        } else if (op == 5) {
            width = 2 + data[i] % 100;
        } else if (op == 6) {
            tui_set_ambiguous_wide((data[i] & 16) != 0);
        } else {
            static const uint32_t keys[] = {TK_BACKSPACE, TK_DELETE, TK_LEFT, TK_RIGHT,
                                           TK_UP, TK_DOWN, TK_HOME, TK_END};
            ev.key = keys[op % 8];
            assert(tui_input_key(&cached, &ev, width) == tui_input_key(&fresh, &ev, width));
        }
        fresh.layout_valid = false;
        assert(tui_input_layout(&cached, width, show));
        assert(tui_input_layout(&fresh, width, show));
        assert(pt_str_eq(pt_buf_str(&cached.text), pt_buf_str(&fresh.text)));
        assert(cached.cur == fresh.cur && cached.nrows == fresh.nrows);
        assert(cached.cur_row == fresh.cur_row && cached.cur_col == fresh.cur_col);
        assert(cached.scroll == fresh.scroll);
        for (j = 0; j < cached.nrows; j++) {
            assert(cached.rows[j].start == fresh.rows[j].start);
            assert(cached.rows[j].end == fresh.rows[j].end);
        }
    }
    tui_input_free(&cached);
    tui_input_free(&fresh);
    tui_set_ambiguous_wide(false);
    return 0;
}
