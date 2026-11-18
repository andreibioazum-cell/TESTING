#ifndef LUAC_JIT_H
#define LUAC_JIT_H

/* Native machine-code JIT compiler for LuaC embedded/Android runtimes.
   Compiles every module, function, closure, and dynamic load() chunk directly
   into executable x86-64 (SysV/Win64) or ARM64 (AAPCS64) machine code — no AST
   interpreter and no bytecode VM dispatch. */

#include "runtime.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#elif defined(__linux__) || defined(__ANDROID__)
#define MAP_ANONYMOUS 0x20
#endif
#endif
#endif

typedef struct JitProto JitProto;
typedef struct JitModule JitModule;
typedef struct JitSession JitSession;
typedef struct JitCall JitCall;

typedef void (*JitCodeFn)(JitCall *call);

struct JitCall {
    LcContext *ctx;
    LcFunction *self;
    LcValue *argv;
    int64_t argc;
    LcValue *slots;
    LcCell **cells;
    LcValue *consts;
    void **ptrs;
    LcValue ret;
};

typedef struct {
    int is_upvalue;
    int index;
} JitCap;

typedef struct {
    char *data;
    size_t len;
} JitStrLit;

typedef struct {
    char *name;
    char *base_name;
    int nfields;
    char **field_names;
    int *field_vtypes;
    int nmethods;
    char **method_names;
} JitStructSpec;

struct JitProto {
    uint8_t *code;
    size_t code_size;
    int max_slots;
    int max_cells;
    int nparams;
    int *param_vtypes;
    int vararg;
    int ncaps;
    JitCap *caps;
    LcValue *consts;
    int nconsts;
    void **ptrs;
    int nptrs;
    JitStrLit **str_lits;
    int nstr_lits;
    JitStructSpec **struct_specs;
    int nstruct_specs;
    char *name;
    JitProto *next;
};

struct JitModule {
    char *name;
    char *source;
    Parser parser;
    Stmt *program;
    JitProto *entry;
    JitModule *next;
};

struct JitSession {
    LcContext *ctx;
    JitProto *protos;
    JitModule *modules;
    char *root_path;
    char *previous_cwd;
    size_t output_offset;
    long budget;
    char **top_globals;
    int ntop_globals;
};

static JitSession *jit_current;

/* ---------------- Executable memory allocation ---------------- */

static size_t jit_page_size(void) {
#ifdef _WIN32
    return 4096;
#else
    long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? (size_t)ps : 4096;
#endif
}

static void *jit_alloc_exec(const uint8_t *buf, size_t size) {
    size_t page = jit_page_size();
    size_t alloc_size = (size + page - 1) & ~(page - 1);
    if (alloc_size == 0) alloc_size = page;
#ifdef _WIN32
    void *mem = VirtualAlloc(NULL, alloc_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD old_prot;
    if (!mem) return NULL;
    memcpy(mem, buf, size);
    if (!VirtualProtect(mem, alloc_size, PAGE_EXECUTE_READ, &old_prot)) {
        VirtualFree(mem, 0, MEM_RELEASE);
        return NULL;
    }
    FlushInstructionCache(GetCurrentProcess(), mem, size);
    return mem;
#else
#if defined(__APPLE__) && defined(MAP_JIT)
    void *mem = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
#else
    void *mem = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (mem == MAP_FAILED) return NULL;
    memcpy(mem, buf, size);
    if (mprotect(mem, alloc_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, alloc_size);
        return NULL;
    }
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char *)mem, (char *)mem + size);
#endif
    return mem;
#endif
}

static void jit_free_exec(void *mem, size_t size) {
    size_t page = jit_page_size();
    size_t alloc_size = (size + page - 1) & ~(page - 1);
    if (!mem) return;
    if (alloc_size == 0) alloc_size = page;
#ifdef _WIN32
    (void)alloc_size;
    VirtualFree(mem, 0, MEM_RELEASE);
#else
    munmap(mem, alloc_size);
#endif
}

/* ---------------- Runtime trampoline & native helpers ---------------- */

static LcValue jit_trampoline(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    JitProto *proto = (JitProto *)self->jit_proto;
    LcValue stack_slots[64];
    LcCell *stack_cells[64];
    LcValue *slots = stack_slots;
    LcCell **cells = stack_cells;
    int ns = proto->max_slots > 0 ? proto->max_slots : 1;
    int nc = proto->max_cells > 0 ? proto->max_cells : 1;
    LcGcFrame frame;
    JitCall call;
    int i;

    if (ns > 64) {
        slots = (LcValue *)calloc((size_t)ns, sizeof(LcValue));
        if (!slots) { lc_error(ctx, "out of memory in JIT frame"); return lc_nil(); }
    } else {
        memset(slots, 0, sizeof(LcValue) * (size_t)ns);
    }
    if (nc > 64) {
        cells = (LcCell **)calloc((size_t)nc, sizeof(LcCell *));
        if (!cells) { if (slots != stack_slots) free(slots); lc_error(ctx, "out of memory in JIT cells"); return lc_nil(); }
    } else {
        memset(cells, 0, sizeof(LcCell *) * (size_t)nc);
    }

    frame.prev = ctx->gc_frames;
    frame.slots = slots;
    frame.slot_count = proto->max_slots;
    frame.cells = cells;
    frame.cell_count = proto->max_cells;
    ctx->gc_frames = &frame;

    for (i = 0; i < proto->nparams; ++i) {
        LcValue val = i < argc ? lc_first(argv[i]) : lc_nil();
        int pt = proto->param_vtypes ? proto->param_vtypes[i] : 0;
        if (pt) val = lc_check_type(ctx, val, pt, "arg");
        lc_cell_replace(ctx, &cells[i], val);
    }
    if (proto->vararg) {
        int n = argc > proto->nparams ? argc - proto->nparams : 0;
        slots[0] = lc_tuple_new(ctx, n, argv + proto->nparams, 0);
    }

    call.ctx = ctx;
    call.self = self;
    call.argv = argv;
    call.argc = argc;
    call.slots = slots;
    call.cells = cells;
    call.consts = proto->consts;
    call.ptrs = proto->ptrs;
    call.ret = lc_nil();

    if (!ctx->error) {
        ((JitCodeFn)proto->code)(&call);
    }

    ctx->gc_frames = frame.prev;
    if (slots != stack_slots) free(slots);
    if (cells != stack_cells) free(cells);
    return call.ret;
}

static int64_t jit_h_gc_check(JitCall *c, int64_t line) {
    c->ctx->line = (int)line;
    lc_gc_check(c->ctx);
    if (jit_current && --jit_current->budget <= 0) {
        lc_error(c->ctx, "execution budget exceeded (possible infinite loop)");
    }
    return c->ctx->error ? 1 : 0;
}

static int64_t jit_h_has_error(JitCall *c) {
    return c->ctx->error ? 1 : 0;
}

static void jit_h_clear_slot(JitCall *c, int64_t slot) {
    c->slots[slot].type = LC_NIL;
}

static void jit_h_clear_range(JitCall *c, int64_t base, int64_t count) {
    int64_t i;
    for (i = 0; i < count; ++i) c->slots[base + i].type = LC_NIL;
}

static void jit_h_load_const(JitCall *c, int64_t dst, int64_t kidx) {
    c->slots[dst] = c->consts[kidx];
}

static void jit_h_load_str(JitCall *c, int64_t dst, int64_t sidx) {
    JitStrLit *lit = (JitStrLit *)c->ptrs[sidx];
    c->slots[dst] = lc_string_n(c->ctx, lit->data, lit->len);
}

static void jit_h_copy_slot(JitCall *c, int64_t dst, int64_t src) {
    c->slots[dst] = c->slots[src];
}

static void jit_h_first(JitCall *c, int64_t slot) {
    c->slots[slot] = lc_first(c->slots[slot]);
}

static void jit_h_vararg(JitCall *c, int64_t dst) {
    c->slots[dst] = c->slots[0];
}

static void jit_h_get_global(JitCall *c, int64_t dst, int64_t sidx) {
    JitStrLit *lit = (JitStrLit *)c->ptrs[sidx];
    LcValue k = lc_string_n(c->ctx, lit->data, lit->len);
    c->slots[dst] = lc_table_get(c->ctx, c->ctx->globals, k);
}

static void jit_h_set_global(JitCall *c, int64_t sidx, int64_t src) {
    JitStrLit *lit = (JitStrLit *)c->ptrs[sidx];
    LcValue k = lc_string_n(c->ctx, lit->data, lit->len);
    lc_table_set(c->ctx, c->ctx->globals, k, c->slots[src]);
}

static void jit_h_cell_get(JitCall *c, int64_t dst, int64_t cell_idx) {
    c->slots[dst] = lc_cell_get(c->cells[cell_idx]);
}

static void jit_h_cell_set(JitCall *c, int64_t cell_idx, int64_t src, int64_t vtype) {
    LcValue v = c->slots[src];
    if (vtype) v = lc_check_type(c->ctx, v, (int)vtype, "local");
    lc_cell_set(c->ctx, c->cells[cell_idx], v);
}

static void jit_h_cell_new(JitCall *c, int64_t cell_idx, int64_t src, int64_t vtype) {
    LcValue v = src >= 0 ? c->slots[src] : lc_nil();
    if (vtype) v = lc_check_type(c->ctx, v, (int)vtype, "local");
    lc_cell_replace(c->ctx, &c->cells[cell_idx], v);
}

static void jit_h_up_get(JitCall *c, int64_t dst, int64_t up_idx) {
    c->slots[dst] = lc_cell_get(c->self->upvalues[up_idx]);
}

static void jit_h_up_set(JitCall *c, int64_t up_idx, int64_t src, int64_t vtype) {
    LcValue v = c->slots[src];
    if (vtype) v = lc_check_type(c->ctx, v, (int)vtype, "upvalue");
    lc_cell_set(c->ctx, c->self->upvalues[up_idx], v);
}

static void jit_h_nth_into_cell(JitCall *c, int64_t cell_idx, int64_t rhs_slot, int64_t idx, int64_t pack) {
    int ridx = (int)(pack & 0xFFFF);
    int vtype = (int)((pack >> 16) & 0xFF);
    int declare = (int)((pack >> 24) & 0xFF);
    LcValue v = rhs_slot >= 0 ? lc_nth(c->slots[rhs_slot + ridx], (int)idx) : lc_nil();
    if (vtype) v = lc_check_type(c->ctx, v, vtype, "local");
    if (declare) lc_cell_replace(c->ctx, &c->cells[cell_idx], v);
    else lc_cell_set(c->ctx, c->cells[cell_idx], v);
}

static void jit_h_nth_into_up(JitCall *c, int64_t up_idx, int64_t rhs_slot, int64_t idx, int64_t pack) {
    int ridx = (int)(pack & 0xFFFF);
    int vtype = (int)((pack >> 16) & 0xFF);
    LcValue v = rhs_slot >= 0 ? lc_nth(c->slots[rhs_slot + ridx], (int)idx) : lc_nil();
    if (vtype) v = lc_check_type(c->ctx, v, vtype, "upvalue");
    lc_cell_set(c->ctx, c->self->upvalues[up_idx], v);
}

static void jit_h_nth_into_global(JitCall *c, int64_t sidx, int64_t rhs_slot, int64_t idx, int64_t ridx) {
    JitStrLit *lit = (JitStrLit *)c->ptrs[sidx];
    LcValue k = lc_string_n(c->ctx, lit->data, lit->len);
    LcValue v = rhs_slot >= 0 ? lc_nth(c->slots[rhs_slot + ridx], (int)idx) : lc_nil();
    lc_table_set(c->ctx, c->ctx->globals, k, v);
}

static void jit_h_nth_into_index(JitCall *c, int64_t tbl_slot, int64_t key_slot, int64_t rhs_slot, int64_t pack) {
    int ridx = (int)(pack & 0xFFFF);
    int idx = (int)((pack >> 16) & 0xFFFF);
    LcValue v = rhs_slot >= 0 ? lc_nth(c->slots[rhs_slot + ridx], idx) : lc_nil();
    lc_table_set(c->ctx, c->slots[tbl_slot], c->slots[key_slot], v);
}

static void jit_h_unary(JitCall *c, int64_t dst, int64_t op, int64_t src) {
    if (op == 3) c->slots[dst] = lc_first(c->slots[src]);
    else c->slots[dst] = lc_unary(c->ctx, (int)op, c->slots[src]);
}

static void jit_h_binary(JitCall *c, int64_t dst, int64_t op, int64_t lhs, int64_t rhs) {
    c->slots[dst] = lc_binary(c->ctx, (int)op, c->slots[lhs], c->slots[rhs]);
}

static void jit_h_table_new(JitCall *c, int64_t dst) {
    c->slots[dst] = lc_table(c->ctx);
}

static void jit_h_table_get(JitCall *c, int64_t dst, int64_t tbl, int64_t key) {
    c->slots[dst] = lc_table_get(c->ctx, c->slots[tbl], c->slots[key]);
}

static void jit_h_table_set(JitCall *c, int64_t tbl, int64_t key, int64_t val) {
    lc_table_set(c->ctx, c->slots[tbl], c->slots[key], c->slots[val]);
}

