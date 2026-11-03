/*
 * %CopyrightBegin%
 * SPDX-License-Identifier: Apache-2.0
 * Copyright Ericsson AB 2008-2026. All Rights Reserved.
 * Copyright 2026 Erlangsters.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * %CopyrightEnd%
 *
 * Public-native adaptation of OTP's erl_bif_re.c. PCRE's restartable matcher
 * remains backend-owned; all Erlang terms, continuations and native lifetimes
 * use the installed public NIF SDK.
 */
#include <erl_nif.h>
#include <beam/native.h>
#include <beam/zlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#ifndef ERLANG_INTEGRATION
#define ERLANG_INTEGRATION 1
#endif
#ifndef PCRE2_STATIC
#define PCRE2_STATIC
#endif
#ifndef PCRE2_CODE_UNIT_WIDTH
#define PCRE2_CODE_UNIT_WIDTH 8
#endif
#include "../vendor/pcre2/pcre2.h"
/* These are package-owned backend types, used only to bound its otherwise
 * length-less deserializer before it reads an externally supplied payload. */
#include "../vendor/pcre2/pcre2_internal.h"

#define RE_LOOP_LIMIT 64000u
#define RE_STACK_RESERVE (10u * 1024u)
#define F_COMPILE 1u
#define F_EXEC 2u
#define F_UNICODE 4u
#define F_OFFSET 8u
#define F_CAPTURE 16u
#define F_GLOBAL 32u
#define F_REPORT 64u
#define F_MATCH_LIMIT 128u
#define F_DEPTH_LIMIT 256u
#define F_EXPORT 512u

typedef struct ReLibrary ReLibrary;
typedef struct {
    uint32_t compile, newline, bsr, match, flags;
    int offset;
    ERL_NIF_TERM capture, type;
    uint32_t match_limit, depth_limit;
} Options;
typedef struct {
    ReLibrary *lib;
    pcre2_code *code;
} Pattern;
typedef enum { CAP_INDEX, CAP_LIST, CAP_BINARY, CAP_NONE } CaptureType;
typedef struct {
    ReLibrary *lib;
    CaptureType type;
    int count;                  /* 0=all, -1=all_but_first, positive=vector */
    size_t capacity;
    int *indices;               /* -N followed by N duplicate-name alternatives */
} Capture;
typedef struct {
    ReLibrary *lib;
    Pattern *pattern;
    ErlNifEnv *owned_env;
    const unsigned char *subject;
    size_t length;
    int original_binary, started, cleaned;
    unsigned flags;
    uint32_t match_flags;
    int offset;
    pcre2_match_data *data;
    pcre2_match_context *context;
    void *restart;
    Capture capture;
} Match;
struct ReLibrary {
    pcre2_general_context *general;
    ErlNifResourceType *patterns, *matches;
    _Atomic size_t bytes, peak_bytes, allocations, patterns_live, matches_live;
    _Atomic unsigned loop_limit;
    _Atomic uint64_t yields;
#ifdef BEAM_RE_TESTING
    ErlNifMutex *coverage_lock;
#endif
};
#ifdef BEAM_RE_TESTING
/* Independent outstanding counts survive native unload, so a leaked backend
 * allocation cannot disappear from the acceptance oracle with its context. */
static _Atomic size_t test_allocations, test_resources, test_bytes, test_peak_bytes;
static _Atomic size_t test_total_allocations, test_total_allocated_bytes;
static _Atomic size_t test_compile_entries, test_compile_running;
static _Atomic unsigned test_compile_hold_ms;
static _Atomic unsigned test_fail_load_stage;
#endif

typedef struct {
    ErlNifEnv *env;
#ifdef BEAM_RE_TESTING
    bool guard_entered;
#endif
} CompileOperation;

typedef union {
    max_align_t alignment;
    struct { size_t size; ReLibrary *lib; } info;
} Allocation;

#define ATOMS(X) \
 X(ok) X(error) X(compile) X(match) X(nomatch) X(badarg) X(badopt) X(none) \
 X(true) X(false) X(re_pattern) X(re_exported_pattern) X(beam_re_error) \
 X(beam_re_dispatch) X(ucompile) X(urun) X(grun) X(index) X(list) X(binary) \
 X(all) X(all_but_first) X(first) X(all_names) X(capture) X(offset) \
 X(match_limit) X(match_limit_recursion) X(newline) X(cr) X(crlf) X(lf) \
 X(nul) X(anycrlf) X(any) X(anchored) X(notempty) X(notempty_atstart) \
 X(notbol) X(noteol) X(no_start_optimize) X(caseless) X(dollar_endonly) \
 X(dotall) X(extended) X(firstline) X(multiline) X(no_auto_capture) X(dupnames) \
 X(ungreedy) X(ucp) X(never_utf) X(report_errors) X(unicode) X(global) \
 X(bsr_anycrlf) X(bsr_unicode) X(export) X(namelist) X(cancelled) X(system_limit)
#define DECL_ATOM(n) static ERL_NIF_TERM a_##n;
ATOMS(DECL_ATOM)
#undef DECL_ATOM

