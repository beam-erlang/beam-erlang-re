%% SPDX-License-Identifier: Apache-2.0
-module(re_test_support).
-export([initialize/0, fail/1, read_file/1, join/1, basename/1,
         set_internal_state/2, get_internal_state/1]).

initialize() ->
    %% Pinned OTP defaults, normally populated from its startup arguments.
    %% The fixture has no OTP init process or feature command-line overrides.
    persistent_term:put({erl_features, enabled_features}, [maybe_expr]),
    persistent_term:put({erl_features, keywords}, ['maybe', 'else']),
    persistent_term:put({erl_features, init_done}, true),
    ok.

fail(Reason) ->
    erlang:error({test_case_failed, Reason}).

read_file(Path) ->
    beam_re_test:read_file(unicode:characters_to_binary(Path)).

join([First | Rest]) -> join(Rest, First).
join([], Acc) -> Acc;
join([[$/ | _] = Absolute | Rest], _) -> join(Rest, Absolute);
join([Next | Rest], []) -> join(Rest, Next);
join([Next | Rest], Acc) -> join(Rest, strip_end(Acc) ++ "/" ++ Next).

strip_end(Path) ->
    case lists:reverse(Path) of
        [$/ | Rest] -> strip_end(lists:reverse(Rest));
        _ -> Path
    end.

basename(Path) -> basename(Path, Path).
basename([], Name) -> Name;
basename([$/ | Rest], _) -> basename(Rest, Rest);
basename([_ | Rest], Name) -> basename(Rest, Name).

set_internal_state(available_internal_state, Enabled) when is_boolean(Enabled) ->
    false;
set_internal_state(re_loop_limit, Value) ->
    beam_re_test:set_limit(Value).

get_internal_state(re_yield_coverage) ->
    beam_re_test:coverage().
