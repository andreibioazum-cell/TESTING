#ifndef LUAC_RUNTIME_H
#define LUAC_RUNTIME_H

/* LuaC runtime: bump-pointer Arena Allocator, dense structs/classes, native values, and C draw commands. */
#if defined(__APPLE__) && !defined(_WIN32)
/* Xcode 26 hides ucontext and RTLD_DEFAULT behind these feature macros when
   the runtime requests POSIX.1-2008 below. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <ctype.h>
#include <inttypes.h>
#ifndef _WIN32
#ifdef __ANDROID__
#include <pthread.h>
#else
#include <ucontext.h>
#endif
#endif
#include <errno.h>
#include <math.h>
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <io.h>
#define LC_INVALID_SOCKET INVALID_SOCKET
#else
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <arpa/inet.h>
#define LC_INVALID_SOCKET (-1)
#endif

typedef struct LcContext LcContext;
typedef struct LcObject LcObject;
typedef struct LcString LcString;
typedef struct LcTable LcTable;
typedef struct LcStruct LcStruct;
typedef struct LcStructDesc LcStructDesc;
typedef struct LcFunction LcFunction;
typedef struct LcTuple LcTuple;
typedef struct LcCell LcCell;
typedef struct LcFile LcFile;
typedef struct LcSocket LcSocket;
typedef struct LcAudio LcAudio;
typedef struct LcCoroutine LcCoroutine;
typedef struct LcFFILibrary LcFFILibrary;
typedef struct LcWebsocket LcWebsocket;

typedef enum {
    LC_NIL = 0, LC_BOOL, LC_NUMBER, LC_INTEGER, LC_STRING, LC_TABLE, LC_FUNCTION, LC_TUPLE, LC_FILE, LC_SOCKET, LC_AUDIO, LC_COROUTINE, LC_FFI, LC_WEBSOCKET, LC_STRUCT, LC_CELL
} LcType;

typedef enum {
    LC_VTYPE_ANY = 0,
    LC_VTYPE_INT,
    LC_VTYPE_FLOAT,
    LC_VTYPE_STR,
    LC_VTYPE_BOOL
} LcValueType;

typedef struct LcValue {
    LcType type;
    union {
        int boolean;
        double number;
        int64_t integer;
        LcString *string;
        LcTable *table;
        LcStruct *strukt;
        LcFunction *function;
        LcTuple *tuple;
        LcCell *cell;
        LcFile *file;
        LcSocket *socket;
        LcAudio *audio;
        LcCoroutine *coroutine;
        LcFFILibrary *ffi;
        LcWebsocket *websocket;
    } as;
} LcValue;

typedef LcValue (*LcNative)(LcContext *, LcFunction *, int, LcValue *);

enum {
    LC_GC_WHITE0 = 0x01,
    LC_GC_WHITE1 = 0x02,
    LC_GC_WHITES = 0x03,
    LC_GC_BLACK  = 0x04,
    LC_GC_FIXED  = 0x08
};

typedef enum {
    LC_GCS_PAUSE = 0,
    LC_GCS_PROPAGATE,
    LC_GCS_ATOMIC,
    LC_GCS_SWEEP
} LcGcState;

struct LcObject {
    uint8_t kind;
    uint8_t marked;
    uint16_t gc_flags;
    uint32_t obj_size;
    LcObject *next;
    LcObject *gc_list;
};

typedef struct LcGcFrame {
    struct LcGcFrame *prev;
    LcValue *slots;
    int slot_count;
    LcCell **cells;
    int cell_count;
} LcGcFrame;

struct LcCoroutine {
    LcObject object;
    LcValue function, resume_args, yielded, result;
    void *stack;
    size_t stack_size;
    int state, started;
    LcGcFrame *saved_frames;
    LcGcFrame *caller_frames;
#ifdef _WIN32
    void *fiber, *caller_fiber;
#elif defined(__ANDROID__)
    LcContext *owner;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int thread_created, turn, cancel;
#else
    ucontext_t context, caller;
#endif
};

struct LcString {
    LcObject object;
    size_t length;
    char data[];
};

typedef struct {
    LcValue key;
    LcValue value;
    int used;
} LcEntry;

struct LcTable {
    LcObject object;
    LcEntry *entries;
    size_t length;
    size_t capacity;
    LcValue metatable;
};

struct LcFunction {
    LcObject object;
    LcNative native;
    LcCell **upvalues;
    int upvalue_count;
    const char *name;
    LcValue bound_self;
    int has_bound_self;
    LcStructDesc *struct_desc;
    void *jit_proto;
};

typedef struct {
    const char *name;
    int vtype;
    LcValue default_value;
} LcStructField;

struct LcStructDesc {
    const char *name;
    LcStructDesc *base;
    LcStructField *fields;
    int field_count;
    const char **method_names;
    LcValue *methods;
    int method_count;
    LcStructDesc *next;
};

struct LcStruct {
    LcObject object;
    LcStructDesc *desc;
    LcValue fields[];
};

struct LcFile {
    LcObject object;
    FILE *handle;
    int closed;
    int standard;
    int pipe;
};

struct LcSocket {
    LcObject object;
    intptr_t handle;
    int closed;
    int type;
    int timeout_ms;
};

struct LcAudio {
    LcObject object;
    int channels;
    int sample_rate;
    size_t frames;
    size_t cursor;
    void *device;
    float *samples; /* interleaved, normalized to [-1, 1] */
};

struct LcTuple {
    LcObject object;
    int count;
    LcValue items[];
};

struct LcCell {
    LcObject object;
    LcValue value;
};

#define LC_SLAB_SIZE (64u * 1024u)

typedef struct LcSlab {
    struct LcSlab *next;
    size_t used;
    char data[LC_SLAB_SIZE];
} LcSlab;

typedef struct {
    int kind; /* 0 = sprite, 1 = rect, 2 = circle, 3 = text, 4 = line, 5 = rect_outline, 6 = circle_outline, 7 = round_rect */
    int sprite_id;
    double x, y, w, h, rot;
    double r, g, b, a;
    double extra;
    int align; /* 0 = left, 1 = center, 2 = right */
    char *text;
    char *font;
} LcDrawCmd;

typedef struct {
    int id;
    char *path;
    int width, height;
} LcSpriteAsset;

struct LcContext {
    /* Lua-style incremental tri-color GC + size-classed slab allocator. */
    LcObject *gc_all;
    LcObject **gc_sweep;
    LcObject *gc_gray;
    LcObject *gc_gray_again;
    LcObject *gc_weak_v;
    LcObject *gc_weak_k;
    LcObject *gc_weak_kv;
    LcGcFrame *gc_frames;
    LcValue *c_roots;
    int c_root_count, c_root_cap;
    LcSlab *slabs;
    void *cell_free;
    void *table_free;
    void *fn_free;
    size_t gc_bytes;
    size_t gc_estimate;
    size_t gc_threshold;
    intptr_t gc_debt;
    int gc_state;
    uint8_t gc_current_white;
    int gc_running;
    int gc_pause;
    int gc_stepmul;
    LcStructDesc *struct_descs;

    LcCoroutine *active_coroutine;
    LcValue globals;
    LcValue string_library; /* Context-owned default __index for strings. */
    LcValue file_methods;
    LcValue socket_methods;
    LcValue audio_methods;
    LcValue websocket_methods;
    LcValue input, output, error_output;
    int socket_initialized;
    char tls_ca_file[4096];
    int tls_verify;
    int error;
    int line;
    const char *source;
    char message[1024];
    /* Direct C 2D draw commands & sprite registry. */
    LcDrawCmd *draw_cmds;
    int draw_count, draw_cap;
    LcSpriteAsset *sprites;
    int sprite_count, sprite_cap;
    double bg_r, bg_g, bg_b;
    int has_bg;
    int screen_w, screen_h;
    char *current_font;
    int text_align;
    /* Optional embedder-owned print capture (Android project runner). */
    int warnings_enabled;
    int capture_output;
    char *captured_output;
    size_t captured_length, captured_capacity;
};

static LcValue lc_nil(void) { LcValue v; memset(&v, 0, sizeof(v)); return v; }
static LcValue lc_bool(int b) { LcValue v = lc_nil(); v.type = LC_BOOL; v.as.boolean = !!b; return v; }
static LcValue lc_number(double n) { LcValue v = lc_nil(); v.type = LC_NUMBER; v.as.number = n; return v; }
static LcValue lc_integer(int64_t n) { LcValue v = lc_nil(); v.type = LC_INTEGER; v.as.integer = n; return v; }
static int lc_is_numeric(LcValue v) { return v.type == LC_NUMBER || v.type == LC_INTEGER || v.type == LC_STRING; }
static int lc_truthy(LcValue v) { return v.type != LC_NIL && !(v.type == LC_BOOL && !v.as.boolean); }
static int lc_is_multi(LcValue v) { return v.type == LC_TUPLE; }

static void lc_error(LcContext *ctx, const char *fmt, ...) {
    va_list ap;
    if (ctx->error) return;
    ctx->error = 1;
    va_start(ap, fmt);
    vsnprintf(ctx->message, sizeof(ctx->message), fmt, ap);
    va_end(ap);
}

#ifdef __ANDROID__
static void lc_android_coroutine_dispose(LcCoroutine *coroutine);
#endif
static void lc_ffi_dispose(LcFFILibrary *library);
static void lc_websocket_dispose(LcWebsocket *websocket);
static int lc_websocket_is_closed(LcWebsocket *websocket);
static LcValue lc_websocket_socket(LcWebsocket *websocket);
static void lc_websocket_clear_socket(LcWebsocket *websocket);
static void lc_audio_dispose(LcAudio *audio);
static void lc_socket_close_handle(LcSocket *socket) {
    if (!socket || socket->closed) return;
#ifdef _WIN32
    closesocket((SOCKET)socket->handle);
#else
    close((int)socket->handle);
#endif
    socket->closed = 1;
    socket->handle = LC_INVALID_SOCKET;
}

static void *lc_slab_alloc(LcContext *ctx, void **free_list, size_t size) {
    size_t aligned = (size + 7u) & ~(size_t)7u;
    if (aligned < sizeof(void *)) aligned = sizeof(void *);
    if (*free_list) {
        void *p = *free_list;
        *free_list = *(void **)p;
        memset(p, 0, aligned);
        return p;
    }
    if (!ctx->slabs || ctx->slabs->used + aligned > LC_SLAB_SIZE) {
        LcSlab *ns = (LcSlab *)malloc(sizeof(*ns));
        if (!ns) { lc_error(ctx, "out of memory"); return NULL; }
        ns->used = 0;
        ns->next = ctx->slabs;
        ctx->slabs = ns;
    }
    {
        void *p = ctx->slabs->data + ctx->slabs->used;
        ctx->slabs->used += aligned;
        memset(p, 0, aligned);
        return p;
    }
}

static void *lc_alloc(LcContext *ctx, size_t size, int kind) {
    LcObject *o;
    if (kind == LC_CELL) o = (LcObject *)lc_slab_alloc(ctx, &ctx->cell_free, size);
    else if (kind == LC_TABLE) o = (LcObject *)lc_slab_alloc(ctx, &ctx->table_free, size);
    else if (kind == LC_FUNCTION) o = (LcObject *)lc_slab_alloc(ctx, &ctx->fn_free, size);
    else {
        o = (LcObject *)calloc(1, size);
        if (!o) lc_error(ctx, "out of memory");
    }
    if (!o) return NULL;
    o->kind = (uint8_t)kind;
    o->marked = ctx->gc_current_white;
    o->gc_flags = 0;
    o->obj_size = (uint32_t)size;
    o->next = ctx->gc_all;
    ctx->gc_all = o;
    ctx->gc_bytes += size;
    ctx->gc_debt += (intptr_t)size;
    return o;
}

static LcObject *lc_value_object(LcValue v) {
    switch (v.type) {
        case LC_STRING: return (LcObject *)v.as.string;
        case LC_TABLE: return (LcObject *)v.as.table;
        case LC_STRUCT: return (LcObject *)v.as.strukt;
        case LC_FUNCTION: return (LcObject *)v.as.function;
        case LC_TUPLE: return (LcObject *)v.as.tuple;
        case LC_CELL: return (LcObject *)v.as.cell;
        case LC_FILE: return (LcObject *)v.as.file;
        case LC_SOCKET: return (LcObject *)v.as.socket;
        case LC_AUDIO: return (LcObject *)v.as.audio;
        case LC_COROUTINE: return (LcObject *)v.as.coroutine;
        case LC_FFI: return (LcObject *)v.as.ffi;
        case LC_WEBSOCKET: return (LcObject *)v.as.websocket;
        default: return NULL;
    }
}

/* Tri-color incremental mark-and-sweep GC (zero refcounting overhead). */
static LcValue lc_retain(LcValue v) { return v; }
static void lc_release(LcContext *ctx, LcValue v) { (void)ctx; (void)v; }
static void lc_cell_release(LcContext *ctx, LcCell *cell) { (void)ctx; (void)cell; }

static void lc_gc_mark_obj(LcContext *ctx, LcObject *o) {
    if (!o || !(o->marked & LC_GC_WHITES)) return;
    o->marked &= (uint8_t)~LC_GC_WHITES;
    o->gc_list = ctx->gc_gray;
    ctx->gc_gray = o;
}

static void lc_gc_mark_value(LcContext *ctx, LcValue v) {
    LcObject *o = lc_value_object(v);
    if (o) lc_gc_mark_obj(ctx, o);
}

static void lc_gc_barrier(LcContext *ctx, LcObject *parent, LcValue child) {
    if (ctx->gc_state == LC_GCS_PROPAGATE && parent && (parent->marked & LC_GC_BLACK)) {
        LcObject *c = lc_value_object(child);
        if (c && (c->marked & LC_GC_WHITES)) lc_gc_mark_obj(ctx, c);
    }
}

static void lc_gc_barrier_back(LcContext *ctx, LcObject *tbl) {
    if (ctx->gc_state == LC_GCS_PROPAGATE && tbl && (tbl->marked & LC_GC_BLACK)) {
        tbl->marked &= (uint8_t)~LC_GC_BLACK;
        tbl->gc_list = ctx->gc_gray_again;
        ctx->gc_gray_again = tbl;
    }
}

static int lc_gc_push_root(LcContext *ctx, LcValue v) {
    if (ctx->c_root_count >= ctx->c_root_cap) {
        int cap = ctx->c_root_cap ? ctx->c_root_cap * 2 : 32;
        LcValue *p = (LcValue *)realloc(ctx->c_roots, (size_t)cap * sizeof(*p));
        if (!p) return ctx->c_root_count;
        ctx->c_roots = p;
        ctx->c_root_cap = cap;
    }
    ctx->c_roots[ctx->c_root_count++] = v;
    return ctx->c_root_count - 1;
}

static void lc_gc_step(LcContext *ctx);
static void lc_gc_check(LcContext *ctx) {
    if (ctx->gc_running && ctx->gc_debt > 0) lc_gc_step(ctx);
}

static LcValue lc_string_n(LcContext *ctx, const char *s, size_t n) {
    LcValue v = lc_nil();
    LcString *str = (LcString *)lc_alloc(ctx, sizeof(*str) + n + 1, LC_STRING);
    if (!str) return v;
    str->length = n;
    if (n) memcpy(str->data, s, n);
    str->data[n] = '\0';
    v.type = LC_STRING;
    v.as.string = str;
    return v;
}

static LcValue lc_string(LcContext *ctx, const char *s) { return lc_string_n(ctx, s, strlen(s)); }

static LcValue lc_table(LcContext *ctx) {
    LcValue v = lc_nil();
    LcTable *t = (LcTable *)lc_alloc(ctx, sizeof(*t), LC_TABLE);
    if (!t) return v;
    v.type = LC_TABLE;
    v.as.table = t;
    return v;
}

static LcCell *lc_cell_new(LcContext *ctx, LcValue value) {
    LcCell *cell = (LcCell *)lc_alloc(ctx, sizeof(*cell), LC_CELL);
    if (!cell) return NULL;
    cell->value = value;
    return cell;
}

static void lc_cell_replace(LcContext *ctx, LcCell **slot, LcValue value) {
    *slot = lc_cell_new(ctx, value);
}

static void lc_cell_set(LcContext *ctx, LcCell *cell, LcValue value) {
    if (!cell) { lc_error(ctx, "invalid local variable"); return; }
    cell->value = value;
    lc_gc_barrier(ctx, &cell->object, value);
}

static LcValue lc_cell_get(LcCell *cell) { return cell ? cell->value : lc_nil(); }

static LcValue lc_first_value(LcValue v);
static LcValue lc_struct_get(LcContext *ctx, LcStruct *s, LcValue key, int bind_method);
static void lc_struct_set(LcContext *ctx, LcStruct *s, LcValue key, LcValue value);

static LcValue lc_check_type(LcContext *ctx, LcValue v, int vtype, const char *name) {
    v = lc_first_value(v);
    if (vtype == LC_VTYPE_ANY || v.type == LC_NIL) return v;
    if (vtype == LC_VTYPE_INT) {
        if (v.type == LC_INTEGER) return v;
        if (v.type == LC_NUMBER && isfinite(v.as.number) && floor(v.as.number) == v.as.number)
            return lc_integer((int64_t)v.as.number);
        lc_error(ctx, "type error: '%s' requires int", name ? name : "variable");
        return lc_integer(0);
    }
    if (vtype == LC_VTYPE_FLOAT) {
        if (v.type == LC_NUMBER) return v;
        if (v.type == LC_INTEGER) return lc_number((double)v.as.integer);
        lc_error(ctx, "type error: '%s' requires float", name ? name : "variable");
        return lc_number(0);
    }
    if (vtype == LC_VTYPE_STR) {
        if (v.type == LC_STRING) return v;
        lc_error(ctx, "type error: '%s' requires str", name ? name : "variable");
        return lc_string(ctx, "");
    }
    if (vtype == LC_VTYPE_BOOL) {
        if (v.type == LC_BOOL) return v;
        lc_error(ctx, "type error: '%s' requires bool", name ? name : "variable");
        return lc_bool(0);
    }
    return v;
}

static LcValue lc_tuple_new(LcContext *ctx, int count, LcValue *values, int consume) {
    LcValue v = lc_nil();
    LcTuple *t;
    int i;
    if (count < 0) count = 0;
    t = (LcTuple *)lc_alloc(ctx, sizeof(*t) + sizeof(LcValue) * (size_t)count, LC_TUPLE);
    if (!t) return v;
    t->count = count;
    for (i = 0; i < count; ++i) t->items[i] = consume ? values[i] : lc_retain(values[i]);
    v.type = LC_TUPLE;
    v.as.tuple = t;
    return v;
}

static LcValue lc_first_value(LcValue v) {
    if (v.type == LC_TUPLE) return v.as.tuple->count ? v.as.tuple->items[0] : lc_nil();
    return v;
}

static LcValue lc_first(LcValue v) { return lc_retain(lc_first_value(v)); }

static LcValue lc_tuple_list(LcContext *ctx, int count, LcValue *values) {
    if (count > 0 && values[count - 1].type == LC_TUPLE) {
        LcTuple *last = values[count - 1].as.tuple;
        int n = count - 1 + last->count, i;
        LcValue *items = (LcValue *)malloc(sizeof(LcValue) * (size_t)(n ? n : 1));
        LcValue result;
        if (!items) { lc_error(ctx, "out of memory"); return lc_nil(); }
        for (i = 0; i < count - 1; ++i) items[i] = lc_first(values[i]);
        for (i = 0; i < last->count; ++i) items[count - 1 + i] = lc_retain(last->items[i]);
        result = lc_tuple_new(ctx, n, items, 1);
        free(items);
        return result;
    }
    return lc_tuple_new(ctx, count, values, 0);
}

/* Return an owned nth result without consuming the tuple. */
static LcValue lc_nth(LcValue v, int index) {
    if (v.type == LC_TUPLE) {
        if (index < 0 || index >= v.as.tuple->count) return lc_nil();
        return lc_retain(v.as.tuple->items[index]);
    }
    return index == 0 ? lc_retain(v) : lc_nil();
}

static int lc_value_equal(LcValue a, LcValue b) {
    a = lc_first_value(a); b = lc_first_value(b);
    if (a.type != b.type) {
        if ((a.type == LC_NUMBER || a.type == LC_INTEGER) && (b.type == LC_NUMBER || b.type == LC_INTEGER)) {
            if (a.type == LC_INTEGER && b.type == LC_INTEGER) return a.as.integer == b.as.integer;
            if (a.type == LC_NUMBER && b.type == LC_NUMBER) return a.as.number == b.as.number;
            { LcValue iv = a.type == LC_INTEGER ? a : b, dv = a.type == LC_NUMBER ? a : b;
              if (!isfinite(dv.as.number) || floor(dv.as.number) != dv.as.number || dv.as.number < -9223372036854775808.0 || dv.as.number >= 9223372036854775808.0) return 0;
              return iv.as.integer == (int64_t)dv.as.number; }
        }
        return 0;
    }
    switch (a.type) {
        case LC_NIL: return 1;
        case LC_BOOL: return a.as.boolean == b.as.boolean;
        case LC_NUMBER: return a.as.number == b.as.number;
        case LC_INTEGER: return a.as.integer == b.as.integer;
        case LC_STRING:
            return a.as.string->length == b.as.string->length &&
                !memcmp(a.as.string->data, b.as.string->data, a.as.string->length);
        case LC_TABLE: return a.as.table == b.as.table;
        case LC_STRUCT: return a.as.strukt == b.as.strukt;
        case LC_FUNCTION: return a.as.function == b.as.function;
        case LC_FILE: return a.as.file == b.as.file;
        case LC_SOCKET: return a.as.socket == b.as.socket;
        case LC_AUDIO: return a.as.audio == b.as.audio;
        case LC_COROUTINE: return a.as.coroutine == b.as.coroutine;
        case LC_FFI: return a.as.ffi == b.as.ffi;
        case LC_WEBSOCKET: return a.as.websocket == b.as.websocket;
        default: return 0;
    }
}

static int lc_table_find(LcTable *t, LcValue key) {
    size_t i;
    key = lc_first_value(key);
    if (key.type == LC_NIL) return -1;
    for (i = 0; i < t->capacity; ++i)
        if (t->entries[i].used && lc_value_equal(t->entries[i].key, key)) return (int)i;
    return -1;
}

static int lc_table_reserve(LcContext *ctx, LcTable *t, size_t needed) {
    LcEntry *p;
    size_t cap;
    if (needed <= t->capacity) return 1;
    cap = t->capacity ? t->capacity * 2 : 8;
    while (cap < needed) cap *= 2;
    p = (LcEntry *)realloc(t->entries, cap * sizeof(*p));
    if (!p) { lc_error(ctx, "out of memory"); return 0; }
    memset(p + t->capacity, 0, (cap - t->capacity) * sizeof(*p));
    ctx->gc_bytes += (cap - t->capacity) * sizeof(*p);
    ctx->gc_debt += (intptr_t)((cap - t->capacity) * sizeof(*p));
    t->entries = p;
    t->capacity = cap;
    return 1;
}

static void lc_table_raw_set(LcContext *ctx, LcValue table, LcValue key, LcValue value) {
    LcTable *t;
    int at, free_slot = -1;
    size_t i;
    table = lc_first_value(table); key = lc_first_value(key); value = lc_first_value(value);
    if (table.type != LC_TABLE) { lc_error(ctx, "attempt to index a %s value", table.type == LC_NIL ? "nil" : "non-table"); return; }
    if (key.type == LC_NIL) { lc_error(ctx, "table index is nil"); return; }
    if (key.type == LC_NUMBER && isnan(key.as.number)) { lc_error(ctx, "table index is NaN"); return; }
    if (key.type == LC_NUMBER && isfinite(key.as.number) && floor(key.as.number) == key.as.number && key.as.number >= -9223372036854775808.0 && key.as.number < 9223372036854775808.0) key = lc_integer((int64_t)key.as.number);
    t = table.as.table;
    at = -1;
    for (i = 0; i < t->capacity; ++i) {
        if (!t->entries[i].used) {
            if (free_slot < 0) free_slot = (int)i;
        } else if (lc_value_equal(t->entries[i].key, key)) {
            at = (int)i;
            break;
        }
    }
    if (value.type == LC_NIL) {
        if (at >= 0) memset(&t->entries[at], 0, sizeof(t->entries[at]));
        return;
    }
    if (at < 0) {
        if (free_slot >= 0) {
            at = free_slot;
            if ((size_t)(at + 1) > t->length) t->length = (size_t)(at + 1);
        } else {
            if (!lc_table_reserve(ctx, t, t->length + 1)) return;
            at = (int)t->length++;
        }
        t->entries[at].used = 1;
        t->entries[at].key = key;
    }
    t->entries[at].value = value;
    lc_gc_barrier_back(ctx, &t->object);
}

static LcValue lc_table_raw_get(LcContext *ctx, LcValue table, LcValue key) {
    int at;
    (void)ctx;
    table = lc_first_value(table); key = lc_first_value(key);
    if (table.type != LC_TABLE) { lc_error(ctx, "attempt to index a non-table value"); return lc_nil(); }
    at = lc_table_find(table.as.table, key);
    return at < 0 ? lc_nil() : lc_retain(table.as.table->entries[at].value);
}

static LcValue lc_call(LcContext *ctx, LcValue fn, int argc, LcValue *argv);