static void *backend_alloc(size_t size, void *opaque)
{
    ReLibrary *lib = opaque;
    Allocation *a;
    size_t current, peak;
    if (size > SIZE_MAX - sizeof(*a)) return NULL;
    a = enif_alloc(sizeof(*a) + size);
    if (!a) return NULL;
    a->info.size = size;
    a->info.lib = lib;
    current = atomic_fetch_add(&lib->bytes, size) + size;
    atomic_fetch_add(&lib->allocations, 1);
    peak = atomic_load(&lib->peak_bytes);
    while (current > peak && !atomic_compare_exchange_weak(&lib->peak_bytes, &peak, current)) {}
#ifdef BEAM_RE_TESTING
    atomic_fetch_add(&test_allocations, 1);
    atomic_fetch_add(&test_total_allocations, 1);
    atomic_fetch_add(&test_total_allocated_bytes, size);
    current = atomic_fetch_add(&test_bytes, size) + size;
    peak = atomic_load(&test_peak_bytes);
    while (current > peak && !atomic_compare_exchange_weak(&test_peak_bytes, &peak, current)) {}
#endif
    return a + 1;
}
static void backend_free(void *pointer, void *opaque)
{
    Allocation *a;
    (void)opaque;
    if (!pointer) return;
    a = (Allocation *)pointer - 1;
    atomic_fetch_sub(&a->info.lib->bytes, a->info.size);
    atomic_fetch_sub(&a->info.lib->allocations, 1);
#ifdef BEAM_RE_TESTING
    atomic_fetch_sub(&test_allocations, 1);
    atomic_fetch_sub(&test_bytes, a->info.size);
#endif
    enif_free(a);
}
static void *backend_realloc(void *pointer, size_t size, ReLibrary *lib)
{
    Allocation *a;
    size_t previous, current, peak;
    if (!pointer) return backend_alloc(size, lib);
    if (size > SIZE_MAX - sizeof(*a)) return NULL;
    a = (Allocation *)pointer - 1;
    previous = a->info.size;
    a = enif_realloc(a, sizeof(*a) + size);
    if (!a) return NULL;
    a->info.size = size;
    if (size >= previous) current = atomic_fetch_add(&lib->bytes, size - previous) + size - previous;
    else current = atomic_fetch_sub(&lib->bytes, previous - size) - (previous - size);
    peak = atomic_load(&lib->peak_bytes);
    while (current > peak && !atomic_compare_exchange_weak(&lib->peak_bytes, &peak, current)) {}
#ifdef BEAM_RE_TESTING
    atomic_fetch_add(&test_total_allocations, 1);
    atomic_fetch_add(&test_total_allocated_bytes, size);
    if (size >= previous) current = atomic_fetch_add(&test_bytes, size - previous) + size - previous;
    else current = atomic_fetch_sub(&test_bytes, previous - size) - (previous - size);
    peak = atomic_load(&test_peak_bytes);
    while (current > peak && !atomic_compare_exchange_weak(&test_peak_bytes, &peak, current)) {}
#endif
    return a + 1;
}
static int cancelled(ErlNifEnv *env)
{
    return beam_native_stop_requested() || !enif_is_current_process_alive(env);
}
static int compile_guard(uint32_t depth, void *opaque)
{
    CompileOperation *operation = opaque;
    ErlNifEnv *env = operation->env;
    (void)depth;
#ifdef BEAM_RE_TESTING
    if (!operation->guard_entered) {
        unsigned hold = atomic_exchange(&test_compile_hold_ms, 0);
        operation->guard_entered = true;
        atomic_fetch_add(&test_compile_entries, 1);
        if (hold) {
            /* One bounded dirty-scheduler test barrier. Cancellation remains
             * live while another process confirms actual native entry. */
            ErlNifTime until = enif_monotonic_time(ERL_NIF_MSEC) + hold;
            while (!cancelled(env) && enif_monotonic_time(ERL_NIF_MSEC) < until)
                atomic_signal_fence(memory_order_seq_cst);
        }
    }
#endif
    return cancelled(env) || beam_native_stack_has_space(RE_STACK_RESERVE) != 1;
}
static ERL_NIF_TERM badarg(ErlNifEnv *env, ERL_NIF_TERM cause)
{
    return enif_make_tuple3(env, a_beam_re_error, a_badarg, cause);
}
static ERL_NIF_TERM exhausted(ErlNifEnv *env)
{
    return enif_raise_exception(env, a_system_limit);
}
static ERL_NIF_TERM dispatch(ErlNifEnv *env, ERL_NIF_TERM action,
                             unsigned count, const ERL_NIF_TERM *arguments)
{
    return enif_make_tuple3(env, a_beam_re_dispatch, action,
                           enif_make_list_from_array(env, arguments, count));
}
static ERL_NIF_TERM bytes(ErlNifEnv *env, const void *source, size_t size)
{
    ERL_NIF_TERM result;
    unsigned char *target = enif_make_new_binary(env, size, &result);
    if (size && !target) return exhausted(env);
    if (size) memcpy(target, source, size);
    return result;
}

static bool parse_options(ErlNifEnv *env, ERL_NIF_TERM list, Options *o)
{
    ERL_NIF_TERM item, tail;
    const ERL_NIF_TERM *tuple;
    int arity, value;
    memset(o, 0, sizeof(*o));
    o->capture = a_all;
    o->type = a_index;
    while (enif_get_list_cell(env, list, &item, &tail)) {
        list = tail;
        if (enif_get_tuple(env, item, &arity, &tuple)) {
            if ((arity == 2 || arity == 3) && tuple[0] == a_capture) {
                o->capture = tuple[1];
                o->type = arity == 3 ? tuple[2] : a_index;
                o->flags |= F_CAPTURE | F_EXEC;
            } else if (arity == 2 && (tuple[0] == a_offset || tuple[0] == a_match_limit ||
                                      tuple[0] == a_match_limit_recursion)) {
                if (!enif_get_int(env, tuple[1], &value) || value < 0) return false;
                if (tuple[0] == a_offset) { o->offset = value; o->flags |= F_OFFSET; }
                else if (tuple[0] == a_match_limit) { o->match_limit = value; o->flags |= F_MATCH_LIMIT; }
                else { o->depth_limit = value; o->flags |= F_DEPTH_LIMIT; }
                o->flags |= F_EXEC;
            } else if (arity == 2 && tuple[0] == a_newline) {
                if (tuple[1] == a_cr) o->newline = PCRE2_NEWLINE_CR;
                else if (tuple[1] == a_crlf) o->newline = PCRE2_NEWLINE_CRLF;
                else if (tuple[1] == a_lf) o->newline = PCRE2_NEWLINE_LF;
                else if (tuple[1] == a_nul) o->newline = PCRE2_NEWLINE_NUL;
                else if (tuple[1] == a_anycrlf) o->newline = PCRE2_NEWLINE_ANYCRLF;
                else if (tuple[1] == a_any) o->newline = PCRE2_NEWLINE_ANY;
                else return false;
            } else return false;
        } else if (item == a_anchored) { o->compile |= PCRE2_ANCHORED; o->match |= PCRE2_ANCHORED; }
        else if (item == a_notempty) { o->match |= PCRE2_NOTEMPTY; o->flags |= F_EXEC; }
        else if (item == a_notempty_atstart) { o->match |= PCRE2_NOTEMPTY_ATSTART; o->flags |= F_EXEC; }
        else if (item == a_notbol) { o->match |= PCRE2_NOTBOL; o->flags |= F_EXEC; }
        else if (item == a_noteol) { o->match |= PCRE2_NOTEOL; o->flags |= F_EXEC; }
#define COMPILE_OPTION(name, flag) else if (item == a_##name) { o->compile |= flag; o->flags |= F_COMPILE; }
        COMPILE_OPTION(no_start_optimize, PCRE2_NO_START_OPTIMIZE)
        COMPILE_OPTION(caseless, PCRE2_CASELESS)
        COMPILE_OPTION(dollar_endonly, PCRE2_DOLLAR_ENDONLY)
        COMPILE_OPTION(dotall, PCRE2_DOTALL)
        COMPILE_OPTION(extended, PCRE2_EXTENDED)
        COMPILE_OPTION(firstline, PCRE2_FIRSTLINE)
        COMPILE_OPTION(multiline, PCRE2_MULTILINE)
        COMPILE_OPTION(no_auto_capture, PCRE2_NO_AUTO_CAPTURE)
        COMPILE_OPTION(dupnames, PCRE2_DUPNAMES)
        COMPILE_OPTION(ungreedy, PCRE2_UNGREEDY)
        COMPILE_OPTION(ucp, PCRE2_UCP)
        COMPILE_OPTION(never_utf, PCRE2_NEVER_UTF)
#undef COMPILE_OPTION
        else if (item == a_report_errors) o->flags |= F_EXEC | F_REPORT;
        else if (item == a_unicode) { o->compile |= PCRE2_UTF; o->flags |= F_COMPILE | F_UNICODE; }
        else if (item == a_global) o->flags |= F_EXEC | F_GLOBAL;
        else if (item == a_bsr_anycrlf) o->bsr = PCRE2_BSR_ANYCRLF;
        else if (item == a_bsr_unicode) o->bsr = PCRE2_BSR_UNICODE;
        else if (item == a_export) o->flags |= F_COMPILE | F_EXPORT;
        else return false;
    }
    return enif_is_empty_list(env, list);
}

