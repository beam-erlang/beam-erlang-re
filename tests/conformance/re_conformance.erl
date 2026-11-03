%% SPDX-License-Identifier: Apache-2.0
-module(re_conformance).
-export([run/1, run_case/2]).

run(DataDir) when is_binary(DataDir) ->
    run(unicode:characters_to_list(DataDir));
run(DataDir) when is_list(DataDir) ->
    run_selection(DataDir, all).

run_case(DataDir, Case) when is_binary(DataDir), is_atom(Case) ->
    run_case(unicode:characters_to_list(DataDir), Case);
run_case(DataDir, Case) when is_list(DataDir), is_atom(Case) ->
    true = lists:member(Case, re_SUITE:all()),
    run_selection(DataDir, {single, Case}).

run_selection(DataDir, Selection) ->
    ok = re_test_support:initialize(),
    [] = re_SUITE:groups(),
    Suite = re_SUITE:suite(),
    {timetrap, Trap} = lists:keyfind(timetrap, 1, Suite),
    Timeout = milliseconds(Trap),
    Config = re_SUITE:init_per_suite([{data_dir, DataDir}]),
    Cases = case Selection of
                all -> re_SUITE:all();
                {single, Case} -> [Case]
            end,
    Results = run_cases(Cases, Config, Timeout, []),
    ok = re_SUITE:end_per_suite(Config),
    Failures = [Failure || {fail, _, _} = Failure <- Results],
    Label = case Selection of
                all -> "RE_CONFORMANCE";
                {single, _} -> "RE_CONFORMANCE_SELECTED"
            end,
    re_test_io:format("~s ~p cases, ~p failures~n",
                      [Label, length(Results), length(Failures)]),
    case Failures of
        [] -> ok;
        _ -> {error, {conformance_failed, Failures}}
    end.

milliseconds({minutes, N}) when is_integer(N), N > 0 -> N * 60000;
milliseconds({seconds, N}) when is_integer(N), N > 0 -> N * 1000;
milliseconds(N) when is_integer(N), N > 0 -> N.

run_cases([], _, _, Results) -> lists:reverse(Results);
run_cases([Case | Rest], Config, Timeout, Results) when is_atom(Case) ->
    Parent = self(),
    Token = make_ref(),
    Start = erlang:monotonic_time(microsecond),
    re_test_io:format("RE_CASE ~p start~n", [Case]),
    {Pid, Monitor} = spawn_monitor(fun() ->
        Outcome = try apply(re_SUITE, Case, [Config]) of
                      {skip, Reason} -> {fail, {skip, bounded(Reason)}};
                      {fail, Reason} -> {fail, {explicit_failure, bounded(Reason)}};
                      _ -> pass
                  catch
                      Class:Reason:Stack ->
                          {fail, {Class, bounded(Reason), bounded(Stack)}}
                  end,
        Parent ! {Token, Outcome}
    end),
    Result = receive
                 {Token, Outcome} ->
                     receive {'DOWN', Monitor, process, Pid, normal} -> ok end,
                     Outcome;
                 {'DOWN', Monitor, process, Pid, Reason} ->
                     {fail, {process_exit, bounded(Reason)}}
             after Timeout ->
                 exit(Pid, kill),
                 receive {'DOWN', Monitor, process, Pid, _} -> ok end,
                 {fail, {timetrap, Timeout}}
             end,
    _ = beam_re_test:set_limit(default),
    Elapsed = erlang:monotonic_time(microsecond) - Start,
    Row = case Result of
              pass -> {pass, Case, Elapsed};
              {fail, Failure} -> {fail, Case, Failure}
          end,
    re_test_io:format("RE_CASE ~p ~p (~p us)~n", [Case, Result, Elapsed]),
    run_cases(Rest, Config, Timeout, [Row | Results]).

bounded(Term) ->
    Bytes = unicode:characters_to_binary(io_lib:format("~P", [Term, 12])),
    case Bytes of
        <<Prefix:8192/binary, _/binary>> -> <<Prefix/binary, "...">>;
        _ -> Bytes
    end.
