%% SPDX-License-Identifier: Apache-2.0
-module(beam_re_test).
-ifdef(RE_CANONICAL_PROFILE).
-define(BEAM_VM, erlang).
-else.
-define(BEAM_VM, beam).
-endif.

-export([read_file/1, write/1, set_limit/1, coverage/0, stats/0, compile_stats/0, compile_hold/1, corrupt_export/2]).
-nifs([read_file/1, write/1, set_limit/1, coverage/0, stats/0, compile_stats/0, compile_hold/1, corrupt_export/2]).
read_file(_) -> ?BEAM_VM:nif_error(unbound_test_control).
write(_) -> ?BEAM_VM:nif_error(unbound_test_control).
set_limit(_) -> ?BEAM_VM:nif_error(unbound_test_control).
coverage() -> ?BEAM_VM:nif_error(unbound_test_control).
stats() -> ?BEAM_VM:nif_error(unbound_test_control).
compile_stats() -> ?BEAM_VM:nif_error(unbound_test_control).
compile_hold(_) -> ?BEAM_VM:nif_error(unbound_test_control).
corrupt_export(_, _) -> ?BEAM_VM:nif_error(unbound_test_control).