static void pattern_destructor(ErlNifEnv *env, void *object)
{
    Pattern *pattern = object;
    (void)env;
    if (pattern->code) {
        pcre2_code_free(pattern->code);
        pattern->code = NULL;
        atomic_fetch_sub(&pattern->lib->patterns_live, 1);
#ifdef BEAM_RE_TESTING
        atomic_fetch_sub(&test_resources, 1);
#endif
    }
}
static Pattern *get_pattern(ErlNifEnv *env, ERL_NIF_TERM term,
                            int *capture_count, int *unicode, int *crlf)
{
    ReLibrary *lib = enif_priv_data(env);
    const ERL_NIF_TERM *tuple;
    Pattern *pattern;
    int arity;
    if (!enif_get_tuple(env, term, &arity, &tuple) || arity != 5 || tuple[0] != a_re_pattern ||
        !enif_get_int(env, tuple[1], capture_count) || *capture_count < 0 ||
        !enif_get_int(env, tuple[2], unicode) || !enif_get_int(env, tuple[3], crlf) ||
        !enif_get_resource(env, tuple[4], lib->patterns, (void **)&pattern) || !pattern->code)
        return NULL;
    return pattern;
}
static ERL_NIF_TERM compiled(ErlNifEnv *env, ReLibrary *lib, pcre2_code *code,
                             unsigned unicode)
{
    Pattern *pattern;
    uint32_t count, newline;
    ERL_NIF_TERM reference;
    pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &count);
    pcre2_pattern_info(code, PCRE2_INFO_NEWLINE, &newline);
    pattern = enif_alloc_resource(lib->patterns, sizeof(*pattern));
    if (!pattern) { pcre2_code_free(code); return exhausted(env); }
    pattern->lib = lib;
    pattern->code = code;
    atomic_fetch_add(&lib->patterns_live, 1);
#ifdef BEAM_RE_TESTING
    atomic_fetch_add(&test_resources, 1);
#endif
    reference = enif_make_resource(env, pattern);
    enif_release_resource(pattern);
    return enif_make_tuple5(env, a_re_pattern, enif_make_uint(env, count),
                           enif_make_uint(env, unicode),
                           enif_make_int(env, newline == PCRE2_NEWLINE_ANY ||
                            newline == PCRE2_NEWLINE_CRLF || newline == PCRE2_NEWLINE_ANYCRLF), reference);
}
static pcre2_code *compile_code(ErlNifEnv *env, ReLibrary *lib,
                                const ErlNifBinary *expression, const Options *o,
                                int *error, PCRE2_SIZE *offset)
{
    pcre2_compile_context *context;
    pcre2_code *code;
    CompileOperation operation = { .env = env };
    context = pcre2_compile_context_create(lib->general);
    if (!context) { *error = PCRE2_ERROR_NOMEMORY; return NULL; }
    pcre2_set_compile_recursion_guard(context, compile_guard, &operation);
    if (o->newline) pcre2_set_newline(context, o->newline);
    if (o->bsr) pcre2_set_bsr(context, o->bsr);
    static const unsigned char empty_expression[1] = {0};
#ifdef BEAM_RE_TESTING
    atomic_fetch_add(&test_compile_running, 1);
#endif
    code = pcre2_compile(expression->data ? expression->data : empty_expression,
                         expression->size, o->compile,
                         error, offset, context);
#ifdef BEAM_RE_TESTING
    atomic_fetch_sub(&test_compile_running, 1);
#endif
    pcre2_compile_context_free(context);
    if (cancelled(env)) { pcre2_code_free(code); return NULL; }
    return code;
}
static ERL_NIF_TERM compile_error(ErlNifEnv *env, int error, PCRE2_SIZE offset,
                                  bool run)
{
    unsigned char message[120];
    ERL_NIF_TERM detail;
    int rc = pcre2_get_error_message(error, message, sizeof(message));
    if (rc < 0) snprintf((char *)message, sizeof(message), "Unknown error (%d)", error);
    detail = enif_make_tuple2(env, enif_make_string(env, (const char *)message, ERL_NIF_LATIN1),
                             enif_make_uint64(env, offset));
    if (run) detail = enif_make_tuple2(env, a_compile, detail);
    return enif_make_tuple2(env, a_error, detail);
}
static void put32(unsigned char *p, uint32_t n)
{
    p[0] = n >> 24; p[1] = n >> 16; p[2] = n >> 8; p[3] = n;
}
static uint32_t get32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static ERL_NIF_TERM export_code(ErlNifEnv *env, ReLibrary *lib, pcre2_code *code,
                                unsigned unicode, ERL_NIF_TERM original,
                                ERL_NIF_TERM options)
{
    uint8_t *encoded;
    PCRE2_SIZE size;
    unsigned char header[14];
    ErlNifBinary expression;
    ERL_NIF_TERM result, encoded_term;
    int rc = pcre2_serialize_encode((const pcre2_code **)&code, 1, &encoded, &size, lib->general);
    if (rc != 1) return exhausted(env);
    if (cancelled(env)) { pcre2_serialize_free(encoded); return enif_raise_exception(env, a_cancelled); }
    memcpy(header, "re-PCRE2", 8);
    put32(header + 8, (uint32_t)beam_zlib_crc32_z(0, encoded, size));
    header[12] = 1; header[13] = unicode;
    encoded_term = bytes(env, encoded, size);
    pcre2_serialize_free(encoded);
    if (!enif_inspect_iolist_as_binary(env, original, &expression)) return badarg(env, a_none);
    result = enif_make_tuple5(env, a_re_exported_pattern, bytes(env, header, sizeof(header)),
                              bytes(env, expression.data, expression.size), options, encoded_term);
    return enif_make_tuple2(env, a_ok, result);
}
static ERL_NIF_TERM compile_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    ReLibrary *lib = enif_priv_data(env);
    Options options;
    ErlNifBinary expression;
    pcre2_code *code;
    PCRE2_SIZE offset = 0;
    int error = 0;
    ERL_NIF_TERM result;
    (void)argc;
    if (cancelled(env)) return enif_raise_exception(env, a_cancelled);
    if (!parse_options(env, argv[1], &options) || options.flags & F_EXEC) return badarg(env, a_badopt);
    if ((options.flags & F_UNICODE) && !enif_is_binary(env, argv[0]))
        return dispatch(env, a_ucompile, 2, argv);
    if (!enif_inspect_iolist_as_binary(env, argv[0], &expression)) return badarg(env, a_none);
    code = compile_code(env, lib, &expression, &options, &error, &offset);
    if (cancelled(env)) { pcre2_code_free(code); return enif_raise_exception(env, a_cancelled); }
    if (!code) return compile_error(env, error, offset, false);
    if (options.flags & F_EXPORT) {
        result = export_code(env, lib, code, !!(options.flags & F_UNICODE), argv[0], argv[1]);
        pcre2_code_free(code);
        return result;
    }
    result = compiled(env, lib, code, !!(options.flags & F_UNICODE));
    if (enif_is_exception(env, result)) return result;
    return enif_make_tuple2(env, a_ok, result);
}