static void jit_h_table_set_int(JitCall *c, int64_t tbl, int64_t idx, int64_t val) {
    lc_table_set(c->ctx, c->slots[tbl], lc_integer(idx), c->slots[val]);
}

static void jit_h_table_add(JitCall *c, int64_t tbl, int64_t idx, int64_t val) {
    lc_table_add(c->ctx, c->slots[tbl], (int)idx, c->slots[val]);
}

static void jit_h_method_get(JitCall *c, int64_t dst, int64_t obj, int64_t sidx) {
    JitStrLit *lit = (JitStrLit *)c->ptrs[sidx];
    LcValue k = lc_string_n(c->ctx, lit->data, lit->len);
    c->slots[dst] = lc_method_get(c->ctx, c->slots[obj], k);
}

static void jit_h_call(JitCall *c, int64_t dst, int64_t fn_slot, int64_t arg_base, int64_t argc) {
    c->slots[dst] = lc_call_list(c->ctx, c->slots[fn_slot], (int)argc, &c->slots[arg_base]);
}

static void jit_h_closure(JitCall *c, int64_t dst, int64_t proto_idx) {
    JitProto *sub = (JitProto *)c->ptrs[proto_idx];
    LcCell *stack_caps[32];
    LcCell **caps = sub->ncaps > 32 ? (LcCell **)malloc(sizeof(LcCell *) * (size_t)sub->ncaps) : stack_caps;
    int i;
    LcValue fn;
    if (!caps) { lc_error(c->ctx, "out of memory"); return; }
    for (i = 0; i < sub->ncaps; ++i) {
        JitCap cap = sub->caps[i];
        caps[i] = cap.is_upvalue ? c->self->upvalues[cap.index] : c->cells[cap.index];
    }
    fn = lc_function_new(c->ctx, jit_trampoline, sub->ncaps, caps, sub->name ? sub->name : "anonymous");
    if (fn.type == LC_FUNCTION) fn.as.function->jit_proto = sub;
    if (caps != stack_caps) free(caps);
    c->slots[dst] = fn;
}

static void jit_h_struct(JitCall *c, int64_t spec_idx, int64_t base_slot) {
    JitStructSpec *sp = (JitStructSpec *)c->ptrs[spec_idx];
    LcStructDesc *base_desc = NULL, *desc;
    int fbase, mbase, i;
    if (sp->base_name) {
        LcValue bk = lc_string(c->ctx, sp->base_name);
        LcValue bfn = lc_table_get(c->ctx, c->ctx->globals, bk);
        if (bfn.type == LC_FUNCTION) base_desc = bfn.as.function->struct_desc;
    }
    desc = lc_struct_desc_new(c->ctx, sp->name, base_desc, sp->nfields, sp->nmethods);
    if (!desc) return;
    fbase = base_desc ? base_desc->field_count : 0;
    mbase = base_desc ? base_desc->method_count : 0;
    for (i = 0; i < sp->nfields; ++i) {
        lc_struct_desc_add_field(desc, fbase + i, sp->field_names[i], sp->field_vtypes[i], c->slots[base_slot + i]);
    }
    for (i = 0; i < sp->nmethods; ++i) {
        lc_struct_desc_add_method(desc, mbase + i, sp->method_names[i], c->slots[base_slot + sp->nfields + i]);
    }
    lc_struct_register(c->ctx, desc);
}

static int64_t jit_h_truthy(JitCall *c, int64_t slot) {
    return lc_truthy(c->slots[slot]) ? 1 : 0;
}

static int64_t jit_h_truthy_clear(JitCall *c, int64_t slot) {
    int64_t r = lc_truthy(c->slots[slot]) ? 1 : 0;
    c->slots[slot].type = LC_NIL;
    return r;
}

static void jit_h_for_prep(JitCall *c, int64_t start_slot, int64_t limit_slot, int64_t step_slot) {
    int ok = 0;
    double s = lc_to_number(c->ctx, c->slots[start_slot], &ok);
    double l = lc_to_number(c->ctx, c->slots[limit_slot], &ok);
    double st = lc_to_number(c->ctx, c->slots[step_slot], &ok);
    c->slots[start_slot] = lc_number(s);
    c->slots[limit_slot] = lc_number(l);
    c->slots[step_slot] = lc_number(st);
}

static int64_t jit_h_for_cond(JitCall *c, int64_t idx_slot, int64_t limit_slot, int64_t step_slot) {
    double cur = c->slots[idx_slot].as.number;
    double limit = c->slots[limit_slot].as.number;
    double step = c->slots[step_slot].as.number;
    return (step >= 0.0 ? cur <= limit : cur >= limit) ? 1 : 0;
}

static void jit_h_for_step(JitCall *c, int64_t idx_slot, int64_t step_slot) {
    c->slots[idx_slot].as.number += c->slots[step_slot].as.number;
}

static void jit_h_forin_prep(JitCall *c, int64_t raw_base, int64_t nexprs, int64_t state_base) {
    int i;
    for (i = 0; i < 3; ++i) {
        int ri = i < (int)nexprs - 1 ? i : (int)nexprs - 1;
        int idx = i < (int)nexprs - 1 ? 0 : i - ((int)nexprs - 1);
        c->slots[state_base + i] = ri < 0 ? lc_nil() : lc_nth(c->slots[raw_base + ri], idx);
    }
}

static int64_t jit_h_forin_next(JitCall *c, int64_t state_base, int64_t res_slot) {
    LcValue args[2];
    LcValue first;
    args[0] = c->slots[state_base + 1];
    args[1] = c->slots[state_base + 2];
    c->slots[res_slot] = lc_call_list(c->ctx, c->slots[state_base + 0], 2, args);
    if (c->ctx->error) return 0;
    first = lc_nth(c->slots[res_slot], 0);
    if (first.type == LC_NIL) return 0;
    c->slots[state_base + 2] = first;
    return 1;
}

static void jit_h_forin_bind(JitCall *c, int64_t cell_idx, int64_t res_slot, int64_t nth_idx) {
    LcValue v = lc_nth(c->slots[res_slot], (int)nth_idx);
    lc_cell_replace(c->ctx, &c->cells[cell_idx], v);
}

static void jit_h_ret_nil(JitCall *c) {
    c->ret = lc_nil();
}

static void jit_h_ret_slot(JitCall *c, int64_t slot) {
    c->ret = c->slots[slot];
}

static void jit_h_ret_tuple(JitCall *c, int64_t base, int64_t count) {
    c->ret = lc_tuple_list(c->ctx, (int)count, &c->slots[base]);
}

/* ---------------- Native machine-code emitter ---------------- */

typedef struct {
    char *name;
    int kind; /* 0 = cell, 1 = upvalue, 2 = global */
    int index;
    int vtype;
} JitSym;

typedef struct {
    char *name;
    size_t offset;
} JitLabel;

typedef struct {
    char *name;
    size_t patch_off;
} JitGotoFixup;

typedef struct JitComp {
    JitSession *session;
    struct JitComp *parent;
    JitProto *proto;
    JitSym **owned_syms;
    int nowned_syms;
    JitSym **locals;
    int nlocals;
    JitSym **caps;
    int ncaps;
    int *cap_src_is_up;
    int *cap_src_idx;
    char **explicit_globals;
    int n_explicit_globals;
    int next_slot;
    int max_slots;
    int next_cell;
    int loop_depth;
    size_t *break_patches;
    int nbreaks;
    int cap_breaks;
    size_t *error_patches;
    int nerrors;
    int cap_errors;
    size_t *ret_patches;
    int nrets;
    int cap_rets;
    JitLabel *labels;
    int nlabels;
    JitGotoFixup *gotos;
    int ngotos;
    uint8_t *code;
    size_t code_len;
    size_t code_cap;
    int failed;
} JitComp;

static void jit_emit_u8(JitComp *jc, uint8_t b) {
    if (jc->code_len + 1 > jc->code_cap) {
        jc->code_cap = jc->code_cap ? jc->code_cap * 2 : 1024;
        jc->code = (uint8_t *)xrealloc(jc->code, jc->code_cap);
    }
    jc->code[jc->code_len++] = b;
}
static void jit_emit_u32(JitComp *jc, uint32_t v) {
    jit_emit_u8(jc, (uint8_t)(v & 0xFF));
    jit_emit_u8(jc, (uint8_t)((v >> 8) & 0xFF));
    jit_emit_u8(jc, (uint8_t)((v >> 16) & 0xFF));
    jit_emit_u8(jc, (uint8_t)((v >> 24) & 0xFF));
}
static void jit_emit_u64(JitComp *jc, uint64_t v) {
    jit_emit_u32(jc, (uint32_t)(v & 0xFFFFFFFFu));
    jit_emit_u32(jc, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

#if defined(__aarch64__) || defined(_M_ARM64)

static void jit_arm_mov_imm64(JitComp *jc, int reg, uint64_t val) {
    uint32_t imm0 = (uint32_t)(val & 0xFFFFu);
    jit_emit_u32(jc, 0xD2800000u | (imm0 << 5) | (uint32_t)reg);
    if ((val >> 16) & 0xFFFFu) {
        uint32_t imm1 = (uint32_t)((val >> 16) & 0xFFFFu);
        jit_emit_u32(jc, 0xF2A00000u | (imm1 << 5) | (uint32_t)reg);
    }
    if ((val >> 32) & 0xFFFFu) {
        uint32_t imm2 = (uint32_t)((val >> 32) & 0xFFFFu);
        jit_emit_u32(jc, 0xF2C00000u | (imm2 << 5) | (uint32_t)reg);
    }
    if ((val >> 48) & 0xFFFFu) {
        uint32_t imm3 = (uint32_t)((val >> 48) & 0xFFFFu);
        jit_emit_u32(jc, 0xF2E00000u | (imm3 << 5) | (uint32_t)reg);
    }
}

static void jit_emit_prologue(JitComp *jc) {
    jit_emit_u32(jc, 0xA9BE7BFDu); /* stp x29, x30, [sp, #-32]! */
    jit_emit_u32(jc, 0x910003FDu); /* mov x29, sp */
    jit_emit_u32(jc, 0xF9000BF3u); /* str x19, [sp, #16] */
    jit_emit_u32(jc, 0xAA0003F3u); /* mov x19, x0 */
}

static void jit_emit_epilogue(JitComp *jc) {
    jit_emit_u32(jc, 0xF9400BF3u); /* ldr x19, [sp, #16] */
    jit_emit_u32(jc, 0xA8C27BFDu); /* ldp x29, x30, [sp], #32 */
    jit_emit_u32(jc, 0xD65F03C0u); /* ret */
}

static void jit_emit_call_c(JitComp *jc, void *fn, int nargs, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {
    jit_emit_u32(jc, 0xAA1303E0u); /* mov x0, x19 */
    if (nargs >= 1) jit_arm_mov_imm64(jc, 1, (uint64_t)a1);
    if (nargs >= 2) jit_arm_mov_imm64(jc, 2, (uint64_t)a2);
    if (nargs >= 3) jit_arm_mov_imm64(jc, 3, (uint64_t)a3);
    if (nargs >= 4) jit_arm_mov_imm64(jc, 4, (uint64_t)a4);
    jit_arm_mov_imm64(jc, 16, (uint64_t)(uintptr_t)fn);
    jit_emit_u32(jc, 0xD63F0200u); /* blr x16 */
}

static size_t jit_emit_jmp(JitComp *jc) {
    size_t off = jc->code_len;
    jit_emit_u32(jc, 0x14000000u);
    return off;
}
static size_t jit_emit_jz(JitComp *jc) {
    size_t off;
    jit_emit_u32(jc, 0xB5000040u); /* cbnz x0, +8 */
    off = jc->code_len;
    jit_emit_u32(jc, 0x14000000u); /* b target */
    return off;
}
static size_t jit_emit_jnz(JitComp *jc) {
    size_t off;
    jit_emit_u32(jc, 0xB4000040u); /* cbz x0, +8 */
    off = jc->code_len;
    jit_emit_u32(jc, 0x14000000u); /* b target */
    return off;
}
static void jit_patch_jump(JitComp *jc, size_t off, size_t target) {
    int32_t delta = (int32_t)((intptr_t)target - (intptr_t)off) / 4;
    uint32_t insn = 0x14000000u | ((uint32_t)delta & 0x03FFFFFFu);
    memcpy(jc->code + off, &insn, 4);
}

#elif defined(__x86_64__) || defined(_M_X64)

static void jit_emit_prologue(JitComp *jc) {
    jit_emit_u8(jc, 0x55); /* push rbp */
    jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0x89); jit_emit_u8(jc, 0xE5); /* mov rbp, rsp */
    jit_emit_u8(jc, 0x53); /* push rbx */
#ifdef _WIN32
    jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0x83); jit_emit_u8(jc, 0xEC); jit_emit_u8(jc, 0x38); /* sub rsp, 56 (32 shadow + 16 5th arg + 8 align) */
    jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0x89); jit_emit_u8(jc, 0xCB); /* mov rbx, rcx */
#else
    jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0x83); jit_emit_u8(jc, 0xEC); jit_emit_u8(jc, 0x08); /* sub rsp, 8 */
    jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0x89); jit_emit_u8(jc, 0xFB); /* mov rbx, rdi */
