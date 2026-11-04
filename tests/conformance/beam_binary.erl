%% SPDX-License-Identifier: Apache-2.0
%% Test-only bridge for canonical conformance fixture dependencies.
-module(beam_binary).
-export([copy/2,referenced_byte_size/1]).
copy(Bytes,Count) -> binary:copy(Bytes,Count).
referenced_byte_size(Bytes) -> binary:referenced_byte_size(Bytes).
