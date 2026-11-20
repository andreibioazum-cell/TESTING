#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    TOK_PLAIN = 0,
    TOK_KEYWORD = 1,
    TOK_TYPE = 2,
    TOK_BUILTIN = 3,
    TOK_STRING = 4,
    TOK_NUMBER = 5,
    TOK_COMMENT = 6,
    TOK_OPERATOR = 7
} LcEditorTokKind;

typedef struct {
    char *text;
    size_t len;
    size_t cursor;
} LcEditorSnap;

#define LC_UNDO_MAX 64

typedef struct {
    char *buf;
    size_t cap;
    size_t gap_start;
    size_t gap_end;

    size_t *line_starts;
    int line_count;
    int line_cap;
    int lines_dirty;

    char *flat_cache;
    size_t flat_cap;

    LcEditorSnap undo_stack[LC_UNDO_MAX];
    int undo_len;
    LcEditorSnap redo_stack[LC_UNDO_MAX];
    int redo_len;
} LcEditor;

static LcEditor g_ed = {0};

static size_t ed_len(void) {
    return g_ed.cap - (g_ed.gap_end - g_ed.gap_start);
}

static char ed_at(size_t pos) {
    if (pos < g_ed.gap_start) return g_ed.buf[pos];
    return g_ed.buf[pos + (g_ed.gap_end - g_ed.gap_start)];
}

static void ed_ensure_gap(size_t need) {
    size_t gap = g_ed.gap_end - g_ed.gap_start;
    if (gap >= need) return;
    size_t cur_len = ed_len();
    size_t new_cap = g_ed.cap ? g_ed.cap : 4096;
    while (new_cap - cur_len < need + 256) new_cap *= 2;
    char *nb = (char *)malloc(new_cap);
    if (!nb) return;
    size_t tail = cur_len - g_ed.gap_start;
    if (g_ed.gap_start > 0) memcpy(nb, g_ed.buf, g_ed.gap_start);
    if (tail > 0) memcpy(nb + new_cap - tail, g_ed.buf + g_ed.gap_end, tail);
    free(g_ed.buf);
    g_ed.buf = nb;
    g_ed.cap = new_cap;
    g_ed.gap_end = new_cap - tail;
}

static void ed_move_gap(size_t pos) {
    size_t len = ed_len();
    if (pos > len) pos = len;
    if (pos == g_ed.gap_start) return;
    size_t gap = g_ed.gap_end - g_ed.gap_start;
    if (pos < g_ed.gap_start) {
        size_t move = g_ed.gap_start - pos;
        memmove(g_ed.buf + g_ed.gap_end - move, g_ed.buf + pos, move);
        g_ed.gap_start = pos;
        g_ed.gap_end -= move;
    } else {
        size_t move = pos - g_ed.gap_start;
        memmove(g_ed.buf + g_ed.gap_start, g_ed.buf + g_ed.gap_end, move);
        g_ed.gap_start += move;
        g_ed.gap_end += move;
    }
    (void)gap;
}

static void ed_clear_snaps(LcEditorSnap *stack, int *len) {
    for (int i = 0; i < *len; ++i) free(stack[i].text);
    *len = 0;
}

static void ed_push_snap(LcEditorSnap *stack, int *len) {
    size_t total = ed_len();
    char *copy = (char *)malloc(total + 1);
    if (!copy) return;
    if (g_ed.gap_start > 0) memcpy(copy, g_ed.buf, g_ed.gap_start);
    size_t tail = total - g_ed.gap_start;
    if (tail > 0) memcpy(copy + g_ed.gap_start, g_ed.buf + g_ed.gap_end, tail);
    copy[total] = '\0';
    if (*len == LC_UNDO_MAX) {
        free(stack[0].text);
        memmove(stack, stack + 1, sizeof(LcEditorSnap) * (LC_UNDO_MAX - 1));
        (*len)--;
    }
    stack[*len].text = copy;
    stack[*len].len = total;
    stack[*len].cursor = g_ed.gap_start;
    (*len)++;
}

static void ed_save_undo(void) {
    ed_clear_snaps(g_ed.redo_stack, &g_ed.redo_len);
    ed_push_snap(g_ed.undo_stack, &g_ed.undo_len);
}

