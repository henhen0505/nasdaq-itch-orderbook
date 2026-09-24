#pragma once

#include "parser/itch_messages.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>

namespace mde {

// Invoked once per retained message, in file order. A message is retained
// if it is a Stock Directory ('R') message, or if its stock_locate has been
// resolved (via a prior 'R' message) to one of target_tickers().
using ItchMessageCallback = std::function<void(const ItchMessage&)>;

// The 5 target tickers this engine tracks. Symbol
// resolution happens via Stock Directory ('R') messages, which map
// stock_locate -> ticker.
const std::unordered_set<std::string>& target_tickers();

// Parses a decompressed ITCH 5.0 binary buffer already resident in memory.
//
// Frames are read via their 2-byte big-endian length prefix. The 9 required
// message types are dispatched to typed structs and passed to `callback`;
// every other type is skipped by frame length without attempting to parse
// its fields. 'R' messages are always parsed and emitted (they are what
// resolves stock_locate -> ticker in the first place); every other message
// type is additionally filtered by stock_locate membership in the set
// resolved so far.
//
// Throws BufferUnderrunError if a frame claims more bytes than the buffer
// actually contains, or if a frame's declared length is too small to hold
// the fields its message type requires.
void parse_buffer(const uint8_t* data, size_t size, const ItchMessageCallback& callback);

// Convenience overload: reads the full contents of `path` (a decompressed
// ITCH 5.0 binary file) into memory, then parses it via parse_buffer().
void parse_file(const std::string& path, const ItchMessageCallback& callback);

} // namespace mde
