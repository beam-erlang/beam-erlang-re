%%
%% %CopyrightBegin%
%%
%% SPDX-License-Identifier: Apache-2.0
%%
%% Copyright Ericsson AB 2008-2025. All Rights Reserved.
%%
%% Licensed under the Apache License, Version 2.0 (the "License");
%% you may not use this file except in compliance with the License.
%% You may obtain a copy of the License at
%%
%%     http://www.apache.org/licenses/LICENSE-2.0
%%
%% Unless required by applicable law or agreed to in writing, software
%% distributed under the License is distributed on an "AS IS" BASIS,
%% WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
%% See the License for the specific language governing permissions and
%% limitations under the License.
%%
%% %CopyrightEnd%
%%
-module(re_checks).
-ifdef(RE_CANONICAL_PROFILE).
-define(BEAM_VM, erlang).
-else.
-define(BEAM_VM, beam).
-endif.

-ifdef(RE_CANONICAL_PROFILE).
-define(RETENTION, persistent_term).
-else.
-define(RETENTION, beam_persistent_term).
-endif.
-export([run/0, retain/0, release/0, resource_checks/0, yield_checks/0,
         share_checks/0]).

run() ->
    profile_absence(),
    ordinary_functions([{version,0}, {compile,1}, {compile,2}, {run,2},
                        {run,3}, {internal_run,4}, {inspect,2}, {import,1}]),
    <<"10.47 2025-10-21">> = re:version(),
    basic(),
    captures(),
    unicode(),
    global(),
    split_and_replace(),
    limits(),
    errors(),
    exports(),
    sub_binaries(),
    share_checks(),
    ok.

profile_absence() ->
    case ?BEAM_VM:is_builtin(beam_persistent_term, put, 2) of
        false -> ok;
        true -> absent_facades([ets,binary,lists,maps,string,unicode,atomics,counters,persistent_term])
    end.
absent_facades([M|Rest]) ->
    false=?BEAM_VM:module_loaded(M),
    []=[E || E={Module,_,_} <- ?BEAM_VM:system_info(snifs), Module=:=M],
    absent_facades(Rest);
absent_facades([]) -> ok.

ordinary_functions([{Function,Arity}|Rest]) ->
    false = ?BEAM_VM:is_builtin(re, Function, Arity),
    ordinary_functions(Rest);
ordinary_functions([]) -> ok.

basic() ->
    {match, [{2,3}]} = re:run(<<"xxabczz">>, <<"abc">>),
    nomatch = re:run("abc", "z"),
    match = re:run([<<"ab">>, $c], [<<"a">>, "bc"], [{capture, none}]),
    {ok, Pattern} = re:compile("^abc$", [caseless]),
    match = re:run(<<"ABC">>, Pattern, [{capture, none}]),
    nomatch = re:run(<<"xABC">>, Pattern),
    {ok, DotAll} = re:compile("a.b", [dotall]),
    match = re:run("a\nb", DotAll, [{capture, none}]),
    {match, [{4,3}]} = re:run("abc abc", "abc", [{offset, 1}]),
    {ok, CRLF} = re:compile("abc$", [{newline, crlf}]),
    match = re:run("abc\r\n", CRLF, [{newline, crlf}, {capture, none}]),
    {error, {Message, Position}} = re:compile("("),
    true = is_list(Message),
    true = is_integer(Position),
    ok.

captures() ->
    {ok, Named} = re:compile("(?<first>a)(?<second>b)?"),
    {namelist, [<<"first">>, <<"second">>]} = re:inspect(Named, namelist),
    {match, [<<"ab">>, <<"a">>, <<"b">>]} =
        re:run("ab", Named, [{capture, all, binary}]),
    {match, ["a", []]} = re:run("a", Named, [{capture, all_names, list}]),
    {match, [{0,1}, {-1,0}]} = re:run("a", Named, [{capture, [first, second]}]),
    {match, [<<"b">>]} = re:run("ab", Named, [{capture, ["second"], binary}]),
    {ok, Duplicate} = re:compile("(?<C>A)|(?<B>B)|(?<C>C)", [dupnames]),
    {namelist, [<<"B">>, <<"C">>]} = re:inspect(Duplicate, namelist),
    {match, [<<"B">>, <<>>]} =
        re:run("B", Duplicate, [{capture, all_names, binary}]),
    ok.