static void ed_rebuild_lines(void) {
    if (!g_ed.lines_dirty && g_ed.line_starts) return;
    size_t total = ed_len();
    g_ed.line_count = 0;
    if (!g_ed.line_starts) {
        g_ed.line_cap = 256;
        g_ed.line_starts = (size_t *)malloc(sizeof(size_t) * (size_t)g_ed.line_cap);
    }
    g_ed.line_starts[g_ed.line_count++] = 0;
    for (size_t i = 0; i < total; ++i) {
        if (ed_at(i) == '\n') {
            if (g_ed.line_count >= g_ed.line_cap) {
                g_ed.line_cap *= 2;
                g_ed.line_starts = (size_t *)realloc(g_ed.line_starts, sizeof(size_t) * (size_t)g_ed.line_cap);
            }
            g_ed.line_starts[g_ed.line_count++] = i + 1;
        }
    }
    g_ed.lines_dirty = 0;
}

void lc_editor_init(void) {
    if (!g_ed.buf) {
        g_ed.cap = 4096;
        g_ed.buf = (char *)malloc(g_ed.cap);
        g_ed.gap_start = 0;
        g_ed.gap_end = g_ed.cap;
    }
    g_ed.lines_dirty = 1;
    ed_rebuild_lines();
}

void lc_editor_free(void) {
    free(g_ed.buf);
    free(g_ed.line_starts);
    free(g_ed.flat_cache);
    ed_clear_snaps(g_ed.undo_stack, &g_ed.undo_len);
    ed_clear_snaps(g_ed.redo_stack, &g_ed.redo_len);
    memset(&g_ed, 0, sizeof(g_ed));
}

static void ed_load_raw(const char *utf8, size_t len, size_t cursor) {
    lc_editor_init();
    if (len + 512 > g_ed.cap) {
        size_t new_cap = len + 1024;
        char *nb = (char *)malloc(new_cap);
        if (!nb) return;
        free(g_ed.buf);
        g_ed.buf = nb;
        g_ed.cap = new_cap;
    }
    if (cursor > len) cursor = len;
    if (cursor > 0) memcpy(g_ed.buf, utf8, cursor);
    size_t tail = len - cursor;
    if (tail > 0) memcpy(g_ed.buf + g_ed.cap - tail, utf8 + cursor, tail);
    g_ed.gap_start = cursor;
    g_ed.gap_end = g_ed.cap - tail;
    g_ed.lines_dirty = 1;
    ed_rebuild_lines();
}

void lc_editor_set_text(const char *utf8) {
    ed_clear_snaps(g_ed.undo_stack, &g_ed.undo_len);
    ed_clear_snaps(g_ed.redo_stack, &g_ed.redo_len);
    if (!utf8) utf8 = "";
    ed_load_raw(utf8, strlen(utf8), 0);
}

const char *lc_editor_get_text(void) {
    lc_editor_init();
    size_t total = ed_len();
    if (total + 1 > g_ed.flat_cap) {
        g_ed.flat_cap = total + 256;
        g_ed.flat_cache = (char *)realloc(g_ed.flat_cache, g_ed.flat_cap);
    }
    if (g_ed.gap_start > 0) memcpy(g_ed.flat_cache, g_ed.buf, g_ed.gap_start);
    size_t tail = total - g_ed.gap_start;
    if (tail > 0) memcpy(g_ed.flat_cache + g_ed.gap_start, g_ed.buf + g_ed.gap_end, tail);
    g_ed.flat_cache[total] = '\0';
    return g_ed.flat_cache;
}

void lc_editor_insert(const char *utf8) {
    if (!utf8 || !*utf8) return;
    lc_editor_init();
    ed_save_undo();
    size_t n = strlen(utf8);
    ed_ensure_gap(n);
    for (size_t i = 0; i < n; ++i) {
        if (utf8[i] == '\r') continue;
        g_ed.buf[g_ed.gap_start++] = utf8[i];
    }
    g_ed.lines_dirty = 1;
}

void lc_editor_newline(void) {
    lc_editor_init();
    ed_save_undo();
    ed_rebuild_lines();
    size_t cur = g_ed.gap_start;
    size_t line_start = cur;
    while (line_start > 0 && ed_at(line_start - 1) != '\n') line_start--;
    int indent = 0;
    while (line_start + (size_t)indent < cur && ed_at(line_start + (size_t)indent) == ' ') indent++;
    int extra = 0;
    size_t back = cur;
    while (back > line_start && (ed_at(back - 1) == ' ' || ed_at(back - 1) == '\t')) back--;
    if (back > line_start && ed_at(back - 1) == ':') extra = 4;
    size_t total_spaces = (size_t)(indent + extra);
    if (total_spaces > 64) total_spaces = 64;
    ed_ensure_gap(1 + total_spaces);
    g_ed.buf[g_ed.gap_start++] = '\n';
    for (size_t i = 0; i < total_spaces; ++i) g_ed.buf[g_ed.gap_start++] = ' ';
    g_ed.lines_dirty = 1;
}