#endif
}

static void jit_emit_epilogue(JitComp *jc) {
#ifdef _WIN32
    jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0x83); jit_emit_u8(jc, 0xC4); jit_emit_u8(jc, 0x38);
#else
    jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0x83); jit_emit_u8(jc, 0xC4); jit_emit_u8(jc, 0x08);
#endif
    jit_emit_u8(jc, 0x5B); /* pop rbx */
    jit_emit_u8(jc, 0x5D); /* pop rbp */
    jit_emit_u8(jc, 0xC3); /* ret */
}

static void jit_emit_call_c(JitComp *jc, void *fn, int nargs, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {
#ifdef _WIN32
    jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0x89); jit_emit_u8(jc, 0xD9); /* mov rcx, rbx */
    if (nargs >= 1) { jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0xBA); jit_emit_u64(jc, (uint64_t)a1); } /* rdx */
    if (nargs >= 2) { jit_emit_u8(jc, 0x49); jit_emit_u8(jc, 0xB8); jit_emit_u64(jc, (uint64_t)a2); } /* r8 */
    if (nargs >= 3) { jit_emit_u8(jc, 0x49); jit_emit_u8(jc, 0xB9); jit_emit_u64(jc, (uint64_t)a3); } /* r9 */
    if (nargs >= 4) {
        jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0xB8); jit_emit_u64(jc, (uint64_t)a4); /* mov rax, a4 */
        jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0x89); jit_emit_u8(jc, 0x44); jit_emit_u8(jc, 0x24); jit_emit_u8(jc, 0x20); /* mov [rsp+32], rax */
    }
#else
    jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0x89); jit_emit_u8(jc, 0xDF); /* mov rdi, rbx */
    if (nargs >= 1) { jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0xBE); jit_emit_u64(jc, (uint64_t)a1); } /* rsi */
    if (nargs >= 2) { jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0xBA); jit_emit_u64(jc, (uint64_t)a2); } /* rdx */
    if (nargs >= 3) { jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0xB9); jit_emit_u64(jc, (uint64_t)a3); } /* rcx */
    if (nargs >= 4) { jit_emit_u8(jc, 0x49); jit_emit_u8(jc, 0xB8); jit_emit_u64(jc, (uint64_t)a4); } /* r8 */
#endif
    jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0xB8); jit_emit_u64(jc, (uint64_t)(uintptr_t)fn); /* mov rax, fn */
    jit_emit_u8(jc, 0xFF); jit_emit_u8(jc, 0xD0); /* call rax */
}

static size_t jit_emit_jmp(JitComp *jc) {
    size_t off;
    jit_emit_u8(jc, 0xE9);
    off = jc->code_len;
    jit_emit_u32(jc, 0);
    return off;
}
static size_t jit_emit_jz(JitComp *jc) {
    size_t off;
    jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0x85); jit_emit_u8(jc, 0xC0); /* test rax, rax */
    jit_emit_u8(jc, 0x0F); jit_emit_u8(jc, 0x84);
    off = jc->code_len;
    jit_emit_u32(jc, 0);
    return off;
}
static size_t jit_emit_jnz(JitComp *jc) {
    size_t off;
    jit_emit_u8(jc, 0x48); jit_emit_u8(jc, 0x85); jit_emit_u8(jc, 0xC0); /* test rax, rax */
    jit_emit_u8(jc, 0x0F); jit_emit_u8(jc, 0x85);
    off = jc->code_len;
    jit_emit_u32(jc, 0);
    return off;
}
static void jit_patch_jump(JitComp *jc, size_t off, size_t target) {
    int32_t rel = (int32_t)((intptr_t)target - (intptr_t)(off + 4));
    memcpy(jc->code + off, &rel, 4);
}

#else
#error "Unsupported architecture for LuaC native JIT (requires x86_64 or arm64)"
#endif

static void jit_add_error_patch(JitComp *jc, size_t off) {
    if (jc->nerrors >= jc->cap_errors) {
        jc->cap_errors = jc->cap_errors ? jc->cap_errors * 2 : 32;
        jc->error_patches = (size_t *)xrealloc(jc->error_patches, sizeof(size_t) * (size_t)jc->cap_errors);
    }
    jc->error_patches[jc->nerrors++] = off;
}

static void jit_add_ret_patch(JitComp *jc, size_t off) {
    if (jc->nrets >= jc->cap_rets) {
        jc->cap_rets = jc->cap_rets ? jc->cap_rets * 2 : 16;
        jc->ret_patches = (size_t *)xrealloc(jc->ret_patches, sizeof(size_t) * (size_t)jc->cap_rets);
    }
    jc->ret_patches[jc->nrets++] = off;
}

static void jit_emit_check_error(JitComp *jc) {
    jit_emit_call_c(jc, (void *)jit_h_has_error, 0, 0, 0, 0, 0);
    jit_add_error_patch(jc, jit_emit_jnz(jc));
}

/* ---------------- Compiler symbol / constant / slot helpers ---------------- */

static int jit_alloc_slot(JitComp *jc) {
    int s = jc->next_slot++;
    if (jc->next_slot > jc->max_slots) jc->max_slots = jc->next_slot;
    return s;
}

static int jit_alloc_slots(JitComp *jc, int n) {
    int base = jc->next_slot;
    jc->next_slot += n;
    if (jc->next_slot > jc->max_slots) jc->max_slots = jc->next_slot;
    return base;
}

static int jit_add_const(JitComp *jc, LcValue v) {
    JitProto *p = jc->proto;
    p->consts = (LcValue *)xrealloc(p->consts, sizeof(LcValue) * (size_t)(p->nconsts + 1));
    p->consts[p->nconsts] = v;
    return p->nconsts++;
}

static int jit_add_ptr(JitComp *jc, void *ptr) {
    JitProto *p = jc->proto;
    p->ptrs = (void **)xrealloc(p->ptrs, sizeof(void *) * (size_t)(p->nptrs + 1));
    p->ptrs[p->nptrs] = ptr;
    return p->nptrs++;
}

static int jit_add_str(JitComp *jc, const char *s, size_t len) {
    JitProto *p = jc->proto;
    int i;
    for (i = 0; i < p->nstr_lits; ++i) {
        if (p->str_lits[i]->len == len && (!len || memcmp(p->str_lits[i]->data, s, len) == 0)) {
            int pi;
            for (pi = 0; pi < p->nptrs; ++pi) if (p->ptrs[pi] == p->str_lits[i]) return pi;
        }
    }
    {
        JitStrLit *lit = (JitStrLit *)xmalloc(sizeof(JitStrLit));
        lit->data = (char *)xmalloc(len + 1);
        if (len) memcpy(lit->data, s, len);
        lit->data[len] = '\0';
        lit->len = len;
        p->str_lits = (JitStrLit **)xrealloc(p->str_lits, sizeof(JitStrLit *) * (size_t)(p->nstr_lits + 1));
        p->str_lits[p->nstr_lits++] = lit;
        return jit_add_ptr(jc, lit);
    }
}

static JitSym *jit_new_local(JitComp *jc, const char *name, int declare, int vtype) {
    JitSym *sym = (JitSym *)xmalloc(sizeof(JitSym));
    sym->name = xstrdup(name);
    sym->kind = 0;
    sym->index = jc->next_cell++;
    sym->vtype = vtype;
    jc->owned_syms = (JitSym **)xrealloc(jc->owned_syms, sizeof(JitSym *) * (size_t)(jc->nowned_syms + 1));
    jc->owned_syms[jc->nowned_syms++] = sym;
    if (jc->next_cell > jc->proto->max_cells) jc->proto->max_cells = jc->next_cell;
    if (declare) {
        jc->locals = (JitSym **)xrealloc(jc->locals, sizeof(JitSym *) * (size_t)(jc->nlocals + 1));
        jc->locals[jc->nlocals++] = sym;
    }
    return sym;
}

static void jit_declare_global(JitComp *jc, const char *name) {
    int i;
    if (!jc || !name) return;
    for (i = 0; i < jc->n_explicit_globals; ++i) if (!strcmp(jc->explicit_globals[i], name)) return;
    jc->explicit_globals = (char **)xrealloc(jc->explicit_globals, sizeof(char *) * (size_t)(jc->n_explicit_globals + 1));
    jc->explicit_globals[jc->n_explicit_globals++] = xstrdup(name);
}

static int jit_is_explicit_global(JitComp *jc, const char *name) {
    int i;
    if (!jc || !name) return 0;
    for (i = 0; i < jc->n_explicit_globals; ++i) if (!strcmp(jc->explicit_globals[i], name)) return 1;
    return 0;
}

static int jit_is_top_global(JitSession *session, const char *name) {
    int i;
    if (!session || !name) return 0;
    for (i = 0; i < session->ntop_globals; ++i) if (!strcmp(session->top_globals[i], name)) return 1;
    return 0;
}

static void jit_add_top_global(JitSession *session, const char *name) {
    int i;
    if (!session || !name) return;
    for (i = 0; i < session->ntop_globals; ++i) if (!strcmp(session->top_globals[i], name)) return;
    session->top_globals = (char **)xrealloc(session->top_globals, sizeof(char *) * (size_t)(session->ntop_globals + 1));
    session->top_globals[session->ntop_globals++] = xstrdup(name);
}

static void jit_collect_top_globals(JitSession *session, Stmt *s) {
    for (; s; s = s->next) {
        int i;
        if (s->kind == S_ASSIGN && s->flag > 0) {
            for (i = 0; i < s->flag; ++i)
                if (s->exprs[i] && s->exprs[i]->kind == E_VAR)
                    jit_add_top_global(session, s->exprs[i]->name);
        } else if (s->kind == S_ASSIGN && s->expr && s->expr->kind == E_VAR) {
            jit_add_top_global(session, s->expr->name);
        } else if (s->kind == S_STRUCT && s->nnames > 0) {
            jit_add_top_global(session, s->names[0]);
        }
    }
}

static JitSym jit_global_sym = { "", 2, 0, 0 };

static JitSym *jit_resolve(JitComp *jc, const char *name) {
    int i;
    if (!jc) return &jit_global_sym;
    if (jit_is_explicit_global(jc, name)) return &jit_global_sym;
    for (i = jc->nlocals - 1; i >= 0; --i) {
        if (!strcmp(jc->locals[i]->name, name)) return jc->locals[i];
    }
    if (jc->parent) {
        JitSym *p = jit_resolve(jc->parent, name);
        if (p->kind != 2) {
            for (i = 0; i < jc->ncaps; ++i) {
                if (!strcmp(jc->caps[i]->name, name)) return jc->caps[i];
            }
            {
                JitSym *up = (JitSym *)xmalloc(sizeof(JitSym));
                int idx = jc->ncaps;
                up->name = xstrdup(name);
                up->kind = 1;
                up->index = idx;
                up->vtype = p->vtype;
                jc->owned_syms = (JitSym **)xrealloc(jc->owned_syms, sizeof(JitSym *) * (size_t)(jc->nowned_syms + 1));
                jc->owned_syms[jc->nowned_syms++] = up;
                jc->caps = (JitSym **)xrealloc(jc->caps, sizeof(JitSym *) * (size_t)(jc->ncaps + 1));
                jc->caps[jc->ncaps] = up;
                jc->cap_src_is_up = (int *)xrealloc(jc->cap_src_is_up, sizeof(int) * (size_t)(jc->ncaps + 1));
                jc->cap_src_idx = (int *)xrealloc(jc->cap_src_idx, sizeof(int) * (size_t)(jc->ncaps + 1));
                jc->cap_src_is_up[jc->ncaps] = (p->kind == 1);
                jc->cap_src_idx[jc->ncaps] = p->index;
                jc->ncaps++;
                return up;
            }
        }
    }
    return &jit_global_sym;
}

static JitProto *jit_compile_fn(JitSession *session, JitComp *parent, Expr *e, const char *name);
static void jit_compile_stmts(JitComp *jc, Stmt *s);

