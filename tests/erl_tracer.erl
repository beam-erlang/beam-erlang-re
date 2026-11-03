%% SPDX-License-Identifier: Apache-2.0
%% Explicit test-only provider for the running-process trace observations.
-module(erl_tracer).
-ifdef(RE_CANONICAL_PROFILE).
-define(BEAM_VM, erlang).
-else.
-define(BEAM_VM, beam).
-endif.

-export([enabled/3, trace/5]).
-nifs([enabled/3, trace/5]).
enabled(_, _, _) -> ?BEAM_VM:nif_error(unbound_test_tracer).
trace(_, _, _, _, _) -> ?BEAM_VM:nif_error(unbound_test_tracer).