static LcValue lc_metamethod(LcContext *ctx, LcValue value, const char *name) {
    LcValue key, result;
    value = lc_first_value(value);
    if (value.type != LC_TABLE || value.as.table->metatable.type != LC_TABLE) return lc_nil();
    key = lc_string(ctx, name);
    result = lc_table_raw_get(ctx, value.as.table->metatable, key);
    lc_release(ctx, key);
    return result;
}

static LcValue lc_table_get(LcContext *ctx, LcValue table, LcValue key) {
    int depth;
    LcValue current = lc_retain(table);
    for (depth = 0; depth < 100; ++depth) {
        LcValue result, method;
        current = lc_first_value(current);
        if (current.type == LC_STRING && ctx->string_library.type == LC_TABLE) {
            result = lc_table_raw_get(ctx, ctx->string_library, key); lc_release(ctx, current); return result;
        }
        if (current.type == LC_FILE && ctx->file_methods.type == LC_TABLE) {
            result = lc_table_raw_get(ctx, ctx->file_methods, key); lc_release(ctx, current); return result;
        }
        if (current.type == LC_SOCKET && ctx->socket_methods.type == LC_TABLE) {
            result = lc_table_raw_get(ctx, ctx->socket_methods, key); lc_release(ctx, current); return result;
        }
        if (current.type == LC_WEBSOCKET && ctx->websocket_methods.type == LC_TABLE) {
            result = lc_table_raw_get(ctx, ctx->websocket_methods, key); lc_release(ctx, current); return result;
        }
        if (current.type == LC_AUDIO && ctx->audio_methods.type == LC_TABLE) {
            result = lc_table_raw_get(ctx, ctx->audio_methods, key); lc_release(ctx, current); return result;
        }
        if (current.type == LC_STRUCT) {
            return lc_struct_get(ctx, current.as.strukt, key, 1);
        }
        if (current.type != LC_TABLE) break;
        result = lc_table_raw_get(ctx, current, key);
        if (result.type != LC_NIL) { lc_release(ctx, current); return result; }
        lc_release(ctx, result);
        method = lc_metamethod(ctx, current, "__index");
        if (method.type == LC_NIL) { lc_release(ctx, current); return method; }
        if (method.type == LC_FUNCTION) {
            LcValue args[2];
            args[0] = current; args[1] = key;
            result = lc_call(ctx, method, 2, args);
            lc_release(ctx, method); lc_release(ctx, current);
            return result;
        }
        lc_release(ctx, current); current = method;
    }
    lc_release(ctx, current);
    if (depth >= 100) lc_error(ctx, "'__index' chain too long; possible loop");
    else lc_error(ctx, "attempt to index a non-table value");
    return lc_nil();
}

static LcValue lc_method_get(LcContext *ctx, LcValue obj, LcValue key) {
    obj = lc_first_value(obj);
    if (obj.type == LC_STRUCT) return lc_struct_get(ctx, obj.as.strukt, key, 0);
    return lc_table_get(ctx, obj, key);
}

static void lc_table_set(LcContext *ctx, LcValue table, LcValue key, LcValue value) {
    LcValue current = lc_retain(table);
    int depth;
    for (depth = 0; depth < 100; ++depth) {
        LcValue method;
        current = lc_first_value(current);
        if (current.type == LC_STRUCT) {
            lc_struct_set(ctx, current.as.strukt, key, value);
            return;
        }
        if (current.type != LC_TABLE) break;
        if (lc_table_find(current.as.table, key) >= 0) {
            lc_table_raw_set(ctx, current, key, value);
            lc_release(ctx, current); return;
        }
        method = lc_metamethod(ctx, current, "__newindex");
        if (method.type == LC_NIL) {
            lc_table_raw_set(ctx, current, key, value);
            lc_release(ctx, current); return;
        }
        if (method.type == LC_FUNCTION) {
            LcValue args[3], ignored;
            args[0] = current; args[1] = key; args[2] = value;
            ignored = lc_call(ctx, method, 3, args);
            lc_release(ctx, ignored); lc_release(ctx, method); lc_release(ctx, current);
            return;
        }
        lc_release(ctx, current); current = method;
    }
    if (depth >= 100) lc_error(ctx, "'__newindex' chain too long; possible loop");
    else lc_table_raw_set(ctx, current, key, value);
    lc_release(ctx, current);
}

static void lc_table_add(LcContext *ctx, LcValue table, int index, LcValue value) {
    if (value.type == LC_TUPLE) {
        int i;
        for (i = 0; i < value.as.tuple->count; ++i)
            lc_table_set(ctx, table, lc_number(index + i), value.as.tuple->items[i]);
    } else {
        lc_table_set(ctx, table, lc_number(index), value);
    }
}

static int lc_table_length(LcValue table) {
    int n = 0;
    if (table.type != LC_TABLE) return 0;
    for (;;) {
        LcValue k = lc_number((double)n + 1);
        if (lc_table_find(table.as.table, k) < 0) break;
        ++n;
    }
    return n;
}

static LcValue lc_function_new(LcContext *ctx, LcNative native, int n, LcCell **upvalues, const char *name) {
    LcValue v = lc_nil();
    LcFunction *f = (LcFunction *)lc_alloc(ctx, sizeof(*f), LC_FUNCTION);
    int i;
    if (!f) return v;
    f->native = native;
    f->name = name;
    f->upvalue_count = n;
    if (n) {
        f->upvalues = (LcCell **)malloc(sizeof(LcCell *) * (size_t)n);
        if (!f->upvalues) { lc_error(ctx, "out of memory"); return lc_nil(); }
        for (i = 0; i < n; ++i) f->upvalues[i] = upvalues[i];
    }
    v.type = LC_FUNCTION;
    v.as.function = f;
    return v;
}

static LcValue lc_function_bound(LcContext *ctx, LcFunction *fn, LcValue self) {
    LcValue v = lc_nil();
    LcFunction *f;
    if (!fn) return v;
    f = (LcFunction *)lc_alloc(ctx, sizeof(*f), LC_FUNCTION);
    if (!f) return v;
    f->native = fn->native;
    f->name = fn->name;
    f->upvalue_count = fn->upvalue_count;
    f->struct_desc = fn->struct_desc;
    f->jit_proto = fn->jit_proto;
    if (fn->upvalue_count > 0 && fn->upvalues) {
        f->upvalues = (LcCell **)malloc(sizeof(LcCell *) * (size_t)fn->upvalue_count);
        if (!f->upvalues) { lc_error(ctx, "out of memory"); return lc_nil(); }
        memcpy(f->upvalues, fn->upvalues, sizeof(LcCell *) * (size_t)fn->upvalue_count);
    }
    f->bound_self = self;
    f->has_bound_self = 1;
    v.type = LC_FUNCTION;
    v.as.function = f;
    return v;
}

static LcStructDesc *lc_struct_desc_new(LcContext *ctx, const char *name, LcStructDesc *base, int own_fields, int own_methods) {
    LcStructDesc *desc = (LcStructDesc *)calloc(1, sizeof(*desc));
    int base_fields = base ? base->field_count : 0;
    int base_methods = base ? base->method_count : 0;
    if (!desc) { lc_error(ctx, "out of memory"); return NULL; }
    desc->name = name;
    desc->base = base;
    desc->field_count = base_fields + own_fields;
    desc->method_count = base_methods + own_methods;
    if (desc->field_count > 0) {
        desc->fields = (LcStructField *)calloc((size_t)desc->field_count, sizeof(LcStructField));
        if (base_fields > 0) memcpy(desc->fields, base->fields, sizeof(LcStructField) * (size_t)base_fields);
    }
    if (desc->method_count > 0) {
        desc->method_names = (const char **)calloc((size_t)desc->method_count, sizeof(const char *));
        desc->methods = (LcValue *)calloc((size_t)desc->method_count, sizeof(LcValue));
        if (base_methods > 0) {
            memcpy(desc->method_names, base->method_names, sizeof(const char *) * (size_t)base_methods);
            memcpy(desc->methods, base->methods, sizeof(LcValue) * (size_t)base_methods);
        }
    }
    desc->next = ctx->struct_descs;
    ctx->struct_descs = desc;
    return desc;
}

static void lc_struct_desc_add_field(LcStructDesc *desc, int index, const char *name, int vtype, LcValue def) {
    int i;
    if (!desc) return;
    /* Allow subclass to override default field value if same name exists. */
    for (i = 0; i < index; ++i) {
        if (desc->fields[i].name && !strcmp(desc->fields[i].name, name)) {
            desc->fields[i].vtype = vtype;
            desc->fields[i].default_value = lc_first_value(def);
            return;
        }
    }
    desc->fields[index].name = name;
    desc->fields[index].vtype = vtype;
    desc->fields[index].default_value = lc_first_value(def);
}

static void lc_struct_desc_add_method(LcStructDesc *desc, int slot, const char *name, LcValue fn) {
    int i;
    if (!desc) return;
    for (i = 0; i < slot; ++i) {
        if (desc->method_names[i] && !strcmp(desc->method_names[i], name)) {
            desc->methods[i] = fn;
            return;
        }
    }
    desc->method_names[slot] = name;
    desc->methods[slot] = fn;
}

static LcValue lc_struct_new(LcContext *ctx, LcStructDesc *desc, int argc, LcValue *argv) {
    LcValue v = lc_nil();
    LcStruct *s;
    int i, init_idx = -1;
    if (!desc) return v;
    s = (LcStruct *)lc_alloc(ctx, sizeof(*s) + sizeof(LcValue) * (size_t)(desc->field_count > 0 ? desc->field_count : 1), LC_STRUCT);
    if (!s) return v;
    s->desc = desc;
    for (i = 0; i < desc->field_count; ++i) {
        if (desc->fields[i].name) s->fields[i] = desc->fields[i].default_value;
    }
    v.type = LC_STRUCT;
    v.as.strukt = s;
    for (i = 0; i < desc->method_count; ++i) {
        if (desc->method_names[i] && !strcmp(desc->method_names[i], "init")) init_idx = i;
    }
    if (init_idx >= 0 && desc->methods[init_idx].type == LC_FUNCTION) {
        LcValue stack_args[33];
        LcValue *call_args = argc < 32 ? stack_args : (LcValue *)malloc(sizeof(LcValue) * (size_t)(argc + 1));
        if (call_args) {
            call_args[0] = v;
            for (i = 0; i < argc; ++i) call_args[i + 1] = lc_first_value(argv[i]);
            lc_call(ctx, desc->methods[init_idx], argc + 1, call_args);
            if (call_args != stack_args) free(call_args);
        }
    } else if (argc == 1 && lc_first_value(argv[0]).type == LC_TABLE) {
        LcValue tbl = lc_first_value(argv[0]);
        for (i = 0; i < desc->field_count; ++i) {
            if (!desc->fields[i].name) continue;
            LcValue k = lc_string(ctx, desc->fields[i].name);
            int idx = lc_table_find(tbl.as.table, k);
            if (idx >= 0) {
                s->fields[i] = lc_check_type(ctx, tbl.as.table->entries[idx].value, desc->fields[i].vtype, desc->fields[i].name);
            }
        }
    } else if (argc > 0) {
        int fi = 0;
        for (i = 0; i < desc->field_count && fi < argc; ++i) {
            if (!desc->fields[i].name) continue;
            s->fields[i] = lc_check_type(ctx, argv[fi], desc->fields[i].vtype, desc->fields[i].name);
            ++fi;
        }
    }
    return v;
}

static LcValue lc_struct_get(LcContext *ctx, LcStruct *s, LcValue key, int bind_method) {
    const char *name;
    int i;
    key = lc_first_value(key);
    if (!s || !s->desc || key.type != LC_STRING) return lc_nil();
    name = key.as.string->data;
    for (i = s->desc->field_count - 1; i >= 0; --i) {
        if (s->desc->fields[i].name && !strcmp(s->desc->fields[i].name, name))
            return s->fields[i];
    }
    for (i = s->desc->method_count - 1; i >= 0; --i) {
        if (s->desc->method_names[i] && !strcmp(s->desc->method_names[i], name)) {
            LcValue m = s->desc->methods[i];
            if (bind_method && m.type == LC_FUNCTION) {
                LcValue self;
                self.type = LC_STRUCT;
                self.as.strukt = s;
                return lc_function_bound(ctx, m.as.function, self);
            }
            return m;
        }
    }
    return lc_nil();
}

static void lc_struct_set(LcContext *ctx, LcStruct *s, LcValue key, LcValue value) {
    const char *name;
    int i;
    key = lc_first_value(key);
    if (!s || !s->desc || key.type != LC_STRING) {
        lc_error(ctx, "struct field name expected");
        return;
    }
    name = key.as.string->data;
    for (i = s->desc->field_count - 1; i >= 0; --i) {
        if (s->desc->fields[i].name && !strcmp(s->desc->fields[i].name, name)) {
            LcValue checked = lc_check_type(ctx, value, s->desc->fields[i].vtype, name);
            s->fields[i] = checked;
            lc_gc_barrier(ctx, &s->object, checked);
            return;
        }
    }
    lc_error(ctx, "struct '%s' has no field '%s'", s->desc->name, name);
}

static LcValue lc_struct_ctor_call(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    return lc_struct_new(ctx, self->struct_desc, argc, argv);
}

static LcValue lc_struct_register(LcContext *ctx, LcStructDesc *desc) {
    LcValue fn = lc_function_new(ctx, lc_struct_ctor_call, 0, NULL, desc->name);
    LcValue key = lc_string(ctx, desc->name);
    if (fn.type == LC_FUNCTION) fn.as.function->struct_desc = desc;
    lc_table_set(ctx, ctx->globals, key, fn);
    return fn;
}

static LcValue lc_call(LcContext *ctx, LcValue fn, int argc, LcValue *argv) {
    int root_mark = ctx->c_root_count;
    int i;
    fn = lc_first_value(fn);
    if (ctx->error) return lc_nil();
    lc_gc_push_root(ctx, fn);
    for (i = 0; i < argc; ++i) lc_gc_push_root(ctx, argv[i]);
    lc_gc_check(ctx);
    if (fn.type != LC_FUNCTION || !fn.as.function->native) {
        LcValue method = lc_metamethod(ctx, fn, "__call");
        if (method.type == LC_FUNCTION) {
            LcValue *args = (LcValue *)malloc(sizeof(LcValue) * (size_t)(argc + 1));
            LcValue result;
            if (!args) { ctx->c_root_count = root_mark; lc_error(ctx, "out of memory in __call"); return lc_nil(); }
            args[0] = fn;
            if (argc) memcpy(args + 1, argv, sizeof(LcValue) * (size_t)argc);
            result = lc_call(ctx, method, argc + 1, args);
            free(args); ctx->c_root_count = root_mark; return result;
        }
        ctx->c_root_count = root_mark;
        lc_error(ctx, "attempt to call a non-function value");
        return lc_nil();
    }
    if (fn.as.function->has_bound_self) {
        LcValue stack_args[33];
        LcValue *call_args = argc < 32 ? stack_args : (LcValue *)malloc(sizeof(LcValue) * (size_t)(argc + 1));
        LcValue res;
        if (!call_args) { ctx->c_root_count = root_mark; lc_error(ctx, "out of memory"); return lc_nil(); }
        call_args[0] = fn.as.function->bound_self;
        if (argc > 0) memcpy(call_args + 1, argv, sizeof(LcValue) * (size_t)argc);
        res = fn.as.function->native(ctx, fn.as.function, argc + 1, call_args);
        if (call_args != stack_args) free(call_args);
        ctx->c_root_count = root_mark;
        return res;
    }
    {
        LcValue res = fn.as.function->native(ctx, fn.as.function, argc, argv);
        ctx->c_root_count = root_mark;
        return res;
    }
}

/* Lua expands only the final expression of an argument list. */
static LcValue lc_call_list(LcContext *ctx, LcValue fn, int argc, LcValue *argv) {
    LcValue result;
    if (argc > 0 && argv[argc - 1].type == LC_TUPLE) {
        LcTuple *t = argv[argc - 1].as.tuple;
        int n = argc - 1 + t->count, i;
        LcValue *a = (LcValue *)malloc(sizeof(LcValue) * (size_t)(n ? n : 1));
        if (!a) { lc_error(ctx, "out of memory in call"); return lc_nil(); }
        for (i = 0; i < argc - 1; ++i) a[i] = lc_first_value(argv[i]);
        for (i = 0; i < t->count; ++i) a[argc - 1 + i] = t->items[i];
        result = lc_call(ctx, fn, n, a);
        free(a);
        return result;
    }
    return lc_call(ctx, fn, argc, argv);
}

static double lc_to_number(LcContext *ctx, LcValue v, int *ok) {
    char *end;
    v = lc_first_value(v);
    *ok = 1;
    if (v.type == LC_NUMBER) return v.as.number;
    if (v.type == LC_INTEGER) return (double)v.as.integer;
    if (v.type == LC_STRING) {
        double n;
        errno = 0;
        n = strtod(v.as.string->data, &end);
        while (isspace((unsigned char)*end)) ++end;
        if (end != v.as.string->data && !*end && errno != ERANGE) return n;
    }
    *ok = 0;
    lc_error(ctx, "number expected");
    return 0;
}

static int64_t lc_to_integer(LcContext *ctx, LcValue value) {
    int ok = 0;
    double number;
    value = lc_first_value(value);
    if (value.type == LC_INTEGER) return value.as.integer;
    number = lc_to_number(ctx, value, &ok);
    if (!ok || !isfinite(number) || floor(number) != number || number < -9223372036854775808.0 || number >= 9223372036854775808.0) {
        lc_error(ctx, "number has no integer representation");
        return 0;
    }
    return (int64_t)number;
}

static LcValue lc_to_string(LcContext *ctx, LcValue v) {
    char buf[128];
    v = lc_first_value(v);
    switch (v.type) {
        case LC_NIL: return lc_string(ctx, "nil");
        case LC_BOOL: return lc_string(ctx, v.as.boolean ? "true" : "false");
        case LC_NUMBER:
            snprintf(buf, sizeof(buf), "%.14g", v.as.number);
            return lc_string(ctx, buf);
        case LC_INTEGER:
            snprintf(buf, sizeof(buf), "%" PRId64, v.as.integer);
            return lc_string(ctx, buf);
        case LC_STRING: return lc_retain(v);
        case LC_TABLE:
            snprintf(buf, sizeof(buf), "table: %p", (void *)v.as.table);
            return lc_string(ctx, buf);
        case LC_STRUCT:
            snprintf(buf, sizeof(buf), "%s: %p", v.as.strukt->desc ? v.as.strukt->desc->name : "struct", (void *)v.as.strukt);
            return lc_string(ctx, buf);
        case LC_FUNCTION:
            snprintf(buf, sizeof(buf), "function: %p", (void *)v.as.function);
            return lc_string(ctx, buf);
        case LC_FILE:
            if (v.as.file->closed) return lc_string(ctx, "file (closed)");
            snprintf(buf, sizeof(buf), "file (%p)", (void *)v.as.file);
            return lc_string(ctx, buf);
        case LC_AUDIO:
            snprintf(buf, sizeof(buf), "audio: %p", (void *)v.as.audio);
            return lc_string(ctx, buf);
        case LC_SOCKET:
            if (v.as.socket->closed) return lc_string(ctx, "socket (closed)");
            snprintf(buf, sizeof(buf), "socket (%p)", (void *)v.as.socket);
            return lc_string(ctx, buf);
        case LC_COROUTINE:
            snprintf(buf, sizeof(buf), "thread: %p", (void *)v.as.coroutine);
            return lc_string(ctx, buf);
        case LC_FFI:
            snprintf(buf, sizeof(buf), "ffi.library: %p", (void *)v.as.ffi);
            return lc_string(ctx, buf);
        case LC_WEBSOCKET:
            if (lc_websocket_is_closed(v.as.websocket)) return lc_string(ctx, "websocket (closed)");
            snprintf(buf, sizeof(buf), "websocket: %p", (void *)v.as.websocket);
            return lc_string(ctx, buf);
        default: return lc_string(ctx, "nil");
    }
}

static const char *lc_type_name(LcValue v) {
    static const char *names[] = {"nil", "boolean", "number", "number", "string", "table", "function", "tuple", "userdata", "userdata", "userdata", "thread", "userdata", "userdata", "struct"};
    v = lc_first_value(v);
    if (v.type == LC_STRUCT && v.as.strukt && v.as.strukt->desc && v.as.strukt->desc->name)
        return v.as.strukt->desc->name;
    return names[(int)v.type];
}

static LcValue lc_call_metamethod(LcContext *ctx, LcValue a, LcValue b, const char *name) {
    LcValue method = lc_metamethod(ctx, a, name), args[2], result;
    if (method.type == LC_NIL) { lc_release(ctx, method); method = lc_metamethod(ctx, b, name); }
    if (method.type == LC_NIL) return method;
    args[0] = a; args[1] = b;
    result = lc_call(ctx, method, 2, args);
    lc_release(ctx, method);
    return result;
}

static LcValue lc_binary(LcContext *ctx, int op, LcValue a, LcValue b) {
    int ok1, ok2;
    double x, y;
    LcValue r = lc_nil();
    a = lc_first_value(a); b = lc_first_value(b);
    switch (op) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6: {
            static const char *names[] = {"__add", "__sub", "__mul", "__div", "__idiv", "__mod", "__pow"};
            if (!lc_is_numeric(a) || !lc_is_numeric(b)) {
                LcValue meta = lc_call_metamethod(ctx, a, b, names[op]);
                if (meta.type != LC_NIL) return meta;
                lc_release(ctx, meta);
            }
            x = lc_to_number(ctx, a, &ok1); y = lc_to_number(ctx, b, &ok2);
            if (!ok1 || !ok2) return r;
            if (a.type == LC_INTEGER && b.type == LC_INTEGER && op != 3 && op != 6) {
                int64_t ix = a.as.integer, iy = b.as.integer;
                if (op == 0) return lc_integer((int64_t)((uint64_t)ix + (uint64_t)iy));
                if (op == 1) return lc_integer((int64_t)((uint64_t)ix - (uint64_t)iy));
                if (op == 2) return lc_integer((int64_t)((uint64_t)ix * (uint64_t)iy));
                if (iy == 0) { lc_error(ctx, "division by zero"); return r; }
                if (op == 4) { int64_t q, rem; if (ix == INT64_MIN && iy == -1) return lc_integer(INT64_MIN); q = ix / iy; rem = ix % iy; if (rem && ((rem < 0) != (iy < 0))) --q; return lc_integer(q); }
                if (op == 5) { int64_t rem; if (ix == INT64_MIN && iy == -1) return lc_integer(0); rem = ix % iy; if (rem && ((rem < 0) != (iy < 0))) rem += iy; return lc_integer(rem); }
            }
            if (op == 0) return lc_number(x + y);
            if (op == 1) return lc_number(x - y);
            if (op == 2) return lc_number(x * y);
            if (op == 3) { if (y == 0) { lc_error(ctx, "division by zero"); return r; } return lc_number(x / y); }
            if (op == 4) { if (y == 0) { lc_error(ctx, "division by zero"); return r; } return lc_number(floor(x / y)); }
            if (op == 5) { if (y == 0) { lc_error(ctx, "division by zero"); return r; } return lc_number(x - floor(x / y) * y); }
            return lc_number(pow(x, y));
        }
        case 7: case 8: {
            int equal = lc_value_equal(a, b);
            if (!equal && a.type == LC_TABLE && b.type == LC_TABLE) {
                LcValue meta = lc_call_metamethod(ctx, a, b, "__eq");
                if (meta.type != LC_NIL) { equal = lc_truthy(meta); lc_release(ctx, meta); return lc_bool(op == 7 ? equal : !equal); }
                lc_release(ctx, meta);
            }
            return lc_bool(op == 7 ? equal : !equal);
        }
        case 9: case 10: case 11: case 12:
            if ((a.type == LC_NUMBER || a.type == LC_INTEGER) && (b.type == LC_NUMBER || b.type == LC_INTEGER)) {
                long double lx = a.type == LC_INTEGER ? (long double)a.as.integer : (long double)a.as.number;
                long double ly = b.type == LC_INTEGER ? (long double)b.as.integer : (long double)b.as.number;
                if (op == 9) return lc_bool(lx < ly);
                if (op == 10) return lc_bool(lx <= ly);
                if (op == 11) return lc_bool(lx > ly);
                return lc_bool(lx >= ly);
            }
            if (a.type == LC_STRING && b.type == LC_STRING) {
                size_t n = a.as.string->length < b.as.string->length ? a.as.string->length : b.as.string->length;
                int c = memcmp(a.as.string->data, b.as.string->data, n);
                if (!c) c = a.as.string->length < b.as.string->length ? -1 : a.as.string->length > b.as.string->length;
                if (op == 9) return lc_bool(c < 0);
                if (op == 10) return lc_bool(c <= 0);
                if (op == 11) return lc_bool(c > 0);
                return lc_bool(c >= 0);
            }
            {
                LcValue meta = (op == 11 || op == 12)
                    ? lc_call_metamethod(ctx, b, a, op == 11 ? "__lt" : "__le")
                    : lc_call_metamethod(ctx, a, b, op == 9 ? "__lt" : "__le");
                if (meta.type != LC_NIL) {
                    int truth = lc_truthy(meta);
                    lc_release(ctx, meta);
                    return lc_bool(truth);
                }
                lc_release(ctx, meta);
            }
            lc_error(ctx, "attempt to compare incompatible values"); return r;
        case 13: {
            LcValue sa, sb;
            if (!lc_is_numeric(a) || !lc_is_numeric(b)) {
                LcValue meta = lc_call_metamethod(ctx, a, b, "__concat");
                if (meta.type != LC_NIL) return meta;
                lc_release(ctx, meta);
            }
            sa = lc_to_string(ctx, a); sb = lc_to_string(ctx, b);
            size_t na, nb;
            if (ctx->error) { lc_release(ctx, sa); lc_release(ctx, sb); return r; }
            na = sa.as.string->length; nb = sb.as.string->length;
            {
                char *s = (char *)malloc(na + nb);
                if (!s) { lc_error(ctx, "out of memory"); }
                else {
                    memcpy(s, sa.as.string->data, na); memcpy(s + na, sb.as.string->data, nb);
                    r = lc_string_n(ctx, s, na + nb); free(s);
                }
            }
            lc_release(ctx, sa); lc_release(ctx, sb); return r;
        }
        case 16: case 17: case 18: {
            static const char *names[] = {"__band", "__bor", "__bxor"};
            int64_t x64, y64;
            if (!lc_is_numeric(a) || !lc_is_numeric(b)) {
                LcValue meta = lc_call_metamethod(ctx, a, b, names[op - 16]);
                if (meta.type != LC_NIL) return meta;
                lc_release(ctx, meta);
            }
            x64 = lc_to_integer(ctx, a); y64 = lc_to_integer(ctx, b);
            if (ctx->error) return r;
            if (op == 16) return lc_integer(x64 & y64);
            if (op == 17) return lc_integer(x64 | y64);
            return lc_integer(x64 ^ y64);
        }
        case 19: case 20: {
            int64_t x64, count;
            int left = op == 19;
            if (!lc_is_numeric(a) || !lc_is_numeric(b)) {
                LcValue meta = lc_call_metamethod(ctx, a, b, left ? "__shl" : "__shr");
                if (meta.type != LC_NIL) return meta;
                lc_release(ctx, meta);
            }
            x64 = lc_to_integer(ctx, a); count = lc_to_integer(ctx, b);
            if (ctx->error) return r;
            if (count < 0) { left = !left; if (count == INT64_MIN) return lc_integer(0); count = -count; }
            if (count >= 64) return lc_integer(0);
            if (left) return lc_integer((int64_t)((uint64_t)x64 << (unsigned)count));
            return lc_integer(x64 >> (unsigned)count);
        }
        default: return r;
    }
}

