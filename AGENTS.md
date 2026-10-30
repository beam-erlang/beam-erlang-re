# BEAM regular-expression package

This repository owns the ordinary `erlangsters/re` package: the `re` API, private
error formatter, public-SDK NIF glue, adapted PCRE2 backend, package recipe and
independent installed acceptance. Read README.md and docs/maintenance.md before
changing imported sources. Preserve upstream notices and source provenance.

Use explicitly selected installed public BEAM SDKs only. No private VM headers,
sibling source/build dependencies, external Erlang execution, OTP application
startup or automatic fetching. The SDK must omit legacy `re` BIF registrations.
Preserve resumable matching/UTF validation, stack safety, errors, compiled-pattern
serialization, cancellation and native ownership. Native code/resources remain
resident until teardown completes. Keep backend symbols private in static and
shared linkage; one process uses one VM implementation.

Package tests own their sources and retain upstream behavioral assertions.
Production modules are `re` and `beam_re_errors`; test instrumentation stays out
of production native entries. Build under ignored build/, install under ignored
install/, and remove disposable copied inputs on success, failure and catchable
interruption after stopping owned processes. Development plans/reports/logs belong
outside tracked product sources. Work in this main checkout. No commits, pushes,
publication, system installation or OTP migration are implied.