/* Bound the length-less decoder and the name metadata used by package helpers.
 * This remains PCRE2's trusted bytecode cache, not a bytecode verifier. A version
 * or architecture mismatch still takes the pinned source-recompile path. */
static int decode_code(ReLibrary *lib, const ErlNifBinary *encoded, pcre2_code **code)
{
    pcre2_serialized_data header;
    pcre2_real_code block;
    size_t prefix = sizeof(header) + TABLES_LENGTH;
    unsigned char *aligned;
    int rc;
    if (encoded->size < sizeof(header)) return PCRE2_ERROR_BADSERIALIZEDDATA;
    memcpy(&header, encoded->data, sizeof(header));
    if (header.magic != 0x50523253u) return PCRE2_ERROR_BADMAGIC;
    if (header.version != ((PCRE2_MAJOR) | ((PCRE2_MINOR) << 16)) ||
        header.config != (sizeof(PCRE2_UCHAR) | (sizeof(void *) << 8) | (sizeof(PCRE2_SIZE) << 16)))
        return PCRE2_ERROR_BADMODE;
    if (header.number_of_codes <= 0 || encoded->size < prefix ||
        encoded->size - prefix < sizeof(block)) return PCRE2_ERROR_BADSERIALIZEDDATA;
    memcpy(&block, encoded->data + prefix, sizeof(block));
    if (block.blocksize <= sizeof(block) || block.blocksize > encoded->size - prefix ||
        block.magic_number != MAGIC_NUMBER || block.name_count > MAX_NAME_COUNT ||
        block.name_entry_size > MAX_NAME_SIZE + IMM2_SIZE + 1 ||
        (block.flags & PCRE2_MODE_MASK) != PCRE2_MODE8 ||
        block.code_start < sizeof(block) || block.code_start >= block.blocksize)
        return PCRE2_ERROR_BADSERIALIZEDDATA;
    if (block.name_count) {
        size_t available = block.code_start - sizeof(block);
        if (block.name_entry_size < IMM2_SIZE + 1 ||
            block.name_count > available / block.name_entry_size)
            return PCRE2_ERROR_BADSERIALIZEDDATA;
        for (size_t i = 0; i < block.name_count; ++i) {
            const unsigned char *entry = encoded->data + prefix + sizeof(block) +
                                          i * block.name_entry_size;
            unsigned group = ((unsigned)entry[0] << 8) | entry[1];
            if (!group || group > block.top_bracket ||
                !memchr(entry + IMM2_SIZE, 0, block.name_entry_size - IMM2_SIZE))
                return PCRE2_ERROR_BADSERIALIZEDDATA;
        }
    }
    aligned = backend_alloc(encoded->size, lib);
    if (!aligned) return PCRE2_ERROR_NOMEMORY;
    memcpy(aligned, encoded->data, encoded->size);
    rc = pcre2_serialize_decode(code, 1, aligned, lib->general);
    backend_free(aligned, NULL);
    return rc;
}
static ERL_NIF_TERM import_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    ReLibrary *lib = enif_priv_data(env);
    const ERL_NIF_TERM *tuple;
    int arity, rc, error = 0;
    ErlNifBinary header, encoded, expression;
    Options options;
    pcre2_code *code = NULL;
    PCRE2_SIZE offset = 0;
    unsigned unicode = 0;
    (void)argc;
    if (cancelled(env)) return enif_raise_exception(env, a_cancelled);
    if (!enif_get_tuple(env, argv[0], &arity, &tuple) || arity != 5 ||
        tuple[0] != a_re_exported_pattern || !enif_inspect_binary(env, tuple[1], &header) ||
        header.size < 14 || memcmp(header.data, "re-PCRE2", 8)) return badarg(env, a_none);
    if (header.data[12] == 1) {
        if (!enif_inspect_binary(env, tuple[4], &encoded) ||
            get32(header.data + 8) != (uint32_t)beam_zlib_crc32_z(0, encoded.data, encoded.size))
            return badarg(env, a_none);
        unicode = header.data[13];
        rc = decode_code(lib, &encoded, &code);
    } else rc = PCRE2_ERROR_BADMODE;
    if (cancelled(env)) { pcre2_code_free(code); return enif_raise_exception(env, a_cancelled); }
    if (rc == 1) return compiled(env, lib, code, unicode);
    if (rc != PCRE2_ERROR_BADMODE && rc != PCRE2_ERROR_BADMAGIC) return badarg(env, a_none);
    if (!enif_inspect_binary(env, tuple[2], &expression) ||
        !parse_options(env, tuple[3], &options) || (options.flags & F_EXEC)) return badarg(env, a_none);
    code = compile_code(env, lib, &expression, &options, &error, &offset);
    if (cancelled(env)) { pcre2_code_free(code); return enif_raise_exception(env, a_cancelled); }
    if (!code) return badarg(env, a_none);
    return compiled(env, lib, code, !!(options.flags & F_UNICODE));
}