static LcValue lc_unary(LcContext *ctx, int op, LcValue a) {
    int ok;
    a = lc_first_value(a);
    if (op == 0) return lc_bool(!lc_truthy(a));
    if (op == 1) {
        if (!lc_is_numeric(a)) {
            LcValue meta = lc_call_metamethod(ctx, a, a, "__unm");
            if (meta.type != LC_NIL) return meta;
            lc_release(ctx, meta);
        }
        if (a.type == LC_INTEGER) return lc_integer((int64_t)(0 - (uint64_t)a.as.integer));
        return lc_number(-lc_to_number(ctx, a, &ok));
    }
    if (op == 4) {
        if (!lc_is_numeric(a)) {
            LcValue meta = lc_call_metamethod(ctx, a, a, "__bnot");
            if (meta.type != LC_NIL) return meta;
            lc_release(ctx, meta);
        }
        return lc_integer(~lc_to_integer(ctx, a));
    }
    if (op == 2) {
        if (a.type == LC_STRING) return lc_number((double)a.as.string->length);
        if (a.type == LC_TABLE) {
            LcValue method = lc_metamethod(ctx, a, "__len");
            if (method.type != LC_NIL) {
                LcValue result = lc_call(ctx, method, 1, &a);
                lc_release(ctx, method); return result;
            }
            lc_release(ctx, method);
            return lc_number(lc_table_length(a));
        }
        lc_error(ctx, "attempt to get length of a %s value", lc_type_name(a));
    }
    return lc_nil();
}

/* Stackful coroutines preserve generated C locals and closure frames. Unix
   uses ucontext, Windows native Fibers, and Android a synchronized pthread
   backend because Bionic provides ucontext_t but no context-switch APIs. */
#ifdef __ANDROID__
static void *lc_coroutine_entry(void *parameter);
#elif !defined(_WIN32)
static LcContext *lc_coroutine_boot_context;
static void lc_coroutine_entry(void);
#else
static void WINAPI lc_coroutine_entry(void *parameter);
static LcContext *lc_windows_coroutine_context;
#endif
static LcValue lc_coroutine_value(LcCoroutine *co) {
    LcValue v = lc_nil(); v.type = LC_COROUTINE; v.as.coroutine = co; return v;
}
static LcValue lc_coroutine_new(LcContext *ctx, LcValue function) {
    LcCoroutine *co;
    if (lc_first_value(function).type != LC_FUNCTION) { lc_error(ctx, "function expected"); return lc_nil(); }
    co = (LcCoroutine *)lc_alloc(ctx, sizeof(*co), LC_COROUTINE);
    if (!co) return lc_nil();
    co->function = lc_retain(lc_first_value(function)); co->stack_size = 1024 * 1024;
#ifdef __ANDROID__
    co->owner = ctx;
    pthread_mutex_init(&co->mutex, NULL);
    pthread_cond_init(&co->condition, NULL);
    {
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, co->stack_size);
        if (pthread_create(&co->thread, &attributes, lc_coroutine_entry, co) != 0) {
            pthread_attr_destroy(&attributes);
            pthread_cond_destroy(&co->condition); pthread_mutex_destroy(&co->mutex);
            lc_release(ctx, lc_coroutine_value(co)); lc_error(ctx, "cannot create coroutine thread"); return lc_nil();
        }
        pthread_attr_destroy(&attributes);
        co->thread_created = 1;
    }
#elif !defined(_WIN32)
    co->stack = malloc(co->stack_size);
    if (!co->stack) { lc_release(ctx, lc_coroutine_value(co)); lc_error(ctx, "out of memory creating coroutine stack"); return lc_nil(); }
    if (getcontext(&co->context) != 0) { free(co->stack); co->stack = NULL; lc_release(ctx, lc_coroutine_value(co)); lc_error(ctx, "cannot create coroutine context"); return lc_nil(); }
    co->context.uc_stack.ss_sp = co->stack; co->context.uc_stack.ss_size = co->stack_size; co->context.uc_link = NULL;
    makecontext(&co->context, lc_coroutine_entry, 0);
#else
    co->fiber = CreateFiber(co->stack_size, lc_coroutine_entry, co);
    if (!co->fiber) { lc_release(ctx, lc_coroutine_value(co)); lc_error(ctx, "cannot create coroutine fiber"); return lc_nil(); }
#endif
    return lc_coroutine_value(co);
}
static LcValue lc_coroutine_pack(LcContext *ctx, int success, LcValue values) {
    LcTuple *tuple; int i, n; LcValue *items, result;
    if (values.type == LC_TUPLE) { tuple = values.as.tuple; n = tuple->count + 1; }
    else { tuple = NULL; n = 2; }
    items = (LcValue *)malloc(sizeof(LcValue) * (size_t)n);
    if (!items) { lc_release(ctx, values); lc_error(ctx, "out of memory"); return lc_nil(); }
    items[0] = lc_bool(success);
    if (tuple) for (i = 1; i < n; ++i) items[i] = lc_retain(tuple->items[i - 1]);
    else items[1] = lc_retain(values);
    lc_release(ctx, values); result = lc_tuple_new(ctx, n, items, 1); free(items); return result;
}
static LcValue lc_coroutine_resume_value(LcContext *ctx, LcCoroutine *co, int argc, LcValue *argv) {
    LcCoroutine *parent = ctx->active_coroutine; LcValue out;
    LcGcFrame *caller_frames = ctx->gc_frames;
    if (!co || co->state == 2) { out = lc_string(ctx, "cannot resume dead coroutine"); return lc_coroutine_pack(ctx, 0, out); }
    if (co->state == 1 || co == parent) { out = lc_string(ctx, "cannot resume running coroutine"); return lc_coroutine_pack(ctx, 0, out); }
    if (co->state == 3) { out = lc_string(ctx, "cannot resume failed coroutine"); return lc_coroutine_pack(ctx, 0, out); }
    co->resume_args = lc_tuple_new(ctx, argc, argv, 0);
    co->yielded = lc_nil(); co->result = lc_nil();
    co->caller_frames = caller_frames;
    ctx->gc_frames = co->saved_frames;
    co->state = 1; ctx->active_coroutine = co;
#ifdef __ANDROID__
    pthread_mutex_lock(&co->mutex);
    co->turn = 1;
    pthread_cond_broadcast(&co->condition);
    while (co->turn) pthread_cond_wait(&co->condition, &co->mutex);
    pthread_mutex_unlock(&co->mutex);
#elif !defined(_WIN32)
    lc_coroutine_boot_context = ctx;
    if (swapcontext(&co->caller, &co->context) != 0) {
        ctx->gc_frames = caller_frames; co->caller_frames = NULL;
        ctx->active_coroutine = parent; co->state = 3;
        out = lc_string(ctx, "coroutine context switch failed"); return lc_coroutine_pack(ctx, 0, out);
    }
#else
    lc_windows_coroutine_context = ctx;
    if (!IsThreadAFiber() && !ConvertThreadToFiber(NULL)) {
        ctx->gc_frames = caller_frames; co->caller_frames = NULL;
        ctx->active_coroutine = parent; out = lc_string(ctx, "cannot convert thread to a fiber"); return lc_coroutine_pack(ctx, 0, out);
    }
    co->caller_fiber = GetCurrentFiber();
    if (!co->caller_fiber) {
        ctx->gc_frames = caller_frames; co->caller_frames = NULL;
        ctx->active_coroutine = parent; out = lc_string(ctx, "coroutine resume requires a fiber thread"); return lc_coroutine_pack(ctx, 0, out);
    }
    SwitchToFiber(co->fiber);
#endif
    if (co->state != 0) co->saved_frames = NULL;
    ctx->gc_frames = caller_frames;
    co->caller_frames = NULL;
    ctx->active_coroutine = parent;
    if (ctx->error || co->state == 3) {
        out = lc_string(ctx, ctx->error ? ctx->message : "coroutine failed");
        ctx->error = 0; ctx->message[0] = 0; co->state = 3;
        return lc_coroutine_pack(ctx, 0, out);
    }
    return lc_coroutine_pack(ctx, 1, lc_retain(co->state == 2 ? co->result : co->yielded));
}
static LcValue lc_builtin_coroutine_create(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self; return lc_coroutine_new(ctx, argc ? argv[0] : lc_nil());
}
static LcValue lc_builtin_coroutine_resume(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue value = argc ? lc_first_value(argv[0]) : lc_nil(); (void)self;
    if (value.type != LC_COROUTINE) { LcValue error = lc_string(ctx, "coroutine expected"); return lc_coroutine_pack(ctx, 0, error); }
    return lc_coroutine_resume_value(ctx, value.as.coroutine, argc - 1, argv + 1);
}
static LcValue lc_builtin_coroutine_yield(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcCoroutine *co = ctx->active_coroutine; LcValue result; (void)self;
    if (!co) { lc_error(ctx, "attempt to yield from outside a coroutine"); return lc_nil(); }
    co->yielded = lc_tuple_new(ctx, argc, argv, 0);
    co->resume_args = lc_nil(); co->state = 0;
    co->saved_frames = ctx->gc_frames;
#ifdef __ANDROID__
    pthread_mutex_lock(&co->mutex);
    co->turn = 0;
    pthread_cond_broadcast(&co->condition);
    while (!co->turn && !co->cancel) pthread_cond_wait(&co->condition, &co->mutex);
    if (co->cancel) {
        co->turn = 0;
        pthread_cond_broadcast(&co->condition);
        pthread_mutex_unlock(&co->mutex);
        pthread_exit(NULL);
    }
    pthread_mutex_unlock(&co->mutex);
#elif !defined(_WIN32)
    if (swapcontext(&co->context, &co->caller) != 0) { lc_error(ctx, "coroutine yield failed"); return lc_nil(); }
#else
    SwitchToFiber(co->caller_fiber);
#endif
    result = lc_retain(co->resume_args); lc_release(ctx, co->resume_args); co->resume_args = lc_nil(); return result;
}
#ifdef __ANDROID__
static void lc_android_coroutine_dispose(LcCoroutine *co) {
    if (!co->thread_created) return;
    pthread_mutex_lock(&co->mutex);
    co->cancel = 1; co->turn = 1;
    pthread_cond_broadcast(&co->condition);
    pthread_mutex_unlock(&co->mutex);
    pthread_join(co->thread, NULL);
    co->thread_created = 0;
    pthread_cond_destroy(&co->condition); pthread_mutex_destroy(&co->mutex);
}
static void *lc_coroutine_entry(void *parameter) {
    LcCoroutine *co = (LcCoroutine *)parameter;
    LcContext *ctx = co->owner;
    int argc;
    LcValue *argv;
    pthread_mutex_lock(&co->mutex);
    while (!co->turn && !co->cancel) pthread_cond_wait(&co->condition, &co->mutex);
    if (co->cancel) {
        co->turn = 0; pthread_cond_broadcast(&co->condition);
        pthread_mutex_unlock(&co->mutex); return NULL;
    }
    pthread_mutex_unlock(&co->mutex);
    argc = co->resume_args.type == LC_TUPLE ? co->resume_args.as.tuple->count : 0;
    argv = argc ? co->resume_args.as.tuple->items : NULL;
    co->started = 1; lc_release(ctx, co->result); co->result = lc_call_list(ctx, co->function, argc, argv);
    lc_release(ctx, co->resume_args); co->resume_args = lc_nil(); co->state = ctx->error ? 3 : 2;
    pthread_mutex_lock(&co->mutex);
    co->turn = 0; pthread_cond_broadcast(&co->condition);
    pthread_mutex_unlock(&co->mutex);
    return NULL;
}
#elif !defined(_WIN32)
static void lc_coroutine_entry(void) {
    LcContext *ctx = lc_coroutine_boot_context; LcCoroutine *co = ctx->active_coroutine;
    int argc = co->resume_args.type == LC_TUPLE ? co->resume_args.as.tuple->count : 0;
    LcValue *argv = argc ? co->resume_args.as.tuple->items : NULL;
    co->started = 1; lc_release(ctx, co->result); co->result = lc_call_list(ctx, co->function, argc, argv);
    lc_release(ctx, co->resume_args); co->resume_args = lc_nil(); co->state = ctx->error ? 3 : 2;
    setcontext(&co->caller); abort();
}
#else
static void WINAPI lc_coroutine_entry(void *parameter) {
    LcCoroutine *co = (LcCoroutine *)parameter; LcContext *ctx = lc_windows_coroutine_context;
    int argc = co->resume_args.type == LC_TUPLE ? co->resume_args.as.tuple->count : 0;
    LcValue *argv = argc ? co->resume_args.as.tuple->items : NULL;
    co->started = 1; lc_release(ctx, co->result); co->result = lc_call_list(ctx, co->function, argc, argv);
    lc_release(ctx, co->resume_args); co->resume_args = lc_nil(); co->state = ctx->error ? 3 : 2;
    SwitchToFiber(co->caller_fiber);
}
#endif
static LcValue lc_builtin_coroutine_status(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue value = argc ? lc_first_value(argv[0]) : lc_nil(); (void)self;
    if (value.type != LC_COROUTINE) { lc_error(ctx, "coroutine expected"); return lc_nil(); }
    if (value.as.coroutine == ctx->active_coroutine) return lc_string(ctx, "running");
    if (value.as.coroutine->state == 0) return lc_string(ctx, "suspended");
    return lc_string(ctx, "dead");
}
static LcValue lc_builtin_coroutine_running(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue out[2]; (void)self; (void)argc; (void)argv;
    if (!ctx->active_coroutine) { out[0] = lc_nil(); out[1] = lc_bool(1); }
    else { out[0] = lc_retain(lc_coroutine_value(ctx->active_coroutine)); out[1] = lc_bool(0); }
    return lc_tuple_new(ctx, 2, out, 1);
}
static LcValue lc_builtin_coroutine_isyieldable(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self; (void)argc; (void)argv; return lc_bool(ctx->active_coroutine != NULL);
}
static LcValue lc_builtin_coroutine_wrap_call(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue packed = lc_coroutine_resume_value(ctx, self->upvalues[0]->value.as.coroutine, argc, argv), first, rest;
    if (packed.type != LC_TUPLE || !packed.as.tuple->count) return packed;
    first = packed.as.tuple->items[0];
    if (!lc_truthy(first)) {
        LcValue message = packed.as.tuple->count > 1 ? packed.as.tuple->items[1] : lc_string(ctx, "coroutine failed");
        if (message.type == LC_STRING) lc_error(ctx, "%s", message.as.string->data); else lc_error(ctx, "coroutine failed");
        lc_release(ctx, packed); return lc_nil();
    }
    rest = lc_tuple_new(ctx, packed.as.tuple->count - 1, packed.as.tuple->items + 1, 0); lc_release(ctx, packed); return rest;
}
static LcValue lc_builtin_coroutine_wrap(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue co = lc_coroutine_new(ctx, argc ? argv[0] : lc_nil()), result; LcCell *up; (void)self;
    if (co.type != LC_COROUTINE) return co;
    up = lc_cell_new(ctx, lc_retain(co));
    result = lc_function_new(ctx, lc_builtin_coroutine_wrap_call, 1, &up, "coroutine.wrap");
    lc_release(ctx, co); return result;
}

/* Built-in library. */
static void lc_print_bytes(LcContext *ctx, const char *data, size_t length) {
    if (!ctx->capture_output) {
        if (length) fwrite(data, 1, length, stdout);
        return;
    }
    if (ctx->captured_length + length + 1 > ctx->captured_capacity) {
        size_t capacity = ctx->captured_capacity ? ctx->captured_capacity * 2 : 256;
        char *memory;
        while (capacity < ctx->captured_length + length + 1) capacity *= 2;
        memory = (char *)realloc(ctx->captured_output, capacity);
        if (!memory) { lc_error(ctx, "out of memory capturing output"); return; }
        ctx->captured_output = memory; ctx->captured_capacity = capacity;
    }
    if (length) memcpy(ctx->captured_output + ctx->captured_length, data, length);
    ctx->captured_length += length; ctx->captured_output[ctx->captured_length] = 0;
}
static LcValue lc_builtin_print(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int i; (void)self;
    for (i = 0; i < argc; ++i) {
        LcValue s = lc_to_string(ctx, argv[i]);
        if (i) lc_print_bytes(ctx, "\t", 1);
        if (s.type == LC_STRING) lc_print_bytes(ctx, s.as.string->data, s.as.string->length);
        lc_release(ctx, s);
    }
    lc_print_bytes(ctx, "\n", 1); fflush(stdout); return lc_nil();
}

/* Lua's default warning handler writes to stderr and recognizes @on/@off.
   Embedded projects capture warnings alongside print so users can see them. */
static LcValue lc_builtin_warn(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int i;
    (void)self;
    if (!argc) { lc_error(ctx, "bad argument #1 to 'warn' (string expected)"); return lc_nil(); }
    for (i = 0; i < argc; ++i) {
        LcValue v = lc_first_value(argv[i]);
        if (v.type != LC_STRING && v.type != LC_NUMBER && v.type != LC_INTEGER) {
            lc_error(ctx, "bad argument #%d to 'warn' (string expected)", i + 1);
            return lc_nil();
        }
    }
    if (argc == 1 && lc_first_value(argv[0]).type == LC_STRING) {
        LcString *s = lc_first_value(argv[0]).as.string;
        if (s->length && s->data[0] == '@') {
            if (s->length == 3 && !memcmp(s->data, "@on", 3)) ctx->warnings_enabled = 1;
            else if (s->length == 4 && !memcmp(s->data, "@off", 4)) ctx->warnings_enabled = 0;
            return lc_nil();
        }
    }
    if (!ctx->warnings_enabled) return lc_nil();
    if (ctx->capture_output) lc_print_bytes(ctx, "Lua warning: ", 13);
    else fputs("Lua warning: ", stderr);
    for (i = 0; i < argc; ++i) {
        LcValue s = lc_to_string(ctx, lc_first_value(argv[i]));
        if (s.type == LC_STRING) {
            if (ctx->capture_output) lc_print_bytes(ctx, s.as.string->data, s.as.string->length);
            else fwrite(s.as.string->data, 1, s.as.string->length, stderr);
        }
        lc_release(ctx, s);
    }
    if (ctx->capture_output) lc_print_bytes(ctx, "\n", 1);
    else { fputc('\n', stderr); fflush(stderr); }
    return lc_nil();
}

static LcValue lc_builtin_type(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self; return lc_string(ctx, lc_type_name(argc ? argv[0] : lc_nil()));
}
static LcValue lc_builtin_tostring(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue value = argc ? argv[0] : lc_nil(), method, result;
    (void)self;
    method = lc_metamethod(ctx, value, "__tostring");
    if (method.type != LC_NIL) {
        result = lc_call(ctx, method, 1, &value);
        lc_release(ctx, method);
        if (lc_first_value(result).type != LC_STRING) {
            lc_release(ctx, result); lc_error(ctx, "'__tostring' must return a string"); return lc_nil();
        }
        return result;
    }
    lc_release(ctx, method);
    return lc_to_string(ctx, value);
}
static LcValue lc_builtin_tonumber(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int ok; double n; (void)self;
    if (!argc) return lc_nil();
    /* Conversion failure in tonumber is a value, not a runtime error. */
    { LcValue v = lc_first_value(argv[0]);
      if (argc > 1) {
        int base = (int)lc_to_integer(ctx, argv[1]), sign = 1, any = 0;
        const char *text; size_t i, length; double value = 0;
        if (v.type != LC_STRING || base < 2 || base > 36) return lc_nil();
        text = v.as.string->data; length = v.as.string->length; i = 0;
        while (i < length && isspace((unsigned char)text[i])) ++i;
        if (i < length && (text[i] == '-' || text[i] == '+')) { if (text[i++] == '-') sign = -1; }
        for (; i < length && isalnum((unsigned char)text[i]); ++i) {
            int digit = isdigit((unsigned char)text[i]) ? text[i] - '0' : tolower((unsigned char)text[i]) - 'a' + 10;
            if (digit >= base) return lc_nil();
            value = value * base + digit; any = 1;
        }
        while (i < length && isspace((unsigned char)text[i])) ++i;
        return any && i == length ? lc_number(sign * value) : lc_nil();
      }
      if (v.type == LC_NUMBER || v.type == LC_INTEGER) return lc_retain(v);
      if (v.type == LC_STRING) { char *end; n = strtod(v.as.string->data, &end); while (isspace((unsigned char)*end)) ++end;
        if (end != v.as.string->data && !*end) return lc_number(n); }
      (void)ok; return lc_nil(); }
}
static LcValue lc_builtin_assert(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self;
    if (!argc || !lc_truthy(argv[0])) { if (argc > 1 && argv[1].type == LC_STRING) lc_error(ctx, "%s", argv[1].as.string->data); else lc_error(ctx, "assertion failed!"); return lc_nil(); }
    return lc_tuple_new(ctx, argc, argv, 0);
}
static LcValue lc_builtin_error(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self;
    if (argc && argv[0].type == LC_STRING) lc_error(ctx, "%s", argv[0].as.string->data); else lc_error(ctx, "error");
    return lc_nil();
}
static LcValue lc_builtin_pcall(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue r, out[2]; (void)self;
    if (!argc) { lc_error(ctx, "pcall expects a function"); return lc_nil(); }
    r = lc_call_list(ctx, argv[0], argc - 1, argv + 1);
    if (ctx->error) {
        out[0] = lc_bool(0); out[1] = lc_string(ctx, ctx->message);
        ctx->error = 0; ctx->message[0] = 0;
        lc_release(ctx, r); return lc_tuple_new(ctx, 2, out, 1);
    }
    if (r.type == LC_TUPLE) {
        int n = r.as.tuple->count + 1, i; LcValue *items = malloc(sizeof(LcValue) * (size_t)n);
        if (!items) { lc_release(ctx, r); lc_error(ctx, "out of memory"); return lc_nil(); }
        items[0] = lc_bool(1); for (i = 1; i < n; ++i) items[i] = lc_retain(r.as.tuple->items[i - 1]);
        lc_release(ctx, r); r = lc_tuple_new(ctx, n, items, 1); free(items); return r;
    }
    out[0] = lc_bool(1); out[1] = r; return lc_tuple_new(ctx, 2, out, 1);
}
static LcValue lc_builtin_select(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int n, i; (void)self;
    if (!argc) return lc_nil();
    if (argv[0].type == LC_STRING && !strcmp(argv[0].as.string->data, "#")) return lc_number(argc - 1);
    n = (int)lc_to_number(ctx, argv[0], &i); if (n < 0) n = argc + n; if (n < 1) n = 1;
    return lc_tuple_new(ctx, argc - n > 0 ? argc - n : 0, argv + n, 0);
}

