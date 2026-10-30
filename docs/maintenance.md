# Source and native maintenance

`UPSTREAM_COMMIT` pins the OTP source for the facade, regex diagnostics, adapted
PCRE2 and test inputs. `vendor/import.json` records exact committed-tree import
paths and hashes before adaptations. Preserve copyright and license notices.
Imports come from a selected committed tree; maintenance never fetches a floating
backend or rewrites expected outputs with the candidate implementation.

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
Compiled objects are VM-local, while explicit export/import owns portable input
validation and fallback compilation. No hot native replacement is promised.

Independent tests own copied inputs, fixture sources, source-bound adaptation
records and process cleanup. Test-only loop controls and allocation counters are
excluded from production native entries. Build/install under ignored directories;
generated results and development reports do not belong in product documentation.
