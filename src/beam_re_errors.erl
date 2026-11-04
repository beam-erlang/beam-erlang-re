%%
%% %CopyrightBegin%
%%
%% SPDX-License-Identifier: Apache-2.0
%%
%% Copyright Ericsson AB 2020-2026. All Rights Reserved.
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

-module(beam_re_errors).
-moduledoc false.
-export([format_error/2]).

%% Regex diagnostics extracted from the pinned OTP formatter. Keeping only the
%% owned operations avoids acquiring a general STDLIB formatting dependency.
format_error(_Reason, [{re, Function, Args, Info} | _]) ->
    Metadata = property(error_info, Info, #{}),
    Cause = case Metadata of
                #{cause := Value} -> Value;
                _ -> none
            end,
    format_error_map(format_re_error(Function, Args, Cause), 1, #{});
format_error(_, _) ->
    #{}.

format_re_error(compile, [_], _) ->
    [not_iodata];
format_re_error(compile, [Re, _Options], Cause) ->
    ReError = try re:compile(Re) of
                  {ok, _} -> [];
                  {error, Reason} ->
                      {bad_regexp, Reason}
              catch
                  _:_ -> not_iodata
              end,
    case Cause of
        badopt ->
            [ReError, bad_options];
        _ ->
            [ReError]
    end;
format_re_error(inspect, [CompiledRE, Item], _) ->
    ReError = try re:inspect(CompiledRE, namelist) of
                  _ -> []
              catch
                  error:_ -> not_compiled_regexp
              end,
    if
        ReError =:= []; not is_atom(Item) ->
            [ReError, <<"not a valid item">>];
        true ->
            [ReError]
    end;
format_re_error(import, [_], _) ->
    [~"not an exported regular expression"];
format_re_error(replace, [Subject, RE, Replacement], _) ->
    [must_be_iodata(Subject),
     must_be_regexp(RE),
     must_be_re_replacement(Replacement)];
format_re_error(replace, [Subject, RE, Replacement, _Options], Cause) ->
    Errors = [must_be_iodata(Subject),
              must_be_regexp(RE),
              must_be_re_replacement(Replacement)],
    case Cause of
        badopt ->
            Errors ++ [bad_options];
        _ ->
            Errors
    end;
format_re_error(run, [Subject, RE], _) ->
    [must_be_iodata(Subject), must_be_regexp(RE)];
format_re_error(run, [Subject, RE, _Options], Cause) ->
    Errors = [must_be_iodata(Subject), must_be_regexp(RE)],
    case Cause of
        badopt ->
            Errors ++ [bad_options];
        _ ->
            Errors
    end;
format_re_error(split, [Subject, RE], _) ->
    [must_be_iodata(Subject), must_be_regexp(RE)];
format_re_error(split, [Subject, RE, _Options], Cause) ->
    Errors = [must_be_iodata(Subject), must_be_regexp(RE)],
    case Cause of
        badopt ->
            Errors ++ [bad_options];
        _ ->
            Errors
    end;
format_re_error(_, _, _) ->
    [].

must_be_regexp(Term) ->
    %% First check if we can compile the regexp as this
    %% returns better error messages
    try re:compile(Term) of
        {ok, _} ->
            [];
        {error, Reason} ->
            {bad_regexp, Reason}
    catch error:_ ->
            %% Then check if we can run it as this also allows
            %% compiled reg exps
            try re:run("", Term) of
                _ -> []
            catch
                error:_ -> not_regexp
            end
    end.

must_be_re_replacement(R) when is_function(R, 1) -> [];
must_be_re_replacement(R) ->
    case is_iodata(R) of
        true -> [];
        false -> bad_replacement
    end.


property(Key, [{Key, Value} | _], _) -> Value;
property(Key, [Key | _], _) -> true;
property(Key, [_ | Rest], Default) -> property(Key, Rest, Default);
property(_, [], Default) -> Default.

is_iodata(Binary) when is_binary(Binary) -> true;
is_iodata(Term) when is_list(Term) ->
    try iolist_size(Term) of
        _ -> true
    catch
        error:_ -> false
    end;
is_iodata(_) -> false.

must_be_iodata(Term) ->
    case is_iodata(Term) of
        true -> [];
        false -> not_iodata
    end.

format_error_map([[] | Rest], Position, Map) ->
    format_error_map(Rest, Position + 1, Map);
format_error_map([{general, Error} | Rest], Position, Map) ->
    format_error_map(Rest, Position, Map#{general => expand_error(Error)});
format_error_map([Error | Rest], Position, Map) ->
    format_error_map(Rest, Position + 1, Map#{Position => expand_error(Error)});
format_error_map([], _, Map) ->
    Map.

expand_error(bad_options) -> <<"invalid options">>;
expand_error(bad_replacement) -> <<"not a valid replacement">>;
expand_error(not_compiled_regexp) -> <<"not a compiled regular expression">>;
expand_error(not_iodata) -> <<"not an iodata term">>;
expand_error(not_regexp) ->
    <<"neither an iodata term nor a compiled regular expression">>;
expand_error({bad_regexp, {Reason, Column}}) ->
    beam_unicode:characters_to_binary(
      ["could not parse regular expression\n", Reason,
       " on character ", integer_to_binary(Column)], unicode);
expand_error(Text) when is_binary(Text) -> Text.
