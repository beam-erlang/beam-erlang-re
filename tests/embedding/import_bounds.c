/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Erlangsters.
 * Test-only mutations of package-owned PCRE metadata. */
#include <erl_nif.h>
#include <beam/zlib.h>
#include <stdint.h>
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
#include "../../vendor/pcre2/pcre2_internal.h"

/* Register only in the package-owned test control module. The production NIF
 * does not expose layout offsets or a way to manufacture malformed caches. */
ERL_NIF_TERM beam_re_test_corrupt_export(ErlNifEnv *env, int argc,
                                        const ERL_NIF_TERM argv[])
{
    const ERL_NIF_TERM *tuple;
    ErlNifBinary header, encoded;
    ERL_NIF_TERM header_term, encoded_term;
    unsigned char *header_copy, *encoded_copy;
    pcre2_real_code block;
    size_t prefix = sizeof(pcre2_serialized_data) + TABLES_LENGTH;
    int arity;
    (void)argc;
    if (!enif_get_tuple(env, argv[0], &arity, &tuple) || arity != 5 ||
        !enif_is_identical(tuple[0], enif_make_atom(env, "re_exported_pattern")) ||
        !enif_inspect_binary(env, tuple[1], &header) || header.size < 14 ||
        !enif_inspect_binary(env, tuple[4], &encoded) || encoded.size < prefix ||
        encoded.size - prefix < sizeof(block))
        return enif_make_badarg(env);
    memcpy(&block, encoded.data + prefix, sizeof(block));
    if (block.blocksize > encoded.size - prefix || block.blocksize <= sizeof(block) ||
        !block.name_count || block.name_entry_size < IMM2_SIZE + 1 ||
        block.name_count > (block.blocksize - sizeof(block)) / block.name_entry_size)
        return enif_make_badarg(env);
    header_copy = enif_make_new_binary(env, header.size, &header_term);
    encoded_copy = enif_make_new_binary(env, encoded.size, &encoded_term);
    if (!header_copy || !encoded_copy)
        return enif_raise_exception(env, enif_make_atom(env, "system_limit"));
    memcpy(header_copy, header.data, header.size);
    memcpy(encoded_copy, encoded.data, encoded.size);
    if (enif_is_identical(argv[1], enif_make_atom(env, "table_extent"))) {
        size_t count = (block.blocksize - sizeof(block)) / block.name_entry_size + 1;
        if (count > MAX_NAME_COUNT) return enif_make_badarg(env);
        block.name_count = (uint16_t)count;
    } else if (enif_is_identical(argv[1], enif_make_atom(env, "unterminated_name"))) {
        memset(encoded_copy + prefix + sizeof(block) + IMM2_SIZE, 'X',
                block.name_entry_size - IMM2_SIZE);
    } else if (enif_is_identical(argv[1], enif_make_atom(env, "invalid_group"))) {
        unsigned group = block.top_bracket == UINT16_MAX ? 0 : block.top_bracket + 1;
        encoded_copy[prefix + sizeof(block)] = (unsigned char)(group >> 8);
        encoded_copy[prefix + sizeof(block) + 1] = (unsigned char)group;
    } else if (enif_is_identical(argv[1], enif_make_atom(env, "code_start"))) {
        block.code_start = block.blocksize;
    } else if (enif_is_identical(argv[1], enif_make_atom(env, "bad_mode"))) {
        block.flags = (block.flags & ~PCRE2_MODE_MASK) | PCRE2_MODE16;
    } else return enif_make_badarg(env);
    memcpy(encoded_copy + prefix, &block, sizeof(block));
    uint32_t crc = (uint32_t)beam_zlib_crc32_z(0, encoded_copy, encoded.size);
    header_copy[8] = (unsigned char)(crc >> 24);
    header_copy[9] = (unsigned char)(crc >> 16);
    header_copy[10] = (unsigned char)(crc >> 8);
    header_copy[11] = (unsigned char)crc;
    return enif_make_tuple5(env, tuple[0], header_term, tuple[2], tuple[3], encoded_term);
}
