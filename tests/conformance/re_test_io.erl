%% SPDX-License-Identifier: Apache-2.0
-module(re_test_io).
-export([format/1, format/2, format/3]).

format(Format) -> format(Format, []).
format(Format, Arguments) ->
    Text = io_lib:format(Format, Arguments),
    beam_re_test:write(unicode:characters_to_binary(Text)).

format(standard_io, Format, Arguments) -> format(Format, Arguments);
format(standard_error, Format, Arguments) -> format(Format, Arguments);
format(Device, _, _) -> erlang:error({unsupported_test_output, Device}).