static LcValue lc_builtin_next(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcTable *t; int i = -1; LcValue out[2]; (void)self;
    if (!argc || argv[0].type != LC_TABLE) { lc_error(ctx, "table expected"); return lc_nil(); }
    t = argv[0].as.table;
    if (argc > 1 && argv[1].type != LC_NIL) {
        i = lc_table_find(t, argv[1]);
        if (i < 0) { lc_error(ctx, "invalid key to 'next'"); return lc_nil(); }
    }
    for (++i; i < (int)t->capacity; ++i) if (t->entries[i].used) {
        out[0] = lc_retain(t->entries[i].key); out[1] = lc_retain(t->entries[i].value);
        return lc_tuple_new(ctx, 2, out, 1);
    }
    return lc_nil();
}
static LcValue lc_builtin_pairs(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue out[3], method;
    (void)self;
    if (argc) {
        method = lc_metamethod(ctx, argv[0], "__pairs");
        if (method.type != LC_NIL) {
            LcValue result = lc_call(ctx, method, 1, argv);
            lc_release(ctx, method); return result;
        }
        lc_release(ctx, method);
    }
    out[0] = lc_function_new(ctx, lc_builtin_next, 0, NULL, "next");
    out[1] = argc ? lc_retain(argv[0]) : lc_nil(); out[2] = lc_nil();
    return lc_tuple_new(ctx, 3, out, 1);
}
static LcValue lc_builtin_ipairs_iter(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int i; LcValue out[2]; (void)self;
    if (argc < 2) return lc_nil();
    i = (int)lc_to_number(ctx, argv[1], &(int){0}) + 1;
    out[0] = lc_number(i); out[1] = lc_table_get(ctx, argv[0], out[0]);
    if (out[1].type == LC_NIL) { lc_release(ctx, out[0]); return lc_nil(); }
    return lc_tuple_new(ctx, 2, out, 1);
}
static LcValue lc_builtin_ipairs(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue out[3]; (void)self;
    out[0] = lc_function_new(ctx, lc_builtin_ipairs_iter, 0, NULL, "ipairs iterator");
    out[1] = argc ? lc_retain(argv[0]) : lc_nil(); out[2] = lc_number(0);
    return lc_tuple_new(ctx, 3, out, 1);
}

static LcValue lc_builtin_xpcall(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue result, message, handled, args[1], out[2];
    if (argc < 2) { lc_error(ctx, "xpcall expects a function and an error handler"); return lc_nil(); }
    {
        LcValue *call_args = (LcValue *)malloc(sizeof(LcValue) * (size_t)(argc - 1));
        if (!call_args) { lc_error(ctx, "out of memory in xpcall"); return lc_nil(); }
        call_args[0] = argv[0];
        if (argc > 2) memcpy(call_args + 1, argv + 2, sizeof(LcValue) * (size_t)(argc - 2));
        result = lc_builtin_pcall(ctx, self, argc - 1, call_args);
        free(call_args);
    }
    if (lc_truthy(lc_first_value(result))) return result;
    message = lc_nth(result, 1);
    args[0] = message;
    handled = lc_call(ctx, argv[1], 1, args);
    out[0] = lc_bool(0); out[1] = lc_first(handled);
    lc_release(ctx, result); lc_release(ctx, message); lc_release(ctx, handled);
    return lc_tuple_new(ctx, 2, out, 1);
}

static LcValue lc_builtin_rawget(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self;
    if (argc < 2 || lc_first_value(argv[0]).type != LC_TABLE) { lc_error(ctx, "table expected"); return lc_nil(); }
    return lc_table_raw_get(ctx, argv[0], argv[1]);
}
static LcValue lc_builtin_rawset(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self;
    if (argc < 3 || lc_first_value(argv[0]).type != LC_TABLE) { lc_error(ctx, "table expected"); return lc_nil(); }
    lc_table_raw_set(ctx, argv[0], argv[1], argv[2]);
    return lc_retain(argv[0]);
}
static LcValue lc_builtin_rawequal(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)ctx; (void)self;
    return lc_bool(lc_value_equal(argc > 0 ? argv[0] : lc_nil(), argc > 1 ? argv[1] : lc_nil()));
}
static LcValue lc_builtin_rawlen(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue value; (void)ctx; (void)self;
    if (!argc) { lc_error(ctx, "table or string expected"); return lc_nil(); }
    value = lc_first_value(argv[0]);
    if (value.type == LC_TABLE) return lc_number(lc_table_length(value));
    if (value.type == LC_STRING) return lc_number((double)value.as.string->length);
    lc_error(ctx, "table or string expected"); return lc_nil();
}
static void lc_gc_collect(LcContext *ctx);

static LcValue lc_builtin_setmetatable(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue table, meta, guard;
    (void)self;
    if (argc < 2) { lc_error(ctx, "setmetatable expects a table and metatable/nil"); return lc_nil(); }
    table = lc_first_value(argv[0]); meta = lc_first_value(argv[1]);
    if (table.type != LC_TABLE || (meta.type != LC_TABLE && meta.type != LC_NIL)) {
        lc_error(ctx, "invalid setmetatable arguments"); return lc_nil();
    }
    guard = lc_metamethod(ctx, table, "__metatable");
    if (guard.type != LC_NIL) { lc_error(ctx, "cannot change a protected metatable"); return lc_nil(); }
    table.as.table->metatable = meta;
    lc_gc_barrier_back(ctx, &table.as.table->object);
    return table;
}
static LcValue lc_builtin_getmetatable(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue value, key, guard;
    (void)self;
    if (!argc) return lc_nil();
    value = lc_first_value(argv[0]);
    if (value.type != LC_TABLE || value.as.table->metatable.type != LC_TABLE) return lc_nil();
    key = lc_string(ctx, "__metatable");
    guard = lc_table_raw_get(ctx, value.as.table->metatable, key);
    return guard.type != LC_NIL ? guard : value.as.table->metatable;
}
static LcValue lc_builtin_collectgarbage(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    const char *option = argc && lc_first_value(argv[0]).type == LC_STRING ? lc_first_value(argv[0]).as.string->data : "collect";
    (void)self;
    if (!strcmp(option, "isrunning")) return lc_bool(ctx->gc_running);
    if (!strcmp(option, "stop")) { ctx->gc_running = 0; return lc_number(0); }
    if (!strcmp(option, "restart")) { ctx->gc_running = 1; return lc_number(0); }
    if (!strcmp(option, "collect")) { lc_gc_collect(ctx); return lc_number(0); }
    if (!strcmp(option, "step")) {
        int prev = ctx->gc_running;
        ctx->gc_running = 1;
        ctx->gc_debt = (argc > 1 ? (intptr_t)lc_to_integer(ctx, argv[1]) * 1024 : 4096);
        lc_gc_step(ctx);
        ctx->gc_running = prev;
        return lc_bool(ctx->gc_state == LC_GCS_PAUSE);
    }
    if (!strcmp(option, "setpause")) {
        int old = ctx->gc_pause;
        if (argc > 1) ctx->gc_pause = (int)lc_to_integer(ctx, argv[1]);
        return lc_integer(old);
    }
    if (!strcmp(option, "setstepmul")) {
        int old = ctx->gc_stepmul;
        if (argc > 1) ctx->gc_stepmul = (int)lc_to_integer(ctx, argv[1]);
        return lc_integer(old);
    }
    if (!strcmp(option, "count")) {
        size_t bytes = ctx->gc_bytes;
        LcValue out[2];
        out[0] = lc_number((double)bytes / 1024.0);
        out[1] = lc_number((double)(bytes % 1024));
        return lc_tuple_new(ctx, 2, out, 1);
    }
    return lc_number(0);
}
static LcValue lc_builtin_gcinfo(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self; (void)argc; (void)argv;
    return lc_number((double)ctx->gc_bytes / 1024.0);
}
static void lc_clear_draw_cmds(LcContext *ctx) {
    int i;
    for (i = 0; i < ctx->draw_count; ++i) {
        free(ctx->draw_cmds[i].text);
        ctx->draw_cmds[i].text = NULL;
        free(ctx->draw_cmds[i].font);
        ctx->draw_cmds[i].font = NULL;
    }
    ctx->draw_count = 0;
    ctx->has_bg = 0;
}
static LcValue lc_builtin_scene_reset(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self; (void)argc; (void)argv;
    lc_clear_draw_cmds(ctx);
    lc_gc_collect(ctx);
    return lc_bool(1);
}

static void lc_push_draw_cmd(LcContext *ctx, LcDrawCmd cmd) {
    if (ctx->draw_count >= ctx->draw_cap) {
        int cap = ctx->draw_cap ? ctx->draw_cap * 2 : 64;
        LcDrawCmd *p = (LcDrawCmd *)realloc(ctx->draw_cmds, (size_t)cap * sizeof(*p));
        if (!p) { free(cmd.text); free(cmd.font); lc_error(ctx, "out of memory in draw queue"); return; }
        ctx->draw_cmds = p;
        ctx->draw_cap = cap;
    }
    ctx->draw_cmds[ctx->draw_count++] = cmd;
}
static double lc_arg_num(LcContext *ctx, int argc, LcValue *argv, int idx, double def) {
    int ok = 0;
    if (idx >= argc) return def;
    { LcValue v = lc_first_value(argv[idx]);
      if (v.type == LC_INTEGER) return (double)v.as.integer;
      if (v.type == LC_NUMBER) return v.as.number;
      if (v.type == LC_STRING) return lc_to_number(ctx, v, &ok);
      return def; }
}
static LcValue lc_builtin_clear_screen(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self;
    lc_clear_draw_cmds(ctx);
    if (argc >= 3) {
        ctx->bg_r = lc_arg_num(ctx, argc, argv, 0, 0.0);
        ctx->bg_g = lc_arg_num(ctx, argc, argv, 1, 0.0);
        ctx->bg_b = lc_arg_num(ctx, argc, argv, 2, 0.0);
        ctx->has_bg = 1;
    }
    return lc_nil();
}
static LcValue lc_builtin_draw_sprite(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcDrawCmd c;
    int id;
    (void)self;
    memset(&c, 0, sizeof(c));
    c.kind = 0;
    id = (int)lc_arg_num(ctx, argc, argv, 0, 1);
    c.x = lc_arg_num(ctx, argc, argv, 1, 0);
    c.y = lc_arg_num(ctx, argc, argv, 2, 0);
    c.sprite_id = id;
    c.w = 32; c.h = 32;
    if (id >= 1 && id <= ctx->sprite_count) {
        c.w = ctx->sprites[id - 1].width > 0 ? ctx->sprites[id - 1].width : 32;
        c.h = ctx->sprites[id - 1].height > 0 ? ctx->sprites[id - 1].height : 32;
    }
    if (argc > 3) c.w = lc_arg_num(ctx, argc, argv, 3, c.w);
    if (argc > 4) c.h = lc_arg_num(ctx, argc, argv, 4, c.h);
    c.rot = lc_arg_num(ctx, argc, argv, 5, 0);
    c.r = c.g = c.b = c.a = 1.0;
    lc_push_draw_cmd(ctx, c);
    return lc_nil();
}
static LcValue lc_builtin_draw_rect(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcDrawCmd c; (void)self;
    memset(&c, 0, sizeof(c));
    c.kind = 1;
    c.x = lc_arg_num(ctx, argc, argv, 0, 0);
    c.y = lc_arg_num(ctx, argc, argv, 1, 0);
    c.w = lc_arg_num(ctx, argc, argv, 2, 32);
    c.h = lc_arg_num(ctx, argc, argv, 3, 32);
    c.r = lc_arg_num(ctx, argc, argv, 4, 1.0);
    c.g = lc_arg_num(ctx, argc, argv, 5, 1.0);
    c.b = lc_arg_num(ctx, argc, argv, 6, 1.0);
    c.a = lc_arg_num(ctx, argc, argv, 7, 1.0);
    c.rot = lc_arg_num(ctx, argc, argv, 8, 0.0);
    lc_push_draw_cmd(ctx, c);
    return lc_nil();
}
static LcValue lc_builtin_draw_circle(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcDrawCmd c; double rad; (void)self;
    memset(&c, 0, sizeof(c));
    c.kind = 2;
    c.x = lc_arg_num(ctx, argc, argv, 0, 0);
    c.y = lc_arg_num(ctx, argc, argv, 1, 0);
    rad = lc_arg_num(ctx, argc, argv, 2, 16);
    c.w = rad * 2.0; c.h = rad * 2.0;
    c.r = lc_arg_num(ctx, argc, argv, 3, 1.0);
    c.g = lc_arg_num(ctx, argc, argv, 4, 1.0);
    c.b = lc_arg_num(ctx, argc, argv, 5, 1.0);
    c.a = lc_arg_num(ctx, argc, argv, 6, 1.0);
    lc_push_draw_cmd(ctx, c);
    return lc_nil();
}
static LcValue lc_builtin_draw_line(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcDrawCmd c; (void)self;
    memset(&c, 0, sizeof(c));
    c.kind = 4;
    c.x = lc_arg_num(ctx, argc, argv, 0, 0);
    c.y = lc_arg_num(ctx, argc, argv, 1, 0);
    c.w = lc_arg_num(ctx, argc, argv, 2, 0);
    c.h = lc_arg_num(ctx, argc, argv, 3, 0);
    c.extra = lc_arg_num(ctx, argc, argv, 4, 2.0);
    c.r = lc_arg_num(ctx, argc, argv, 5, 1.0);
    c.g = lc_arg_num(ctx, argc, argv, 6, 1.0);
    c.b = lc_arg_num(ctx, argc, argv, 7, 1.0);
    c.a = lc_arg_num(ctx, argc, argv, 8, 1.0);
    lc_push_draw_cmd(ctx, c);
    return lc_nil();
}
static LcValue lc_builtin_draw_rect_outline(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcDrawCmd c; (void)self;
    memset(&c, 0, sizeof(c));
    c.kind = 5;
    c.x = lc_arg_num(ctx, argc, argv, 0, 0);
    c.y = lc_arg_num(ctx, argc, argv, 1, 0);
    c.w = lc_arg_num(ctx, argc, argv, 2, 32);
    c.h = lc_arg_num(ctx, argc, argv, 3, 32);
    c.extra = lc_arg_num(ctx, argc, argv, 4, 2.0);
    c.r = lc_arg_num(ctx, argc, argv, 5, 1.0);
    c.g = lc_arg_num(ctx, argc, argv, 6, 1.0);
    c.b = lc_arg_num(ctx, argc, argv, 7, 1.0);
    c.a = lc_arg_num(ctx, argc, argv, 8, 1.0);
    c.rot = lc_arg_num(ctx, argc, argv, 9, 0.0);
    lc_push_draw_cmd(ctx, c);
    return lc_nil();
}
static LcValue lc_builtin_draw_circle_outline(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcDrawCmd c; double rad; (void)self;
    memset(&c, 0, sizeof(c));
    c.kind = 6;
    c.x = lc_arg_num(ctx, argc, argv, 0, 0);
    c.y = lc_arg_num(ctx, argc, argv, 1, 0);
    rad = lc_arg_num(ctx, argc, argv, 2, 16);
    c.w = rad * 2.0; c.h = rad * 2.0;
    c.extra = lc_arg_num(ctx, argc, argv, 3, 2.0);
    c.r = lc_arg_num(ctx, argc, argv, 4, 1.0);
    c.g = lc_arg_num(ctx, argc, argv, 5, 1.0);
    c.b = lc_arg_num(ctx, argc, argv, 6, 1.0);
    c.a = lc_arg_num(ctx, argc, argv, 7, 1.0);
    lc_push_draw_cmd(ctx, c);
    return lc_nil();
}
static LcValue lc_builtin_draw_round_rect(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcDrawCmd c; (void)self;
    memset(&c, 0, sizeof(c));
    c.kind = 7;
    c.x = lc_arg_num(ctx, argc, argv, 0, 0);
    c.y = lc_arg_num(ctx, argc, argv, 1, 0);
    c.w = lc_arg_num(ctx, argc, argv, 2, 32);
    c.h = lc_arg_num(ctx, argc, argv, 3, 32);
    c.extra = lc_arg_num(ctx, argc, argv, 4, 8.0);
    c.r = lc_arg_num(ctx, argc, argv, 5, 1.0);
    c.g = lc_arg_num(ctx, argc, argv, 6, 1.0);
    c.b = lc_arg_num(ctx, argc, argv, 7, 1.0);
    c.a = lc_arg_num(ctx, argc, argv, 8, 1.0);
    c.rot = lc_arg_num(ctx, argc, argv, 9, 0.0);
    lc_push_draw_cmd(ctx, c);
    return lc_nil();
}
static LcValue lc_builtin_set_font(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self;
    free(ctx->current_font);
    ctx->current_font = NULL;
    if (argc >= 1) {
        LcValue v = lc_first_value(argv[0]);
        if (v.type == LC_STRING && v.as.string->length > 0) {
            ctx->current_font = (char *)malloc(v.as.string->length + 1);
            if (ctx->current_font) {
                memcpy(ctx->current_font, v.as.string->data, v.as.string->length);
                ctx->current_font[v.as.string->length] = '\0';
            }
        }
    }
    return lc_nil();
}
static LcValue lc_builtin_set_text_align(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self;
    ctx->text_align = 0;
    if (argc >= 1) {
        LcValue v = lc_first_value(argv[0]);
        if (v.type == LC_STRING) {
            if (!strcmp(v.as.string->data, "center")) ctx->text_align = 1;
            else if (!strcmp(v.as.string->data, "right")) ctx->text_align = 2;
            else ctx->text_align = 0;
        }
    }
    return lc_nil();
}
static LcValue lc_builtin_draw_text(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcDrawCmd c; LcValue s; const char *txt; size_t len; (void)self;
    if (argc < 1) return lc_nil();
    memset(&c, 0, sizeof(c));
    c.kind = 3;
    s = lc_to_string(ctx, argv[0]);
    txt = s.type == LC_STRING ? s.as.string->data : "";
    len = s.type == LC_STRING ? s.as.string->length : 0;
    c.text = (char *)malloc(len + 1);
    if (c.text) { memcpy(c.text, txt, len); c.text[len] = '\0'; }
    if (ctx->current_font) {
        size_t flen = strlen(ctx->current_font);
        c.font = (char *)malloc(flen + 1);
        if (c.font) memcpy(c.font, ctx->current_font, flen + 1);
    }
    c.align = ctx->text_align;
    c.x = lc_arg_num(ctx, argc, argv, 1, 16);
    c.y = lc_arg_num(ctx, argc, argv, 2, 32);
    c.w = lc_arg_num(ctx, argc, argv, 3, 18);
    c.r = lc_arg_num(ctx, argc, argv, 4, 1.0);
    c.g = lc_arg_num(ctx, argc, argv, 5, 1.0);
    c.b = lc_arg_num(ctx, argc, argv, 6, 1.0);
    c.a = lc_arg_num(ctx, argc, argv, 7, 1.0);
    lc_push_draw_cmd(ctx, c);
    return lc_nil();
}
static LcValue lc_builtin_screen_width(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self; (void)argc; (void)argv;
    return lc_integer(ctx->screen_w > 0 ? ctx->screen_w : 480);
}
static LcValue lc_builtin_screen_height(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self; (void)argc; (void)argv;
    return lc_integer(ctx->screen_h > 0 ? ctx->screen_h : 800);
}

static LcValue lc_package_table(LcContext *ctx, const char *name) {
    LcValue key = lc_string(ctx, "package"), package = lc_table_raw_get(ctx, ctx->globals, key), field;
    lc_release(ctx, key); key = lc_string(ctx, name); field = lc_table_raw_get(ctx, package, key);
    lc_release(ctx, key); lc_release(ctx, package); return field;
}
static void lc_preload_native(LcContext *ctx, const char *name, LcNative native) {
    LcValue preload = lc_package_table(ctx, "preload"), key = lc_string(ctx, name);
    LcValue function = lc_function_new(ctx, native, 0, NULL, name);
    lc_table_set(ctx, preload, key, function);
    lc_release(ctx, preload); lc_release(ctx, key); lc_release(ctx, function);
}

static LcValue lc_builtin_require(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue loaded, preload, name, loader, result;
    (void)self;
    if (!argc || lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "module name expected"); return lc_nil(); }
    name = lc_first_value(argv[0]); loaded = lc_package_table(ctx, "loaded"); result = lc_table_get(ctx, loaded, name);
    if (lc_truthy(result)) { lc_release(ctx, loaded); return result; }
    lc_release(ctx, result); preload = lc_package_table(ctx, "preload"); loader = lc_table_get(ctx, preload, name);
    if (loader.type != LC_FUNCTION) {
        lc_release(ctx, loaded); lc_release(ctx, preload); lc_release(ctx, loader);
        lc_error(ctx, "module '%s' not found (literal disk requires must be linked at compile time)", name.as.string->data);
        return lc_nil();
    }
    /* Mark the module before execution so recursive requires terminate just
       like Lua's package.loaded cycle guard. */
    lc_table_set(ctx, loaded, name, lc_bool(1));
    result = lc_call(ctx, loader, 1, &name);
    if (ctx->error) { LcValue nil = lc_nil(); lc_table_set(ctx, loaded, name, nil); lc_release(ctx, loaded); lc_release(ctx, preload); lc_release(ctx, loader); lc_release(ctx, result); return lc_nil(); }
    if (result.type != LC_NIL) lc_table_set(ctx, loaded, name, result);
    { LcValue check = lc_table_get(ctx, loaded, name);
      if (check.type == LC_NIL) { lc_table_set(ctx, loaded, name, lc_bool(1)); lc_release(ctx, check); check = lc_table_get(ctx, loaded, name); }
      lc_release(ctx, result); result = check; }
    lc_release(ctx, loaded); lc_release(ctx, preload); lc_release(ctx, loader); return result;
}

static LcValue lc_math_unary(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int ok; double x;
    x = lc_to_number(ctx, argc ? argv[0] : lc_nil(), &ok);
    /* Function pointer stored in the name field is avoided; dispatch by name. */
    if (!strcmp(self->name, "sin")) return lc_number(sin(x));
    if (!strcmp(self->name, "cos")) return lc_number(cos(x));
    if (!strcmp(self->name, "tan")) return lc_number(tan(x));
    if (!strcmp(self->name, "asin")) return lc_number(asin(x));
    if (!strcmp(self->name, "acos")) return lc_number(acos(x));
    if (!strcmp(self->name, "atan")) return lc_number(argc > 1 ? atan2(x, lc_to_number(ctx, argv[1], &ok)) : atan(x));
    if (!strcmp(self->name, "exp")) return lc_number(exp(x));
    if (!strcmp(self->name, "log")) { double base = argc > 1 ? lc_to_number(ctx, argv[1], &ok) : 0; return lc_number(argc > 1 ? log(x) / log(base) : log(x)); }
    if (!strcmp(self->name, "sqrt")) return lc_number(sqrt(x));
    if (!strcmp(self->name, "floor")) return lc_number(floor(x));
    if (!strcmp(self->name, "ceil")) return lc_number(ceil(x));
    if (!strcmp(self->name, "abs")) return lc_number(fabs(x));
    if (!strcmp(self->name, "sinh")) return lc_number(sinh(x));
    if (!strcmp(self->name, "cosh")) return lc_number(cosh(x));
    if (!strcmp(self->name, "tanh")) return lc_number(tanh(x));
    if (!strcmp(self->name, "deg")) return lc_number(x * (180.0 / 3.14159265358979323846));
    if (!strcmp(self->name, "rad")) return lc_number(x * (3.14159265358979323846 / 180.0));
    return lc_nil();
}
static LcValue lc_math_minmax(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int i, ok; double best, x; if (!argc) { lc_error(ctx, "number expected"); return lc_nil(); }
    best = lc_to_number(ctx, argv[0], &ok);
    for (i = 1; i < argc; ++i) { x = lc_to_number(ctx, argv[i], &ok); if ((!strcmp(self->name, "min") && x < best) || (!strcmp(self->name, "max") && x > best)) best = x; }
    return lc_number(best);
}
static LcValue lc_math_random(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    double r = (double)rand() / ((double)RAND_MAX + 1.0); int ok;
    if (!argc) return lc_number(r);
    if (argc == 1) return lc_number(1 + floor(r * lc_to_number(ctx, argv[0], &ok)));
    { int lo = (int)lc_to_number(ctx, argv[0], &ok), hi = (int)lc_to_number(ctx, argv[1], &ok); return lc_number(lo + floor(r * (hi - lo + 1))); }
}
static LcValue lc_math_randomseed(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int ok; if (argc) srand((unsigned)lc_to_number(ctx, argv[0], &ok)); (void)self; return lc_nil();
}

static LcValue lc_math_other(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int ok;
    double x = lc_to_number(ctx, argc ? argv[0] : lc_nil(), &ok);
    if (!strcmp(self->name, "fmod")) return lc_number(fmod(x, lc_to_number(ctx, argc > 1 ? argv[1] : lc_nil(), &ok)));
    if (!strcmp(self->name, "pow")) return lc_number(pow(x, lc_to_number(ctx, argc > 1 ? argv[1] : lc_nil(), &ok)));
    if (!strcmp(self->name, "atan2")) return lc_number(atan2(x, lc_to_number(ctx, argc > 1 ? argv[1] : lc_nil(), &ok)));
    if (!strcmp(self->name, "ldexp")) return lc_number(ldexp(x, (int)lc_to_number(ctx, argc > 1 ? argv[1] : lc_nil(), &ok)));
    if (!strcmp(self->name, "modf")) {
        double integral, fraction = modf(x, &integral); LcValue out[2];
        out[0] = lc_number(integral); out[1] = lc_number(fraction); return lc_tuple_new(ctx, 2, out, 1);
    }
    if (!strcmp(self->name, "frexp")) {
        int exponent; double mantissa = frexp(x, &exponent); LcValue out[2];
        out[0] = lc_number(mantissa); out[1] = lc_number(exponent); return lc_tuple_new(ctx, 2, out, 1);
    }
    if (!strcmp(self->name, "tointeger")) {
        LcValue value = argc ? lc_first_value(argv[0]) : lc_nil();
        if (value.type == LC_INTEGER) return lc_retain(value);
        if (!isfinite(x) || floor(x) != x || x < -9223372036854775808.0 || x >= 9223372036854775808.0) return lc_nil();
        return lc_integer((int64_t)x);
    }
    if (!strcmp(self->name, "type")) { LcValue value = argc ? lc_first_value(argv[0]) : lc_nil(); return lc_string(ctx, value.type == LC_INTEGER ? "integer" : "float"); }
    if (!strcmp(self->name, "ult")) return lc_bool((uint64_t)(int64_t)x < (uint64_t)lc_to_integer(ctx, argc > 1 ? argv[1] : lc_nil()));
    return lc_nil();
}