static int jit_compile_expr(JitComp *jc, Expr *e, int dst) {
    int i;
    if (!e) {
        int k = jit_add_const(jc, lc_nil());
        jit_emit_call_c(jc, (void *)jit_h_load_const, 2, dst, k, 0, 0);
        return dst;
    }
    switch (e->kind) {
        case E_NIL: {
            int k = jit_add_const(jc, lc_nil());
            jit_emit_call_c(jc, (void *)jit_h_load_const, 2, dst, k, 0, 0);
            return dst;
        }
        case E_BOOL: {
            int k = jit_add_const(jc, lc_bool(e->flag));
            jit_emit_call_c(jc, (void *)jit_h_load_const, 2, dst, k, 0, 0);
            return dst;
        }
        case E_NUMBER: {
            LcValue v = e->is_integer ? lc_integer(e->integer) : lc_number(e->number);
            int k = jit_add_const(jc, v);
            jit_emit_call_c(jc, (void *)jit_h_load_const, 2, dst, k, 0, 0);
            return dst;
        }
        case E_STRING: {
            int sidx = jit_add_str(jc, e->name ? e->name : "", e->name ? strlen(e->name) : 0);
            jit_emit_call_c(jc, (void *)jit_h_load_str, 2, dst, sidx, 0, 0);
            return dst;
        }
        case E_VAR: {
            JitSym *sym = jit_resolve(jc, e->name);
            if (sym->kind == 2) {
                int sidx = jit_add_str(jc, e->name, strlen(e->name));
                jit_emit_call_c(jc, (void *)jit_h_get_global, 2, dst, sidx, 0, 0);
            } else if (sym->kind == 1) {
                jit_emit_call_c(jc, (void *)jit_h_up_get, 2, dst, sym->index, 0, 0);
            } else {
                jit_emit_call_c(jc, (void *)jit_h_cell_get, 2, dst, sym->index, 0, 0);
            }
            return dst;
        }
        case E_VARARG: {
            if (!jc->proto->vararg) {
                lc_error(jc->session->ctx, "cannot use '...' outside a vararg function");
                jc->failed = 1;
                return dst;
            }
            jit_emit_call_c(jc, (void *)jit_h_vararg, 1, dst, 0, 0, 0);
            return dst;
        }
        case E_UNARY: {
            int mark = jc->next_slot;
            int src = jit_alloc_slot(jc);
            jit_compile_expr(jc, e->a, src);
            jit_emit_call_c(jc, (void *)jit_h_unary, 3, dst, e->op, src, 0);
            jit_emit_call_c(jc, (void *)jit_h_clear_slot, 1, src, 0, 0, 0);
            jit_emit_check_error(jc);
            jc->next_slot = mark;
            return dst;
        }
        case E_BINARY: {
            if (e->op == 14 || e->op == 15) {
                size_t jmp_done;
                jit_compile_expr(jc, e->a, dst);
                jit_emit_call_c(jc, (void *)jit_h_first, 1, dst, 0, 0, 0);
                jit_emit_call_c(jc, (void *)jit_h_truthy, 1, dst, 0, 0, 0);
                jmp_done = (e->op == 14) ? jit_emit_jnz(jc) : jit_emit_jz(jc);
                jit_compile_expr(jc, e->b, dst);
                jit_emit_call_c(jc, (void *)jit_h_first, 1, dst, 0, 0, 0);
                jit_patch_jump(jc, jmp_done, jc->code_len);
                return dst;
            } else {
                int mark = jc->next_slot;
                int lhs = jit_alloc_slot(jc);
                int rhs = jit_alloc_slot(jc);
                jit_compile_expr(jc, e->a, lhs);
                jit_compile_expr(jc, e->b, rhs);
                jit_emit_call_c(jc, (void *)jit_h_binary, 4, dst, e->op, lhs, rhs);
                jit_emit_call_c(jc, (void *)jit_h_clear_range, 2, lhs, 2, 0, 0);
                jit_emit_check_error(jc);
                jc->next_slot = mark;
                return dst;
            }
        }
        case E_INDEX: {
            int mark = jc->next_slot;
            int tbl = jit_alloc_slot(jc);
            int key = jit_alloc_slot(jc);
            jit_compile_expr(jc, e->a, tbl);
            jit_compile_expr(jc, e->b, key);
            jit_emit_call_c(jc, (void *)jit_h_table_get, 3, dst, tbl, key, 0);
            jit_emit_call_c(jc, (void *)jit_h_clear_range, 2, tbl, 2, 0, 0);
            jit_emit_check_error(jc);
            jc->next_slot = mark;
            return dst;
        }
        case E_TABLE: {
            int mark = jc->next_slot;
            int array_index = 1;
            jit_emit_call_c(jc, (void *)jit_h_table_new, 1, dst, 0, 0, 0);
            for (i = 0; i < e->count; ++i) {
                Expr *ke = e->keys ? e->keys[i] : NULL;
                int expand = !ke && i == e->count - 1;
                int kslot = ke ? jit_alloc_slot(jc) : -1;
                int vslot = jit_alloc_slot(jc);
                if (ke) jit_compile_expr(jc, ke, kslot);
                jit_compile_expr(jc, e->items[i], vslot);
                if (expand) {
                    jit_emit_call_c(jc, (void *)jit_h_table_add, 3, dst, array_index, vslot, 0);
                } else if (ke) {
                    jit_emit_call_c(jc, (void *)jit_h_table_set, 3, dst, kslot, vslot, 0);
                } else {
                    jit_emit_call_c(jc, (void *)jit_h_table_set_int, 3, dst, array_index, vslot, 0);
                }
                if (ke) jit_emit_call_c(jc, (void *)jit_h_clear_slot, 1, kslot, 0, 0, 0);
                jit_emit_call_c(jc, (void *)jit_h_clear_slot, 1, vslot, 0, 0, 0);
                jit_emit_check_error(jc);
                if (!ke) array_index++;
                jc->next_slot = mark;
            }
            return dst;
        }
        case E_CALL: {
            int mark = jc->next_slot;
            int base = e->flag ? 1 : 0;
            int n = e->count + base;
            int fn_slot = jit_alloc_slot(jc);
            int arg_base = jit_alloc_slots(jc, n > 0 ? n : 1);
            if (e->flag) {
                int sidx = jit_add_str(jc, e->name, strlen(e->name));
                int tmp_obj = jit_alloc_slot(jc);
                jit_compile_expr(jc, e->a, tmp_obj);
                jit_emit_call_c(jc, (void *)jit_h_copy_slot, 2, arg_base, tmp_obj, 0, 0);
                jit_emit_call_c(jc, (void *)jit_h_clear_slot, 1, tmp_obj, 0, 0, 0);
                jit_emit_call_c(jc, (void *)jit_h_method_get, 3, fn_slot, arg_base, sidx, 0);
                jit_emit_check_error(jc);
            } else {
                int tmp_fn = jit_alloc_slot(jc);
                jit_compile_expr(jc, e->a, tmp_fn);
                jit_emit_call_c(jc, (void *)jit_h_copy_slot, 2, fn_slot, tmp_fn, 0, 0);
                jit_emit_call_c(jc, (void *)jit_h_clear_slot, 1, tmp_fn, 0, 0, 0);
            }
            for (i = 0; i < e->count; ++i) {
                int tmp_arg = jit_alloc_slot(jc);
                jit_compile_expr(jc, e->items[i], tmp_arg);
                jit_emit_call_c(jc, (void *)jit_h_copy_slot, 2, arg_base + i + base, tmp_arg, 0, 0);
                jit_emit_call_c(jc, (void *)jit_h_clear_slot, 1, tmp_arg, 0, 0, 0);
                jc->next_slot = arg_base + (n > 0 ? n : 1);
            }
            jit_emit_call_c(jc, (void *)jit_h_call, 4, dst, fn_slot, arg_base, n);
            jit_emit_call_c(jc, (void *)jit_h_clear_range, 2, fn_slot, 1 + (n > 0 ? n : 1), 0, 0);
            jit_emit_check_error(jc);
            jc->next_slot = mark;
            return dst;
        }
        case E_FUNCTION: {
            JitProto *sub = jit_compile_fn(jc->session, jc, e, "anonymous");
            if (!sub) { jc->failed = 1; return dst; }
            int pidx = jit_add_ptr(jc, sub);
            jit_emit_call_c(jc, (void *)jit_h_closure, 2, dst, pidx, 0, 0);
            return dst;
        }
        default:
            return dst;
    }
}

static void jit_emit_assign_targets(JitComp *jc, Stmt *s, JitSym **targets, int declare) {
    int mark = jc->next_slot;
    int nr = s->nexprs;
    int rhs_base = nr > 0 ? jit_alloc_slots(jc, nr) : -1;
    int i;
    for (i = 0; i < nr; ++i) {
        int tmp = jit_alloc_slot(jc);
        jit_compile_expr(jc, s->exprs[i], tmp);
        jit_emit_call_c(jc, (void *)jit_h_copy_slot, 2, rhs_base + i, tmp, 0, 0);
        jit_emit_call_c(jc, (void *)jit_h_clear_slot, 1, tmp, 0, 0, 0);
        jc->next_slot = rhs_base + nr;
    }
    if (declare == 2) {
        for (i = 0; i < s->nnames; ++i) {
            jc->locals = (JitSym **)xrealloc(jc->locals, sizeof(JitSym *) * (size_t)(jc->nlocals + 1));
            jc->locals[jc->nlocals++] = targets[i];
        }
    }
    for (i = 0; i < s->nnames; ++i) {
        int ri = i < nr - 1 ? i : nr - 1;
        int idx = i < nr - 1 ? 0 : i - (nr - 1);
        int64_t pack = (int64_t)(ri >= 0 ? ri : 0) | ((int64_t)(targets[i]->vtype & 0xFF) << 16) | ((int64_t)(declare ? 1 : 0) << 24);
        jit_emit_call_c(jc, (void *)jit_h_nth_into_cell, 4, targets[i]->index, ri >= 0 ? rhs_base : -1, idx, pack);
        jit_emit_check_error(jc);
    }
    if (nr > 0) jit_emit_call_c(jc, (void *)jit_h_clear_range, 2, rhs_base, nr, 0, 0);
    jc->next_slot = mark;
}

static void jit_compile_if(JitComp *jc, Stmt *s, int i, size_t *end_patches, int *nend) {
    if (i >= s->nblocks) return;
    if (!s->conds[i]) {
        jit_compile_stmts(jc, s->blocks[i]);
        return;
    }
    {
        int mark = jc->next_slot;
        int cond = jit_alloc_slot(jc);
        size_t jz_false;
        jit_compile_expr(jc, s->conds[i], cond);
        jit_emit_call_c(jc, (void *)jit_h_truthy_clear, 1, cond, 0, 0, 0);
        jc->next_slot = mark;
        jz_false = jit_emit_jz(jc);
        jit_compile_stmts(jc, s->blocks[i]);
        if (i + 1 < s->nblocks) {
            end_patches[(*nend)++] = jit_emit_jmp(jc);
        }
        jit_patch_jump(jc, jz_false, jc->code_len);
        jit_compile_if(jc, s, i + 1, end_patches, nend);
    }
}