static bool capture_push(Capture *capture, int value)
{
    int *indices;
    size_t capacity;
    if (capture->count == INT_MAX) return false;
    if ((size_t)capture->count == capture->capacity) {
        if (capture->capacity > SIZE_MAX / 2) return false;
        capacity = capture->capacity ? capture->capacity * 2 : 8;
        if (capacity > SIZE_MAX / sizeof(int)) return false;
        indices = backend_realloc(capture->indices, capacity * sizeof(int), capture->lib);
        if (!indices) return false;
        capture->indices = indices;
        capture->capacity = capacity;
    }
    capture->indices[capture->count++] = value;
    return true;
}
static bool capture_name(Capture *capture, pcre2_code *code, const char *name, bool duplicates)
{
    int index;
    if (duplicates) {
        PCRE2_SPTR first, last;
        int size = pcre2_substring_nametable_scan(code, (PCRE2_SPTR)name, &first, &last);
        if (size == PCRE2_ERROR_NOSUBSTRING) return capture_push(capture, -1);
        if (size < 0) return false;
        if (first != last) {
            int count = (int)((last - first) / size) + 1;
            if (!capture_push(capture, -count)) return false;
            while (count--) {
                if (!capture_push(capture, ((unsigned)first[0] << 8) | first[1])) return false;
                first += size;
            }
            return true;
        }
        index = ((unsigned)first[0] << 8) | first[1];
    } else {
        index = pcre2_substring_number_from_name(code, (PCRE2_SPTR)name);
        if (index == PCRE2_ERROR_NOSUBSTRING) index = -1;
        else if (index < 0) return false;
    }
    return capture_push(capture, index);
}
static bool build_capture(ErlNifEnv *env, const Options *options, pcre2_code *code, Capture *capture)
{
    ERL_NIF_TERM value = options->capture, head, tail;
    uint32_t flags;
    bool duplicates;
    memset(capture, 0, sizeof(*capture));
    capture->lib = enif_priv_data(env);
    if (options->type == a_index) capture->type = CAP_INDEX;
    else if (options->type == a_list) capture->type = CAP_LIST;
    else if (options->type == a_binary) capture->type = CAP_BINARY;
    else return false;
    if (value == a_all) return true;
    if (value == a_none || enif_is_empty_list(env, value)) { capture->type = CAP_NONE; return true; }
    if (value == a_all_but_first) { capture->count = -1; return true; }
    if (value == a_first) return capture_push(capture, 0);
    if (pcre2_pattern_info(code, PCRE2_INFO_ALLOPTIONS, &flags)) return false;
    duplicates = !!(flags & PCRE2_DUPNAMES);
    if (value == a_all_names) {
        uint32_t count, size;
        PCRE2_SPTR names, previous = NULL;
        if (pcre2_pattern_info(code, PCRE2_INFO_NAMECOUNT, &count) ||
            pcre2_pattern_info(code, PCRE2_INFO_NAMEENTRYSIZE, &size) ||
            pcre2_pattern_info(code, PCRE2_INFO_NAMETABLE, &names)) return false;
        if (!count) { capture->type = CAP_NONE; return true; }
        while (count--) {
            if (!previous || !duplicates || strcmp((const char *)previous + 2, (const char *)names + 2)) {
                if (!capture_name(capture, code, (const char *)names + 2, duplicates)) return false;
            }
            previous = names;
            names += size;
        }
        return true;
    }
    while (enif_get_list_cell(env, value, &head, &tail)) {
        int index;
        value = tail;
        if (enif_get_int(env, head, &index)) {
            if (index < 0 || !capture_push(capture, index)) return false;
        } else {
            char *name;
            unsigned length;
            ErlNifBinary binary;
            if (enif_is_atom(env, head)) {
                if (!enif_get_atom_length(env, head, &length, ERL_NIF_UTF8) || length == UINT_MAX) return false;
                name = backend_alloc((size_t)length + 1, capture->lib);
                if (!name) return false;
                if (!enif_get_atom(env, head, name, length + 1, ERL_NIF_UTF8)) { backend_free(name, NULL); return false; }
            } else {
                if (!enif_inspect_iolist_as_binary(env, head, &binary) || binary.size == SIZE_MAX) return false;
                name = backend_alloc(binary.size + 1, capture->lib);
                if (!name) return false;
                if (binary.size) memcpy(name, binary.data, binary.size);
                name[binary.size] = 0;
            }
            bool ok = capture_name(capture, code, name, duplicates);
            backend_free(name, NULL);
            if (!ok) return false;
        }
    }
    return enif_is_empty_list(env, value);
}
static void match_cleanup(Match *match)
{
    if (match->cleaned) return;
    match->cleaned = 1;
    if (match->restart && match->data) pcre2_free_restart_data(match->data);
    match->restart = NULL;
    pcre2_match_data_free(match->data); match->data = NULL;
    pcre2_match_context_free(match->context); match->context = NULL;
    backend_free(match->capture.indices, NULL); match->capture.indices = NULL;
    if (match->pattern) { enif_release_resource(match->pattern); match->pattern = NULL; }
    if (match->owned_env) { enif_free_env(match->owned_env); match->owned_env = NULL; }
    match->subject = NULL;
}
static void match_destructor(ErlNifEnv *env, void *object)
{
    Match *match = object;
    (void)env;
    match_cleanup(match);
    atomic_fetch_sub(&match->lib->matches_live, 1);
#ifdef BEAM_RE_TESTING
    atomic_fetch_sub(&test_resources, 1);
#endif
}
static ERL_NIF_TERM captured(ErlNifEnv *env, Match *match, ERL_NIF_TERM subject,
                             int index, int result_count)
{
    PCRE2_SIZE *vector = pcre2_get_ovector_pointer(match->data);
    PCRE2_SIZE start = PCRE2_UNSET, length = 0;
    if (index >= 0 && index < result_count) {
        start = vector[index * 2];
        if (start != PCRE2_UNSET) length = vector[index * 2 + 1] - start;
    }
    if (match->capture.type == CAP_INDEX)
        return enif_make_tuple2(env, start == PCRE2_UNSET ? enif_make_int(env, -1) : enif_make_uint64(env, start),
                               enif_make_uint64(env, length));
    if (match->capture.type == CAP_BINARY) {
        if (match->original_binary && start != PCRE2_UNSET)
            return enif_make_sub_binary(env, subject, start, length);
        return bytes(env, start == PCRE2_UNSET ? (const unsigned char *)"" : match->subject + start, length);
    }
    ERL_NIF_TERM result = enif_make_list(env, 0);
    if (start == PCRE2_UNSET) return result;
    for (size_t i = length; i > 0; --i)
        result = enif_make_list_cell(env, enif_make_uint(env, match->subject[start + i - 1]), result);
    return result;
}
static ERL_NIF_TERM match_result(ErlNifEnv *env, Match *match, ERL_NIF_TERM subject, int rc)
{
    ERL_NIF_TERM list = enif_make_list(env, 0), result;
    int count = match->capture.count;
    PCRE2_SIZE *vector = pcre2_get_ovector_pointer(match->data);
    if (rc <= 0) {
        if (match->flags & F_REPORT) {
            if (rc == PCRE2_ERROR_MATCHLIMIT) return enif_make_tuple2(env, a_error, a_match_limit);
            if (rc == PCRE2_ERROR_DEPTHLIMIT) return enif_make_tuple2(env, a_error, a_match_limit_recursion);
        }
        return a_nomatch;
    }
    if (match->capture.type == CAP_NONE) return a_match;
    if (count <= 0) {
        for (int i = rc - 1; i >= -count; --i)
            list = enif_make_list_cell(env, captured(env, match, subject, i, rc), list);
    } else {
        ERL_NIF_TERM *values = backend_alloc((size_t)count * sizeof(*values), match->lib);
        unsigned n = 0;
        if (!values) return exhausted(env);
        for (int i = 0; i < count; ++i) {
            int index = match->capture.indices[i];
            if (index < -1) {
                int end = i - index;
                index = match->capture.indices[i + 1];
                for (int j = i + 1; j <= end; ++j) {
                    int alternative = match->capture.indices[j];
                    if (alternative >= 0 && alternative < rc && vector[alternative * 2] != PCRE2_UNSET) {
                        index = alternative;
                        break;
                    }
                }
                i = end;
            }
            values[n++] = captured(env, match, subject, index, rc);
        }
        list = enif_make_list_from_array(env, values, n);
        backend_free(values, NULL);
    }
    result = enif_make_tuple2(env, a_match, list);
    return result;
}
static ERL_NIF_TERM match_continue(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    ReLibrary *lib = enif_priv_data(env);
    Match *match;
    unsigned limit = atomic_load(&lib->loop_limit);
    int rc, remaining;
    ERL_NIF_TERM result;
    (void)argc;
    if (!enif_get_resource(env, argv[1], lib->matches, (void **)&match) || match->cleaned)
        return badarg(env, a_none);
    if (cancelled(env)) { match_cleanup(match); return enif_raise_exception(env, a_cancelled); }
    pcre2_set_loops_left(match->data, (int)limit);
    pcre2_set_restart_flags(match->data, 0);
#ifdef BEAM_RE_TESTING
    enif_mutex_lock(lib->coverage_lock);
#endif
    if (match->started) rc = pcre2_match(NULL, NULL, 0, 0, 0, match->data, NULL);
    else {
        match->started = 1;
        rc = pcre2_match(match->pattern->code, match->subject, match->length,
                         match->offset, match->match_flags, match->data, match->context);
    }
#ifdef BEAM_RE_TESTING
    enif_mutex_unlock(lib->coverage_lock);
#endif
    remaining = pcre2_get_loops_left(match->data);
    unsigned consumed = remaining >= (int)limit ? 0 : limit - (remaining > 0 ? (unsigned)remaining : 0);
    unsigned percent = (unsigned)(((uint64_t)consumed * 100 + RE_LOOP_LIMIT - 1) / RE_LOOP_LIMIT);
    if (percent) enif_consume_timeslice(env, percent > 100 ? 100 : percent);
    if (cancelled(env)) { match_cleanup(match); return enif_raise_exception(env, a_cancelled); }
    if (rc == PCRE2_ERROR_LOOP_LIMIT) {
        atomic_fetch_add(&lib->yields, 1);
        return enif_schedule_nif(env, "re_match_continue", 0, match_continue, 2, argv);
    }
    if (rc == PCRE2_ERROR_BADUTFOFFSET || rc == PCRE2_ERROR_BADOFFSET ||
        rc == PCRE2_ERROR_BADMAGIC || rc == PCRE2_ERROR_BADMODE ||
        (rc <= PCRE2_ERROR_UTF8_ERR1 && rc >= PCRE2_ERROR_UTF8_ERR21))
        result = badarg(env, a_none);
    else if (rc >= 0 || rc == PCRE2_ERROR_NOMATCH || rc == PCRE2_ERROR_MATCHLIMIT ||
             rc == PCRE2_ERROR_DEPTHLIMIT || rc == PCRE2_ERROR_HEAPLIMIT || rc == PCRE2_ERROR_RECURSELOOP)
        result = match_result(env, match, argv[0], rc);
    else result = enif_raise_exception(env, enif_make_atom(env, "internal_error"));
    match_cleanup(match);
    return result;
}