static uint32_t lc_bit_arg(LcContext *ctx, LcValue value) { return (uint32_t)lc_to_integer(ctx, value); }
static LcValue lc_bit_unary(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    uint32_t x = lc_bit_arg(ctx, argc ? argv[0] : lc_nil());
    if (!strcmp(self->name, "tobit")) return lc_number((double)(int32_t)x);
    if (!strcmp(self->name, "bnot")) return lc_number((double)(uint32_t)~x);
    if (!strcmp(self->name, "bswap")) return lc_number((double)(uint32_t)((x >> 24) | ((x >> 8) & 0xff00) | ((x << 8) & 0xff0000) | (x << 24)));
    return lc_nil();
}
static LcValue lc_bit_nary(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    uint32_t value = !strcmp(self->name, "band") ? UINT32_MAX : 0;
    int i;
    for (i = 0; i < argc; ++i) {
        uint32_t x = lc_bit_arg(ctx, argv[i]);
        if (!strcmp(self->name, "band")) value &= x;
        else if (!strcmp(self->name, "bor")) value |= x;
        else value ^= x;
    }
    return lc_number((double)value);
}
static LcValue lc_bit_shift(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    uint32_t x = lc_bit_arg(ctx, argc ? argv[0] : lc_nil());
    int64_t count = lc_to_integer(ctx, argc > 1 ? argv[1] : lc_nil());
    uint64_t magnitude = count < 0 ? (uint64_t)(-(count + 1)) + 1 : (uint64_t)count;
    unsigned shift = (unsigned)(magnitude & 31u);
    if (!strcmp(self->name, "lshift")) return lc_number(count < 0 ? (double)(x >> shift) : (double)(uint32_t)(x << shift));
    if (!strcmp(self->name, "rshift")) return lc_number(count < 0 ? (double)(uint32_t)(x << shift) : (double)(x >> shift));
    if (!strcmp(self->name, "arshift")) {
        int32_t signed_x = (int32_t)x;
        return lc_number(count < 0 ? (double)(uint32_t)(x << shift) : (double)(int32_t)(signed_x >> shift));
    }
    if (!strcmp(self->name, "rol") || !strcmp(self->name, "lrotate")) return lc_number((double)(uint32_t)((x << shift) | (x >> ((32 - shift) & 31))));
    return lc_number((double)(uint32_t)((x >> shift) | (x << ((32 - shift) & 31))));
}
static LcValue lc_bit32_shift(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    uint32_t x = lc_bit_arg(ctx, argc ? argv[0] : lc_nil());
    int64_t count = lc_to_integer(ctx, argc > 1 ? argv[1] : lc_nil());
    uint64_t magnitude = count < 0 ? (uint64_t)(-(count + 1)) + 1 : (uint64_t)count;
    int left = !strcmp(self->name, "lshift");
    if (count < 0) left = !left;
    if (magnitude >= 32) {
        if (strcmp(self->name, "arshift") == 0 && !left && (int32_t)x < 0) return lc_number(-1);
        return lc_number(0);
    }
    if (left) return lc_number((double)(uint32_t)(x << (unsigned)magnitude));
    if (!strcmp(self->name, "arshift")) return lc_number((double)(int32_t)((int32_t)x >> (unsigned)magnitude));
    return lc_number((double)(x >> (unsigned)magnitude));
}
static LcValue lc_bit_tohex(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    uint32_t x = lc_bit_arg(ctx, argc ? argv[0] : lc_nil());
    int digits = argc > 1 ? (int)lc_to_integer(ctx, argv[1]) : 8;
    int lower = digits < 0, i;
    char buffer[17];
    if (digits < 0) digits = -digits;
    if (digits > 16) digits = 16;
    for (i = 0; i < digits; ++i) {
        int shift = (digits - i - 1) * 4;
        unsigned nibble = shift < 32 ? (x >> shift) & 15u : 0;
        buffer[i] = (char)(nibble < 10 ? '0' + nibble : (lower ? 'a' : 'A') + nibble - 10);
    }
    buffer[digits] = 0;
    return lc_string(ctx, buffer);
}
static LcValue lc_bit_extract(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    uint32_t x = lc_bit_arg(ctx, argc ? argv[0] : lc_nil()), value;
    int replace = !strcmp(self->name, "replace");
    int field = argc > (replace ? 2 : 1) ? (int)lc_to_integer(ctx, argv[replace ? 2 : 1]) : 0;
    int width = argc > (replace ? 3 : 2) ? (int)lc_to_integer(ctx, argv[replace ? 3 : 2]) : 1;
    if (field < 0 || width <= 0 || field + width > 32) { lc_error(ctx, "invalid bit field"); return lc_nil(); }
    if (replace) {
        value = lc_bit_arg(ctx, argc > 1 ? argv[1] : lc_nil());
        { uint32_t mask = width == 32 ? UINT32_MAX : ((UINT32_C(1) << width) - 1) << field;
          return lc_number((double)((x & ~mask) | ((value << field) & mask))); }
    }
    value = width == 32 ? x : (x >> field) & ((UINT32_C(1) << width) - 1);
    return lc_number((double)value);
}
static LcValue lc_bit_btest(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    uint32_t value = UINT32_MAX; int i; (void)self;
    for (i = 0; i < argc; ++i) value &= lc_bit_arg(ctx, argv[i]);
    return lc_bool(value != 0);
}

static LcValue lc_string_len(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self; if (!argc || lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "string expected"); return lc_nil(); } return lc_number((double)lc_first_value(argv[0]).as.string->length);
}
static LcValue lc_string_sub(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue v; int len, start, end, ok; (void)self;
    if (!argc || lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "string expected"); return lc_nil(); }
    v = lc_first_value(argv[0]); len = (int)v.as.string->length;
    start = argc > 1 ? (int)lc_to_number(ctx, argv[1], &ok) : 1; end = argc > 2 ? (int)lc_to_number(ctx, argv[2], &ok) : -1;
    if (start < 0) start = len + start + 1;
    if (end < 0) end = len + end + 1;
    if (start < 1) start = 1;
    if (end > len) end = len;
    if (start > end || start > len) return lc_string(ctx, "");
    return lc_string_n(ctx, v.as.string->data + start - 1, (size_t)(end - start + 1));
}
static LcValue lc_string_case(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue v, r; size_t i; int upper = !strcmp(self->name, "upper"); (void)self;
    if (!argc || lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "string expected"); return lc_nil(); }
    v = lc_first_value(argv[0]); r = lc_string_n(ctx, v.as.string->data, v.as.string->length);
    for (i = 0; i < r.as.string->length; ++i) r.as.string->data[i] = (char)(upper ? toupper((unsigned char)r.as.string->data[i]) : tolower((unsigned char)r.as.string->data[i]));
    return r;
}
static LcValue lc_string_rep(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue v, r; int n, ok; size_t i, len; char *s; (void)self;
    if (argc < 2 || lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "string expected"); return lc_nil(); }
    v = lc_first_value(argv[0]); n = (int)lc_to_number(ctx, argv[1], &ok); if (n < 0) n = 0;
    {
        const char *separator = argc > 2 && lc_first_value(argv[2]).type == LC_STRING ? lc_first_value(argv[2]).as.string->data : "";
        size_t separator_length = argc > 2 && lc_first_value(argv[2]).type == LC_STRING ? lc_first_value(argv[2]).as.string->length : 0;
        if (n > 0 && ((v.as.string->length && (size_t)n > SIZE_MAX / v.as.string->length) ||
            (separator_length && n > 1 && (size_t)(n - 1) > SIZE_MAX / separator_length))) { lc_error(ctx, "string.rep result is too large"); return lc_nil(); }
        len = v.as.string->length * (size_t)n + (n > 0 ? (size_t)(n - 1) * separator_length : 0);
        if (len < v.as.string->length) { lc_error(ctx, "string.rep result is too large"); return lc_nil(); }
        s = malloc(len + 1); if (!s) { lc_error(ctx, "out of memory"); return lc_nil(); }
        { size_t offset = 0;
          for (i = 0; i < (size_t)n; ++i) {
              if (i && separator_length) { memcpy(s + offset, separator, separator_length); offset += separator_length; }
              memcpy(s + offset, v.as.string->data, v.as.string->length); offset += v.as.string->length;
          }
        }
    }
    r = lc_string_n(ctx, s, len); free(s); return r;
}

/* Independent Lua-pattern implementation. All offsets are byte offsets. */
typedef struct {
    LcContext *ctx;
    const char *source, *pattern;
    size_t source_length, pattern_length;
    int level;
    const char *capture[32][2];
    unsigned char kind[32]; /* 0 unused, 1 open, 2 closed, 3 position */
} LcPattern;

typedef struct { char *data; size_t length, capacity; } LcTextBuffer;
static void lc_text_need(LcContext *ctx, LcTextBuffer *b, size_t extra) {
    size_t needed = b->length + extra + 1;
    if (needed <= b->capacity) return;
    b->capacity = b->capacity ? b->capacity : 128;
    while (b->capacity < needed) b->capacity *= 2;
    b->data = (char *)realloc(b->data, b->capacity);
    if (!b->data) { b->capacity = 0; lc_error(ctx, "out of memory building string"); }
}
static void lc_text_add(LcContext *ctx, LcTextBuffer *b, const char *s, size_t n) {
    if (!n || ctx->error) return;
    lc_text_need(ctx, b, n);
    if (!b->data) return;
    memcpy(b->data + b->length, s, n); b->length += n; b->data[b->length] = 0;
}
static void lc_text_free(LcTextBuffer *b) { free(b->data); memset(b, 0, sizeof(*b)); }

static int lc_pat_class(int c, int kind) {
    int result;
    switch (tolower((unsigned char)kind)) {
        case 'a': result = isalpha(c); break; case 'c': result = iscntrl(c); break;
        case 'd': result = isdigit(c); break; case 'g': result = isgraph(c); break;
        case 'l': result = islower(c); break; case 'p': result = ispunct(c); break;
        case 's': result = isspace(c); break; case 'u': result = isupper(c); break;
        case 'w': result = isalnum(c); break; case 'x': result = isxdigit(c); break;
        default: return c == kind;
    }
    return isupper((unsigned char)kind) ? !result : result;
}
static const char *lc_pat_set_end(const char *p, const char *end) {
    const char *s = p + 1;
    if (s < end && *s == '^') ++s;
    if (s < end && *s == ']') ++s;
    while (s < end) {
        if (*s == '%' && s + 1 < end) s += 2;
        else if (*s++ == ']') return s;
    }
    return end;
}
static int lc_pat_in_set(int c, const char *begin, const char *end) {
    int negate = 0, matched = 0;
    const char *p = begin + 1;
    if (p < end && *p == '^') { negate = 1; ++p; }
    while (p < end) {
        if (*p == '%' && p + 1 < end) { if (lc_pat_class(c, p[1])) matched = 1; p += 2; }
        else if (p + 2 < end && p[1] == '-') {
            if (c >= (unsigned char)p[0] && c <= (unsigned char)p[2]) matched = 1;
            p += 3;
        } else { if (c == (unsigned char)*p) matched = 1; ++p; }
    }
    return negate ? !matched : matched;
}
static const char *lc_pat_item_end(const char *p, const char *end) {
    if (p >= end) return end;
    if (*p == '[') return lc_pat_set_end(p, end);
    if (*p == '%' && p + 1 < end) return p + 2;
    return p + 1;
}
static int lc_pat_one(LcPattern *m, const char *s, const char *p, const char *end) {
    int c;
    if (s >= m->source + m->source_length || p >= end) return 0;
    c = (unsigned char)*s;
    if (*p == '.') return 1;
    if (*p == '[') {
        const char *close = lc_pat_set_end(p, end);
        if (close <= p + 1 || close[-1] != ']') { lc_error(m->ctx, "malformed pattern (missing ']')"); return 0; }
        return lc_pat_in_set(c, p, close - 1);
    }
    if (*p == '%' && p + 1 < end) return lc_pat_class(c, p[1]);
    return c == (unsigned char)*p;
}
static const char *lc_pat_match(LcPattern *m, const char *s, const char *p);
static const char *lc_pat_match_inner(LcPattern *m, const char *s, const char *p) {
    const char *source_end = m->source + m->source_length;
    const char *pattern_end = m->pattern + m->pattern_length;
    if (p >= pattern_end) return s;
    if (*p == '(') {
        int index;
        if (m->level >= 32) { lc_error(m->ctx, "too many captures"); return NULL; }
        index = m->level++;
        if (p + 1 < pattern_end && p[1] == ')') {
            m->kind[index] = 3; m->capture[index][0] = m->capture[index][1] = s;
            return lc_pat_match(m, s, p + 2);
        }
        m->kind[index] = 1; m->capture[index][0] = s; m->capture[index][1] = NULL;
        return lc_pat_match(m, s, p + 1);
    }
    if (*p == ')') {
        int i;
        for (i = m->level - 1; i >= 0; --i) if (m->kind[i] == 1) {
            m->kind[i] = 2; m->capture[i][1] = s;
            return lc_pat_match(m, s, p + 1);
        }
        lc_error(m->ctx, "invalid pattern capture"); return NULL;
    }
    if (*p == '$' && p + 1 == pattern_end) return s == source_end ? s : NULL;
    if (*p == '%' && p + 1 < pattern_end && p[1] == 'b') {
        const char *next;
        int balance = 1;
        unsigned char open, close;
        if (p + 3 >= pattern_end) { lc_error(m->ctx, "malformed pattern (missing arguments to '%b')"); return NULL; }
        open = (unsigned char)p[2]; close = (unsigned char)p[3];
        if (s >= source_end || (unsigned char)*s != open) return NULL;
        next = s + 1;
        while (next < source_end) {
            if ((unsigned char)*next == close && --balance == 0) return lc_pat_match(m, next + 1, p + 4);
            if ((unsigned char)*next == open) ++balance;
            ++next;
        }
        return NULL;
    }
    if (*p == '%' && p + 1 < pattern_end && p[1] == 'f') {
        const char *set = p + 2, *close;
        int previous = s == m->source ? 0 : (unsigned char)s[-1];
        int current = s == source_end ? 0 : (unsigned char)*s;
        if (set >= pattern_end || *set != '[') { lc_error(m->ctx, "missing '[' after '%f'"); return NULL; }
        close = lc_pat_set_end(set, pattern_end);
        if (close <= set + 1 || close[-1] != ']') { lc_error(m->ctx, "malformed frontier pattern"); return NULL; }
        if (!lc_pat_in_set(previous, set, close - 1) && lc_pat_in_set(current, set, close - 1))
            return lc_pat_match(m, s, close);
        return NULL;
    }
    if (*p == '%' && p + 1 < pattern_end && isdigit((unsigned char)p[1]) && p[1] != '0') {
        int index = p[1] - '1';
        size_t length;
        if (index >= m->level || m->kind[index] != 2) { lc_error(m->ctx, "invalid capture index"); return NULL; }
        length = (size_t)(m->capture[index][1] - m->capture[index][0]);
        if ((size_t)(source_end - s) >= length && !memcmp(s, m->capture[index][0], length))
            return lc_pat_match(m, s + length, p + 2);
        return NULL;
    }
    {
        const char *item_end = lc_pat_item_end(p, pattern_end);
        char quantifier = item_end < pattern_end ? *item_end : 0;
        const char *rest = quantifier == '?' || quantifier == '*' || quantifier == '+' || quantifier == '-' ? item_end + 1 : item_end;
        if (quantifier == '?') {
            const char *result;
            if (lc_pat_one(m, s, p, item_end)) { result = lc_pat_match(m, s + 1, rest); if (result) return result; }
            return lc_pat_match(m, s, rest);
        }
        if (quantifier == '-') {
            for (;;) {
                const char *result = lc_pat_match(m, s, rest);
                if (result) return result;
                if (!lc_pat_one(m, s, p, item_end)) return NULL;
                ++s;
            }
        }
        if (quantifier == '*' || quantifier == '+') {
            const char *cursor = s;
            if (quantifier == '+') { if (!lc_pat_one(m, cursor, p, item_end)) return NULL; ++cursor; }
            while (lc_pat_one(m, cursor, p, item_end)) ++cursor;
            for (;;) {
                const char *result = lc_pat_match(m, cursor, rest);
                if (result) return result;
                if (cursor == s || (quantifier == '+' && cursor == s + 1)) return NULL;
                --cursor;
            }
        }
        if (!lc_pat_one(m, s, p, item_end)) return NULL;
        return lc_pat_match(m, s + 1, rest);
    }
}
static const char *lc_pat_match(LcPattern *m, const char *s, const char *p) {
    int level = m->level;
    const char *captures[32][2];
    unsigned char kinds[32];
    const char *result;
    memcpy(captures, m->capture, sizeof(captures)); memcpy(kinds, m->kind, sizeof(kinds));
    result = lc_pat_match_inner(m, s, p);
    if (!result) { m->level = level; memcpy(m->capture, captures, sizeof(captures)); memcpy(m->kind, kinds, sizeof(kinds)); }
    return result;
}
static int lc_pat_search(LcPattern *m, size_t initial, int plain, const char **start, const char **end) {
    size_t at;
    int anchored = !plain && m->pattern_length && m->pattern[0] == '^';
    for (at = initial; at <= m->source_length; ++at) {
        m->level = 0; memset(m->kind, 0, sizeof(m->kind));
        *start = m->source + at;
        if (plain) {
            if (m->pattern_length > m->source_length - at) { if (anchored) break; else continue; }
            if (!m->pattern_length || !memcmp(*start, m->pattern, m->pattern_length)) { *end = *start + m->pattern_length; return 1; }
        } else {
            const char *pattern = m->pattern + (anchored ? 1 : 0);
            *end = lc_pat_match(m, *start, pattern);
            if (*end) return 1;
            if (m->ctx->error) return 0;
        }
        if (anchored) break;
    }
    return 0;
}
static LcValue lc_pat_capture(LcPattern *m, int index, const char *start, const char *end) {
    if (index < 0) return lc_string_n(m->ctx, start, (size_t)(end - start));
    if (m->kind[index] == 3) return lc_number((double)(m->capture[index][0] - m->source + 1));
    if (m->kind[index] != 2) { lc_error(m->ctx, "unfinished capture"); return lc_nil(); }
    return lc_string_n(m->ctx, m->capture[index][0], (size_t)(m->capture[index][1] - m->capture[index][0]));
}
static LcValue lc_pat_results(LcPattern *m, const char *start, const char *end) {
    int i;
    if (!m->level) return lc_pat_capture(m, -1, start, end);
    {
        LcValue values[32];
        for (i = 0; i < m->level; ++i) values[i] = lc_pat_capture(m, i, start, end);
        return lc_tuple_new(m->ctx, m->level, values, 1);
    }
}
static int lc_pat_initial(LcContext *ctx, int argc, LcValue *argv, size_t length) {
    int initial = argc > 2 ? (int)lc_to_integer(ctx, argv[2]) : 1;
    if (initial < 0) initial = (int)length + initial + 1;
    if (initial < 1) initial = 1;
    return initial > (int)length + 1 ? -1 : initial - 1;
}
static LcValue lc_string_byte(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue value; int length, start, end, count, i; LcValue *items;
    (void)self;
    if (!argc || lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "string expected"); return lc_nil(); }
    value = lc_first_value(argv[0]); length = (int)value.as.string->length;
    start = argc > 1 ? (int)lc_to_integer(ctx, argv[1]) : 1; end = argc > 2 ? (int)lc_to_integer(ctx, argv[2]) : start;
    if (start < 0) start = length + start + 1; if (end < 0) end = length + end + 1;
    if (start < 1) start = 1; if (end > length) end = length;
    if (start > end) return lc_tuple_new(ctx, 0, NULL, 0);
    count = end - start + 1; items = (LcValue *)malloc(sizeof(LcValue) * (size_t)count);
    if (!items) { lc_error(ctx, "out of memory in string.byte"); return lc_nil(); }
    for (i = 0; i < count; ++i) items[i] = lc_number((unsigned char)value.as.string->data[start - 1 + i]);
    { LcValue result = lc_tuple_new(ctx, count, items, 1); free(items); return result; }
}
static LcValue lc_string_char(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    char *bytes = (char *)malloc((size_t)(argc ? argc : 1)); LcValue result; int i;
    (void)self;
    if (!bytes) { lc_error(ctx, "out of memory in string.char"); return lc_nil(); }
    for (i = 0; i < argc; ++i) { int64_t c = lc_to_integer(ctx, argv[i]); if (c < 0 || c > 255) { free(bytes); lc_error(ctx, "value out of range in string.char"); return lc_nil(); } bytes[i] = (char)c; }
    result = lc_string_n(ctx, bytes, (size_t)argc); free(bytes); return result;
}
static LcValue lc_string_reverse(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue value, result; size_t i, length; char *bytes;
    (void)self;
    if (!argc || lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "string expected"); return lc_nil(); }
    value = lc_first_value(argv[0]); length = value.as.string->length; bytes = (char *)malloc(length ? length : 1);
    if (!bytes) { lc_error(ctx, "out of memory in string.reverse"); return lc_nil(); }
    for (i = 0; i < length; ++i) bytes[i] = value.as.string->data[length - i - 1];
    result = lc_string_n(ctx, bytes, length); free(bytes); return result;
}
static LcValue lc_string_format(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue format; LcTextBuffer output; size_t at = 0; int argument = 1;
    (void)self;
    if (!argc || lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "format string expected"); return lc_nil(); }
    format = lc_first_value(argv[0]); memset(&output, 0, sizeof(output));
    while (at < format.as.string->length && !ctx->error) {
        char c = format.as.string->data[at];
        if (c != '%') { lc_text_add(ctx, &output, &c, 1); ++at; continue; }
        if (++at >= format.as.string->length) { lc_error(ctx, "invalid format (ends with '%')"); break; }
        c = format.as.string->data[at];
        if (c == '%') { lc_text_add(ctx, &output, &c, 1); ++at; continue; }
        {
            char spec[40], integer_spec[48]; size_t length = 0; const char *value_format = format.as.string->data + at;
            spec[length++] = '%';
            while (at < format.as.string->length && strchr("-+ #0", format.as.string->data[at]) && length < 12) spec[length++] = format.as.string->data[at++];
            while (at < format.as.string->length && isdigit((unsigned char)format.as.string->data[at]) && length < 20) spec[length++] = format.as.string->data[at++];
            if (at < format.as.string->length && format.as.string->data[at] == '.') {
                spec[length++] = format.as.string->data[at++];
                while (at < format.as.string->length && isdigit((unsigned char)format.as.string->data[at]) && length < 28) spec[length++] = format.as.string->data[at++];
            }
            if (at >= format.as.string->length) { lc_error(ctx, "invalid format"); break; }
            c = format.as.string->data[at++]; spec[length++] = c; spec[length] = 0;
            if (!strchr("cdiouxXeEfFgGaAqs", c)) { lc_error(ctx, "invalid format option '%%%s'", spec); break; }
            if (argument >= argc + 1) { lc_error(ctx, "not enough arguments for string.format"); break; }
            {
                LcValue value = lc_first_value(argv[argument++]); char buffer[1024]; int written = 0;
                if (c == 's') {
                    LcValue string = lc_to_string(ctx, value);
                    if (string.type == LC_STRING) {
                        if (strchr(spec, '.') || strncmp(spec, "%s", 2)) written = snprintf(buffer, sizeof(buffer), spec, string.as.string->data);
                        else { lc_text_add(ctx, &output, string.as.string->data, string.as.string->length); written = -2; }
                    }
                    lc_release(ctx, string);
                } else if (c == 'q') {
                    size_t i;
                    if (value.type == LC_STRING) {
                        lc_text_add(ctx, &output, "\"", 1);
                        for (i = 0; i < value.as.string->length; ++i) {
                            unsigned char ch = (unsigned char)value.as.string->data[i]; char escaped[8];
                            if (ch == '\"' || ch == '\\') { lc_text_add(ctx, &output, "\\", 1); lc_text_add(ctx, &output, (const char *)&ch, 1); }
                            else if (ch == '\n') lc_text_add(ctx, &output, "\\n", 2);
                            else if (ch == '\r') lc_text_add(ctx, &output, "\\r", 2);
                            else if (ch < 32 || ch == 127) { snprintf(escaped, sizeof(escaped), "\\%03u", (unsigned)ch); lc_text_add(ctx, &output, escaped, 4); }
                            else lc_text_add(ctx, &output, (const char *)&ch, 1);
                        }
                        lc_text_add(ctx, &output, "\"", 1);
                    } else if (value.type == LC_NIL || value.type == LC_BOOL || value.type == LC_NUMBER || value.type == LC_INTEGER) {
                        LcValue string = lc_to_string(ctx, value); lc_text_add(ctx, &output, string.as.string->data, string.as.string->length); lc_release(ctx, string);
                    } else lc_error(ctx, "%q expects a string, number, boolean or nil");
                    written = -2;
                } else if (c == 'c') {
                    char ch = (char)lc_to_integer(ctx, value); lc_text_add(ctx, &output, &ch, 1); written = -2;
                } else if (strchr("diouxX", c)) {
                    int64_t integer = lc_to_integer(ctx, value);
                    size_t prefix = length - 1;
                    memcpy(integer_spec, spec, prefix); strcpy(integer_spec + prefix, "ll"); strcat(integer_spec, spec + prefix);
                    if (c == 'd' || c == 'i') written = snprintf(buffer, sizeof(buffer), integer_spec, (long long)integer);
                    else written = snprintf(buffer, sizeof(buffer), integer_spec, (unsigned long long)(uint64_t)integer);
                } else {
                    double number = lc_to_number(ctx, value, &written);
                    written = snprintf(buffer, sizeof(buffer), spec, number);
                }
                if (written >= (int)sizeof(buffer)) {
                    int needed = written + 1; char *large = (char *)malloc((size_t)needed);
                    if (!large) { lc_error(ctx, "out of memory in string.format"); break; }
                    /* Re-evaluate the single argument with a correctly sized destination. */
                    if (c == 's') { LcValue string = lc_to_string(ctx, value); snprintf(large, (size_t)needed, spec, string.as.string->data); lc_release(ctx, string); }
                    else if (strchr("di", c)) snprintf(large, (size_t)needed, integer_spec, (long long)lc_to_integer(ctx, value));
                    else if (strchr("ouxX", c)) snprintf(large, (size_t)needed, integer_spec, (unsigned long long)(uint64_t)lc_to_integer(ctx, value));
                    else { int ok; double number = lc_to_number(ctx, value, &ok); snprintf(large, (size_t)needed, spec, number); }
                    lc_text_add(ctx, &output, large, (size_t)written); free(large);
                } else if (written >= 0) lc_text_add(ctx, &output, buffer, (size_t)written);
            }
        }
    }
    { LcValue result = lc_string_n(ctx, output.data ? output.data : "", output.length); lc_text_free(&output); return result; }
}