static void jit_compile_stmt(JitComp *jc, Stmt *s) {
    int i;
    jit_emit_call_c(jc, (void *)jit_h_gc_check, 1, s->line, 0, 0, 0);
    jit_add_error_patch(jc, jit_emit_jnz(jc));

    switch (s->kind) {
        case S_LOCAL: {
            JitSym **targets = (JitSym **)xmalloc(sizeof(JitSym *) * (size_t)s->nnames);
            for (i = 0; i < s->nnames; ++i) {
                int vt = s->var_types ? s->var_types[i] : 0;
                targets[i] = jit_new_local(jc, s->names[i], 0, vt);
            }
            jit_emit_assign_targets(jc, s, targets, 2);
            free(targets);
            break;
        }
        case S_LOCALFUNC: {
            JitSym *sym = jit_new_local(jc, s->names[0], 1, 0);
            int mark = jc->next_slot;
            int fn_slot = jit_alloc_slot(jc);
            JitProto *sub;
            int pidx;
            jit_emit_call_c(jc, (void *)jit_h_cell_new, 3, sym->index, -1, 0, 0);
            sub = jit_compile_fn(jc->session, jc, s->expr, s->names[0]);
            if (!sub) { jc->failed = 1; break; }
            pidx = jit_add_ptr(jc, sub);
            jit_emit_call_c(jc, (void *)jit_h_closure, 2, fn_slot, pidx, 0, 0);
            jit_emit_call_c(jc, (void *)jit_h_cell_set, 3, sym->index, fn_slot, 0, 0);
            jit_emit_call_c(jc, (void *)jit_h_clear_slot, 1, fn_slot, 0, 0, 0);
            jc->next_slot = mark;
            break;
        }
        case S_STRUCT: {
            int mark = jc->next_slot;
            int count = s->nfields + s->nexprs;
            int base_slot = jit_alloc_slots(jc, count > 0 ? count : 1);
            JitStructSpec *sp = (JitStructSpec *)xmalloc(sizeof(JitStructSpec));
            int sp_idx;
            memset(sp, 0, sizeof(*sp));
            sp->name = xstrdup(s->names[0]);
            sp->base_name = s->nnames > 1 ? xstrdup(s->names[1]) : NULL;
            sp->nfields = s->nfields;
            sp->field_names = s->nfields ? (char **)xmalloc(sizeof(char *) * (size_t)s->nfields) : NULL;
            sp->field_vtypes = s->nfields ? (int *)xmalloc(sizeof(int) * (size_t)s->nfields) : NULL;
            sp->nmethods = s->nexprs;
            sp->method_names = s->nexprs ? (char **)xmalloc(sizeof(char *) * (size_t)s->nexprs) : NULL;
            for (i = 0; i < s->nfields; ++i) {
                sp->field_names[i] = xstrdup(s->fields[i].name);
                sp->field_vtypes[i] = s->fields[i].vtype;
                if (s->fields[i].def) {
                    int tmp = jit_alloc_slot(jc);
                    jit_compile_expr(jc, s->fields[i].def, tmp);
                    jit_emit_call_c(jc, (void *)jit_h_copy_slot, 2, base_slot + i, tmp, 0, 0);
                    jit_emit_call_c(jc, (void *)jit_h_clear_slot, 1, tmp, 0, 0, 0);
                    jc->next_slot = base_slot + (count > 0 ? count : 1);
                } else {
                    int vt = s->fields[i].vtype;
                    if (vt == 1) {
                        int k = jit_add_const(jc, lc_integer(0));
                        jit_emit_call_c(jc, (void *)jit_h_load_const, 2, base_slot + i, k, 0, 0);
                    } else if (vt == 2) {
                        int k = jit_add_const(jc, lc_number(0));
                        jit_emit_call_c(jc, (void *)jit_h_load_const, 2, base_slot + i, k, 0, 0);
                    } else if (vt == 3) {
                        int sidx = jit_add_str(jc, "", 0);
                        jit_emit_call_c(jc, (void *)jit_h_load_str, 2, base_slot + i, sidx, 0, 0);
                    } else if (vt == 4) {
                        int k = jit_add_const(jc, lc_bool(0));
                        jit_emit_call_c(jc, (void *)jit_h_load_const, 2, base_slot + i, k, 0, 0);
                    } else {
                        int k = jit_add_const(jc, lc_nil());
                        jit_emit_call_c(jc, (void *)jit_h_load_const, 2, base_slot + i, k, 0, 0);
                    }
                }
            }
            for (i = 0; i < s->nexprs; ++i) {
                JitProto *mproto = jit_compile_fn(jc->session, jc, s->exprs[i], s->method_names[i]);
                int pidx;
                sp->method_names[i] = xstrdup(s->method_names[i]);
                if (!mproto) { jc->failed = 1; break; }
                pidx = jit_add_ptr(jc, mproto);
                jit_emit_call_c(jc, (void *)jit_h_closure, 2, base_slot + s->nfields + i, pidx, 0, 0);
            }
            jc->proto->struct_specs = (JitStructSpec **)xrealloc(jc->proto->struct_specs, sizeof(JitStructSpec *) * (size_t)(jc->proto->nstruct_specs + 1));
            jc->proto->struct_specs[jc->proto->nstruct_specs++] = sp;
            sp_idx = jit_add_ptr(jc, sp);
            jit_emit_call_c(jc, (void *)jit_h_struct, 2, sp_idx, base_slot, 0, 0);
            jit_emit_call_c(jc, (void *)jit_h_clear_range, 2, base_slot, count > 0 ? count : 1, 0, 0);
            jit_emit_check_error(jc);
            jc->next_slot = mark;
            break;
        }
        case S_COMPOUND: {
            int mark = jc->next_slot;
            Expr *target = s->expr;
            int rhs = jit_alloc_slot(jc);
            jit_compile_expr(jc, s->expr2, rhs);
            if (target->kind == E_VAR) {
                JitSym *sym = jit_resolve(jc, target->name);
                int cur = jit_alloc_slot(jc);
                int res = jit_alloc_slot(jc);
                if (sym->kind == 2) {
                    int sidx = jit_add_str(jc, target->name, strlen(target->name));
                    jit_emit_call_c(jc, (void *)jit_h_get_global, 2, cur, sidx, 0, 0);
                    jit_emit_call_c(jc, (void *)jit_h_binary, 4, res, s->flag, cur, rhs);
                    jit_emit_check_error(jc);
                    jit_emit_call_c(jc, (void *)jit_h_set_global, 2, sidx, res, 0, 0);
                } else if (sym->kind == 1) {
                    jit_emit_call_c(jc, (void *)jit_h_up_get, 2, cur, sym->index, 0, 0);
                    jit_emit_call_c(jc, (void *)jit_h_binary, 4, res, s->flag, cur, rhs);
                    jit_emit_check_error(jc);
                    jit_emit_call_c(jc, (void *)jit_h_up_set, 3, sym->index, res, sym->vtype, 0);
                } else {
                    jit_emit_call_c(jc, (void *)jit_h_cell_get, 2, cur, sym->index, 0, 0);
                    jit_emit_call_c(jc, (void *)jit_h_binary, 4, res, s->flag, cur, rhs);
                    jit_emit_check_error(jc);
                    jit_emit_call_c(jc, (void *)jit_h_cell_set, 3, sym->index, res, sym->vtype, 0);
                }
                jit_emit_call_c(jc, (void *)jit_h_clear_range, 2, rhs, 3, 0, 0);
                jit_emit_check_error(jc);
            } else if (target->kind == E_INDEX) {
                int obj = jit_alloc_slot(jc);
                int key = jit_alloc_slot(jc);
                int cur = jit_alloc_slot(jc);
                int res = jit_alloc_slot(jc);
                jit_compile_expr(jc, target->a, obj);
                jit_compile_expr(jc, target->b, key);
                jit_emit_call_c(jc, (void *)jit_h_table_get, 3, cur, obj, key, 0);
                jit_emit_check_error(jc);
                jit_emit_call_c(jc, (void *)jit_h_binary, 4, res, s->flag, cur, rhs);
                jit_emit_check_error(jc);
                jit_emit_call_c(jc, (void *)jit_h_table_set, 3, obj, key, res, 0);
                jit_emit_call_c(jc, (void *)jit_h_clear_range, 2, rhs, 5, 0, 0);
                jit_emit_check_error(jc);
            } else {
                lc_error(jc->session->ctx, "invalid compound assignment target");
                jc->failed = 1;
            }
            jc->next_slot = mark;
            break;
        }
        case S_ASSIGN: {
            int mark = jc->next_slot;
            int nt = s->flag;
            int nr = s->nexprs - nt;
            if (s->is_global) {
                for (i = 0; i < nt; ++i) {
                    if (s->exprs[i] && s->exprs[i]->kind == E_VAR)
                        jit_declare_global(jc, s->exprs[i]->name);
                }
                if (nr <= 0) break;
            }
            if (nt == 0 && s->expr && s->expr2) {
                if (s->expr->kind == E_VAR) {
                    JitSym *sym = jit_resolve(jc, s->expr->name);
                    int vslot = jit_alloc_slot(jc);
                    jit_compile_expr(jc, s->expr2, vslot);
                    if (sym->kind == 2) {
                        int sidx = jit_add_str(jc, s->expr->name, strlen(s->expr->name));
                        jit_emit_call_c(jc, (void *)jit_h_set_global, 2, sidx, vslot, 0, 0);
                    } else if (sym->kind == 1) {
                        jit_emit_call_c(jc, (void *)jit_h_up_set, 3, sym->index, vslot, sym->vtype, 0);
                    } else {
                        jit_emit_call_c(jc, (void *)jit_h_cell_set, 3, sym->index, vslot, sym->vtype, 0);
                    }
                    jit_emit_call_c(jc, (void *)jit_h_clear_slot, 1, vslot, 0, 0, 0);
                    jit_emit_check_error(jc);
                } else {
                    int obj = jit_alloc_slot(jc);
                    int key = jit_alloc_slot(jc);
                    int val = jit_alloc_slot(jc);
                    jit_compile_expr(jc, s->expr->a, obj);
                    jit_compile_expr(jc, s->expr->b, key);
                    jit_compile_expr(jc, s->expr2, val);
                    jit_emit_call_c(jc, (void *)jit_h_table_set, 3, obj, key, val, 0);
                    jit_emit_call_c(jc, (void *)jit_h_clear_range, 2, obj, 3, 0, 0);
                    jit_emit_check_error(jc);
                }
                jc->next_slot = mark;
                break;
            }
            {
                int rhs_base = nr > 0 ? jit_alloc_slots(jc, nr) : -1;
                for (i = 0; i < nr; ++i) {
                    int tmp = jit_alloc_slot(jc);
                    jit_compile_expr(jc, s->exprs[nt + i], tmp);
                    jit_emit_call_c(jc, (void *)jit_h_copy_slot, 2, rhs_base + i, tmp, 0, 0);
                    jit_emit_call_c(jc, (void *)jit_h_clear_slot, 1, tmp, 0, 0, 0);
                    jc->next_slot = rhs_base + nr;
                }
                for (i = 0; i < nt; ++i) {
                    Expr *target = s->exprs[i];
                    int ri = i < nr - 1 ? i : nr - 1;
                    int idx = i < nr - 1 ? 0 : i - (nr - 1);
                    if (target->kind == E_VAR) {
                        JitSym *sym = jit_resolve(jc, target->name);
                        if (sym->kind == 2 && !s->is_global && jc->parent != NULL && !jit_is_top_global(jc->session, target->name)) {
                            sym = jit_new_local(jc, target->name, 1, 0);
                            int64_t pack = (int64_t)(ri >= 0 ? ri : 0) | ((int64_t)1 << 24);
                            jit_emit_call_c(jc, (void *)jit_h_nth_into_cell, 4, sym->index, ri >= 0 ? rhs_base : -1, idx, pack);
                        } else if (sym->kind == 2) {
                            int sidx = jit_add_str(jc, target->name, strlen(target->name));
                            jit_emit_call_c(jc, (void *)jit_h_nth_into_global, 4, sidx, ri >= 0 ? rhs_base : -1, idx, ri >= 0 ? ri : 0);
                        } else if (sym->kind == 1) {
                            int64_t pack = (int64_t)(ri >= 0 ? ri : 0) | ((int64_t)(sym->vtype & 0xFF) << 16);
                            jit_emit_call_c(jc, (void *)jit_h_nth_into_up, 4, sym->index, ri >= 0 ? rhs_base : -1, idx, pack);
                        } else {
                            int64_t pack = (int64_t)(ri >= 0 ? ri : 0) | ((int64_t)(sym->vtype & 0xFF) << 16);
                            jit_emit_call_c(jc, (void *)jit_h_nth_into_cell, 4, sym->index, ri >= 0 ? rhs_base : -1, idx, pack);
                        }
                        jit_emit_check_error(jc);
                    } else if (target->kind == E_INDEX) {
                        int tmark = jc->next_slot;
                        int obj = jit_alloc_slot(jc);
                        int key = jit_alloc_slot(jc);
                        int64_t pack = (int64_t)(ri >= 0 ? ri : 0) | ((int64_t)idx << 16);
                        jit_compile_expr(jc, target->a, obj);
                        jit_compile_expr(jc, target->b, key);
                        jit_emit_call_c(jc, (void *)jit_h_nth_into_index, 4, obj, key, ri >= 0 ? rhs_base : -1, pack);
                        jit_emit_call_c(jc, (void *)jit_h_clear_range, 2, obj, 2, 0, 0);
                        jit_emit_check_error(jc);
                        jc->next_slot = tmark;
                    } else {
                        lc_error(jc->session->ctx, "invalid assignment target");
                        jc->failed = 1;
                    }
                }
                if (nr > 0) jit_emit_call_c(jc, (void *)jit_h_clear_range, 2, rhs_base, nr, 0, 0);
                jc->next_slot = mark;
            }
            break;
        }
        case S_EXPR: {
            int mark = jc->next_slot;
            int tmp = jit_alloc_slot(jc);
            jit_compile_expr(jc, s->expr, tmp);
            jit_emit_call_c(jc, (void *)jit_h_clear_slot, 1, tmp, 0, 0, 0);
            jc->next_slot = mark;
            break;
        }
        case S_IF: {
            size_t *end_patches = (size_t *)xmalloc(sizeof(size_t) * (size_t)(s->nblocks + 1));
            int nend = 0;
            jit_compile_if(jc, s, 0, end_patches, &nend);
            for (i = 0; i < nend; ++i) jit_patch_jump(jc, end_patches[i], jc->code_len);
            free(end_patches);
            break;
        }
        case S_WHILE: {
            int saved_breaks = jc->nbreaks;
            size_t loop_head = jc->code_len;
            int mark = jc->next_slot;
            int cond = jit_alloc_slot(jc);
            size_t jz_exit;
            jit_emit_call_c(jc, (void *)jit_h_gc_check, 1, s->line, 0, 0, 0);
            jit_add_error_patch(jc, jit_emit_jnz(jc));
            jit_compile_expr(jc, s->expr, cond);
            jit_emit_call_c(jc, (void *)jit_h_truthy_clear, 1, cond, 0, 0, 0);
            jc->next_slot = mark;
            jz_exit = jit_emit_jz(jc);
            jc->loop_depth++;
            jit_compile_stmts(jc, s->body);
            jc->loop_depth--;
            jit_patch_jump(jc, jit_emit_jmp(jc), loop_head);
            jit_patch_jump(jc, jz_exit, jc->code_len);
            while (jc->nbreaks > saved_breaks) {
                jit_patch_jump(jc, jc->break_patches[--jc->nbreaks], jc->code_len);
            }
            break;
        }
        case S_REPEAT: {
            int saved_breaks = jc->nbreaks;
            int mark_locals = jc->nlocals;
            size_t loop_head = jc->code_len;
            Stmt *it;
            int mark, cond;
            jit_emit_call_c(jc, (void *)jit_h_gc_check, 1, s->line, 0, 0, 0);
            jit_add_error_patch(jc, jit_emit_jnz(jc));
            jc->loop_depth++;
            for (it = s->body; it; it = it->next) jit_compile_stmt(jc, it);
            jc->loop_depth--;
            mark = jc->next_slot;
            cond = jit_alloc_slot(jc);
            jit_compile_expr(jc, s->expr, cond);
            jit_emit_call_c(jc, (void *)jit_h_truthy_clear, 1, cond, 0, 0, 0);
            jc->next_slot = mark;
            jit_patch_jump(jc, jit_emit_jz(jc), loop_head);
            while (jc->nbreaks > saved_breaks) {
                jit_patch_jump(jc, jc->break_patches[--jc->nbreaks], jc->code_len);
            }
            jc->nlocals = mark_locals;
            break;
        }
        case S_DO: {
            jit_compile_stmts(jc, s->body);
            break;
        }
        case S_FORNUM: {
            int saved_breaks = jc->nbreaks;
            int mark_locals = jc->nlocals;
            int mark = jc->next_slot;
            int s_slot = jit_alloc_slot(jc);
            int l_slot = jit_alloc_slot(jc);
            int st_slot = jit_alloc_slot(jc);
            JitSym *var;
            size_t loop_head, jz_exit;
            jit_compile_expr(jc, s->expr, s_slot);
            jit_compile_expr(jc, s->expr2, l_slot);
            if (s->expr3) jit_compile_expr(jc, s->expr3, st_slot);
            else {
                int k = jit_add_const(jc, lc_number(1));
                jit_emit_call_c(jc, (void *)jit_h_load_const, 2, st_slot, k, 0, 0);
            }
            jit_emit_call_c(jc, (void *)jit_h_for_prep, 3, s_slot, l_slot, st_slot, 0);
            jit_emit_check_error(jc);
            var = jit_new_local(jc, s->names[0], 1, 0);
            loop_head = jc->code_len;
            jit_emit_call_c(jc, (void *)jit_h_gc_check, 1, s->line, 0, 0, 0);
            jit_add_error_patch(jc, jit_emit_jnz(jc));
            jit_emit_call_c(jc, (void *)jit_h_for_cond, 3, s_slot, l_slot, st_slot, 0);
            jz_exit = jit_emit_jz(jc);
            jit_emit_call_c(jc, (void *)jit_h_cell_new, 3, var->index, s_slot, 0, 0);
            jc->loop_depth++;
            jit_compile_stmts(jc, s->body);
            jc->loop_depth--;
            jit_emit_call_c(jc, (void *)jit_h_for_step, 2, s_slot, st_slot, 0, 0);
            jit_patch_jump(jc, jit_emit_jmp(jc), loop_head);
            jit_patch_jump(jc, jz_exit, jc->code_len);
            while (jc->nbreaks > saved_breaks) {
                jit_patch_jump(jc, jc->break_patches[--jc->nbreaks], jc->code_len);
            }
            jit_emit_call_c(jc, (void *)jit_h_clear_range, 2, s_slot, 3, 0, 0);
            jc->next_slot = mark;
            jc->nlocals = mark_locals;
            break;
        }
        case S_FORIN: {
            int saved_breaks = jc->nbreaks;
            int mark_locals = jc->nlocals;
            int mark = jc->next_slot;
            int state_base = jit_alloc_slots(jc, 3);
            int res_slot = jit_alloc_slot(jc);
            int raw_base = s->nexprs > 0 ? jit_alloc_slots(jc, s->nexprs) : -1;
            size_t loop_head, jz_exit;
            for (i = 0; i < s->nexprs; ++i) {
                int tmp = jit_alloc_slot(jc);
                jit_compile_expr(jc, s->exprs[i], tmp);
                jit_emit_call_c(jc, (void *)jit_h_copy_slot, 2, raw_base + i, tmp, 0, 0);
                jit_emit_call_c(jc, (void *)jit_h_clear_slot, 1, tmp, 0, 0, 0);
                jc->next_slot = raw_base + s->nexprs;
            }
            jit_emit_call_c(jc, (void *)jit_h_forin_prep, 3, raw_base >= 0 ? raw_base : 0, s->nexprs, state_base, 0);
            if (raw_base >= 0) jit_emit_call_c(jc, (void *)jit_h_clear_range, 2, raw_base, s->nexprs, 0, 0);
            jc->next_slot = res_slot + 1;
            loop_head = jc->code_len;
            jit_emit_call_c(jc, (void *)jit_h_gc_check, 1, s->line, 0, 0, 0);
            jit_add_error_patch(jc, jit_emit_jnz(jc));
            jit_emit_call_c(jc, (void *)jit_h_forin_next, 2, state_base, res_slot, 0, 0);
            jz_exit = jit_emit_jz(jc);
            for (i = 0; i < s->nnames; ++i) {
                JitSym *var = jit_new_local(jc, s->names[i], 1, 0);
                jit_emit_call_c(jc, (void *)jit_h_forin_bind, 3, var->index, res_slot, i, 0);
            }
            jit_emit_call_c(jc, (void *)jit_h_clear_slot, 1, res_slot, 0, 0, 0);
            jc->loop_depth++;
            jit_compile_stmts(jc, s->body);
            jc->loop_depth--;
            jit_patch_jump(jc, jit_emit_jmp(jc), loop_head);
            jit_patch_jump(jc, jz_exit, jc->code_len);
            jit_emit_check_error(jc);
            while (jc->nbreaks > saved_breaks) {
                jit_patch_jump(jc, jc->break_patches[--jc->nbreaks], jc->code_len);
            }
            jit_emit_call_c(jc, (void *)jit_h_clear_range, 2, state_base, 4, 0, 0);
            jc->next_slot = mark;
            jc->nlocals = mark_locals;
            break;
        }
        case S_RETURN: {
            if (!s->nexprs) {
                jit_emit_call_c(jc, (void *)jit_h_ret_nil, 0, 0, 0, 0, 0);
            } else if (s->nexprs == 1) {
                int mark = jc->next_slot;
                int tmp = jit_alloc_slot(jc);
                jit_compile_expr(jc, s->exprs[0], tmp);
                jit_emit_call_c(jc, (void *)jit_h_ret_slot, 1, tmp, 0, 0, 0);
                jit_emit_call_c(jc, (void *)jit_h_clear_slot, 1, tmp, 0, 0, 0);
                jc->next_slot = mark;
            } else {
                int mark = jc->next_slot;
                int base = jit_alloc_slots(jc, s->nexprs);
                for (i = 0; i < s->nexprs; ++i) {
                    int tmp = jit_alloc_slot(jc);
                    jit_compile_expr(jc, s->exprs[i], tmp);
                    jit_emit_call_c(jc, (void *)jit_h_copy_slot, 2, base + i, tmp, 0, 0);
                    jit_emit_call_c(jc, (void *)jit_h_clear_slot, 1, tmp, 0, 0, 0);
                    jc->next_slot = base + s->nexprs;
                }
                jit_emit_call_c(jc, (void *)jit_h_ret_tuple, 2, base, s->nexprs, 0, 0);
                jit_emit_call_c(jc, (void *)jit_h_clear_range, 2, base, s->nexprs, 0, 0);
                jc->next_slot = mark;
            }
            jit_add_ret_patch(jc, jit_emit_jmp(jc));
            break;
        }
        case S_BREAK: {
            if (!jc->loop_depth) {
                lc_error(jc->session->ctx, "'break' outside a loop");
                jc->failed = 1;
            } else {
                if (jc->nbreaks >= jc->cap_breaks) {
                    jc->cap_breaks = jc->cap_breaks ? jc->cap_breaks * 2 : 16;
                    jc->break_patches = (size_t *)xrealloc(jc->break_patches, sizeof(size_t) * (size_t)jc->cap_breaks);
                }
                jc->break_patches[jc->nbreaks++] = jit_emit_jmp(jc);
            }
            break;
        }
        case S_LABEL: {
            jc->labels = (JitLabel *)xrealloc(jc->labels, sizeof(JitLabel) * (size_t)(jc->nlabels + 1));
            jc->labels[jc->nlabels].name = xstrdup(s->names[0]);
            jc->labels[jc->nlabels].offset = jc->code_len;
            jc->nlabels++;
            break;
        }
        case S_GOTO: {
            jc->gotos = (JitGotoFixup *)xrealloc(jc->gotos, sizeof(JitGotoFixup) * (size_t)(jc->ngotos + 1));
            jc->gotos[jc->ngotos].name = xstrdup(s->names[0]);
            jc->gotos[jc->ngotos].patch_off = jit_emit_jmp(jc);
            jc->ngotos++;
            break;
        }
        default:
            break;
    }
}

