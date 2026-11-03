%% SPDX-License-Identifier: Apache-2.0
%% Copyright 2026 Erlangsters.
-module(import_bounds_checks).
-ifdef(RE_CANONICAL_PROFILE).
-define(BEAM_VM, erlang).
-else.
-define(BEAM_VM, beam).
-endif.

-export([run/0]).

run() ->
    {ok, Exported} = re:compile(<<"(?<safe>a)">>, [export]),
    valid_cache(Exported),
    cases([table_extent, unterminated_name, invalid_group, code_start, bad_mode],
          Exported).

cases([Case | Rest], Exported) ->
    Corrupted = beam_re_test:corrupt_export(Exported, Case),
    try re:import(Corrupted) of
        Accepted -> ?BEAM_VM:error({accepted_corrupt_metadata, Case, Accepted})
    catch
        error:badarg -> ok
    end,
    valid_cache(Exported),
    cases(Rest, Exported);
cases([], _) ->
    ok.

valid_cache(Exported) ->
    Pattern = re:import(Exported),
    {namelist, [<<"safe">>]} = re:inspect(Pattern, namelist),
    {match, [{1, 1}, {1, 1}]} = re:run(<<"ba">>, Pattern),
    ok.