static LcValue lc_string_find(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcPattern match; LcValue source, pattern, captures, out[34];
    const char *start, *end; int initial, plain, count, i;
    (void)self;
    if (argc < 2 || lc_first_value(argv[0]).type != LC_STRING || lc_first_value(argv[1]).type != LC_STRING) { lc_error(ctx, "string and pattern expected"); return lc_nil(); }
    source = lc_first_value(argv[0]); pattern = lc_first_value(argv[1]);
    memset(&match, 0, sizeof(match)); match.ctx = ctx; match.source = source.as.string->data; match.source_length = source.as.string->length;
    match.pattern = pattern.as.string->data; match.pattern_length = pattern.as.string->length;
    initial = lc_pat_initial(ctx, argc, argv, match.source_length); if (initial < 0) return lc_nil();
    plain = (argc > 3 && lc_truthy(argv[3])) || !strcmp(self->name, "plainfind");
    if (!lc_pat_search(&match, (size_t)initial, plain, &start, &end)) return lc_nil();
    if (!strcmp(self->name, "match")) return lc_pat_results(&match, start, end);
    out[0] = lc_number(start - match.source + 1); out[1] = lc_number(end - match.source);
    captures = lc_pat_results(&match, start, end); count = 2;
    if (match.level) {
        if (captures.type == LC_TUPLE) for (i = 0; i < captures.as.tuple->count; ++i) out[count++] = lc_retain(captures.as.tuple->items[i]);
        else out[count++] = lc_retain(captures);
    }
    lc_release(ctx, captures); return lc_tuple_new(ctx, count, out, 1);
}
static LcValue lc_string_gmatch_iter(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcPattern match; LcValue source = lc_cell_get(self->upvalues[0]), pattern = lc_cell_get(self->upvalues[1]);
    LcValue position = lc_cell_get(self->upvalues[2]), result;
    const char *start, *end; size_t initial = (size_t)position.as.number;
    (void)argc; (void)argv;
    memset(&match, 0, sizeof(match)); match.ctx = ctx; match.source = source.as.string->data; match.source_length = source.as.string->length;
    match.pattern = pattern.as.string->data; match.pattern_length = pattern.as.string->length;
    if (initial > match.source_length || !lc_pat_search(&match, initial, 0, &start, &end)) {
        lc_release(ctx, source); lc_release(ctx, pattern); lc_release(ctx, position); return lc_nil();
    }
    result = lc_pat_results(&match, start, end);
    lc_cell_set(ctx, self->upvalues[2], lc_number((double)((end - match.source) + (end == start))));
    lc_release(ctx, source); lc_release(ctx, pattern); lc_release(ctx, position); return result;
}
static LcValue lc_string_gmatch(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue source, pattern, result; LcCell *upvalues[3]; int i;
    (void)self;
    if (argc < 2 || lc_first_value(argv[0]).type != LC_STRING || lc_first_value(argv[1]).type != LC_STRING) { lc_error(ctx, "string and pattern expected"); return lc_nil(); }
    source = lc_retain(lc_first_value(argv[0])); pattern = lc_retain(lc_first_value(argv[1]));
    upvalues[0] = lc_cell_new(ctx, lc_retain(source));
    upvalues[1] = lc_cell_new(ctx, lc_retain(pattern));
    upvalues[2] = lc_cell_new(ctx, lc_number(0));
    result = lc_function_new(ctx, lc_string_gmatch_iter, 3, upvalues, "gmatch iterator");
    for (i = 0; i < 3; ++i) lc_cell_release(ctx, upvalues[i]);
    lc_release(ctx, source); lc_release(ctx, pattern); return result;
}
static void lc_pat_replacement(LcPattern *m, LcTextBuffer *buffer, LcValue replacement, const char *start, const char *end) {
    LcContext *ctx = m->ctx;
    replacement = lc_first_value(replacement);
    if (replacement.type == LC_STRING) {
        size_t i;
        for (i = 0; i < replacement.as.string->length; ++i) {
            char c = replacement.as.string->data[i];
            if (c != '%') { lc_text_add(ctx, buffer, &c, 1); continue; }
            if (++i >= replacement.as.string->length) { lc_error(ctx, "invalid use of '%' in replacement string"); return; }
            c = replacement.as.string->data[i];
            if (c == '%') lc_text_add(ctx, buffer, &c, 1);
            else if (isdigit((unsigned char)c)) {
                int index = c - '0'; LcValue value;
                if (index == 0) value = lc_string_n(ctx, start, (size_t)(end - start));
                else if (index <= m->level) value = lc_pat_capture(m, index - 1, start, end);
                else { lc_error(ctx, "invalid capture index in replacement"); return; }
                if (value.type == LC_STRING) lc_text_add(ctx, buffer, value.as.string->data, value.as.string->length);
                else { LcValue string = lc_to_string(ctx, value); lc_text_add(ctx, buffer, string.as.string->data, string.as.string->length); lc_release(ctx, string); }
                lc_release(ctx, value);
            } else { lc_error(ctx, "invalid use of '%' in replacement string"); return; }
        }
        return;
    }
    {
        LcValue key = m->level ? lc_pat_capture(m, 0, start, end) : lc_string_n(ctx, start, (size_t)(end - start)), value;
        if (replacement.type == LC_TABLE) value = lc_table_get(ctx, replacement, key);
        else if (replacement.type == LC_FUNCTION) {
            if (m->level) {
                LcValue args[32], tuple = lc_pat_results(m, start, end); int i, n = m->level;
                for (i = 0; i < n; ++i) args[i] = lc_nth(tuple, i);
                value = lc_call(ctx, replacement, n, args);
                for (i = 0; i < n; ++i) lc_release(ctx, args[i]);
                lc_release(ctx, tuple);
            } else value = lc_call(ctx, replacement, 1, &key);
        } else { lc_release(ctx, key); lc_error(ctx, "replacement must be string, table or function"); return; }
        lc_release(ctx, key);
        if (!lc_truthy(value)) lc_text_add(ctx, buffer, start, (size_t)(end - start));
        else {
            LcValue string = lc_to_string(ctx, value);
            if (string.type == LC_STRING) lc_text_add(ctx, buffer, string.as.string->data, string.as.string->length);
            else lc_error(ctx, "invalid replacement value");
            lc_release(ctx, string);
        }
        lc_release(ctx, value);
    }
}
static LcValue lc_string_gsub(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcPattern match; LcTextBuffer buffer; LcValue source, pattern, replacement, result, out[2];
    size_t position = 0; int replacements = 0, maximum, anchored;
    (void)self;
    if (argc < 3 || lc_first_value(argv[0]).type != LC_STRING || lc_first_value(argv[1]).type != LC_STRING) { lc_error(ctx, "string, pattern and replacement expected"); return lc_nil(); }
    source = lc_first_value(argv[0]); pattern = lc_first_value(argv[1]); replacement = lc_first_value(argv[2]);
    memset(&match, 0, sizeof(match)); memset(&buffer, 0, sizeof(buffer)); match.ctx = ctx;
    match.source = source.as.string->data; match.source_length = source.as.string->length;
    match.pattern = pattern.as.string->data; match.pattern_length = pattern.as.string->length;
    maximum = argc > 3 ? (int)lc_to_integer(ctx, argv[3]) : (int)match.source_length + 1;
    anchored = match.pattern_length && match.pattern[0] == '^';
    while (position <= match.source_length && replacements < maximum && !ctx->error) {
        const char *start, *end;
        if (!lc_pat_search(&match, position, 0, &start, &end)) break;
        lc_text_add(ctx, &buffer, match.source + position, (size_t)(start - (match.source + position)));
        lc_pat_replacement(&match, &buffer, replacement, start, end);
        ++replacements;
        if (end > start) position = (size_t)(end - match.source);
        else {
            position = (size_t)(end - match.source);
            if (position < match.source_length) lc_text_add(ctx, &buffer, match.source + position, 1);
            ++position;
        }
        if (anchored) break;
    }
    if (position < match.source_length) lc_text_add(ctx, &buffer, match.source + position, match.source_length - position);
    result = lc_string_n(ctx, buffer.data ? buffer.data : "", buffer.length);
    lc_text_free(&buffer);
    out[0] = result; out[1] = lc_number(replacements); return lc_tuple_new(ctx, 2, out, 1);
}

static LcValue lc_table_insert(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int n, pos, i; (void)self;
    if (argc < 2 || argv[0].type != LC_TABLE) { lc_error(ctx, "table expected"); return lc_nil(); }
    n = lc_table_length(argv[0]); pos = argc > 2 ? (int)lc_to_number(ctx, argv[1], &(int){0}) : n + 1;
    for (i = n; i >= pos; --i) { LcValue v = lc_table_get(ctx, argv[0], lc_number(i)); lc_table_set(ctx, argv[0], lc_number(i + 1), v); lc_release(ctx, v); }
    lc_table_set(ctx, argv[0], lc_number(pos), argv[argc - 1]); return lc_nil();
}
static LcValue lc_table_remove(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int n, pos, i; LcValue old; (void)self;
    if (!argc || argv[0].type != LC_TABLE) { lc_error(ctx, "table expected"); return lc_nil(); }
    n = lc_table_length(argv[0]); pos = argc > 1 ? (int)lc_to_number(ctx, argv[1], &(int){0}) : n;
    old = lc_table_get(ctx, argv[0], lc_number(pos));
    for (i = pos; i < n; ++i) { LcValue v = lc_table_get(ctx, argv[0], lc_number(i + 1)); lc_table_set(ctx, argv[0], lc_number(i), v); lc_release(ctx, v); }
    lc_table_set(ctx, argv[0], lc_number(n), lc_nil()); return old;
}
static LcValue lc_table_concat(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int i, n, start = 1, end; LcValue acc; (void)self;
    if (!argc || argv[0].type != LC_TABLE) { lc_error(ctx, "table expected"); return lc_nil(); }
    n = lc_table_length(argv[0]);
    end = argc > 3 ? (int)lc_to_number(ctx, argv[3], &(int){0}) : n;
    if (argc > 2 && argv[2].type != LC_NIL) start = (int)lc_to_number(ctx, argv[2], &(int){0});
    acc = lc_string(ctx, "");
    for (i = start; i <= end; ++i) {
        LcValue v = lc_table_get(ctx, argv[0], lc_number(i)), s = lc_to_string(ctx, v), next;
        if (i > start && argc > 1) next = lc_binary(ctx, 13, acc, argv[1]); else next = lc_retain(acc);
        lc_release(ctx, acc); acc = lc_binary(ctx, 13, next, s);
        lc_release(ctx, next); lc_release(ctx, s); lc_release(ctx, v);
    }
    return acc;
}

static LcValue lc_table_pack(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue table = lc_table(ctx), key; int i;
    (void)self;
    for (i = 0; i < argc; ++i) lc_table_set(ctx, table, lc_number(i + 1), argv[i]);
    key = lc_string(ctx, "n");
    lc_table_set(ctx, table, key, lc_number(argc));
    lc_release(ctx, key);
    return table;
}
static LcValue lc_table_unpack(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int start = 1, end, count, i;
    LcValue *items, result;
    (void)self;
    if (!argc || lc_first_value(argv[0]).type != LC_TABLE) { lc_error(ctx, "table expected"); return lc_nil(); }
    end = lc_table_length(argv[0]);
    if (argc > 1) start = (int)lc_to_integer(ctx, argv[1]);
    if (argc > 2) end = (int)lc_to_integer(ctx, argv[2]);
    if (end < start) return lc_tuple_new(ctx, 0, NULL, 0);
    count = end - start + 1;
    if (count > 1000000) { lc_error(ctx, "too many results to unpack"); return lc_nil(); }
    items = (LcValue *)malloc(sizeof(LcValue) * (size_t)count);
    if (!items) { lc_error(ctx, "out of memory in table.unpack"); return lc_nil(); }
    for (i = 0; i < count; ++i) items[i] = lc_table_get(ctx, argv[0], lc_number(start + i));
    result = lc_tuple_new(ctx, count, items, 1);
    free(items); return result;
}
static LcValue lc_table_move(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int first, last, target, count, i;
    LcValue destination, *items;
    (void)self;
    if (argc < 3 || lc_first_value(argv[0]).type != LC_TABLE) { lc_error(ctx, "table.move expects source, first, last and target"); return lc_nil(); }
    first = (int)lc_to_integer(ctx, argv[1]); last = (int)lc_to_integer(ctx, argv[2]); target = (int)lc_to_integer(ctx, argv[3]);
    destination = argc > 4 ? argv[4] : argv[0];
    if (lc_first_value(destination).type != LC_TABLE) { lc_error(ctx, "destination table expected"); return lc_nil(); }
    if (last < first) return lc_retain(destination);
    count = last - first + 1;
    items = (LcValue *)malloc(sizeof(LcValue) * (size_t)count);
    if (!items) { lc_error(ctx, "out of memory in table.move"); return lc_nil(); }
    for (i = 0; i < count; ++i) items[i] = lc_table_get(ctx, argv[0], lc_number(first + i));
    for (i = 0; i < count; ++i) { lc_table_set(ctx, destination, lc_number(target + i), items[i]); lc_release(ctx, items[i]); }
    free(items); return lc_retain(destination);
}
static LcValue lc_table_sort(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int length, i;
    (void)self;
    if (!argc || lc_first_value(argv[0]).type != LC_TABLE) { lc_error(ctx, "table expected"); return lc_nil(); }
    length = lc_table_length(argv[0]);
    for (i = 2; i <= length && !ctx->error; ++i) {
        LcValue value = lc_table_get(ctx, argv[0], lc_number(i));
        int j = i - 1;
        while (j >= 1) {
            LcValue previous = lc_table_get(ctx, argv[0], lc_number(j)), less;
            if (argc > 1 && lc_first_value(argv[1]).type != LC_NIL) {
                LcValue args[2]; args[0] = value; args[1] = previous;
                less = lc_call(ctx, argv[1], 2, args);
            } else less = lc_binary(ctx, 9, value, previous);
            if (!lc_truthy(less)) { lc_release(ctx, previous); lc_release(ctx, less); break; }
            lc_release(ctx, less);
            lc_table_set(ctx, argv[0], lc_number(j + 1), previous);
            lc_release(ctx, previous);
            --j;
        }
        lc_table_set(ctx, argv[0], lc_number(j + 1), value);
        lc_release(ctx, value);
    }
    return lc_nil();
}

/* Strict UTF-8 helpers (RFC 3629), independent of locale and ICU. */
static int lc_utf8_decode_at(const char *s, size_t length, size_t at, uint32_t *codepoint, size_t *next) {
    const unsigned char *p = (const unsigned char *)s;
    uint32_t value;
    if (at >= length) return 0;
    if (p[at] < 0x80) { *codepoint = p[at]; *next = at + 1; return 1; }
    if (p[at] >= 0xc2 && p[at] <= 0xdf && at + 1 < length && (p[at+1] & 0xc0) == 0x80) {
        *codepoint = ((uint32_t)(p[at] & 0x1f) << 6) | (p[at+1] & 0x3f); *next = at + 2; return 1;
    }
    if (p[at] >= 0xe0 && p[at] <= 0xef && at + 2 < length && (p[at+1] & 0xc0) == 0x80 && (p[at+2] & 0xc0) == 0x80) {
        if ((p[at] == 0xe0 && p[at+1] < 0xa0) || (p[at] == 0xed && p[at+1] >= 0xa0)) return 0;
        value = ((uint32_t)(p[at] & 0x0f) << 12) | ((uint32_t)(p[at+1] & 0x3f) << 6) | (p[at+2] & 0x3f);
        *codepoint = value; *next = at + 3; return 1;
    }
    if (p[at] >= 0xf0 && p[at] <= 0xf4 && at + 3 < length && (p[at+1] & 0xc0) == 0x80 && (p[at+2] & 0xc0) == 0x80 && (p[at+3] & 0xc0) == 0x80) {
        if ((p[at] == 0xf0 && p[at+1] < 0x90) || (p[at] == 0xf4 && p[at+1] >= 0x90)) return 0;
        value = ((uint32_t)(p[at] & 7) << 18) | ((uint32_t)(p[at+1] & 0x3f) << 12) | ((uint32_t)(p[at+2] & 0x3f) << 6) | (p[at+3] & 0x3f);
        *codepoint = value; *next = at + 4; return 1;
    }
    return 0;
}
static int lc_utf8_position(LcContext *ctx, int argc, LcValue *argv, int argument, int default_value, size_t length) {
    int position = argc >= argument ? (int)lc_to_integer(ctx, argv[argument - 1]) : default_value;
    if (position < 0) position = (int)length + position + 1;
    if (position < 1) position = 1;
    return position - 1;
}
static LcValue lc_utf8_char(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcTextBuffer buffer; int i; LcValue result; (void)self; memset(&buffer, 0, sizeof(buffer));
    for (i = 0; i < argc && !ctx->error; ++i) {
        int64_t input = lc_to_integer(ctx, argv[i]); char bytes[4]; int count;
        if (input < 0 || input > 0x10ffff || (input >= 0xd800 && input <= 0xdfff)) { lc_error(ctx, "invalid UTF-8 codepoint"); break; }
        if (input < 0x80) { bytes[0] = (char)input; count = 1; }
        else if (input < 0x800) { bytes[0] = (char)(0xc0 | (input >> 6)); bytes[1] = (char)(0x80 | (input & 63)); count = 2; }
        else if (input < 0x10000) { bytes[0] = (char)(0xe0 | (input >> 12)); bytes[1] = (char)(0x80 | ((input >> 6) & 63)); bytes[2] = (char)(0x80 | (input & 63)); count = 3; }
        else { bytes[0] = (char)(0xf0 | (input >> 18)); bytes[1] = (char)(0x80 | ((input >> 12) & 63)); bytes[2] = (char)(0x80 | ((input >> 6) & 63)); bytes[3] = (char)(0x80 | (input & 63)); count = 4; }
        lc_text_add(ctx, &buffer, bytes, (size_t)count);
    }
    result = lc_string_n(ctx, buffer.data ? buffer.data : "", buffer.length); lc_text_free(&buffer); return result;
}
static LcValue lc_utf8_len(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue value; size_t length, at, end, count = 0; uint32_t cp; size_t next;
    (void)self;
    if (!argc || lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "string expected"); return lc_nil(); }
    value = lc_first_value(argv[0]); length = value.as.string->length;
    at = (size_t)lc_utf8_position(ctx, argc, argv, 2, 1, length);
    end = (size_t)lc_utf8_position(ctx, argc, argv, 3, -1, length) + 1;
    if (at > length) at = length; if (end > length) end = length;
    while (at < end) {
        if (!lc_utf8_decode_at(value.as.string->data, end, at, &cp, &next)) {
            LcValue out[2]; out[0] = lc_nil(); out[1] = lc_number((double)at + 1); return lc_tuple_new(ctx, 2, out, 1);
        }
        at = next; ++count;
    }
    return lc_number((double)count);
}
static LcValue lc_utf8_codepoint(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue value, *items, result; size_t length, at, end, count = 0, capacity = 16; uint32_t cp; size_t next;
    (void)self;
    if (!argc || lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "string expected"); return lc_nil(); }
    value = lc_first_value(argv[0]); length = value.as.string->length;
    at = (size_t)lc_utf8_position(ctx, argc, argv, 2, 1, length);
    end = (size_t)lc_utf8_position(ctx, argc, argv, 3, argc > 1 ? (int)lc_to_integer(ctx, argv[1]) : 1, length) + 1;
    if (end > length) end = length;
    items = (LcValue *)malloc(sizeof(LcValue) * capacity);
    if (!items) { lc_error(ctx, "out of memory in utf8.codepoint"); return lc_nil(); }
    while (at < end && !ctx->error) {
        if (!lc_utf8_decode_at(value.as.string->data, end, at, &cp, &next)) { free(items); lc_error(ctx, "invalid UTF-8 code"); return lc_nil(); }
        if (count == capacity) {
            LcValue *larger; capacity *= 2; larger = (LcValue *)realloc(items, sizeof(LcValue) * capacity);
            if (!larger) { free(items); lc_error(ctx, "out of memory"); return lc_nil(); }
            items = larger;
        }
        items[count++] = lc_number(cp); at = next;
    }
    result = lc_tuple_new(ctx, (int)count, items, 1); free(items); return result;
}
static LcValue lc_utf8_offset(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue value; size_t length, at; int64_t n; uint32_t cp; size_t next;
    (void)self;
    if (argc < 2 || lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "string and offset expected"); return lc_nil(); }
    value = lc_first_value(argv[0]); length = value.as.string->length; n = lc_to_integer(ctx, argv[1]);
    at = (size_t)lc_utf8_position(ctx, argc, argv, 3, n >= 0 ? 1 : -1, length);
    if (at > length) return lc_nil();
    if (n == 0) { while (at > 0 && at < length && (((unsigned char)value.as.string->data[at]) & 0xc0) == 0x80) --at; return lc_number((double)at + 1); }
    if (n > 0) {
        if (at < length && (((unsigned char)value.as.string->data[at]) & 0xc0) == 0x80) { lc_error(ctx, "initial position is a continuation byte"); return lc_nil(); }
        while (n-- > 1) { if (at >= length) return lc_nil(); if (!lc_utf8_decode_at(value.as.string->data, length, at, &cp, &next)) { lc_error(ctx, "invalid UTF-8 code"); return lc_nil(); } at = next; }
        return lc_number((double)at + 1);
    }
    while (at > 0 && at < length && (((unsigned char)value.as.string->data[at]) & 0xc0) == 0x80) --at;
    while (n++ < -1) {
        if (at == 0) return lc_nil();
        --at; while (at > 0 && (((unsigned char)value.as.string->data[at]) & 0xc0) == 0x80) --at;
    }
    return lc_number((double)at + 1);
}
static LcValue lc_utf8_codes_iter(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue source = lc_cell_get(self->upvalues[0]), position = lc_cell_get(self->upvalues[1]), out[2];
    size_t at = (size_t)position.as.number, next; uint32_t cp;
    (void)argc; (void)argv;
    if (at >= source.as.string->length || !lc_utf8_decode_at(source.as.string->data, source.as.string->length, at, &cp, &next)) {
        lc_release(ctx, source); lc_release(ctx, position); return lc_nil();
    }
    lc_cell_set(ctx, self->upvalues[1], lc_number((double)next));
    out[0] = lc_number((double)at + 1); out[1] = lc_number(cp);
    lc_release(ctx, source); lc_release(ctx, position); return lc_tuple_new(ctx, 2, out, 1);
}
static LcValue lc_utf8_codes(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue source; LcCell *upvalues[2]; LcValue result;
    (void)self;
    if (!argc || lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "string expected"); return lc_nil(); }
    source = lc_retain(lc_first_value(argv[0]));
    upvalues[0] = lc_cell_new(ctx, lc_retain(source)); upvalues[1] = lc_cell_new(ctx, lc_number(0));
    result = lc_function_new(ctx, lc_utf8_codes_iter, 2, upvalues, "utf8.codes iterator");
    lc_cell_release(ctx, upvalues[0]); lc_cell_release(ctx, upvalues[1]); lc_release(ctx, source); return result;
}

