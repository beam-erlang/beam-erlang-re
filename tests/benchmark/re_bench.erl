%% SPDX-License-Identifier: Apache-2.0
-module(re_bench).
-export([verify/1, prepare/2, sample/0, contended/0, latency_json/0,
         collect/0, finish/0]).

verify(Builtin) ->
    Builtin = erlang:is_builtin(re, run, 3),
    <<"10.47 2025-10-21">> = re:version(),
    ok.

prepare(Case, Iterations) ->
    Inputs = inputs(Case),
    persistent_term:put({?MODULE, workload}, {Case, Iterations, Inputs}),
    ok.

inputs(compile) -> <<"^(?<word>[a-z]+)-(?<number>[0-9]+)$">>;
inputs(compiled) ->
    {ok, Pattern} = re:compile(<<"^abc$">>),
    {<<"abc">>, Pattern};
inputs(implicit) -> {<<"abc-123">>, <<"^[a-z]+-[0-9]+$">>};
inputs(large) ->
    {ok, Pattern} = re:compile(<<"^a+$">>),
    {binary:copy(<<"a">>, 1024 * 1024), Pattern};
inputs(unicode) ->
    {ok, Pattern} = re:compile(<<"b">>, [unicode]),
    {binary:copy(<<"abc\n">>, 262144), Pattern};
inputs(backtrack) ->
    {ok, Pattern} = re:compile(<<"(a+)*zz">>),
    {<<"aaaaaaaaaaaaaz">>, Pattern};
inputs(global) ->
    {ok, Pattern} = re:compile(<<"b">>),
    {binary:copy(<<"abc\n">>, 256), Pattern};
inputs(captures) ->
    {ok, Pattern} = re:compile(<<"([a-z])([0-9])">>),
    {<<"a1 b2 c3 d4">>, Pattern}.

sample() ->
    {Case, Iterations, Inputs} = persistent_term:get({?MODULE, workload}),
    repeat(Case, Iterations, Inputs).

repeat(_, 0, _) -> ok;
repeat(Case, Remaining, Inputs) ->
    operation(Case, Inputs),
    repeat(Case, Remaining - 1, Inputs).

operation(compile, Pattern) -> {ok, _} = re:compile(Pattern);
operation(compiled, {Subject, Pattern}) ->
    match = re:run(Subject, Pattern, [{capture, none}]);
operation(implicit, {Subject, Pattern}) ->
    match = re:run(Subject, Pattern, [{capture, none}]);
operation(large, {Subject, Pattern}) ->
    match = re:run(Subject, Pattern, [{capture, none}]);
operation(unicode, {Subject, Pattern}) ->
    {match, [{1,1}]} = re:run(Subject, Pattern);
operation(backtrack, {Subject, Pattern}) ->
    {error, match_limit} =
        re:run(Subject, Pattern, [{match_limit, 3000}, report_errors]);
operation(global, {Subject, Pattern}) ->
    match = re:run(Subject, Pattern, [global, {capture, none}]);
operation(captures, {Subject, Pattern}) ->
    {match, [[<<"a1">>, <<"a">>, <<"1">>],
             [<<"b2">>, <<"b">>, <<"2">>],
             [<<"c3">>, <<"c">>, <<"3">>],
             [<<"d4">>, <<"d">>, <<"4">>]]} =
        re:run(Subject, Pattern, [global, {capture, all, binary}]).

contended() ->
    Parent = self(),
    {Observer, Monitor} = spawn_monitor(fun() ->
        Parent ! {ready, self()},
        observe(Parent, erlang:monotonic_time(nanosecond), 0, 0, [])
    end),
    try
        receive {ready, Observer} -> ok
        after 1000 -> erlang:error(observer_not_ready)
        end,
        sample(),
        Observer ! stop,
        receive
            {latency, Observer, Count, Maximum, Samples} ->
                persistent_term:put({?MODULE, latency}, {Count, Maximum, Samples})
        after 5000 -> erlang:error(observer_no_result)
        end,
        receive {'DOWN', Monitor, process, Observer, normal} -> ok
        after 1000 -> erlang:error(observer_not_stopped)
        end,
        ok
    after
        exit(Observer, kill),
        erlang:demonitor(Monitor, [flush])
    end.

observe(Parent, Previous, Count, Maximum, Samples) ->
    Now = erlang:monotonic_time(nanosecond),
    Gap = Now - Previous,
    NewMaximum = max(Gap, Maximum),
    NewSamples = case Count < 16384 of
                     true -> [Gap | Samples];
                     false -> Samples
                 end,
    receive
        stop -> Parent ! {latency, self(), Count + 1, NewMaximum, NewSamples}
    after 0 ->
        erlang:yield(),
        observe(Parent, Now, Count + 1, NewMaximum, NewSamples)
    end.

latency_json() ->
    {Count, Maximum, Samples} = persistent_term:get({?MODULE, latency}),
    iolist_to_binary(["{\"observations\":", integer_to_binary(Count),
                      ",\"max_ns\":", integer_to_binary(Maximum),
                      ",\"samples_ns\":[", integers_json(Samples), "]}"]).

integers_json([]) -> [];
integers_json([Value]) -> integer_to_binary(Value);
integers_json([Value | Rest]) ->
    [integer_to_binary(Value), $, | integers_json(Rest)].

collect() ->
    erlang:garbage_collect(),
    ok.

finish() ->
    persistent_term:erase({?MODULE, workload}),
    persistent_term:erase({?MODULE, latency}),
    collect().
