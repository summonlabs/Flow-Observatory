// Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Binary codec for one observation. Shared by the journal (observation log) and
// by the loopback transport.

#ifndef FLOWOBS_SRC_OBS_CODEC_HPP
#define FLOWOBS_SRC_OBS_CODEC_HPP

#include "binary.hpp"
#include "flowobs/limits.hpp"
#include "flowobs/observation.hpp"

namespace flowobs {
namespace detail {

void encode_observation(Writer& writer, const Observation& observation);
bool decode_observation(Reader& reader, const Limits& limits, Observation& out);

}  // namespace detail
}  // namespace flowobs

#endif  // FLOWOBS_SRC_OBS_CODEC_HPP
