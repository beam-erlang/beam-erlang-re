# BEAM regular expressions

`erlangsters/re` is an experimental ordinary native package exposing the OTP 29
`re` API with its own PCRE2 10.47 backend. It uses an explicitly selected installed
BEAM SDK whose legacy `re` BIF registrations are disabled. The package supplies
`re` and its private `beam_re_errors` formatter; it requires no OTP startup.

The native implementation uses public SDK interfaces. Matching and UTF-8
validation yield through regular NIF continuations. Compilation, serialization
and import run on dirty CPU schedulers with guarded native stack use. The
backend is privately namespaced for coexistence with host PCRE libraries.

Exported patterns contain serialized PCRE bytecode and must come from a trusted
producer. Their checksum detects accidental corruption; it does not authenticate
the producer or validate arbitrary bytecode. Use textual patterns across an
untrusted boundary.

Add the explicitly selected local package to a project's `beam.toml`:

```toml
[dependencies]
"erlangsters/re" = { version = "0.0.1", path = "../beam-erlang-re" }
```

Build/install using the selected SDK's `beam build` and `beam-pkg install`, then
use `beam run --project` or `beam shell --project`. Launch never builds or
installs code. Native artifacts match the exact selected SDK build. Shared and
static outputs are independent selections; the embedding host selects its VM.

Embedding hosts bind `chardata_to_utf8_2` and `chardata_to_codepoints_2` to
`beam_unicode:characters_to_binary/2` and `characters_to_list/2`.
These native conversions need no ordinary Unicode code. The private identities
are exact-SDK runtime details; the modern CLI supplies them. The bare modern
test profile has 42 bindings. Independent canonical conformance uses 556
bindings and test-only ordinary Unicode/binary bridges. The same production
sources are compiled with each host's target binding profile. The bridges are
excluded from production archives.

The [API reference](doc/src/re.md) describes regular-expression syntax. The Erlang
source documents functions and options. [Maintenance](docs/maintenance.md) owns
source provenance and native maintenance. Linux interpreter/JIT acceptance is
run explicitly; configuration alone establishes no platform qualification.

This package is an alternative provider of module `re`. It cannot replace the
VM's legacy BIFs or coexist with another package supplying that module.

Run the complete installed acceptance against each candidate SDK explicitly:

```sh
python3 -B tests/acceptance.py --sdk /absolute/candidate-sdk \
  --source "$PWD" --work /absolute/task-scratch/re-acceptance
```

The runner exercises package commands, the original upstream suite, three native
linkage combinations, relocation, isolated execution and resource cleanup. It
removes disposable payloads and retains a compact result. Linux isolation needs
working user namespaces and Bubblewrap. Focused `--case`, `--link` and
`--embedding-only` runs do not replace the complete acceptance. See the
[benchmark procedure](tests/benchmark/README.md) for matched default/candidate
measurements.