static void jit_compile_stmts(JitComp *jc, Stmt *s) {
    int mark = jc->nlocals;
    for (; s && !jc->failed; s = s->next) jit_compile_stmt(jc, s);
    jc->nlocals = mark;
}

static JitProto *jit_compile_fn(JitSession *session, JitComp *parent, Expr *e, const char *name) {
    JitProto *proto = (JitProto *)calloc(1, sizeof(JitProto));
    JitComp jc;
    int i;
    size_t epilogue_off;

    if (!proto) return NULL;
    memset(&jc, 0, sizeof(jc));
    jc.session = session;
    jc.parent = parent;
    jc.proto = proto;
    proto->name = xstrdup(name ? name : "chunk");
    proto->nparams = e ? e->nparams : 0;
    proto->vararg = e ? e->flag : 0;
    if (proto->vararg) {
        jc.next_slot = 1;
        jc.max_slots = 1;
    }
    if (proto->nparams > 0) {
        proto->param_vtypes = (int *)xmalloc(sizeof(int) * (size_t)proto->nparams);
        for (i = 0; i < proto->nparams; ++i) {
            int pt = (e->param_types ? e->param_types[i] : 0);
            proto->param_vtypes[i] = pt;
            jit_new_local(&jc, e->params[i], 1, pt);
        }
    }

    jit_emit_prologue(&jc);
    jit_compile_stmts(&jc, e ? e->body : NULL);

    epilogue_off = jc.code_len;
    jit_emit_epilogue(&jc);

    for (i = 0; i < jc.nerrors; ++i) jit_patch_jump(&jc, jc.error_patches[i], epilogue_off);
    for (i = 0; i < jc.nrets; ++i) jit_patch_jump(&jc, jc.ret_patches[i], epilogue_off);
    for (i = 0; i < jc.ngotos; ++i) {
        int j, found = 0;
        for (j = 0; j < jc.nlabels; ++j) {
            if (!strcmp(jc.gotos[i].name, jc.labels[j].name)) {
                jit_patch_jump(&jc, jc.gotos[i].patch_off, jc.labels[j].offset);
                found = 1;
                break;
            }
        }
        if (!found) {
            lc_error(session->ctx, "undefined label '%s'", jc.gotos[i].name);
            jc.failed = 1;
        }
    }

    if (!jc.failed) {
        proto->code_size = jc.code_len;
        proto->code = (uint8_t *)jit_alloc_exec(jc.code, jc.code_len);
        if (!proto->code) {
            lc_error(session->ctx, "cannot allocate executable JIT memory");
            jc.failed = 1;
        }
    }

    proto->ncaps = jc.ncaps;
    if (jc.ncaps > 0) {
        proto->caps = (JitCap *)xmalloc(sizeof(JitCap) * (size_t)jc.ncaps);
        for (i = 0; i < jc.ncaps; ++i) {
            proto->caps[i].is_upvalue = jc.cap_src_is_up[i];
            proto->caps[i].index = jc.cap_src_idx[i];
        }
    }

    for (i = 0; i < jc.nowned_syms; ++i) { free(jc.owned_syms[i]->name); free(jc.owned_syms[i]); }
    free(jc.owned_syms);
    free(jc.locals);
    free(jc.caps);
    free(jc.cap_src_is_up);
    free(jc.cap_src_idx);
    for (i = 0; i < jc.n_explicit_globals; ++i) free(jc.explicit_globals[i]);
    free(jc.explicit_globals);
    free(jc.break_patches);
    free(jc.error_patches);
    free(jc.ret_patches);
    for (i = 0; i < jc.nlabels; ++i) free(jc.labels[i].name);
    free(jc.labels);
    for (i = 0; i < jc.ngotos; ++i) free(jc.gotos[i].name);
    free(jc.gotos);
    free(jc.code);

    if (jc.failed) {
        proto->next = session->protos;
        session->protos = proto;
        return NULL;
    }

    proto->next = session->protos;
    session->protos = proto;
    return proto;
}

/* ---------------- Project module loading & public API ---------------- */

static char *jit_read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    char *source;
    long size;
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
    source = (char *)xmalloc((size_t)size + 1);
    if (fread(source, 1, (size_t)size, file) != (size_t)size) { fclose(file); free(source); return NULL; }
    fclose(file);
    source[size] = 0;
    if (length) *length = (size_t)size;
    return source;
}