void lc_editor_backspace(void) {
    lc_editor_init();
    if (g_ed.gap_start == 0) return;
    ed_save_undo();
    /* Remove one UTF-8 codepoint backward. */
    g_ed.gap_start--;
    while (g_ed.gap_start > 0 && (((unsigned char)g_ed.buf[g_ed.gap_start] & 0xC0) == 0x80)) {
        g_ed.gap_start--;
    }
    g_ed.lines_dirty = 1;
}

void lc_editor_delete(void) {
    lc_editor_init();
    if (g_ed.gap_end >= g_ed.cap) return;
    ed_save_undo();
    g_ed.gap_end++;
    while (g_ed.gap_end < g_ed.cap && (((unsigned char)g_ed.buf[g_ed.gap_end] & 0xC0) == 0x80)) {
        g_ed.gap_end++;
    }
    g_ed.lines_dirty = 1;
}

int lc_editor_line_count(void) {
    lc_editor_init();
    ed_rebuild_lines();
    return g_ed.line_count;
}

static size_t ed_line_end_byte(int line) {
    ed_rebuild_lines();
    if (line + 1 < g_ed.line_count) return g_ed.line_starts[line + 1] - 1;
    return ed_len();
}

int lc_editor_cursor_line(void) {
    lc_editor_init();
    ed_rebuild_lines();
    size_t cur = g_ed.gap_start;
    int lo = 0, hi = g_ed.line_count - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (g_ed.line_starts[mid] <= cur) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

int lc_editor_cursor_col(void) {
    int line = lc_editor_cursor_line();
    size_t start = g_ed.line_starts[line];
    int col = 0;
    for (size_t i = start; i < g_ed.gap_start; ++i) {
        if (((unsigned char)ed_at(i) & 0xC0) != 0x80) col++;
    }
    return col;
}

static size_t ed_pos_for_line_col(int line, int col) {
    ed_rebuild_lines();
    if (line < 0) line = 0;
    if (line >= g_ed.line_count) line = g_ed.line_count - 1;
    size_t start = g_ed.line_starts[line];
    size_t end = ed_line_end_byte(line);
    int c = 0;
    size_t i = start;
    while (i < end && c < col) {
        i++;
        while (i < end && (((unsigned char)ed_at(i) & 0xC0) == 0x80)) i++;
        c++;
    }
    return i;
}

void lc_editor_set_cursor_line_col(int line, int col) {
    lc_editor_init();
    ed_move_gap(ed_pos_for_line_col(line, col));
}

void lc_editor_move_cursor(int dx, int dy) {
    lc_editor_init();
    ed_rebuild_lines();
    if (dy != 0) {
        int line = lc_editor_cursor_line() + dy;
        int col = lc_editor_cursor_col();
        ed_move_gap(ed_pos_for_line_col(line, col));
    }
    if (dx < 0) {
        for (int s = 0; s < -dx && g_ed.gap_start > 0; ++s) {
            size_t p = g_ed.gap_start - 1;
            while (p > 0 && (((unsigned char)ed_at(p) & 0xC0) == 0x80)) p--;
            ed_move_gap(p);
        }
    } else if (dx > 0) {
        size_t total = ed_len();
        for (int s = 0; s < dx && g_ed.gap_start < total; ++s) {
            size_t p = g_ed.gap_start + 1;
            while (p < total && (((unsigned char)ed_at(p) & 0xC0) == 0x80)) p++;
            ed_move_gap(p);
        }
    }
}

int lc_editor_get_line(int line, char *out, int cap) {
    lc_editor_init();
    ed_rebuild_lines();
    if (line < 0 || line >= g_ed.line_count || !out || cap <= 0) return 0;
    size_t start = g_ed.line_starts[line];
    size_t end = ed_line_end_byte(line);
    size_t n = end > start ? end - start : 0;
    if ((int)n >= cap) n = (size_t)(cap - 1);
    for (size_t i = 0; i < n; ++i) out[i] = ed_at(start + i);
    out[n] = '\0';
    return (int)n;
}

int lc_editor_undo(void) {
    if (g_ed.undo_len <= 0) return 0;
    ed_push_snap(g_ed.redo_stack, &g_ed.redo_len);
    LcEditorSnap snap = g_ed.undo_stack[--g_ed.undo_len];
    ed_load_raw(snap.text, snap.len, snap.cursor);
    free(snap.text);
    return 1;
}

int lc_editor_redo(void) {
    if (g_ed.redo_len <= 0) return 0;
    ed_push_snap(g_ed.undo_stack, &g_ed.undo_len);
    LcEditorSnap snap = g_ed.redo_stack[--g_ed.redo_len];
    ed_load_raw(snap.text, snap.len, snap.cursor);
    free(snap.text);
    return 1;
}

static int ed_match_word(const char *word, size_t len, const char *const *list) {
    for (int i = 0; list[i]; ++i) {
        if (strlen(list[i]) == len && memcmp(word, list[i], len) == 0) return 1;
    }
    return 0;
}

int lc_editor_tokenize_line(int line, int *out_triplets, int max_triplets) {
    static const char *const keywords[] = {
        "and", "break", "class", "do", "elif", "else", "elseif", "end",
        "false", "for", "func", "function", "global", "goto", "if", "in",
        "local", "nil", "not", "or", "repeat", "return", "struct", "then",
        "true", "until", "while", NULL
    };
    static const char *const types[] = { "int", "float", "str", "bool", NULL };
    static const char *const builtins[] = {
        "assert", "clear_screen", "draw_circle", "draw_rect", "draw_sprite",
        "draw_text", "engine", "ipairs", "load_sprite", "math", "os", "pairs",
        "pcall", "print", "require", "scene_reset", "self", "string", "table",
        "tonumber", "tostring", "type", NULL
    };

    char buf[4096];
    int bytes = lc_editor_get_line(line, buf, sizeof(buf));
    int count = 0;
    int col = 0;
    int i = 0;

    while (i < bytes && count < max_triplets) {
        unsigned char c = (unsigned char)buf[i];
        if (c == ' ' || c == '\t') {
            i++;
            col++;
            continue;
        }
        /* Comment: -- or # (when followed by space or start, wait: #t is length op in Lua; -- is comment) */
        if (c == '-' && i + 1 < bytes && buf[i + 1] == '-') {
            int rem_chars = 0;
            for (int j = i; j < bytes; ++j) if (((unsigned char)buf[j] & 0xC0) != 0x80) rem_chars++;
            out_triplets[count * 3 + 0] = col;
            out_triplets[count * 3 + 1] = rem_chars;
            out_triplets[count * 3 + 2] = TOK_COMMENT;
            count++;
            break;
        }
        /* String literal */
        if (c == '"' || c == '\'') {
            char q = (char)c;
            int start_col = col;
            int len_chars = 1;
            i++; col++;
            while (i < bytes) {
                if (buf[i] == '\\' && i + 1 < bytes) {
                    i += 2; col += 2; len_chars += 2;
                    continue;
                }
                char ch = buf[i];
                if (((unsigned char)ch & 0xC0) != 0x80) { col++; len_chars++; }
                i++;
                if (ch == q) break;
            }
            out_triplets[count * 3 + 0] = start_col;
            out_triplets[count * 3 + 1] = len_chars;
            out_triplets[count * 3 + 2] = TOK_STRING;
            count++;
            continue;
        }
        /* Number literal */
        if (isdigit(c) || (c == '.' && i + 1 < bytes && isdigit((unsigned char)buf[i + 1]))) {
            int start_col = col;
            int start_i = i;
            while (i < bytes && (isalnum((unsigned char)buf[i]) || buf[i] == '.' || buf[i] == '_')) i++;
            int len_chars = i - start_i;
            col += len_chars;
            out_triplets[count * 3 + 0] = start_col;
            out_triplets[count * 3 + 1] = len_chars;
            out_triplets[count * 3 + 2] = TOK_NUMBER;
            count++;
            continue;
        }
        /* Identifier / keyword / type / builtin */
        if (isalpha(c) || c == '_') {
            int start_col = col;
            int start_i = i;
            while (i < bytes && (isalnum((unsigned char)buf[i]) || buf[i] == '_')) i++;
            size_t wlen = (size_t)(i - start_i);
            int kind = TOK_PLAIN;
            if (ed_match_word(buf + start_i, wlen, keywords)) kind = TOK_KEYWORD;
            else if (ed_match_word(buf + start_i, wlen, types)) kind = TOK_TYPE;
            else if (ed_match_word(buf + start_i, wlen, builtins)) kind = TOK_BUILTIN;
            col += (int)wlen;
            if (kind != TOK_PLAIN) {
                out_triplets[count * 3 + 0] = start_col;
                out_triplets[count * 3 + 1] = (int)wlen;
                out_triplets[count * 3 + 2] = kind;
                count++;
            }
            continue;
        }
        /* Operators */
        if (strchr("+-*/%=<>~:#.,()[]{}", c)) {
            out_triplets[count * 3 + 0] = col;
            out_triplets[count * 3 + 1] = 1;
            out_triplets[count * 3 + 2] = TOK_OPERATOR;
            count++;
            i++; col++;
            continue;
        }
        /* Other UTF-8 char */
        i++;
        while (i < bytes && (((unsigned char)buf[i] & 0xC0) == 0x80)) i++;
        col++;
    }
    return count;
}