unicode() ->
    Subject = [<<"a">>, [16#400], [<<"b">>]],
    {ok, Pattern} = re:compile([$(, 16#400, $)], [unicode]),
    {match, [[16#400], [16#400]]} = re:run(Subject, Pattern, [{capture, all, list}]),
    {match, [{1,2}, {1,2}]} = re:run(Subject, Pattern),
    {match, [<<16#400/utf8>>]} =
        re:run(Subject, [16#400], [unicode, {capture, first, binary}]),
    nomatch = re:run([16#400], "\\w", [unicode]),
    {match, [{0,2}]} = re:run([16#400], "\\w", [unicode, ucp]),
    expect_badarg(run, [<<255>>, "a", [unicode]], []),
    expect_badarg(run, [<<16#C3>>, "a", [unicode]], []),
    expect_badarg(compile, [[16#110000], [unicode]], []),
    expect_badarg(run, [[16#110000], "a", [unicode]], []),
    ok.

global() ->
    {match, [[{0,2}, {0,1}], [{3,2}, {3,1}]]} =
        re:run("ab ab", "(a)b", [global]),
    {match, [[<<"a">>], [<<"a">>]]} =
        re:run("ab ab", "(?<x>a)b", [global, {capture, all_names, binary}]),
    {match, [[{0,0}], [{1,0}], [{1,2}], [{3,0}]]} =
        re:run("cat", "(|at)", [global, {capture, first}]),
    match = re:run("ab ab", "a", [global, {capture, none}]),
    nomatch = re:run("ab", "z", [global]),
    {match, [[[16#400]], [[16#400]]]} =
        re:run([16#400, $a, 16#400], [16#400],
               [unicode, global, {capture, first, list}]),
    ok.

split_and_replace() ->
    [<<"a">>, <<"b">>, <<>>] = re:split("a,b,", ",", [{return, binary}]),
    ["a", "b"] = re:split("a,b,", ",", [trim, {return, list}]),
    [<<"a">>, <<",">>, <<"b">>] =
        re:split("a,b", "(,)", [{return, binary}]),
    [[<<"a">>, <<",">>], [<<"b">>]] =
        re:split("a,b", "(,)", [group, {return, binary}]),
    [<<"a">>, <<"b,c">>] =
        re:split("a,b,c", ",", [{parts, 2}, {return, binary}]),
    <<"aXcX">> = re:replace("abcb", "b", "X", [global, {return, binary}]),
    "aXcb" = re:replace("abcb", "b", "X", [{return, list}]),
    <<"b-ad-c">> =
        re:replace("abcd", "(.)(.)",
                   fun(_, [A, B]) -> <<B/binary, $-, A/binary>> end,
                   [global, {return, binary}]),
    <<"a",16#400/utf8,"c">> =
        re:replace("abc", "b", [16#400], [unicode, {return, binary}]),
    <<"<ab>cd">> = re:replace("abcd", "(ab)", "<\\1>", [{return, binary}]),
    ok.

limits() ->
    Subject = "aaaaaaaaaaaaaz",
    nomatch = re:run(Subject, "(a+)*zz", [{match_limit, 3000}]),
    {error, match_limit} =
        re:run(Subject, "(a+)*zz", [{match_limit, 3000}, report_errors]),
    {error, match_limit_recursion} =
        re:run(Subject, "(a+)*zz", [{match_limit_recursion, 10}, report_errors]),
    {error, match_limit} =
        re:run(Subject, "(a+)*zz", [{match_limit, 3000}, report_errors, global]),
    expect_badarg(run, ["a", "a", [{match_limit, -1}]], []),
    ok.

errors() ->
    expect_badarg(compile, [not_iodata], [{1, <<"not an iodata term">>}]),
    expect_badarg(compile, ["a", [bad_option]], [{2, <<"invalid options">>}]),
    expect_badarg(run, [not_iodata, "a"], [{1, <<"not an iodata term">>}]),
    expect_badarg(run, ["a", {re_pattern,0,0,0,make_ref()}],
                  [{2, <<"neither an iodata term nor a compiled regular expression">>}]),
    expect_badarg(run, ["a", "a", [bad_option]], [{3, <<"invalid options">>}]),
    expect_badarg(run, ["a", {1,2,3,4}, [global]], []),
    expect_badarg(inspect, ["a", namelist], [{1, <<"not a compiled regular expression">>}]),
    expect_badarg(import, [17], [{1, <<"not an exported regular expression">>}]),
    expect_badarg(split, ["a", "a", [bad_option]], empty),
    expect_badarg(replace, ["a", "a", "x", [bad_option]], empty),
    expect_badarg(split, ["a", "a", [{return, invalid}]], [{3, <<"invalid options">>}]),
    expect_badarg(replace, ["a", "a", "x", [{return, invalid}]], [{4, <<"invalid options">>}]),
    case re:run("a", "(", [report_errors]) of
        {error, {compile, {Text, Offset}}} ->
            true = is_list(Text),
            true = is_integer(Offset)
    end,
    ok.

expect_badarg(Function, Args, Expected) ->
    try apply(re, Function, Args) of
        Result -> ?BEAM_VM:error({expected_badarg, Function, Args, Result})
    catch
        error:badarg:Stack ->
            [{re, Function, Args, Info}, {?MODULE, expect_badarg, 3, _} | _] = Stack,
            #{module := beam_re_errors} = property(error_info, Info),
            Formatted = beam_re_errors:format_error(badarg, Stack),
            check_diagnostics(Expected, Formatted)
    end.

check_diagnostics(empty, Actual) ->
    0 = map_size(Actual),
    ok;
check_diagnostics([{Position, Text} | Rest], Actual) ->
    #{Position := Text} = Actual,
    check_diagnostics(Rest, Actual);
check_diagnostics([], _) -> ok.

property(Key, [{Key, Value} | _]) -> Value;
property(Key, [_ | Rest]) -> property(Key, Rest).

exports() ->
    {ok, Exported} = re:compile(<<"exported">>, [export]),
    export_roundtrip(Exported),
    {ok, ExportedList} = re:compile("exported", [export]),
    export_roundtrip(ExportedList),
    ok.

export_roundtrip(Exported) ->
    match = re:run("exported", re:import(Exported), [{capture, none}]),
    {re_exported_pattern, Hdr, RE, Opts, Encoding} = Exported,
    Unaligned = {re_exported_pattern, unalign(Hdr), unalign(RE), Opts, unalign(Encoding)},
    match = re:run("exported", re:import(Unaligned), [{capture, none}]),
    Fake = setelement(3, Exported, <<"fallback">>),
    match = re:run("exported", re:import(Fake), [{capture, none}]),
    <<Magic:32, Major:16, Minor:16/little, Tail/binary>> = Encoding,
    ChangedVersion = <<Magic:32, Major:16, (Minor+1):16/little, Tail/binary>>,
    match = re:run("fallback", re:import(rebuild(Fake, Hdr, ChangedVersion)),
                   [{capture, none}]),
    <<MagicBig:32/big, AfterMagic/binary>> = Encoding,
    ChangedEndian = <<MagicBig:32/little, AfterMagic/binary>>,
    match = re:run("fallback", re:import(rebuild(Fake, Hdr, ChangedEndian)),
                   [{capture, none}]),
    <<Title:8/binary, CRC:32, EncodingVersion, Unicode>> = Hdr,
    ChangedHeader = <<Title/binary, CRC:32, (EncodingVersion+1), Unicode>>,
    Fallback = rebuild(Fake, ChangedHeader, Encoding),
    match = re:run("fallback", re:import(Fallback), [{capture, none}]),
    expect_badarg(import, [setelement(3, Fallback, <<"(broken fallback">>)], []),
    <<First, Remainder/binary>> = Encoding,
    Corrupt = setelement(5, Exported, <<(First bxor 1), Remainder/binary>>),
    expect_badarg(import, [Corrupt], []),
    ok.

rebuild({re_exported_pattern, _, RE, Opts, _}, Hdr, Encoding) ->
    <<Title:8/binary, _:32, HeaderRest/binary>> = Hdr,
    {re_exported_pattern,
     <<Title/binary, (?BEAM_VM:crc32(Encoding)):32, HeaderRest/binary>>,
     RE, Opts, Encoding}.

unalign(Binary) ->
    erts_debug:unaligned_bitstring(Binary, 3).

sub_binaries() ->
    Subject = beam_bytes:repeat(<<"abcd">>, 1024),
    {match, [Subject]} = re:run(Subject, <<"^.+$">>, [{capture, first, binary}]),
    {match, [Capture]} = re:run(Subject, <<"^.{512}(.{512})">>, [{capture, [1], binary}]),
    512 = byte_size(Capture),
    true = beam_bytes:referenced_size(Capture) >= byte_size(Subject),
    ?RETENTION:put({?MODULE, capture}, Capture),
    erlang:garbage_collect(),
    Capture = ?RETENTION:get({?MODULE, capture}),
    <<"abcd", _/binary>> = Capture,
    ?RETENTION:erase({?MODULE, capture}),
    Pattern = unalign(<<"^abcd$">>),
    match = re:run(unalign(<<"abcd">>), Pattern, [{capture, none}]),
    {ok, Compiled} = re:compile(Pattern),
    match = re:run(unalign(<<"abcd">>), Compiled, [{capture, none}]),
    expect_badarg(run, [<<0:1, "abcd">>, Pattern], []),
    ok.

share_checks() ->
    {ok, Pattern} = re:compile("^(?<word>[a-z]+)$"),
    Parent = self(),
    {Pid, Ref} = erlang:spawn_monitor(fun() ->
        Parent ! {self(), re:run("shared", Pattern, [{capture, all_names, binary}])}
    end),
    receive {Pid, {match, [<<"shared">>]}} -> ok
    after 1000 -> ?BEAM_VM:error(shared_pattern_timeout)
    end,
    receive {'DOWN', Ref, process, Pid, normal} -> ok
    after 1000 -> ?BEAM_VM:error(shared_pattern_exit_timeout)
    end,
    {match, [<<"stillvalid">>]} =
        re:run("stillvalid", Pattern, [{capture, all_names, binary}]),
    ok.

yield_checks() ->
    Previous = beam_re_test:set_limit(-1),
    try
        Subject = beam_bytes:repeat(<<"abc\n">>, 100000),
        Go = make_ref(),
        {Pid, Ref} = erlang:spawn_monitor(fun() ->
            receive Go -> ok end,
            {match, [{1,1}]} = re:run(Subject, <<"b">>, [unicode])
        end),
        1 = ?BEAM_VM:trace(Pid, true, [running]),
        Pid ! Go,
        Count = validation_trace(Pid, Ref, 0),
        true = Count >= 5,
        ok
    after
        beam_re_test:set_limit(Previous)
    end.

validation_trace(Pid, Ref, Count) ->
    receive
        {trace, Pid, out, {re, re_match_continue, 2}} ->
            validation_trace(Pid, Ref, Count + 1);
        {'DOWN', Ref, process, Pid, normal} ->
            Barrier = ?BEAM_VM:trace_delivered(Pid),
            validation_barrier(Pid, Barrier, Count);
        {'DOWN', Ref, process, Pid, Reason} ->
            ?BEAM_VM:error({validation_failed, Reason})
    after 5000 -> ?BEAM_VM:error(validation_timeout)
    end.

validation_barrier(Pid, Barrier, Count) ->
    receive
        {trace, Pid, out, {re, re_match_continue, 2}} ->
            validation_barrier(Pid, Barrier, Count + 1);
        {trace_delivered, Pid, Barrier} -> Count
    after 5000 -> ?BEAM_VM:error(validation_trace_timeout)
    end.

resource_checks() ->
    release(),
    erlang:garbage_collect(),
    Baseline = beam_re_test:stats(),
    {Pid, Ref} = erlang:spawn_monitor(fun() -> run() end),
    receive {'DOWN', Ref, process, Pid, normal} -> ok;
            {'DOWN', Ref, process, Pid, Reason} -> ?BEAM_VM:error({resource_case_failed, Reason})
    after 5000 -> ?BEAM_VM:error(resource_case_timeout)
    end,
    await_stats(Baseline, 200),
    kill_compiling(),
    await_stats(Baseline, 200),
    kill_matching("\\w+\\d"),
    await_stats(Baseline, 200),
    compiled_kill(),
    erlang:garbage_collect(),
    await_stats(Baseline, 200),
    ok.

kill_compiling() ->
    {Entered, 0} = beam_re_test:compile_stats(),
    Previous = beam_re_test:compile_hold(1000),
    {Pid, Ref} = erlang:spawn_monitor(fun() -> re:compile("(?<word>[a-z]+)") end),
    try
        await_compile_entry(Pid, Ref, Entered, 500),
        exit(Pid, kill),
        receive {'DOWN', Ref, process, Pid, killed} -> ok
        after 1000 -> ?BEAM_VM:error(killed_compile_timeout)
        end,
        await_compile_done(200)
    after
        exit(Pid, kill),
        ?BEAM_VM:demonitor(Ref, [flush]),
        beam_re_test:compile_hold(Previous)
    end.

await_compile_entry(Pid, Ref, Before, Remaining) ->
    case beam_re_test:compile_stats() of
        {Entered, Active} when Entered > Before, Active > 0 -> ok;
        Actual when Remaining =:= 0 ->
            ?BEAM_VM:error({compile_did_not_enter_native_guard, Before, Actual});
        _ ->
            receive
                {'DOWN', Ref, process, Pid, Reason} ->
                    ?BEAM_VM:error({compile_ended_before_kill, Reason})
            after 1 ->
                await_compile_entry(Pid, Ref, Before, Remaining - 1)
            end
    end.

await_compile_done(Remaining) ->
    case beam_re_test:compile_stats() of
        {_, 0} -> ok;
        Actual when Remaining =:= 0 ->
            ?BEAM_VM:error({native_compile_not_reclaimed, Actual});
        _ ->
            receive after 5 -> ok end,
            await_compile_done(Remaining - 1)
    end.

compiled_kill() ->
    {ok, Pattern} = re:compile("\\w+\\d"),
    kill_matching(Pattern).

kill_matching(Pattern) ->
    Previous = beam_re_test:set_limit(1),
    try
        Subject = beam_bytes:repeat(<<"hejsan">>, 100000),
        Go = make_ref(),
        {Pid, Ref} = erlang:spawn_monitor(fun() ->
            receive Go -> ok end,
            re:run(Subject, Pattern)
        end),
        1 = ?BEAM_VM:trace(Pid, true, [running]),
        Pid ! Go,
        receive
            {trace, Pid, out, {re, re_match_continue, 2}} -> ok;
            {'DOWN', Ref, process, Pid, Reason} ->
                ?BEAM_VM:error({match_ended_before_kill, Reason})
        after 5000 -> ?BEAM_VM:error(match_did_not_yield)
        end,
        exit(Pid, kill),
        receive {'DOWN', Ref, process, Pid, killed} -> ok
        after 1000 -> ?BEAM_VM:error(killed_match_timeout)
        end
    after
        beam_re_test:set_limit(Previous)
    end.

await_stats(Expected, Remaining) ->
    case beam_re_test:stats() of
        Expected -> ok;
        Actual when Remaining =:= 0 ->
            ?BEAM_VM:error({native_resources_not_reclaimed, Expected, Actual});
        _ ->
            erlang:garbage_collect(),
            receive after 5 -> ok end,
            await_stats(Expected, Remaining - 1)
    end.

retain() ->
    release(),
    Subject = beam_bytes:repeat(<<"hejsan">>, 100000),
    {ok, Pattern} = re:compile("\\w+\\d"),
    {ok, CapturePattern} = re:compile("^(.+)$"),
    {match, [Capture]} = re:run(Subject, CapturePattern, [{capture, [1], binary}]),
    Go = make_ref(),
    {Pid, Ref} = erlang:spawn_monitor(fun() ->
        receive Go -> ok end,
        retained_worker(Subject, Pattern)
    end),
    1 = ?BEAM_VM:trace(Pid, true, [running]),
    Pid ! Go,
    receive {trace, Pid, out, {re, re_match_continue, 2}} -> ok
    after 5000 -> ?BEAM_VM:error(retained_match_did_not_yield)
    end,
    1 = ?BEAM_VM:trace(Pid, false, [running]),
    ?BEAM_VM:demonitor(Ref, [flush]),
    ?RETENTION:put({?MODULE, retained},
                        {Pattern, CapturePattern, Subject, Capture, Pid}),
    ok.

retained_worker(Subject, Pattern) ->
    re:run(Subject, Pattern),
    retained_worker(Subject, Pattern).

release() ->
    case ?RETENTION:get({?MODULE, retained}, undefined) of
        undefined -> ok;
        {_, _, _, _, Pid} ->
            Ref = ?BEAM_VM:monitor(process, Pid),
            exit(Pid, kill),
            receive {'DOWN', Ref, process, Pid, _} -> ok
            after 1000 -> ?BEAM_VM:error(retained_worker_stop_timeout)
            end,
            ?RETENTION:erase({?MODULE, retained}),
            ok
    end.