/* Both direct compiled calls and dirty textual compilation enter matching on a
 * regular scheduler. The original arguments remain available for the Erlang
 * Unicode/global helpers and their public error presentation. */
static ERL_NIF_TERM run_compiled(ErlNifEnv *env, const ERL_NIF_TERM argv[],
                                ERL_NIF_TERM pattern_term, const Options *parsed,
                                bool textual)
{
    ReLibrary *lib = enif_priv_data(env);
    const Options options = *parsed;
    Pattern *pattern;
    Match *match;
    int capture_count, unicode, crlf;
    bool first, list_capture;
    ERL_NIF_TERM dispatch_args[3], resource, next[2];
    ErlNifBinary subject;
    if (cancelled(env)) return enif_raise_exception(env, a_cancelled);
    if (argv[3] == a_true) first = true;
    else if (argv[3] == a_false) first = false;
    else return badarg(env, a_none);
    list_capture = (options.flags & F_CAPTURE) && options.type == a_list;
    pattern = get_pattern(env, pattern_term, &capture_count, &unicode, &crlf);
    if (!pattern) return badarg(env, a_none);
    if (unicode && (!enif_is_binary(env, argv[0]) || (list_capture && !(options.flags & F_GLOBAL))))
        return dispatch(env, a_urun, 3, argv);
    if (options.flags & F_GLOBAL) {
        dispatch_args[0] = argv[0];
        dispatch_args[1] = pattern_term;
        dispatch_args[2] = textual ? enif_make_tuple3(env, argv[2],
            options.flags & F_COMPILE ? a_true : a_false, argv[1]) :
            enif_make_tuple2(env, argv[2], a_false);
        return dispatch(env, a_grun, 3, dispatch_args);
    }
    if (!textual) {
        uint32_t compiled_value;
        if (options.newline && (pcre2_pattern_info(pattern->code, PCRE2_INFO_NEWLINE, &compiled_value) ||
                               compiled_value != options.newline)) return badarg(env, a_none);
        if (options.bsr && (pcre2_pattern_info(pattern->code, PCRE2_INFO_BSR, &compiled_value) ||
                           compiled_value != options.bsr)) return badarg(env, a_none);
    }
    match = enif_alloc_resource(lib->matches, sizeof(*match));
    if (!match) return exhausted(env);
    memset(match, 0, sizeof(*match));
    match->lib = lib;
    atomic_fetch_add(&lib->matches_live, 1);
#ifdef BEAM_RE_TESTING
    atomic_fetch_add(&test_resources, 1);
#endif
    match->pattern = pattern;
    enif_keep_resource(pattern);
    match->original_binary = enif_is_binary(env, argv[0]);
    match->flags = options.flags;
    match->match_flags = options.match | (first ? 0 : PCRE2_NO_UTF_CHECK);
    match->offset = options.offset;
    if (!build_capture(env, &options, pattern->code, &match->capture)) {
        enif_release_resource(match);
        return badarg(env, a_none);
    }
    match->owned_env = enif_alloc_env();
    if (!match->owned_env) { enif_release_resource(match); return exhausted(env); }
    ERL_NIF_TERM owned_subject = enif_make_copy(match->owned_env, argv[0]);
    if (!enif_inspect_iolist_as_binary(match->owned_env, owned_subject, &subject)) {
        enif_release_resource(match);
        return badarg(env, a_none);
    }
    static const unsigned char empty_subject[1] = {0};
    match->subject = subject.data ? subject.data : empty_subject;
    match->length = subject.size;
    match->data = pcre2_match_data_create_from_pattern(pattern->code, lib->general);
    if (!match->data) { enif_release_resource(match); return exhausted(env); }
    pcre2_set_restart_data(match->data, &match->restart);
    if (options.flags & (F_MATCH_LIMIT | F_DEPTH_LIMIT)) {
        match->context = pcre2_match_context_create(lib->general);
        if (!match->context) { enif_release_resource(match); return exhausted(env); }
        if (options.flags & F_MATCH_LIMIT) pcre2_set_match_limit(match->context, options.match_limit);
        if (options.flags & F_DEPTH_LIMIT) pcre2_set_depth_limit(match->context, options.depth_limit);
    }
    if (cancelled(env)) { enif_release_resource(match); return enif_raise_exception(env, a_cancelled); }
    resource = enif_make_resource(env, match);
    enif_release_resource(match);
    next[0] = argv[0]; next[1] = resource;
    return match_continue(env, 2, next);
}

