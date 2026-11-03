# Regex implementation comparison

`tests/benchmark.py` compares explicitly selected installed SDKs: a baseline with
legacy `re` BIFs enabled and a candidate without them. Use preserved legacy SDKs
after physical VM-PCRE removal; do not retain a backend for benchmarking. Both
must have comparable native compiler settings and the same PCRE2 pin. The driver
checks target/compiler metadata and public native headers, records SDK build
identities, and accepts the producer's source snapshot digest separately:

```sh
python3 -B tests/benchmark.py \
  --baseline-sdk "$BASELINE_SDK" --candidate-sdk "$CANDIDATE_SDK" \
  --source "$RE_SOURCE" --work "$BENCHMARK_WORK" \
  --baseline-source-id "$BASELINE_SOURCE_DIGEST" \
  --candidate-source-id "$CANDIDATE_SOURCE_DIGEST"
```

Source digests are producer attestations; matching installed metadata alone
does not prove complete source equivalence. Distinct source identities remain
distinct in the report. `--producer-source-id` retains support for historical
same-source comparisons. Without either form, source equivalence is unverified.
No system Erlang or sibling runtime source
is used for compilation or execution. The original facade and documentation
under `original/` and `doc/` are pristine committed-tree exports verified by
`imports.json`. Baseline list helpers are separately pinned test inputs already
owned under `tests/conformance/support/`.

Each workload runs in separate baseline/candidate host processes, alternating
their order for every repeated sample. Each process configures one ordinary,
one dirty CPU and one dirty I/O scheduler, prepares its input outside measurement,
and performs two warm-up throughput/contended pairs. The default is five measured
pairs per case. `--samples`, `--warmups` and `--cases` allow focused runs; the
report retains those selections and does not represent a partial run as the full
comparison.

The cases cover compilation, cheap precompiled matching, implicit compilation,
large subjects, Unicode validation, limited backtracking, global matching and
capture-heavy binary output. Every operation asserts its expected result.
`CLOCK_PROCESS_CPUTIME_ID` sums CPU across runtime threads; `CLOCK_MONOTONIC`
records wall time. A timed VM call includes call/result bookkeeping and the
workload's loop, but excludes VM startup, input preparation and explicit GC.

Contended measurements run a second Erlang process which cooperatively yields
and records gaps between its executions. The report includes percentiles over
the first 16,384 gaps in each sample and the maximum over all observed gaps.
These are scheduler-progress observations, not a hard real-time guarantee.
Contended time also includes observer startup, collection and shutdown.

The candidate uses backend memory instrumentation with debug yield coverage
disabled. The uncontended sample reports successful backend allocation and
reallocation requests and their requested byte volume; a reallocation contributes
its full new size. After that sample and an explicit Erlang GC, the report records
outstanding backend bytes, allocations and resources while prepared inputs remain
live. These snapshots may include pending reclamation. Peak tracked backend bytes
cover the whole process, including input preparation, warmups and contended work.
Termination must leave no tracked allocations, resources or bytes.

Instrumentation overhead is included in candidate timings. Backend counters
exclude allocator headers, NIF resource payloads, owned environments, subject
flattening buffers and ordinary Erlang storage. The baseline has no equivalent
counters, so unavailable values remain `null`. Whole-process peak RSS is reported
separately; it includes VM images, ordinary modules and prepared inputs. The
baseline SDK retains its legacy VM PCRE backend; final candidate SDKs contain no
VM PCRE. RSS cannot isolate regex retention and is not used as a leak oracle.

The driver records median, spread and candidate/baseline CPU ratios without a
performance pass threshold. Unexpected results, leaked candidate native state,
process failures and incomplete measurement runs are failures. There is no
performance-parity claim. Owned processes are stopped on timeout/interruption;
temporary copied sources, compiled modules, hosts and native libraries are
removed on all exits. Only compact inputs, measurements and bounded failure
diagnostics remain in `results.json` under the selected work directory.
