#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate the pinned PCRE restart tables and a complete private symbol map."""
import argparse
from pathlib import Path
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
lines = (a.source / 'pcre2_match.c').read_text().splitlines()
cost = [i for i, line in enumerate(lines, 1) if 'COST_CHK(' in line and 'define' not in line]
mem = [i for i, line in enumerate(lines, 1) if 'MEMCHR_ERLANG(' in line and 'define' not in line]
(a.output / 'pcre2_match_loop_break_cases.gen.h').write_text(''.join(
    f'case {i}: goto L_LOOP_COUNT_{i};\n' for i in cost if 'DBG_FAKE_' not in lines[i-1]))
(a.output / 'pcre2_match_memchr_break_cases.gen.h').write_text(''.join(
    f'case {i}: goto ERLANG_PCRE2_MATCH_YIELD_LINE_{i};\n' for i in mem))
(a.output / 'pcre2_match_yield_coverage.gen.h').write_text(''.join(
    f'#define ERLANG_YIELD_POINT_{i} {n}\n{i},\n' for n, i in enumerate(cost + mem)) +
    f'#define ERLANG_YIELD_POINT_CNT {len(cost)+len(mem)}\n')
headers = '\n'.join((a.source / n).read_text() for n in ('pcre2.h', 'pcre2_internal.h', 'pcre2_intmodedep.h'))
symbols = {n + '8' for n in re.findall(r'PCRE2_SUFFIX\(([A-Za-z_][A-Za-z0-9_]*)\)', headers)}
aliases = set(re.findall(r'^#define\s+(_pcre2_\w+)\s+PCRE2_SUFFIX', headers, re.M))
symbols.update('_pcre2_' + n for n in re.findall(r'PRIV\(([A-Za-z_][A-Za-z0-9_]*)\)', headers)
               if '_pcre2_' + n not in aliases and n != 'name')
symbols.update(re.findall(r'\berts_dbg_pcre_\w+', '\n'.join(lines)))
(a.output / 'backend_symbols.h').write_text('/* Generated from the pinned backend declarations. */\n' + ''.join(
    f'#define {n} beam_re_backend_{n.lstrip("_")}\n' for n in sorted(symbols)))
(a.output / 'config.h').write_text('''/* Package-owned backend configuration. */
#ifndef BEAM_RE_PCRE_CONFIG_H
#define BEAM_RE_PCRE_CONFIG_H
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_MEMMOVE 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define STDC_HEADERS 1
#endif
''')
