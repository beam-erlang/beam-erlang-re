# Standalone upstream conformance

`prepare.py --sdk PREFIX --output DIR` verifies the pinned source hashes and
applies the exact replacements recorded in `adaptations.json`. It writes a
`manifest.json` containing compiler inputs, include directories and the explicit
module inventory. It does not compile with or discover an external runtime.

The caller compiles those sources with the selected installed BEAM command and
embeds their bytes alongside the package and its test-only `beam_re_test` native
control module. Invoke `re_conformance:run(DataDir)` with the copied upstream
corpus directory as a character list or UTF-8 binary. The runner executes the original
`re_SUITE:all/0` selection in order, independently enforces its original per-case
timetrap and reports exceptions, skips and unavailable yield coverage as
nonpassing results. It preserves the suite's internal matching, error, monitor
and yield assertions.

The adapters provide native test file input and diagnostic output without OTP
file or I/O services, map explicit Common Test failures to test exceptions and
route loop-budget/coverage controls to the package backend. The UTF validation
trace observes `re:re_match_continue/2` while keeping the original result and
minimum-yield assertions. The Common Test install hook is replaced by the
caller's explicit compiled and embedded fixture inventory. Corpus generators
remain present but are not executed; checked-in expected outputs remain inputs.

`support-imports.json` attests the copied, test-only pure Erlang helpers and
headers from the package's OTP source pin. `unicode_util.erl` is the unchanged
source-attested generated output of that pin's Unicode generator, with its
original generated hash. These sources belong to the conformance fixture and
are never included in package production archives or resolved from tooling
inventories at execution time.

The pure formatting path uses the pinned `epp:default_encoding/0` constant and
`erl_features:keywords/0`. The fixture initializes the pin's default enabled
feature (`maybe_expr`) and its `maybe`/`else` keywords explicitly, replacing the
startup argument discovery that would otherwise require OTP's `init` process.
The preprocessor's file operations are not invoked.