static ERL_NIF_TERM run_after_compile(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    Options options;
    (void)argc;
    if (cancelled(env)) return enif_raise_exception(env, a_cancelled);
    if (!parse_options(env, argv[2], &options) || options.flags & F_EXPORT)
        return badarg(env, a_badopt);
    return run_compiled(env, argv, argv[4], &options, true);
}

static ERL_NIF_TERM run_compile(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    ReLibrary *lib = enif_priv_data(env);
    Options options;
    ErlNifBinary expression;
    pcre2_code *code;
    PCRE2_SIZE error_offset = 0;
    int error = 0;
    ERL_NIF_TERM next[5];
    (void)argc;
    if (cancelled(env)) return enif_raise_exception(env, a_cancelled);
    if (!parse_options(env, argv[2], &options) || options.flags & F_EXPORT)
        return badarg(env, a_badopt);
    if (!enif_inspect_iolist_as_binary(env, argv[1], &expression)) return badarg(env, a_none);
    code = compile_code(env, lib, &expression, &options, &error, &error_offset);
    if (cancelled(env)) { pcre2_code_free(code); return enif_raise_exception(env, a_cancelled); }
    if (!code) return options.flags & F_REPORT ? compile_error(env, error, error_offset, true) : badarg(env, a_none);
    next[0] = argv[0]; next[1] = argv[1]; next[2] = argv[2]; next[3] = argv[3];
    next[4] = compiled(env, lib, code, !!(options.flags & F_UNICODE));
    if (enif_is_exception(env, next[4])) return next[4];
    if (cancelled(env)) return enif_raise_exception(env, a_cancelled);
    return enif_schedule_nif(env, "re_run_after_compile", 0, run_after_compile, 5, next);
}

static ERL_NIF_TERM run_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    Options options;
    const ERL_NIF_TERM *tuple;
    int arity;
    bool list_capture;
    (void)argc;
    if (cancelled(env)) return enif_raise_exception(env, a_cancelled);
    if (!parse_options(env, argv[2], &options) || options.flags & F_EXPORT)
        return badarg(env, a_badopt);
    if (argv[3] != a_true && argv[3] != a_false) return badarg(env, a_none);
    if (enif_get_tuple(env, argv[1], &arity, &tuple) && arity == 5) {
        if (options.flags & F_COMPILE) return badarg(env, a_none);
        return run_compiled(env, argv, argv[1], &options, false);
    }
    if (!enif_is_binary(env, argv[1]) && !enif_is_list(env, argv[1])) return badarg(env, a_none);
    list_capture = (options.flags & F_CAPTURE) && options.type == a_list;
    if ((options.flags & F_UNICODE) &&
        (!enif_is_binary(env, argv[0]) || !enif_is_binary(env, argv[1]) ||
         (list_capture && !(options.flags & F_GLOBAL))))
        return dispatch(env, a_urun, 3, argv);
    return enif_schedule_nif(env, "re_run_compile", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                             run_compile, 4, argv);
}
static ERL_NIF_TERM inspect_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    Pattern *pattern;
    int captures, unicode, crlf;
    uint32_t count, size, flags;
    PCRE2_SPTR names, previous = NULL;
    ERL_NIF_TERM *values, result;
    unsigned n = 0;
    (void)argc;
    pattern = get_pattern(env, argv[0], &captures, &unicode, &crlf);
    if (!pattern || argv[1] != a_namelist ||
        pcre2_pattern_info(pattern->code, PCRE2_INFO_ALLOPTIONS, &flags) ||
        pcre2_pattern_info(pattern->code, PCRE2_INFO_NAMECOUNT, &count) ||
        pcre2_pattern_info(pattern->code, PCRE2_INFO_NAMEENTRYSIZE, &size) ||
        pcre2_pattern_info(pattern->code, PCRE2_INFO_NAMETABLE, &names)) return badarg(env, a_none);
    if (!count) return enif_make_tuple2(env, a_namelist, enif_make_list(env, 0));
    values = backend_alloc((size_t)count * sizeof(*values), pattern->lib);
    if (!values) return exhausted(env);
    while (count--) {
        if (!previous || !(flags & PCRE2_DUPNAMES) || strcmp((const char *)previous + 2, (const char *)names + 2))
            values[n++] = bytes(env, names + 2, strlen((const char *)names + 2));
        previous = names;
        names += size;
    }
    result = enif_make_tuple2(env, a_namelist, enif_make_list_from_array(env, values, n));
    backend_free(values, NULL);
    return result;
}
static ERL_NIF_TERM version_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    unsigned char version[32];
    int size = pcre2_config(PCRE2_CONFIG_VERSION, version);
    (void)argc; (void)argv;
    if (size <= 0 || size > (int)sizeof(version)) return exhausted(env);
    return bytes(env, version, (size_t)size - 1);
}