static LcValue lc_file_value(LcContext *ctx, FILE *handle, int standard, int pipe) {
    LcValue value = lc_nil(); LcFile *file;
    if (!handle) return value;
    file = (LcFile *)lc_alloc(ctx, sizeof(*file), LC_FILE);
    if (!file) return value;
    file->handle = handle; file->standard = standard; file->pipe = pipe;
    value.type = LC_FILE; value.as.file = file; return value;
}
static LcFile *lc_file_check(LcContext *ctx, LcValue value, int allow_closed) {
    value = lc_first_value(value);
    if (value.type != LC_FILE || (!allow_closed && value.as.file->closed)) { lc_error(ctx, "open file expected"); return NULL; }
    return value.as.file;
}
static LcValue lc_file_failure(LcContext *ctx, const char *operation) {
    LcValue out[3]; int code = errno; char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s: %s", operation, strerror(code));
    out[0] = lc_nil(); out[1] = lc_string(ctx, buffer); out[2] = lc_number(code);
    return lc_tuple_new(ctx, 3, out, 1);
}
static LcValue lc_file_close(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcFile *file = lc_file_check(ctx, argc ? argv[0] : ctx->output, 1); int status;
    (void)self;
    if (!file) return lc_nil();
    if (file->standard) return lc_file_failure(ctx, "cannot close standard file");
    if (file->closed) return lc_file_failure(ctx, "attempt to use a closed file");
    errno = 0;
#ifdef _WIN32
    status = file->pipe ? _pclose(file->handle) : fclose(file->handle);
#else
    status = file->pipe ? pclose(file->handle) : fclose(file->handle);
#endif
    file->closed = 1; file->handle = NULL;
    return status == 0 ? lc_bool(1) : lc_file_failure(ctx, "close");
}
static LcValue lc_file_flush(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcFile *file = lc_file_check(ctx, argc ? argv[0] : ctx->output, 0); (void)self;
    if (!file) return lc_nil();
    if (fflush(file->handle) != 0) return lc_file_failure(ctx, "flush");
    return lc_retain(argv[0]);
}
static LcValue lc_file_write(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcFile *file = lc_file_check(ctx, argc ? argv[0] : ctx->output, 0); int i; (void)self;
    if (!file) return lc_nil();
    for (i = 1; i < argc; ++i) {
        LcValue value = lc_first_value(argv[i]), string;
        if (value.type != LC_STRING && value.type != LC_NUMBER && value.type != LC_INTEGER) { lc_error(ctx, "file:write expects strings or numbers"); return lc_nil(); }
        string = lc_to_string(ctx, value);
        if (fwrite(string.as.string->data, 1, string.as.string->length, file->handle) != string.as.string->length) {
            lc_release(ctx, string); return lc_file_failure(ctx, "write");
        }
        lc_release(ctx, string);
    }
    return lc_retain(argv[0]);
}
static LcValue lc_file_read(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcFile *file = lc_file_check(ctx, argc ? argv[0] : ctx->input, 0);
    LcValue *results; int count = 0, formats, i; (void)self;
    if (!file) return lc_nil();
    formats = argc > 1 ? argc - 1 : 1; results = (LcValue *)calloc((size_t)formats, sizeof(LcValue));
    if (!results) { lc_error(ctx, "out of memory in file:read"); return lc_nil(); }
    for (i = 0; i < formats; ++i) {
        LcValue format = argc > 1 ? lc_first_value(argv[i + 1]) : lc_string(ctx, "*l");
        if (format.type == LC_NUMBER || format.type == LC_INTEGER) {
            int wanted = (int)format.as.number;
            char *bytes; size_t got;
            if (wanted < 0) { lc_error(ctx, "invalid read count"); break; }
            bytes = (char *)malloc((size_t)wanted + 1);
            if (!bytes) { lc_error(ctx, "out of memory in file:read"); break; }
            got = wanted ? fread(bytes, 1, (size_t)wanted, file->handle) : 0;
            if (got || wanted == 0) results[count++] = lc_string_n(ctx, bytes, got); else results[count++] = lc_nil();
            free(bytes);
        } else if (format.type == LC_STRING) {
            const char *mode = format.as.string->data; if (*mode == '*') ++mode;
            if (*mode == 'n') {
                double number;
                if (fscanf(file->handle, "%lf", &number) == 1) results[count++] = lc_number(number); else results[count++] = lc_nil();
            } else if (*mode == 'a') {
                LcTextBuffer buffer; char chunk[1024]; size_t got; memset(&buffer, 0, sizeof(buffer));
                while ((got = fread(chunk, 1, sizeof(chunk), file->handle)) != 0) lc_text_add(ctx, &buffer, chunk, got);
                results[count++] = lc_string_n(ctx, buffer.data ? buffer.data : "", buffer.length); lc_text_free(&buffer);
            } else if (*mode == 'l' || *mode == 'L') {
                LcTextBuffer buffer; char chunk[256]; int any = 0; memset(&buffer, 0, sizeof(buffer));
                while (fgets(chunk, sizeof(chunk), file->handle)) {
                    size_t length = strlen(chunk); any = 1;
                    if (*mode == 'l' && length && chunk[length - 1] == '\n') --length;
                    lc_text_add(ctx, &buffer, chunk, length);
                    if (length < sizeof(chunk) - 1 || (length && chunk[length - 1] == '\n')) break;
                }
                results[count++] = any ? lc_string_n(ctx, buffer.data ? buffer.data : "", buffer.length) : lc_nil(); lc_text_free(&buffer);
            } else { lc_error(ctx, "invalid file read format"); }
        } else lc_error(ctx, "invalid file read format");
        if (argc <= 1) lc_release(ctx, format);
        if (ctx->error || (count && lc_first_value(results[count - 1]).type == LC_NIL)) break;
    }
    if (ctx->error) { for (i = 0; i < count; ++i) lc_release(ctx, results[i]); free(results); return lc_nil(); }
    if (count == 1) { LcValue result = results[0]; free(results); return result; }
    { LcValue result = lc_tuple_new(ctx, count, results, 1); free(results); return result; }
}
static LcValue lc_file_seek(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcFile *file = lc_file_check(ctx, argc ? argv[0] : lc_nil(), 0);
    const char *whence = "cur"; int origin = SEEK_CUR; long offset = 0;
    (void)self;
    if (!file) return lc_nil();
    if (argc > 1 && lc_first_value(argv[1]).type == LC_STRING) whence = lc_first_value(argv[1]).as.string->data;
    if (!strcmp(whence, "set")) origin = SEEK_SET; else if (!strcmp(whence, "end")) origin = SEEK_END;
    if (argc > 2) offset = (long)lc_to_integer(ctx, argv[2]);
    if (fseek(file->handle, offset, origin) != 0) return lc_file_failure(ctx, "seek");
    return lc_number((double)ftell(file->handle));
}
static LcValue lc_file_setvbuf(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcFile *file = lc_file_check(ctx, argc ? argv[0] : lc_nil(), 0);
    const char *mode = argc > 1 && lc_first_value(argv[1]).type == LC_STRING ? lc_first_value(argv[1]).as.string->data : "no";
    int kind = !strcmp(mode, "full") ? _IOFBF : !strcmp(mode, "line") ? _IOLBF : _IONBF;
    size_t size = argc > 2 ? (size_t)lc_to_integer(ctx, argv[2]) : BUFSIZ;
    (void)self;
    if (!file) return lc_nil();
    if (setvbuf(file->handle, NULL, kind, size) != 0) return lc_file_failure(ctx, "setvbuf");
    return lc_retain(argv[0]);
}
static LcValue lc_file_lines_iter(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue file = lc_cell_get(self->upvalues[0]), formats = lc_cell_get(self->upvalues[2]), args[33], result;
    int count = 1, i; (void)argc; (void)argv;
    args[0] = file;
    if (formats.type == LC_TUPLE) {
        count += formats.as.tuple->count;
        for (i = 1; i < count; ++i) args[i] = formats.as.tuple->items[i - 1];
    }
    result = lc_file_read(ctx, self, count, args);
    if (lc_first_value(result).type == LC_NIL && self->upvalues[1]->value.as.boolean) {
        LcValue ignored = lc_file_close(ctx, self, 1, args); lc_release(ctx, ignored);
    }
    lc_release(ctx, file); lc_release(ctx, formats); return result;
}
static LcValue lc_file_lines_from(LcContext *ctx, LcValue file, int autoclose, int argc, LcValue *argv) {
    LcCell *upvalues[3]; LcValue result, formats;
    upvalues[0] = lc_cell_new(ctx, lc_retain(file)); upvalues[1] = lc_cell_new(ctx, lc_bool(autoclose));
    formats = lc_tuple_new(ctx, argc, argv, 0); upvalues[2] = lc_cell_new(ctx, lc_retain(formats));
    result = lc_function_new(ctx, lc_file_lines_iter, 3, upvalues, "file lines iterator");
    lc_cell_release(ctx, upvalues[0]); lc_cell_release(ctx, upvalues[1]); lc_cell_release(ctx, upvalues[2]); lc_release(ctx, formats);
    return result;
}
static LcValue lc_file_lines(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self;
    if (!argc || lc_first_value(argv[0]).type != LC_FILE) { lc_error(ctx, "file expected"); return lc_nil(); }
    return lc_file_lines_from(ctx, argv[0], 0, argc - 1, argv + 1);
}
static LcValue lc_io_open(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    const char *path, *mode = "r"; FILE *handle;
    (void)self;
    if (!argc || lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "filename expected"); return lc_nil(); }
    path = lc_first_value(argv[0]).as.string->data;
    if (argc > 1 && lc_first_value(argv[1]).type == LC_STRING) mode = lc_first_value(argv[1]).as.string->data;
    errno = 0; handle = fopen(path, mode);
    if (!handle) return lc_file_failure(ctx, path);
    return lc_file_value(ctx, handle, 0, 0);
}
static LcValue lc_io_tmpfile(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    FILE *handle; (void)self; (void)argc; (void)argv;
    errno = 0; handle = tmpfile(); return handle ? lc_file_value(ctx, handle, 0, 0) : lc_file_failure(ctx, "tmpfile");
}
static LcValue lc_io_lines(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue file; int skip = 0;
    (void)self;
    if (argc && lc_first_value(argv[0]).type == LC_STRING) {
        file = lc_io_open(ctx, self, 1, argv); skip = 1;
        if (lc_first_value(file).type != LC_FILE) return file;
    } else file = lc_retain(ctx->input);
    { LcValue result = lc_file_lines_from(ctx, file, skip, argc - skip, argv + skip); lc_release(ctx, file); return result; }
}
static LcValue lc_io_read(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue args[34]; int i; args[0] = ctx->input;
    for (i = 0; i < argc && i < 32; ++i) args[i + 1] = argv[i];
    return lc_file_read(ctx, self, argc + 1, args);
}
static LcValue lc_io_write(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue args[34]; int i; args[0] = ctx->output;
    for (i = 0; i < argc && i < 32; ++i) args[i + 1] = argv[i];
    return lc_file_write(ctx, self, argc + 1, args);
}
static LcValue lc_io_default(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int input = !strcmp(self->name, "input"); LcValue file;
    if (!argc) return lc_retain(input ? ctx->input : ctx->output);
    if (lc_first_value(argv[0]).type == LC_STRING) {
        LcValue args[2]; args[0] = argv[0]; args[1] = lc_string(ctx, input ? "r" : "w");
        file = lc_io_open(ctx, self, 2, args); lc_release(ctx, args[1]);
    } else file = lc_retain(argv[0]);
    if (lc_file_check(ctx, file, 0)) {
        LcValue old = input ? ctx->input : ctx->output;
        if (input) ctx->input = lc_retain(file); else ctx->output = lc_retain(file);
        lc_release(ctx, old);
    }
    return file;
}
static LcValue lc_io_type(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue value; (void)ctx; (void)self;
    if (!argc) return lc_nil(); value = lc_first_value(argv[0]);
    if (value.type != LC_FILE) return lc_nil();
    return lc_string(ctx, value.as.file->closed ? "closed file" : "file");
}
static LcValue lc_io_popen(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    const char *command, *mode = "r"; FILE *handle;
    (void)self;
    if (!argc || lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "command expected"); return lc_nil(); }
    command = lc_first_value(argv[0]).as.string->data;
    if (argc > 1 && lc_first_value(argv[1]).type == LC_STRING) mode = lc_first_value(argv[1]).as.string->data;
#ifdef _WIN32
    handle = _popen(command, mode);
#else
    handle = popen(command, mode);
#endif
    return handle ? lc_file_value(ctx, handle, 0, 1) : lc_file_failure(ctx, "popen");
}

static LcValue lc_io_flush(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue file = ctx->output; (void)argc; (void)argv;
    return lc_file_flush(ctx, self, 1, &file);
}

static void lc_set_field(LcContext *ctx, LcValue table, const char *name, LcValue value);
static double lc_os_field(LcContext *ctx, LcValue table, const char *name, double fallback) {
    LcValue key = lc_string(ctx, name), value = lc_table_get(ctx, table, key);
    double number; int ok;
    lc_release(ctx, key);
    if (value.type == LC_NIL) { lc_release(ctx, value); return fallback; }
    number = lc_to_number(ctx, value, &ok); lc_release(ctx, value); return number;
}
static LcValue lc_os_clock(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) { (void)self; (void)argc; (void)argv; return lc_number((double)clock() / CLOCKS_PER_SEC); }
static LcValue lc_os_time(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self;
    if (argc && lc_first_value(argv[0]).type == LC_TABLE) {
        struct tm fields; time_t timestamp; memset(&fields, 0, sizeof(fields));
        fields.tm_year = (int)lc_os_field(ctx, argv[0], "year", 1970) - 1900;
        fields.tm_mon = (int)lc_os_field(ctx, argv[0], "month", 1) - 1;
        fields.tm_mday = (int)lc_os_field(ctx, argv[0], "day", 1);
        fields.tm_hour = (int)lc_os_field(ctx, argv[0], "hour", 12);
        fields.tm_min = (int)lc_os_field(ctx, argv[0], "min", 0);
        fields.tm_sec = (int)lc_os_field(ctx, argv[0], "sec", 0);
        fields.tm_isdst = -1; timestamp = mktime(&fields);
        if (timestamp == (time_t)-1) return lc_nil();
        return lc_number((double)timestamp);
    }
    return lc_number((double)time(NULL));
}
static LcValue lc_os_date(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    const char *format = argc && lc_first_value(argv[0]).type == LC_STRING ? lc_first_value(argv[0]).as.string->data : "%c";
    time_t timestamp = argc > 1 ? (time_t)lc_to_integer(ctx, argv[1]) : time(NULL);
    struct tm *pointer, fields; int utc = *format == '!'; char buffer[1024]; size_t length;
    (void)self;
    if (utc) ++format;
#ifdef _WIN32
    pointer = utc ? gmtime(&timestamp) : localtime(&timestamp);
    if (pointer) fields = *pointer;
#else
    memset(&fields, 0, sizeof(fields));
    pointer = utc ? gmtime_r(&timestamp, &fields) : localtime_r(&timestamp, &fields);
#endif
    if (!pointer) return lc_nil();
    if (!strcmp(format, "*t")) {
        LcValue table = lc_table(ctx);
        lc_set_field(ctx, table, "year", lc_number(fields.tm_year + 1900)); lc_set_field(ctx, table, "month", lc_number(fields.tm_mon + 1));
        lc_set_field(ctx, table, "day", lc_number(fields.tm_mday)); lc_set_field(ctx, table, "hour", lc_number(fields.tm_hour));
        lc_set_field(ctx, table, "min", lc_number(fields.tm_min)); lc_set_field(ctx, table, "sec", lc_number(fields.tm_sec));
        lc_set_field(ctx, table, "wday", lc_number(fields.tm_wday + 1)); lc_set_field(ctx, table, "yday", lc_number(fields.tm_yday + 1));
        lc_set_field(ctx, table, "isdst", lc_bool(fields.tm_isdst > 0)); return table;
    }
    length = strftime(buffer, sizeof(buffer), format, &fields); return lc_string_n(ctx, buffer, length);
}
static LcValue lc_os_difftime(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int ok; double second = lc_to_number(ctx, argc ? argv[0] : lc_nil(), &ok);
    double first = lc_to_number(ctx, argc > 1 ? argv[1] : lc_nil(), &ok);
    (void)self; return lc_number(second - first);
}
static LcValue lc_os_getenv(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    const char *value; (void)self;
    if (!argc || lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "environment variable name expected"); return lc_nil(); }
    value = getenv(lc_first_value(argv[0]).as.string->data); return value ? lc_string(ctx, value) : lc_nil();
}
static LcValue lc_os_fileop(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int result;
    (void)self;
    if (!argc || lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "path expected"); return lc_nil(); }
    errno = 0;
    if (!strcmp(self->name, "remove")) result = remove(lc_first_value(argv[0]).as.string->data);
    else {
        if (argc < 2 || lc_first_value(argv[1]).type != LC_STRING) { lc_error(ctx, "destination path expected"); return lc_nil(); }
        result = rename(lc_first_value(argv[0]).as.string->data, lc_first_value(argv[1]).as.string->data);
    }
    return result == 0 ? lc_bool(1) : lc_file_failure(ctx, self->name);
}
static LcValue lc_os_tmpname(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    (void)self; (void)argc; (void)argv;
#ifdef _WIN32
    { char buffer[L_tmpnam]; if (!tmpnam(buffer)) { lc_error(ctx, "cannot create temporary filename"); return lc_nil(); } return lc_string(ctx, buffer); }
#else
    { char buffer[] = "/tmp/luac-XXXXXX"; int descriptor = mkstemp(buffer);
      if (descriptor < 0) return lc_file_failure(ctx, "tmpname");
      close(descriptor); unlink(buffer); return lc_string(ctx, buffer); }
#endif
}
static LcValue lc_os_execute(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int status; LcValue out[3]; (void)self;
    if (!argc) return lc_bool(system(NULL) != 0);
    if (lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "command expected"); return lc_nil(); }
    status = system(lc_first_value(argv[0]).as.string->data);
    out[0] = status == 0 ? lc_bool(1) : lc_nil(); out[1] = lc_string(ctx, "exit"); out[2] = lc_number(status);
    return lc_tuple_new(ctx, 3, out, 1);
}
static LcValue lc_os_exit(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    int code = 0; (void)ctx; (void)self;
    if (argc) { LcValue value = lc_first_value(argv[0]); code = value.type == LC_BOOL ? (value.as.boolean ? 0 : 1) : (int)lc_to_integer(ctx, value); }
    exit(code); return lc_nil();
}
static LcValue lc_os_setlocale(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    const char *locale = NULL; int category = LC_ALL; const char *result;
    (void)self;
    if (argc && lc_first_value(argv[0]).type == LC_STRING) locale = lc_first_value(argv[0]).as.string->data;
    if (argc > 1 && lc_first_value(argv[1]).type == LC_STRING) {
        const char *name = lc_first_value(argv[1]).as.string->data;
        if (!strcmp(name, "collate")) category = LC_COLLATE; else if (!strcmp(name, "ctype")) category = LC_CTYPE;
        else if (!strcmp(name, "monetary")) category = LC_MONETARY; else if (!strcmp(name, "numeric")) category = LC_NUMERIC;
        else if (!strcmp(name, "time")) category = LC_TIME;
    }
    result = setlocale(category, locale); return result ? lc_string(ctx, result) : lc_nil();
}

static void lc_set_global(LcContext *ctx, const char *name, LcValue value) {
    LcValue key = lc_string(ctx, name); lc_table_set(ctx, ctx->globals, key, value); lc_release(ctx, key); lc_release(ctx, value);
}
static void lc_set_field(LcContext *ctx, LcValue table, const char *name, LcValue value) {
    LcValue key = lc_string(ctx, name); lc_table_set(ctx, table, key, value); lc_release(ctx, key); lc_release(ctx, value);
}
static void lc_add_native(LcContext *ctx, LcValue table, const char *name, LcNative fn) {
    lc_set_field(ctx, table, name, lc_function_new(ctx, fn, 0, NULL, name));
}

static void lc_audio_init(LcContext *ctx);
#include "luac_libraries.h"
#include "luac_audio.h"

static void lc_gc_mark_frames(LcContext *ctx, LcGcFrame *f) {
    int i;
    for (; f; f = f->prev) {
        for (i = 0; i < f->slot_count; ++i) lc_gc_mark_value(ctx, f->slots[i]);
        for (i = 0; i < f->cell_count; ++i) if (f->cells[i]) lc_gc_mark_obj(ctx, (LcObject *)f->cells[i]);
    }
}

static void lc_gc_table_mode(LcTable *t, int *wk, int *wv) {
    *wk = 0; *wv = 0;
    if (t->metatable.type == LC_TABLE) {
        LcTable *mt = t->metatable.as.table;
        size_t i;
        for (i = 0; i < mt->capacity; ++i) {
            if (mt->entries[i].used && mt->entries[i].key.type == LC_STRING &&
                mt->entries[i].key.as.string->length == 6 &&
                !memcmp(mt->entries[i].key.as.string->data, "__mode", 6) &&
                mt->entries[i].value.type == LC_STRING) {
                const char *m = mt->entries[i].value.as.string->data;
                *wk = strchr(m, 'k') != NULL;
                *wv = strchr(m, 'v') != NULL;
                break;
            }
        }
    }
}

static size_t lc_gc_propagate_one(LcContext *ctx, LcObject *o) {
    o->marked |= LC_GC_BLACK;
    switch (o->kind) {
        case LC_CELL:
            lc_gc_mark_value(ctx, ((LcCell *)o)->value);
            return sizeof(LcCell);
        case LC_TUPLE: {
            LcTuple *tp = (LcTuple *)o;
            int i;
            for (i = 0; i < tp->count; ++i) lc_gc_mark_value(ctx, tp->items[i]);
            return sizeof(LcTuple) + (size_t)tp->count * sizeof(LcValue);
        }
        case LC_STRUCT: {
            LcStruct *st = (LcStruct *)o;
            int i, n = st->desc ? st->desc->field_count : 0;
            for (i = 0; i < n; ++i) lc_gc_mark_value(ctx, st->fields[i]);
            return sizeof(LcStruct) + (size_t)n * sizeof(LcValue);
        }
        case LC_FUNCTION: {
            LcFunction *fn = (LcFunction *)o;
            int i;
            for (i = 0; i < fn->upvalue_count; ++i)
                if (fn->upvalues[i]) lc_gc_mark_obj(ctx, (LcObject *)fn->upvalues[i]);
            if (fn->has_bound_self) lc_gc_mark_value(ctx, fn->bound_self);
            return sizeof(LcFunction) + (size_t)fn->upvalue_count * sizeof(LcCell *);
        }
        case LC_COROUTINE: {
            LcCoroutine *co = (LcCoroutine *)o;
            lc_gc_mark_value(ctx, co->function);
            lc_gc_mark_value(ctx, co->resume_args);
            lc_gc_mark_value(ctx, co->yielded);
            lc_gc_mark_value(ctx, co->result);
            lc_gc_mark_frames(ctx, co->saved_frames);
            lc_gc_mark_frames(ctx, co->caller_frames);
            return sizeof(LcCoroutine);
        }
        case LC_TABLE: {
            LcTable *t = (LcTable *)o;
            int wk, wv;
            size_t i;
            lc_gc_mark_value(ctx, t->metatable);
            lc_gc_table_mode(t, &wk, &wv);
            if (!wk && !wv) {
                for (i = 0; i < t->capacity; ++i) {
                    if (t->entries[i].used) {
                        lc_gc_mark_value(ctx, t->entries[i].key);
                        lc_gc_mark_value(ctx, t->entries[i].value);
                    }
                }
            } else if (wv && !wk) {
                for (i = 0; i < t->capacity; ++i) {
                    if (t->entries[i].used) lc_gc_mark_value(ctx, t->entries[i].key);
                }
                if (!(o->gc_flags & 2)) {
                    o->gc_flags |= 2;
                    o->gc_list = ctx->gc_weak_v;
                    ctx->gc_weak_v = o;
                }
            } else if (wk && !wv) {
                if (!(o->gc_flags & 2)) {
                    o->gc_flags |= 2;
                    o->gc_list = ctx->gc_weak_k;
                    ctx->gc_weak_k = o;
                }
            } else {
                if (!(o->gc_flags & 2)) {
                    o->gc_flags |= 2;
                    o->gc_list = ctx->gc_weak_kv;
                    ctx->gc_weak_kv = o;
                }
            }
            return sizeof(LcTable) + t->capacity * sizeof(LcEntry);
        }
        default:
            return o->obj_size ? o->obj_size : 16;
    }
}

static size_t lc_gc_drain_gray(LcContext *ctx, size_t budget) {
    size_t work = 0;
    while (ctx->gc_gray && work < budget) {
        LcObject *o = ctx->gc_gray;
        ctx->gc_gray = o->gc_list;
        o->gc_list = NULL;
        work += lc_gc_propagate_one(ctx, o);
    }
    return work;
}

static int lc_gc_is_cleared(uint8_t dead_white, LcValue v) {
    LcObject *o = lc_value_object(v);
    if (!o) return 0;
    return (o->marked & dead_white) != 0;
}

static void lc_gc_clear_weak_list(LcObject *list, uint8_t dead_white, int wk, int wv) {
    while (list) {
        LcTable *t = (LcTable *)list;
        LcObject *next = list->gc_list;
        size_t i;
        list->gc_list = NULL;
        list->gc_flags &= (uint16_t)~2;
        for (i = 0; i < t->capacity; ++i) {
            if (!t->entries[i].used) continue;
            if ((wk && lc_gc_is_cleared(dead_white, t->entries[i].key)) ||
                (wv && lc_gc_is_cleared(dead_white, t->entries[i].value))) {
                memset(&t->entries[i], 0, sizeof(t->entries[i]));
            }
        }
        list = next;
    }
}