static int jit_safe_search_path(const char *path) {
    const char *segment = path, *p;
    if (!*path || *path == '/' || strchr(path, '\\') || strchr(path, ':')) return 0;
    for (p = path;; ++p) {
        if (*p == '/' || !*p) {
            if (p - segment == 2 && segment[0] == '.' && segment[1] == '.') return 0;
            if (!*p) break;
            segment = p + 1;
        }
    }
    return 1;
}

static char *jit_module_path(JitSession *session, const char *name, int main_file) {
    Str path, module_name;
    LcValue package, key, patterns;
    const char *start, *end;
    size_t i;
    if (main_file) {
        sb_init(&path); sb_add(&path, session->root_path); sb_add(&path, "/main.lua");
        return path.data;
    }
    if (!*name || name[0] == '.' || strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) return NULL;
    sb_init(&module_name);
    for (i = 0; name[i]; ++i) { char c = name[i] == '.' ? '/' : name[i]; sb_n(&module_name, &c, 1); }
    package = lc_table_get(session->ctx, session->ctx->globals, lc_string(session->ctx, "package"));
    key = lc_string(session->ctx, "path");
    patterns = lc_table_get(session->ctx, package, key);
    if (patterns.type != LC_STRING) {
        sb_free(&module_name);
        lc_error(session->ctx, "package.path must be a string");
        return NULL;
    }
    start = patterns.as.string->data;
    while (*start) {
        Str candidate;
        end = strchr(start, ';');
        if (!end) end = start + strlen(start);
        sb_init(&candidate);
        for (; start < end; ++start) {
            if (*start == '?') sb_add(&candidate, module_name.data);
            else sb_n(&candidate, start, 1);
        }
        if (candidate.data && jit_safe_search_path(candidate.data)) {
            sb_init(&path); sb_add(&path, session->root_path); sb_add(&path, "/"); sb_add(&path, candidate.data);
            sb_free(&candidate);
            if (access(path.data, R_OK) == 0) { sb_free(&module_name); return path.data; }
            sb_free(&path);
        } else sb_free(&candidate);
        if (!*end) break;
        start = end + 1;
    }
    sb_free(&module_name);
    return NULL;
}

static JitModule *jit_compile_module(JitSession *session, const char *name, int main_file) {
    JitModule *module;
    char *path = jit_module_path(session, name, main_file), *source;
    Expr chunk;
    if (!path) {
        if (!session->ctx->error) lc_error(session->ctx, "module '%s' not found in project package.path", name);
        return NULL;
    }
    source = jit_read_file(path, NULL);
    if (!source) { lc_error(session->ctx, "module '%s' not found at %s", main_file ? "main" : name, path); free(path); return NULL; }
    module = (JitModule *)calloc(1, sizeof(*module));
    if (!module) { free(source); free(path); lc_error(session->ctx, "out of memory"); return NULL; }
    module->name = path;
    module->source = source;
    api_parse(&module->parser, source, path, &module->program);
    module->next = session->modules;
    session->modules = module;
    if (module->parser.failed) { lc_error(session->ctx, "%s", module->parser.error); return module; }
    jit_collect_top_globals(session, module->program);
    memset(&chunk, 0, sizeof(chunk));
    chunk.kind = E_FUNCTION;
    chunk.flag = 0;
    chunk.body = module->program;
    module->entry = jit_compile_fn(session, NULL, &chunk, path);
    return module;
}

static LcValue jit_execute_module(JitSession *session, JitModule *module) {
    LcValue fn;
    if (!module || !module->entry || session->ctx->error) return lc_nil();
    fn = lc_function_new(session->ctx, jit_trampoline, 0, NULL, module->name);
    if (fn.type == LC_FUNCTION) fn.as.function->jit_proto = module->entry;
    return lc_call(session->ctx, fn, 0, NULL);
}

static LcValue jit_load_text(JitSession *session, const char *source, size_t length, const char *name) {
    LcContext *ctx = session->ctx;
    JitModule *module;
    Expr chunk;
    LcValue fn;
    if (memchr(source, '\0', length)) {
        LcValue values[2] = {lc_nil(), lc_string(ctx, "binary chunks and embedded NUL bytes are not supported")};
        return lc_tuple_new(ctx, 2, values, 1);
    }
    module = (JitModule *)calloc(1, sizeof(*module));
    if (!module) { lc_error(ctx, "out of memory"); return lc_nil(); }
    module->source = xstrndup(source, length);
    module->name = xstrdup(name);
    api_parse(&module->parser, module->source, module->name, &module->program);
    if (module->parser.failed) {
        LcValue values[2] = {lc_nil(), lc_string(ctx, module->parser.error)};
        token_clear(&module->parser.token); free_stmt(module->program);
        free(module->source); free(module->name); free(module);
        return lc_tuple_new(ctx, 2, values, 1);
    }
    memset(&chunk, 0, sizeof(chunk));
    chunk.kind = E_FUNCTION;
    chunk.flag = 1;
    chunk.body = module->program;
    module->entry = jit_compile_fn(session, NULL, &chunk, name);
    module->next = session->modules;
    session->modules = module;
    if (!module->entry) {
        LcValue values[2] = {lc_nil(), lc_string(ctx, ctx->message)};
        ctx->error = 0;
        return lc_tuple_new(ctx, 2, values, 1);
    }
    fn = lc_function_new(ctx, jit_trampoline, 0, NULL, name);
    if (fn.type == LC_FUNCTION) fn.as.function->jit_proto = module->entry;
    return fn;
}

static LcValue jit_load_error(LcContext *ctx, const char *message) {
    LcValue values[2] = {lc_nil(), lc_string(ctx, message)};
    return lc_tuple_new(ctx, 2, values, 1);
}

static LcValue jit_builtin_load(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    JitSession *session = jit_current;
    LcValue source, arg;
    const char *name = "=(load)";
    int mode_index = 2;
    (void)self;
    if (!argc || (source = lc_first_value(argv[0])).type != LC_STRING) {
        Str text;
        if (!argc || lc_first_value(argv[0]).type != LC_FUNCTION) return jit_load_error(ctx, "chunk source must be a string or reader function");
        sb_init(&text);
        for (;;) {
            LcValue chunk = lc_first(lc_call(ctx, argv[0], 0, NULL));
            if (ctx->error) { sb_free(&text); return lc_nil(); }
            if (chunk.type == LC_NIL) break;
            if (chunk.type != LC_STRING) { sb_free(&text); return jit_load_error(ctx, "reader function must return a string"); }
            sb_n(&text, chunk.as.string->data, chunk.as.string->length);
        }
        if (argc > 1 && (arg = lc_first_value(argv[1])).type == LC_STRING) name = arg.as.string->data;
        if (argc > 2 && (arg = lc_first_value(argv[2])).type == LC_STRING && strchr(arg.as.string->data, 't') == NULL) {
            sb_free(&text); return jit_load_error(ctx, "binary chunks are not supported");
        }
        { LcValue loaded = jit_load_text(session, text.data ? text.data : "", text.len, name); sb_free(&text); return loaded; }
    }
    if (strcmp(self && self->name ? self->name : "", "loadstring") == 0) mode_index = 99;
    if (argc > 1 && (arg = lc_first_value(argv[1])).type == LC_STRING) name = arg.as.string->data;
    if (argc > mode_index && (arg = lc_first_value(argv[mode_index])).type == LC_STRING && strchr(arg.as.string->data, 't') == NULL)
        return jit_load_error(ctx, "binary chunks are not supported");
    if (argc > mode_index + 1 && lc_first_value(argv[mode_index + 1]).type != LC_NIL) {
        lc_error(ctx, "custom load environment is not supported");
        return lc_nil();
    }
    return jit_load_text(session, source.as.string->data, source.as.string->length, name);
}

static LcValue jit_builtin_loadfile(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    JitSession *session = jit_current;
    LcValue path, mode;
    char *source;
    size_t length = 0;
    (void)self;
    if (!argc || (path = lc_first_value(argv[0])).type != LC_STRING) return jit_load_error(ctx, "file name expected");
    if (argc > 1 && (mode = lc_first_value(argv[1])).type == LC_STRING && strchr(mode.as.string->data, 't') == NULL)
        return jit_load_error(ctx, "binary chunks are not supported");
    source = jit_read_file(path.as.string->data, &length);
    if (!source) return jit_load_error(ctx, "cannot open chunk file");
    { LcValue loaded = jit_load_text(session, source, length, path.as.string->data); free(source); return loaded; }
}

static LcValue jit_builtin_dofile(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    LcValue loaded = jit_builtin_loadfile(ctx, self, argc, argv);
    LcValue function = lc_first(loaded);
    if (function.type != LC_FUNCTION) {
        if (!ctx->error) {
            LcValue message = lc_nth(loaded, 1);
            lc_error(ctx, "%s", message.type == LC_STRING ? message.as.string->data : "cannot load file");
        }
        return lc_nil();
    }
    return lc_call(ctx, function, 0, NULL);
}

static LcValue jit_require(LcContext *ctx, LcFunction *self, int argc, LcValue *argv) {
    JitSession *session = jit_current;
    const char *name;
    LcValue loaded, key, result, preload, loader;
    JitModule *module;
    if (!argc || lc_first_value(argv[0]).type != LC_STRING) { lc_error(ctx, "module name expected"); return lc_nil(); }
    if (lc_native_module_name(lc_first_value(argv[0]).as.string->data))
        return lc_builtin_require(ctx, self, argc, argv);
    name = lc_first_value(argv[0]).as.string->data;
    loaded = lc_package_table(ctx, "loaded");
    key = lc_string(ctx, name);
    result = lc_table_get(ctx, loaded, key);
    if (lc_truthy(result)) return result;
    preload = lc_package_table(ctx, "preload");
    loader = lc_table_get(ctx, preload, key);
    if (loader.type == LC_FUNCTION) return lc_builtin_require(ctx, self, argc, argv);
    lc_table_set(ctx, loaded, key, lc_bool(1));
    module = jit_compile_module(session, name, 0);
    result = module ? jit_execute_module(session, module) : lc_nil();
    if (ctx->error) lc_table_set(ctx, loaded, key, lc_nil());
    else if (result.type != LC_NIL) lc_table_set(ctx, loaded, key, result);
    result = lc_table_get(ctx, loaded, key);
    if (!ctx->error && result.type == LC_NIL) {
        result = lc_bool(1);
        lc_table_set(ctx, loaded, key, result);
    }
    return result;
}

static void jit_json_string(Str *json, const char *text) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    sb_add(json, "\"");
    for (; *p; ++p) {
        char escaped[7];
        if (*p == '\"' || *p == '\\') { char pair[2] = {'\\', (char)*p}; sb_n(json, pair, 2); }
        else if (*p == '\n') sb_add(json, "\\n");
        else if (*p == '\r') sb_add(json, "\\r");
        else if (*p == '\t') sb_add(json, "\\t");
        else if (*p < 32) { snprintf(escaped, sizeof(escaped), "\\u%04x", *p); sb_add(json, escaped); }
        else sb_n(json, (const char *)p, 1);
    }
    sb_add(json, "\"");
}

static LcValue jit_field(LcContext *ctx, LcValue table, const char *name) {
    return lc_table_raw_get(ctx, table, lc_string(ctx, name));
}

static double jit_number_field(LcContext *ctx, LcValue table, const char *name, double fallback) {
    LcValue value = lc_first_value(jit_field(ctx, table, name));
    if (value.type == LC_NUMBER) return value.as.number;
    if (value.type == LC_INTEGER) return (double)value.as.integer;
    return fallback;
}

