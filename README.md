# BEAM regular expressions

`erlangsters/re` is an experimental ordinary native package exposing the OTP 29
`re` API with its own PCRE2 10.47 backend. It uses an explicitly selected installed
BEAM SDK whose legacy `re` BIF registrations are disabled. The package supplies
`re` and its private `beam_re_errors` formatter; it requires no OTP startup.

The native implementation uses public SDK interfaces. Matching and UTF-8
validation yield through regular NIF continuations. Compilation, serialization
and import run on dirty CPU schedulers with guarded native stack use. The
backend is privately namespaced for coexistence with host PCRE libraries.

Add the explicitly selected local package to a project's `beam.toml`:

```toml
[dependencies]
"erlangsters/re" = { version = "0.0.1", path = "../beam-erlang-re" }
```

Build/install using the selected SDK's `beam build` and `beam-pkg install`, then
use `beam run --project` or `beam shell --project`. Launch never builds or
installs code. Native artifacts match the exact selected SDK build. Shared and
static outputs are independent selections; the embedding host selects its VM.

The [API reference](doc/src/re.md) describes regular-expression syntax. The Erlang
source documents functions and options. [Maintenance](docs/maintenance.md) owns
source provenance and native maintenance. Linux interpreter/JIT acceptance is
run explicitly; configuration alone establishes no platform qualification.

This package is an alternative provider of module `re`. It cannot replace the
VM's legacy BIFs or coexist with another package supplying that module.