static void lc_gc_mark_roots(LcContext *ctx) {
    LcStructDesc *sd;
    int i;
    lc_gc_mark_value(ctx, ctx->globals);
    lc_gc_mark_value(ctx, ctx->string_library);
    lc_gc_mark_value(ctx, ctx->file_methods);
    lc_gc_mark_value(ctx, ctx->socket_methods);
    lc_gc_mark_value(ctx, ctx->audio_methods);
    lc_gc_mark_value(ctx, ctx->websocket_methods);
    lc_gc_mark_value(ctx, ctx->input);
    lc_gc_mark_value(ctx, ctx->output);
    lc_gc_mark_value(ctx, ctx->error_output);
    for (i = 0; i < ctx->c_root_count; ++i) lc_gc_mark_value(ctx, ctx->c_roots[i]);
    lc_gc_mark_frames(ctx, ctx->gc_frames);
    if (ctx->active_coroutine) lc_gc_mark_obj(ctx, (LcObject *)ctx->active_coroutine);
    for (sd = ctx->struct_descs; sd; sd = sd->next) {
        for (i = 0; i < sd->field_count; ++i) lc_gc_mark_value(ctx, sd->fields[i].default_value);
        for (i = 0; i < sd->method_count; ++i) lc_gc_mark_value(ctx, sd->methods[i]);
    }
}

static void lc_gc_atomic(LcContext *ctx) {
    LcObject *again;
    int changed, i;
    uint8_t dead_white;
    lc_gc_mark_value(ctx, ctx->globals);
    for (i = 0; i < ctx->c_root_count; ++i) lc_gc_mark_value(ctx, ctx->c_roots[i]);
    lc_gc_mark_frames(ctx, ctx->gc_frames);
    if (ctx->active_coroutine) lc_gc_mark_obj(ctx, (LcObject *)ctx->active_coroutine);
    lc_gc_drain_gray(ctx, (size_t)-1);
    again = ctx->gc_gray_again;
    ctx->gc_gray_again = NULL;
    while (again) {
        LcObject *o = again;
        again = o->gc_list;
        o->gc_list = NULL;
        lc_gc_propagate_one(ctx, o);
    }
    lc_gc_drain_gray(ctx, (size_t)-1);
    do {
        LcObject *w;
        changed = 0;
        for (w = ctx->gc_weak_k; w; w = w->gc_list) {
            LcTable *t = (LcTable *)w;
            size_t i;
            for (i = 0; i < t->capacity; ++i) {
                LcObject *ko, *vo;
                if (!t->entries[i].used) continue;
                ko = lc_value_object(t->entries[i].key);
                vo = lc_value_object(t->entries[i].value);
                if ((!ko || !(ko->marked & LC_GC_WHITES)) && vo && (vo->marked & LC_GC_WHITES)) {
                    lc_gc_mark_obj(ctx, vo);
                    changed = 1;
                }
            }
        }
        if (changed) lc_gc_drain_gray(ctx, (size_t)-1);
    } while (changed);
    dead_white = ctx->gc_current_white;
    ctx->gc_current_white ^= LC_GC_WHITES;
    lc_gc_clear_weak_list(ctx->gc_weak_v, dead_white, 0, 1);
    lc_gc_clear_weak_list(ctx->gc_weak_k, dead_white, 1, 0);
    lc_gc_clear_weak_list(ctx->gc_weak_kv, dead_white, 1, 1);
    ctx->gc_weak_v = ctx->gc_weak_k = ctx->gc_weak_kv = NULL;
    ctx->gc_sweep = &ctx->gc_all;
    ctx->gc_estimate = ctx->gc_bytes;
    ctx->gc_state = LC_GCS_SWEEP;
}

static void lc_gc_free_obj(LcContext *ctx, LcObject *o) {
    if (o->kind == LC_FILE) {
        LcFile *file = (LcFile *)o;
        if (!file->closed && !file->standard) {
#ifdef _WIN32
            if (file->pipe) _pclose(file->handle); else
#else
            if (file->pipe) pclose(file->handle); else
#endif
            fclose(file->handle);
        }
        file->closed = 1;
    } else if (o->kind == LC_SOCKET) {
        lc_socket_close_handle((LcSocket *)o);
    } else if (o->kind == LC_AUDIO) {
        lc_audio_dispose((LcAudio *)o);
        free(((LcAudio *)o)->samples);
    } else if (o->kind == LC_WEBSOCKET) {
        lc_websocket_dispose((LcWebsocket *)o);
    } else if (o->kind == LC_FFI) {
        lc_ffi_dispose((LcFFILibrary *)o);
    } else if (o->kind == LC_COROUTINE) {
#ifdef _WIN32
        if (((LcCoroutine *)o)->fiber) DeleteFiber(((LcCoroutine *)o)->fiber);
#elif defined(__ANDROID__)
        lc_android_coroutine_dispose((LcCoroutine *)o);
#else
        free(((LcCoroutine *)o)->stack);
#endif
    }
    if (ctx->gc_bytes >= o->obj_size) ctx->gc_bytes -= o->obj_size;
    else ctx->gc_bytes = 0;

    if (o->kind == LC_TABLE) {
        LcTable *t = (LcTable *)o;
        size_t eb = t->capacity * sizeof(LcEntry);
        if (ctx->gc_bytes >= eb) ctx->gc_bytes -= eb;
        else ctx->gc_bytes = 0;
        free(t->entries);
        *(void **)o = ctx->table_free;
        ctx->table_free = o;
    } else if (o->kind == LC_FUNCTION) {
        LcFunction *f = (LcFunction *)o;
        free(f->upvalues);
        *(void **)o = ctx->fn_free;
        ctx->fn_free = o;
    } else if (o->kind == LC_CELL) {
        *(void **)o = ctx->cell_free;
        ctx->cell_free = o;
    } else {
        free(o);
    }
}

static int lc_gc_sweep_step(LcContext *ctx, int count) {
    uint8_t other_white = (uint8_t)(ctx->gc_current_white ^ LC_GC_WHITES);
    while (*ctx->gc_sweep && count-- > 0) {
        LcObject *curr = *ctx->gc_sweep;
        if ((curr->marked & other_white) && !(curr->marked & LC_GC_FIXED)) {
            *ctx->gc_sweep = curr->next;
            lc_gc_free_obj(ctx, curr);
        } else {
            curr->marked = (uint8_t)((curr->marked & ~(LC_GC_WHITES | LC_GC_BLACK)) | ctx->gc_current_white);
            ctx->gc_sweep = &curr->next;
        }
    }
    return *ctx->gc_sweep == NULL;
}

static void lc_gc_step(LcContext *ctx) {
    size_t budget;
    if (!ctx->gc_running) return;
    budget = (ctx->gc_debt > 0 ? (size_t)ctx->gc_debt : 1024u) * (size_t)(ctx->gc_stepmul > 0 ? ctx->gc_stepmul : 200) / 100u;
    if (budget < 2048u) budget = 2048u;
    if (budget > 262144u) budget = 262144u;
    ctx->gc_debt = 0;

    if (ctx->gc_state == LC_GCS_PAUSE) {
        ctx->gc_gray = ctx->gc_gray_again = NULL;
        ctx->gc_weak_v = ctx->gc_weak_k = ctx->gc_weak_kv = NULL;
        lc_gc_mark_roots(ctx);
        ctx->gc_state = LC_GCS_PROPAGATE;
    }
    if (ctx->gc_state == LC_GCS_PROPAGATE) {
        lc_gc_drain_gray(ctx, budget);
        if (!ctx->gc_gray) lc_gc_atomic(ctx);
    } else if (ctx->gc_state == LC_GCS_ATOMIC) {
        lc_gc_atomic(ctx);
    }
    if (ctx->gc_state == LC_GCS_SWEEP) {
        if (lc_gc_sweep_step(ctx, 96)) {
            ctx->gc_state = LC_GCS_PAUSE;
            ctx->gc_threshold = (ctx->gc_estimate / 100u) * (size_t)(ctx->gc_pause > 100 ? ctx->gc_pause : 150);
            if (ctx->gc_threshold < 64u * 1024u) ctx->gc_threshold = 64u * 1024u;
            ctx->gc_debt = (intptr_t)ctx->gc_bytes - (intptr_t)ctx->gc_threshold;
        }
    }
}

static void lc_gc_collect(LcContext *ctx) {
    int prev = ctx->gc_running;
    ctx->gc_running = 1;
    if (ctx->gc_state == LC_GCS_SWEEP) {
        lc_gc_sweep_step(ctx, 0x7fffffff);
        ctx->gc_state = LC_GCS_PAUSE;
    }
    ctx->gc_gray = ctx->gc_gray_again = NULL;
    ctx->gc_weak_v = ctx->gc_weak_k = ctx->gc_weak_kv = NULL;
    lc_gc_mark_roots(ctx);
    ctx->gc_state = LC_GCS_PROPAGATE;
    lc_gc_atomic(ctx);
    lc_gc_sweep_step(ctx, 0x7fffffff);
    ctx->gc_state = LC_GCS_PAUSE;
    ctx->gc_threshold = (ctx->gc_estimate / 100u) * (size_t)(ctx->gc_pause > 100 ? ctx->gc_pause : 150);
    if (ctx->gc_threshold < 64u * 1024u) ctx->gc_threshold = 64u * 1024u;
    ctx->gc_debt = (intptr_t)ctx->gc_bytes - (intptr_t)ctx->gc_threshold;
    ctx->gc_running = prev;
}

static LcContext *lc_context_new(const char *source) {
    LcContext *ctx = (LcContext *)calloc(1, sizeof(*ctx));
    LcValue math, str, tab, io, os, bit, bit32, utf8, package, loaded, preload, searchers;
    static const char *unary[] = {"sin","cos","tan","asin","acos","atan","exp","log","sqrt","floor","ceil","abs","sinh","cosh","tanh","deg","rad",NULL};
    int i;
    if (!ctx) return NULL;
    ctx->source = source;
    ctx->tls_verify = 1;
    ctx->gc_current_white = LC_GC_WHITE0;
    ctx->gc_state = LC_GCS_PAUSE;
    ctx->gc_running = 1;
    ctx->gc_pause = 200;
    ctx->gc_stepmul = 200;
    ctx->gc_threshold = 128u * 1024u;
    ctx->gc_debt = -(intptr_t)ctx->gc_threshold;
    ctx->screen_w = 480;
    ctx->screen_h = 800;
    ctx->globals = lc_table(ctx);
    lc_set_global(ctx, "print", lc_function_new(ctx, lc_builtin_print, 0, NULL, "print"));
    lc_set_global(ctx, "warn", lc_function_new(ctx, lc_builtin_warn, 0, NULL, "warn"));
    lc_set_global(ctx, "type", lc_function_new(ctx, lc_builtin_type, 0, NULL, "type"));
    lc_set_global(ctx, "tostring", lc_function_new(ctx, lc_builtin_tostring, 0, NULL, "tostring"));
    lc_set_global(ctx, "tonumber", lc_function_new(ctx, lc_builtin_tonumber, 0, NULL, "tonumber"));
    lc_set_global(ctx, "assert", lc_function_new(ctx, lc_builtin_assert, 0, NULL, "assert"));
    lc_set_global(ctx, "error", lc_function_new(ctx, lc_builtin_error, 0, NULL, "error"));
    lc_set_global(ctx, "pcall", lc_function_new(ctx, lc_builtin_pcall, 0, NULL, "pcall"));
    lc_set_global(ctx, "select", lc_function_new(ctx, lc_builtin_select, 0, NULL, "select"));
    lc_set_global(ctx, "next", lc_function_new(ctx, lc_builtin_next, 0, NULL, "next"));
    lc_set_global(ctx, "pairs", lc_function_new(ctx, lc_builtin_pairs, 0, NULL, "pairs"));
    lc_set_global(ctx, "ipairs", lc_function_new(ctx, lc_builtin_ipairs, 0, NULL, "ipairs"));
    lc_set_global(ctx, "xpcall", lc_function_new(ctx, lc_builtin_xpcall, 0, NULL, "xpcall"));
    lc_set_global(ctx, "rawget", lc_function_new(ctx, lc_builtin_rawget, 0, NULL, "rawget"));
    lc_set_global(ctx, "rawset", lc_function_new(ctx, lc_builtin_rawset, 0, NULL, "rawset"));
    lc_set_global(ctx, "rawequal", lc_function_new(ctx, lc_builtin_rawequal, 0, NULL, "rawequal"));
    lc_set_global(ctx, "rawlen", lc_function_new(ctx, lc_builtin_rawlen, 0, NULL, "rawlen"));
    lc_set_global(ctx, "setmetatable", lc_function_new(ctx, lc_builtin_setmetatable, 0, NULL, "setmetatable"));
    lc_set_global(ctx, "getmetatable", lc_function_new(ctx, lc_builtin_getmetatable, 0, NULL, "getmetatable"));
    lc_set_global(ctx, "collectgarbage", lc_function_new(ctx, lc_builtin_collectgarbage, 0, NULL, "collectgarbage"));
    lc_set_global(ctx, "gcinfo", lc_function_new(ctx, lc_builtin_gcinfo, 0, NULL, "gcinfo"));
    lc_set_global(ctx, "scene_reset", lc_function_new(ctx, lc_builtin_scene_reset, 0, NULL, "scene_reset"));
    lc_set_global(ctx, "clear_screen", lc_function_new(ctx, lc_builtin_clear_screen, 0, NULL, "clear_screen"));
    lc_set_global(ctx, "draw_sprite", lc_function_new(ctx, lc_builtin_draw_sprite, 0, NULL, "draw_sprite"));
    lc_set_global(ctx, "draw_rect", lc_function_new(ctx, lc_builtin_draw_rect, 0, NULL, "draw_rect"));
    lc_set_global(ctx, "draw_rect_outline", lc_function_new(ctx, lc_builtin_draw_rect_outline, 0, NULL, "draw_rect_outline"));
    lc_set_global(ctx, "draw_round_rect", lc_function_new(ctx, lc_builtin_draw_round_rect, 0, NULL, "draw_round_rect"));
    lc_set_global(ctx, "draw_circle", lc_function_new(ctx, lc_builtin_draw_circle, 0, NULL, "draw_circle"));
    lc_set_global(ctx, "draw_circle_outline", lc_function_new(ctx, lc_builtin_draw_circle_outline, 0, NULL, "draw_circle_outline"));
    lc_set_global(ctx, "draw_line", lc_function_new(ctx, lc_builtin_draw_line, 0, NULL, "draw_line"));
    lc_set_global(ctx, "set_font", lc_function_new(ctx, lc_builtin_set_font, 0, NULL, "set_font"));
    lc_set_global(ctx, "set_text_align", lc_function_new(ctx, lc_builtin_set_text_align, 0, NULL, "set_text_align"));
    lc_set_global(ctx, "draw_text", lc_function_new(ctx, lc_builtin_draw_text, 0, NULL, "draw_text"));
    lc_set_global(ctx, "screen_width", lc_function_new(ctx, lc_builtin_screen_width, 0, NULL, "screen_width"));
    lc_set_global(ctx, "screen_height", lc_function_new(ctx, lc_builtin_screen_height, 0, NULL, "screen_height"));
    lc_set_global(ctx, "require", lc_function_new(ctx, lc_builtin_require, 0, NULL, "require"));
    lc_set_global(ctx, "_G", lc_retain(ctx->globals));
    lc_set_global(ctx, "_VERSION", lc_string(ctx, "LuaC 0.5 (Incremental GC + Machine Code)"));
    math = lc_table(ctx);
    for (i = 0; unary[i]; ++i) lc_add_native(ctx, math, unary[i], lc_math_unary);
    lc_add_native(ctx, math, "min", lc_math_minmax); lc_add_native(ctx, math, "max", lc_math_minmax);
    lc_add_native(ctx, math, "random", lc_math_random); lc_add_native(ctx, math, "randomseed", lc_math_randomseed);
    lc_add_native(ctx, math, "fmod", lc_math_other); lc_add_native(ctx, math, "modf", lc_math_other);
    lc_add_native(ctx, math, "frexp", lc_math_other); lc_add_native(ctx, math, "ldexp", lc_math_other);
    lc_add_native(ctx, math, "pow", lc_math_other); lc_add_native(ctx, math, "atan2", lc_math_other);
    lc_add_native(ctx, math, "tointeger", lc_math_other); lc_add_native(ctx, math, "type", lc_math_other);
    lc_add_native(ctx, math, "ult", lc_math_other);
    lc_set_field(ctx, math, "pi", lc_number(3.14159265358979323846)); lc_set_field(ctx, math, "huge", lc_number(HUGE_VAL));
    lc_set_field(ctx, math, "maxinteger", lc_integer(INT64_MAX));
    lc_set_field(ctx, math, "mininteger", lc_integer(INT64_MIN));
    lc_set_global(ctx, "math", math);
    str = lc_table(ctx); lc_add_native(ctx, str, "len", lc_string_len); lc_add_native(ctx, str, "sub", lc_string_sub);
    lc_add_native(ctx, str, "upper", lc_string_case); lc_add_native(ctx, str, "lower", lc_string_case); lc_add_native(ctx, str, "rep", lc_string_rep);
    lc_add_native(ctx, str, "byte", lc_string_byte); lc_add_native(ctx, str, "char", lc_string_char);
    lc_add_native(ctx, str, "reverse", lc_string_reverse); lc_add_native(ctx, str, "format", lc_string_format); lc_add_native(ctx, str, "find", lc_string_find);
    lc_add_native(ctx, str, "match", lc_string_find); lc_add_native(ctx, str, "gmatch", lc_string_gmatch);
    lc_add_native(ctx, str, "gsub", lc_string_gsub);
    ctx->string_library = lc_retain(str);
    lc_set_global(ctx, "string", str);
    tab = lc_table(ctx); lc_add_native(ctx, tab, "insert", lc_table_insert); lc_add_native(ctx, tab, "remove", lc_table_remove); lc_add_native(ctx, tab, "concat", lc_table_concat);
    lc_add_native(ctx, tab, "pack", lc_table_pack); lc_add_native(ctx, tab, "unpack", lc_table_unpack);
    lc_add_native(ctx, tab, "move", lc_table_move); lc_add_native(ctx, tab, "sort", lc_table_sort);
    lc_set_global(ctx, "table", tab);
    lc_set_global(ctx, "unpack", lc_function_new(ctx, lc_table_unpack, 0, NULL, "unpack"));
    bit = lc_table(ctx);
    lc_add_native(ctx, bit, "tobit", lc_bit_unary); lc_add_native(ctx, bit, "bnot", lc_bit_unary); lc_add_native(ctx, bit, "bswap", lc_bit_unary);
    lc_add_native(ctx, bit, "band", lc_bit_nary); lc_add_native(ctx, bit, "bor", lc_bit_nary); lc_add_native(ctx, bit, "bxor", lc_bit_nary);
    lc_add_native(ctx, bit, "lshift", lc_bit_shift); lc_add_native(ctx, bit, "rshift", lc_bit_shift); lc_add_native(ctx, bit, "arshift", lc_bit_shift);
    lc_add_native(ctx, bit, "rol", lc_bit_shift); lc_add_native(ctx, bit, "ror", lc_bit_shift); lc_add_native(ctx, bit, "tohex", lc_bit_tohex);
    lc_set_global(ctx, "bit", bit);
    bit32 = lc_table(ctx);
    lc_add_native(ctx, bit32, "bnot", lc_bit_unary); lc_add_native(ctx, bit32, "band", lc_bit_nary);
    lc_add_native(ctx, bit32, "bor", lc_bit_nary); lc_add_native(ctx, bit32, "bxor", lc_bit_nary); lc_add_native(ctx, bit32, "btest", lc_bit_btest);
    lc_add_native(ctx, bit32, "lshift", lc_bit32_shift); lc_add_native(ctx, bit32, "rshift", lc_bit32_shift); lc_add_native(ctx, bit32, "arshift", lc_bit32_shift);
    lc_add_native(ctx, bit32, "lrotate", lc_bit_shift); lc_add_native(ctx, bit32, "rrotate", lc_bit_shift);
    lc_add_native(ctx, bit32, "extract", lc_bit_extract); lc_add_native(ctx, bit32, "replace", lc_bit_extract);
    lc_set_global(ctx, "bit32", bit32);
    utf8 = lc_table(ctx);
    lc_add_native(ctx, utf8, "char", lc_utf8_char); lc_add_native(ctx, utf8, "len", lc_utf8_len);
    lc_add_native(ctx, utf8, "codepoint", lc_utf8_codepoint); lc_add_native(ctx, utf8, "offset", lc_utf8_offset);
    lc_add_native(ctx, utf8, "codes", lc_utf8_codes);
    lc_set_field(ctx, utf8, "charpattern", lc_string(ctx, "[\0-\x7F\xC2-\xF4][\x80-\xBF]*"));
    lc_set_global(ctx, "utf8", utf8);
    io = lc_table(ctx);
    lc_add_native(ctx, io, "open", lc_io_open); lc_add_native(ctx, io, "close", lc_file_close);
    lc_add_native(ctx, io, "flush", lc_io_flush); lc_add_native(ctx, io, "input", lc_io_default);
    lc_add_native(ctx, io, "output", lc_io_default); lc_add_native(ctx, io, "lines", lc_io_lines);
    lc_add_native(ctx, io, "read", lc_io_read); lc_add_native(ctx, io, "write", lc_io_write);
    lc_add_native(ctx, io, "type", lc_io_type); lc_add_native(ctx, io, "tmpfile", lc_io_tmpfile);
    lc_add_native(ctx, io, "popen", lc_io_popen);
    ctx->file_methods = lc_table(ctx);
    lc_add_native(ctx, ctx->file_methods, "close", lc_file_close); lc_add_native(ctx, ctx->file_methods, "flush", lc_file_flush);
    lc_add_native(ctx, ctx->file_methods, "read", lc_file_read); lc_add_native(ctx, ctx->file_methods, "write", lc_file_write);
    lc_add_native(ctx, ctx->file_methods, "seek", lc_file_seek); lc_add_native(ctx, ctx->file_methods, "setvbuf", lc_file_setvbuf);
    lc_add_native(ctx, ctx->file_methods, "lines", lc_file_lines);
    ctx->input = lc_file_value(ctx, stdin, 1, 0); ctx->output = lc_file_value(ctx, stdout, 1, 0); ctx->error_output = lc_file_value(ctx, stderr, 1, 0);
    lc_set_field(ctx, io, "stdin", lc_retain(ctx->input)); lc_set_field(ctx, io, "stdout", lc_retain(ctx->output)); lc_set_field(ctx, io, "stderr", lc_retain(ctx->error_output));
    lc_set_global(ctx, "io", io);
    os = lc_table(ctx); lc_add_native(ctx, os, "clock", lc_os_clock); lc_add_native(ctx, os, "time", lc_os_time);
    lc_add_native(ctx, os, "date", lc_os_date); lc_add_native(ctx, os, "difftime", lc_os_difftime);
    lc_add_native(ctx, os, "getenv", lc_os_getenv); lc_add_native(ctx, os, "remove", lc_os_fileop); lc_add_native(ctx, os, "rename", lc_os_fileop);
    lc_add_native(ctx, os, "tmpname", lc_os_tmpname); lc_add_native(ctx, os, "execute", lc_os_execute);
    lc_add_native(ctx, os, "exit", lc_os_exit); lc_add_native(ctx, os, "setlocale", lc_os_setlocale);
    lc_set_global(ctx, "os", os);
    package = lc_table(ctx); loaded = lc_table(ctx); preload = lc_table(ctx); searchers = lc_table(ctx);
    lc_set_field(ctx, package, "loaded", loaded); lc_set_field(ctx, package, "preload", preload); lc_set_field(ctx, package, "searchers", searchers);
    lc_set_field(ctx, package, "path", lc_string(ctx, ""));

    lc_set_field(ctx, package, "cpath", lc_string(ctx, ""));
#ifdef _WIN32
    lc_set_field(ctx, package, "config", lc_string(ctx, "\\\n;\n?\n!\n-\n"));
#else
    lc_set_field(ctx, package, "config", lc_string(ctx, "/\n;\n?\n!\n-\n"));
#endif
    lc_set_global(ctx, "package", package);
    lc_register_libraries(ctx);
    ctx->gc_debt = -(intptr_t)ctx->gc_threshold;
    return ctx;
}

static void lc_context_destroy(LcContext *ctx) {
    LcObject *o, *next;
    LcSlab *s, *snext;
    LcStructDesc *sd, *sdnext;
    int i;
    if (!ctx) return;
    o = ctx->gc_all; ctx->gc_all = NULL;
    while (o) {
        next = o->next;
        lc_gc_free_obj(ctx, o);
        o = next;
    }
    for (s = ctx->slabs; s; s = snext) { snext = s->next; free(s); }
    for (sd = ctx->struct_descs; sd; sd = sdnext) {
        sdnext = sd->next;
        free(sd->fields);
        free((void *)sd->method_names);
        free(sd->methods);
        free(sd);
    }
    for (i = 0; i < ctx->sprite_count; ++i) free(ctx->sprites[i].path);
    free(ctx->sprites);
    lc_clear_draw_cmds(ctx);
    free(ctx->draw_cmds);
    free(ctx->current_font);
    free(ctx->c_roots);
#ifdef _WIN32
    if (ctx->socket_initialized) WSACleanup();
#endif
    free(ctx->captured_output);
    free(ctx);
}

static int lc_report_error(LcContext *ctx) {
    if (!ctx->error) return 0;
    fprintf(stderr, "%s:%d: %s\n", ctx->source ? ctx->source : "luac", ctx->line, ctx->message);
    return 1;
}

#endif