static void jit_json_scene(JitSession *session, Str *json) {
    LcContext *ctx = session->ctx;
    LcValue world = lc_table_get(ctx, ctx->globals, lc_string(ctx, "world")), entities, camera, overlay;
    int i, first = 1;
    sb_add(json, ",\"entities\":[");
    if (world.type == LC_TABLE) {
        entities = jit_field(ctx, world, "entities");
        if (entities.type == LC_TABLE) for (i = 0; i < (int)entities.as.table->capacity; ++i) {
            LcValue entity, active, texture, color;
            if (!entities.as.table->entries[i].used) continue;
            entity = entities.as.table->entries[i].value;
            if (entity.type != LC_TABLE) continue;
            active = jit_field(ctx, entity, "active");
            if (!lc_truthy(active)) continue;
            if (!first) sb_add(json, ",");
            first = 0;
            sb_add(json, "{");
            sb_add(json, "\"x\":"); sb_printf(json, "%.8g", jit_number_field(ctx, entity, "x", 0));
            sb_add(json, ",\"y\":"); sb_printf(json, "%.8g", jit_number_field(ctx, entity, "y", 0));
            sb_add(json, ",\"width\":"); sb_printf(json, "%.8g", jit_number_field(ctx, entity, "width", 32));
            sb_add(json, ",\"height\":"); sb_printf(json, "%.8g", jit_number_field(ctx, entity, "height", 32));
            sb_add(json, ",\"rotation\":"); sb_printf(json, "%.8g", jit_number_field(ctx, entity, "rotation", 0));
            sb_add(json, ",\"scale\":"); sb_printf(json, "%.8g", jit_number_field(ctx, entity, "scale", 1));
            texture = jit_field(ctx, entity, "texture");
            sb_add(json, ",\"texture\":");
            if (texture.type == LC_TABLE) {
                LcValue path = jit_field(ctx, texture, "path");
                if (path.type == LC_STRING) jit_json_string(json, path.as.string->data);
                else sb_add(json, "null");
            } else sb_add(json, "null");
            color = jit_field(ctx, entity, "color");
            sb_add(json, ",\"color\":[");
            if (color.type == LC_TABLE) {
                int c;
                for (c = 1; c <= 4; ++c) {
                    LcValue channel = lc_first_value(lc_table_raw_get(ctx, color, lc_integer(c)));
                    double n = channel.type == LC_NUMBER ? channel.as.number : (channel.type == LC_INTEGER ? (double)channel.as.integer : 1.0);
                    if (c > 1) sb_add(json, ",");
                    sb_printf(json, "%.4g", n);
                }
            } else sb_add(json, "1,1,1,1");
            sb_add(json, "]}");
        }
    }
    sb_add(json, "],\"camera\":");
    camera = world.type == LC_TABLE ? jit_field(ctx, world, "camera") : lc_nil();
    if (camera.type == LC_TABLE) {
        sb_add(json, "{\"x\":"); sb_printf(json, "%.8g", jit_number_field(ctx, camera, "x", 0));
        sb_add(json, ",\"y\":"); sb_printf(json, "%.8g", jit_number_field(ctx, camera, "y", 0));
        sb_add(json, ",\"zoom\":"); sb_printf(json, "%.8g", jit_number_field(ctx, camera, "zoom", 1));
        sb_add(json, "}");
    } else sb_add(json, "{\"x\":0,\"y\":0,\"zoom\":1}");
    sb_add(json, ",\"texts\":[");
    overlay = world.type == LC_TABLE ? jit_field(ctx, world, "overlay_text") : lc_nil();
    if (overlay.type == LC_TABLE) {
        int index;
        first = 1;
        for (index = 1; index <= lc_table_length(overlay); ++index) {
            LcValue item = lc_table_raw_get(ctx, overlay, lc_integer(index)), text;
            if (item.type != LC_TABLE) continue;
            text = jit_field(ctx, item, "text");
            if (!first) sb_add(json, ",");
            first = 0;
            sb_add(json, "{\"text\":");
            jit_json_string(json, text.type == LC_STRING ? text.as.string->data : "");
            sb_add(json, ",\"x\":"); sb_printf(json, "%.8g", jit_number_field(ctx, item, "x", 0));
            sb_add(json, ",\"y\":"); sb_printf(json, "%.8g", jit_number_field(ctx, item, "y", 0));
            sb_add(json, ",\"size\":"); sb_printf(json, "%.8g", jit_number_field(ctx, item, "size", 16));
            sb_add(json, "}");
        }
    }
    sb_add(json, "]");
    sb_add(json, ",\"clear_color\":");
    if (ctx->has_bg) sb_printf(json, "[%.4g,%.4g,%.4g,1]", ctx->bg_r, ctx->bg_g, ctx->bg_b);
    else sb_add(json, "null");
    sb_add(json, ",\"draw_cmds\":[");
    for (i = 0; i < ctx->draw_count; ++i) {
        LcDrawCmd *cmd = &ctx->draw_cmds[i];
        const char *sprite_path = (cmd->kind == 0 && cmd->sprite_id >= 1 && cmd->sprite_id <= ctx->sprite_count)
            ? ctx->sprites[cmd->sprite_id - 1].path : NULL;
        if (i > 0) sb_add(json, ",");
        sb_add(json, "{\"kind\":"); sb_printf(json, "%d", cmd->kind);
        sb_add(json, ",\"x\":"); sb_printf(json, "%.8g", cmd->x);
        sb_add(json, ",\"y\":"); sb_printf(json, "%.8g", cmd->y);
        sb_add(json, ",\"w\":"); sb_printf(json, "%.8g", cmd->w);
        sb_add(json, ",\"h\":"); sb_printf(json, "%.8g", cmd->h);
        sb_add(json, ",\"rot\":"); sb_printf(json, "%.8g", cmd->rot);
        sb_add(json, ",\"extra\":"); sb_printf(json, "%.8g", cmd->extra);
        sb_add(json, ",\"align\":"); sb_printf(json, "%d", cmd->align);
        sb_add(json, ",\"color\":[");
        sb_printf(json, "%.4g,%.4g,%.4g,%.4g", cmd->r, cmd->g, cmd->b, cmd->a);
        sb_add(json, "],\"path\":");
        if (sprite_path) jit_json_string(json, sprite_path); else sb_add(json, "null");
        sb_add(json, ",\"font\":");
        if (cmd->font) jit_json_string(json, cmd->font); else sb_add(json, "null");
        sb_add(json, ",\"text\":");
        if (cmd->text) jit_json_string(json, cmd->text); else sb_add(json, "null");
        sb_add(json, "}");
    }
    sb_add(json, "]");
}

static char *jit_project_frame(JitSession *session, double dt) {
    LcContext *ctx = session->ctx;
    Str json;
    session->budget = 500000;
    lc_clear_draw_cmds(ctx);
    if (!ctx->error && dt > 0) {
        LcValue update = lc_table_get(ctx, ctx->globals, lc_string(ctx, "update"));
        if (update.type == LC_FUNCTION) {
            LcValue argument = lc_number(dt);
            lc_call(ctx, update, 1, &argument);
        }
        if (!ctx->error) {
            LcValue world = lc_table_get(ctx, ctx->globals, lc_string(ctx, "world"));
            LcValue engine = lc_table_get(ctx, ctx->globals, lc_string(ctx, "engine"));
            if (world.type == LC_TABLE && engine.type == LC_TABLE) {
                LcValue method = jit_field(ctx, engine, "update");
                if (method.type == LC_FUNCTION) {
                    LcValue args[2];
                    args[0] = world;
                    args[1] = lc_number(dt);
                    lc_call(ctx, method, 2, args);
                }
            }
        }
        if (!ctx->error) {
            LcValue draw = lc_table_get(ctx, ctx->globals, lc_string(ctx, "draw"));
            if (draw.type == LC_FUNCTION) lc_call(ctx, draw, 0, NULL);
        }
    }
    sb_init(&json);
    sb_add(&json, "{\"ok\":"); sb_add(&json, ctx->error ? "false" : "true");
    sb_add(&json, ",\"output\":");
    if (ctx->captured_output && session->output_offset < ctx->captured_length)
        jit_json_string(&json, ctx->captured_output + session->output_offset);
    else jit_json_string(&json, "");
    session->output_offset = ctx->captured_length;
    sb_add(&json, ",\"error\":");
    if (ctx->error) {
        Str message;
        sb_init(&message);
        sb_printf(&message, "%s:%d: %s", ctx->source ? ctx->source : "project", ctx->line, ctx->message);
        jit_json_string(&json, message.data);
        sb_free(&message);
    } else sb_add(&json, "null");
    jit_json_scene(session, &json);
    sb_add(&json, "}");
    return json.data;
}

LUAC_API void *luac_project_open(const char *root_path, char **error_message) {
    JitSession *session;
    JitModule *main_module;
    char cwd[PATH_MAX];
    if (error_message) *error_message = NULL;
    if (!root_path || !*root_path) { if (error_message) *error_message = xstrdup("project path required"); return NULL; }
    session = (JitSession *)calloc(1, sizeof(*session));
    if (!session) { if (error_message) *error_message = xstrdup("out of memory"); return NULL; }
    session->previous_cwd = getcwd(cwd, sizeof(cwd)) ? xstrdup(cwd) : NULL;
    if (root_path[0] == '/' || root_path[0] == '\\' || (root_path[0] && root_path[1] == ':') || !session->previous_cwd)
        session->root_path = xstrdup(root_path);
    else {
        Str absolute;
        sb_init(&absolute); sb_add(&absolute, session->previous_cwd); sb_add(&absolute, "/"); sb_add(&absolute, root_path);
        session->root_path = absolute.data;
    }
    if (chdir(root_path) != 0) {
        if (error_message) {
            Str message;
            sb_init(&message); sb_printf(&message, "cannot open project: %s", strerror(errno));
            *error_message = message.data;
        }
        free(session->previous_cwd); free(session->root_path); free(session);
        return NULL;
    }
    session->ctx = lc_context_new(session->root_path);
    session->ctx->capture_output = 1;
    session->budget = 2000000;
    jit_current = session;
    lc_set_global(session->ctx, "require", lc_function_new(session->ctx, jit_require, 0, NULL, "project require"));
    lc_set_global(session->ctx, "load", lc_function_new(session->ctx, jit_builtin_load, 0, NULL, "load"));
    lc_set_global(session->ctx, "loadstring", lc_function_new(session->ctx, jit_builtin_load, 0, NULL, "loadstring"));
    lc_set_global(session->ctx, "loadfile", lc_function_new(session->ctx, jit_builtin_loadfile, 0, NULL, "loadfile"));
    lc_set_global(session->ctx, "dofile", lc_function_new(session->ctx, jit_builtin_dofile, 0, NULL, "dofile"));
    {
        LcValue package = lc_table_get(session->ctx, session->ctx->globals, lc_string(session->ctx, "package"));
        lc_set_field(session->ctx, package, "path", lc_string(session->ctx, "./?.lua;./?/init.lua"));
    }
    main_module = jit_compile_module(session, "main", 1);
    if (main_module && !session->ctx->error) jit_execute_module(session, main_module);
    return session;
}

LUAC_API char *luac_project_step(void *handle, double dt) {
    JitSession *session = (JitSession *)handle;
    if (!session) return xstrdup("{\"ok\":false,\"error\":\"project is not running\",\"output\":\"\",\"entities\":[],\"texts\":[]}");
    jit_current = session;
    return jit_project_frame(session, dt);
}

LUAC_API void luac_project_set_viewport(void *handle, int width, int height) {
    JitSession *session = (JitSession *)handle;
    if (!session || !session->ctx) return;
    if (width > 0) session->ctx->screen_w = width;
    if (height > 0) session->ctx->screen_h = height;
}

LUAC_API void luac_project_touch(void *handle, int id, double x, double y, int down) {
    JitSession *session = (JitSession *)handle;
    LcValue world, engine, method, on_touch, args[5];
    if (!session || session->ctx->error) return;
    jit_current = session;
    on_touch = lc_table_get(session->ctx, session->ctx->globals, lc_string(session->ctx, "on_touch"));
    if (on_touch.type == LC_FUNCTION) {
        args[0] = lc_integer(id);
        args[1] = lc_number(x);
        args[2] = lc_number(y);
        args[3] = lc_bool(down);
        lc_call(session->ctx, on_touch, 4, args);
    }
    world = lc_table_get(session->ctx, session->ctx->globals, lc_string(session->ctx, "world"));
    engine = lc_table_get(session->ctx, session->ctx->globals, lc_string(session->ctx, "engine"));
    if (!session->ctx->error && world.type == LC_TABLE && engine.type == LC_TABLE) {
        method = jit_field(session->ctx, engine, "inject_touch");
        if (method.type == LC_FUNCTION) {
            args[0] = world;
            args[1] = lc_integer(id);
            args[2] = lc_number(x);
            args[3] = lc_number(y);
            args[4] = lc_bool(down);
            lc_call(session->ctx, method, 5, args);
        }
    }
}

LUAC_API void luac_project_close(void *handle) {
    JitSession *session = (JitSession *)handle;
    JitProto *proto, *next_proto;
    JitModule *module, *next_module;
    int i;
    if (!session) return;
    jit_current = session;
    lc_context_destroy(session->ctx);
    for (proto = session->protos; proto; proto = next_proto) {
        next_proto = proto->next;
        jit_free_exec(proto->code, proto->code_size);
        free(proto->param_vtypes);
        free(proto->caps);
        free(proto->consts);
        free(proto->ptrs);
        for (i = 0; i < proto->nstr_lits; ++i) {
            free(proto->str_lits[i]->data);
            free(proto->str_lits[i]);
        }
        free(proto->str_lits);
        for (i = 0; i < proto->nstruct_specs; ++i) {
            int j;
            JitStructSpec *sp = proto->struct_specs[i];
            free(sp->name);
            free(sp->base_name);
            for (j = 0; j < sp->nfields; ++j) free(sp->field_names[j]);
            free(sp->field_names);
            free(sp->field_vtypes);
            for (j = 0; j < sp->nmethods; ++j) free(sp->method_names[j]);
            free(sp->method_names);
            free(sp);
        }
        free(proto->struct_specs);
        free(proto->name);
        free(proto);
    }
    for (module = session->modules; module; module = next_module) {
        next_module = module->next;
        token_clear(&module->parser.token);
        free_stmt(module->program);
        free(module->source);
        free(module->name);
        free(module);
    }
    for (i = 0; i < session->ntop_globals; ++i) free(session->top_globals[i]);
    free(session->top_globals);
    if (session->previous_cwd) chdir(session->previous_cwd);
    free(session->previous_cwd);
    free(session->root_path);
    free(session);
    jit_current = NULL;
}

#endif

