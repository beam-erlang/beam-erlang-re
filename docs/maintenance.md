# Source and native maintenance

`UPSTREAM_COMMIT` pins the OTP source for the facade, regex diagnostics, adapted
PCRE2 and test inputs. `vendor/import.json` records exact committed-tree import
paths and hashes before adaptations. Preserve copyright and license notices.
Imports come from a selected committed tree; maintenance never fetches a floating
backend or rewrites expected outputs with the candidate implementation.

The reviewed adaptations from that import are:

| Input | Package adaptation |
| --- | --- |
| `re.erl` | Ordinary public wrappers and private NIF operations; local list/slicing helpers; public error-frame normalization. Global matching, replacement, splitting and Unicode conversion retain the imported Erlang algorithms. |
| `erl_stdlib_errors.erl` | Regex-only extraction into `beam_re_errors`, with local formatting helpers and an explicit private formatter identity. |
| `erl_bif_re.c` | Public NIF terms, binaries, resources and callbacks replace private VM operations. Per-operation compilation contexts, guarded dirty compilation/import/export, regular resumable matching and cancellation own native lifetime. |
| `erts/emulator/pcre` | Backend sources remain pinned. Package configuration and generators select interrupted matching, generated restart tables and a private symbol namespace. |
| Upstream suites and helpers | Pristine originals remain under `tests/upstream`; hash-checked harness replacements are recorded in `tests/conformance/adaptations.json`. Test helpers and benchmark baseline inputs have separate import manifests. |

The maximum matcher budget is 64,000 loop-cost units per invocation. Consumed
work is charged through `enif_consume_timeslice`; exhausted work schedules a
regular continuation. This policy does not promise the BIF's exact reduction
counts. Compilation uses the public stack query with a 10 KiB reserve and keeps
the normal PCRE stack-check failure when the bound is insufficient or unknown.

The backend retains Erlang's interrupted matching and UTF-validation machinery.
The package owns its configuration, explicit source inventory, generated restart
tables and backend symbol map. Consumer builds use public installed SDK targets
and headers only. Shared exports and static definitions must preserve backend
privacy, including debug instrumentation. The existing public SDK CRC operation
supports exported-pattern integrity; no additional compression implementation is
bundled.

Public wrappers retain `re` results and diagnostics. Formatter identity,
continuation identity, compilation scheduling and exact reduction accounting are
implementation adaptations. Native resources keep their code resident through
reclamation; caller termination does not imply a dirty callback has finished.
Compiled objects are VM-local. Explicit export/import checks framing, checksum,
serialized lengths and name-table bounds, and recompiles the included source
when the cache format is incompatible. PCRE serialization is a trusted bytecode
cache, not an untrusted-input format: these checks do not verify all opcodes or
authenticate the producer. Preserve that trust boundary when changing decoding.
No hot native replacement is promised.

Independent tests own copied inputs, fixture sources, source-bound adaptation
records and process cleanup. Test-only loop controls and allocation counters are
excluded from production native entries. Build/install under ignored directories;
generated results and development reports do not belong in product documentation.

Installed bare acceptance uses independent target binding profiles. Modern
selects 42 contracts for the implementation and its test controllers, with none
of the nine canonical facade modules. Canonical conformance selects all 556
canonical contracts. Its test-only `beam_binary` bridge calls canonical
repetition and referenced-size operations.
Its test-only `beam_unicode` bridge calls canonical conversions.
The same production sources are compiled for each target profile; the native
implementation and algorithm remain shared while imports name that host's
selected destinations. Preserve upstream hashes, selections, assertions and
deadlines.