#ifdef BEAM_RE_TESTING
static _Atomic(ReLibrary *) test_library;
#if defined(_WIN32)
#define RE_TEST_EXPORT __declspec(dllexport)
#else
#define RE_TEST_EXPORT __attribute__((visibility("default")))
#endif
RE_TEST_EXPORT size_t beam_re_test_allocations(void)
{
    return atomic_load(&test_allocations);
}
RE_TEST_EXPORT size_t beam_re_test_resources(void)
{
    return atomic_load(&test_resources);
}
RE_TEST_EXPORT size_t beam_re_test_bytes(void)
{
    return atomic_load(&test_bytes);
}
RE_TEST_EXPORT size_t beam_re_test_peak_bytes(void)
{
    return atomic_load(&test_peak_bytes);
}
RE_TEST_EXPORT size_t beam_re_test_total_allocations(void)
{
    return atomic_load(&test_total_allocations);
}
RE_TEST_EXPORT size_t beam_re_test_total_allocated_bytes(void)
{
    return atomic_load(&test_total_allocated_bytes);
}
RE_TEST_EXPORT size_t beam_re_test_compile_entered(void)
{
    return atomic_load(&test_compile_entries);
}
RE_TEST_EXPORT size_t beam_re_test_compile_active(void)
{
    return atomic_load(&test_compile_running);
}
RE_TEST_EXPORT unsigned beam_re_test_compile_hold(unsigned milliseconds)
{
    if (milliseconds > 1000) milliseconds = 1000;
    return atomic_exchange(&test_compile_hold_ms, milliseconds);
}
RE_TEST_EXPORT unsigned beam_re_test_fail_load(unsigned stage)
{
    if (stage > 2) stage = 0;
    return atomic_exchange(&test_fail_load_stage, stage);
}
RE_TEST_EXPORT int beam_re_test_set_limit(int limit)
{
    ReLibrary *lib = atomic_load(&test_library);
    if (!lib) return -1;
    unsigned previous = atomic_exchange(&lib->loop_limit, limit > 0 ? (unsigned)limit : RE_LOOP_LIMIT);
    return previous == RE_LOOP_LIMIT ? -1 : (int)previous;
}
RE_TEST_EXPORT size_t beam_re_test_yield_coverage(const int **lines, const uint64_t **visits,
                                                 const uint64_t **yields)
{
#ifdef DEBUG
    extern const int erts_dbg_pcre_cost_chk_lines[];
    extern uint64_t erts_dbg_pcre_cost_chk_visits[], erts_dbg_pcre_cost_chk_yields[];
    extern const int erts_dbg_pcre_cost_chk_cnt;
    *lines = erts_dbg_pcre_cost_chk_lines;
    *visits = erts_dbg_pcre_cost_chk_visits;
    *yields = erts_dbg_pcre_cost_chk_yields;
    return (size_t)erts_dbg_pcre_cost_chk_cnt;
#else
    *lines = NULL; *visits = NULL; *yields = NULL;
    return 0;
#endif
}
#endif

static int load(ErlNifEnv *env, void **private_data, ERL_NIF_TERM information)
{
    ReLibrary *lib;
    ErlNifResourceFlags tried;
    (void)information;
#ifdef BEAM_RE_TESTING
    unsigned fail_stage = atomic_exchange(&test_fail_load_stage, 0);
    if (fail_stage == 1) return -1;
#endif
    lib = enif_alloc(sizeof(*lib));
    if (!lib) return -1;
    memset(lib, 0, sizeof(*lib));
    atomic_init(&lib->bytes, 0); atomic_init(&lib->peak_bytes, 0);
    atomic_init(&lib->allocations, 0); atomic_init(&lib->patterns_live, 0);
    atomic_init(&lib->matches_live, 0); atomic_init(&lib->yields, 0);
    atomic_init(&lib->loop_limit, RE_LOOP_LIMIT);
#define INIT_ATOM(n) a_##n = enif_make_atom(env, #n);
    ATOMS(INIT_ATOM)
#undef INIT_ATOM
    lib->patterns = enif_open_resource_type(env, NULL, "beam_re_pattern", pattern_destructor,
                                            ERL_NIF_RT_CREATE, &tried);
    lib->matches = enif_open_resource_type(env, NULL, "beam_re_match", match_destructor,
                                           ERL_NIF_RT_CREATE, &tried);
#ifdef BEAM_RE_TESTING
    lib->coverage_lock = enif_mutex_create("beam_re_yield_coverage");
#endif
    lib->general = pcre2_general_context_create(backend_alloc, backend_free, lib);
    if (!lib->patterns || !lib->matches || !lib->general
#ifdef BEAM_RE_TESTING
        || !lib->coverage_lock
        || fail_stage == 2
#endif
    ) {
        pcre2_general_context_free(lib->general);
#ifdef BEAM_RE_TESTING
        if (lib->coverage_lock) enif_mutex_destroy(lib->coverage_lock);
#endif
        enif_free(lib);
        return -1;
    }
    *private_data = lib;
#ifdef BEAM_RE_TESTING
    atomic_store(&test_library, lib);
#endif
    return 0;
}
static void unload(ErlNifEnv *env, void *private_data)
{
    ReLibrary *lib = private_data;
    (void)env;
#ifdef BEAM_RE_TESTING
    atomic_store(&test_library, NULL);
    enif_mutex_destroy(lib->coverage_lock);
#endif
    pcre2_general_context_free(lib->general);
    if (atomic_load(&lib->allocations) || atomic_load(&lib->patterns_live) ||
        atomic_load(&lib->matches_live))
        beam_native_report_cleanup(1, "regular-expression backend retained native state at unload");
    enif_free(lib);
}
static ErlNifFunc functions[] = {
    {"compile_nif", 2, compile_nif, ERL_NIF_DIRTY_JOB_CPU_BOUND},
    {"run_nif", 4, run_nif, 0},
    {"inspect_nif", 2, inspect_nif, 0},
    {"import_nif", 1, import_nif, ERL_NIF_DIRTY_JOB_CPU_BOUND},
    {"version_nif", 0, version_nif, 0}
};
ERL_NIF_INIT(re, functions, load, NULL, NULL, unload)
