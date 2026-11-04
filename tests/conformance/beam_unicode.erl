%% SPDX-License-Identifier: Apache-2.0
%% Test-owned bridge: canonical conformance runs production RE unchanged.
-module(beam_unicode).
-export([characters_to_binary/2,characters_to_list/2]).
characters_to_binary(Data,Encoding) -> unicode:characters_to_binary(Data,Encoding).
characters_to_list(Data,Encoding) -> unicode:characters_to_list(Data,Encoding).
